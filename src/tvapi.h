/*
 * tvapi.h — internal header shared by every src/tvapi*.c file: the /api/v1
 * compatibility layer the native TV app (PPSA99500) talks to.
 *
 * Copyright (C) 2026 XENO STORE contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The TV app is ported from Orbit Store 1.2.0 (GPL-3.0-or-later), so /api/v1
 * speaks the reference Orbit Store backend's dialect: the same paths, the same
 * JSON shapes, strict JSON types (a bool is never 0/1, a number never a
 * string) and the sentence error envelope {"error":"<human sentence>"}.
 * Binding design: PORT-SPEC sections 1.1-1.8 (field tables in map-*.md).
 *
 * Files (each one owns its section of this header):
 *   tvapi.c           route table, tvapi_init, lazy start, gate, session
 *                     helpers, reply helpers, instance id, tvapi.json state
 *   tvapi_storage.c   tv-storage thread: the drive snapshot and the drive
 *                     vocabulary (ids, labels) used by downloads and library
 *   tvapi_system.c    /system, /session, /zero/companion, the stop drain
 *   tvapi_catalog.c   TV catalogue index (tvcat_*), /sources, /catalog,
 *                     /catalog/updates, /favorites
 *   tvapi_art.c       /art
 *   tvapi_downloads.c /downloads, /storage, /settings (downloads and
 *                     extraction), /debrid, /browser
 *   tvapi_library.c   /library*
 *   tvapi_update.c    /updates, /tv-app
 *
 * Request flow. server.c gate -> api.c dispatcher -> the route table in
 * tvapi.c. Every table entry is a small wrapper that first calls tv_gate()
 * with the flags of PORT-SPEC 1.4 for that method and path, and calls the
 * handler declared below only when the gate passed. So a handler starts with
 * the subsystems started (R13), the stopping check, the Bearer check and the
 * JSON-object body check already done; it must not call tv_gate() again and
 * it must answer exactly once (tv_obj_send / tv_arr_send / tv_reply /
 * tv_error / tv_send_bytes).
 *
 * Rules for every tvapi file (PORT-SPEC 1.2):
 *  - Threads only through tv_thread_start() (it wraps xthread_start(), so it
 *    has an explicit stack, and it counts the thread for /api/diagnostics).
 *  - JSON only through the jw_* writer; booleans with jw_kv_bool, integers
 *    with jw_kv_i64/jw_kv_int, strings with jw_kv_str. Read request members
 *    with the strict tv_body_* helpers below (jv_get_bool/jv_get_i64 coerce).
 *  - Request paths never touch a drive: no statvfs, no directory walk, no
 *    file creation, no network. Take state from the snapshots and caches.
 *  - settings_t (~30 KB) never goes on a polled path: use the cheap getters
 *    (settings_concurrent_jobs, settings_default_root, settings_delete_archives,
 *    settings_updates_auto_check, settings_sources_acknowledged).
 *  - Locks: tvdl_mu (tvapi_storage.c), tvcat_mu, art_mu, tvlib_mu and
 *    tvapi.c's own lock are leaf locks: never call another module while
 *    holding one. Never call catalog_* or settings_* inside a
 *    catalog_visit_* callback, and never call tvcat_*, catalog_* or
 *    settings_* while holding a lock dl.c takes: copy (dl_brief_list) first,
 *    then look things up.
 *  - Never use api_error()/http_send_error(): their "error" is a code, and
 *    the app would show it. Use tv_error() with a sentence. Where the app has
 *    a translation key, the sentence must match it exactly (map-*.md list them).
 *  - Ported validators, cooldowns or sentences carry a comment
 *    "Ported from Orbit Store <file> (GPL-3.0-or-later)".
 *
 * Core helpers added for this layer (declared in their own headers):
 *   dl.h       dl_brief_list / dl_brief_get (lean job copies), dl_state_healthy,
 *              dl_save_queue (returns int), dl_pause_all_for_stop,
 *              dl_workers_running; dl_action: cancel also takes a failed job,
 *              extract takes {"password"}, cancel {deleteFiles} zeroes written
 *   settings.h settings_concurrent_jobs, settings_default_root,
 *              settings_delete_archives, settings_updates_auto_check,
 *              settings_sources_acknowledged, settings_merge_checked
 *              (SETTINGS_ERR_SAVE -> a 503 "Could not save ..." sentence),
 *              settings_favorite_set, settings_favorites_count
 *   catalog.h  catalog_refreshing, catalog_refresh_runs, CATALOG_REFRESH_BUSY
 *              (refreshes never overlap), catalog_visit_root
 *   storage.h  storage_peek_roots (no I/O), storage_title_from_name; console
 *              /mnt roots count only when mounted
 *   server.h   http_send_sentence; api.h API_V1_MSG_*, api_is_v1_path
 *   sys.c      sys_local_ip is thread-safe; it returns -1 when there is no LAN address
 */
