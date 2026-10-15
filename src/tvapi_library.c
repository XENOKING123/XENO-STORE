/*
 * tvapi_library.c — GET|POST /api/v1/library, GET|POST
 * /api/v1/library/storage and POST /api/v1/library/actions, option B (our
 * own drive scan) with the tvlib worker (PORT-SPEC 1.5.4; map-library).
 *
 * Copyright (C) 2026 XENO STORE contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ported from Orbit Store backend/library.c (GPL-3.0-or-later): the action
 * validator and its sentences, the 30 s (GET) / 5 s (POST) re-read
 * cooldowns, the snapshot members, the sourceKey identity
 * sha256(titleId \n path \n format), the storage job object and the
 * "replay returns 202, a reused request id returns 409" rule. The rows come
 * from XENO STORE's own drive scan instead of ShadowMount's games API.
 *
 * Threads and locks. One detached worker ("tvlib") owns every scan, every
 * action and the copy-job monitor. The HTTP handlers only copy cached state
 * under tvlib_mu (a leaf lock: nothing else is called while it is held), so
 * no request ever walks a drive, calls statvfs or waits for the worker.
 *
 * Change detection (map-library 3.1). Before walking, the worker builds a
 * signature from the drive snapshot, the mtimes of each root's three scan
 * folders and their first-level subfolders, the installed-titles folder, the
 * catalogue revision, the finished-job count and the catalogue filename
 * lookups of the files that did not resolve through the catalogue last time.
 * When nothing changed it only marks the snapshot fresh: no walk.
 */
#include "tvapi.h"

#include "app_install.h"
#include "catalog.h"
#include "config.h"
#include "cx_common.h"
#include "install.h"
#include "log.h"
#include "settings.h"
#include "sha256.h"
#include "storage.h"
#include "util.h"
#include "xthread.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- timing (R10, REF/library.c:760-768, map-library 3.1, 6.6) ------------ */
#define LIB_GET_COOLDOWN_MS 30000     /* a GET schedules a re-check at most every 30 s */
#define LIB_POST_COOLDOWN_MS 5000     /* a POST (Refresh) every 5 s; also the trigger floor */
#define LIB_STALE_MS 90000            /* R10: "stale" after 90 s without a successful check */
#define STORAGE_STALE_MS 30000        /* /library/storage: the drive snapshot is older than 30 s */
#define STORAGE_KICK_MS 5000          /* POST /library/storage forces a rescan at most every 5 s */
#define JOB_POLL_ACTIVE_MS 1000
#define JOB_POLL_IDLE_MS 5000
#define TRIGGER_EVERY_MS 1000
#define PROBE_EVERY_MS 5000
#define SCAN_FOLLOWUP_MS 15000        /* one more check after a scan action (ShadowMountPlus registers) */
#define TICK_MS 100
#define SIG_RECENT_S 2                /* an mtime this close to "now" may hide a same-second change */
#define SIG_DIR_MAX 4096              /* first-level entries looked at per scan folder */
#define PROBE_MAX 2000                /* storage.c LIB_MAX_FILES */

/* ---- sentences (map-library 6.7: app translation keys, verbatim) ---------- */
#define S_BAD_ACTION "Expected a supported action and its current Library identifiers."
#define S_CONFIRM "Confirm this action first."
#define S_UNAVAILABLE "Library is unavailable."
#define S_UNAVAILABLE_RESTART "Library is unavailable. Restart XENO STORE to try again."   /* PORT-SPEC 2.6 rebrand */
#define S_REQUEST_REUSED "This request ID was already used for another action."
#define S_IN_PROGRESS "Another Library operation is in progress. Wait for it to finish."
#define S_INCOMPATIBLE "Refresh Library with a compatible ShadowMount version before using this action."
#define S_WAITING "Waiting to start\xE2\x80\xA6"
#define S_CHECKING "Checking current game and drive status\xE2\x80\xA6"
#define S_UPDATED "Library updated."
#define S_SCAN_REQUESTED "Scan requested. ShadowMount may register or mount detected games. Refresh Library after it finishes."
#define S_STARTED "Storage operation started. Progress is available in Library and Storage."
#define S_CANCEL_REQUESTED "Cancellation requested. Waiting for the storage job to stop."
#define S_PAUSE_DOWNLOADS "Pause active and queued downloads before copying or moving games."
#define S_SOURCE_CHANGED "This game's source changed. Refresh Library and select it again."
#define S_SOURCE_UNSUPPORTED "This source is missing or does not support that action."
#define S_DEST_CHANGED "The selected destination changed or is no longer writable. Refresh storage."
#define S_DEST_INSIDE "Choose a different destination outside the source folder."
#define S_NO_SPACE "Not enough free space after unfinished downloads on the destination drive."
#define S_EXISTS "A file with this name is already on that drive. XENO STORE will not overwrite it."   /* PORT-SPEC App. A */
#define S_FAILED "Could not complete the Library action. Refresh and try again."
#define S_JOB_FINISHED "This job has finished or can no longer be cancelled."
#define S_JOB_UNKNOWN "Cannot confirm the current storage job. Refresh storage before trying again."
#define S_JOB_STATUS "Cannot confirm storage job status. Reconnect ShadowMount; the job may still be running."
#define S_NO_DRIVES "No drives reported."
#define S_INTERNAL "Internal storage"

/* D3: no mount, unmount or measure (list_games) in option B. */
static const char *const LIB_CAPS[] = {"rescan", "copy_game_source", "move_game_source",
                                       "storage_job_status", "storage_job_cancel", "storage_space"};

/* ---- state ------------------------------------------------------------------ */

typedef struct lib_row {
  char titleId[16], platform[8], format[16], sourceKey[65], location[64];
  char root[256];          /* the scan root of a file row; "" for an installed-only row */
  char *path, *title;      /* heap */
  int64_t size;            /* -1 = unknown */
  unsigned installed : 1, onDrive : 1, managed : 1, canManage : 1, carried : 1, file : 1;
} lib_row;

typedef struct lib_action {
  int has;
  char id[72], action[12], titleId[16], state[12], message[256];
} lib_action;

typedef struct lib_job {
  int has;
  char xs_id[16];
  int64_t alias;           /* storageJob.id: a positive integer for the XS job id */
  int move;
  char titleId[16];
  char src[1024], dst[1024];
  char state[12];
  char error[256];
  int64_t total, processed, speed;
  int active, cancellable, cancel_requested;
} lib_job;

typedef struct lib_cmd {
  char action[12], titleId[16], sourceKey[72], destinationId[72];
  int64_t jobId;
} lib_cmd;

/* Shared with the handlers, under tvlib_mu (a leaf lock). */
static pthread_mutex_t tvlib_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_started;                       /* the worker runs */
static char g_status[16] = "idle", g_message[256];
static int g_busy, g_pending, g_forced, g_wake;
static time_t g_checked_at, g_updated_at;   /* wall clock (Unix s) */
static int64_t g_attempted_ms, g_refreshed_ms;   /* monotonic */
static char *g_games;                       /* the rendered games[] array, once per walk */
static size_t g_games_len;
static lib_action g_action;
static lib_job g_job;
static char g_storage_error[256];
static int64_t g_next_alias = 1;
static int g_cmd_pending;
static lib_cmd g_cmd;
static char g_last_req[72], g_last_body[65];   /* last accepted requestId + sha256 of its raw body */
static int64_t g_storage_kick_ms;

/* Worker-only state (no lock: only the tvlib thread touches it). */
static lib_row *w_rows;
static int w_nrows;
static int w_walked;                        /* a walk succeeded at least once */
static uint64_t w_dir_sig, w_probe_sig;
static int w_sig_valid, w_sig_unstable;
static char **w_probe;                      /* basenames that did not resolve through the catalogue */
static int w_nprobe;
static int w_want, w_want_forced;           /* a re-check is wanted once the 5 s floor allows it */
static int64_t w_followup_ms;
static int w_last_done = -1;
static int64_t w_last_rev = -1;
static unsigned w_last_gen;
static int w_have_gen;
static int64_t w_walks;

/* ---- small helpers ---------------------------------------------------------- */

static uint64_t fnv(uint64_t h, const void *data, size_t n) {
  const unsigned char *p = data;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
  return h;
}
#define FNV0 1469598103934665603ULL

/* sha256_hex("<a>\n<b>[\n<c>]"): the reference identity() (REF/library.c:128-134). */
static void identity(const char *a, const char *b, const char *c, char out[65]) {
  sha256_ctx ctx;
  uint8_t d[32];
  sha256_init(&ctx);
  sha256_update(&ctx, a, strlen(a));
  sha256_update(&ctx, "\n", 1);
  sha256_update(&ctx, b, strlen(b));
  if (c) {
    sha256_update(&ctx, "\n", 1);
    sha256_update(&ctx, c, strlen(c));
  }
  sha256_final(&ctx, d);
  sha256_digest_hex(d, out);
}

