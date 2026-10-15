/*
 * update.c — see update.h.
 *
 * Feed format (Payload Manager repository, SPEC §12):
 *   {"payloads":[{"name","version","filename","url","sha256","minFw","notes"}], ...}
 * The entry whose filename is xeno-store.elf (or the first entry) wins.
 * Fallback: GitHub Releases "latest" — tag v<ver> (xeno-store-v<ver> still accepted), asset
 * xeno-store.elf, and SHA256SUMS when present for the hash. GitHub is also
 * asked when the feed names a release older than this build; the higher wins.
 *
 * The TV app's pinned install and the ELF64 test follow the reference
 * backend's update worker. Ported from Orbit Store backend/updates.c
 * (GPL-3.0-or-later).
 */
#include <errno.h>
#include "update.h"
#include "config.h"
#include "http_client.h"
#include "log.h"
#include "settings.h"
#include "sha256.h"
#include "sys.h"
#include "cx_common.h"
#include "util.h"
#include "xthread.h"

#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GITHUB_LATEST "https://api.github.com/repos/XENOKING123/XENO-STORE/releases/latest"
#define GITHUB_LATEST_PATH "/repos/XENOKING123/XENO-STORE/releases/latest"
#define HANDOFF_WINDOW_S 90   /* a handed-off copy reaches server_start well within this */
#define MAX_ELF (64LL << 20)  /* the TV app's starter sends at most 64 MiB */
#define THROTTLE_MIN_S 60     /* after a 403/429/503 from the release host */

/* update_mu guards U and is never held over I/O; check_mu serialises checks
 * (held over the network); apply_mu lets one apply run at a time (trylock).
 * Order when nested: check_mu or apply_mu, then update_mu. */
static pthread_mutex_t update_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t check_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t apply_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int checking, applying;
static int last_check_rc = -1;           /* the last check's result (under check_mu) */
static char last_check_err[256];
static volatile time_t throttled_until;  /* the release host asked us to back off */
static char data_dir[512];
static int handoff_settling;   /* another copy of this build just handed off: its marker is not ours to clear */

/* One release as a feed or GitHub describes it. Parsed into a local copy and
 * only then published in U, so a failed or partial parse never shows. */
typedef struct rel_info {
  char latest[32], url[1024], sha256[65], notes[512];
} rel_info;

static struct {
  rel_info r;
  time_t checkedAt;
  int available;
} U;

void update_init(const char *dir) {
  xstrlcpy(data_dir, dir, sizeof data_dir);
  pthread_mutex_lock(&update_mu);
  if (!U.checkedAt) xstrlcpy(U.r.latest, XENO_VERSION, sizeof U.r.latest);
  pthread_mutex_unlock(&update_mu);
}

#ifdef XENO_HOST
static char api_url[600];
static pthread_once_t api_url_once = PTHREAD_ONCE_INIT;
static void api_url_init(void) {
  const char *base = cx_base_override("GITHUB_API");
  if (base[0]) xsnprintf(api_url, sizeof api_url, "%s%s", base, GITHUB_LATEST_PATH);
}
#endif

const char *update_release_api_url(void) {
#ifdef XENO_HOST
  pthread_once(&api_url_once, api_url_init);
  if (api_url[0]) return api_url;
#endif
  return GITHUB_LATEST;
}

static void note_throttle(const xhttp_resp *resp) {
  if (resp->status != 403 && resp->status != 429 && resp->status != 503) return;
  int wait = resp->retry_after > THROTTLE_MIN_S ? resp->retry_after : THROTTLE_MIN_S;
  if (wait > 3600) wait = 3600;
  throttled_until = util_now() + wait;
}

static char *fetch(const char *url, int timeout_ms, size_t max, size_t *len, xhttp_resp *resp) {
  xhttp_req r;
  memset(&r, 0, sizeof r);
  r.url = url;
  r.range_start = r.range_end = -1;
  r.follow_redirects = 5;
  r.connect_timeout_ms = timeout_ms;
  r.recv_timeout_ms = timeout_ms;
  r.extra_headers = "Accept: application/json\r\nCache-Control: no-cache\r\n";
  char *body = xhttp_fetch(&r, resp, max, len);
  if (body) note_throttle(resp);
  return body;
}

