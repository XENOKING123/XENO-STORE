/*
 * app_install.h — the store installs its own Games-row app (PPSA99500), so
 * nobody needs FTP for it: a cx_jobs job ("app-install") downloads
 * PPSA99500.ffpkg into /data/homebrew, verifies it, swaps the file in and
 * asks ShadowMountPlus to rescan. Two ways in:
 *  - the web UI (GET /api/app/status, POST /api/app/install; progress through
 *    /api/saves/jobs/{id}): the latest release, hash from its SHA256SUMS;
 *  - the TV app (/api/v1/tv-app, tvapi_update.c): app_install_start_pinned()
 *    installs exactly the release its check found (URL, size, SHA-256), checks
 *    the UFS2 image and re-checks what is installed right before the swap.
 * Both write the install record <data>/tv-app.json
 * {version,checksum,size,modified,inode} after the swap (the web UI path with
 * version ""), which tells a copy the store installed from one the owner
 * placed. Both use the same job type, so they never run at the same time.
 *
 * Host build: XENO_APP_BASE overrides the release URL (loopback test
 * server) and /data/homebrew maps below <data>/sim like the other console
 * paths (cx_common.h).
 */
#ifndef XENO_APP_INSTALL_H
#define XENO_APP_INSTALL_H

#include "json.h"
#include <stddef.h>
#include <stdint.h>

#define APP_TITLE_ID "PPSA99500"
/* Where a pinned TV app release may come from (console). */
#define APP_RELEASE_BASE "https://github.com/XENOKING123/XENO-STORE/releases/download/"
/* The UFS2 superblock magic 19 01 54 19 (0x19540119 little-endian) sits at
 * 65536 + 1372 in our images (64 KiB blocks, superblock at 65536). */
#define APP_UFS2_MAGIC_OFFSET (65536 + 1372)
#define APP_MIN_BYTES (APP_UFS2_MAGIC_OFFSET + 4)
#define APP_MAX_BYTES (512LL << 20)

/* <homebrew>/PPSA99500.ffpkg: where both paths put the app. */
void app_install_pkg_path(char *out, size_t cap);

/* Writes installed, path, size, version, job (running job id or ""),
 * lastJob, titleId, file, registered into the open object. */
void app_install_status_json(jw *w);

/* Starts the install job. Returns 0 and the job id, or -1 with code
 * (in_progress | downloads_unavailable | forbidden | io | oom | busy) and err. */
int app_install_start(char *job_id, size_t idcap, char *code, size_t ccap, char *err, size_t errcap);

/* What is in /data/homebrew now (the /api/v1/tv-app "installed" value),
 * checked in this order: <hb>/PPSA99500/eboot.bin exists -> FOLDER (an lstat
 * error other than ENOENT/ENOTDIR -> BLOCKED); <hb>/PPSA99500.ffpkg missing ->
 * NONE, not a regular file or unreadable -> BLOCKED; the install record
 * matches its size, mtime and inode -> STORE (version = the recorded one);
 * otherwise MANUAL. Two lstat calls and one small read; no walk. */
enum { APP_STATE_NONE = 0, APP_STATE_STORE, APP_STATE_MANUAL, APP_STATE_FOLDER, APP_STATE_BLOCKED };
int app_install_state(char *version, size_t cap);
/* The wire names: none | orbit | manual | folder | blocked (R11: "orbit"
 * means "installed by this store"). */
const char *app_install_state_name(int state);

/* 1 when url may carry a pinned TV app release: APP_RELEASE_BASE + ... +
 * "/PPSA99500.ffpkg", no '?', '#', '\', ' ' or "..". Host build: also a
 * loopback http:// URL of that shape (test servers). */
int app_install_release_url_ok(const char *url);

/* The TV app's install: downloads `url` (checked by the caller's release
 * check; re-validated here), requires exactly `size` bytes, SHA-256 `sha256`
 * (64 hex) and the UFS2 magic, re-checks the installed state just before the
 * rename (FOLDER, BLOCKED, MANUAL without `replace` or a newer STORE copy
 * abort), renames over <hb>/PPSA99500.ffpkg, writes the install record with
 * `version` and asks ShadowMountPlus to rescan. Returns 0 and the job id, or
 * -1 with code (in_progress | downloads_unavailable | forbidden | bad_request
 * | io | oom | busy) and err. Progress through app_install_snapshot(). */
int app_install_start_pinned(const char *url, const char *sha256, int64_t size, const char *version, int replace,
                             char *job_id, size_t idcap, char *code, size_t ccap, char *err, size_t errcap);

/* The install job as the TV app reads it (memory only, under app_mu). */
typedef struct app_install_info {
  int running;          /* an app-install job (web UI or TV app) is queued or running */
  int pinned;           /* the last job was started by app_install_start_pinned() */
  char job[16];         /* the last job's id, "" before the first one */
  char phase[16];       /* "" | downloading | verifying | installing | installed | error */
  char error[256];      /* a sentence when phase is "error" */
  char version[32];     /* the pinned version ("" for the web UI path) */
  int64_t received;     /* bytes downloaded so far */
  int64_t total;        /* expected bytes (0 when unknown) */
} app_install_info;
void app_install_snapshot(app_install_info *out);

#endif