#ifndef XENO_TVAPI_H
#define XENO_TVAPI_H

#include "api.h"
#include "dl.h"
#include "json.h"
#include "server.h"
#include "storage.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ==== tvapi.c: gate, sentences, session, replies ========================== */

/* Gate flags (the "Gate" column of PORT-SPEC 1.4). */
#define TV_S 1u   /* 503 TV_MSG_STOPPING while tv_stopping() */
#define TV_B 2u   /* 401 TV_MSG_UNPAIRED without a valid Bearer token */
#define TV_O 4u   /* POST only: 415 for a non-JSON Content-Type, 400 unless the body is a JSON object */

/* Sentences shared by every tvapi file. The app translates exact matches. */
#define TV_MSG_STOPPING "XENO STORE is stopping. It will be back in a moment."
#define TV_MSG_UNPAIRED "Pair this device with your PS5 to continue."
#define TV_MSG_NOT_OBJECT API_V1_MSG_NOT_OBJECT                  /* "Expected a JSON object." */
#define TV_MSG_USE_JSON "Use application/json."
#define TV_MSG_NOT_AVAILABLE API_V1_MSG_NOT_AVAILABLE            /* unknown route / wrong method */
#define TV_MSG_PAIR_AGAIN API_V1_MSG_PAIR_AGAIN                  /* core PIN gate refused the token */
#define TV_MSG_FAILED API_V1_MSG_FAILED                          /* generic 500 */
#define TV_MSG_NOT_READY "This part of XENO STORE is not ready yet."   /* stubs only */
#define TV_MSG_LOW_MEMORY "XENO STORE is low on memory. Try again."
#define TV_MSG_SESSION_REMOTE "Pair this device using the code shown on your PS5."
#define TV_MSG_PAIRING_SAVE "Could not save pairing."
#define TV_MSG_ZERO "Orbit Zero is not part of XENO STORE."

/* Runs the gate of PORT-SPEC 1.3 in the R3 order and returns 1 when the
 * request may go on, or 0 after it answered:
 *   0. starts the TV subsystems once (pthread_once; R13), always;
 *   1. TV_S and tv_stopping()            -> 503 TV_MSG_STOPPING
 *   2. TV_B and !tv_authed(req)          -> 401 TV_MSG_UNPAIRED
 *   3. TV_O on a POST: a Content-Type that is present and not
 *      application/json                  -> 415 TV_MSG_USE_JSON
 *      the body is not a JSON object (empty, [], "x", 1, null)
 *                                        -> 400 TV_MSG_NOT_OBJECT
 * GET and HEAD skip step 3. The route table calls it; handlers do not. */
int tv_gate(api_ctx *c, unsigned flags);

/* 1 when the request carries "Authorization: Bearer <token>" and the token is
 * a paired one (pairing_token_valid: 64 hex, constant time). */
int tv_authed(const http_req *req);