/* "v1.2.3" and "xeno-store-v1.2.3" name the release 1.2.3. */
static const char *strip_v(const char *v) {
  if (util_starts_with(v, "xeno-store-v")) return v + 12;
  return (v[0] == 'v' || v[0] == 'V') ? v + 1 : v;
}

/* Parses the Payload Manager feed into *out. */
static int parse_feed(const char *txt, size_t len, rel_info *out) {
  memset(out, 0, sizeof *out);
  jdoc *d = json_parse(txt, len, 5000, 16);
  if (!d) return -1;
  jv best = { NULL, 0xFFFFFFFFu };
  JV_FOREACH(p, jv_get(json_root(d), "payloads")) {
    const char *fn = jv_get_str(p, "filename", "");
    if (!jv_ok(best) || strcmp(fn, "xeno-store.elf") == 0) best = p;
    if (strcmp(fn, "xeno-store.elf") == 0) break;
  }
  if (!jv_ok(best)) { json_free(d); return -1; }
  xstrlcpy(out->latest, strip_v(jv_get_str(best, "version", XENO_VERSION)), sizeof out->latest);
  xstrlcpy(out->url, jv_get_str(best, "url", ""), sizeof out->url);
  xstrlcpy(out->sha256, jv_get_str(best, "sha256", ""), sizeof out->sha256);
  xstrlcpy(out->notes, jv_get_str(best, "notes", ""), sizeof out->notes);
  util_lower(out->sha256);
  json_free(d);
  return out->latest[0] ? 0 : -1;
}

int update_sums_lookup(const char *sums, const char *name, char out[65]) {
  for (const char *line = sums; line && *line;) {
    const char *nl = strchr(line, '\n');
    size_t n = 0;
    while (n < 64 && isxdigit((unsigned char)line[n])) n++;
    if (n == 64 && (line[64] == ' ' || line[64] == '\t')) {
      const char *nm = line + 64;
      while (*nm == ' ' || *nm == '\t' || *nm == '*') nm++;
      size_t nl_len = nl ? (size_t)(nl - nm) : strlen(nm);
      while (nl_len > 0 && (nm[nl_len - 1] == '\r' || nm[nl_len - 1] == ' ')) nl_len--;
      if (nl_len == strlen(name) && strncmp(nm, name, nl_len) == 0) {
        memcpy(out, line, 64);
        out[64] = 0;
        util_lower(out);
        return 0;
      }
    }
    line = nl ? nl + 1 : NULL;
  }
  return -1;
}

/* Parses GitHub's releases/latest JSON into *out (fetches SHA256SUMS too). */
static int parse_github(const char *txt, size_t len, rel_info *out) {
  memset(out, 0, sizeof *out);
  jdoc *d = json_parse(txt, len, 50000, 16);
  if (!d) return -1;
  const char *v = strip_v(jv_get_str(json_root(d), "tag_name", ""));
  if (!*v) { json_free(d); return -1; }
  xstrlcpy(out->latest, v, sizeof out->latest);
  xstrlcpy(out->notes, jv_get_str(json_root(d), "body", ""), sizeof out->notes);
  char sums_url[1024] = "";
  JV_FOREACH(a, jv_get(json_root(d), "assets")) {
    const char *name = jv_get_str(a, "name", "");
    if (strcmp(name, "xeno-store.elf") == 0) xstrlcpy(out->url, jv_get_str(a, "browser_download_url", ""), sizeof out->url);
    if (strcmp(name, "SHA256SUMS") == 0) xstrlcpy(sums_url, jv_get_str(a, "browser_download_url", ""), sizeof sums_url);
  }
  json_free(d);
  if (!out->url[0]) return -1;   /* a release without the payload asset is not an update */
  if (sums_url[0]) {
    xhttp_resp resp;
    size_t sl;
    char *sums = fetch(sums_url, 15000, 64 << 10, &sl, &resp);
    if (sums && resp.status == 200 && update_sums_lookup(sums, "xeno-store.elf", out->sha256) != 0) out->sha256[0] = 0;
    free(sums);
  }
  return 0;
}