/* The drive and destination ids of /library/storage (map-library 5.2). */
static void drive_id(const char *root, char out[65]) { identity("drive", root, NULL, out); }
static void dest_path(const char *root, char *out, size_t cap) { xsnprintf(out, cap, "%s/homebrew", root); }
static void dest_id(const char *root, char out[65]) {
  char p[320];
  dest_path(root, p, sizeof p);
  identity("dest", p, NULL, out);
}

/* Ported from Orbit Store backend/library.c (GPL-3.0-or-later): title_id(). */
static int title_id_ok(const char *id) {
  if (!id || strlen(id) != 9) return 0;
  for (int i = 0; i < 9; i++) {
    char ch = id[i];
    if (i < 4 ? !(ch >= 'A' && ch <= 'Z') : !(ch >= '0' && ch <= '9')) return 0;
  }
  return 1;
}

static int utf8_ok(const char *s) {
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    unsigned c = *p;
    int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (n < 0 || (n == 1 && c < 0xC2) || (n == 3 && c > 0xF4)) return 0;
    p++;
    for (int i = 0; i < n; i++, p++) if ((*p & 0xC0) != 0x80) return 0;
  }
  return 1;
}

static void strip_slashes(const char *in, char *out, size_t cap) {
  xstrlcpy(out, in ? in : "", cap);
  size_t n = strlen(out);
  while (n > 1 && out[n - 1] == '/') out[--n] = 0;
}

static int same_root(const char *a, const char *b) {
  char x[512], y[512];
  strip_slashes(a, x, sizeof x);
  strip_slashes(b, y, sizeof y);
  return strcmp(x, y) == 0;
}

/* The formats a Library row can have (D10: no partials, archives or others). */
static int lib_format_ok(const char *f) {
  static const char *const ok[] = {"exFAT", "FFPFSC", "FFPFS", "FFPKG", "PKG", "FPKG"};
  for (size_t i = 0; i < XS_ARRAY_LEN(ok); i++) if (strcmp(f, ok[i]) == 0) return 1;
  return 0;
}
/* D4/D5: images can be copied or moved; packages cannot (REF/library.c:266-267). */
static int image_format(const char *f) {
  return strcmp(f, "exFAT") == 0 || strcmp(f, "FFPFSC") == 0 || strcmp(f, "FFPFS") == 0 || strcmp(f, "FFPKG") == 0;
}

/* platform: the catalogue's value when it is PS5/PS4, else the title ID prefix. */
static void platform_for(const char *catalog_platform, const char *tid, char *out, size_t cap) {
  if (catalog_platform && util_strieq(catalog_platform, "ps5")) xstrlcpy(out, "ps5", cap);
  else if (catalog_platform && util_strieq(catalog_platform, "ps4")) xstrlcpy(out, "ps4", cap);
  else if (util_starts_with(tid, "PPSA")) xstrlcpy(out, "ps5", cap);
  else if (util_starts_with(tid, "CUSA")) xstrlcpy(out, "ps4", cap);
  else xstrlcpy(out, "unknown", cap);
}

/* titleName of a param.json (localizedParameters.en-US first), "" when none. */
static void param_title(const char *path, char *out, size_t cap) {
  out[0] = 0;
  jdoc *pj = cx_json_file(path, 1 << 16, 2000);
  if (!pj) return;
  jv lt = jv_get(json_root(pj), "localizedParameters");
  const char *t = jv_get_str(jv_get(lt, "en-US"), "titleName", "");
  if (!*t) t = jv_get_str(json_root(pj), "titleName", "");
  if (utf8_ok(t)) xstrlcpy(out, t, cap);
  json_free(pj);
}

static void appmeta_title(const char *tid, const char *install_dir, char *out, size_t cap) {
  char p[1200];
  xsnprintf(p, sizeof p, "%s/%s/param.json", cx_appmeta_root(), tid);
  param_title(p, out, cap);
  if (!out[0] && install_dir && *install_dir) {
    xsnprintf(p, sizeof p, "%s/sce_sys/param.json", install_dir);
    param_title(p, out, cap);
  }
}

static int64_t now_ms(void) { return util_now_ms(); }

static void wake_locked(void) { g_wake = 1; }

/* ---- snapshot writers (memory only) ------------------------------------------ */

static void write_job_locked(jw *w) {
  jw_obj_begin(w);
  jw_kv_i64(w, "id", g_job.alias);
  jw_kv_str(w, "operation", g_job.move ? "move" : "copy");
  jw_kv_str(w, "state", g_job.state);
  jw_kv_str(w, "titleId", g_job.titleId);
  jw_kv_str(w, "source", g_job.src);
  jw_kv_str(w, "destination", g_job.dst);
  jw_kv_str(w, "error", g_job.error);
  jw_kv_i64(w, "result", 0);
  jw_kv_i64(w, "totalBytes", g_job.total > 0 ? g_job.total : 0);
  jw_kv_i64(w, "processedBytes", g_job.processed > 0 ? g_job.processed : 0);
  jw_kv_i64(w, "speed", g_job.speed > 0 ? g_job.speed : 0);
  jw_kv_bool(w, "active", g_job.active);
  jw_kv_bool(w, "cancellable", g_job.cancellable);
  jw_kv_bool(w, "cancelRequested", g_job.cancel_requested);
  jw_obj_end(w);
}

/* The GET /api/v1/library members (REF/library.c:770-786). */
static void write_library(jw *w) {
  pthread_mutex_lock(&tvlib_mu);
  int64_t now = now_ms();
  int ready = strcmp(g_status, "ready") == 0;
  jw_kv_str(w, "status", g_started ? g_status : "unavailable");
  jw_kv_str(w, "message", g_started ? g_message : S_UNAVAILABLE_RESTART);
  jw_kv_str(w, "provider", XENO_APP_NAME);
  jw_kv_str(w, "providerVersion", XENO_VERSION);
  jw_kv_bool(w, "busy", g_busy || g_pending);
  jw_kv_bool(w, "stale", g_updated_at != 0 && (!ready || now - g_refreshed_ms >= LIB_STALE_MS));
  jw_kv_i64(w, "checkedAt", (int64_t)g_checked_at);
  jw_kv_i64(w, "updatedAt", (int64_t)g_updated_at);
  int64_t since = now - g_attempted_ms;
  jw_kv_i64(w, "refreshAfter", g_attempted_ms && since < LIB_POST_COOLDOWN_MS ? (LIB_POST_COOLDOWN_MS - since + 999) / 1000 : 0);
  if (g_games) jw_kv_raw(w, "games", g_games, g_games_len);
  else { jw_key(w, "games"); jw_arr_begin(w); jw_arr_end(w); }
  jw_key(w, "capabilities");
  jw_arr_begin(w);
  if (g_started) for (size_t i = 0; i < XS_ARRAY_LEN(LIB_CAPS); i++) jw_str(w, LIB_CAPS[i]);
  jw_arr_end(w);
  if (g_action.has) {
    jw_key(w, "action");
    jw_obj_begin(w);
    jw_kv_str(w, "id", g_action.id);
    jw_kv_str(w, "action", g_action.action);
    jw_kv_str(w, "titleId", g_action.titleId);
    jw_kv_str(w, "state", g_action.state);
    jw_kv_str(w, "message", g_action.message);
    jw_obj_end(w);
  } else {
    jw_kv_null(w, "action");
  }
  if (g_job.has) { jw_key(w, "storageJob"); write_job_locked(w); }
  else jw_kv_null(w, "storageJob");
  jw_kv_str(w, "storageError", g_storage_error);
  jw_kv_bool(w, "storageBusy", g_job.has && g_job.active);
  pthread_mutex_unlock(&tvlib_mu);
}

/* The library cooldown check (REF/library.c:760-768): schedules a background
 * re-check when the last attempt is old enough; never waits for it. */
static void schedule_check(int64_t cooldown_ms, int forced) {
  int stopping = tv_stopping();
  pthread_mutex_lock(&tvlib_mu);
  int64_t now = now_ms();
  if (g_started && !stopping && !g_busy && !g_pending && (!g_attempted_ms || now - g_attempted_ms >= cooldown_ms)) {
    g_pending = 1;
    if (forced) g_forced = 1;
    g_attempted_ms = now;
    wake_locked();
  }
  pthread_mutex_unlock(&tvlib_mu);
}