/* 1 while the process is going away: g_cfg.stop_requested (SIGTERM, web UI
 * stop or restart) or the TV stop drain (tv_drain_active()). Host test hook:
 * also 1 while the file named by $XENO_TVAPI_STOPPING_FILE exists. */
int tv_stopping(void);

/* Drain state (the TV app's stop, PORT-SPEC 1.5.5 step 3-4). The flag lives
 * in tvapi.c so tv_stopping() sees it; only tvsys_stop_begin() sets it, once
 * the stop is accepted. It is never cleared: the process exits after it. */
int tv_drain_active(void);
void tv_drain_set_active(int on);

/* The per-process instance id: 32 lowercase hex chars, made once by
 * tvapi_init() with util_rand_hex, never persisted, never empty. */
const char *tv_instance_id(void);

/* Monotonic ms (util_now_ms) of the last GET /api/v1/system from a loopback
 * caller, 0 = never. tvapi.c records it before tvsys_h_system runs;
 * tvapi_client_seen() (api.h, read by main.c) is "within the last 10 s". */
int64_t tv_client_seen_ms(void);

/* The "auto" default-drive flag of <data>/tvapi.json {"preferredAuto":bool},
 * loaded by tvapi_init(). tv_pref_auto_set() writes the file atomically and
 * returns 0, or -1 when it could not be written (the flag still changes in
 * memory and is logged; PORT-SPEC 1.6.3). */
int tv_pref_auto(void);
int tv_pref_auto_set(int on);

/* Starts a TV thread: xthread_start() with `stack` bytes (0 = 1 MiB), named
 * `name` (tv-storage, tv-art, tvlib, tv-update, tv-catalog, tv-drain), logged
 * and counted for /api/diagnostics. Same return value as xthread_start (0 =
 * started). Long-lived threads exit when g_cfg.stop_requested is set and
 * start no new dl_* work once tv_stopping() is true. */
int tv_thread_start(pthread_t *out, const char *name, void *(*fn)(void *), void *arg, size_t stack, int detached);

/* 1 when the request method is POST (handlers shared by GET and POST). */
int tv_is_post(const api_ctx *c);

/* ---- replies (all set Cache-Control: no-store; never gzip) ---------------
 * tv_error:   {"error":"<sentence>"} with `status`.
 * tv_obj_begin / tv_obj_send: jw_init + jw_obj_begin ... jw_obj_end + send.
 * tv_arr_begin / tv_arr_send: the same for a top-level array (/catalog,
 *   /downloads, /storage, /favorites answer bare arrays).
 * tv_reply:   sends a writer that is already complete (frees it).
 * A writer that ran out of memory is answered 503 TV_MSG_LOW_MEMORY. */
void tv_error(api_ctx *c, int status, const char *sentence);
void tv_obj_begin(jw *w);
void tv_obj_send(api_ctx *c, int status, jw *w);
void tv_arr_begin(jw *w);
void tv_arr_send(api_ctx *c, int status, jw *w);
void tv_reply(api_ctx *c, int status, jw *w);
/* Raw bytes (art): `ctype` such as "image/webp"; copies nothing, the caller
 * keeps `data` alive for the call. */
void tv_send_bytes(api_ctx *c, int status, const char *ctype, const void *data, size_t len);
/* 503 TV_MSG_NOT_READY: what the stub handlers answer until implemented. */
void tv_not_ready(api_ctx *c);

/* ---- strict request readers (no coercion; the reference checks types) ----
 * Each returns 1 when `key` is present in the JSON object body with exactly
 * that JSON type (and stores the value), else 0. tv_body_int accepts only a
 * number with an integral value within +-2^53. tv_body_has: present with any
 * type, null included. tv_body_type: JV_* of the member (JV_INVALID = absent). */
int tv_body_has(const api_ctx *c, const char *key);
int tv_body_type(const api_ctx *c, const char *key);
int tv_body_str(const api_ctx *c, const char *key, const char **out);
int tv_body_bool(const api_ctx *c, const char *key, int *out);
int tv_body_int(const api_ctx *c, const char *key, int64_t *out);

