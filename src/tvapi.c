/*
 * tvapi.c — the /api/v1 compatibility layer for the native TV app: the single
 * route table, tvapi_init(), the lazy start of the TV services, the request
 * gate, session helpers and reply helpers. See tvapi.h.
 *
 * Copyright (C) 2026 XENO STORE contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The gate order, the stopping exemptions and the error sentences follow the
 * reference backend's request gate. Ported from Orbit Store backend/api.c
 * (GPL-3.0-or-later), with our own sentences where the spec rebrands them.
 */
#include "tvapi.h"

#include "config.h"
#include "log.h"
#include "pairing.h"
#include "util.h"
#include "xthread.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TV_CLIENT_SEEN_WINDOW_MS 10000

/* tvapi.c's own state, under tv_mu (a leaf lock). */
static pthread_mutex_t tv_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_instance[33];
static int g_pref_auto;                  /* <data>/tvapi.json "preferredAuto" */
static volatile int g_drain_active;      /* the TV stop drain is running */
static volatile int64_t g_client_seen;   /* util_now_ms() of the last loopback GET /system */
static volatile int g_started;           /* the lazy start ran */
static volatile int64_t g_started_ms;
static volatile int g_threads;           /* threads started through tv_thread_start() */
static pthread_once_t g_start_once = PTHREAD_ONCE_INIT;

/* ---- instance id, client seen, drain, tvapi.json -------------------------- */

const char *tv_instance_id(void) { return g_instance; }

int64_t tv_client_seen_ms(void) { return g_client_seen; }

int tvapi_client_seen(void) {
  int64_t t = g_client_seen;
  return t != 0 && util_now_ms() - t <= TV_CLIENT_SEEN_WINDOW_MS;
}

int tv_drain_active(void) { return g_drain_active; }

void tv_drain_set_active(int on) {
  g_drain_active = on ? 1 : 0;
  LOGI("tvapi: stop drain %s", on ? "started" : "cleared");
}

static void tvapi_json_path(char *out, size_t cap) { xsnprintf(out, cap, "%s/tvapi.json", g_cfg.data_dir); }

static void load_state(void) {
  char path[600];
  tvapi_json_path(path, sizeof path);
  size_t len = 0;
  char *txt = util_read_file(path, &len, 64u << 10);
  if (!txt) return;   /* absent on first use: defaults */
  jdoc *d = json_parse(txt, len, 1000, 8);
  free(txt);
  if (!d) { LOGW("tvapi: %s is not valid JSON, using defaults", path); return; }
  jv auto_flag = jv_get(json_root(d), "preferredAuto");
  pthread_mutex_lock(&tv_mu);
  g_pref_auto = jv_type(auto_flag) == JV_BOOL && jv_bool(auto_flag, 0);
  pthread_mutex_unlock(&tv_mu);
  json_free(d);
}

int tv_pref_auto(void) {
  pthread_mutex_lock(&tv_mu);
  int v = g_pref_auto;
  pthread_mutex_unlock(&tv_mu);
  return v;
}

int tv_pref_auto_set(int on) {
  /* tv_mu is held across the small write so two setters cannot reorder the file */
  pthread_mutex_lock(&tv_mu);
  g_pref_auto = on ? 1 : 0;
  jw w;
  jw_init(&w);
  jw_obj_begin(&w);
  jw_kv_bool(&w, "preferredAuto", g_pref_auto);
  jw_obj_end(&w);
  size_t len = 0;
  char *txt = jw_detach(&w, &len);
  char path[600];
  tvapi_json_path(path, sizeof path);
  int rc = txt ? util_write_file_atomic(path, txt, len) : -1;
  pthread_mutex_unlock(&tv_mu);
  free(txt);
  if (rc != 0) LOGW("tvapi: cannot write %s (the automatic drive choice lasts until restart)", path);
  return rc == 0 ? 0 : -1;
}

/* ---- threads and the lazy start (R13) -------------------------------------- */