static void write_drive(jw *w, const tv_drive *d, int destination) {
  char id[65], path[320];
  if (destination) { dest_id(d->path, id); dest_path(d->path, path, sizeof path); }
  else { drive_id(d->path, id); xstrlcpy(path, d->path, sizeof path); }
  int64_t total = d->total > 0 ? d->total : 0, fr = d->free > 0 ? d->free : 0;
  jw_obj_begin(w);
  jw_kv_str(w, "id", id);
  jw_kv_str(w, "path", path);
  jw_kv_str(w, "label", d->label);
  jw_kv_str(w, "mountPoint", d->path);
  jw_kv_str(w, "filesystem", "");
  jw_kv_bool(w, "readOnly", destination ? 0 : !d->writable);
  jw_kv_i64(w, "totalBytes", total);
  jw_kv_i64(w, "freeBytes", fr);
  jw_kv_i64(w, "usedBytes", total > fr ? total - fr : 0);
  jw_obj_end(w);
}

/* ---- handlers ------------------------------------------------------------------ */

void tvlib_h_library(api_ctx *c) {
  int post = tv_is_post(c);
  schedule_check(post ? LIB_POST_COOLDOWN_MS : LIB_GET_COOLDOWN_MS, post);
  jw w;
  tv_obj_begin(&w);
  write_library(&w);
  tv_obj_send(c, 200, &w);
}

void tvlib_h_storage(api_ctx *c) {
  /* Like the reference, a storage request also runs the library cooldown check. */
  schedule_check(LIB_GET_COOLDOWN_MS, 0);
  int kicked = 0;
  if (tv_is_post(c)) {
    pthread_mutex_lock(&tvlib_mu);
    int64_t now = now_ms();
    if (!g_storage_kick_ms || now - g_storage_kick_ms >= STORAGE_KICK_MS) { g_storage_kick_ms = now; kicked = 1; }
    pthread_mutex_unlock(&tvlib_mu);
    if (kicked) tvdl_storage_kick(1);   /* a flag for the tv-storage thread; never waits */
  }
  /* about 5 KB: on the stack, so this route never fails for lack of memory
   * (the app ignores its errors; problems go in "error" and "stale") */
  tv_drive d[TVDL_MAX_DRIVES];
  tvdl_snap_info info;
  memset(&info, 0, sizeof info);
  int n = tvdl_snapshot(d, TVDL_MAX_DRIVES, 0, &info);
  int64_t now = now_ms();
  jw w;
  tv_obj_begin(&w);
  jw_key(&w, "drives");
  jw_arr_begin(&w);
  for (int i = 0; i < n; i++) write_drive(&w, &d[i], 0);
  jw_arr_end(&w);
  jw_key(&w, "destinations");
  jw_arr_begin(&w);
  for (int i = 0; i < n; i++) if (d[i].writable) write_drive(&w, &d[i], 1);
  jw_arr_end(&w);
  jw_kv_bool(&w, "busy", info.kick_pending || kicked);
  jw_kv_bool(&w, "stale", info.updated_ms == 0 || now - info.updated_ms >= STORAGE_STALE_MS);
  jw_kv_i64(&w, "updatedAt", (int64_t)info.updated_at);
  jw_kv_str(&w, "error", info.count == 0 ? S_NO_DRIVES : "");
  tv_obj_send(c, 200, &w);
}

/* Ported from Orbit Store backend/library.c (GPL-3.0-or-later): text_field(),
 * a JSON string of at most `max` bytes without control characters. */
static int text_field(const api_ctx *c, const char *key, size_t max, const char **out) {
  const char *s = NULL;
  if (!tv_body_str(c, key, &s) || !s) return 0;
  size_t n = strlen(s);
  if (n > max) return 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) if (*p < 32 || *p == 127) return 0;
  if (out) *out = s;
  return 1;
}

/* Ported from Orbit Store backend/library.c (GPL-3.0-or-later): capability(). */
static const char *capability(const char *action) {
  return !strcmp(action, "scan") ? "rescan" : !strcmp(action, "mount") ? "mount_game"
       : !strcmp(action, "unmount") ? "unmount_game" : !strcmp(action, "copy") ? "copy_game_source"
       : !strcmp(action, "move") ? "move_game_source" : !strcmp(action, "cancel") ? "storage_job_cancel"
       : !strcmp(action, "measure") ? "list_games" : "";
}

static int has_capability(const char *cap) {
  for (size_t i = 0; i < XS_ARRAY_LEN(LIB_CAPS); i++) if (strcmp(LIB_CAPS[i], cap) == 0) return 1;
  return 0;
}

/* POST /api/v1/library/actions. Ported from Orbit Store backend/library.c
 * (GPL-3.0-or-later): library_action(), the checks, their order and their
 * sentences, with strict JSON types (a string is a string, true is true). */
void tvlib_h_actions(api_ctx *c) {
  const char *action = "", *request = "", *tid = "", *skey = "", *dest = "";
  if (!tv_body_str(c, "action", &action) || !action) action = "";
  int titled = strcmp(action, "scan") && strcmp(action, "measure") && strcmp(action, "cancel");
  int transfer = !strcmp(action, "copy") || !strcmp(action, "move");
  int64_t job_id = 0;
  int ok = *capability(action) && text_field(c, "requestId", 64, &request) && strlen(request) >= 8;
  if (ok && titled) {
    if (!tv_body_str(c, "titleId", &tid) || !tid || !title_id_ok(tid)) ok = 0;
    else if (!text_field(c, "sourceKey", 64, &skey) || strlen(skey) != 64) ok = 0;
  }
  if (ok && transfer && (!text_field(c, "destinationId", 64, &dest) || strlen(dest) != 64)) ok = 0;
  if (ok && !strcmp(action, "cancel") &&
      (!tv_body_int(c, "jobId", &job_id) || job_id <= 0 || job_id > 9007199254740991LL)) ok = 0;
  if (!ok) { tv_error(c, 400, S_BAD_ACTION); return; }
  if (!strcmp(action, "scan") || !strcmp(action, "move") || !strcmp(action, "unmount")) {
    int confirmed = 0;
    if (!tv_body_bool(c, "confirmed", &confirmed) || !confirmed) { tv_error(c, 400, S_CONFIRM); return; }
  }
  /* The replay check compares the raw body bytes (the app builds them
   * deterministically): a digest stands in for the stored copy. */
  char body_hash[65];
  sha256_hex(c->req->body ? c->req->body : "", c->req->body ? c->req->body_len : 0, body_hash);
  /* action.titleId: the game for mount/unmount/copy/move; for scan and
   * cancel (the app sends none) only a well-formed one is echoed. */
  if (!titled) {
    const char *t = NULL;
    tid = tv_body_str(c, "titleId", &t) && t && title_id_ok(t) ? t : "";
  }

  int stopping = tv_stopping();
  int code = 202, replay = 0;
  const char *sentence = NULL;
  pthread_mutex_lock(&tvlib_mu);
  if (!g_started || stopping) { code = 503; sentence = S_UNAVAILABLE; }
  else if (strcmp(request, g_last_req) == 0) {
    if (strcmp(body_hash, g_last_body) != 0) { code = 409; sentence = S_REQUEST_REUSED; }
    else replay = 1;   /* an exact replay of the accepted request: 202, nothing runs again */
  } else if (g_cmd_pending || (g_action.has && (!strcmp(g_action.state, "queued") || !strcmp(g_action.state, "running"))) ||
             (g_job.has && g_job.active && strcmp(action, "cancel") != 0)) {
    code = 409; sentence = S_IN_PROGRESS;
  } else if (strcmp(g_status, "ready") != 0 || !has_capability(capability(action))) {
    code = 409; sentence = S_INCOMPATIBLE;
  } else {
    memset(&g_cmd, 0, sizeof g_cmd);
    xstrlcpy(g_cmd.action, action, sizeof g_cmd.action);
    xstrlcpy(g_cmd.titleId, tid, sizeof g_cmd.titleId);
    xstrlcpy(g_cmd.sourceKey, skey, sizeof g_cmd.sourceKey);
    xstrlcpy(g_cmd.destinationId, dest, sizeof g_cmd.destinationId);
    g_cmd.jobId = job_id;
    g_cmd_pending = 1;
    xstrlcpy(g_last_req, request, sizeof g_last_req);
    xstrlcpy(g_last_body, body_hash, sizeof g_last_body);
    memset(&g_action, 0, sizeof g_action);
    g_action.has = 1;
    xstrlcpy(g_action.id, request, sizeof g_action.id);
    xstrlcpy(g_action.action, action, sizeof g_action.action);
    xstrlcpy(g_action.titleId, tid, sizeof g_action.titleId);
    xstrlcpy(g_action.state, "queued", sizeof g_action.state);
    xstrlcpy(g_action.message, S_WAITING, sizeof g_action.message);
    g_storage_error[0] = 0;   /* "until the next action" (map-library 6.6) */
    wake_locked();
  }
  pthread_mutex_unlock(&tvlib_mu);
  if (code != 202) { tv_error(c, code, sentence); return; }
  LOGI("tvlib: %s %s (%s)", action, replay ? "replayed, not run again" : "accepted", tid[0] ? tid : "no title");
  schedule_check(LIB_GET_COOLDOWN_MS, 0);
  jw w;
  tv_obj_begin(&w);
  write_library(&w);
  tv_obj_send(c, 202, &w);
}

