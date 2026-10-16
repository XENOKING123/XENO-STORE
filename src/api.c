/*
 * api.c — see api.h. Every handler builds its reply with the JSON writer and
 * answers through api_send()/api_error(); the envelope rules of SPEC §6 are
 * enforced here (ok:true on success, {ok:false,error,message} on failure).
 *
 * Mutating requests (non-GET) require the X-XENO-PIN header when
 * settings.pairing.pin is set (SPEC §15).
 */
#include "api.h"
#include "assets.h"
#include "catalog.h"
#include "config.h"
#include "dl.h"
#include "http_client.h"
#include "install.h"
#include "log.h"
#include "pairing.h"
#include "settings.h"
#include "storage.h"
#include "sys.h"
#include "update.h"
#include "cx_common.h"
#include "util.h"
#include "xthread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ---- envelope helpers --------------------------------------------------- */

void api_begin(jw *w) {
  jw_init(w);
  jw_obj_begin(w);
  jw_kv_bool(w, "ok", 1);
}

void api_send(api_ctx *c, jw *w) {
  jw_obj_end(w);
  http_send_json(c->req, 200, w);
}

void api_error(api_ctx *c, int status, const char *code, const char *msg) {
  http_send_error(c->req, status, code, msg);
}

static int status_for_code(const char *code) {
  if (strcmp(code, "not_found") == 0 || strcmp(code, "bad_release") == 0) return 404;
  if (strcmp(code, "exists") == 0) return 409;
  if (strcmp(code, "no_space") == 0 || strcmp(code, "root_unavailable") == 0) return 422;
  if (strcmp(code, "downloads_unavailable") == 0) return 503;
  return 400;
}

static const char *body_str(api_ctx *c, const char *key, const char *dflt) { return jv_get_str(c->body, key, dflt); }
const char *api_body_str(api_ctx *c, const char *key, const char *dflt) { return body_str(c, key, dflt); }

/* ---- system ------------------------------------------------------------- */

static void storage_summary_json(jw *w) {
  storage_root roots[STORAGE_MAX_ROOTS];
  int n = storage_get_roots(roots, STORAGE_MAX_ROOTS);
  jw_arr_begin(w);
  for (int i = 0; i < n; i++) {
    jw_obj_begin(w);
    jw_kv_str(w, "path", roots[i].path);
    jw_kv_str(w, "label", roots[i].label);
    jw_kv_str(w, "kind", roots[i].kind);
    jw_kv_i64(w, "free", roots[i].free);
    jw_kv_i64(w, "total", roots[i].total);
    jw_kv_bool(w, "writable", roots[i].writable);
    jw_kv_bool(w, "default", roots[i].is_default);
    jw_obj_end(w);
  }
  jw_arr_end(w);
}

static void h_status(api_ctx *c) {
  jw w;
  api_begin(&w);
  jw_kv_str(&w, "app", XENO_APP_NAME);
  jw_kv_str(&w, "version", XENO_VERSION);
  jw_kv_str(&w, "build", XENO_BUILD_HASH);
  jw_kv_str(&w, "platform", sys_platform());
  jw_kv_str(&w, "fw", sys_fw_version());
  char ip[48];
  sys_local_ip(ip, sizeof ip);
  jw_kv_str(&w, "ip", ip);
  jw_kv_int(&w, "port", g_cfg.port);
  jw_kv_i64(&w, "uptime", (int64_t)(util_now() - g_cfg.started_at));
  jw_kv_bool(&w, "downloadsAvailable", xhttp_available());
  jw_kv_str(&w, "transport", xhttp_backend_name());
  jw_kv_bool(&w, "safeMode", g_cfg.safe_mode);
  jw_key(&w, "catalog"); jw_obj_begin(&w); catalog_status_json(&w); jw_obj_end(&w);
  dl_summary s;
  dl_summary_get(&s);
  jw_key(&w, "queue"); jw_obj_begin(&w);
  jw_kv_int(&w, "active", s.active); jw_kv_int(&w, "queued", s.queued); jw_kv_int(&w, "paused", s.paused);
  jw_kv_int(&w, "failed", s.failed); jw_kv_int(&w, "done", s.done); jw_kv_i64(&w, "speed", s.speed);
  jw_kv_i64(&w, "remaining", s.remaining); jw_kv_i64(&w, "eta", s.eta); jw_kv_bool(&w, "keepAwake", s.keepAwake);
  jw_obj_end(&w);
  jw_key(&w, "storage"); storage_summary_json(&w);
  jw_key(&w, "update"); jw_obj_begin(&w); update_status_json(&w); jw_obj_end(&w);
  if (api_status_extra) api_status_extra(&w);
  jw_key(&w, "notices"); jw_arr_begin(&w);
  if (!xhttp_available()) {
    jw_obj_begin(&w); jw_kv_str(&w, "id", "downloads_unavailable");
    jw_kv_str(&w, "text", "Downloads unavailable on this firmware (network libraries did not load). Browsing still works."); jw_obj_end(&w);
  }
  if (g_cfg.safe_mode) {
    jw_obj_begin(&w); jw_kv_str(&w, "id", "recovered_crash");
    jw_kv_str(&w, "text", "XENO STORE restarted after a problem. Automatic checks are paused for this session; details are in Settings → Diagnostics."); jw_obj_end(&w);
  }
  if (XENO_ASSETS_PLACEHOLDER_CATALOG) {
    jw_obj_begin(&w); jw_kv_str(&w, "id", "catalog_placeholder");
    jw_kv_str(&w, "text", "This build has no embedded catalog yet — refresh the catalog from Settings."); jw_obj_end(&w);
  }
  jw_arr_end(&w);
  api_send(c, &w);
}

