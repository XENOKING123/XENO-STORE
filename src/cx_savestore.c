/*
 * cx_savestore.c — see cx_savestore.h.
 *
 * The merged index is rebuilt at most once an hour (or on refresh=1) from
 * every configured source and kept as JSON text; requests parse it, filter
 * and attach the compatibility fields computed against this console. The
 * rebuild runs on its own thread (xthread_start): a request only waits for it
 * when there is no copy yet or the user asked for a refresh, and never longer
 * than a few seconds.
 */
#include "cx_savestore.h"
#include "api.h"
#include "catalog.h"
#include "cx_accounts.h"
#include "cx_common.h"
#include "cx_feeds.h"
#include "cx_garlic.h"
#include "cx_jobs.h"
#include "cx_savemount.h"
#include "cx_saves.h"
#include "http_client.h"
#include "log.h"
#include "settings.h"
#include "storage.h"
#include "util.h"
#include "xthread.h"
#include "zip.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define INDEX_MAX_AGE_S 3600
#define INDEX_MAX_ROWS 2000
#define ROW_KEY_LEN 72
#define DEFAULT_SAVES_INDEX "https://raw.githubusercontent.com/XENOKING123/XENO-STORE/main/updates/saves.json"
#define SAVE_ZIP_MAX (2LL << 30)
#define UPLOAD_MAX (512LL << 20)
#define CATBOX_MAX (200LL << 20)
#define SHARE_MAX_ENTRIES 4096
#define LIST_WAIT_MS 15000      /* the UI gives GET /api/saves/store 20 s */
#define ROW_WAIT_MS 30000

static pthread_mutex_t store_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t store_cv = PTHREAD_COND_INITIALIZER;
static char *merged_text;
static size_t merged_len;
static time_t merged_at;
static char merged_sig[300];          /* backend_sig() of the copy */
static int building, pending_force;   /* guarded by store_mu */

static const char *const DOWNLOAD_HOSTS[] = {"pixeldrain.com", "catbox.moe", "github.com", "githubusercontent.com", "archive.org",
                                             "supabase.co", "garlicsaves.com", "gofile.io", "buzzheavier.com", "mediafire.com"};

/* ---- backend config ----------------------------------------------------- */

/* The database backend (SPEC §17.22.8). The live default is the owner's
 * XenoKing site, published in updates/saves-backend.json; Settings → Saves →
 * Backend overrides it. The anon key is the project's public key (row-level
 * security decides what it may do) and is only ever sent to b->url's own
 * origin (backend_host_ok), never across a redirect (follow = -1). */
typedef struct backend {
  char kind[12], url[256], anonKey[SET_ANONKEY_LEN], bucket[64], fn[64];
  char name[48];          /* display label ("XenoKing"); the host for a settings backend */
  char counter[64];       /* download-counter RPC, only when the published file names one */
  int garlicList;         /* also list rows through the function ({"action":"list"}): published file opt-in */
  int moderated;          /* inserts carry status "pending" + source "xeno" (template backend) */
  int fromSettings;       /* typed into Settings → Saves → Backend (backend_traits probes it) */
  char error[160];        /* why the Settings override is not used (GET /api/saves/store: backendError) */
} backend;

/* bucket / function / RPC names: [A-Za-z0-9_.-]{1,63} */
static int ident_ok(const char *s) {
  size_t n = strlen(s);
  if (n < 1 || n > 63) return 0;
  for (const char *p = s; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) return 0;
  return 1;
}

/* https://host[:port] (no credentials); plain http only for a loopback mock on the host build. */
static int backend_url_ok(const char *url) {
  xurl u;
  if (util_url_parse(url, &u) != 0 || u.has_userinfo || !u.host[0]) return 0;
  if (strcmp(u.scheme, "https") == 0) return 1;
  return strcmp(u.scheme, "http") == 0 && cx_host_is_loopback(u.host);
}

static void backend_fill(backend *b, const char *url, const char *key, const char *bucket, const char *fn) {
  xstrlcpy(b->kind, "supabase", sizeof b->kind);
  xstrlcpy(b->url, url, sizeof b->url);
  size_t ul = strlen(b->url);
  while (ul > 0 && b->url[ul - 1] == '/') b->url[--ul] = 0;
  xstrlcpy(b->anonKey, key, sizeof b->anonKey);
  xstrlcpy(b->bucket, ident_ok(bucket) ? bucket : "ps5-saves", sizeof b->bucket);
  xstrlcpy(b->fn, ident_ok(fn) ? fn : "", sizeof b->fn);
}

/* An anon key travels in a header line: printable ASCII only. */
static int anon_key_ok(const char *k) {
  if (!k[0] || strlen(k) >= SET_ANONKEY_LEN) return 0;
  for (const unsigned char *p = (const unsigned char *)k; *p; p++) if (*p <= 0x20 || *p >= 0x7f) return 0;
  return 1;
}

static void backend_get(backend *b) {
  memset(b, 0, sizeof *b);
  settings_t s;
  settings_get(&s);
  /* Settings override the published saves-backend.json. An override that
   * cannot be used (no URL or key, not https, a key with spaces) never falls
   * back to the published site: a share meant for the user's own server must
   * not land in the owner's public collection. The console keeps the GitHub
   * index only and says why (backendError in GET /api/saves/store). */
  if (strcmp(s.saves.backend.kind, "supabase") == 0) {
    if (backend_url_ok(s.saves.backend.url) && anon_key_ok(s.saves.backend.anonKey)) {
      backend_fill(b, s.saves.backend.url, s.saves.backend.anonKey, s.saves.backend.bucket, s.saves.backend.garlicFunction);
      util_url_host(b->url, b->name, sizeof b->name);
      b->fromSettings = 1;
      return;
    }
    xstrlcpy(b->kind, "github", sizeof b->kind);
    if (!s.saves.backend.url[0] || !s.saves.backend.anonKey[0]) xstrlcpy(b->error, "Settings → Saves → Backend: enter the project URL and its anon key", sizeof b->error);
    else if (!backend_url_ok(s.saves.backend.url)) xstrlcpy(b->error, "Settings → Saves → Backend: the URL must be https://<project>.supabase.co", sizeof b->error);
    else xstrlcpy(b->error, "Settings → Saves → Backend: the anon key is not valid", sizeof b->error);
    return;
  }
  if (strcmp(s.saves.backend.kind, "none") == 0) { xstrlcpy(b->kind, "none", sizeof b->kind); return; }
  xstrlcpy(b->kind, "github", sizeof b->kind);
  jdoc *d = cx_feed_doc(CX_FEED_SAVES_BACKEND);
  if (!d) return;
  jv r = json_root(d);
  const char *url = jv_get_str(r, "url", ""), *key = jv_get_str(r, "anonKey", "");
  if (strcmp(jv_get_str(r, "kind", ""), "supabase") == 0 && backend_url_ok(url) && anon_key_ok(key)) {
    backend_fill(b, url, key, jv_get_str(r, "bucket", "ps5-saves"), jv_get_str(r, "garlicFunction", "garlic-savedb"));
    /* display label: printable, short */
    const char *nm = jv_get_str(r, "name", "");
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)nm; *p && j + 1 < sizeof b->name; p++) if (*p >= 0x20 && *p != 0x7f) b->name[j++] = (char)*p;
    b->name[j] = 0;
    util_trim(b->name);
    if (!b->name[0]) util_url_host(b->url, b->name, sizeof b->name);
    const char *ctr = jv_get_str(r, "downloadCounter", "");
    if (ident_ok(ctr)) xstrlcpy(b->counter, ctr, sizeof b->counter);
    b->garlicList = jv_get_bool(r, "garlicList", 0);
    b->moderated = jv_get_bool(r, "moderated", 0);
  }
  json_free(d);
}

static int site_on(const backend *b) { return strcmp(b->kind, "supabase") == 0; }

/* Same scheme, host and port as the configured backend, below its URL. */
static int backend_host_ok(const backend *b, const char *url) {
  size_t bl = strlen(b->url);
  if (!bl || strncmp(url, b->url, bl) != 0 || url[bl] != '/') return 0;
  xurl a, c;
  if (util_url_parse(url, &a) != 0 || util_url_parse(b->url, &c) != 0 || a.has_userinfo || c.has_userinfo) return 0;
  return strcmp(a.scheme, c.scheme) == 0 && util_strieq(a.host, c.host) && a.port == c.port;
}

/* apikey + Authorization headers (plus extra lines); -1 when they do not fit. */
static int supabase_headers(const backend *b, char *out, size_t cap, const char *extra) {
  int n = snprintf(out, cap, "apikey: %s\r\nAuthorization: Bearer %s\r\n%s", b->anonKey, b->anonKey, extra ? extra : "");
  if (n < 0 || (size_t)n >= cap) { if (cap) out[0] = 0; return -1; }
  return 0;
}

/* Request options for the backend: no redirects, so the key never leaves its origin. */
static void sb_opt(cx_http *o, const char *method, const char *headers, const char *body, size_t body_len, int timeout_ms, size_t max_bytes) {
  memset(o, 0, sizeof *o);
  o->method = method;
  o->headers = headers;
  o->body = body;
  o->body_len = body_len;
  o->timeout_ms = timeout_ms;
  o->max_bytes = max_bytes;
  o->follow = -1;
}

/* A path inside the bucket as the backend stores it: relative, no "..", no
 * query/fragment or control characters ("garlic:<id>" is not one). */
static int bucket_path_ok(const char *p) {
  size_t n = strlen(p);
  if (n < 1 || n > 240 || p[0] == '/' || util_istarts_with(p, "garlic:") || util_istarts_with(p, "http")) return 0;
  if (strstr(p, "..") || strstr(p, "//")) return 0;
  for (const unsigned char *c = (const unsigned char *)p; *c; c++) if (*c < 0x20 || *c == 0x7f || *c == '?' || *c == '#' || *c == '\\') return 0;
  return 1;
}

/* A backend typed into Settings (the published file says what it is itself):
 * the XenoKing site's table has the base columns only, while a deployment of
 * backend/supabase (migration 0004) adds status / source and the
 * increment_downloads RPC, and its insert policy only takes pending rows from
 * consoles. One read tells them apart — GET …?select=status&limit=1 answers
 * 200 there and 400 / 42703 (no such column) on the site. The answer is kept
 * per URL; a probe that got neither is tried again next time. */
static pthread_mutex_t traits_mu = PTHREAD_MUTEX_INITIALIZER;
static char traits_url[256];
static int traits_template = -1;      /* -1 unknown, 0 base columns, 1 template */

static void backend_traits(backend *b) {
  if (!site_on(b) || !b->fromSettings) return;
  pthread_mutex_lock(&traits_mu);
  int t = strcmp(traits_url, b->url) == 0 ? traits_template : -1;
  pthread_mutex_unlock(&traits_mu);
  if (t < 0 && xhttp_available()) {
    char url[600], hdr[1400];
    xsnprintf(url, sizeof url, "%s/rest/v1/ps5_save_collection?select=status&limit=1", b->url);
    if (backend_host_ok(b, url) && supabase_headers(b, hdr, sizeof hdr, "Accept: application/json\r\n") == 0) {
      cx_http o;
      sb_opt(&o, NULL, hdr, NULL, 0, 15000, 64 << 10);
      int st = 0;
      size_t len = 0;
      char *resp = cx_fetch(url, &o, &st, &len, NULL);
      if (resp && st >= 200 && st < 300) t = 1;
      else if (resp && st == 400) {
        jdoc *d = json_parse(resp, len, 1000, 8);
        if (d && strcmp(jv_get_str(json_root(d), "code", ""), "42703") == 0) t = 0;
        if (d) json_free(d);
      }
      free(resp);
      if (t >= 0) {
        pthread_mutex_lock(&traits_mu);
        xstrlcpy(traits_url, b->url, sizeof traits_url);
        traits_template = t;
        pthread_mutex_unlock(&traits_mu);
        LOGI("savestore: %s is %s", b->name, t ? "a backend/supabase deployment (moderated inserts, download counter)" : "a plain save table (base columns)");
      } else LOGW("savestore: could not tell what kind of table %s has (HTTP %d)", b->name, st);
    }
  }
  if (t == 1) {
    b->moderated = 1;
    if (!b->counter[0]) xstrlcpy(b->counter, "increment_downloads", sizeof b->counter);
  }
}

/* Cached request (any method/headers) under <data>/feeds/<name>.json. */
static char *cached_request(const char *name, const char *url, const cx_http *opt, int max_age, int force, size_t *len, int *from_cache) {
  char path[700];
  xsnprintf(path, sizeof path, "%s/feeds/%s.json", cx_data_dir(), name);
  time_t mt = util_file_mtime(path);
  if (from_cache) *from_cache = 0;
  if (!force && mt > 0 && util_now() - mt < max_age) {
    char *t = util_read_file(path, len, 8u << 20);
    if (t) { if (from_cache) *from_cache = 1; return t; }
  }
  if (xhttp_available()) {
    int status = 0;
    size_t l = 0;
    char *t = cx_fetch(url, opt, &status, &l, NULL);
    if (t && status >= 200 && status < 300) {
      jdoc *d = json_parse(t, l, 400000, 32);
      if (d) {
        json_free(d);
        char dir[700];
        xsnprintf(dir, sizeof dir, "%s/feeds", cx_data_dir());
        util_mkdir_p(dir);
        util_write_file_atomic(path, t, l);
        if (len) *len = l;
        return t;
      }
    }
    if (t) LOGW("savestore: %s returned HTTP %d", name, status); else LOGW("savestore: %s failed: %s", name, xhttp_last_error());
    free(t);
  }
  char *t = util_read_file(path, len, 8u << 20);
  if (t && from_cache) *from_cache = 1;
  return t;
}

/* ---- row normalization -------------------------------------------------- */

static const char *infer_category(const char *cat, const char *text) {
  static const char *const cats[] = {"starter", "platinum", "checkpoint", "modded", "chapter", "other"};
  if (cat && *cat) for (size_t i = 0; i < XS_ARRAY_LEN(cats); i++) if (util_strieq(cat, cats[i])) return cats[i];
  if (!text) return "other";
  if (util_strcasestr(text, "100%") || util_strcasestr(text, "platinum")) return "platinum";
  if (util_strcasestr(text, "start") || util_strcasestr(text, "ng+") || util_strcasestr(text, "max")) return "starter";
  if (util_strcasestr(text, "trophy") || util_strcasestr(text, "boss")) return "checkpoint";
  if (util_strcasestr(text, "mod") || util_strcasestr(text, "glitch")) return "modded";
  if (util_strcasestr(text, "chapter")) return "chapter";
  return "other";
}