/* ---- the scan (worker) ---------------------------------------------------------- */

static void rows_free(lib_row *rows, int n) {
  for (int i = 0; i < n; i++) { free(rows[i].path); free(rows[i].title); }
  free(rows);
}

typedef struct row_vec {
  lib_row *v;
  int n, cap;
} row_vec;

/* Takes ownership of r->path and r->title (freed on failure). The vector is
 * sized once and never moves: the walk's sets point into its rows. */
static int rows_push(row_vec *rv, lib_row *r) {
  if (rv->n >= rv->cap) { free(r->path); free(r->title); return -1; }
  rv->v[rv->n++] = *r;
  return 0;
}

static int row_cmp(const void *a, const void *b) {
  const lib_row *x = a, *y = b;
  int c = strcmp(x->title, y->title);
  if (c) return c;
  c = strcmp(x->path, y->path);
  if (c) return c;
  return strcmp(x->titleId, y->titleId);
}

/* A small open-addressing set of strings (paths, title ids, source keys). */
typedef struct str_set {
  const char **slot;
  size_t cap;
} str_set;

static int set_init(str_set *s, size_t want) {
  size_t cap = 64;
  while (cap < want * 2) cap <<= 1;
  s->slot = calloc(cap, sizeof *s->slot);
  s->cap = s->slot ? cap : 0;
  return s->slot ? 0 : -1;
}
static void set_free(str_set *s) { free(s->slot); s->slot = NULL; s->cap = 0; }
/* 1 when present; with add, inserts it (the string must outlive the set). */
static int set_has(str_set *s, const char *k, int add) {
  if (!s->cap) return 0;
  size_t i = (size_t)fnv(FNV0, k, strlen(k)) & (s->cap - 1);
  for (size_t probes = 0; probes < s->cap; probes++, i = (i + 1) & (s->cap - 1)) {
    if (!s->slot[i]) { if (add) s->slot[i] = k; return 0; }
    if (strcmp(s->slot[i], k) == 0) return 1;
  }
  return 0;
}

typedef struct walk_scratch {
  tvcat_rel rel;
  cat_game game;
  char clean[300], fa[600], fb[600];
} walk_scratch;

typedef struct resolved {
  char tid[16], platform[8], title[256];
  int step;   /* D15 step that resolved it (1..4) */
} resolved;

/* D15: finished job -> catalogue filename -> PPSA/CUSA in the name or path ->
 * exact catalogue title -> skip (0). The step also fixes the platform and,
 * when the catalogue knows the game, the title. */
static int resolve_file(jv f, const dl_brief *jobs, int njobs, walk_scratch *ws, resolved *out) {
  memset(out, 0, sizeof *out);
  const char *path = jv_get_str(f, "path", "");
  const char *name = jv_get_str(f, "name", util_basename(path));
  /* 1. the queue job that wrote this file */
  const char *jid = jv_get_str(f, "jobId", "");
  if (*jid) {
    for (int i = 0; i < njobs; i++) {
      const dl_brief *b = &jobs[i];
      if (strcmp(b->id, jid) != 0) continue;
      if (strcmp(b->type, "download") == 0 && b->releaseId[0] && tvcat_release(b->releaseId, &ws->rel) == 0 &&
          title_id_ok(ws->rel.titleId)) {
        xstrlcpy(out->tid, ws->rel.titleId, sizeof out->tid);
        platform_for(ws->rel.platform, out->tid, out->platform, sizeof out->platform);
        xstrlcpy(out->title, ws->rel.title, sizeof out->title);
        out->step = 1;
        return 1;
      }
      break;
    }
  }
  /* 2. the TV catalogue's filename index */
  if (tvcat_by_filename(name, &ws->rel) == 0 && title_id_ok(ws->rel.titleId)) {
    xstrlcpy(out->tid, ws->rel.titleId, sizeof out->tid);
    platform_for(ws->rel.platform, out->tid, out->platform, sizeof out->platform);
    xstrlcpy(out->title, ws->rel.title, sizeof out->title);
    out->step = 2;
    return 1;
  }
  /* 3. PPSA/CUSA in the file name, else the path (storage.c already looked) */
  const char *ft = jv_get_str(f, "titleId", "");
  if (cx_titleid_valid(ft)) {
    xstrlcpy(out->tid, ft, sizeof out->tid);
    if (catalog_match_titleid(ft, &ws->game) == 0) {
      xstrlcpy(out->title, ws->game.title, sizeof out->title);
      platform_for(ws->game.platform, out->tid, out->platform, sizeof out->platform);
    } else {
      platform_for(NULL, out->tid, out->platform, sizeof out->platform);
    }
    out->step = 3;
    return 1;
  }
  /* 4. the scan's title match, only when it is exact (catalog_match_title is fuzzy) */
  const char *gid = jv_get_str(f, "gameId", ""), *gtitle = jv_get_str(f, "title", "");
  if (*gid && *gtitle) {
    storage_title_from_name(name, ws->clean, sizeof ws->clean);
    util_fold(ws->clean, ws->fa, sizeof ws->fa);
    util_trim(ws->fa);
    util_fold(gtitle, ws->fb, sizeof ws->fb);
    util_trim(ws->fb);
    if (ws->fa[0] && strcmp(ws->fa, ws->fb) == 0 && catalog_find_game(gid, &ws->game) == 0) {
      for (int i = 0; i < ws->game.ntitleIds && i < CAT_MAX_TITLEIDS; i++) {
        char t[16];
        xstrlcpy(t, ws->game.titleIds[i], sizeof t);
        util_upper(t);   /* as the TV catalogue spells it (tv_release_title_id) */
        if (!cx_titleid_valid(t)) continue;
        xstrlcpy(out->tid, t, sizeof out->tid);
        xstrlcpy(out->title, ws->game.title, sizeof out->title);
        platform_for(ws->game.platform, out->tid, out->platform, sizeof out->platform);
        out->step = 4;
        return 1;
      }
    }
  }
  return 0;
}

static void probe_reset(void) {
  for (int i = 0; i < w_nprobe; i++) free(w_probe[i]);
  w_nprobe = 0;
}

static void probe_add(const char *name) {
  if (!w_probe) w_probe = calloc(PROBE_MAX, sizeof *w_probe);
  if (!w_probe || w_nprobe >= PROBE_MAX) return;
  char *s = util_strdup(name);
  if (s) w_probe[w_nprobe++] = s;
}

/* What the catalogue filename index says today about the files it did not
 * know at the last walk (the index may come up after the first walk). */
static uint64_t probe_hash(void) {
  static tvcat_rel rel;   /* worker only: kept off the stack */
  uint64_t h = FNV0;
  for (int i = 0; i < w_nprobe; i++) {
    int hit = tvcat_by_filename(w_probe[i], &rel) == 0 && title_id_ok(rel.titleId);
    h = fnv(h, &hit, sizeof hit);
    if (hit) h = fnv(h, rel.titleId, strlen(rel.titleId));
  }
  return h;
}

typedef struct sig_ctx {
  uint64_t h;
  int unstable;
  time_t now;
} sig_ctx;

static int sig_stat(sig_ctx *s, const char *path) {
  struct stat st;
  int ok = stat(path, &st) == 0;
  int64_t rec[6] = {ok, ok ? (int64_t)st.st_dev : 0, ok ? (int64_t)st.st_ino : 0, ok ? (int64_t)st.st_mtime : 0,
                    ok ? (int64_t)st.st_size : 0, ok ? (int64_t)st.st_nlink : 0};
  s->h = fnv(s->h, path, strlen(path));
  s->h = fnv(s->h, rec, sizeof rec);
  if (ok && st.st_mtime >= s->now - SIG_RECENT_S) s->unstable = 1;
  return ok && S_ISDIR(st.st_mode);
}

/* A scan folder and its first-level subfolders (the walk goes one level down). */
static void sig_scan_dir(sig_ctx *s, const char *dir) {
  if (!sig_stat(s, dir)) return;
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  int n = 0;
  char p[1100];
  while ((e = readdir(d)) != NULL && n < SIG_DIR_MAX) {
    if (e->d_name[0] == '.') continue;
    n++;
    if (xsnprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p - 1) continue;
    struct stat st;
    if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    sig_stat(s, p);
  }
  closedir(d);
}