/* Everything a user can copy into a bug report: the last crash report, the
 * thread stack sizes and the transport. */
static void h_diagnostics(api_ctx *c) {
  long up = -1;
  int fresh = 0;
  const char *rep = crash_previous(&up, &fresh);
  jw w;
  api_begin(&w);
  jw_kv_str(&w, "version", XENO_VERSION);
  jw_kv_str(&w, "build", XENO_BUILD_HASH);
  jw_kv_str(&w, "platform", sys_platform());
  jw_kv_str(&w, "fw", sys_fw_version());
  jw_kv_str(&w, "transport", xhttp_backend_name());
  jw_kv_i64(&w, "uptime", (int64_t)(util_now() - g_cfg.started_at));
  jw_kv_bool(&w, "safeMode", g_cfg.safe_mode);
  jw_key(&w, "threads"); jw_obj_begin(&w);
  jw_kv_i64(&w, "platformDefaultStack", (int64_t)xthread_platform_default_stack());
  jw_kv_i64(&w, "storeStack", (int64_t)XT_STACK_DEFAULT);
  jw_obj_end(&w);
  jw_key(&w, "crash"); jw_obj_begin(&w);
  jw_kv_bool(&w, "present", rep[0] != 0);
  jw_kv_bool(&w, "lastRun", fresh);
  jw_kv_i64(&w, "uptime", (int64_t)up);
  jw_kv_str(&w, "report", rep);
  jw_obj_end(&w);
  /* queue persistence (a failed queue.json write) and the job workers still running */
  jw_key(&w, "queue"); jw_obj_begin(&w);
  jw_kv_bool(&w, "stateHealthy", dl_state_healthy());
  jw_kv_int(&w, "workersRunning", dl_workers_running());
  jw_obj_end(&w);
  if (tvapi_diag_json) { jw_key(&w, "tvapi"); jw_obj_begin(&w); tvapi_diag_json(&w); jw_obj_end(&w); }
  api_send(c, &w);
}

static void h_log(api_ctx *c) {
  int lines = http_query_int(c->req, "lines", 200);
  if (lines < 1) lines = 1;
  if (lines > 400) lines = 400;
  char (*buf)[LOG_LINE_MAX] = malloc((size_t)lines * LOG_LINE_MAX);
  if (!buf) { api_error(c, 500, "oom", "Out of memory"); return; }
  int n = log_recent(buf, lines);
  jw w;
  api_begin(&w);
  jw_key(&w, "lines"); jw_arr_begin(&w);
  for (int i = 0; i < n; i++) jw_str(&w, buf[i]);
  jw_arr_end(&w);
  free(buf);
  api_send(c, &w);
}

static void h_toast(api_ctx *c) {
  const char *text = body_str(c, "text", "");
  if (!*text) { api_error(c, 400, "bad_request", "text is required"); return; }
  sys_toast("%s", text);
  jw w;
  api_begin(&w);
  api_send(c, &w);
}

static void h_stop(api_ctx *c) {
  dl_save_queue();
  sys_toast("XENO STORE stopping");
  jw w;
  api_begin(&w);
  jw_kv_bool(&w, "stopping", 1);
  api_send(c, &w);
  LOGI("stop requested via API by %s", c->req->client_ip);
  g_cfg.stop_requested = 1;
}

static void h_update_check(api_ctx *c) {
  char err[256];
  int rc = update_check(err, sizeof err);
  jw w;
  api_begin(&w);
  update_status_json(&w);
  if (rc != 0) jw_kv_str(&w, "warning", err);
  api_send(c, &w);
}

static void h_update_apply(api_ctx *c) {
  char path[600], err[256];
  if (update_apply(path, sizeof path, err, sizeof err) != 0) { api_error(c, 502, "update_failed", err); return; }
  jw w;
  api_begin(&w);
  jw_kv_str(&w, "path", path);
  jw_kv_bool(&w, "restartRequired", 1);
  api_send(c, &w);
}

/* Restart into the staged update: the reply goes out first, then main() stops
 * the server and the downloads (checkpoints saved, port released) and hands the
 * staged ELF to the local loader, which starts the new version. */