typedef struct dedupe {
  char (*keys)[ROW_KEY_LEN];
  int n;
} dedupe;

static int seen_key(dedupe *d, const char *key) {
  if (!key || !*key) return 0;
  for (int i = 0; i < d->n; i++) if (strcmp(d->keys[i], key) == 0) return 1;
  if (d->n < INDEX_MAX_ROWS) xstrlcpy(d->keys[d->n++], key, ROW_KEY_LEN);
  return 0;
}

static void write_row(jw *w, const char *id, const char *titleId, const char *gameId, const char *title, const char *category,
                      const char *name, const char *description, const char *author, const char *gameVersion, const char *region,
                      const char *dirName, int64_t size, const char *sha256, const char *url, const char *mirror, const char *format,
                      const char *createdAt, int downloads, const char *status, jv tags, const char *screenshot, const char *source,
                      const char *storagePath, const char *garlicId, const char *cover, int needsResign,
                      const char *coverPath, const char *sourceName) {
  jw_obj_begin(w);
  jw_kv_str(w, "id", id);
  jw_kv_str(w, "titleId", titleId);
  jw_kv_str(w, "gameId", gameId);
  jw_kv_str(w, "title", title);
  jw_kv_str(w, "category", category);
  jw_kv_str(w, "name", name);
  jw_kv_str(w, "description", description);
  jw_kv_str(w, "author", author);
  jw_kv_str(w, "gameVersion", gameVersion);
  jw_kv_str(w, "region", region);
  jw_kv_str(w, "dirName", dirName);
  jw_kv_i64(w, "size", size);
  jw_kv_str(w, "sha256", sha256);
  jw_kv_str(w, "url", url);
  jw_kv_str(w, "mirror", mirror);
  jw_kv_str(w, "format", format);
  jw_kv_str(w, "createdAt", createdAt);
  jw_kv_int(w, "downloads", downloads);
  jw_kv_str(w, "status", status);
  jw_key(w, "tags");
  if (jv_ok(tags) && jv_type(tags) == JV_ARR) jw_value(w, tags); else { jw_arr_begin(w); jw_arr_end(w); }
  jw_kv_str(w, "screenshot", screenshot);
  jw_kv_str(w, "source", source);
  jw_kv_str(w, "storagePath", storagePath);
  jw_kv_str(w, "garlicId", garlicId);
  jw_kv_str(w, "cover", cover);
  jw_kv_bool(w, "needsResign", needsResign);
  cx_jw_kv_opt(w, "coverPath", coverPath);
  cx_jw_kv_opt(w, "sourceName", sourceName);
  jw_obj_end(w);
}

static int add_index_rows(jw *w, jv root, const char *source, dedupe *dd, int *count) {
  jv arr = jv_get(root, "saves");
  if (jv_type(arr) != JV_ARR) arr = root;
  if (jv_type(arr) != JV_ARR) return 0;
  int n = 0;
  JV_FOREACH(it, arr) {
    if (*count >= INDEX_MAX_ROWS) break;
    const char *tid = jv_get_str(it, "titleId", "");
    const char *url = jv_get_str(it, "url", "");
    const char *status = jv_get_str(it, "status", "published");
    if (!cx_titleid_valid(tid) || tid[0] != 'P' || !util_istarts_with(url, "http")) continue;
    if (strcmp(status, "published") != 0) continue;
    const char *sha = jv_get_str(it, "sha256", "");
    const char *id = jv_get_str(it, "id", "");
    char key[ROW_KEY_LEN];
    if (sha[0]) xsnprintf(key, sizeof key, "sha:%s", sha); else xsnprintf(key, sizeof key, "id:%s", id);
    if (!id[0] || seen_key(dd, key)) continue;
    char gid[128] = "";
    xstrlcpy(gid, jv_get_str(it, "gameId", ""), sizeof gid);
    cat_game g;
    if (!gid[0] && catalog_match_titleid(tid, &g) == 0) xstrlcpy(gid, g.id, sizeof gid);
    write_row(w, id, tid, gid, jv_get_str(it, "title", ""), infer_category(jv_get_str(it, "category", ""), jv_get_str(it, "description", "")),
              jv_get_str(it, "name", ""), jv_get_str(it, "description", ""), jv_get_str(it, "author", ""), jv_get_str(it, "gameVersion", ""),
              jv_get_str(it, "region", ""), jv_get_str(it, "dirName", ""), jv_get_i64(it, "size", 0), sha, url, jv_get_str(it, "mirror", ""),
              jv_get_str(it, "format", "xeno-save-zip"), jv_get_str(it, "createdAt", ""), (int)jv_get_i64(it, "downloads", 0), status,
              jv_get(it, "tags"), jv_get_str(it, "screenshot", ""), source, "", "", jv_get_str(it, "cover", ""), jv_get_bool(it, "needsResign", 1), "", "");
    (*count)++;
    n++;
  }
  return n;
}

/* UTF-8 helpers for the display name: the byte offset after max_cp code
 * points (at most 4 bytes each, even in malformed text, so callers can size
 * their buffers as 4 * max_cp). */
static size_t utf8_cut(const char *t, size_t max_cp) {
  size_t i = 0, cp = 0;
  while (t[i] && cp < max_cp) {
    i++;
    for (int k = 0; k < 3 && (t[i] & 0xC0) == 0x80; k++) i++;
    cp++;
  }
  return i;
}

/* Display name of a site row: the first sentence or line of the notes, cut to
 * about 80 characters on a word boundary; "<game> save" without notes. (The
 * site's file_name is a download name — "PPSA02785_A_Plague_Tale:_Requiem.zip",
 * sometimes mojibake — and never shown.) */
static void notes_name(const char *notes, const char *game, char *out, size_t cap) {
  while (*notes == ' ' || *notes == '\t' || *notes == '\r' || *notes == '\n') notes++;
  if (!*notes) { xsnprintf(out, cap, "%s save", game[0] ? game : "Community"); return; }
  size_t end = 0;
  for (size_t i = 0; notes[i]; i++) {
    char ch = notes[i], nx = notes[i + 1];
    if (ch == '\n' || ch == '\r') break;
    if ((ch == '.' || ch == '!' || ch == '?') && (nx == 0 || nx == ' ' || nx == '\n' || nx == '\r' || nx == '\t')) {
      end = ch == '.' ? i : i + 1;      /* keep "!" / "?", drop the full stop */
      break;
    }
    end = i + 1;
  }
  char line[640];                       /* 150 code points of at most 4 bytes, plus "…" */
  size_t lim = utf8_cut(notes, 150);   /* only the start of a very long sentence matters */
  if (end > lim) end = lim;
  memcpy(line, notes, end);
  line[end] = 0;
  util_trim(line);
  size_t cut = utf8_cut(line, 80);
  if (line[cut]) {
    /* longer than 80 characters: back to the last space past the first 40 */
    size_t min = utf8_cut(line, 40), sp = cut;
    while (sp > min && line[sp] != ' ') sp--;
    if (sp > min) cut = sp;
    line[cut] = 0;
    /* no dangling separator (",;:-|" or a " — " left by a share's "<name> — <description>") */
    for (int again = 1; again;) {
      util_trim(line);
      size_t l = strlen(line);
      again = 0;
      if (l > 0 && strchr(",;:-|", line[l - 1])) { line[l - 1] = 0; again = 1; }
      else if (l >= 3 && memcmp(line + l - 3, "\xE2\x80\x94", 3) == 0) { line[l - 3] = 0; again = 1; }   /* — */
    }
    size_t l = strlen(line);
    /* a share's name already cut with "…" keeps that one */
    if (l && !(l >= 3 && memcmp(line + l - 3, "\xE2\x80\xA6", 3) == 0)) xstrlcat(line, "\xE2\x80\xA6", sizeof line);   /* … */
  }
  if (!line[0]) { xsnprintf(out, cap, "%s save", game[0] ? game : "Community"); return; }
  if (strlen(line) >= cap) line[utf8_cut(line, (cap - 1) / 4)] = 0;   /* never split a code point */
  xstrlcpy(out, line, cap);
}