/* The cheap signature of everything a walk depends on, except the probe part. */
static uint64_t dir_signature(int *unstable) {
  sig_ctx s = {FNV0, 0, util_now()};
  tv_drive *d = calloc(TVDL_MAX_DRIVES, sizeof *d);
  int n = d ? tvdl_snapshot(d, TVDL_MAX_DRIVES, 0, NULL) : 0;
  static const char *const subs[] = {"homebrew", "xeno-store/pkg", "xeno-store/files"};
  for (int i = 0; i < n; i++) {
    s.h = fnv(s.h, d[i].path, strlen(d[i].path) + 1);
    for (size_t k = 0; k < XS_ARRAY_LEN(subs); k++) {
      char dir[600];
      xsnprintf(dir, sizeof dir, "%s/%s", d[i].path, subs[k]);
      sig_scan_dir(&s, dir);
    }
  }
  free(d);
  sig_stat(&s, storage_user_app_dir());
  int64_t rev = catalog_revision();
  s.h = fnv(s.h, &rev, sizeof rev);
  dl_summary sm;
  dl_summary_get(&sm);
  s.h = fnv(s.h, &sm.done, sizeof sm.done);
  *unstable = s.unstable;
  return s.h;
}

static int brief_unfinished(const dl_brief *b) {
  return b->state == DL_QUEUED || b->state == DL_CONNECTING || b->state == DL_DOWNLOADING || b->state == DL_VERIFYING ||
         b->state == DL_PAUSED || b->state == DL_EXTRACTING || b->running;
}

/* Renders the games[] array once per walk (handlers copy the text). */
static char *render_rows(const lib_row *rows, int n, size_t *len) {
  jw w;
  jw_init(&w);
  jw_arr_begin(&w);
  for (int i = 0; i < n; i++) {
    const lib_row *r = &rows[i];
    jw_obj_begin(&w);
    jw_kv_str(&w, "titleId", r->titleId);
    jw_kv_str(&w, "title", r->title);
    jw_kv_str(&w, "platform", r->platform);
    jw_kv_str(&w, "format", r->format);
    jw_kv_str(&w, "path", r->path);
    jw_kv_str(&w, "runtimePath", "");
    jw_kv_str(&w, "sourceKey", r->sourceKey);
    jw_kv_str(&w, "location", r->location);
    jw_kv_bool(&w, "installed", r->installed);
    jw_kv_bool(&w, "mounted", 0);
    jw_kv_bool(&w, "onDrive", r->onDrive);
    jw_kv_bool(&w, "managed", r->managed);
    jw_kv_bool(&w, "canManageSource", r->canManage);
    if (r->size >= 0) jw_kv_i64(&w, "sizeBytes", r->size);
    else jw_kv_null(&w, "sizeBytes");
    jw_kv_str(&w, "sizeStatus", r->size >= 0 ? "ready" : "unavailable");
    jw_kv_null(&w, "installedSizeBytes");
    jw_obj_end(&w);
  }
  jw_arr_end(&w);
  return jw_detach(&w, len);
}

static void walk_failed(time_t wall) {
  pthread_mutex_lock(&tvlib_mu);
  xstrlcpy(g_status, "error", sizeof g_status);
  xstrlcpy(g_message, S_UNAVAILABLE, sizeof g_message);
  g_checked_at = wall;
  g_busy = 0;
  pthread_mutex_unlock(&tvlib_mu);
  w_sig_valid = 0;
}

/* Builds the rows from one storage_library_json() walk (map-library 3.3, 3.4). */
/* A root storage.c still lists can already be gone (a drive pulled, a folder
 * renamed) when a walk runs before the next drive scan. It must count as gone
 * then, or its games drop out of the Library instead of staying listed as
 * unavailable (3.4), and are lost for good once the scan catches up. The same
 * test as storage.c: a directory, and under /mnt a mounted one. */
static int root_alive(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
#ifndef XENO_HOST
  struct stat mnt;
  if (util_starts_with(path, "/mnt/") && stat("/mnt", &mnt) == 0 && st.st_dev == mnt.st_dev) return 0;
#endif
  return 1;
}

static int build_rows(jv root, const dl_brief *jobs, int njobs, row_vec *out, int *skipped_out) {
  walk_scratch *ws = calloc(1, sizeof *ws);
  str_set paths = {0}, file_tids = {0}, installed = {0}, keys = {0}, roots = {0};
  storage_root *peek = calloc(STORAGE_MAX_ROOTS, sizeof *peek);
  char (*peek_paths)[512] = calloc(STORAGE_MAX_ROOTS, sizeof *peek_paths);
  int rc = -1, skipped = 0;
  jv files = jv_get(root, "files"), inst = jv_get(root, "installed");
  size_t nf = jv_len(files), ni = jv_len(inst);
  size_t cap = nf + ni + (size_t)w_nrows + 1;
  out->v = calloc(cap, sizeof *out->v);
  out->cap = out->v ? (int)cap : 0;
  if (!out->v || !ws || !peek || !peek_paths || set_init(&paths, nf + 1) || set_init(&file_tids, nf + ni + 1) ||
      set_init(&installed, ni + 1) || set_init(&keys, nf + ni + (size_t)w_nrows + 1) ||
      set_init(&roots, STORAGE_MAX_ROOTS + nf + 1))
    goto done;
  probe_reset();
  JV_FOREACH(e, inst) {
    const char *t = jv_get_str(e, "titleId", "");
    if (cx_titleid_valid(t)) set_has(&installed, t, 1);
  }
  /* the roots the walk looked at: storage.c's roots plus every root that listed a file */
  int npeek = storage_peek_roots(peek, STORAGE_MAX_ROOTS);
  for (int i = 0; i < npeek; i++) {
    strip_slashes(peek[i].path, peek_paths[i], sizeof peek_paths[i]);
    if (root_alive(peek_paths[i])) set_has(&roots, peek_paths[i], 1);
  }

  /* 3.3: file rows */
  JV_FOREACH(f, files) {
    const char *rpath = jv_get_str(f, "root", "");
    if (*rpath) set_has(&roots, rpath, 1);
    if (jv_get_bool(f, "partial", 0)) continue;
    const char *format = jv_get_str(f, "format", "");
    const char *path = jv_get_str(f, "path", "");
    if (!lib_format_ok(format) || path[0] != '/' || !utf8_ok(path)) continue;
    if (set_has(&paths, path, 1)) continue;
    resolved res;
    if (!resolve_file(f, jobs, njobs, ws, &res)) {
      skipped++;
      probe_add(jv_get_str(f, "name", util_basename(path)));
      if (skipped == 1) LOGD("tvlib: no title ID for %s (not listed)", path);
      continue;
    }
    if (res.step >= 3) probe_add(jv_get_str(f, "name", util_basename(path)));
    if (strcmp(res.tid, APP_TITLE_ID) == 0) continue;   /* D9: never our own app */
    lib_row r;
    memset(&r, 0, sizeof r);
    xstrlcpy(r.titleId, res.tid, sizeof r.titleId);
    xstrlcpy(r.platform, res.platform, sizeof r.platform);
    xstrlcpy(r.format, format, sizeof r.format);
    xstrlcpy(r.root, rpath, sizeof r.root);
    identity(r.titleId, path, r.format, r.sourceKey);
    tvdl_location_for_path(path, r.location, sizeof r.location);
    char title[256];
    xstrlcpy(title, res.title, sizeof title);
    if (!title[0] || !utf8_ok(title)) appmeta_title(r.titleId, NULL, title, sizeof title);
    if (!title[0]) {
      storage_title_from_name(jv_get_str(f, "name", util_basename(path)), title, sizeof title);
      if (!utf8_ok(title)) title[0] = 0;
    }
    if (!title[0]) xstrlcpy(title, r.titleId, sizeof title);
    r.path = util_strdup(path);
    r.title = util_strdup(title);
    if (!r.path || !r.title) { free(r.path); free(r.title); goto done; }
    r.size = jv_get_i64(f, "size", -1);
    if (r.size < 0) r.size = -1;
    r.installed = set_has(&installed, r.titleId, 0);
    r.onDrive = 1;
    r.managed = r.canManage = image_format(r.format);
    r.file = 1;
    if (rows_push(out, &r)) goto done;
    set_has(&file_tids, out->v[out->n - 1].titleId, 1);
    set_has(&keys, out->v[out->n - 1].sourceKey, 1);
  }

  /* 3.4: titles installed on the console with no source file */
  JV_FOREACH(e, inst) {
    const char *t = jv_get_str(e, "titleId", "");
    const char *path = jv_get_str(e, "path", "");
    if (!cx_titleid_valid(t) || strcmp(t, APP_TITLE_ID) == 0 || !*path || !utf8_ok(path)) continue;
    if (set_has(&file_tids, t, 0)) continue;
    lib_row r;
    memset(&r, 0, sizeof r);
    xstrlcpy(r.titleId, t, sizeof r.titleId);
    identity(r.titleId, path, "", r.sourceKey);
    if (set_has(&keys, r.sourceKey, 0)) continue;
    platform_for(jv_get_str(e, "platform", ""), t, r.platform, sizeof r.platform);
    xstrlcpy(r.location, S_INTERNAL, sizeof r.location);
    char title[256];
    xstrlcpy(title, jv_get_str(e, "title", ""), sizeof title);
    if (!utf8_ok(title)) title[0] = 0;
    if (!title[0]) appmeta_title(t, path, title, sizeof title);
    if (!title[0]) xstrlcpy(title, t, sizeof title);
    r.path = util_strdup(path);
    r.title = util_strdup(title);
    if (!r.path || !r.title) { free(r.path); free(r.title); goto done; }
    r.size = -1;
    r.installed = 1;
    r.onDrive = 1;   /* D11: a playable install is not "Drive disconnected" */
    if (rows_push(out, &r)) goto done;
    set_has(&file_tids, out->v[out->n - 1].titleId, 1);
    set_has(&keys, out->v[out->n - 1].sourceKey, 1);
  }

  /* 3.4: sources on a drive that went away stay listed, unavailable */
  for (int i = 0; i < w_nrows; i++) {
    const lib_row *o = &w_rows[i];
    if (!o->file || !o->root[0]) continue;
    char rp[512];
    strip_slashes(o->root, rp, sizeof rp);
    if (set_has(&roots, rp, 0) || set_has(&roots, o->root, 0) || set_has(&keys, o->sourceKey, 0)) continue;
    lib_row r = *o;
    r.path = util_strdup(o->path);
    r.title = util_strdup(o->title);
    if (!r.path || !r.title) { free(r.path); free(r.title); goto done; }
    r.onDrive = 0;
    r.carried = 1;
    r.installed = set_has(&installed, r.titleId, 0);
    if (rows_push(out, &r)) goto done;
    set_has(&keys, out->v[out->n - 1].sourceKey, 1);
  }
  rc = 0;
done:
  set_free(&paths);
  set_free(&file_tids);
  set_free(&installed);
  set_free(&keys);
  set_free(&roots);
  free(peek);
  free(peek_paths);
  free(ws);
  *skipped_out = skipped;
  return rc;
}