/* One feed or GitHub round trip into *out: 0, or -1 with err. */
static int fetch_feed(const char *url, rel_info *out, char *err, size_t errcap) {
  xhttp_resp resp;
  size_t len = 0;
  char *txt = fetch(url, 20000, 1 << 20, &len, &resp);
  int rc = -1;
  if (!txt) { LOGW("update: feed fetch failed: %s", xhttp_last_error()); xsnprintf(err, errcap, "Update check failed: %s", xhttp_last_error()); }
  else if (resp.status != 200) { LOGW("update: feed returned HTTP %d", resp.status); xsnprintf(err, errcap, "The update feed returned HTTP %d", resp.status); }
  else if ((rc = parse_feed(txt, len, out)) != 0) xsnprintf(err, errcap, "Could not read the update feed");
  free(txt);
  return rc;
}

static int fetch_github(rel_info *out, char *err, size_t errcap) {
  xhttp_resp resp;
  size_t len = 0;
  char *txt = fetch(update_release_api_url(), 20000, 4 << 20, &len, &resp);
  int rc = -1;
  if (!txt) xsnprintf(err, errcap, "Update check failed: %s", xhttp_last_error());
  else if (resp.status != 200) xsnprintf(err, errcap, "GitHub returned HTTP %d", resp.status);
  else if ((rc = parse_github(txt, len, out)) != 0) xsnprintf(err, errcap, "The latest GitHub release has no xeno-store.elf");
  free(txt);
  return rc;
}

/* The network part of a check: never holds update_mu (E5), so status reads
 * answer from memory meanwhile. Caller holds check_mu. */
static int check_locked(char *err, size_t errcap) {
  char feed_url[512] = "";
  settings_t *s = malloc(sizeof *s);
  if (!s) { xsnprintf(err, errcap, "out of memory"); return -1; }
  settings_get(s);
  xstrlcpy(feed_url, s->updatesFeed, sizeof feed_url);
  free(s);
  static const rel_info none;
  rel_info feed = none, gh = none;
  int feed_rc = -1, gh_rc = -1;
  char feed_err[256] = "", gh_err[256] = "";
  if (feed_url[0]) feed_rc = fetch_feed(feed_url, &feed, feed_err, sizeof feed_err);
  /* A feed that lags behind this build (a release before its feed patch, a
   * fork's feed) does not hide a newer GitHub release: ask GitHub too. */
  if (feed_rc != 0 || util_semver_cmp(feed.latest, XENO_VERSION) < 0) {
    gh_rc = fetch_github(&gh, gh_err, sizeof gh_err);
    if (gh_rc != 0 && feed_rc == 0) LOGI("update: the feed names %s (older than %s); GitHub: %s", feed.latest, XENO_VERSION, gh_err);
  }
  const rel_info *pick = NULL;
  if (feed_rc == 0 && gh_rc == 0) pick = util_semver_cmp(gh.latest, feed.latest) > 0 ? &gh : &feed;
  else if (feed_rc == 0) pick = &feed;
  else if (gh_rc == 0) pick = &gh;
  if (!pick) {
    xstrlcpy(err, gh_err[0] ? gh_err : feed_err[0] ? feed_err : "Could not read the update feed", errcap);
    return -1;
  }
  time_t now = util_now();
  pthread_mutex_lock(&update_mu);
  U.r = *pick;
  U.checkedAt = now;
  U.available = util_semver_cmp(U.r.latest, XENO_VERSION) > 0;
  int avail = U.available;
  pthread_mutex_unlock(&update_mu);
  LOGI("update: latest %s from %s (current %s) %s", pick->latest, pick == &gh ? "GitHub" : "the feed", XENO_VERSION,
       avail ? "AVAILABLE" : "up to date");
  err[0] = 0;
  return 0;
}

