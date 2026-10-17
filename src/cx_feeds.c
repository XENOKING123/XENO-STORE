/*
 * cx_feeds.c — see cx_feeds.h.
 */
#include "cx_feeds.h"
#include "config.h"
#include "cx_common.h"
#include "http_client.h"
#include "log.h"
#include "settings.h"
#include "util.h"
#include "xthread.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FEED_MAX_AGE_S (6 * 3600)
#define FEED_MAX_BYTES (2u << 20)
#define FEED_FIRST_DELAY_S 20
#define FEED_RETRY_S 600       /* copies of another build still to fetch: the next automatic try */

static pthread_mutex_t feeds_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fetch_mu = PTHREAD_MUTEX_INITIALIZER;   /* one fetch + cache write per feed at a time */
static struct {
  const char *name;
  char *text;
  size_t len;
  time_t fetchedAt;
  int otherBuild;          /* cached by another build: due whatever its age */
} feeds[CX_FEED_COUNT] = { {"featured", NULL, 0, 0, 0}, {"messages", NULL, 0, 0, 0}, {"companions", NULL, 0, 0, 0}, {"saves-backend", NULL, 0, 0, 0} };
static time_t boot_at, last_auto;
static volatile int refreshing;
static volatile int ensuring[CX_FEED_COUNT];   /* a cx_feed_ensure_wait thread runs for the feed */
static int build_marked;   /* <data>/feeds/.build names this build (guarded by fetch_mu) */

static const char *const FEED_HOSTS[] = {"raw.githubusercontent.com", "github.com", "githubusercontent.com", "api.github.com"};

int cx_feed_host_allowed(const char *url) {
  char host[256];
  util_url_host(url, host, sizeof host);
  return cx_host_in(host, FEED_HOSTS, XS_ARRAY_LEN(FEED_HOSTS)) || cx_host_is_loopback(host);
}

static void cache_path(const char *name, char *out, size_t cap) { xsnprintf(out, cap, "%s/feeds/%s.json", cx_data_dir(), name); }
static void build_path(char *out, size_t cap) { xsnprintf(out, cap, "%s/feeds/.build", cx_data_dir()); }

char *cx_cached_fetch(const char *name, const char *url, int max_age_s, int force, size_t max_bytes,
                      size_t *len, int *from_cache, time_t *fetched_at) {
  char path[700];
  cache_path(name, path, sizeof path);
  time_t mt = util_file_mtime(path);
  time_t now = util_now();
  if (from_cache) *from_cache = 0;
  if (fetched_at) *fetched_at = mt;
  if (!force && mt > 0 && now - mt < max_age_s) {
    char *txt = util_read_file(path, len, max_bytes);
    if (txt) { if (from_cache) *from_cache = 1; return txt; }
  }
  char *fresh = NULL;
  if (url && *url && xhttp_available()) {
    if (!cx_feed_host_allowed(url)) LOGW("feeds: %s host not allowed (%s)", name, url);
    else {
      int status = 0;
      size_t l = 0;
      cx_http o = {0};
      o.max_bytes = max_bytes;
      o.headers = "Cache-Control: no-cache\r\n";
      fresh = cx_fetch(url, &o, &status, &l, NULL);
      if (fresh && status == 200 && l > 0) {
        jdoc *d = json_parse(fresh, l, 200000, 32);
        if (d) {
          json_free(d);
          char dir[700];
          xsnprintf(dir, sizeof dir, "%s/feeds", cx_data_dir());
          util_mkdir_p(dir);
          if (util_write_file_atomic(path, fresh, l) != 0) LOGW("feeds: cannot cache %s", name);
          if (len) *len = l;
          if (fetched_at) *fetched_at = now;
          return fresh;
        }
        LOGW("feeds: %s is not valid JSON", name);
      } else if (fresh) LOGW("feeds: %s returned HTTP %d", name, status);
      else LOGW("feeds: %s fetch failed: %s", name, xhttp_last_error());
      free(fresh);
      fresh = NULL;
    }
  }
  /* fall back to whatever is cached, however old */
  char *txt = util_read_file(path, len, max_bytes);
  if (txt && from_cache) *from_cache = 1;
  return txt;
}