/* Row ids become "sb:<uuid>" (route parameter, file name): [A-Za-z0-9-]{1,64} only. */
static int row_uuid_ok(const char *u) {
  size_t n = strlen(u);
  if (n < 1 || n > 64) return 0;
  for (const char *p = u; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-')) return 0;
  return 1;
}

/* Rows of public.ps5_save_collection (the XenoKing site, or a template
 * deployment). The site's columns: id, title_id, game, shared_by, notes,
 * file_path, file_name, file_size, created_at, cover_path, garlic_id; the
 * template adds category / status / tags / downloads … which are read when
 * present. Garlic rows (file_path "garlic:<id>", garlic_id) download through
 * the garlic function, the others from the bucket through a signed URL. */
static int add_supabase_rows(jw *w, jv arr, const backend *b, dedupe *dd, int *count) {
  if (jv_type(arr) != JV_ARR) return 0;
  int n = 0;
  JV_FOREACH(it, arr) {
    if (*count >= INDEX_MAX_ROWS) break;
    if (jv_type(it) != JV_OBJ) continue;
    const char *tid = jv_get_str(it, "title_id", "");
    const char *status = jv_get_str(it, "status", "published");
    if (!cx_titleid_valid(tid) || tid[0] != 'P' || strcmp(status, "published") != 0) continue;
    const char *uuid = jv_get_str(it, "id", "");
    if (!row_uuid_ok(uuid)) continue;
    const char *path = jv_get_str(it, "file_path", "");
    int64_t gnum = jv_get_i64(it, "garlic_id", 0);   /* an integer on the site, a string elsewhere */
    if (gnum <= 0 && util_istarts_with(path, "garlic:")) gnum = strtoll(path + 7, NULL, 10);
    char garlic[24] = "";
    if (gnum > 0 && gnum < 1000000000LL) xsnprintf(garlic, sizeof garlic, "%lld", (long long)gnum);
    const char *storage = bucket_path_ok(path) ? path : "";
    if (!garlic[0] && !storage[0]) {
      const char *fname = jv_get_str(it, "file_name", "");
      if (!util_istarts_with(fname, "http")) continue;   /* nothing to download */
    }
    char key[ROW_KEY_LEN];
    if (garlic[0]) xsnprintf(key, sizeof key, "garlic:%s", garlic); else xsnprintf(key, sizeof key, "sb:%s", uuid);
    if (seen_key(dd, key)) continue;
    char id[80];
    xsnprintf(id, sizeof id, "sb:%s", uuid);
    char gid[128] = "";
    cat_game g;
    if (catalog_match_titleid(tid, &g) == 0) xstrlcpy(gid, g.id, sizeof gid);
    const char *notes = jv_get_str(it, "notes", "");
    const char *game = jv_get_str(it, "game", "");
    const char *fname = jv_get_str(it, "file_name", "");
    const char *url = util_istarts_with(fname, "http") ? fname : "";   /* legacy rows kept a link in file_name */
    char name[400];
    const char *nm = jv_get_str(it, "name", "");
    if (nm[0]) xstrlcpy(name, nm, sizeof name); else notes_name(notes, game, name, sizeof name);
    const char *cpath = jv_get_str(it, "cover_path", "");
    char cover[512] = "";
    if (bucket_path_ok(cpath)) xsnprintf(cover, sizeof cover, "/api/saves/store/%s/cover", id); else cpath = "";
    write_row(w, id, tid, gid, game[0] ? game : tid, infer_category(jv_get_str(it, "category", ""), notes),
              name, notes, jv_get_str(it, "shared_by", ""),
              jv_get_str(it, "game_version", ""), jv_get_str(it, "region", ""), "", jv_get_i64(it, "file_size", 0), jv_get_str(it, "sha256", ""),
              url, "", "xeno-save-zip", jv_get_str(it, "created_at", ""), (int)jv_get_i64(it, "downloads", 0), "published", jv_get(it, "tags"), "",
              garlic[0] && !storage[0] ? "garlic" : "supabase", storage, garlic, cover, 1, cpath, b->name);
    (*count)++;
    n++;
  }
  return n;
}

/* Cache file names per URL, so a copy fetched for another source or backend is never reused. */
static unsigned url_hash24(const char *url) {
  uint32_t hash = 2166136261u;
  for (const unsigned char *p = (const unsigned char *)url; *p; p++) hash = (hash ^ *p) * 16777619u;
  return hash & 0xffffff;
}

/* "kind|url" of the backend a merged copy was built from: a copy built for
 * another backend (published file changed, settings edited) is not served. */
static void backend_sig(const backend *b, char *out, size_t cap) { xsnprintf(out, cap, "%s|%s", b->kind, b->url); }

static void build_merged(int force) {
  /* follow the published saves-backend.json even when the feeds were never
   * refreshed (first start, catalog auto-refresh off); a user refresh looks
   * again when the copy is older than 10 minutes */
  cx_feed_ensure(CX_FEED_SAVES_BACKEND, force ? 600 : 6 * 3600);
  settings_t s;
  settings_get(&s);
  backend b;
  backend_get(&b);
  if (b.error[0]) LOGW("savestore: %s — listing the GitHub index only", b.error);
  char sig[sizeof merged_sig];
  backend_sig(&b, sig, sizeof sig);
  dedupe dd;
  dd.keys = calloc(INDEX_MAX_ROWS, ROW_KEY_LEN);
  dd.n = 0;
  jw w;
  jw_init(&w);
  jw_obj_begin(&w);
  jw_key(&w, "sources"); jw_arr_begin(&w);
  jw jr;
  jw_init(&jr);
  jw_arr_begin(&jr);
  int count = 0;
  if (!dd.keys) { jw_free(&jr); jw_free(&w); return; }
  /* GitHub index feeds */
  const char *urls[SET_MAX_INDEX_URLS + 1];
  int nu = 0;
  char def_index[600];
  const char *ov = cx_base_override("SAVES_INDEX");   /* host build tests: a local copy of the published index */
  if (*ov) xsnprintf(def_index, sizeof def_index, "%s/saves.json", ov); else xstrlcpy(def_index, DEFAULT_SAVES_INDEX, sizeof def_index);
  if (strcmp(b.kind, "none") != 0) urls[nu++] = def_index;
  for (int i = 0; i < s.saves.n_indexUrls; i++) urls[nu++] = s.saves.indexUrls[i];
  for (int i = 0; i < nu; i++) {
    if (!cx_feed_host_allowed(urls[i])) { LOGW("savestore: index host not allowed: %s", urls[i]); continue; }
    char name[48];
    xsnprintf(name, sizeof name, "saves-index-%06x", url_hash24(urls[i]));
    size_t len = 0;
    int fc = 0;
    cx_http o = {0};
    o.headers = "Cache-Control: no-cache\r\n";
    o.max_bytes = 8u << 20;
    char *txt = cached_request(name, urls[i], &o, INDEX_MAX_AGE_S, force, &len, &fc);
    int added = 0;
    if (txt) {
      jdoc *d = json_parse(txt, len, 400000, 32);
      if (d) { added = add_index_rows(&jr, json_root(d), "github", &dd, &count); json_free(d); }
      free(txt);
    }
    jw_obj_begin(&w); jw_kv_str(&w, "kind", "github"); jw_kv_str(&w, "url", urls[i]); jw_kv_int(&w, "rows", added); jw_kv_bool(&w, "ok", txt != NULL); jw_kv_bool(&w, "cached", fc); jw_obj_end(&w);
  }
  /* Supabase REST (read only: GET on the one table) */
  if (site_on(&b)) {
    char hdr[1400], url[600];
    int hok = supabase_headers(&b, hdr, sizeof hdr, "Accept: application/json\r\n") == 0;
    xsnprintf(url, sizeof url, "%s/rest/v1/ps5_save_collection?select=*&order=created_at.desc&limit=2000", b.url);
    cx_http o;
    sb_opt(&o, NULL, hdr, NULL, 0, 20000, 8u << 20);
    size_t len = 0;
    int fc = 0, added = 0;
    char cname[48];
    xsnprintf(cname, sizeof cname, "saves-supabase-%06x", url_hash24(b.url));
    char *txt = hok && backend_host_ok(&b, url) ? cached_request(cname, url, &o, INDEX_MAX_AGE_S, force, &len, &fc) : NULL;
    if (txt) { jdoc *d = json_parse(txt, len, 400000, 32); if (d) { added = add_supabase_rows(&jr, json_root(d), &b, &dd, &count); json_free(d); } free(txt); }
    jw_obj_begin(&w); jw_kv_str(&w, "kind", "supabase"); jw_kv_str(&w, "name", b.name); jw_kv_str(&w, "url", b.url); jw_kv_int(&w, "rows", added); jw_kv_bool(&w, "ok", txt != NULL); jw_kv_bool(&w, "cached", fc); jw_obj_end(&w);
    /* Listing through the function scrapes Garlic on the owner's side: only
     * when the published file opts in ("garlicList": true). The site's Garlic
     * rows are already in the table. */
    if (b.fn[0] && b.garlicList) {
      char furl[600];
      xsnprintf(furl, sizeof furl, "%s/functions/v1/%s", b.url, b.fn);
      hok = supabase_headers(&b, hdr, sizeof hdr, "Content-Type: application/json\r\n") == 0;
      cx_http fo;
      sb_opt(&fo, "POST", hdr, "{\"action\":\"list\"}", strlen("{\"action\":\"list\"}"), 30000, 8u << 20);
      len = 0; fc = 0; added = 0;
      xsnprintf(cname, sizeof cname, "saves-garlic-%06x", url_hash24(b.url));
      txt = hok && backend_host_ok(&b, furl) ? cached_request(cname, furl, &fo, INDEX_MAX_AGE_S, force, &len, &fc) : NULL;
      if (txt) {
        jdoc *d = json_parse(txt, len, 400000, 32);
        if (d) {
          jv r = json_root(d);
          jv arr = jv_type(r) == JV_ARR ? r : jv_get(r, "saves");
          if (jv_type(arr) != JV_ARR) arr = jv_get(r, "rows");
          if (jv_type(arr) != JV_ARR) arr = jv_get(r, "data");
          added = add_supabase_rows(&jr, arr, &b, &dd, &count);
          json_free(d);
        }
        free(txt);
      }
      jw_obj_begin(&w); jw_kv_str(&w, "kind", "garlic"); jw_kv_str(&w, "url", furl); jw_kv_int(&w, "rows", added); jw_kv_bool(&w, "ok", txt != NULL); jw_kv_bool(&w, "cached", fc); jw_obj_end(&w);
    }
  }
  jw_arr_end(&w);
  jw_arr_end(&jr);
  size_t rl;
  char *rows = jw_detach(&jr, &rl);
  jw_key(&w, "saves");
  if (rows) jw_raw(&w, rows, rl); else { jw_arr_begin(&w); jw_arr_end(&w); }
  free(rows);
  jw_kv_int(&w, "count", count);
  jw_kv_i64(&w, "updatedAt", (int64_t)util_now());
  jw_obj_end(&w);
  size_t len;
  char *txt = jw_detach(&w, &len);
  free(dd.keys);
  pthread_mutex_lock(&store_mu);
  free(merged_text);
  merged_text = txt;
  merged_len = txt ? len : 0;
  merged_at = util_now();
  xstrlcpy(merged_sig, sig, sizeof merged_sig);
  pthread_mutex_unlock(&store_mu);
  LOGI("savestore: index rebuilt (%d rows)", count);
}

/* Index thread: builds, then once more when a forced refresh arrived meanwhile. */
static void *build_thread(void *p) {
  int force = (int)(intptr_t)p;
  for (;;) {
    xthread_note("saves index rebuild%s", force ? " (refresh)" : "");
    build_merged(force);
    pthread_mutex_lock(&store_mu);
    if (pending_force) { pending_force = 0; force = 1; pthread_mutex_unlock(&store_mu); continue; }
    building = 0;
    pthread_cond_broadcast(&store_cv);
    pthread_mutex_unlock(&store_mu);
    return NULL;
  }
}

static void kick_build(int force) {
  pthread_mutex_lock(&store_mu);
  if (building) { if (force) pending_force = 1; pthread_mutex_unlock(&store_mu); return; }
  building = 1;
  pthread_mutex_unlock(&store_mu);
  if (xthread_start(NULL, "saves-index", build_thread, (void *)(intptr_t)force, 0, 1) != 0) {
    LOGW("savestore: index thread did not start, rebuilding inline");
    build_thread((void *)(intptr_t)force);
  }
}

/* Waits (bounded) until no rebuild is running. */
static void wait_build(int timeout_ms) {
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += timeout_ms / 1000;
  until.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
  pthread_mutex_lock(&store_mu);
  while (building) if (pthread_cond_timedwait(&store_cv, &store_mu, &until) == ETIMEDOUT) break;
  pthread_mutex_unlock(&store_mu);
}

/* Parsed copy of the merged index (caller frees). A stale copy is served as-is
 * while the index thread refreshes it; with no copy yet (or refresh=1) the
 * caller waits up to wait_ms. *pending is set when a rebuild is still running. */
static jdoc *merged_doc(int refresh, int wait_ms, int *pending) {
  backend b;
  backend_get(&b);
  char sig[sizeof merged_sig];
  backend_sig(&b, sig, sizeof sig);
  pthread_mutex_lock(&store_mu);
  int have = merged_text != NULL && strcmp(merged_sig, sig) == 0;
  int stale = !have || util_now() - merged_at >= INDEX_MAX_AGE_S;
  pthread_mutex_unlock(&store_mu);
  if (stale || refresh) kick_build(refresh);
  if (!have || refresh) wait_build(wait_ms);
  pthread_mutex_lock(&store_mu);
  jdoc *d = merged_text ? json_parse(merged_text, merged_len, 400000, 32) : NULL;
  if (pending) *pending = building;
  pthread_mutex_unlock(&store_mu);
  return d;
}

static void register_routes(void);

void cx_savestore_init(void) { register_routes(); }
void cx_savestore_tick(void) {}

/* ---- installed version -------------------------------------------------- */

static int read_content_version(const char *path, char *out, size_t cap) {
  jdoc *d = cx_json_file(path, 1 << 20, 20000);
  if (!d) return -1;
  const char *v = jv_get_str(json_root(d), "contentVersion", "");
  if (!*v) v = jv_get_str(json_root(d), "masterVersion", "");
  if (!*v) v = jv_get_str(json_root(d), "version", "");
  int rc = -1;
  if (*v) { xstrlcpy(out, v, cap); rc = 0; }
  json_free(d);
  return rc;
}

int cx_installed_version(const char *titleId, char *out, size_t cap, char *source, size_t scap) {
  out[0] = 0;
  if (source) source[0] = 0;
  if (!cx_titleid_valid(titleId)) return -1;
  char p[800];
  xsnprintf(p, sizeof p, "%s/%s/param.json", cx_appmeta_root(), titleId);
  if (read_content_version(p, out, cap) == 0) { if (source) xstrlcpy(source, "appmeta", scap); return 0; }
  xsnprintf(p, sizeof p, "%s/%s/sce_sys/param.json", storage_user_app_dir(), titleId);
  if (read_content_version(p, out, cap) == 0) { if (source) xstrlcpy(source, "app", scap); return 0; }
  return -1;
}

/* ---- list with compatibility -------------------------------------------- */

static int row_matches_q(jv row, const char *q) {
  if (!q || !*q) return 1;
  char f[160], h[600];
  util_fold(q, f, sizeof f);
  util_trim(f);
  if (!f[0]) return 1;
  const char *keys[] = {"title", "titleId", "author", "name", "description"};
  for (size_t i = 0; i < XS_ARRAY_LEN(keys); i++) {
    util_fold(jv_get_str(row, keys[i], ""), h, sizeof h);
    if (strstr(h, f)) return 1;
  }
  return 0;
}

static void compat_json(jw *w, jv row) {
  const char *tid = jv_get_str(row, "titleId", "");
  const char *sv = jv_get_str(row, "gameVersion", "");
  int installed = storage_title_installed(tid);
  char iv[32] = "", src[12] = "";
  if (installed) cx_installed_version(tid, iv, sizeof iv, src, sizeof src);
  jw_kv_bool(w, "installed", installed);
  jw_kv_str(w, "installedVersion", iv);
  const char *compat;
  char text[200];
  int cmp = 0;
  if (!installed) {
    compat = "not_installed";
    xsnprintf(text, sizeof text, "Game not installed");
    /* another region of the same game installed? */
    const char *gid = jv_get_str(row, "gameId", "");
    cat_game g;
    if (gid[0] && catalog_find_game(gid, &g) == 0) {
      for (int i = 0; i < g.ntitleIds; i++)
        if (strcmp(g.titleIds[i], tid) != 0 && storage_title_installed(g.titleIds[i])) {
          jw_kv_bool(w, "otherRegion", 1);
          jw_kv_str(w, "installedTitleId", g.titleIds[i]);
          xsnprintf(text, sizeof text, "You have %s installed — a save never loads on a different title ID", g.titleIds[i]);
          break;
        }
    }
  } else if (!sv[0] || !iv[0]) {
    compat = "unknown";
    xsnprintf(text, sizeof text, "Version unknown — check before installing");
  } else {
    cmp = util_semver_cmp(sv, iv);
    if (cmp > 0) { compat = "update_game"; xsnprintf(text, sizeof text, "Made on %s, you have %s — update the game first", sv, iv); }
    else if (cmp == 0) { compat = "match"; xsnprintf(text, sizeof text, "Matches your %s", iv); }
    else { compat = "match"; xsnprintf(text, sizeof text, "Made on %s, you have %s (newer game loads older saves)", sv, iv); }
  }
  jw_kv_int(w, "versionCompare", cmp);
  jw_kv_str(w, "compat", compat);
  jw_kv_str(w, "compatText", text);
}

int cx_savestore_list_json(const char *titleId, const char *category, const char *q, int refresh, jw *w) {
  int pending = 0;
  jdoc *d = merged_doc(refresh, LIST_WAIT_MS, &pending);
  jw_key(w, "saves");
  jw_arr_begin(w);
  int n = 0;
  if (d) {
    JV_FOREACH(row, jv_get(json_root(d), "saves")) {
      if (titleId && *titleId && !util_strieq(jv_get_str(row, "titleId", ""), titleId)) continue;
      if (category && *category && !util_strieq(jv_get_str(row, "category", ""), category)) continue;
      if (!row_matches_q(row, q)) continue;
      jw_obj_begin(w);
      JV_FOREACH(m, row) { jw_key(w, jv_key(m)); jw_value(w, m); }
      compat_json(w, row);
      jw_obj_end(w);
      n++;
    }
  }
  jw_arr_end(w);
  jw_kv_int(w, "count", n);
  jw_key(w, "sources");
  if (d && jv_ok(jv_get(json_root(d), "sources"))) jw_value(w, jv_get(json_root(d), "sources")); else { jw_arr_begin(w); jw_arr_end(w); }
  jw_kv_i64(w, "updatedAt", d ? jv_get_i64(json_root(d), "updatedAt", 0) : 0);
  jw_kv_bool(w, "building", pending);
  backend b;
  backend_get(&b);
  jw_kv_str(w, "backend", b.kind);
  /* the share wizard defaults to the site upload when the database backend is on */
  jw_kv_str(w, "backendName", site_on(&b) ? b.name : "");
  jw_kv_bool(w, "siteUpload", site_on(&b));
  if (b.error[0]) jw_kv_str(w, "backendError", b.error);   /* the Settings override is not usable */
  jw_kv_str(w, "engine", cx_tier_name(cx_savemount_tier()));
  if (d) json_free(d);
  return 0;
}

/* Copies a row by id into a small struct for the job worker. */
typedef struct row_copy {
  char id[80], titleId[12], title[160], name[160], url[1024], mirror[512], sha256[65], storagePath[256], garlicId[24], dirName[32], source[12];
  int64_t size;
} row_copy;

static int find_row(const char *id, row_copy *out) {
  jdoc *d = merged_doc(0, ROW_WAIT_MS, NULL);
  if (!d) return -1;
  int rc = -1;
  JV_FOREACH(row, jv_get(json_root(d), "saves")) {
    if (strcmp(jv_get_str(row, "id", ""), id) != 0) continue;
    memset(out, 0, sizeof *out);
    xstrlcpy(out->id, id, sizeof out->id);
    xstrlcpy(out->titleId, jv_get_str(row, "titleId", ""), sizeof out->titleId);
    xstrlcpy(out->title, jv_get_str(row, "title", ""), sizeof out->title);
    xstrlcpy(out->name, jv_get_str(row, "name", ""), sizeof out->name);
    xstrlcpy(out->url, jv_get_str(row, "url", ""), sizeof out->url);
    xstrlcpy(out->mirror, jv_get_str(row, "mirror", ""), sizeof out->mirror);
    xstrlcpy(out->sha256, jv_get_str(row, "sha256", ""), sizeof out->sha256);
    xstrlcpy(out->storagePath, jv_get_str(row, "storagePath", ""), sizeof out->storagePath);
    xstrlcpy(out->garlicId, jv_get_str(row, "garlicId", ""), sizeof out->garlicId);
    xstrlcpy(out->dirName, jv_get_str(row, "dirName", ""), sizeof out->dirName);
    xstrlcpy(out->source, jv_get_str(row, "source", ""), sizeof out->source);
    out->size = jv_get_i64(row, "size", 0);
    rc = 0;
    break;
  }
  json_free(d);
  return rc;
}

/* ---- supabase helpers --------------------------------------------------- */

/* Signed URL for an object of the bucket (POST /storage/v1/object/sign/…,
 * which the site allows for the anon key). */
static int supabase_sign(const backend *b, const char *path, int expires_s, char *out, size_t cap) {
  if (!bucket_path_ok(path)) return -1;
  char url[800], hdr[1400], enc[700], body[48];
  util_url_encode_path(path, enc, sizeof enc);
  xsnprintf(url, sizeof url, "%s/storage/v1/object/sign/%s/%s", b->url, b->bucket, enc);
  if (supabase_headers(b, hdr, sizeof hdr, "Content-Type: application/json\r\n") != 0 || !backend_host_ok(b, url)) return -1;
  xsnprintf(body, sizeof body, "{\"expiresIn\":%d}", expires_s);
  cx_http o;
  sb_opt(&o, "POST", hdr, body, strlen(body), 15000, 64 << 10);
  int status = 0;
  size_t len = 0;
  char *t = cx_fetch(url, &o, &status, &len, NULL);
  int rc = -1;
  if (t && status >= 200 && status < 300) {
    jdoc *d = json_parse(t, len, 1000, 8);
    if (d) {
      const char *su = jv_get_str(json_root(d), "signedURL", jv_get_str(json_root(d), "signedUrl", ""));
      if (util_istarts_with(su, "http")) { if (backend_host_ok(b, su) && strlen(su) < cap) { xstrlcpy(out, su, cap); rc = 0; } }
      else if (su[0] == '/') { int n = snprintf(out, cap, "%s/storage/v1%s", b->url, su); rc = (n > 0 && (size_t)n < cap) ? 0 : -1; }
      json_free(d);
    }
  } else LOGW("savestore: signing %s failed (HTTP %d)", path, status);
  free(t);
  return rc;
}

/* Signed cover URLs are reused until shortly before they expire, so a grid of
 * covers costs the site one signature per cover and hour, not per view. */
#define COVER_CACHE_N 96
#define COVER_TTL_S 3600
static struct { char id[80]; char url[1024]; time_t until; } cover_cache[COVER_CACHE_N];
static int cover_next;
static pthread_mutex_t cover_mu = PTHREAD_MUTEX_INITIALIZER;

int cx_savestore_cover_url(const char *id, char *out, size_t cap) {
  out[0] = 0;
  time_t now = util_now();
  pthread_mutex_lock(&cover_mu);
  for (int i = 0; i < COVER_CACHE_N; i++)
    if (cover_cache[i].id[0] && strcmp(cover_cache[i].id, id) == 0 && cover_cache[i].until > now) { xstrlcpy(out, cover_cache[i].url, cap); break; }
  pthread_mutex_unlock(&cover_mu);
  if (out[0]) return 0;
  jdoc *d = merged_doc(0, 5000, NULL);
  if (!d) return -1;
  int rc = -1;
  JV_FOREACH(row, jv_get(json_root(d), "saves")) {
    if (strcmp(jv_get_str(row, "id", ""), id) != 0) continue;
    const char *cover = jv_get_str(row, "cover", "");
    if (util_istarts_with(cover, "http")) { xstrlcpy(out, cover, cap); rc = 0; break; }
    /* database row: cover_path is an object of the bucket */
    const char *cp = jv_get_str(row, "coverPath", "");
    if (util_starts_with(id, "sb:") && cp[0]) {
      backend b;
      backend_get(&b);
      char su[1024];
      if (site_on(&b) && supabase_sign(&b, cp, COVER_TTL_S, su, sizeof su) == 0) {
        xstrlcpy(out, su, cap);
        rc = 0;
        pthread_mutex_lock(&cover_mu);
        int slot = cover_next++ % COVER_CACHE_N;
        xstrlcpy(cover_cache[slot].id, id, sizeof cover_cache[slot].id);
        xstrlcpy(cover_cache[slot].url, su, sizeof cover_cache[slot].url);
        cover_cache[slot].until = now + COVER_TTL_S - 600;   /* the 302 is cached 50 min by the browser */
        pthread_mutex_unlock(&cover_mu);
      }
    }
    break;
  }
  json_free(d);
  return rc;
}

/* ---- install / download jobs -------------------------------------------- */

typedef struct inst_arg {
  row_copy row;
  uint32_t uid;
  int backup_first, download_only;
  char dest[1100];
} inst_arg;

static int dl_progress(void *user, int64_t done, int64_t total) {
  cx_job *j = user;
  cx_job_progress(j, done, total > 0 ? total : j->total, "Downloading");
  return cx_job_cancelled(j);
}

static int download_host_ok(const backend *b, const char *url) {
  char host[256];
  util_url_host(url, host, sizeof host);
  return cx_host_in(host, DOWNLOAD_HOSTS, XS_ARRAY_LEN(DOWNLOAD_HOSTS)) || cx_host_is_loopback(host) || (b && backend_host_ok(b, url));
}

/* 1 when the file starts like a zip (local header, or an empty archive). */
static int file_is_zip(const char *path) {
  unsigned char m[4] = {0};
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  size_t n = fread(m, 1, 4, f);
  fclose(f);
  return n == 4 && m[0] == 'P' && m[1] == 'K' && ((m[2] == 3 && m[3] == 4) || (m[2] == 5 && m[3] == 6));
}

static int fetch_zip(cx_job *j, inst_arg *a, char *err, size_t errcap) {
  backend b;
  backend_get(&b);
  row_copy *r = &a->row;
  const char *who = b.name[0] ? b.name : "the save database";
  if (r->storagePath[0] && site_on(&b)) {
    /* bucket object: a short-lived signed URL, then a plain download (no key) */
    char signed_url[1200];
    if (supabase_sign(&b, r->storagePath, 300, signed_url, sizeof signed_url) != 0) { xsnprintf(err, errcap, "%s did not sign the download of this save", who); return -1; }
    return cx_download_file(signed_url, NULL, a->dest, r->sha256, SAVE_ZIP_MAX, dl_progress, j, &j->cancel, err, errcap);
  }
  if (r->garlicId[0] && site_on(&b) && b.fn[0]) {
    /* Garlic row: the site's function signs in to Garlic and streams the zip */
    char url[600], hdr[1400], body[96];
    xsnprintf(url, sizeof url, "%s/functions/v1/%s", b.url, b.fn);
    if (supabase_headers(&b, hdr, sizeof hdr, "Content-Type: application/json\r\nAccept: application/zip, application/json\r\n") != 0 || !backend_host_ok(&b, url)) {
      xsnprintf(err, errcap, "backend host not allowed"); return -1;
    }
    for (const char *p = r->garlicId; *p; p++) if (*p < '0' || *p > '9') { xsnprintf(err, errcap, "bad Garlic id"); return -1; }
    xsnprintf(body, sizeof body, "{\"action\":\"download\",\"id\":%s}", r->garlicId);
    cx_http o;
    sb_opt(&o, "POST", hdr, body, strlen(body), 90000, 0);
    char e2[200] = "";
    if (cx_download_file(url, &o, a->dest, "", SAVE_ZIP_MAX, dl_progress, j, &j->cancel, e2, sizeof e2) != 0) {
      if (cx_job_cancelled(j)) xsnprintf(err, errcap, "%s", e2);
      else xsnprintf(err, errcap, "%s could not fetch this save from Garlic (%s) — try again later", who, e2);
      return -1;
    }
    const char *ct = cx_download_last_type();
    if (util_strcasestr(ct, "zip") || file_is_zip(a->dest)) {
      if (!file_is_zip(a->dest)) { util_unlink_quiet(a->dest); xsnprintf(err, errcap, "%s returned a damaged zip", who); return -1; }
      return 0;
    }
    /* not a zip: a JSON answer carries an error (or, on older functions, a link) */
    size_t len = 0;
    char *t = util_read_file(a->dest, &len, 64 << 10);
    char next[1024] = "", why[160] = "";
    if (t) {
      jdoc *d = json_parse(t, len, 1000, 8);
      if (d) {
        jv rr = json_root(d);
        xstrlcpy(next, jv_get_str(rr, "url", jv_get_str(rr, "download_url", "")), sizeof next);
        xstrlcpy(why, jv_get_str(rr, "error", jv_get_str(rr, "message", "")), sizeof why);
        json_free(d);
      }
      free(t);
    }
    util_unlink_quiet(a->dest);
    if (util_istarts_with(next, "http") && download_host_ok(&b, next))
      return cx_download_file(next, NULL, a->dest, r->sha256, SAVE_ZIP_MAX, dl_progress, j, &j->cancel, err, errcap);
    if (why[0]) xsnprintf(err, errcap, "%s could not fetch this save from Garlic: %s", who, why);
    else xsnprintf(err, errcap, "%s did not return a zip for this save", who);
    return -1;
  }
  const char *urls[2] = {r->url, r->mirror};
  int tried = 0;
  for (int i = 0; i < 2; i++) {
    if (!util_istarts_with(urls[i], "http")) continue;
    if (!download_host_ok(&b, urls[i])) { xsnprintf(err, errcap, "download host not allowed"); continue; }
    tried = 1;
    if (cx_download_file(urls[i], NULL, a->dest, r->sha256, SAVE_ZIP_MAX, dl_progress, j, &j->cancel, err, errcap) == 0) return 0;
    if (cx_job_cancelled(j)) return -1;
  }
  if (!tried && !err[0]) xsnprintf(err, errcap, "%s", (r->garlicId[0] || r->storagePath[0]) ? "the save database backend is not configured on this console" : "the index row has no download URL");
  return -1;
}

/* Download counter: only for a backend whose published file names its RPC
 * ("downloadCounter": "increment_downloads", the template backend), or a
 * Settings backend that backend_traits() found to be one. The XenoKing site
 * has none, so a console never calls anything there but the reads, the bucket
 * upload and the one insert of a share. The GitHub index has no counter
 * either; its rows carry whatever count the index file holds. */
static void bump_downloads(const row_copy *r) {
  if (!util_starts_with(r->id, "sb:") || !r->id[3]) return;
  backend b;
  backend_get(&b);
  backend_traits(&b);   /* a Settings backend built from backend/supabase has increment_downloads */
  if (!site_on(&b) || !b.counter[0] || !xhttp_available()) return;
  char url[600], hdr[1400], body[160];
  xsnprintf(url, sizeof url, "%s/rest/v1/rpc/%s", b.url, b.counter);
  if (!backend_host_ok(&b, url) || supabase_headers(&b, hdr, sizeof hdr, "Content-Type: application/json\r\n") != 0) return;
  jw bw;
  jw_init(&bw);
  jw_obj_begin(&bw);
  jw_kv_str(&bw, "id", r->id + 3);
  jw_obj_end(&bw);
  size_t bl;
  char *bt = jw_detach(&bw, &bl);
  if (!bt) return;
  xstrlcpy(body, bt, sizeof body);
  free(bt);
  cx_http o;
  sb_opt(&o, "POST", hdr, body, strlen(body), 10000, 64 << 10);
  int status = 0;
  size_t len = 0;
  char *resp = cx_fetch(url, &o, &status, &len, NULL);
  if (!resp || status < 200 || status >= 300) LOGW("savestore: download counter for %s not bumped (HTTP %d)", r->id, status);
  free(resp);
}

static int install_worker(cx_job *j, void *p) {
  inst_arg *a = p;
  char err[256] = "";
  /* a running (or suspended) game keeps its save mounted: refuse before downloading */
  if (!a->download_only && cx_title_running(a->row.titleId) == 1) {
    char msg[CXJ_MSG];
    xsnprintf(msg, sizeof msg, "%s is running: close the game (not just suspend it) before its save is replaced", a->row.titleId);
    cx_job_fail(j, "title_running", msg);
    return -1;
  }
  if (!a->download_only && cx_savemount_tier() != CX_TIER_NATIVE) {
    /* Community saves are re-signed for this account by Garlic Save Manager:
     * make sure it answers (starting it when it is not running) before
     * anything is downloaded, so a missing engine fails in seconds. */
    cx_job_progress(j, 0, 0, "Checking Garlic Save Manager");
    char gerr[200] = "";
    if (!cx_garlic_ensure(1, gerr, sizeof gerr)) {
      char msg[CXJ_MSG];
      xsnprintf(msg, sizeof msg, "This save needs Garlic Save Manager to be re-signed for your account: %s", gerr[0] ? gerr : "it is not running");
      cx_job_fail(j, cx_job_cancelled(j) ? "cancelled" : "garlic_unavailable", msg);
      return -1;
    }
    if (cx_job_cancelled(j)) { cx_job_fail(j, "cancelled", "Install cancelled"); return -1; }
  }
  cx_job_progress(j, 0, a->row.size, "Downloading");
  if (storage_ensure_parent_dir(a->dest) != 0) { cx_job_fail(j, "io", "Cannot create the community saves folder"); return -1; }
  if (fetch_zip(j, a, err, sizeof err) != 0) { cx_job_fail(j, cx_job_cancelled(j) ? "cancelled" : "download_failed", err); util_unlink_quiet(a->dest); return -1; }
  char sha[65] = "";
  cx_sha256_file(a->dest, sha);
  jw rw;
  jw_init(&rw);
  jw_obj_begin(&rw);
  jw_kv_str(&rw, "zipPath", a->dest);
  jw_kv_str(&rw, "sha256", sha);
  jw_kv_i64(&rw, "size", util_file_size(a->dest));
  jw_kv_str(&rw, "titleId", a->row.titleId);
  if (a->download_only) {
    jw_obj_end(&rw);
    cx_job_set_result(j, &rw);
    bump_downloads(&a->row);
    cx_job_progress(j, 1, 1, "Downloaded");
    return 0;
  }
  cx_job_progress(j, 0, 0, "Installing to your saves");
  char base[1024], code[32], e2[256];
  cx_saves_backup_dir(base, sizeof base);
  cx_import_result ir;
  if (cx_savemount_import_zip_job(a->uid, a->row.titleId, a->row.dirName, a->dest, a->backup_first, base, 0, j, &ir, code, sizeof code, e2, sizeof e2) != 0) {
    jw_free(&rw);
    cx_job_fail(j, code, e2);
    return -1;
  }
  jw_kv_str(&rw, "tier", ir.tier);
  jw_kv_str(&rw, "style", ir.style);
  jw_kv_str(&rw, "preBackup", ir.preBackup);
  jw_kv_int(&rw, "files", ir.files);
  jw_kv_i64(&rw, "bytes", ir.bytes);
  jw_kv_str(&rw, "accountId", ir.accountId);
  if (ir.note[0]) jw_kv_str(&rw, "note", ir.note);
  jw_obj_end(&rw);
  cx_job_set_result(j, &rw);
  bump_downloads(&a->row);
  cx_job_progress(j, 1, 1, "Save installed — launch the game");
  return 0;
}

int cx_savestore_install(const char *id, uint32_t uid, int backup_first, int download_only, char *job_id, size_t idcap, char *code, size_t ccap, char *err, size_t errcap) {
  xstrlcpy(code, "bad_request", ccap);
  inst_arg *a = calloc(1, sizeof *a);
  if (!a) { xstrlcpy(code, "oom", ccap); xsnprintf(err, errcap, "out of memory"); return -1; }
  if (!id || !*id || find_row(id, &a->row) != 0) { free(a); xstrlcpy(code, "not_found", ccap); xsnprintf(err, errcap, "unknown save id"); return -1; }
  if (!uid) uid = cx_user_foreground();
  if (!download_only && cx_user_find(uid, NULL) != 0) { free(a); xsnprintf(err, errcap, "unknown user"); return -1; }
  if (!xhttp_available()) { free(a); xstrlcpy(code, "downloads_unavailable", ccap); xsnprintf(err, errcap, "network transport unavailable"); return -1; }
  a->uid = uid;
  a->backup_first = backup_first;
  a->download_only = download_only;
  settings_t s;
  settings_get(&s);
  storage_root sr;
  if (storage_find_root(s.defaultRoot, &sr) != 0) {
    storage_root roots[STORAGE_MAX_ROOTS];
    if (storage_get_roots(roots, STORAGE_MAX_ROOTS) < 1) { free(a); xstrlcpy(code, "root_unavailable", ccap); xsnprintf(err, errcap, "no drive available"); return -1; }
    sr = roots[0];
  }
  /* titleId/id/title all come from the community index (external data): every
   * component of the destination path goes through storage_sanitize_filename,
   * so a hostile "../" titleId cannot escape the saves folder. */
  char tdir[64], fname[200], safe_id[80], safe_tid[24];
  if (storage_sanitize_filename(a->row.titleId, safe_tid, sizeof safe_tid) != 0) xstrlcpy(safe_tid, "save", sizeof safe_tid);
  if (storage_sanitize_filename(a->row.title[0] ? a->row.title : a->row.titleId, tdir, sizeof tdir) != 0) xstrlcpy(tdir, safe_tid, sizeof tdir);
  if (storage_sanitize_filename(a->row.id, safe_id, sizeof safe_id) != 0) xstrlcpy(safe_id, "save", sizeof safe_id);
  xsnprintf(fname, sizeof fname, "%s_%s.zip", safe_tid, safe_id);
  xsnprintf(a->dest, sizeof a->dest, "%s/xeno-store/saves/community/%s/%s", sr.path, tdir, fname);
  char title[256];
  xsnprintf(title, sizeof title, "%s — %s", a->row.title[0] ? a->row.title : a->row.titleId, a->row.name[0] ? a->row.name : a->row.id);
  if (cx_job_start(download_only ? "save-download" : "save-install", title, install_worker, a, job_id, idcap) != 0) { free(a); xstrlcpy(code, "busy", ccap); xsnprintf(err, errcap, "too many operations running"); return -1; }
  return 0;
}

/* ---- export ------------------------------------------------------------- */

/* A decrypted, shareable export (native engine or Garlic Save Manager). */
typedef struct share_export {
  char tier[8], zipPath[1200], sha256[65], format[20], accountId[8];
  int64_t size;
  int files;
  char *manifest;          /* JSON text, malloc'd */
} share_export;

/* ACCOUNT_ID state of a param.sfo image: 1 zeroed (or the whole file is
 * zeroed), -1 an id is still set or the file is not a readable PSF. */
static int sfo_account_state(const uint8_t *buf, size_t len) {
  size_t i = 0;
  while (i < len && !buf[i]) i++;
  if (i == len) return 1;
  if (len < 20 || memcmp(buf, "\0PSF", 4) != 0) return -1;
  uint8_t *copy = malloc(len);
  if (!copy) return -1;
  memcpy(copy, buf, len);
  int rc = -1;
  if (cx_sfo_set_account_id(copy, len, 0) == 0) rc = memcmp(copy, buf, len) == 0 ? 1 : -1;
  free(copy);
  return rc;
}

/* Same for the param.sfo inside a save zip: 1 present with no account id,
 * 0 no param.sfo in the zip, -1 an account id is still there. */
static int zip_account_state(const char *zipPath) {
  void *buf = NULL;
  size_t len = 0;
  if (zip_extract_to_memory(zipPath, "param.sfo", &buf, &len, 1 << 20) != 0) return 0;
  int rc = sfo_account_state(buf, len);
  free(buf);
  return rc;
}

/* Zeroes ACCOUNT_ID in <root>/sce_sys/param.sfo: 1 done, 0 no param.sfo,
 * -1 it could not be removed (the export must not be shared then). */
static int strip_account_id(const char *root) {
  char p[1400];
  xsnprintf(p, sizeof p, "%s/sce_sys/param.sfo", root);
  if (!util_file_exists(p)) return 0;
  size_t sl = 0;
  char *sfo = util_read_file(p, &sl, 1 << 20);
  if (!sfo) return -1;
  int rc = -1;
  if (sfo_account_state((const uint8_t *)sfo, sl) == 1) rc = 1;
  else if (sl >= 20 && memcmp(sfo, "\0PSF", 4) == 0 && cx_sfo_set_account_id((uint8_t *)sfo, sl, 0) == 0 && util_write_file_atomic(p, sfo, sl) == 0) rc = 1;
  free(sfo);
  return rc;
}

/* Drops what never leaves the console: sealed keys, raw save images, our own
 * manifests (a fresh one is written) and anything that is not a plain file. */
static void prune_unshareable(const char *dir, int depth) {
  if (depth > 8) return;
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char p[1400];
    xsnprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat sb;
    if (lstat(p, &sb) != 0) continue;
    if (S_ISDIR(sb.st_mode)) { prune_unshareable(p, depth + 1); continue; }
    if (!S_ISREG(sb.st_mode) || util_strieq(e->d_name, "sealedkey") || util_strieq(e->d_name, "keystone") || util_istarts_with(e->d_name, "sdimg_") ||
        (depth == 0 && (util_strieq(e->d_name, "xeno-save.json") || util_strieq(e->d_name, "xeno-backup.json"))))
      unlink(p);
  }
  closedir(d);
}