/* One check (map-library 4): the signature, then a walk only when something
 * changed or the check is forced (a POST refresh, an action). */
static void check(int forced) {
  time_t wall = util_now();
  pthread_mutex_lock(&tvlib_mu);
  g_busy = 1;
  g_checked_at = wall;
  pthread_mutex_unlock(&tvlib_mu);

  xthread_note("tvlib: signature");
  int unstable = 0;
  uint64_t dsig = dir_signature(&unstable);
  uint64_t psig = probe_hash();
  if (!forced && w_walked && w_sig_valid && !w_sig_unstable && dsig == w_dir_sig && psig == w_probe_sig) {
    pthread_mutex_lock(&tvlib_mu);
    if (strcmp(g_status, "ready") == 0) { g_refreshed_ms = now_ms(); g_updated_at = wall; }
    g_busy = 0;
    pthread_mutex_unlock(&tvlib_mu);
    w_sig_unstable = unstable;
    LOGD("tvlib: nothing changed, no walk");
    return;
  }

  xthread_note("tvlib: walk");
  int64_t t0 = now_ms();
  jw w;
  jw_init(&w);
  jw_obj_begin(&w);
  storage_library_json(&w);   /* also rebuilds the copy allow-list of dl_add_copy_job */
  jw_obj_end(&w);
  size_t len = 0;
  char *text = jw_detach(&w, &len);
  jdoc *doc = text ? json_parse(text, len, 0, 0) : NULL;
  free(text);
  if (!doc) { LOGW("tvlib: the drive scan could not be read"); walk_failed(wall); return; }
  dl_brief *jobs = NULL;
  int njobs = dl_brief_list(&jobs);
  if (njobs < 0) njobs = 0;
  row_vec rv = {0};
  int skipped = 0;
  int built = build_rows(json_root(doc), jobs, njobs, &rv, &skipped);
  free(jobs);
  json_free(doc);
  char *games = NULL;
  size_t games_len = 0;
  if (built == 0) {
    if (rv.n > 1) qsort(rv.v, (size_t)rv.n, sizeof rv.v[0], row_cmp);
    games = render_rows(rv.v, rv.n, &games_len);
  }
  if (built != 0 || !games) {
    rows_free(rv.v, rv.n);
    LOGW("tvlib: out of memory while building the Library");
    walk_failed(wall);
    return;
  }
  int carried = 0;
  for (int i = 0; i < rv.n; i++) carried += rv.v[i].carried;
  rows_free(w_rows, w_nrows);
  w_rows = rv.v;
  w_nrows = rv.n;
  w_walked = 1;
  w_dir_sig = dsig;
  w_probe_sig = probe_hash();   /* the probe list changed with this walk */
  w_sig_valid = 1;
  w_sig_unstable = unstable;
  w_walks++;
  char *old;
  pthread_mutex_lock(&tvlib_mu);
  old = g_games;
  g_games = games;
  g_games_len = games_len;
  xstrlcpy(g_status, "ready", sizeof g_status);
  g_message[0] = 0;
  g_updated_at = wall;
  g_refreshed_ms = now_ms();
  g_busy = 0;
  pthread_mutex_unlock(&tvlib_mu);
  free(old);
  LOGI("tvlib: walked the drives (walk %lld%s): %d row(s), %d carried, %d file(s) without a title ID, %lld ms",
       (long long)w_walks, forced ? ", forced" : "", w_nrows, carried, skipped, (long long)(now_ms() - t0));
}

/* ---- the storage job (map-library 6.6) -------------------------------------------- */

/* Maps an XS copy job onto storageJob. A job that reached a terminal state
 * while its worker still runs (a cancel winding down) stays "running". */
static int job_apply_locked(const dl_brief *b) {
  const char *st;
  int terminal = 0;
  switch (b->state) {
    case DL_QUEUED: st = "queued"; break;
    case DL_PAUSED: st = "paused"; break;
    case DL_DONE: case DL_INSTALLED: case DL_EXTRACTED: st = "completed"; terminal = 1; break;
    case DL_FAILED: st = "failed"; terminal = 1; break;
    case DL_CANCELLED: st = "cancelled"; terminal = 1; break;
    default: st = "running"; break;
  }
  int winding = terminal && b->running;
  if (winding) st = "running";
  /* dl_action refuses a cancel while a worker still owns a job that is not
   * transferring (a pause or a cancel winding down) */
  int xs_busy = b->running && !(b->state == DL_CONNECTING || b->state == DL_DOWNLOADING || b->state == DL_VERIFYING ||
                                b->state == DL_EXTRACTING);
  int was_active = g_job.active;
  g_job.active = !terminal || winding;
  g_job.cancellable = g_job.active && !xs_busy;
  if (!g_job.active) g_job.cancel_requested = 0;
  xstrlcpy(g_job.state, st, sizeof g_job.state);
  xstrlcpy(g_job.error, b->error, sizeof g_job.error);
  if (b->srcPath[0]) xstrlcpy(g_job.src, b->srcPath, sizeof g_job.src);
  if (b->path[0]) xstrlcpy(g_job.dst, b->path, sizeof g_job.dst);
  g_job.move = b->move;
  g_job.total = b->total;
  g_job.processed = b->written;
  g_job.speed = b->speed > 0 ? (int64_t)b->speed : 0;
  return was_active && !g_job.active;   /* 1 = it just ended */
}

/* A web-UI copy (or one restored from queue.json) becomes the storage job
 * when no TV job is active, so working() covers it too. */