static void h_update_restart(api_ctx *c) {
  char path[600];
  if (!update_staged(path, sizeof path)) { api_error(c, 409, "nothing_staged", "Download the update first (Settings → Updates → Install update)"); return; }
  int port = cx_local_port("LOADER", 9021);
  if (!cx_tcp_probe(port, 800)) { api_error(c, 503, "loader_unavailable", "The ELF loader is not answering on 127.0.0.1:9021 — load etaHEN or elfldr, then try again"); return; }
  LOGI("update: restart requested (staged %s, loader :%d)", path, port);
  g_cfg.restart_requested = 1;
  g_cfg.stop_requested = 1;
  jw w;
  api_begin(&w);
  jw_kv_bool(&w, "restarting", 1);
  jw_kv_str(&w, "path", path);
  jw_kv_int(&w, "loaderPort", port);
  api_send(c, &w);
}

/* ---- catalog ------------------------------------------------------------ */

static void h_catalog_info(api_ctx *c) {
  jw w;
  api_begin(&w);
  catalog_info_json(&w);
  api_send(c, &w);
}

static void h_catalog_refresh(api_ctx *c) {
  char err[256];
  int updated = 0;
  int force = jv_get_bool(c->body, "force", 0) || http_query_int(c->req, "force", 0);
  int rc = catalog_refresh(force, &updated, err, sizeof err);
  if (rc == CATALOG_REFRESH_BUSY) { api_error(c, 409, "refresh_busy", err); return; }
  if (rc != 0) { api_error(c, 502, "refresh_failed", err); return; }
  jw w;
  api_begin(&w);
  jw_kv_bool(&w, "updated", updated);
  jw_kv_i64(&w, "revision", catalog_revision());
  api_send(c, &w);
}

static void h_catalog_search(api_ctx *c) {
  char q[256], platform[16];
  http_query_get(c->req, "q", q, sizeof q);
  http_query_get(c->req, "platform", platform, sizeof platform);
  int limit = http_query_int(c->req, "limit", 50);
  jw w;
  api_begin(&w);
  catalog_search(q, platform, limit, &w);
  api_send(c, &w);
}

static void h_links_check(api_ctx *c) {
  jv urls = jv_get(c->body, "urls");
  if (jv_type(urls) != JV_ARR) { api_error(c, 400, "bad_request", "urls must be an array"); return; }
  if (!xhttp_available()) { api_error(c, 503, "downloads_unavailable", "Network transport unavailable"); return; }
  jw w;
  api_begin(&w);
  catalog_links_check(urls, &w);
  api_send(c, &w);
}

static void h_links_resolve(api_ctx *c) {
  const char *url = body_str(c, "url", "");
  if (!util_istarts_with(url, "http")) { api_error(c, 400, "bad_request", "url must be http(s)"); return; }
  if (!xhttp_available()) { api_error(c, 503, "downloads_unavailable", "Network transport unavailable"); return; }
  char err[256];
  jw w;
  api_begin(&w);
  int rc = catalog_links_resolve(url, &w, err, sizeof err);
  if (rc != 0) jw_kv_str(&w, "warning", err);
  api_send(c, &w);
}

/* ---- queue -------------------------------------------------------------- */

static void h_queue_get(api_ctx *c) {
  jw w;
  api_begin(&w);
  dl_queue_json(&w, http_query_int(c->req, "history", 0));
  api_send(c, &w);
}

static int body_tristate(api_ctx *c, const char *key) {
  jv v = jv_get(c->body, key);
  if (!jv_ok(v) || jv_type(v) == JV_NULL) return -1;
  return jv_bool(v, 0) ? 1 : 0;
}

static void h_queue_add(api_ctx *c) {
  if (!jv_ok(c->body)) { api_error(c, 400, "bad_request", "JSON body required"); return; }
  dl_add_req r;
  memset(&r, 0, sizeof r);
  r.gameId = body_str(c, "gameId", "");
  r.releaseId = body_str(c, "releaseId", "");
  r.root = body_str(c, "root", "");
  r.url = body_str(c, "url", "");
  r.filename = body_str(c, "filename", "");
  r.format = body_str(c, "format", "");
  r.title = body_str(c, "title", "");
  r.sha256 = body_str(c, "sha256", "");
  r.password = body_str(c, "password", "");
  r.mirrors = jv_get(c->body, "mirrors");
  r.size = jv_get_i64(c->body, "size", 0);
  r.connections = (int)jv_get_i64(c->body, "connections", 0);
  r.verify = body_tristate(c, "verify");
  r.autoInstall = body_tristate(c, "autoInstall");
  r.overwrite = jv_get_bool(c->body, "overwrite", 0);
  char id[16], code[32], msg[256];
  if (dl_add(&r, id, sizeof id, code, sizeof code, msg, sizeof msg) != 0) { api_error(c, status_for_code(code), code, msg); return; }
  jw w;
  api_begin(&w);
  jw_key(&w, "job");
  dl_job_json(&w, id, 0);
  api_send(c, &w);
}