static const char *feed_url(const settings_t *s, int feed) {
  switch (feed) {
    case CX_FEED_FEATURED: return s->featuredUrl;
    case CX_FEED_MESSAGES: return s->messagesUrl;
    case CX_FEED_COMPANIONS: return s->companionsUrl;
    default: return s->savesBackendUrl;
  }
}

static void load_one(int feed, int force) {
  settings_t s;
  settings_get(&s);
  size_t len = 0;
  int from_cache = 0;
  time_t at = 0;
  pthread_mutex_lock(&fetch_mu);
  char *txt = cx_cached_fetch(feeds[feed].name, feed_url(&s, feed), FEED_MAX_AGE_S, force, FEED_MAX_BYTES, &len, &from_cache, &at);
  pthread_mutex_lock(&feeds_mu);
  free(feeds[feed].text);
  feeds[feed].text = txt;
  feeds[feed].len = txt ? len : 0;
  feeds[feed].fetchedAt = txt ? at : 0;
  if (txt && !from_cache) feeds[feed].otherBuild = 0;
  int left = 0;
  for (int i = 0; i < CX_FEED_COUNT; i++) left += feeds[i].otherBuild;
  pthread_mutex_unlock(&feeds_mu);
  /* no copy of another build is left: from now on they are this build's */
  if (txt && !from_cache && !left && !build_marked) {
    char p[700];
    build_path(p, sizeof p);
    if (util_write_file_atomic(p, XENO_VERSION "\n", strlen(XENO_VERSION "\n")) == 0) build_marked = 1;
    else LOGW("feeds: cannot write %s", p);
  }
  pthread_mutex_unlock(&fetch_mu);
}

static int other_build(int feed) {
  pthread_mutex_lock(&feeds_mu);
  int o = feeds[feed].otherBuild;
  pthread_mutex_unlock(&feeds_mu);
  return o;
}

static int other_build_left(void) {
  int n = 0;
  for (int i = 0; i < CX_FEED_COUNT; i++) n += other_build(i);
  return n > 0;
}

/* Missing, older than max_age_s or cached by another build (due whatever its age). */
static int due(int feed, int max_age_s, int *other) {
  pthread_mutex_lock(&feeds_mu);
  time_t at = feeds[feed].text ? feeds[feed].fetchedAt : 0;
  *other = feeds[feed].otherBuild;
  pthread_mutex_unlock(&feeds_mu);
  return *other || at <= 0 || util_now() - at >= max_age_s;
}

void cx_feed_ensure(int feed, int max_age_s) {
  int other;
  if (feed < 0 || feed >= CX_FEED_COUNT || !due(feed, max_age_s, &other)) return;
  /* a shorter age than the feed's own 6 h means "fetch now" (user refresh) */
  load_one(feed, other || max_age_s < FEED_MAX_AGE_S);
}

static void *ensure_thread(void *arg) {
  int feed = (int)(intptr_t)arg;
  cx_feed_ensure(feed, FEED_MAX_AGE_S);
  __atomic_store_n(&ensuring[feed], 0, __ATOMIC_SEQ_CST);
  return NULL;
}

void cx_feed_ensure_wait(int feed, int wait_ms) {
  int other;
  if (feed < 0 || feed >= CX_FEED_COUNT || !due(feed, FEED_MAX_AGE_S, &other)) return;
  if (!__atomic_exchange_n(&ensuring[feed], 1, __ATOMIC_SEQ_CST) &&
      xthread_start(NULL, "feed-ensure", ensure_thread, (void *)(intptr_t)feed, 0, 1) != 0) {
    __atomic_store_n(&ensuring[feed], 0, __ATOMIC_SEQ_CST);
    LOGW("feeds: cannot start the %s fetch", feeds[feed].name);
    return;
  }
  int64_t until = util_now_ms() + wait_ms;
  while (__atomic_load_n(&ensuring[feed], __ATOMIC_SEQ_CST) && util_now_ms() < until) util_sleep_ms(50);
}