/* The folder that holds the save inside an extracted Garlic export: the files
 * may sit at the top, below one wrapper folder, or below <titleId>/<save>/
 * when Garlic lists every save of the title (then the one named dn wins). */
static int find_save_root(const char *dir, const char *dn, char *out, size_t cap) {
  char cur[1300], img[48];
  xstrlcpy(cur, dir, sizeof cur);
  xsnprintf(img, sizeof img, "sdimg_%s", dn);
  for (int depth = 0; depth < 4; depth++) {
    DIR *d = opendir(cur);
    if (!d) return -1;
    int files = 0, dirs = 0, sys = 0;
    char only[256] = "", match[256] = "";
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      if (e->d_name[0] == '.') continue;
      char p[1600];
      xsnprintf(p, sizeof p, "%s/%s", cur, e->d_name);
      struct stat sb;
      if (lstat(p, &sb) != 0) continue;
      if (S_ISREG(sb.st_mode)) files++;
      else if (S_ISDIR(sb.st_mode)) {
        dirs++;
        if (strcmp(e->d_name, "sce_sys") == 0) sys = 1;
        xstrlcpy(only, e->d_name, sizeof only);
        if (strcmp(e->d_name, dn) == 0 || strcmp(e->d_name, img) == 0) xstrlcpy(match, e->d_name, sizeof match);
      }
    }
    closedir(d);
    if (files > 0 || sys) { xstrlcpy(out, cur, cap); return 0; }
    const char *next = match[0] ? match : dirs == 1 ? only : NULL;
    if (!next) return -1;
    size_t l = strlen(cur);
    if (l + 1 + strlen(next) >= sizeof cur) return -1;
    xsnprintf(cur + l, sizeof cur - l, "/%s", next);
  }
  return -1;
}