int update_check(char *err, size_t errcap) {
  err[0] = 0;
  if (!xhttp_available()) { xsnprintf(err, errcap, "network transport unavailable"); return -1; }
  if (pthread_mutex_trylock(&check_mu) != 0) {
    /* A check is on the network already (web UI, housekeeping or the TV app):
     * wait for it and share its result instead of asking the hosts twice. */
    pthread_mutex_lock(&check_mu);
    int rc = last_check_rc;
    xstrlcpy(err, last_check_err, errcap);
    pthread_mutex_unlock(&check_mu);
    return rc;
  }
  checking = 1;
  char e[256] = "";
  int rc = check_locked(e, sizeof e);
  last_check_rc = rc;
  xstrlcpy(last_check_err, e, sizeof last_check_err);
  checking = 0;
  pthread_mutex_unlock(&check_mu);
  xstrlcpy(err, e, errcap);
  settings_touch_updates_check();
  return rc;
}

void update_status_json(jw *w) {
  pthread_mutex_lock(&update_mu);
  jw_kv_bool(w, "available", U.available);
  jw_kv_str(w, "latest", U.r.latest[0] ? U.r.latest : XENO_VERSION);
  jw_kv_str(w, "current", XENO_VERSION);
  jw_kv_str(w, "url", U.r.url);
  jw_kv_str(w, "sha256", U.r.sha256);
  jw_kv_str(w, "notes", U.r.notes);
  jw_kv_i64(w, "checkedAt", (int64_t)U.checkedAt);
  pthread_mutex_unlock(&update_mu);
  char staged[600];
  jw_kv_bool(w, "staged", update_staged(staged, sizeof staged));
}

void update_snapshot(update_info *out) {
  memset(out, 0, sizeof *out);
  pthread_mutex_lock(&update_mu);
  xstrlcpy(out->latest, U.r.latest, sizeof out->latest);
  xstrlcpy(out->url, U.r.url, sizeof out->url);
  xstrlcpy(out->sha256, U.r.sha256, sizeof out->sha256);
  out->checked_at = U.checkedAt;
  out->available = U.available;
  pthread_mutex_unlock(&update_mu);
  out->checking = checking;
  out->applying = applying;
  time_t t = throttled_until;
  out->throttled_until = t > util_now() ? t : 0;
}

/* Streams url to dst while hashing; returns 0 and the hex digest. */
static int download_to_file(const char *url, const char *dst, char hex[65], char *err, size_t errcap) {
  xhttp_req req;
  memset(&req, 0, sizeof req);
  req.url = url;
  req.range_start = req.range_end = -1;
  req.follow_redirects = 8;
  req.connect_timeout_ms = 20000;
  req.recv_timeout_ms = 60000;
  xhttp_conn *c = NULL;
  xhttp_resp resp;
  if (xhttp_open(&req, &c, &resp) != 0) { xsnprintf(err, errcap, "Download failed: %s", xhttp_last_error()); return -1; }
  if (resp.status != 200) { xsnprintf(err, errcap, "Download returned HTTP %d", resp.status); xhttp_close(c); return -1; }
  int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
  if (fd < 0) { xsnprintf(err, errcap, "Cannot write %s", dst); xhttp_close(c); return -1; }
  char *buf = malloc(256u << 10);
  sha256_ctx h;
  sha256_init(&h);
  int64_t total = 0;
  int rc = 0;
  for (;;) {
    int n = buf ? xhttp_read(c, buf, 256u << 10) : -1;
    if (n < 0) { xsnprintf(err, errcap, "Download interrupted: %s", xhttp_last_error()); rc = -1; break; }
    if (n == 0) break;
    sha256_update(&h, buf, (size_t)n);
    ssize_t off = 0;
    while (off < n) {
      ssize_t w = write(fd, buf + off, (size_t)(n - off));
      if (w <= 0) { xsnprintf(err, errcap, "Write failed"); rc = -1; break; }
      off += w;
    }
    if (rc) break;
    total += n;
    if (total > MAX_ELF) { xsnprintf(err, errcap, "ELF larger than 64 MiB — refusing"); rc = -1; break; }
  }
  free(buf);
  fsync(fd);
  close(fd);
  xhttp_close(c);
  if (rc == 0 && total < 4096) { xsnprintf(err, errcap, "Downloaded file is too small to be an ELF"); rc = -1; }
  if (rc == 0) {
    uint8_t dgst[32];
    sha256_final(&h, dgst);
    sha256_digest_hex(dgst, hex);
  }
  return rc;
}