void cx_feeds_init(void) {
  boot_at = util_now();
  /* Copies cached by another build are fetched again on the first refresh,
   * whatever their age: an update from 1.2.1 kept its saves-backend.json
   * (GitHub only) and placeholder companion hashes for up to 6 h. The
   * marker names the build the copies belong to (none before 1.3.1). */
  char mark[700];
  build_path(mark, sizeof mark);
  char *bv = util_read_file(mark, NULL, 64);
  if (bv) util_trim(bv);
  build_marked = bv && strcmp(bv, XENO_VERSION) == 0;
  int other = 0;
  /* Offline start: load the cached copies without touching the network. */
  for (int i = 0; i < CX_FEED_COUNT; i++) {
    char path[700];
    cache_path(feeds[i].name, path, sizeof path);
    size_t len = 0;
    char *txt = util_read_file(path, &len, FEED_MAX_BYTES);
    pthread_mutex_lock(&feeds_mu);
    feeds[i].text = txt;
    feeds[i].len = txt ? len : 0;
    feeds[i].fetchedAt = txt ? util_file_mtime(path) : 0;
    feeds[i].otherBuild = txt && !build_marked;
    other += feeds[i].otherBuild;
    pthread_mutex_unlock(&feeds_mu);
  }
  if (other) LOGI("feeds: %d cached feed(s) from %s, due whatever their age (this is %s)", other, bv && *bv ? bv : "an earlier build", XENO_VERSION);
  free(bv);
}

void cx_feeds_refresh(int force) {
  if (refreshing) return;
  refreshing = 1;
  settings_t s;
  settings_get(&s);
  /* a copy of another build is due at once while the catalog refreshes on
   * its own; with auto-refresh off it keeps the 6 h rule here and is
   * fetched where it is needed (cx_feed_ensure: the Save Store's
   * saves-backend.json; companions.json for the payload list, a payload
   * install and the Garlic start) */
  for (int i = 0; i < CX_FEED_COUNT; i++) load_one(i, force || (s.catalogAutoRefresh && other_build(i)));
  refreshing = 0;
}

void catalog_on_refresh(int updated) {
  (void)updated;
  cx_feeds_refresh(0);
}

static void *refresh_thread(void *arg) {
  (void)arg;
  cx_feeds_refresh(0);
  return NULL;
}

void cx_feeds_tick(void) {
  time_t now = util_now();
  if (!xhttp_available() || g_cfg.safe_mode || now - boot_at < FEED_FIRST_DELAY_S) return;
  /* copies of another build left (offline): tried again soon, not in 6 h */
  if (last_auto && now - last_auto < (other_build_left() ? FEED_RETRY_S : FEED_MAX_AGE_S)) return;
  settings_t s;
  settings_get(&s);
  if (!s.catalogAutoRefresh) { last_auto = now; return; }
  last_auto = now;
  if (xthread_start(NULL, "feeds", refresh_thread, NULL, 0, 1) != 0) LOGW("feeds: cannot start refresh thread");
}

void cx_feed_json(int feed, jw *w) {
  jdoc *d = cx_feed_doc(feed);
  if (!d) { jw_null(w); return; }
  jw_value(w, json_root(d));
  json_free(d);
}

jdoc *cx_feed_doc(int feed) {
  if (feed < 0 || feed >= CX_FEED_COUNT) return NULL;
  pthread_mutex_lock(&feeds_mu);
  jdoc *d = feeds[feed].text ? json_parse(feeds[feed].text, feeds[feed].len, 200000, 32) : NULL;
  pthread_mutex_unlock(&feeds_mu);
  return d;
}

time_t cx_feed_fetched_at(int feed) {
  if (feed < 0 || feed >= CX_FEED_COUNT) return 0;
  pthread_mutex_lock(&feeds_mu);
  time_t t = feeds[feed].fetchedAt;
  pthread_mutex_unlock(&feeds_mu);
  return t;
}