static void manifest_files(const char *root, const char *rel, jw *w, int depth, int *files) {
  if (depth > 8) return;
  char dir[1400];
  if (rel[0]) xsnprintf(dir, sizeof dir, "%s/%s", root, rel); else xstrlcpy(dir, root, sizeof dir);
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char relp[1024], full[1600];
    if (rel[0]) xsnprintf(relp, sizeof relp, "%s/%s", rel, e->d_name); else xstrlcpy(relp, e->d_name, sizeof relp);
    xsnprintf(full, sizeof full, "%s/%s", root, relp);
    struct stat sb;
    if (lstat(full, &sb) != 0) continue;
    if (S_ISDIR(sb.st_mode)) { manifest_files(root, relp, w, depth + 1, files); continue; }
    if (!S_ISREG(sb.st_mode)) continue;
    char hex[65] = "";
    cx_sha256_file(full, hex);
    jw_obj_begin(w);
    jw_kv_str(w, "path", relp);
    jw_kv_i64(w, "size", (int64_t)sb.st_size);
    jw_kv_str(w, "sha256", hex);
    jw_obj_end(w);
    (*files)++;
  }
  closedir(d);
}

/* Decrypted export through Garlic Save Manager, repacked as an xeno-save-zip:
 * sealed keys and raw images dropped, ACCOUNT_ID zeroed, manifest written.
 * 0 ok, -1 failed (err), -2 the account id could not be removed. */
static int garlic_export(uint32_t uid, const char *titleId, const char *dn, const char *dest, const char *gameVersion, const char *region,
                         share_export *x, char *err, size_t errcap) {
  char rnd[12], work[1100], tmpzip[1200], stage[1200], root[1300];
  util_rand_hex(rnd, 8);
  xsnprintf(work, sizeof work, "%s/stage", cx_data_dir());
  if (cx_is_host()) xsnprintf(work, sizeof work, "%s/sim/stage", cx_data_dir());
  xsnprintf(tmpzip, sizeof tmpzip, "%s/garlic-%s-%s.zip", work, titleId, rnd);
  xsnprintf(stage, sizeof stage, "%s/garlic-%s-%s", work, titleId, rnd);
  if (util_mkdir_p(work) != 0) { xsnprintf(err, errcap, "cannot create the staging folder"); return -1; }
  int rc = cx_garlic_export_dec(titleId, dn, uid, tmpzip, NULL, err, errcap);   /* the request waits for the answer */
  if (rc != 0 || !util_file_exists(tmpzip)) {
    if (!err[0]) xsnprintf(err, errcap, "Garlic Save Manager returned no export");
    util_unlink_quiet(tmpzip);
    return -1;
  }
  zip_stats st;
  memset(&st, 0, sizeof st);
  rc = zip_extract_to_dir(tmpzip, stage, (uint64_t)SAVE_ZIP_MAX, NULL, NULL, &st);
  util_unlink_quiet(tmpzip);
  if (rc != 0) { cx_rm_rf(stage, 10); xsnprintf(err, errcap, "cannot read the Garlic export: %s", st.error); return -1; }
  if (find_save_root(stage, dn, root, sizeof root) != 0) { cx_rm_rf(stage, 10); xsnprintf(err, errcap, "the Garlic export has no save folder %s", dn); return -1; }
  prune_unshareable(root, 0);
  int acct = strip_account_id(root);
  if (acct < 0) { cx_rm_rf(stage, 10); xsnprintf(err, errcap, "the account ID could not be removed from sce_sys/param.sfo"); return -2; }
  if (cx_count_files(root, 8) == 0) { cx_rm_rf(stage, 10); xsnprintf(err, errcap, "the Garlic export of %s is empty", dn); return -1; }
  jw mw;
  jw_init(&mw);
  jw_obj_begin(&mw);
  jw_kv_str(&mw, "titleId", titleId);
  jw_kv_str(&mw, "dirName", dn);
  jw_kv_str(&mw, "kind", titleId[0] == 'P' ? "ps5" : "ps4");
  jw_kv_str(&mw, "format", "xeno-save-zip");
  jw_kv_str(&mw, "gameVersion", gameVersion);
  jw_kv_str(&mw, "region", region);
  jw_kv_bool(&mw, "accountIdStripped", 1);
  jw_kv_bool(&mw, "sameConsoleOnly", 0);
  jw_kv_str(&mw, "engine", "garlic");
  jw_kv_str(&mw, "createdBy", "XENO STORE " XENO_VERSION);
  char iso[40];
  util_iso8601(util_now(), iso, sizeof iso);
  jw_kv_str(&mw, "createdAt", iso);
  int files = 0;
  jw_key(&mw, "files"); jw_arr_begin(&mw);
  manifest_files(root, "", &mw, 0, &files);
  jw_arr_end(&mw);
  jw_obj_end(&mw);
  size_t ml = 0;
  char *mtxt = jw_detach(&mw, &ml);
  char mp[1400];
  xsnprintf(mp, sizeof mp, "%s/xeno-save.json", root);
  if (!mtxt || util_write_file_atomic(mp, mtxt, ml) != 0) { free(mtxt); cx_rm_rf(stage, 10); xsnprintf(err, errcap, "cannot write the manifest"); return -1; }
  storage_ensure_parent_dir(dest);
  memset(&st, 0, sizeof st);
  rc = zip_create_from_dir(dest, root, NULL, NULL, &st);
  cx_rm_rf(stage, 10);
  if (rc != 0) { free(mtxt); util_unlink_quiet(dest); xsnprintf(err, errcap, "zip failed: %s", st.error); return -1; }
  xstrlcpy(x->tier, "garlic", sizeof x->tier);
  xstrlcpy(x->zipPath, dest, sizeof x->zipPath);
  xstrlcpy(x->format, "xeno-save-zip", sizeof x->format);
  xstrlcpy(x->accountId, acct == 1 ? "removed" : "none", sizeof x->accountId);
  x->size = util_file_size(dest);
  x->files = files;
  cx_sha256_file(dest, x->sha256);
  x->manifest = mtxt;
  LOGI("savestore: Garlic export %s/%s -> %s (%d files)", titleId, dn, dest, files);
  return 0;
}