static void adopt(const dl_brief *jobs, int n) {
  pthread_mutex_lock(&tvlib_mu);
  int busy = g_job.has && g_job.active;
  char cur[16];
  xstrlcpy(cur, g_job.has ? g_job.xs_id : "", sizeof cur);
  pthread_mutex_unlock(&tvlib_mu);
  if (busy) return;
  const dl_brief *pick = NULL;
  for (int i = 0; i < n; i++) {
    const dl_brief *b = &jobs[i];
    if (strcmp(b->type, "copy") != 0 || b->removePending || !brief_unfinished(b)) continue;
    if (!pick || b->addedAt >= pick->addedAt) pick = b;
  }
  if (!pick) return;
  char tid[16] = "";
  for (int i = 0; i < w_nrows && !tid[0]; i++)
    if (w_rows[i].path && strcmp(w_rows[i].path, pick->srcPath) == 0) xstrlcpy(tid, w_rows[i].titleId, sizeof tid);
  if (!tid[0]) {
    static tvcat_rel rel;   /* worker only */
    if (tvcat_by_filename(util_basename(pick->srcPath), &rel) == 0 && title_id_ok(rel.titleId)) xstrlcpy(tid, rel.titleId, sizeof tid);
  }
  if (!tid[0] && util_find_titleid(pick->srcPath, tid, sizeof tid) != 0) tid[0] = 0;
  pthread_mutex_lock(&tvlib_mu);
  if (!(g_job.has && strcmp(cur, pick->id) == 0)) {
    int64_t alias = g_next_alias++;
    memset(&g_job, 0, sizeof g_job);
    g_job.has = 1;
    g_job.alias = alias;
    xstrlcpy(g_job.xs_id, pick->id, sizeof g_job.xs_id);
    g_storage_error[0] = 0;
  }
  xstrlcpy(g_job.titleId, tid, sizeof g_job.titleId);
  g_job.active = 1;
  job_apply_locked(pick);
  int64_t alias = g_job.alias;
  pthread_mutex_unlock(&tvlib_mu);
  LOGI("tvlib: following copy job %s as storage job %lld", pick->id, (long long)alias);
}

static void job_ended(const char *state) {
  w_want = 1;
  w_want_forced = 1;
  tvdl_storage_kick(0);
  LOGI("tvlib: storage job ended (%s)", state);
  if (strcmp(state, "completed") == 0 && !tv_stopping()) {
    /* XS copy jobs do not nudge ShadowMountPlus themselves (map-library 6.6) */
    settings_t *s = malloc(sizeof *s);
    int nudge = 0;
    if (s) { settings_get(s); nudge = s->shadowMountScan; free(s); }
    if (nudge) {
      char st[256];
      xthread_note("tvlib: ShadowMountPlus scan");
      install_shadowmount_scan(st, sizeof st);
      LOGI("tvlib: ShadowMountPlus after the copy: %s", st);
    }
  }
}

/* Follows the storage job (1 s while active, 5 s otherwise) and adopts one. */
static void job_tick(int adopt_too) {
  pthread_mutex_lock(&tvlib_mu);
  int has = g_job.has, active = g_job.active;
  char id[16];
  xstrlcpy(id, g_job.xs_id, sizeof id);
  pthread_mutex_unlock(&tvlib_mu);
  if (has && active) {
    dl_brief *b = malloc(sizeof *b);
    if (!b) return;
    int rc = dl_brief_get(id, b);
    int ended = 0;
    char st[12] = "";
    pthread_mutex_lock(&tvlib_mu);
    if (g_job.has && strcmp(g_job.xs_id, id) == 0) {
      if (rc != 0) {
        /* removed from the queue (web UI): the result is unknown */
        ended = g_job.active;
        g_job.active = g_job.cancellable = g_job.cancel_requested = 0;
        xstrlcpy(g_job.state, "failed", sizeof g_job.state);
        g_job.speed = 0;
        xstrlcpy(g_storage_error, S_JOB_STATUS, sizeof g_storage_error);
      } else {
        ended = job_apply_locked(b);
      }
      xstrlcpy(st, g_job.state, sizeof st);
    }
    pthread_mutex_unlock(&tvlib_mu);
    free(b);
    if (ended) job_ended(st);
    return;
  }
  if (!adopt_too) return;
  /* Adoption needs the whole queue: only when the summary shows unfinished work. */
  dl_summary sm;
  dl_summary_get(&sm);
  if (sm.active + sm.queued + sm.paused == 0) return;
  dl_brief *jobs = NULL;
  int n = dl_brief_list(&jobs);
  if (n > 0) adopt(jobs, n);
  free(jobs);
}

/* ---- actions (worker) ------------------------------------------------------------- */

static void action_set(const char *state, const char *message) {
  pthread_mutex_lock(&tvlib_mu);
  xstrlcpy(g_action.state, state, sizeof g_action.state);
  xstrlcpy(g_action.message, message, sizeof g_action.message);
  pthread_mutex_unlock(&tvlib_mu);
}

/* dl_action() with a small literal body. */
static int dl_verb(const char *id, const char *verb, const char *body) {
  jdoc *d = json_parse(body, strlen(body), 16, 4);
  if (!d) return -1;
  char code[32] = "", msg[256] = "";
  int rc = dl_action(id, verb, json_root(d), code, sizeof code, msg, sizeof msg);
  json_free(d);
  if (rc != 0) LOGW("tvlib: %s on %s: %s %s", verb, id, code, msg);
  return rc;
}

static void do_scan(char *ok, size_t cap) {
  tvdl_storage_kick(1);   /* new drives first */
  for (int i = 0; i < 50 && !g_cfg.stop_requested; i++) {
    tvdl_snap_info info;
    tvdl_snapshot(NULL, 0, 0, &info);
    if (!info.kick_pending) break;
    util_sleep_ms(TICK_MS);
  }
  check(1);
  char st[256] = "";
  xthread_note("tvlib: ShadowMountPlus scan");
  install_shadowmount_scan(st, sizeof st);
  LOGI("tvlib: scan: %s", st);
  xstrlcpy(ok, util_starts_with(st, "HTTP 2") ? S_SCAN_REQUESTED : S_UPDATED, cap);
  w_followup_ms = now_ms() + SCAN_FOLLOWUP_MS;
}

static const char *copy_error(const char *code) {
  if (!strcmp(code, "no_space")) return S_NO_SPACE;
  if (!strcmp(code, "same_drive")) return S_DEST_INSIDE;
  if (!strcmp(code, "root_unavailable")) return S_DEST_CHANGED;
  if (!strcmp(code, "forbidden") || !strcmp(code, "not_found") || !strcmp(code, "bad_request")) return S_SOURCE_CHANGED;
  if (!strcmp(code, "exists")) return S_EXISTS;
  return S_FAILED;
}

/* copy / move (map-library 6.4, steps a-j). */
static const char *do_transfer(const lib_cmd *cmd, char *ok, size_t cap) {
  int move = strcmp(cmd->action, "move") == 0;
  check(1);   /* a: the current files, and the copy allow-list rebuilt */
  const lib_row *row = NULL;
  for (int i = 0; i < w_nrows; i++)
    if (strcmp(w_rows[i].titleId, cmd->titleId) == 0 && strcmp(w_rows[i].sourceKey, cmd->sourceKey) == 0) { row = &w_rows[i]; break; }
  if (!row) return S_SOURCE_CHANGED;
  if (!row->managed || !row->onDrive || !row->canManage || !row->file) return S_SOURCE_UNSUPPORTED;   /* b */
  char path[1024], src_root[256], tid[16];
  xstrlcpy(path, row->path, sizeof path);
  xstrlcpy(src_root, row->root, sizeof src_root);
  xstrlcpy(tid, row->titleId, sizeof tid);
  /* c (D7) and d: Ported from Orbit Store backend/library.c (GPL-3.0-or-later):
   * no copy or move while a download is queued or active. */
  dl_brief *jobs = NULL;
  int n = dl_brief_list(&jobs);
  if (n < 0) return S_FAILED;
  int downloading = 0, copying = 0;
  for (int i = 0; i < n; i++) {
    const dl_brief *b = &jobs[i];
    if (b->removePending) continue;
    if (strcmp(b->type, "download") == 0 && !b->synthetic &&
        (b->state == DL_QUEUED || b->state == DL_CONNECTING || b->state == DL_DOWNLOADING || b->state == DL_VERIFYING ||
         b->state == DL_EXTRACTING))
      downloading = 1;
    if (strcmp(b->type, "copy") == 0 && brief_unfinished(b)) copying = 1;
  }
  free(jobs);
  if (downloading) return S_PAUSE_DOWNLOADS;
  if (copying) return S_IN_PROGRESS;
  /* e: a writable drive whose destination id is the one the app chose */
  tv_drive *d = calloc(TVDL_MAX_DRIVES, sizeof *d);
  if (!d) return S_FAILED;
  int nd = tvdl_snapshot(d, TVDL_MAX_DRIVES, 1, NULL);
  int hit = -1;
  for (int i = 0; i < nd && hit < 0; i++) {
    char id[65];
    dest_id(d[i].path, id);
    if (strcmp(id, cmd->destinationId) == 0) hit = i;
  }
  char dest_root[256];
  if (hit >= 0) xstrlcpy(dest_root, d[hit].path, sizeof dest_root);
  free(d);
  if (hit < 0) return S_DEST_CHANGED;
  if (same_root(src_root, dest_root)) return S_DEST_INSIDE;   /* f */
  if (tv_stopping()) return S_UNAVAILABLE;
  /* g: an XS copy job (never overwrite: that deletes the old file) */
  char id[16] = "", code[32] = "", msg[256] = "";
  if (dl_add_copy_job(path, dest_root, move, 0, id, sizeof id, code, sizeof code, msg, sizeof msg) != 0) {
    LOGI("tvlib: %s %s refused: %s (%s)", move ? "move" : "copy", path, code, msg);
    return copy_error(code);
  }
  /* h: start it now, first in the queue */
  dl_verb(id, "startNow", "{}");
  dl_verb(id, "move", "{\"direction\":\"top\"}");
  /* i */
  dl_brief *b = malloc(sizeof *b);
  int have = b && dl_brief_get(id, b) == 0;
  pthread_mutex_lock(&tvlib_mu);
  int64_t alias = g_next_alias++;
  memset(&g_job, 0, sizeof g_job);
  g_job.has = 1;
  g_job.alias = alias;
  xstrlcpy(g_job.xs_id, id, sizeof g_job.xs_id);
  xstrlcpy(g_job.titleId, tid, sizeof g_job.titleId);
  xstrlcpy(g_job.src, path, sizeof g_job.src);
  g_job.move = move;
  g_job.active = 1;
  g_job.cancellable = 1;
  xstrlcpy(g_job.state, "queued", sizeof g_job.state);
  if (have) job_apply_locked(b);
  g_storage_error[0] = 0;
  pthread_mutex_unlock(&tvlib_mu);
  free(b);
  LOGI("tvlib: %s %s -> %s as job %s (storage job %lld)", move ? "move" : "copy", path, dest_root, id, (long long)alias);
  xstrlcpy(ok, S_STARTED, cap);   /* j */
  return NULL;
}

