/*
 * cx_garlic.h — Garlic Save Manager (earthonion/garlic-savemgr) client: the
 * console-side save re-signer the save engine relies on when the SaveData
 * mount API is not usable. Garlic listens on 127.0.0.1:8082 and parses each
 * request from a single recv(), so every call goes through cx_local_http()
 * (request line + headers in one send, bodies streamed from files).
 *
 * Real import flow (garlic-savemgr src/main.c):
 *   GET  /api/status                       {"mounted":bool,"mount_point":..}
 *   GET  /api/create_pfs?size=<bytes>      formats + mounts a fresh PS5 image
 *   POST /api/upload_file?name=<relpath>   raw octet-stream body per file
 *   GET  /api/import_check?uid=<hex>       {"exists":bool,"title_id":..}
 *   GET  /api/import_finish?uid=<hex>      re-sign (account id + user id),
 *        [&force=1&title_id=..&dir_name=..] unmount, copy to /user/home, DB row;
 *                                          {"sfo_zeroed":true} asks for force
 *   GET  /api/unmount                      cleanup after a failure
 *   GET  /api/export_title_dec?id=<TID>    zip "<TID>/<save>/<rel>" of every
 *                                          save of the title, decrypted
 * Failures are HTTP 200 with {"error":".."}: success is "ok":true and no
 * "error" key.
 *
 * Starting Garlic: an ELF the user already has (the store's own companion
 * copy in <data>/payloads/, /data, /data/homebrew, /data/etaHEN, USB
 * homebrew/payloads folders) or the pinned upstream release (companions feed
 * entry of kind "saves", else the compiled-in URL + sha256) is sent to the
 * ELF loader on :9021. A second copy is never started while anything listens
 * on Garlic's port: a starting Garlic unmounts /data/mount_sd first.
 *
 * Host build: ports come from XENO_PORT_GARLIC / XENO_PORT_LOADER, console
 * paths map below <data>/sim (cx_console_path) and the compiled-in download
 * is off, so tests never touch the real /data or the network.
 */
#ifndef XENO_CX_GARLIC_H
#define XENO_CX_GARLIC_H

#include "json.h"
#include <stddef.h>
#include <stdint.h>

typedef struct cx_job cx_job;

/* Pinned upstream release (also in updates/companions.json). The repository
 * has no licence: the store never rehosts this file, it downloads it from the
 * author's own release on the user's console and checks the sha256. */
#define CX_GARLIC_ELF_URL "https://github.com/earthonion/garlic-savemgr/releases/download/v1.7/garlic-savemgr.elf"
#define CX_GARLIC_ELF_SHA256 "124051ab3a762474720ae53187d2920bc96d6be1d69aa298e715667efc385a2f"
#define CX_GARLIC_ELF_VERSION "1.7"
#define CX_GARLIC_ELF_SIZE 1497744
#define CX_GARLIC_COMPANION_ID "garlic-save-manager"

int cx_garlic_port(void);

/* 1 when Garlic answers GET /api/status (cached for a few seconds; an import
 * or export this store is running counts as running). */
int cx_garlic_running(void);

/* 1 = Garlic answers /api/status on :8082 (started it if allowed and needed:
 * found ELF on the console or downloaded the pinned upstream release, sent to
 * the ELF loader on :9021, polled up to 20 s); 0 = not available (err says
 * why, user-facing). */
int cx_garlic_ensure(int start_if_missing, char *err, size_t errcap);

/* Writes "garlic":{available, running, busy, port, canStart,
 * starting, loaderAvailable, saveOpen?, version?, elfPath?, source?, detail}
 * into the open object.
 * available = running, or it can be started on demand. */
void cx_garlic_status_json(jw *w);

typedef struct cx_garlic_result {
  char titleId[16];
  char dirName[64];
  char mainTitle[128];
  char warning[160];
  int existed;              /* import_check / import_finish: a save was replaced */
  int forced;               /* import_finish needed force=1 (zeroed param.sfo) */
  int files;
  int64_t bytes;
} cx_garlic_result;

/* Full create_pfs → upload_file… → import_check → import_finish flow for an
 * extracted decrypted save folder (sce_sys/param.sfo at its root).
 * xeno-save.json, sce_sys/sealedkey and sce_sys/keystone are never uploaded.
 * user_id 0 lets Garlic pick the foreground user. total_bytes (may be 0) is
 * the progress total. Progress via cx_job_progress (j may be NULL); unmounts
 * Garlic on any failure. 0 ok, -1 with err. */
int cx_garlic_import_dir(const char *dir, const char *title_id, uint32_t user_id, const char *dir_name, int64_t total_bytes,
                         cx_job *j, char *err, size_t errcap);
int cx_garlic_import_dir_ex(const char *dir, const char *title_id, uint32_t user_id, const char *dir_name, int64_t total_bytes,
                            cx_job *j, cx_garlic_result *res, char *err, size_t errcap);

/* Decrypted, shareable export of one save (title_id/dir_name of user_id):
 * out_zip holds the save's files at the zip root (sce_sys/param.sfo …),
 * ACCOUNT_ID cleared, no sealedkey/keystone and no manifest (callers add
 * xeno-save.json). 0 ok, -1 with err. */
int cx_garlic_export_dec(const char *title_id, const char *dir_name, uint32_t user_id, const char *out_zip,
                         cx_job *j, char *err, size_t errcap);
/* Same, into a directory (created; emptied first). */
int cx_garlic_export_dec_dir(const char *title_id, const char *dir_name, uint32_t user_id, const char *out_dir,
                             cx_job *j, char *err, size_t errcap);

#endif