/* 1 when the file is a regular file of 64 bytes to 64 MiB that starts like
 * our payload: ELF64 (7F 45 4C 46 02), little-endian (01), x86-64 (e_machine
 * 3E 00 at bytes 18-19) — exactly the test the TV app's starter applies
 * before it sends a saved copy, so a file it would skip is never reported or
 * installed as the saved copy. Ported from Orbit Store app/src/orbit/starter.cpp
 * readable_elf (GPL-3.0-or-later). */
static int file_is_elf(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  struct stat st;
  unsigned char h[20];
  ssize_t r = -1;
  int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 64 && st.st_size <= MAX_ELF;
  if (ok) {
    do { r = read(fd, h, sizeof h); } while (r < 0 && errno == EINTR);
    ok = r == (ssize_t)sizeof h && memcmp(h, "\177ELF\2\1", 6) == 0 && h[18] == 0x3E && h[19] == 0x00;
  }
  close(fd);
  return ok;
}

/* The ELF magic alone (4 bytes, any size). The start-up hand-off and the web
 * UI's "staged" flag keep this test: the ELF loader is the final judge there,
 * and a file the strict test refuses is never written by an apply. */
static int file_has_elf_magic(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  unsigned char m[4];
  ssize_t r;
  do { r = read(fd, m, sizeof m); } while (r < 0 && errno == EINTR);
  close(fd);
  return r == 4 && m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F';
}

/* Moves a verified download over <data>/xeno-store.elf and records its
 * version (no newline). The old .version goes first, so a failure between the
 * two steps leaves an unversioned copy, which the next start sets aside,
 * never a new ELF under an old version. */
static int install_staged(const char *staging, const char *final, const char *version, char *err, size_t errcap) {
  char vpath[640];
  xsnprintf(vpath, sizeof vpath, "%s.version", final);
  util_unlink_quiet(vpath);
  if (rename(staging, final) != 0) { xsnprintf(err, errcap, "Cannot rename staged ELF"); util_unlink_quiet(staging); return -1; }
  util_fsync_dir(final);
  /* The version next to it lets an older copy (the Games-row app's built-in ELF) hand over at start. */
  if (util_write_file_atomic(vpath, version, strlen(version)) != 0) {
    LOGW("update: cannot record the staged version in %s", vpath);
    xsnprintf(err, errcap, "Cannot record the version of the saved copy");
    return -1;
  }
  return 0;
}