static void h_queue_action(api_ctx *c) {
  const char *id = c->params[0], *action = c->params[1];
  char code[32], msg[256];
  if (dl_action(id, action, c->body, code, sizeof code, msg, sizeof msg) != 0) { api_error(c, status_for_code(code), code, msg); return; }
  jw w;
  api_begin(&w);
  if (strcmp(action, "remove") != 0) { jw_key(&w, "job"); dl_job_json(&w, id, 0); }
  else jw_kv_str(&w, "removed", id);
  api_send(c, &w);
}

static void h_queue_log(api_ctx *c) {
  jw w;
  api_begin(&w);
  if (dl_job_log_json(&w, c->params[0]) != 0) { jw_free(&w); api_error(c, 404, "not_found", "No such job"); return; }
  api_send(c, &w);
}

static void h_queue_clear(api_ctx *c) {
  int n = dl_clear(jv_get(c->body, "states"));
  jw w;
  api_begin(&w);
  jw_kv_int(&w, "removed", n);
  api_send(c, &w);
}

static void h_space(api_ctx *c) {
  char root[256], bytes_s[32];
  http_query_get(c->req, "root", root, sizeof root);
  http_query_get(c->req, "bytes", bytes_s, sizeof bytes_s);
  int64_t bytes = 0;
  util_parse_i64(bytes_s, &bytes);
  if (bytes < 0) bytes = 0;
  storage_root sr;
  if (!root[0]) {
    storage_root roots[STORAGE_MAX_ROOTS];
    int n = storage_get_roots(roots, STORAGE_MAX_ROOTS);
    int found = 0;
    for (int i = 0; i < n; i++) if (roots[i].is_default) { sr = roots[i]; found = 1; break; }
    if (!found && n) { sr = roots[0]; found = 1; }
    if (!found) { api_error(c, 422, "root_unavailable", "No storage root is available"); return; }
  } else if (storage_find_root(root, &sr) != 0) {
    api_error(c, 422, "root_unavailable", "Unknown storage root");
    return;
  }
  int64_t after_queue = sr.free - dl_remaining_on_root(sr.path);
  int64_t after_this = after_queue - bytes;
  jw w;
  api_begin(&w);
  jw_kv_str(&w, "root", sr.path);
  jw_kv_i64(&w, "free", sr.free);
  jw_kv_i64(&w, "afterQueue", after_queue);
  jw_kv_i64(&w, "afterThis", after_this);
  jw_kv_bool(&w, "fits", after_this >= DL_RESERVE_BYTES);
  api_send(c, &w);
}

/* ---- storage & library -------------------------------------------------- */

static void h_storage(api_ctx *c) {
  storage_scan_roots();
  jw w;
  api_begin(&w);
  jw_key(&w, "roots");
  storage_summary_json(&w);
  api_send(c, &w);
}

static void h_storage_default(api_ctx *c) {
  const char *root = body_str(c, "root", "");
  if (storage_find_root(root, NULL) != 0) { api_error(c, 422, "root_unavailable", "Unknown storage root"); return; }
  jw patch;
  jw_init(&patch);
  jw_obj_begin(&patch); jw_key(&patch, "downloads"); jw_obj_begin(&patch); jw_kv_str(&patch, "defaultRoot", root); jw_obj_end(&patch); jw_obj_end(&patch);
  size_t len;
  char *txt = jw_detach(&patch, &len);
  char err[128] = "";
  int rc = -1;
  if (txt) {
    jdoc *d = json_parse(txt, len, 100, 8);
    if (d) { rc = settings_merge(json_root(d), err, sizeof err); json_free(d); }
    free(txt);
  }
  if (rc != 0) { api_error(c, 400, "bad_request", err[0] ? err : "Cannot save default root"); return; }
  jw w;
  api_begin(&w);
  jw_kv_str(&w, "root", root);
  api_send(c, &w);
}

static void h_library(api_ctx *c) {
  if (http_query_int(c->req, "scan", 1)) storage_scan_roots();
  jw w;
  api_begin(&w);
  storage_library_json(&w);
  api_send(c, &w);
}

static void h_library_delete(api_ctx *c) {
  const char *path = body_str(c, "path", "");
  char err[256];
  if (!*path) { api_error(c, 400, "bad_request", "path is required"); return; }
  if (storage_library_delete(path, err, sizeof err) != 0) { api_error(c, 403, "forbidden", err); return; }
  jw w;
  api_begin(&w);
  api_send(c, &w);
}

static void h_library_install(api_ctx *c) {
  const char *path = body_str(c, "path", "");
  if (!*path) { api_error(c, 400, "bad_request", "path is required"); return; }
  if (!storage_library_path_allowed(path)) { api_error(c, 403, "forbidden", "Path is not in the last library scan"); return; }
  char id[16], msg[256];
  if (dl_add_install_job(path, id, sizeof id, msg, sizeof msg) != 0) { api_error(c, 422, "install_failed", msg); return; }
  jw w;
  api_begin(&w);
  jw_key(&w, "job");
  dl_job_json(&w, id, 0);
  api_send(c, &w);
}