/* cancel (map-library 6.5). */
static const char *do_cancel(const lib_cmd *cmd, char *ok, size_t cap) {
  pthread_mutex_lock(&tvlib_mu);
  int has = g_job.has;
  char id[16];
  xstrlcpy(id, g_job.xs_id, sizeof id);
  pthread_mutex_unlock(&tvlib_mu);
  if (!has) return S_JOB_FINISHED;
  dl_brief *b = malloc(sizeof *b);
  if (!b) return S_FAILED;
  if (dl_brief_get(id, b) != 0) { free(b); return S_JOB_UNKNOWN; }
  pthread_mutex_lock(&tvlib_mu);
  int ended = job_apply_locked(b);
  int can = g_job.active && g_job.cancellable && g_job.alias == cmd->jobId;
  char st[12];
  xstrlcpy(st, g_job.state, sizeof st);
  pthread_mutex_unlock(&tvlib_mu);
  free(b);
  if (ended) job_ended(st);
  if (!can) return S_JOB_FINISHED;
  if (tv_stopping()) return S_UNAVAILABLE;
  if (dl_verb(id, "cancel", "{\"deleteFiles\":true}") != 0) return S_JOB_FINISHED;
  pthread_mutex_lock(&tvlib_mu);
  g_job.cancel_requested = 1;
  pthread_mutex_unlock(&tvlib_mu);
  job_tick(0);   /* show the new state at once */
  LOGI("tvlib: cancel requested for job %s", id);
  xstrlcpy(ok, S_CANCEL_REQUESTED, cap);
  return NULL;
}

/* The common frame of every command (REF/library.c:474-639). */
static void execute(const lib_cmd *cmd) {
  action_set("running", S_CHECKING);
  char ok[256] = "";
  const char *err = NULL;
  if (tv_stopping()) err = S_UNAVAILABLE;
  else if (!strcmp(cmd->action, "scan")) do_scan(ok, sizeof ok);
  else if (!strcmp(cmd->action, "copy") || !strcmp(cmd->action, "move")) err = do_transfer(cmd, ok, sizeof ok);
  else if (!strcmp(cmd->action, "cancel")) err = do_cancel(cmd, ok, sizeof ok);
  else err = S_INCOMPATIBLE;   /* mount, unmount, measure: refused before (no capability) */
  if (err) {
    action_set("error", err);
    LOGI("tvlib: %s ended: %s", cmd->action, err);
  } else {
    action_set("complete", ok[0] ? ok : S_UPDATED);
    tvdl_storage_kick(0);
  }
  w_want = 1;   /* re-read after every action, as the reference does */
}

/* ---- the worker ----------------------------------------------------------------- */

/* Extra re-check triggers (map-library 3.1, B only): a download landed, the
 * catalogue or the set of drives changed, the catalogue filename index now
 * knows a file, a scan's follow-up is due. Then, past the 5 s floor, one
 * check is scheduled. */
static void triggers(int64_t now, int64_t *last_probe) {
  if (!w_walked) return;
  dl_summary sm;
  dl_summary_get(&sm);
  if (w_last_done >= 0 && sm.done != w_last_done) w_want = 1;
  w_last_done = sm.done;
  int64_t rev = catalog_revision();
  if (w_last_rev >= 0 && rev != w_last_rev) w_want = 1;
  w_last_rev = rev;
  tvdl_snap_info info;
  tvdl_snapshot(NULL, 0, 0, &info);
  if (w_have_gen && info.gen != w_last_gen) w_want = 1;
  w_last_gen = info.gen;
  w_have_gen = 1;
  if (w_nprobe && now - *last_probe >= PROBE_EVERY_MS) {
    *last_probe = now;
    xthread_note("tvlib: probe");
    if (probe_hash() != w_probe_sig) w_want = 1;
  }
  if (w_followup_ms && now >= w_followup_ms) { w_followup_ms = 0; w_want = 1; w_want_forced = 1; }
  if (!w_want) return;
  pthread_mutex_lock(&tvlib_mu);
  if (!g_busy && !g_pending && (!g_attempted_ms || now - g_attempted_ms >= LIB_POST_COOLDOWN_MS)) {
    g_pending = 1;
    if (w_want_forced) g_forced = 1;
    g_attempted_ms = now;
    w_want = w_want_forced = 0;
  }
  pthread_mutex_unlock(&tvlib_mu);
}

static void *tvlib_main(void *arg) {
  (void)arg;
  xthread_note("tvlib");
  int64_t last_trigger = 0, last_job = 0, last_probe = 0;
  while (!g_cfg.stop_requested) {
    int stopping = tv_stopping();
    lib_cmd cmd;
    int have_cmd = 0;
    pthread_mutex_lock(&tvlib_mu);
    if (g_cmd_pending) { cmd = g_cmd; g_cmd_pending = 0; have_cmd = 1; }
    g_wake = 0;
    pthread_mutex_unlock(&tvlib_mu);
    if (have_cmd) {
      xthread_note("tvlib: %s", cmd.action);
      execute(&cmd);
      continue;
    }
    if (!stopping) {
      int64_t now = now_ms();
      pthread_mutex_lock(&tvlib_mu);
      int active = g_job.has && g_job.active;
      pthread_mutex_unlock(&tvlib_mu);
      if (now - last_job >= (active ? JOB_POLL_ACTIVE_MS : JOB_POLL_IDLE_MS)) {
        last_job = now;
        xthread_note("tvlib: storage job");
        job_tick(1);
      }
      if (now - last_trigger >= TRIGGER_EVERY_MS) {
        last_trigger = now;
        xthread_note("tvlib: triggers");
        triggers(now, &last_probe);
      }
      int run = 0, forced = 0;
      pthread_mutex_lock(&tvlib_mu);
      if (g_pending) { run = 1; forced = g_forced; g_pending = g_forced = 0; g_busy = 1; }
      pthread_mutex_unlock(&tvlib_mu);
      if (run) { check(forced); continue; }
    }
    xthread_note("tvlib: idle");
    for (int i = 0; i < 1000 / TICK_MS && !g_cfg.stop_requested; i++) {
      util_sleep_ms(TICK_MS);
      pthread_mutex_lock(&tvlib_mu);
      int wk = g_wake || g_cmd_pending || g_pending;
      pthread_mutex_unlock(&tvlib_mu);
      if (wk) break;
    }
  }
  pthread_mutex_lock(&tvlib_mu);
  g_started = 0;
  pthread_mutex_unlock(&tvlib_mu);
  return NULL;
}

void tvlib_start(void) {
  pthread_mutex_lock(&tvlib_mu);
  g_started = 1;
  pthread_mutex_unlock(&tvlib_mu);
  if (tv_thread_start(NULL, "tvlib", tvlib_main, NULL, 0, 1) != 0) {
    pthread_mutex_lock(&tvlib_mu);
    g_started = 0;
    pthread_mutex_unlock(&tvlib_mu);
    LOGE("tvlib: worker not started; the Library answers \"unavailable\"");
  }
}