/* ==== per-subsystem start functions (lazy start, R13) ======================
 * Called once, in this order, on the first /api/v1 request that reaches the
 * route table (pthread_once inside tv_gate()). Each one must return quickly:
 * start its thread(s) with tv_thread_start() and return; no network, no
 * statvfs, no walk. A start failure is logged and the routes still answer. */
void tvdl_storage_start(void);   /* tvapi_storage.c: the tv-storage thread (first) */
void tvsys_start(void);          /* tvapi_system.c */
void tvcat_start(void);          /* tvapi_catalog.c */
void tvart_start(void);          /* tvapi_art.c: two art workers */
void tvdl_start(void);           /* tvapi_downloads.c */
void tvlib_start(void);          /* tvapi_library.c: the tvlib worker */
void tvup_start(void);           /* tvapi_update.c: the tv-update worker */

/* ==== route handlers (one per row of PORT-SPEC 1.4) ========================
 * Called by tvapi.c after tv_gate() passed with the flags shown, so every
 * start function above has already returned when a handler runs. GET and
 * POST routes that share a handler tell the method apart with tv_is_post()
 * (HEAD arrives as a GET route with method "HEAD"). Captures ({id}, {kind},
 * {action}) are in c->params[0..], at most 95 bytes, never containing '/'. */

/* tvapi_system.c */
void tvsys_h_system(api_ctx *c);     /* GET /api/v1/system (no gate): 200 from memory, also while stopping */
void tvsys_h_session(api_ctx *c);    /* GET /api/v1/session (S): loopback only (403 otherwise) */
void tvsys_h_zero(api_ctx *c);       /* GET, POST /api/v1/zero/companion (no gate): 404 TV_MSG_ZERO */

/* tvapi_catalog.c */
void tvcat_h_sources(api_ctx *c);    /* GET (S) / POST (S,B,O) /api/v1/sources */
void tvcat_h_catalog(api_ctx *c);    /* GET /api/v1/catalog (S): bare array */
void tvcat_h_updates(api_ctx *c);    /* GET (S) / POST (S,B,O) /api/v1/catalog/updates */
void tvcat_h_favorites(api_ctx *c);  /* GET (S,B) / POST (S,B,O) /api/v1/favorites: bare array */

/* tvapi_art.c */
void tvart_h_art(api_ctx *c);        /* GET /api/v1/art/{id}/{kind} (S,B): params[0] = game id, params[1] = cover|hero */

/* tvapi_downloads.c */
void tvdl_h_jobs(api_ctx *c);        /* GET /api/v1/downloads (S,B): bare array */
void tvdl_h_create(api_ctx *c);      /* POST /api/v1/downloads (S,B,O): 201 {"id"} */
void tvdl_h_action(api_ctx *c);      /* POST /api/v1/downloads/{id}/{action} (S,B,O): params[0] = job id, params[1] = action */
void tvdl_h_storage(api_ctx *c);     /* GET /api/v1/storage (S,B): bare array of writable drives */
void tvdl_h_preferred(api_ctx *c);   /* POST /api/v1/storage/preferred (S,B,O) */
void tvdl_h_limit(api_ctx *c);       /* POST /api/v1/settings/downloads (S,B,O) */
void tvdl_h_extraction(api_ctx *c);  /* POST /api/v1/settings/extraction (S,B,O) */
void tvdl_h_debrid(api_ctx *c);      /* GET /api/v1/debrid (S,B) */
void tvdl_h_browser(api_ctx *c);     /* GET (S,B) / POST (S,B,O) /api/v1/browser: never 404 */

/* tvapi_library.c */
void tvlib_h_library(api_ctx *c);    /* GET (S,B) / POST (S,B,O) /api/v1/library */
void tvlib_h_storage(api_ctx *c);    /* GET (S,B) / POST (S,B,O) /api/v1/library/storage */
void tvlib_h_actions(api_ctx *c);    /* POST /api/v1/library/actions (S,B,O): 202 + snapshot */

