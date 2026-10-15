/*
 * app_install.h — the store installs its own Games-row app (PPSA99500), so
 * nobody needs FTP for it: a cx_jobs job ("app-install") downloads
 * PPSA99500.ffpkg from the latest release (hash from its SHA256SUMS) into
 * /data/homebrew, verifies the hash, swaps the file in and asks
 * ShadowMountPlus to rescan. Exposed as GET /api/app/status and
 * POST /api/app/install (progress through /api/saves/jobs/{id}).
 *
 * Host build: XENO_APP_BASE overrides the release URL (loopback test
 * server) and /data/homebrew maps below <data>/sim like the other console
 * paths (cx_common.h).
 */
#ifndef XENO_APP_INSTALL_H
#define XENO_APP_INSTALL_H

#include "json.h"
#include <stddef.h>

#define APP_TITLE_ID "PPSA99500"

/* Writes installed, path, size, version, job (running job id or ""),
 * lastJob, titleId, file, registered into the open object. */
void app_install_status_json(jw *w);

/* Starts the install job. Returns 0 and the job id, or -1 with code
 * (in_progress | downloads_unavailable | forbidden | io | oom | busy) and err. */
int app_install_start(char *job_id, size_t idcap, char *code, size_t ccap, char *err, size_t errcap);

#endif