int cx_savestore_export(uint32_t uid, const char *titleId, const char *dirName, jw *w, char *code, size_t ccap, char *err, size_t errcap) {
  xstrlcpy(code, "bad_request", ccap);
  if (!uid) uid = cx_user_foreground();
  if (cx_user_find(uid, NULL) != 0) { xsnprintf(err, errcap, "unknown user"); return -1; }
  if (!cx_titleid_valid(titleId)) { xsnprintf(err, errcap, "invalid title id"); return -1; }
  if (dirName && *dirName && !cx_dirname_valid(dirName)) { xsnprintf(err, errcap, "invalid save directory name"); return -1; }
  char dn[32] = "";
  if (dirName && *dirName) xstrlcpy(dn, dirName, sizeof dn);
  else {
    char tdir[1024];
    if (cx_save_title_dir(uid, titleId, "", tdir, sizeof tdir, NULL, 0) == 0) {
      char names[8][32];
      if (cx_saves_dirnames(tdir, names, 8) > 0) xstrlcpy(dn, names[0], sizeof dn);
    }
  }
  /* a running (or suspended) game keeps its save mounted, and Garlic writes
   * its working copy back over the image */
  if (cx_title_running(titleId) == 1) {
    xstrlcpy(code, "title_running", ccap);
    xsnprintf(err, errcap, "%s is running: close the game (not just suspend it) before its save is exported", titleId);
    return -1;
  }
  char base[1024], stamp[24], dest[1200];
  cx_saves_backup_dir(base, sizeof base);
  cx_stamp(stamp, sizeof stamp);
  xsnprintf(dest, sizeof dest, "%s/export/%s_%s_%s.zip", base, titleId, dn[0] ? dn : "save", stamp);
  char iv[32] = "";
  cx_installed_version(titleId, iv, sizeof iv, NULL, 0);
  cat_game g;
  int have_g = catalog_match_titleid(titleId, &g) == 0;
  share_export x;
  memset(&x, 0, sizeof x);
  char reason[24] = "", detail[300] = "";
  int ok = 0;
  if (cx_savemount_tier() == CX_TIER_NATIVE) {
    cx_export_result *r = calloc(1, sizeof *r);
    if (!r) { xstrlcpy(code, "oom", ccap); xsnprintf(err, errcap, "out of memory"); return -1; }
    if (cx_savemount_export(uid, titleId, dn, dest, r, err, errcap) != 0) { free(r); xstrlcpy(code, "export_failed", ccap); return -1; }
    if (strcmp(r->format, "xeno-save-zip") == 0) {
      xstrlcpy(x.tier, r->tier, sizeof x.tier);
      xstrlcpy(x.zipPath, r->zipPath, sizeof x.zipPath);
      xstrlcpy(x.sha256, r->sha256, sizeof x.sha256);
      xstrlcpy(x.format, r->format, sizeof x.format);
      x.size = r->size;
      x.files = r->files;
      x.manifest = util_strdup(r->manifest[0] ? r->manifest : "{}");
      ok = 1;
    } else util_unlink_quiet(r->zipPath);   /* a raw copy loads nowhere else: nothing to share */
    free(r);
  }
  if (!ok) {
    char gerr[256] = "";
    if (!dn[0]) { xstrlcpy(reason, "no_save", sizeof reason); xsnprintf(detail, sizeof detail, "No save folder of %s was found on this profile.", titleId); }
    else if (cx_garlic_ensure(1, gerr, sizeof gerr)) {
      int grc = garlic_export(uid, titleId, dn, dest, iv, have_g ? g.region : "", &x, gerr, sizeof gerr);
      if (grc == 0) ok = 1;
      else if (grc == -2) { xstrlcpy(reason, "account_id", sizeof reason); xstrlcpy(detail, gerr, sizeof detail); }
      else { xstrlcpy(code, "export_failed", ccap); xsnprintf(err, errcap, "Garlic Save Manager export failed: %s", gerr); return -1; }
    } else {
      xstrlcpy(reason, "needs_garlic", sizeof reason);
      xsnprintf(detail, sizeof detail, "A shareable (decrypted) export needs Garlic Save Manager or the native save engine: %s", gerr[0] ? gerr : "neither is available");
    }
  }
  if (ok) {
    /* belt and braces: the zip itself must not carry an account id */
    int acct = zip_account_state(x.zipPath);
    if (acct < 0) {
      util_unlink_quiet(x.zipPath);
      free(x.manifest);
      x.manifest = NULL;
      ok = 0;
      xstrlcpy(reason, "account_id", sizeof reason);
      xstrlcpy(detail, "the account ID could not be removed from sce_sys/param.sfo", sizeof detail);
    } else if (!x.accountId[0]) xstrlcpy(x.accountId, acct == 1 ? "removed" : "none", sizeof x.accountId);
  }
  if (ok) {
    jw_kv_str(w, "zipPath", x.zipPath);
    jw_kv_raw(w, "manifest", x.manifest ? x.manifest : "{}", x.manifest ? strlen(x.manifest) : 2);
    jw_kv_str(w, "sha256", x.sha256);
    jw_kv_i64(w, "size", x.size);
    jw_kv_int(w, "files", x.files);
    jw_kv_str(w, "format", x.format);
    jw_kv_str(w, "tier", x.tier);
    jw_kv_bool(w, "accountIdStripped", 1);
    jw_kv_str(w, "accountId", x.accountId);
    jw_kv_bool(w, "shareable", 1);
  } else {
    jw_kv_str(w, "zipPath", "");
    jw_kv_str(w, "format", "");
    jw_kv_str(w, "tier", "raw");
    jw_kv_bool(w, "accountIdStripped", 0);
    jw_kv_bool(w, "shareable", 0);
    jw_kv_str(w, "reason", reason);
    jw_kv_str(w, "detail", detail);
    LOGI("savestore: export of %s/%s is not shareable (%s)", titleId, dn, reason);
  }
  free(x.manifest);
  jw_kv_str(w, "titleId", titleId);
  jw_kv_str(w, "dirName", dn);
  jw_kv_str(w, "gameVersion", iv);
  if (have_g) { jw_kv_str(w, "gameId", g.id); jw_kv_str(w, "title", g.title); jw_kv_str(w, "cover", g.cover); jw_kv_str(w, "region", g.region); }
  return 0;
}

/* Refuses a zip that cannot be shared: raw images, sealed keys, raw backups,
 * same-console manifests and an ACCOUNT_ID left in param.sfo. */
static int share_check(const char *zipPath, char *err, size_t errcap) {
  zip_entry_info *list = calloc(SHARE_MAX_ENTRIES, sizeof *list);
  if (!list) { xsnprintf(err, errcap, "out of memory"); return -1; }
  zip_stats st;
  memset(&st, 0, sizeof st);
  int n = zip_list(zipPath, list, SHARE_MAX_ENTRIES, &st);
  int rc = 0;
  if (n < 0) { xsnprintf(err, errcap, "cannot read the zip: %s", st.error); rc = -1; }
  for (int i = 0; rc == 0 && i < n; i++) {
    const char *b = util_basename(list[i].name);
    if (util_strieq(b, "sealedkey") || util_strieq(b, "keystone")) { xsnprintf(err, errcap, "the zip contains a sealed key (%s) — it cannot be shared", list[i].name); rc = -1; }
    else if (util_istarts_with(b, "sdimg_")) { xsnprintf(err, errcap, "the zip contains a raw save image (%s): only a decrypted export can be shared", list[i].name); rc = -1; }
    else if (util_strieq(b, "xeno-backup.json")) { xsnprintf(err, errcap, "this is a raw console backup: only a decrypted export can be shared"); rc = -1; }
  }
  free(list);
  if (rc != 0) return -1;
  void *mf = NULL;
  size_t mflen = 0;
  if (zip_extract_to_memory(zipPath, "xeno-save.json", &mf, &mflen, 1 << 20) == 0) {
    jdoc *d = json_parse(mf, mflen, 200000, 16);
    free(mf);
    if (d) {
      jv r = json_root(d);
      const char *fmt = jv_get_str(r, "format", "");
      jv same = jv_get(r, "sameConsoleOnly"), acct = jv_get(r, "accountIdStripped");
      if (*fmt && strcmp(fmt, "xeno-save-zip") != 0) { xsnprintf(err, errcap, "the export format is %s: only an xeno-save-zip can be shared", fmt); rc = -1; }
      else if (jv_type(same) == JV_BOOL && jv_bool(same, 0)) { xsnprintf(err, errcap, "this export only loads on this console (raw copy): it cannot be shared"); rc = -1; }
      else if (jv_type(acct) == JV_BOOL && !jv_bool(acct, 1)) { xsnprintf(err, errcap, "the export still carries the account ID"); rc = -1; }
      json_free(d);
    }
  }
  if (rc == 0 && zip_account_state(zipPath) < 0) { xsnprintf(err, errcap, "sce_sys/param.sfo still carries an account ID: export the save again"); rc = -1; }
  return rc;
}

/* ---- upload ------------------------------------------------------------- */

static char *read_whole(const char *path, int64_t max, size_t *len, char *err, size_t errcap) {
  int64_t sz = util_file_size(path);
  if (sz < 0) { xsnprintf(err, errcap, "file not found"); return NULL; }
  if (sz > max) { xsnprintf(err, errcap, "file is larger than the %lld MB limit of this host", (long long)(max >> 20)); return NULL; }
  char *buf = util_read_file(path, len, (size_t)max + 1);
  if (!buf) xsnprintf(err, errcap, "cannot read the file");
  return buf;
}