/* tvapi_update.c */
void tvup_h_updates(api_ctx *c);     /* GET (B, not S) / POST (S,B,O) /api/v1/updates */
void tvup_h_tvapp(api_ctx *c);       /* GET (S,B) / POST (S,B,O) /api/v1/tv-app */

/* ==== tvapi_system.c: the stop sequence ===================================== */

/* POST /api/v1/updates {"action":"stop","confirmed":true} (tvapi_update.c
 * validates the body, then calls this). PORT-SPEC 1.5.5 steps 1-4:
 *  1-2. dl_pause_all_for_stop("Paused for the XENO STORE restart"): pause
 *       queued and active jobs and save queue.json synchronously;
 *  3.   tv_drain_set_active(1) (GET /system then says stopping:true);
 *  4.   a detached tv-drain thread waits until dl_workers_running() == 0
 *       (polls every 100 ms, at most 10 s), calls dl_save_queue() again and
 *       sets g_cfg.stop_requested. It never sets g_cfg.restart_requested: the
 *       app starts the next copy itself.
 * If the tv-drain thread cannot start, it sets g_cfg.stop_requested at once
 * (main's shutdown then stops the workers itself) and still succeeds.
 * Returns TVSYS_STOP_OK (the caller replies 202 with the update status,
 * including instanceId, runningVersion and savedVersion),
 * TVSYS_STOP_SAVE_FAILED (the jobs were put back; reply 503 "Could not save
 * the queue. XENO STORE has not been stopped."), TVSYS_STOP_ALREADY (a stop
 * is running; the gate normally answers 503 before), TVSYS_STOP_INSTALLING
 * (nothing was changed: a package install is starting or still reads its
 * package from /pkgsrv/, which a stop would cut off; reply 409) or
 * TVSYS_STOP_FAILED (nothing was changed: the stub before the system step;
 * reply 503). */
enum {
  TVSYS_STOP_OK = 0, TVSYS_STOP_SAVE_FAILED = -1, TVSYS_STOP_ALREADY = -2, TVSYS_STOP_FAILED = -3,
  TVSYS_STOP_INSTALLING = -4
};
#define TVSYS_STOP_PAUSE_MESSAGE "Paused for the XENO STORE restart"
int tvsys_stop_begin(void);

/* ==== tvapi_catalog.c: the TV catalogue index ===============================
 * The index covers every TV-visible release (tier A, R1) of every exposed
 * source, enabled or not, and is rebuilt when catalog_revision(), the
 * featured feed or the compile switches change. tvcat_rel is the lookup
 * result (the index may store releases more compactly). Every string is
 * exactly what GET /api/v1/catalog sends for that option, so a download job
 * or a library row that copies it matches the app's option (APP/model.cpp
 * release_job / "In Library" compare titleId, format, filename, size). */
typedef struct tvcat_rel {
  char id[128];            /* release id */
  char gameId[96];         /* game id ([A-Za-z0-9._-]{1,95}) */
  char title[256];         /* game title */
  char platform[8];        /* "ps5" | "ps4" (lowercase) */
  char titleId[16];        /* tv_release_title_id(): uppercase, "" when none */
  char format[16];         /* TV spelling: PKG, FPKG, FFPKG, exFAT, FFPFSC, FFPFS; "Unknown" for a ZIP (TV_SHOW_ZIP) */
  char compression[8];     /* "zip" or "" */
  char filename[256];      /* catalogue filename, unsanitised */
  char sourceId[32];       /* "archive", "vikingfile", ... */
  char kind[12];           /* game | update | dlc | backport */
  char version[32];        /* "" when the catalogue has none */
  char region[16];         /* EUR | USA | JPN | ASIA | "" */
  int64_t sizeBytes;       /* > 0 */
  int64_t extractedBytes;  /* sizeBytes for a ZIP option, else 0 */
  int browserAvailable;    /* the source kind is "browser" (R6) */
  int directAvailable;     /* the source kind is "direct" (R6) */
} tvcat_rel;