static void h_shadowmount_scan(api_ctx *c) {
  char status[160];
  int available = install_shadowmount_scan(status, sizeof status);
  jw w;
  api_begin(&w);
  jw_kv_bool(&w, "available", available);
  jw_kv_str(&w, "status", status);
  api_send(c, &w);
}

/* ---- settings, favorites, collections, recent ---------------------------- */

static void h_settings_get(api_ctx *c) {
  jw w;
  api_begin(&w);
  jw_key(&w, "settings");
  settings_write_json(&w);
  api_send(c, &w);
}

static void h_settings_put(api_ctx *c) {
  char err[160];
  if (!jv_ok(c->body)) { api_error(c, 400, "bad_request", "JSON object required"); return; }
  jv obj = jv_ok(jv_get(c->body, "settings")) ? jv_get(c->body, "settings") : c->body;
  if (settings_merge(obj, err, sizeof err) != 0) { api_error(c, 400, "invalid_settings", err); return; }
  storage_scan_roots();
  h_settings_get(c);
}

static void h_favorites_get(api_ctx *c) {
  jw w;
  api_begin(&w);
  settings_favorites_json(&w);
  api_send(c, &w);
}

static void h_favorites_put(api_ctx *c) {
  if (settings_favorites_set(jv_get(c->body, "ids")) != 0) { api_error(c, 400, "bad_request", "ids must be an array"); return; }
  h_favorites_get(c);
}

static void h_favorites_toggle(api_ctx *c) {
  const char *id = body_str(c, "id", "");
  int st = settings_favorite_toggle(id);
  if (st < 0) { api_error(c, 400, "bad_request", "id is required"); return; }
  jw w;
  api_begin(&w);
  jw_kv_str(&w, "id", id);
  jw_kv_bool(&w, "favorite", st);
  api_send(c, &w);
}

static void h_collections_get(api_ctx *c) {
  jw w;
  api_begin(&w);
  settings_collections_json(&w);
  api_send(c, &w);
}

static void h_collections_put(api_ctx *c) {
  char err[160];
  jv list = jv_ok(jv_get(c->body, "collections")) ? jv_get(c->body, "collections") : c->body;
  if (settings_collections_set(list, err, sizeof err) != 0) { api_error(c, 400, "bad_request", err); return; }
  h_collections_get(c);
}

static void h_recent_get(api_ctx *c) {
  jw w;
  api_begin(&w);
  settings_recent_json(&w);
  api_send(c, &w);
}

static void h_recent_add(api_ctx *c) {
  const char *id = body_str(c, "id", "");
  if (!*id) { api_error(c, 400, "bad_request", "id is required"); return; }
  settings_recent_add(id);
  h_recent_get(c);
}

static void h_export(api_ctx *c) {
  jw w;
  api_begin(&w);
  settings_export_json(&w);
  api_send(c, &w);
}

static void h_import(api_ctx *c) {
  char err[160];
  if (settings_import_json(c->body, err, sizeof err) != 0) { api_error(c, 400, "bad_request", err); return; }
  storage_scan_roots();
  jw w;
  api_begin(&w);
  jw_kv_bool(&w, "imported", 1);
  api_send(c, &w);
}

static void h_pair(api_ctx *c) {
  char ip[48], url[96];
  sys_local_ip(ip, sizeof ip);
  xsnprintf(url, sizeof url, "http://%s:%d/", ip, g_cfg.port);
  settings_t s;
  settings_get(&s);
  jw w;
  api_begin(&w);
  jw_kv_str(&w, "url", url);
  jw_kv_str(&w, "ip", ip);
  jw_kv_int(&w, "port", g_cfg.port);
  jw_kv_bool(&w, "pinRequired", s.pin[0] != 0);
  /* The literal code is shown only on the console (loopback peer); a LAN phone
   * must read it off the PS5 toast. `cooldown` throttles POST /api/pair/show. */
  jw_kv_str(&w, "code", c->req->client_is_loopback ? pairing_code() : "");
  jw_kv_int(&w, "cooldown", pairing_show_cooldown());
  /* The web UI's first page on a PC / phone: "local" = the console itself (no linking needed),
   * "linked" = this browser already holds a valid device token. */
  const char *bearer = util_istarts_with(c->req->authorization, "Bearer ") ? c->req->authorization + 7 : NULL;
  jw_kv_bool(&w, "local", c->req->client_is_loopback);
  jw_kv_bool(&w, "linked", bearer && pairing_token_valid(bearer));
  api_send(c, &w);
}

static void h_pair_show(api_ctx *c) {
  int retry = 0;
  int rc = pairing_show(&retry);
  if (rc == 1) {   /* within cooldown */
    jw w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kv_bool(&w, "ok", 0);
    jw_kv_str(&w, "error", "cooldown");
    jw_kv_str(&w, "message", "Wait a moment before showing the code again.");
    jw_kv_int(&w, "retryAfter", retry);
    jw_kv_int(&w, "status", 429);
    jw_obj_end(&w);
    http_send_json(c->req, 429, &w);
    return;
  }
  jw w;
  api_begin(&w);
  jw_kv_bool(&w, "shown", 1);
  jw_kv_int(&w, "retryAfter", retry);
  api_send(c, &w);
}