int tv_thread_start(pthread_t *out, const char *name, void *(*fn)(void *), void *arg, size_t stack, int detached) {
  int rc = xthread_start(out, name, fn, arg, stack, detached);
  if (rc == 0) {
    __atomic_add_fetch(&g_threads, 1, __ATOMIC_SEQ_CST);
    LOGI("tvapi: thread %s started", name);
  } else {
    LOGW("tvapi: thread %s could not start (%d)", name, rc);
  }
  return rc;
}

static void tv_start(void) {
  if (g_cfg.stop_requested) return;   /* going away: nothing to start */
  LOGI("tvapi: first /api/v1 request, starting the TV app services");
  g_started_ms = util_now_ms();
  tvdl_storage_start();   /* first: the others read the drive snapshot */
  tvsys_start();
  tvcat_start();
  tvart_start();
  tvdl_start();
  tvlib_start();
  tvup_start();
  g_started = 1;
}

/* ---- gate ----------------------------------------------------------------- */

int tv_authed(const http_req *req) {
  if (!util_istarts_with(req->authorization, "Bearer ")) return 0;
  const char *tok = req->authorization + 7;
  while (*tok == ' ') tok++;
  return pairing_token_valid(tok);
}

int tv_stopping(void) {
  if (g_cfg.stop_requested || g_drain_active) return 1;
#ifdef XENO_HOST
  /* Host test hook: the stopping state on demand, without stopping anything. */
  const char *f = getenv("XENO_TVAPI_STOPPING_FILE");
  if (f && *f) { struct stat st; if (stat(f, &st) == 0) return 1; }
#endif
  return 0;
}

int tv_is_post(const api_ctx *c) { return strcmp(c->req->method, "POST") == 0; }

/* Content-Type is application/json, with or without parameters. */
static int content_type_json(const char *ct) {
  if (!util_istarts_with(ct, "application/json")) return 0;
  char next = ct[16];
  return next == 0 || next == ';' || next == ' ' || next == '\t';
}

int tv_gate(api_ctx *c, unsigned flags) {
  pthread_once(&g_start_once, tv_start);
  if ((flags & TV_S) && tv_stopping()) { tv_error(c, 503, TV_MSG_STOPPING); return 0; }
  if ((flags & TV_B) && !tv_authed(c->req)) { tv_error(c, 401, TV_MSG_UNPAIRED); return 0; }
  if ((flags & TV_O) && tv_is_post(c)) {
    /* The app always sends Content-Type: application/json; a missing header is
     * tolerated (the body check below still applies), a different one is not. */
    if (c->req->content_type[0] && !content_type_json(c->req->content_type)) { tv_error(c, 415, TV_MSG_USE_JSON); return 0; }
    if (!c->doc || jv_type(c->body) != JV_OBJ) { tv_error(c, 400, TV_MSG_NOT_OBJECT); return 0; }
  }
  return 1;
}

/* ---- replies -------------------------------------------------------------- */

void tv_error(api_ctx *c, int status, const char *sentence) { http_send_sentence(c->req, status, sentence); }

void tv_not_ready(api_ctx *c) { tv_error(c, 503, TV_MSG_NOT_READY); }

void tv_reply(api_ctx *c, int status, jw *w) {
  size_t len = 0;
  char *txt = jw_detach(w, &len);
  if (!txt) { tv_error(c, 503, TV_MSG_LOW_MEMORY); return; }
  http_send(c->req, status, "application/json; charset=utf-8", txt, len, "Cache-Control: no-store\r\n");
  free(txt);
}

void tv_obj_begin(jw *w) { jw_init(w); jw_obj_begin(w); }
void tv_obj_send(api_ctx *c, int status, jw *w) { jw_obj_end(w); tv_reply(c, status, w); }
void tv_arr_begin(jw *w) { jw_init(w); jw_arr_begin(w); }
void tv_arr_send(api_ctx *c, int status, jw *w) { jw_arr_end(w); tv_reply(c, status, w); }

void tv_send_bytes(api_ctx *c, int status, const char *ctype, const void *data, size_t len) {
  http_send(c->req, status, ctype, data, len, "Cache-Control: no-store\r\n");
}

/* ---- strict body readers -------------------------------------------------- */