static int multipart_post(const char *url, const char *field, const char *extra_fields, const char *fname, const char *data, size_t len,
                          char **resp, size_t *rlen, int *status, char *err, size_t errcap) {
  char boundary[48];
  char rnd[20];
  util_rand_hex(rnd, 16);
  xsnprintf(boundary, sizeof boundary, "----XenoStore%s", rnd);
  char head[1024];
  int hl = xsnprintf(head, sizeof head, "%s--%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\nContent-Type: application/zip\r\n\r\n",
                     extra_fields ? extra_fields : "", boundary, field, fname);
  char tail[80];
  int tl = xsnprintf(tail, sizeof tail, "\r\n--%s--\r\n", boundary);
  size_t total = (size_t)hl + len + (size_t)tl;
  char *body = malloc(total);
  if (!body) { xsnprintf(err, errcap, "out of memory"); return -1; }
  memcpy(body, head, (size_t)hl);
  memcpy(body + hl, data, len);
  memcpy(body + hl + len, tail, (size_t)tl);
  char hdr[160];
  xsnprintf(hdr, sizeof hdr, "Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
  cx_http o = {0};
  o.method = "POST";
  o.headers = hdr;
  o.body = body;
  o.body_len = total;
  o.timeout_ms = 600000;
  o.max_bytes = 1 << 20;
  *resp = cx_fetch(url, &o, status, rlen, NULL);
  free(body);
  if (!*resp) { xsnprintf(err, errcap, "upload failed: %s", xhttp_last_error()); return -1; }
  return 0;
}

int cx_savestore_upload(const char *zipPath, const char *host, jw *w, char *code, size_t ccap, char *err, size_t errcap) {
  xstrlcpy(code, "bad_request", ccap);
  if (!cx_saves_zip_allowed(zipPath)) { xstrlcpy(code, "forbidden", ccap); xsnprintf(err, errcap, "zipPath must be an export below <drive>/xeno-store/saves/"); return -1; }
  if (share_check(zipPath, err, errcap) != 0) { xstrlcpy(code, "not_shareable", ccap); return -1; }
  if (!xhttp_available()) { xstrlcpy(code, "downloads_unavailable", ccap); xsnprintf(err, errcap, "network transport unavailable"); return -1; }
  /* the published saves-backend.json decides whether "site" exists, even
   * before anything listed saves on this console (first start, API clients) */
  cx_feed_ensure(CX_FEED_SAVES_BACKEND, 6 * 3600);
  settings_t s;
  settings_get(&s);
  backend b;
  backend_get(&b);
  char h[12];
  int explicit_host = host && *host;
  xstrlcpy(h, explicit_host ? host : s.saves.uploadHost, sizeof h);
  util_lower(h);
  if (strcmp(h, "site") == 0 && !site_on(&b)) {
    if (explicit_host) { xstrlcpy(code, "site_unavailable", ccap); xsnprintf(err, errcap, "the save site is not this console's backend (Settings → Saves → Backend)"); return -1; }
    xstrlcpy(h, "pixeldrain", sizeof h);   /* the settings default, without a site backend */
  }
  const char *fname = util_basename(zipPath);
  char sha[65] = "";
  cx_sha256_file(zipPath, sha);
  int64_t size = util_file_size(zipPath);
  char url[1024] = "", storage[96] = "";
  if (strcmp(h, "site") == 0) {
    /* Into the site's own bucket: ps5-saves/xeno/<yyyymmdd>-<16 hex>.zip, never
     * overwriting (x-upsert: false). The submit step inserts the row that
     * points at it (file_path = storagePath). */
    size_t len = 0;
    char *data = read_whole(zipPath, UPLOAD_MAX, &len, err, errcap);
    if (!data) return -1;
    char day[16], rnd[20], u[700], hdr[1500];
    struct tm tm;
    time_t now = util_now();
    gmtime_r(&now, &tm);
    xsnprintf(day, sizeof day, "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    util_rand_hex(rnd, 16);
    xsnprintf(storage, sizeof storage, "xeno/%s-%s.zip", day, rnd);
    xsnprintf(u, sizeof u, "%s/storage/v1/object/%s/%s", b.url, b.bucket, storage);
    if (supabase_headers(&b, hdr, sizeof hdr, "Content-Type: application/zip\r\nx-upsert: false\r\n") != 0 || !backend_host_ok(&b, u)) {
      free(data); xstrlcpy(code, "upload_failed", ccap); xsnprintf(err, errcap, "backend host not allowed"); return -1;
    }
    cx_http o;
    sb_opt(&o, "POST", hdr, data, len, 600000, 64 << 10);
    int status = 0;
    size_t rl = 0;
    char *resp = cx_fetch(u, &o, &status, &rl, NULL);
    free(data);
    const char *who = b.name[0] ? b.name : "the save site";
    if (!resp) { xstrlcpy(code, "upload_failed", ccap); xsnprintf(err, errcap, "upload to %s failed: %s", who, xhttp_last_error()); return -1; }
    if (status >= 200 && status < 300) xstrlcpy(url, u, sizeof url);
    else {
      jdoc *d = json_parse(resp, rl, 1000, 8);
      xstrlcpy(code, "upload_failed", ccap);
      xsnprintf(err, errcap, "%s refused the upload (HTTP %d%s%.120s)", who, status, d ? ": " : "", d ? jv_get_str(json_root(d), "message", jv_get_str(json_root(d), "error", "")) : "");
      if (d) json_free(d);
      LOGW("savestore: site upload of %s answered HTTP %d", fname, status);
    }
    free(resp);
  } else if (strcmp(h, "pixeldrain") == 0) {
    const char *base = cx_base_override("PIXELDRAIN");
    if (!*base) base = "https://pixeldrain.com";
    size_t len = 0;
    char *data = read_whole(zipPath, UPLOAD_MAX, &len, err, errcap);
    if (!data) return -1;
    char enc[300], u[600];
    cx_url_encode(fname, enc, sizeof enc);
    xsnprintf(u, sizeof u, "%s/api/file/%s", base, enc);
    cx_http o = {0};
    o.method = "PUT";
    o.headers = "Content-Type: application/zip\r\n";
    o.body = data;
    o.body_len = len;
    o.timeout_ms = 600000;
    o.max_bytes = 1 << 20;
    int status = 0;
    size_t rl = 0;
    char *resp = cx_fetch(u, &o, &status, &rl, NULL);
    free(data);
    if (!resp) { xstrlcpy(code, "upload_failed", ccap); xsnprintf(err, errcap, "pixeldrain upload failed: %s", xhttp_last_error()); return -1; }
    jdoc *d = json_parse(resp, rl, 1000, 8);
    const char *id = d ? jv_get_str(json_root(d), "id", "") : "";
    if (status >= 200 && status < 300 && *id) xsnprintf(url, sizeof url, "%s/api/file/%s", base, id);
    else { xstrlcpy(code, "upload_failed", ccap); xsnprintf(err, errcap, "pixeldrain answered HTTP %d%s%s", status, d ? ": " : "", d ? jv_get_str(json_root(d), "message", "") : ""); }
    if (d) json_free(d);
    free(resp);
  } else if (strcmp(h, "catbox") == 0) {
    const char *base = cx_base_override("CATBOX");
    if (!*base) base = "https://catbox.moe";
    size_t len = 0;
    char *data = read_whole(zipPath, CATBOX_MAX, &len, err, errcap);
    if (!data) return -1;
    char u[600];
    xsnprintf(u, sizeof u, "%s/user/api.php", base);
    char *resp = NULL;
    size_t rl = 0;
    int status = 0;
    /* catbox wants reqtype=fileupload before the file part */
    {
      char b2[48], r2[20];
      util_rand_hex(r2, 16);
      xsnprintf(b2, sizeof b2, "----XenoStore%s", r2);
      char head[600];
      int hl = xsnprintf(head, sizeof head, "--%s\r\nContent-Disposition: form-data; name=\"reqtype\"\r\n\r\nfileupload\r\n--%s\r\nContent-Disposition: form-data; name=\"fileToUpload\"; filename=\"%s\"\r\nContent-Type: application/zip\r\n\r\n", b2, b2, fname);
      char tail[80];
      int tl = xsnprintf(tail, sizeof tail, "\r\n--%s--\r\n", b2);
      size_t total = (size_t)hl + len + (size_t)tl;
      char *body = malloc(total);
      if (!body) { free(data); xsnprintf(err, errcap, "out of memory"); return -1; }
      memcpy(body, head, (size_t)hl); memcpy(body + hl, data, len); memcpy(body + hl + len, tail, (size_t)tl);
      free(data);
      char hdr[160];
      xsnprintf(hdr, sizeof hdr, "Content-Type: multipart/form-data; boundary=%s\r\n", b2);
      cx_http o = {0};
      o.method = "POST"; o.headers = hdr; o.body = body; o.body_len = total; o.timeout_ms = 600000; o.max_bytes = 1 << 20;
      resp = cx_fetch(u, &o, &status, &rl, NULL);
      free(body);
    }
    if (!resp) { xstrlcpy(code, "upload_failed", ccap); xsnprintf(err, errcap, "catbox upload failed: %s", xhttp_last_error()); return -1; }
    util_trim(resp);
    if (status >= 200 && status < 300 && util_istarts_with(resp, "http")) xstrlcpy(url, resp, sizeof url);
    else { xstrlcpy(code, "upload_failed", ccap); xsnprintf(err, errcap, "catbox answered HTTP %d: %.80s", status, resp); }
    free(resp);
  } else if (strcmp(h, "custom") == 0) {
    if (!util_istarts_with(s.saves.uploadEndpoint, "http")) { xsnprintf(err, errcap, "set Settings → Saves → Upload endpoint first"); return -1; }
    size_t len = 0;
    char *data = read_whole(zipPath, UPLOAD_MAX, &len, err, errcap);
    if (!data) return -1;
    char *resp = NULL;
    size_t rl = 0;
    int status = 0;
    int rc = multipart_post(s.saves.uploadEndpoint, "file", "", fname, data, len, &resp, &rl, &status, err, errcap);
    free(data);
    if (rc != 0) { xstrlcpy(code, "upload_failed", ccap); return -1; }
    util_trim(resp);
    jdoc *d = json_parse(resp, rl, 1000, 8);
    if (d) { xstrlcpy(url, jv_get_str(json_root(d), "url", ""), sizeof url); json_free(d); }
    else if (util_istarts_with(resp, "http")) xstrlcpy(url, resp, sizeof url);
    if (status < 200 || status >= 300 || !url[0]) { xstrlcpy(code, "upload_failed", ccap); xsnprintf(err, errcap, "upload endpoint answered HTTP %d without a url", status); }
    free(resp);
  } else { xsnprintf(err, errcap, "host must be site, pixeldrain, catbox or custom"); return -1; }
  if (!url[0]) return -1;
  jw_kv_str(w, "url", url);
  if (storage[0]) jw_kv_str(w, "storagePath", storage);
  jw_kv_str(w, "sha256", sha);
  jw_kv_i64(w, "size", size);
  jw_kv_str(w, "host", h);
  jw_kv_str(w, "name", fname);
  LOGI("savestore: uploaded %s to %s", fname, h);
  return 0;
}

/* ---- submit ------------------------------------------------------------- */

#define SITE_NOTES_MAX 300     /* characters: the site's practical length (template CHECK) */
#define SITE_GAME_MAX 120
#define SITE_SHARED_BY_MAX 60

/* Appends up to max_cp code points of src to dst (cap bytes), cutting on a
 * word boundary with "…" when src is longer. Returns the code points added. */
static size_t utf8_len(const char *t) { size_t n = 0; for (; *t; t++) if ((*t & 0xC0) != 0x80) n++; return n; }

static void append_cut(char *dst, size_t cap, const char *src, size_t max_cp) {
  if (!max_cp) return;
  size_t cut = utf8_cut(src, max_cp);
  if (!src[cut]) { xstrlcat(dst, src, cap); return; }
  if (max_cp < 2) return;
  cut = utf8_cut(src, max_cp - 1);                      /* room for "…" */
  size_t sp = cut, min = utf8_cut(src, (max_cp - 1) / 2);
  while (sp > min && src[sp] != ' ') sp--;
  if (sp > min) cut = sp;
  size_t dl = strlen(dst);
  if (dl + cut + 4 > cap) return;
  memcpy(dst + dl, src, cut);
  dst[dl + cut] = 0;
  util_trim(dst);
  xstrlcat(dst, "\xE2\x80\xA6", cap);
}

/* Drops an incomplete UTF-8 sequence left at the end by a byte-wise cut. */
static void utf8_fix_tail(char *s) {
  size_t n = strlen(s), i = n;
  while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
  if (i == 0) { s[0] = 0; return; }
  unsigned char lead = (unsigned char)s[i - 1];
  size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
  if (n - (i - 1) < need) s[i - 1] = 0;
}

/* Plain one-line text: control characters become spaces, runs collapse. */
static void one_line(const char *in, char *out, size_t cap) {
  size_t j = 0;
  int sp = 0;
  for (const unsigned char *p = (const unsigned char *)in; *p && j + 1 < cap; p++) {
    if (*p <= 0x20 || *p == 0x7f) { if (j && !sp) { out[j++] = ' '; sp = 1; } continue; }
    out[j++] = (char)*p;
    sp = 0;
  }
  out[j] = 0;
  utf8_fix_tail(out);
  util_trim(out);
}

/* The site's notes: "<name> — <description>" within SITE_NOTES_MAX
 * characters, then the share's tags as "#tag" words when they still fit. */
static void site_notes(jv meta, char *out, size_t cap) {
  char name[400], desc[1400];
  one_line(jv_get_str(meta, "name", ""), name, sizeof name);
  /* the description keeps its line breaks as spaces (the site shows one paragraph) */
  one_line(jv_get_str(meta, "description", ""), desc, sizeof desc);
  out[0] = 0;
  append_cut(out, cap, name, 80);
  if (desc[0]) {
    if (out[0]) xstrlcat(out, " \xE2\x80\x94 ", cap);                /* " — " */
    size_t used = utf8_len(out);
    if (used + 1 < SITE_NOTES_MAX) append_cut(out, cap, desc, SITE_NOTES_MAX - used);
  }
  jv tags = jv_get(meta, "tags");
  if (jv_type(tags) != JV_ARR) return;
  int ntags = 0;
  JV_FOREACH(t, tags) {
    if (ntags >= 10) break;
    char tag[64], word[72] = "#";
    one_line(jv_str(t, ""), tag, sizeof tag);
    size_t j = 1;
    for (const char *p = tag; *p && j + 1 < 26; p++) {
      if (*p == '#' || *p == ',') continue;
      word[j++] = *p == ' ' ? '-' : *p;
    }
    word[j] = 0;
    utf8_fix_tail(word);
    if (strlen(word) < 2 || strstr(out, word)) continue;
    if (utf8_len(out) + 1 + utf8_len(word) > SITE_NOTES_MAX || strlen(out) + strlen(word) + 2 > cap) break;
    if (out[0]) xstrlcat(out, " ", cap);
    xstrlcat(out, word, cap);
    ntags++;
  }
}

/* "<TITLEID>_<title with spaces as _>.zip", ASCII only. */
static void site_file_name(const char *tid, const char *title, char *out, size_t cap) {
  char t[100];
  size_t j = 0;
  for (const unsigned char *p = (const unsigned char *)title; *p && j + 1 < sizeof t; p++) {
    char c = (char)*p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.') t[j++] = c;
    else if ((c == ' ' || c == '_') && j && t[j - 1] != '_') t[j++] = '_';
  }
  while (j && (t[j - 1] == '_' || t[j - 1] == '.' || t[j - 1] == '-')) j--;
  t[j] = 0;
  if (t[0]) xsnprintf(out, cap, "%s_%s.zip", tid, t); else xsnprintf(out, cap, "%s.zip", tid);
}

/* The object path the site upload produced: xeno/<8 digits>-<16 hex>.zip. */
static int site_storage_path_ok(const char *p) {
  if (strlen(p) != strlen("xeno/20261015-0123456789abcdef.zip") || strncmp(p, "xeno/", 5) != 0 || strcmp(p + 30, ".zip") != 0 || p[13] != '-') return 0;
  for (int i = 5; i < 13; i++) if (p[i] < '0' || p[i] > '9') return 0;
  for (int i = 14; i < 30; i++) if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return 0;
  return 1;
}

/* Inserts the share into public.ps5_save_collection: only the site's columns
 * (a template backend marked "moderated" also gets status/source). Returns 0
 * with the new row id, or -1 with why. */
static int site_insert(const backend *b, jv meta, const char *tid, const char *nick, char *id_out, size_t idcap, char *why, size_t whycap) {
  const char *sp = jv_get_str(meta, "storagePath", "");
  const char *who = b->name[0] ? b->name : "the save site";
  if (!site_storage_path_ok(sp)) { xsnprintf(why, whycap, "the zip was not uploaded to %s (upload host \"site\"), so it cannot be added to its collection", who); return -1; }
  char game[SITE_GAME_MAX * 4 + 8] = "", shared[SITE_SHARED_BY_MAX * 4 + 8] = "", line[600], notes[SITE_NOTES_MAX * 4 + 16], fname[160];
  one_line(jv_get_str(meta, "title", ""), line, sizeof line);
  append_cut(game, sizeof game, line[0] ? line : tid, SITE_GAME_MAX);
  one_line(nick, line, sizeof line);
  append_cut(shared, sizeof shared, line[0] ? line : "anonymous", SITE_SHARED_BY_MAX);
  site_notes(meta, notes, sizeof notes);
  site_file_name(tid, game, fname, sizeof fname);
  int64_t size = jv_get_i64(meta, "size", 0);
  jw rw;
  jw_init(&rw);
  jw_obj_begin(&rw);
  jw_kv_str(&rw, "title_id", tid);
  jw_kv_str(&rw, "game", game);
  jw_kv_str(&rw, "shared_by", shared);
  if (notes[0]) jw_kv_str(&rw, "notes", notes); else { jw_key(&rw, "notes"); jw_null(&rw); }
  jw_kv_str(&rw, "file_path", sp);
  jw_kv_str(&rw, "file_name", fname);
  if (size > 0) jw_kv_i64(&rw, "file_size", size); else { jw_key(&rw, "file_size"); jw_null(&rw); }
  if (b->moderated) { jw_kv_str(&rw, "status", "pending"); jw_kv_str(&rw, "source", "xeno"); }
  jw_obj_end(&rw);
  size_t rl;
  char *row = jw_detach(&rw, &rl);
  if (!row) { xsnprintf(why, whycap, "out of memory"); return -1; }
  char u[600], hdr[1500];
  xsnprintf(u, sizeof u, "%s/rest/v1/ps5_save_collection", b->url);
  if (supabase_headers(b, hdr, sizeof hdr, "Content-Type: application/json\r\nAccept: application/json\r\nPrefer: return=representation\r\n") != 0 || !backend_host_ok(b, u)) {
    free(row); xsnprintf(why, whycap, "backend host not allowed"); return -1;
  }
  cx_http o;
  sb_opt(&o, "POST", hdr, row, rl, 30000, 256 << 10);
  int status = 0;
  size_t len = 0;
  char *resp = cx_fetch(u, &o, &status, &len, NULL);
  free(row);
  int rc = -1;
  if (resp && status >= 200 && status < 300) {
    rc = 0;   /* inserted; the id comes back with return=representation */
    jdoc *d = json_parse(resp, len, 2000, 8);
    if (d) {
      jv r = json_root(d);
      jv first = jv_type(r) == JV_ARR ? jv_at(r, 0) : r;
      const char *rid = jv_get_str(first, "id", "");
      if (row_uuid_ok(rid)) xstrlcpy(id_out, rid, idcap);
      json_free(d);
    }
  } else if (resp) {
    jdoc *d = json_parse(resp, len, 1000, 8);
    xsnprintf(why, whycap, "%s refused the save (HTTP %d%s%.160s)", who, status, d ? ": " : "", d ? jv_get_str(json_root(d), "message", jv_get_str(json_root(d), "error", "")) : "");
    if (d) json_free(d);
  } else xsnprintf(why, whycap, "%s did not answer: %s", who, xhttp_last_error());
  free(resp);
  return rc;
}

/* The canonical submission: what the intake Action parses from the issue, and
 * what a custom submit endpoint receives. Caller frees. */
static char *submit_block(jv meta, const char *tid, const char *category, const char *nick, const char *url, const char *sha, int64_t size, size_t *bl) {
  jw bw;
  jw_init(&bw);
  jw_obj_begin(&bw);
  jw_kv_str(&bw, "titleId", tid);
  jw_kv_str(&bw, "gameId", jv_get_str(meta, "gameId", ""));
  jw_kv_str(&bw, "title", jv_get_str(meta, "title", ""));
  jw_kv_str(&bw, "category", category);
  jw_kv_str(&bw, "name", jv_get_str(meta, "name", ""));
  jw_kv_str(&bw, "description", jv_get_str(meta, "description", ""));
  jw_kv_str(&bw, "author", nick);
  jw_kv_str(&bw, "gameVersion", jv_get_str(meta, "gameVersion", ""));
  jw_kv_str(&bw, "region", jv_get_str(meta, "region", ""));
  jw_kv_str(&bw, "dirName", jv_get_str(meta, "dirName", ""));
  jw_kv_i64(&bw, "size", size);
  jw_kv_str(&bw, "sha256", sha);
  jw_kv_str(&bw, "url", url);
  jw_kv_str(&bw, "format", "xeno-save-zip");
  jw_kv_str(&bw, "kind", "ps5");
  jw_key(&bw, "tags");
  if (jv_type(jv_get(meta, "tags")) == JV_ARR) jw_value(&bw, jv_get(meta, "tags")); else { jw_arr_begin(&bw); jw_arr_end(&bw); }
  jw_obj_end(&bw);
  return jw_detach(&bw, bl);
}

/* A copy anyone can download, for the GitHub issue (or a custom endpoint),
 * when the zip is only in the site's private bucket: that object opens with
 * the anon key only, so an issue carrying its URL could never pass intake and
 * a published index row could not be downloaded. The same export goes up
 * again to a public host — the Settings upload host, pixeldrain when that is
 * "site" — through cx_savestore_upload, so its zip checks (an export below
 * <drive>/xeno-store/saves/, share_check) run again. */
static int public_copy(jv meta, const settings_t *s, char *url, size_t ucap, char *sha, size_t shacap, int64_t *size, char *host, size_t hcap,
                       char *why, size_t whycap) {
  const char *zp = jv_get_str(meta, "zipPath", "");
  if (!zp[0]) { xsnprintf(why, whycap, "the request names no zipPath to upload a public copy from"); return -1; }
  const char *h = "pixeldrain";
  if (strcmp(s->saves.uploadHost, "catbox") == 0) h = "catbox";
  else if (strcmp(s->saves.uploadHost, "custom") == 0 && util_istarts_with(s->saves.uploadEndpoint, "http")) h = "custom";
  jw uw;
  jw_init(&uw);
  jw_obj_begin(&uw);
  char code[32] = "", err[256] = "";
  int rc = cx_savestore_upload(zp, h, &uw, code, sizeof code, err, sizeof err);
  jw_obj_end(&uw);
  size_t l = 0;
  char *t = jw_detach(&uw, &l);
  jdoc *d = rc == 0 && t ? json_parse(t, l, 1000, 8) : NULL;
  free(t);
  if (!d) { xsnprintf(why, whycap, "%s", err[0] ? err : "the upload returned no link"); return -1; }
  jv r = json_root(d);
  xstrlcpy(url, jv_get_str(r, "url", ""), ucap);
  const char *hs = jv_get_str(r, "sha256", "");
  if (hs[0]) xstrlcpy(sha, hs, shacap);
  *size = jv_get_i64(r, "size", *size);
  xstrlcpy(host, jv_get_str(r, "host", h), hcap);
  json_free(d);
  if (!util_istarts_with(url, "http")) { xsnprintf(why, whycap, "the upload returned no link"); return -1; }
  return 0;
}

int cx_savestore_submit(jv meta, jw *w, char *code, size_t ccap, char *err, size_t errcap) {
  xstrlcpy(code, "bad_request", ccap);
  if (jv_type(meta) != JV_OBJ) { xsnprintf(err, errcap, "JSON object required"); return -1; }
  const char *tid = jv_get_str(meta, "titleId", "");
  const char *url = jv_get_str(meta, "url", "");
  const char *desc = jv_get_str(meta, "description", "");
  if (!cx_titleid_valid(tid) || tid[0] != 'P') { xsnprintf(err, errcap, "titleId must be a PS5 id (PPSA…)"); return -1; }
  if (!util_istarts_with(url, "http")) { xsnprintf(err, errcap, "url must be the uploaded zip link"); return -1; }
  if (strlen(desc) > 300 * 4 || utf8_len(desc) > 300) { xsnprintf(err, errcap, "description must be 300 characters or less"); return -1; }
  settings_t s;
  settings_get(&s);
  const char *nick = jv_get_str(meta, "author", jv_get_str(meta, "nickname", ""));
  if (!*nick) nick = s.saves.nickname;
  const char *category = infer_category(jv_get_str(meta, "category", ""), desc);
  const char *method = "github", *status = "pending";
  char inserted_id[80] = "", site_err[300] = "";
  /* the backend this share goes to: the published file counts even before
   * anything listed saves on this console */
  cx_feed_ensure(CX_FEED_SAVES_BACKEND, 6 * 3600);
  backend b;
  backend_get(&b);
  backend_traits(&b);
  if (site_on(&b) && xhttp_available()) {
    /* the save site: one insert of the site's own columns, live at once */
    if (site_insert(&b, meta, tid, nick, inserted_id, sizeof inserted_id, site_err, sizeof site_err) == 0) {
      method = "site";
      status = b.moderated ? "pending" : "published";
      LOGI("savestore: %s added to %s (%s)", tid, b.name, inserted_id[0] ? inserted_id : "no id returned");
      /* rebuild the index now (bounded) so the new row is in the store's next listing */
      kick_build(1);
      wait_build(LIST_WAIT_MS);
    } else LOGW("savestore: site insert failed: %s — falling back to the GitHub issue", site_err);
  }
  if (strcmp(method, "site") == 0) {
    size_t bl;
    char *block = submit_block(meta, tid, category, nick, url, jv_get_str(meta, "sha256", ""), jv_get_i64(meta, "size", 0), &bl);
    if (!block) { xstrlcpy(code, "oom", ccap); xsnprintf(err, errcap, "out of memory"); return -1; }
    jw_kv_str(w, "method", method);
    jw_kv_str(w, "status", status);
    jw_kv_str(w, "backend", b.name);
    jw_kv_str(w, "backendName", b.name);
    if (inserted_id[0]) {
      char sid[96];
      xsnprintf(sid, sizeof sid, "sb:%s", inserted_id);
      jw_kv_str(w, "id", inserted_id);
      jw_kv_str(w, "storeId", sid);
    }
    jw_kv_str(w, "storagePath", jv_get_str(meta, "storagePath", ""));
    jw_kv_raw(w, "submission", block, bl);
    char note[200];
    if (b.moderated) xsnprintf(note, sizeof note, "Shared — the save waits for review on %s.", b.name);
    else xsnprintf(note, sizeof note, "Shared — the save is live on %s and in the store.", b.name);
    jw_kv_str(w, "note", note);
    free(block);
    return 0;
  }
  /* Not on the site. The GitHub issue (or a custom endpoint) needs a link
   * anyone can open, and an object of the site's private bucket is not one. */
  char pub[1024], sha[65], pub_host[16] = "";
  int64_t size = jv_get_i64(meta, "size", 0);
  xstrlcpy(pub, url, sizeof pub);
  xstrlcpy(sha, jv_get_str(meta, "sha256", ""), sizeof sha);
  if (site_on(&b) && backend_host_ok(&b, url)) {
    char why[256] = "";
    if (public_copy(meta, &s, pub, sizeof pub, sha, sizeof sha, &size, pub_host, sizeof pub_host, why, sizeof why) != 0) {
      xstrlcpy(code, "site_insert_failed", ccap);
      xsnprintf(err, errcap, "%s; the zip is only in %s's private bucket and a public copy for the GitHub issue failed (%s) — try again",
                site_err[0] ? site_err : "the save was not added", b.name, why);
      LOGW("savestore: %s", err);
      return -1;
    }
    LOGI("savestore: public copy on %s for the GitHub issue: %s", pub_host, pub);
  }
  size_t bl;
  char *block = submit_block(meta, tid, category, nick, pub, sha, size, &bl);
  if (!block) { xstrlcpy(code, "oom", ccap); xsnprintf(err, errcap, "out of memory"); return -1; }
  /* the user's own submit endpoint (Settings → Saves), whenever the site did not take the save */
  if (util_istarts_with(s.saves.submitEndpoint, "http") && xhttp_available()) {
    cx_http o = {0};
    o.method = "POST"; o.headers = "Content-Type: application/json\r\n"; o.body = block; o.body_len = bl; o.timeout_ms = 30000;
    int st = 0;
    size_t len = 0;
    char *resp = cx_fetch(s.saves.submitEndpoint, &o, &st, &len, NULL);
    if (resp && st >= 200 && st < 300) method = "custom";
    else LOGW("savestore: custom submit failed (HTTP %d) — falling back to the GitHub issue", st);
    free(resp);
  }
  jw_kv_str(w, "method", method);
  jw_kv_str(w, "status", status);
  if (site_err[0]) jw_kv_str(w, "siteError", site_err);
  if (pub_host[0]) { jw_kv_str(w, "publicUrl", pub); jw_kv_str(w, "publicHost", pub_host); }
  if (strcmp(method, "custom") == 0) {
    jw_kv_raw(w, "submission", block, bl);
    jw_kv_str(w, "note", "Submitted — the save appears in the store once it is approved.");
    free(block);
    return 0;
  }
  /* GitHub issue hand-off (no backend, or the backend refused) */
  char title[300], enc_title[900];
  xsnprintf(title, sizeof title, "[save] %s — %s", jv_get_str(meta, "title", tid), jv_get_str(meta, "name", category));
  cx_url_encode(title, enc_title, sizeof enc_title);
  char body[2400];
  xsnprintf(body, sizeof body, "<!-- Submitted from XENO STORE %s. Keep the JSON block below unchanged. -->\n\n```json\n%s\n```\n", XENO_VERSION, block);
  size_t ecap = strlen(body) * 3 + 1;
  char *enc_body = malloc(ecap);
  if (!enc_body) { free(block); xstrlcpy(code, "oom", ccap); xsnprintf(err, errcap, "out of memory"); return -1; }
  cx_url_encode(body, enc_body, ecap);
  size_t icap = strlen(enc_body) + 1200;
  char *issue = malloc(icap);
  if (!issue) { free(block); free(enc_body); xstrlcpy(code, "oom", ccap); xsnprintf(err, errcap, "out of memory"); return -1; }
  xsnprintf(issue, icap, "https://github.com/XENOKING123/XENO-STORE/issues/new?template=save-submission.md&labels=save-submission&title=%s&body=%s", enc_title, enc_body);
  free(enc_body);
  jw_kv_str(w, "issueUrl", issue);
  jw_kv_str(w, "qr", issue);
  jw_kv_raw(w, "submission", block, bl);
  jw_kv_str(w, "note", "Open the link (or scan the QR) on your phone and press Submit new issue — the save is reviewed before it appears in the store.");
  free(block);
  free(issue);
  return 0;
}

/* ---- Garlic Save Manager routes ----------------------------------------- */

static pthread_mutex_t garlic_mu = PTHREAD_MUTEX_INITIALIZER;
static char garlic_job[16];

/* GET /api/saves/garlic → {"ok":true,"garlic":{…},"engine":"…"} */
static void h_garlic_status(api_ctx *c) {
  jw w;
  api_begin(&w);
  cx_garlic_status_json(&w);
  jw_kv_str(&w, "engine", cx_tier_name(cx_savemount_tier()));
  api_send(c, &w);
}

static int garlic_start_worker(cx_job *j, void *p) {
  (void)p;
  cx_job_progress(j, 0, 0, "Starting Garlic Save Manager");
  char err[256] = "";
  if (!cx_garlic_ensure(1, err, sizeof err)) {
    cx_job_fail(j, "garlic_unavailable", err[0] ? err : "Garlic Save Manager did not start");
    return -1;
  }
  jw rw;
  jw_init(&rw);
  jw_obj_begin(&rw);
  cx_garlic_status_json(&rw);
  jw_obj_end(&rw);
  cx_job_set_result(j, &rw);
  cx_job_progress(j, 1, 1, "Garlic Save Manager is running");
  return 0;
}

/* POST /api/saves/garlic/start {"wait":false} → {"ok":true,"job":{type:"garlic-start",…}}:
 * finds or downloads Garlic, sends it to the ELF loader and waits for it to
 * answer, on a job thread (one at a time; a second call returns the same job). */
static void h_garlic_start(api_ctx *c) {
  char id[16] = "";
  pthread_mutex_lock(&garlic_mu);
  if (garlic_job[0] && cx_jobs_active("garlic-start") > 0) xstrlcpy(id, garlic_job, sizeof id);
  else if (cx_job_start("garlic-start", "Garlic Save Manager", garlic_start_worker, NULL, id, sizeof id) == 0) xstrlcpy(garlic_job, id, sizeof garlic_job);
  pthread_mutex_unlock(&garlic_mu);
  if (!id[0]) { api_error(c, 429, "busy", "too many operations running"); return; }
  if (jv_get_bool(c->body, "wait", 0)) cx_job_wait(id, 60000);
  jw w;
  api_begin(&w);
  jw_key(&w, "job");
  cx_job_json(&w, id);
  api_send(c, &w);
}

static const api_route GARLIC_ROUTES[] = {
  { "GET", "/api/saves/garlic", h_garlic_status },
  { "POST", "/api/saves/garlic/start", h_garlic_start },
};

static void register_routes(void) {
  if (api_register_routes(GARLIC_ROUTES, XS_ARRAY_LEN(GARLIC_ROUTES)) != 0) LOGE("savestore: Garlic routes not registered");
}