static void h_pair_redeem(api_ctx *c) {
  const char *code = body_str(c, "code", "");
  char token[72], name[48];
  int retry = 0;
  pairing_device_name(c->req->user_agent, name, sizeof name);
  int rc = pairing_redeem(code, name, token, sizeof token, &retry);
  if (rc == 0) {
    jw w;
    api_begin(&w);
    jw_kv_str(&w, "token", token);
    jw_kv_bool(&w, "paired", 1);
    api_send(c, &w);
    return;
  }
  int status = -rc;
  if (status == 429) {
    jw w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kv_bool(&w, "ok", 0);
    jw_kv_str(&w, "error", "locked_out");
    jw_kv_str(&w, "message", "Too many attempts. Wait a minute and try again.");
    jw_kv_int(&w, "retryAfter", retry);
    jw_kv_int(&w, "status", 429);
    jw_obj_end(&w);
    http_send_json(c->req, 429, &w);
    return;
  }
  const char *scode = status == 503 ? "save_failed" : "bad_code";
  const char *msg = status == 503 ? "Could not save the pairing." : "Pairing failed. Check the code shown on the console.";
  api_error(c, status, scode, msg);
}

/* Settings → Pair a device. The /api/pair routes skip the PIN gate, so these check the
 * caller themselves: the console itself or a paired device, never a stranger.
 * Returns the caller's bearer token (NULL without one) through *self. */
static int pair_caller_ok(api_ctx *c, const char **self) {
  const char *bearer = util_istarts_with(c->req->authorization, "Bearer ") ? c->req->authorization + 7 : NULL;
  *self = bearer;
  if (c->req->client_is_loopback || (bearer && pairing_token_valid(bearer))) return 1;
  api_error(c, 403, "not_paired", "Pair this device first.");
  return 0;
}

static void h_pair_devices(api_ctx *c) {
  const char *self;
  if (!pair_caller_ok(c, &self)) return;
  jw w;
  api_begin(&w);
  pairing_devices_json(&w, self);
  api_send(c, &w);
}

static void h_pair_unpair(api_ctx *c) {
  const char *self;
  if (!pair_caller_ok(c, &self)) return;
  const char *id = body_str(c, "id", "");
  if (!*id) { api_error(c, 400, "bad_request", "id is required"); return; }
  int rc = pairing_unpair(id);
  if (rc == -404) { api_error(c, 404, "not_found", "No paired device has that id."); return; }
  if (rc != 0) { api_error(c, 503, "save_failed", "Could not save the pairing."); return; }
  jw w;
  api_begin(&w);
  jw_kv_bool(&w, "unpaired", 1);
  pairing_devices_json(&w, self);
  api_send(c, &w);
}

static void h_pair_unpair_others(api_ctx *c) {
  const char *self;
  if (!pair_caller_ok(c, &self)) return;
  int n = pairing_unpair_others(self);
  if (n < 0) { api_error(c, 503, "save_failed", "Could not save the pairing."); return; }
  jw w;
  api_begin(&w);
  jw_kv_int(&w, "unpaired", n);
  pairing_devices_json(&w, self);
  api_send(c, &w);
}

/* The Games-row app (app/eboot) calls this when the store is already running: open the
 * console's browser full-screen on the store. Loopback only; needs no PIN. */
static void h_launcher_open(api_ctx *c) {
  if (!c->req->client_is_loopback) { api_error(c, 403, "forbidden", "Only the console itself can open its browser."); return; }
  char url[64];
  xsnprintf(url, sizeof url, "http://127.0.0.1:%d/?tv=1", g_cfg.port);
  /* The app tile and the store's own start-up open can arrive together: open once. */
  int recent = sys_browser_opened_ago_ms() < 8000;
  int rc = recent ? 0 : sys_launch_browser(url);
  jw w;
  api_begin(&w);
  jw_kv_bool(&w, "launched", rc == 0);
  jw_kv_bool(&w, "alreadyOpen", recent);
  jw_kv_str(&w, "url", url);
  api_send(c, &w);
}

static void h_session(api_ctx *c) {
  if (!c->req->client_is_loopback) {
    api_error(c, 403, "forbidden", "Pair this device using the code shown on the console.");
    return;
  }
  char token[72];
  if (pairing_ensure_token(token, sizeof token) != 0) { api_error(c, 503, "save_failed", "Could not create a session token."); return; }
  char ip[48], url[96];
  sys_local_ip(ip, sizeof ip);
  xsnprintf(url, sizeof url, "http://%s:%d/", ip, g_cfg.port);
  jw w;
  api_begin(&w);
  jw_kv_str(&w, "address", ip);
  jw_kv_int(&w, "port", g_cfg.port);
  jw_kv_str(&w, "token", token);
  jw_kv_str(&w, "pairUrl", url);
  jw_kv_str(&w, "code", pairing_code());
  api_send(c, &w);
}