static jv body_member(const api_ctx *c, const char *key) {
  jv none = { NULL, 0xFFFFFFFFu };
  if (!c->doc || jv_type(c->body) != JV_OBJ) return none;
  return jv_get(c->body, key);
}

int tv_body_type(const api_ctx *c, const char *key) {
  jv v = body_member(c, key);
  return jv_ok(v) ? jv_type(v) : JV_INVALID;
}

int tv_body_has(const api_ctx *c, const char *key) { return tv_body_type(c, key) != JV_INVALID; }

int tv_body_str(const api_ctx *c, const char *key, const char **out) {
  jv v = body_member(c, key);
  if (!jv_ok(v) || jv_type(v) != JV_STR) return 0;
  if (out) *out = jv_str(v, "");
  return 1;
}

int tv_body_bool(const api_ctx *c, const char *key, int *out) {
  jv v = body_member(c, key);
  if (!jv_ok(v) || jv_type(v) != JV_BOOL) return 0;
  if (out) *out = jv_bool(v, 0);
  return 1;
}

int tv_body_int(const api_ctx *c, const char *key, int64_t *out) {
  jv v = body_member(c, key);
  if (!jv_ok(v) || jv_type(v) != JV_NUM) return 0;
  double d = jv_dbl(v, 0.5);
  if (!(d >= -9007199254740992.0 && d <= 9007199254740992.0)) return 0;   /* also rejects NaN */
  int64_t i = (int64_t)d;
  if ((double)i != d) return 0;
  if (out) *out = i;
  return 1;
}

/* ---- the route table (PORT-SPEC 1.4) ---------------------------------------
 * One wrapper per method + pattern: the gate with that row's flags, then the
 * handler. The table is registered once; API_EXT_MAX is 8 and the core uses
 * two slots, so every /api/v1 route lives in this one table. */

#define TV_ROUTE(name, flags, handler) \
  static void name(api_ctx *c) { if (tv_gate(c, (flags))) handler(c); }

static void system_get(api_ctx *c) {
  if (!tv_gate(c, 0)) return;
  /* E8: main.c skips the browser while the TV app (a loopback caller) polls */
  if (c->req->client_is_loopback) g_client_seen = util_now_ms();
  tvsys_h_system(c);
}
TV_ROUTE(session_get, TV_S, tvsys_h_session)
TV_ROUTE(zero_any, 0, tvsys_h_zero)
TV_ROUTE(sources_get, TV_S, tvcat_h_sources)
TV_ROUTE(sources_post, TV_S | TV_B | TV_O, tvcat_h_sources)
TV_ROUTE(catalog_get, TV_S, tvcat_h_catalog)
TV_ROUTE(catalog_updates_get, TV_S, tvcat_h_updates)
TV_ROUTE(catalog_updates_post, TV_S | TV_B | TV_O, tvcat_h_updates)
TV_ROUTE(art_get, TV_S | TV_B, tvart_h_art)
TV_ROUTE(favorites_get, TV_S | TV_B, tvcat_h_favorites)
TV_ROUTE(favorites_post, TV_S | TV_B | TV_O, tvcat_h_favorites)
TV_ROUTE(downloads_get, TV_S | TV_B, tvdl_h_jobs)
TV_ROUTE(downloads_post, TV_S | TV_B | TV_O, tvdl_h_create)
TV_ROUTE(download_action_post, TV_S | TV_B | TV_O, tvdl_h_action)
TV_ROUTE(storage_get, TV_S | TV_B, tvdl_h_storage)
TV_ROUTE(storage_preferred_post, TV_S | TV_B | TV_O, tvdl_h_preferred)
TV_ROUTE(settings_downloads_post, TV_S | TV_B | TV_O, tvdl_h_limit)
TV_ROUTE(settings_extraction_post, TV_S | TV_B | TV_O, tvdl_h_extraction)
TV_ROUTE(debrid_get, TV_S | TV_B, tvdl_h_debrid)
TV_ROUTE(browser_get, TV_S | TV_B, tvdl_h_browser)
TV_ROUTE(browser_post, TV_S | TV_B | TV_O, tvdl_h_browser)
TV_ROUTE(library_get, TV_S | TV_B, tvlib_h_library)
TV_ROUTE(library_post, TV_S | TV_B | TV_O, tvlib_h_library)
TV_ROUTE(library_storage_get, TV_S | TV_B, tvlib_h_storage)
TV_ROUTE(library_storage_post, TV_S | TV_B | TV_O, tvlib_h_storage)
TV_ROUTE(library_actions_post, TV_S | TV_B | TV_O, tvlib_h_actions)
TV_ROUTE(updates_get, TV_B, tvup_h_updates)              /* readable while stopping */
TV_ROUTE(updates_post, TV_S | TV_B | TV_O, tvup_h_updates)
TV_ROUTE(tvapp_get, TV_S | TV_B, tvup_h_tvapp)
TV_ROUTE(tvapp_post, TV_S | TV_B | TV_O, tvup_h_tvapp)