static int apply_locked(char *out_path, size_t cap, char *err, size_t errcap) {
  char url[1024], expect[65], ver[32];
  pthread_mutex_lock(&update_mu);
  xstrlcpy(url, U.r.url, sizeof url);
  pthread_mutex_unlock(&update_mu);
  if (!url[0] && update_check(err, errcap) != 0) return -1;
  pthread_mutex_lock(&update_mu);
  xstrlcpy(url, U.r.url, sizeof url);
  xstrlcpy(expect, U.r.sha256, sizeof expect);
  xstrlcpy(ver, U.r.latest, sizeof ver);
  pthread_mutex_unlock(&update_mu);
  if (!url[0]) { xsnprintf(err, errcap, "No download URL in the update feed"); return -1; }
  char staging[600], final[600];
  xsnprintf(staging, sizeof staging, "%s/xeno-store.elf.new", data_dir);
  xsnprintf(final, sizeof final, "%s/xeno-store.elf", data_dir);
  char hex[65];
  if (download_to_file(url, staging, hex, err, errcap) != 0) { util_unlink_quiet(staging); return -1; }
  if (expect[0] && strcmp(expect, hex) != 0) {
    xsnprintf(err, errcap, "SHA-256 mismatch (expected %.12s…, got %.12s…)", expect, hex);
    util_unlink_quiet(staging);
    return -1;
  }
  if (!file_is_elf(staging)) { xsnprintf(err, errcap, "Downloaded file is not an ELF64 x86-64 payload"); util_unlink_quiet(staging); return -1; }
  if (install_staged(staging, final, ver, err, errcap) != 0) return -1;
  xstrlcpy(out_path, final, cap);
  LOGI("update: staged %s %s (sha256 %s)", final, ver, hex);
  return 0;
}

int update_apply(char *out_path, size_t cap, char *err, size_t errcap) {
  err[0] = 0;
  if (pthread_mutex_trylock(&apply_mu) != 0) { xsnprintf(err, errcap, "Another update is being installed. Wait for it to finish."); return UPDATE_BUSY; }
  applying = 1;
  int rc = apply_locked(out_path, cap, err, errcap);
  applying = 0;
  pthread_mutex_unlock(&apply_mu);
  return rc;
}

/* ---- the TV app's pinned install ------------------------------------------ */

typedef struct pin_progress {
  update_progress_fn fn;
  void *user;
  volatile int *cancel;
} pin_progress;

static int pin_progress_cb(void *user, int64_t done, int64_t total) {
  pin_progress *p = user;
  if (p->cancel && *p->cancel) return 1;
  return p->fn ? p->fn(p->user, done, total > 0 ? total : -1) : 0;
}

static int is_hex64(const char *s) {
  if (strlen(s) != 64) return 0;
  for (int i = 0; i < 64; i++) if (!isxdigit((unsigned char)s[i])) return 0;
  return 1;
}

/* https:// only (host build: also a loopback test server). The SHA-256 from
 * the same check pins the bytes, so the host is not restricted further (a
 * custom feed may publish elsewhere). */
static int pinned_url_ok(const char *url) {
  xurl u;
  if (util_url_parse(url, &u) != 0 || u.has_userinfo) return 0;
  if (strcmp(u.scheme, "https") == 0) return 1;
  return cx_host_is_loopback(u.host);
}

static int apply_pinned_locked(const char *version, const char *sha256, update_progress_fn progress, void *user,
                               volatile int *cancel, char *err, size_t errcap) {
  char want[65], url[1024];
  xstrlcpy(want, sha256 ? sha256 : "", sizeof want);
  util_lower(want);
  if (!version || !version[0] || !is_hex64(want)) { xsnprintf(err, errcap, "the release has no version or SHA-256 to pin"); return -1; }
  pthread_mutex_lock(&update_mu);
  int match = U.checkedAt > 0 && strcmp(U.r.latest, version) == 0 && strcmp(U.r.sha256, want) == 0 && U.r.url[0];
  xstrlcpy(url, U.r.url, sizeof url);
  pthread_mutex_unlock(&update_mu);
  if (!match) { xsnprintf(err, errcap, "the checked release changed; check again"); return -1; }
  if (!pinned_url_ok(url)) { xsnprintf(err, errcap, "the release URL is not https"); return -1; }
  char staging[600], final[600];
  xsnprintf(staging, sizeof staging, "%s/xeno-store.elf.new", data_dir);
  xsnprintf(final, sizeof final, "%s/xeno-store.elf", data_dir);
  util_unlink_quiet(staging);
  pin_progress pp = { progress, user, cancel };
  cx_http o = {0};
  o.timeout_ms = 30000;
  o.follow = 5;
  xthread_note("update: download %s", version);
  if (cx_download_file(url, &o, staging, want, MAX_ELF, pin_progress_cb, &pp, cancel, err, errcap) != 0) {
    xthread_net(NULL, NULL);
    util_unlink_quiet(staging);
    return -1;
  }
  xthread_net(NULL, NULL);
  if (cancel && *cancel) { util_unlink_quiet(staging); xsnprintf(err, errcap, "cancelled"); return -1; }
  if (!file_is_elf(staging)) {
    util_unlink_quiet(staging);
    xsnprintf(err, errcap, "the download is not an ELF64 x86-64 payload");
    return -1;
  }
  if (progress) progress(user, -1, -1);   /* "saving" */
  if (install_staged(staging, final, version, err, errcap) != 0) return -1;
  LOGI("update: saved %s as %s (sha256 %s)", version, final, want);
  return 0;
}