/* 0 and *out filled when `releaseId` is a TV-visible release, -1 otherwise.
 * Hash lookup under tvcat_mu (a leaf lock); never walks the catalogue on the
 * caller's thread once the index is built. */
int tvcat_release(const char *releaseId, tvcat_rel *out);
/* The first TV-visible release (catalogue order) whose catalogue filename
 * equals `filename` exactly (library title ID resolution, D15 step 2).
 * 0 and *out filled, or -1. */
int tvcat_by_filename(const char *filename, tvcat_rel *out);
/* settings sources.acknowledged (the first-run download notice, shared with
 * the web UI's wizard). */
int tvcat_sources_acknowledged(void);
/* GET /system "catalogueRevision": (catalog_revision() << 20) |
 * (fnv1a(featured hero ids) & 0xFFFFF), from the meta cache. Memory only. */
int64_t tvcat_catalogue_revision(void);
/* The TV title ID of one release, first match wins: the release titleId
 * (uppercased), the game's titleIds[0], the first
 * (CUSA|PPSA|PLAS|PCAS|PCJS|PLJS|PLJM|PCJM)\d{5} in the filename or the URL
 * path, else "". Takes catalogue nodes, so call it only inside a
 * catalog_visit_* callback (or on nodes of a parsed copy). */
void tv_release_title_id(jv game, jv rel, char *out, size_t cap);
/* The game of a TV-visible release: 0 and the game id in gameId_out (cap >=
 * 96), or -1 when the release is unknown or hidden from the TV catalogue. */
int tv_find_release(const char *releaseId, char *gameId_out, size_t cap);

/* ==== tvapi_downloads.c: cross-calls ========================================= */

/* POST /api/v1/sources side effect (optional, PORT-SPEC 1.5.2): pause the
 * queued and active download jobs of `sourceId` after the source was turned
 * off. Returns the number of jobs paused (0 when none or not implemented). */
int tvdl_pause_source(const char *sourceId);

/* ==== tvapi_storage.c: the drive snapshot (PORT-SPEC 1.6.2, R4, R5) ==========
 * One tv-storage thread (started first by the lazy start) keeps a snapshot of
 * every mounted root, read-only ones included:
 *  - every 2 s it takes a presence signature: stat() of /data, /mnt/ext0|1,
 *    /mnt/usb0..7 with the mount test st_dev(root) != st_dev("/mnt") (host
 *    build: the downloads dir and the --root dirs, presence only);
 *  - when that signature changes, every 60 s, or after tvdl_storage_kick(1)
 *    it runs storage_scan_roots() (rediscovery + writability probe);
 *    otherwise storage_get_roots() (one statvfs per root);
 *  - it maps the roots to tv_drive and swaps them in under tvdl_mu.
 * Readers only copy the snapshot: free/total/writable come from it, and
 * nothing on a request path calls statvfs or stat. Before the first refresh
 * the snapshot is seeded from storage_peek_roots() (no filesystem access). */

#define TVDL_MAX_DRIVES STORAGE_MAX_ROOTS
/* projectedFreeBytes = freeBytes - pendingBytes - this margin: XENO's own
 * reserve (DL_RESERVE_BYTES, 256 MiB) minus the app's 16 MiB, so the app's
 * Download button and dl_add's refusal agree (map-downloads §6). */
#define TVDL_PROJECTION_MARGIN (DL_RESERVE_BYTES - (16LL << 20))