static const api_route TV_ROUTES[] = {
  { "GET", "/api/v1/system", system_get },
  { "GET", "/api/v1/session", session_get },
  { "GET", "/api/v1/zero/companion", zero_any },
  { "POST", "/api/v1/zero/companion", zero_any },
  { "GET", "/api/v1/sources", sources_get },
  { "POST", "/api/v1/sources", sources_post },
  { "GET", "/api/v1/catalog", catalog_get },
  { "GET", "/api/v1/catalog/updates", catalog_updates_get },
  { "POST", "/api/v1/catalog/updates", catalog_updates_post },
  { "GET", "/api/v1/art/{id}/{kind}", art_get },
  { "GET", "/api/v1/favorites", favorites_get },
  { "POST", "/api/v1/favorites", favorites_post },
  { "GET", "/api/v1/downloads", downloads_get },
  { "POST", "/api/v1/downloads", downloads_post },
  { "POST", "/api/v1/downloads/{id}/{action}", download_action_post },
  { "GET", "/api/v1/storage", storage_get },
  { "POST", "/api/v1/storage/preferred", storage_preferred_post },
  { "POST", "/api/v1/settings/downloads", settings_downloads_post },
  { "POST", "/api/v1/settings/extraction", settings_extraction_post },
  { "GET", "/api/v1/debrid", debrid_get },
  { "GET", "/api/v1/browser", browser_get },
  { "POST", "/api/v1/browser", browser_post },
  { "GET", "/api/v1/library", library_get },
  { "POST", "/api/v1/library", library_post },
  { "GET", "/api/v1/library/storage", library_storage_get },
  { "POST", "/api/v1/library/storage", library_storage_post },
  { "POST", "/api/v1/library/actions", library_actions_post },
  { "GET", "/api/v1/updates", updates_get },
  { "POST", "/api/v1/updates", updates_post },
  { "GET", "/api/v1/tv-app", tvapp_get },
  { "POST", "/api/v1/tv-app", tvapp_post },
};

/* ---- init and diagnostics -------------------------------------------------- */

void tvapi_init(void) {
  util_rand_hex(g_instance, 32);
  load_state();
  if (api_register_routes(TV_ROUTES, XS_ARRAY_LEN(TV_ROUTES)) != 0) LOGE("tvapi: route table not registered (extension slots full)");
  else LOGI("tvapi: %zu /api/v1 route(s) registered; the TV app services start on first use", XS_ARRAY_LEN(TV_ROUTES));
}

void tvapi_diag_json(jw *w) {
  jw_kv_bool(w, "started", g_started);
  jw_kv_i64(w, "startedAgoMs", g_started ? util_now_ms() - g_started_ms : 0);
  jw_kv_int(w, "threads", __atomic_load_n(&g_threads, __ATOMIC_SEQ_CST));
  jw_kv_int(w, "routes", (int)XS_ARRAY_LEN(TV_ROUTES));
  int64_t seen = g_client_seen;
  jw_kv_i64(w, "clientSeenAgoMs", seen ? util_now_ms() - seen : -1);
  jw_kv_bool(w, "stopping", tv_stopping());
  jw_kv_bool(w, "preferredAuto", tv_pref_auto());
  if (g_started) tvdl_storage_diag_json(w);
}