int update_apply_pinned(const char *version, const char *sha256, update_progress_fn progress, void *user,
                        volatile int *cancel, char *err, size_t errcap) {
  err[0] = 0;
  if (pthread_mutex_trylock(&apply_mu) != 0) { xsnprintf(err, errcap, "another update is being installed"); return UPDATE_BUSY; }
  applying = 1;
  int rc = apply_pinned_locked(version, sha256, progress, user, cancel, err, errcap);
  applying = 0;
  pthread_mutex_unlock(&apply_mu);
  return rc;
}

/* The version recorded next to the staged ELF ("" when missing or unreadable). */
static void staged_version(const char *elf, char *ver, size_t cap) {
  char vpath[640];
  xsnprintf(vpath, sizeof vpath, "%s.version", elf);
  ver[0] = 0;
  size_t len = 0;
  char *v = util_read_file(vpath, &len, 64);
  if (!v) return;
  xstrlcpy(ver, v, cap);
  free(v);
  util_trim(ver);
}

int update_saved_version(char *ver, size_t cap) {
  char path[600];
  xsnprintf(path, sizeof path, "%s/xeno-store.elf", data_dir);
  if (cap) ver[0] = 0;
  if (!file_is_elf(path)) return 0;
  staged_version(path, ver, cap);
  return ver[0] != 0;
}

int update_staged(char *path, size_t cap) {
  xsnprintf(path, cap, "%s/xeno-store.elf", data_dir);
  if (!file_has_elf_magic(path)) return 0;
  /* Only a download newer than this build counts: a leftover from 1.1.x/1.2.0
   * has no .version and restarting into it would downgrade. */
  char ver[32];
  staged_version(path, ver, sizeof ver);
  return ver[0] && util_semver_cmp(ver, XENO_VERSION) > 0;
}

static void marker_path(char *out, size_t cap) { xsnprintf(out, cap, "%s/handoff.pending", data_dir); }

/* Renames the staged ELF and its .version to <name><suffix> so no later start picks them up. */
static void staged_set_aside(const char *elf, const char *suffix) {
  char from[640], to[660];
  xsnprintf(to, sizeof to, "%s%s", elf, suffix);
  rename(elf, to);
  xsnprintf(from, sizeof from, "%s.version", elf);
  xsnprintf(to, sizeof to, "%s.version%s", elf, suffix);
  rename(from, to);
}

/* The staged update cannot be trusted: set it aside, drop the marker, keep this copy. */
static int handoff_failed(const char *elf, const char *ver, const char *why) {
  char mpath[600];
  staged_set_aside(elf, ".failed");
  marker_path(mpath, sizeof mpath);
  util_unlink_quiet(mpath);
  LOGW("update: the downloaded %s %s — set aside as %s.failed, running the built-in %s", ver, why, elf, XENO_VERSION);
  sys_toast("XENO STORE %s did not start · running %s", ver, XENO_VERSION);
  return 0;
}