typedef struct tv_drive {
  char id[64];        /* usbN | ext0 | ext1 | internal (console); host: internal (downloads dir),
                         root<i> (--root i); otherwise the root's basename made [A-Za-z0-9_-]{1,63} */
  char label[64];     /* R4: "USB storage N+1", "M.2 SSD" (ext1), "Extended storage N+1" (other extN),
                         "Internal storage" (/data, host downloads dir); host --root: XENO's label (basename) */
  char path[256];     /* the root (mount point): "/mnt/usb0", "/data", a host dir. /api/v1/storage
                         sends path + "/homebrew"; /api/v1/library/storage sends it as is */
  char kind[12];      /* XENO kind: internal | m2 | usb */
  int external;       /* kind != "internal" */
  int writable;       /* R5: the writability probe of the last storage_scan_roots() */
  int is_default;     /* path == settings downloads.defaultRoot */
  int order;          /* sort group: 0 USB and host --root, 1 extN, 2 other, 3 internal */
  int64_t free, total;   /* bytes, from the last statvfs of the tv-storage thread */
} tv_drive;

typedef struct tvdl_snap_info {
  time_t updated_at;   /* wall clock (util_now) of the last refresh; 0 = only the seed so far */
  int64_t updated_ms;  /* util_now_ms() of the last refresh (staleness: now_ms - updated_ms) */
  time_t scanned_at;   /* wall clock of the last storage_scan_roots() */
  unsigned gen;        /* +1 whenever the set of drives, a writable flag or a label changes */
  int kick_pending;    /* a tvdl_storage_kick() has not been served yet (library "busy") */
  int count;           /* drives in the snapshot, read-only ones included */
} tvdl_snap_info;

/* Copies the snapshot in display order (USB and host roots, then ext, then
 * other, then internal; by number inside a group). writable_only = 1 skips
 * read-only drives (/api/v1/storage lists only writable ones; R5). Returns
 * the number copied (at most max). info may be NULL. */
int tvdl_snapshot(tv_drive *out, int max, int writable_only, tvdl_snap_info *info);
/* Wakes the tv-storage thread now; rescan = 1 also forces storage_scan_roots()
 * (POST /api/v1/library/storage). Never blocks on the refresh. */
void tvdl_storage_kick(int rescan);
/* Looks a drive up in the snapshot by id / by root path: 0 when it is there
 * and writable, -2 when it is there but read-only (out filled either way),
 * -1 when it is not mounted. out may be NULL. */
int tvdl_drive_for_id(const char *id, tv_drive *out);
int tvdl_drive_for_root(const char *root, tv_drive *out);
/* 1 when `root` (a job's root path) is mounted, writable or not. */
int tvdl_root_present(const char *root);
/* The drive id of a root path: a pure function of the path (no snapshot), so
 * a job on an unplugged drive still maps to its id. */
void tvdl_id_for_root(const char *root, char *out, size_t cap);
/* The R4 label of a root path; xs_label (storage_root.label, may be NULL)
 * names host --root drives and unknown roots. */
void tvdl_label_for_root(const char *root, const char *xs_label, char *out, size_t cap);
/* The Library "location" of a file path (map-library §3.2): "USB storage N+1",
 * "M.2 SSD", "Extended storage N+1", "Internal storage" for /data and /user;
 * console: "Other location" otherwise; host: the label of the root holding it. */
void tvdl_location_for_path(const char *path, char *out, size_t cap);
/* GET /system "preferredStorage": "" while tv_pref_auto() is set, otherwise
 * tvdl_id_for_root(settings downloads.defaultRoot). Memory only. */
void tvdl_preferred_storage_id(char *out, size_t cap);
/* The drive "Choose automatically" resolves to: the first external writable
 * drive in snapshot order, else the internal one. 0 and *out, or -1. */
int tvdl_auto_drive(tv_drive *out);
/* freeBytes - pendingBytes - TVDL_PROJECTION_MARGIN (may be negative). The
 * caller passes pending = dl_remaining_on_root(d->path) (queue lock only). */
int64_t tvdl_projected_free(const tv_drive *d, int64_t pending);
/* /api/diagnostics: writes "drives" (the snapshot) and "storage" (info). */
void tvdl_storage_diag_json(jw *w);

#endif