#ifdef XENO_HOST
/* Host build only (tests): runs the TV app's stop step 1-2 on its own, i.e.
 * pauses every queued and active job with the restart message and saves
 * queue.json synchronously (dl_pause_all_for_stop). Nothing exits. */
static void h_debug_pause_all(api_ctx *c) {
  int n = dl_pause_all_for_stop("Paused for the XENO STORE restart");
  if (n == DL_PAUSE_INSTALLING) { api_error(c, 409, "installing", "A package install is reading from XENO STORE"); return; }
  if (n < 0) { api_error(c, 503, "save_failed", "The queue could not be saved; the jobs were left as they were"); return; }
  jw w;
  api_begin(&w);
  jw_kv_int(&w, "paused", n);
  jw_kv_int(&w, "workersRunning", dl_workers_running());
  jw_kv_bool(&w, "stateHealthy", dl_state_healthy());
  api_send(c, &w);
}
#endif

/* ---- routing ------------------------------------------------------------ */

static const api_route ROUTES[] = {
  { "GET", "/api/status", h_status },
  { "GET", "/api/log", h_log },
  { "GET", "/api/diagnostics", h_diagnostics },
  { "POST", "/api/system/toast", h_toast },
  { "POST", "/api/system/stop", h_stop },
  { "GET", "/api/update/check", h_update_check },
  { "POST", "/api/update/check", h_update_check },
  { "POST", "/api/update/apply", h_update_apply },
  { "POST", "/api/update/restart", h_update_restart },
  { "POST", "/api/launcher/open", h_launcher_open },
  { "GET", "/api/catalog/info", h_catalog_info },
  { "POST", "/api/catalog/refresh", h_catalog_refresh },
  { "GET", "/api/catalog/search", h_catalog_search },
  { "POST", "/api/links/check", h_links_check },
  { "POST", "/api/links/resolve", h_links_resolve },
  { "GET", "/api/queue", h_queue_get },
  { "POST", "/api/queue", h_queue_add },
  { "POST", "/api/queue/clear", h_queue_clear },
  { "GET", "/api/queue/{id}/log", h_queue_log },
  { "POST", "/api/queue/{id}/{action}", h_queue_action },
  { "GET", "/api/space", h_space },
  { "GET", "/api/storage", h_storage },
  { "POST", "/api/storage/default", h_storage_default },
  { "GET", "/api/library", h_library },
  { "POST", "/api/library/delete", h_library_delete },
  { "POST", "/api/library/install", h_library_install },
  { "POST", "/api/shadowmount/scan", h_shadowmount_scan },
  { "GET", "/api/settings", h_settings_get },
  { "PUT", "/api/settings", h_settings_put },
  { "GET", "/api/favorites", h_favorites_get },
  { "PUT", "/api/favorites", h_favorites_put },
  { "POST", "/api/favorites/toggle", h_favorites_toggle },
  { "GET", "/api/collections", h_collections_get },
  { "PUT", "/api/collections", h_collections_put },
  { "GET", "/api/recent", h_recent_get },
  { "POST", "/api/recent", h_recent_add },
  { "GET", "/api/export", h_export },
  { "POST", "/api/import", h_import },
  { "GET", "/api/pair", h_pair },
  { "POST", "/api/pair/show", h_pair_show },
  { "POST", "/api/pair", h_pair_redeem },
  { "GET", "/api/pair/devices", h_pair_devices },
  { "POST", "/api/pair/unpair", h_pair_unpair },
  { "POST", "/api/pair/unpair-others", h_pair_unpair_others },
  { "GET", "/api/session", h_session },
#ifdef XENO_HOST
  { "POST", "/api/debug/queue/pause-all", h_debug_pause_all },
#endif
};

/* Extension route tables (registered by Phase 2 modules). */
#define API_EXT_MAX 8
static struct { const api_route *t; size_t n; } g_ext[API_EXT_MAX];
static int g_ext_n = 0;
int api_register_routes(const api_route *table, size_t n) {
  if (!table || !n || g_ext_n >= API_EXT_MAX) return -1;
  g_ext[g_ext_n].t = table; g_ext[g_ext_n].n = n; g_ext_n++;
  return 0;
}
void api_ext_init(void) {
  if (ext_downloads_init) ext_downloads_init();
  if (ext_console_init) ext_console_init();
  if (tvapi_init) tvapi_init();   /* the /api/v1 table (TV app); its services start on first use */
}

int api_is_v1_path(const char *path) {
  return util_starts_with(path, "/api/v1/") || strcmp(path, "/api/v1") == 0;
}