int update_handoff_newer(int loader_port) {
  char path[600], ver[32];
  xsnprintf(path, sizeof path, "%s/xeno-store.elf", data_dir);
  if (!file_has_elf_magic(path)) return 0;
  staged_version(path, ver, sizeof ver);
  if (!ver[0] || util_semver_cmp(ver, XENO_VERSION) < 0) {
    LOGI("update: setting aside the stale %s (version %s) as .old", path, ver[0] ? ver : "unknown");
    staged_set_aside(path, ".old");
    return 0;
  }
  if (util_semver_cmp(ver, XENO_VERSION) == 0) return 0;   /* this copy is that update */
  /* Crash guard: the staged version stopped soon after its last start. */
  long up = -1;
  int fresh = 0;
  const char *rep = crash_previous(&up, &fresh);
  char tag[64];
  xsnprintf(tag, sizeof tag, "XENO STORE %s stopped", ver);
  if (fresh && up >= 0 && up < CRASH_EARLY_S && util_starts_with(rep, tag)) return handoff_failed(path, ver, "stopped right after its last start");
  /* Hand-off marker ("<time> <version that handed off>"), cleared by a copy
   * once its server is up. A young one from another build means this copy was
   * just handed the ELF (or the .version overstates it): do not hand over again.
   * A young one this very build wrote means two copies started at once: run this
   * one and leave the marker to the handed-off copy. An old one means the copy it
   * started never came up. */
  char mpath[600];
  marker_path(mpath, sizeof mpath);
  size_t len = 0;
  char *m = util_read_file(mpath, &len, 128);
  if (m) {
    char *end = m;
    long long ts = strtoll(m, &end, 10);
    char from[32] = "";
    if (end == m) ts = (long long)util_file_mtime(mpath);
    else xstrlcpy(from, end, sizeof from);
    free(m);
    util_trim(from);
    long long age = (long long)util_now() - ts;
    if (age < HANDOFF_WINDOW_S && strcmp(from, XENO_VERSION) != 0) {
      LOGI("update: started by a hand-off %lld s ago — not handing over to %s again", age, ver);
      return 0;
    }
    if (age >= 0 && age < HANDOFF_WINDOW_S) {
      handoff_settling = 1;
      LOGI("update: this build handed over to %s %lld s ago and that copy may still be starting — not handing over again", ver, age);
      return 0;
    }
    return handoff_failed(path, ver, "never came up after the last hand-off");
  }
  char err[160];
  if (update_relaunch(loader_port, err, sizeof err) != 0) {
    LOGW("update: could not start the downloaded %s (%s) — running %s", ver, err, XENO_VERSION);
    return 0;
  }
  LOGI("update: handed over to the downloaded %s (this copy is %s)", ver, XENO_VERSION);
  return 1;
}

void update_handoff_done(void) {
  if (handoff_settling) return;
  char mpath[600];
  marker_path(mpath, sizeof mpath);
  util_unlink_quiet(mpath);
}

int update_relaunch(int loader_port, char *err, size_t errcap) {
  char path[600];
  if (!update_staged(path, sizeof path)) { xsnprintf(err, errcap, "No downloaded update is waiting"); return -1; }
  size_t len = 0;
  char *elf = util_read_file(path, &len, 96u << 20);
  if (!elf || len < 64) { free(elf); xsnprintf(err, errcap, "Cannot read %s", path); return -1; }
  /* Written first: the started copy may come up before this one returns. */
  char mpath[600], mark[64];
  marker_path(mpath, sizeof mpath);
  xsnprintf(mark, sizeof mark, "%lld %s\n", (long long)util_now(), XENO_VERSION);
  if (util_write_file_atomic(mpath, mark, strlen(mark)) != 0) LOGW("update: cannot write %s", mpath);
  int rc = cx_tcp_send_all(loader_port, elf, len, 15000, err, errcap);
  free(elf);
  if (rc == 0) LOGI("update: handed %s (%zu bytes) to the ELF loader on :%d", path, len, loader_port);
  else { util_unlink_quiet(mpath); LOGE("update: relaunch failed: %s", err); }
  return rc;
}