/* Matches path against pattern segment by segment; fills captures. */
static int route_match(const char *pattern, const char *path, api_ctx *c) {
  const char *p = pattern, *s = path;
  c->nparams = 0;
  while (*p && *s) {
    if (*p == '{') {
      const char *close = strchr(p, '}');
      if (!close) return 0;
      const char *seg_end = strchr(s, '/');
      size_t n = seg_end ? (size_t)(seg_end - s) : strlen(s);
      if (n == 0 || n >= sizeof c->params[0] || c->nparams >= 3) return 0;
      memcpy(c->params[c->nparams], s, n);
      c->params[c->nparams][n] = 0;
      c->nparams++;
      p = close + 1;
      s += n;
      continue;
    }
    if (*p != *s) return 0;
    p++; s++;
  }
  return *p == 0 && *s == 0;
}

void api_init(void) {}

void api_handle(http_req *req) {
  api_ctx c;
  memset(&c, 0, sizeof c);
  c.req = req;
  c.body.d = NULL;
  c.body.i = 0xFFFFFFFFu;
  int method_seen = 0;
  const api_route *hit = NULL;
  for (size_t i = 0; i < XS_ARRAY_LEN(ROUTES); i++) {
    api_ctx probe = c;
    if (!route_match(ROUTES[i].pattern, req->path, &probe)) continue;
    method_seen = 1;
    if (strcmp(ROUTES[i].method, req->method) == 0 || (strcmp(req->method, "HEAD") == 0 && strcmp(ROUTES[i].method, "GET") == 0)) {
      hit = &ROUTES[i];
      c = probe;
      break;
    }
  }
  /* Extension tables (Phase 2 modules) are consulted after the core table. */
  for (int t = 0; !hit && t < g_ext_n; t++) {
    for (size_t i = 0; i < g_ext[t].n; i++) {
      api_ctx probe = c;
      if (!route_match(g_ext[t].t[i].pattern, req->path, &probe)) continue;
      method_seen = 1;
      if (strcmp(g_ext[t].t[i].method, req->method) == 0 || (strcmp(req->method, "HEAD") == 0 && strcmp(g_ext[t].t[i].method, "GET") == 0)) {
        hit = &g_ext[t].t[i];
        c = probe;
        break;
      }
    }
  }
  /* Paths under /api/v1 are the TV app's dialect: the dispatcher's own refusals carry a
   * sentence in "error" instead of a code (PORT-SPEC 1.3), because the app
   * shows that field to the player. */
  int v1 = api_is_v1_path(req->path);
  if (!hit) {
    if (v1) http_send_sentence(req, method_seen ? 405 : 404, API_V1_MSG_NOT_AVAILABLE);
    else if (method_seen) http_send_error(req, 405, "method_not_allowed", "Method not allowed for this route");
    else http_send_error(req, 404, "not_found", "Unknown API route");
    return;
  }
  /* A paired device's last seen (Settings → Pair a device). */
  const char *bearer = util_istarts_with(req->authorization, "Bearer ") ? req->authorization + 7 : NULL;
  if (bearer) pairing_touch(bearer, req->user_agent);
  /* A token the console no longer knows was unpaired (Settings → Pair a device):
   * refused even where no token is needed, so a device that still has the store
   * open goes back to the Link page instead of carrying on. Pairing itself stays
   * reachable, the console itself is never cut off, and the TV app's dialect
   * answers for its own token (paired:false, 401). */
  if (bearer && !v1 && !req->client_is_loopback && !util_starts_with(req->path, "/api/pair") && !pairing_token_valid(bearer)) {
    http_send_error(req, 403, "not_paired", "This device is no longer paired with the console. Pair it again with the code the PS5 shows.");
    return;
  }
  if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "HEAD") != 0 &&
      !util_starts_with(req->path, "/api/pair") &&   /* pairing itself must stay reachable */
      !(req->client_is_loopback && strcmp(req->path, "/api/launcher/open") == 0)) {   /* the Games-row app */
    settings_t s;
    settings_get(&s);
    if (s.pin[0]) {
      int authed = strcmp(s.pin, req->pin) == 0 || (bearer && pairing_token_valid(bearer));
      if (!authed) {
        if (v1) http_send_sentence(req, 401, API_V1_MSG_PAIR_AGAIN);
        else http_send_error(req, 401, "pin_required",
                             "This XENO STORE requires the pairing PIN (X-XENO-PIN) or a paired device token (Authorization: Bearer)");
        return;
      }
    }
  }
  if (req->body && req->body_len) {
    c.doc = json_parse(req->body, req->body_len, 100000, 32);
    if (c.doc) c.body = json_root(c.doc);
    else if (strcmp(req->method, "GET") != 0) {
      if (v1) http_send_sentence(req, 400, API_V1_MSG_NOT_OBJECT);
      else http_send_error(req, 400, "bad_json", "Request body is not valid JSON");
      return;
    }
  }
  hit->fn(&c);
  json_free(c.doc);
  if (!req->responded) {
    if (v1) http_send_sentence(req, 500, API_V1_MSG_FAILED);
    else http_send_error(req, 500, "no_response", "Handler produced no response");
  }
}
