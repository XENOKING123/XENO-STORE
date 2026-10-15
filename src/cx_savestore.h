/*
 * cx_savestore.h — PS5 Save Data Store engine side (SPEC §17.22): merged
 * community index (GitHub index feeds + Supabase REST + Garlic rows through
 * the edge function), compatibility against the installed titles, install /
 * download jobs, export, upload adapters and submission. Also registers
 * GET /api/saves/garlic and POST /api/saves/garlic/start (cx_garlic.h).
 */
#ifndef XENO_CX_SAVESTORE_H
#define XENO_CX_SAVESTORE_H

#include "json.h"
#include <stddef.h>
#include <stdint.h>

void cx_savestore_init(void);
/* Writes "saves": [...], "sources": [...], "updatedAt", "count" and "building"
 * (the index is rebuilt on its own thread; the first request waits for it a
 * few seconds, later ones get the previous copy while it refreshes). */
int cx_savestore_list_json(const char *titleId, const char *category, const char *q, int refresh, jw *w);
/* Installed version of a title (appmeta param.json contentVersion, then
 * /user/app/<id>/sce_sys/param.json). source gets "appmeta"|"app"|"". */
int cx_installed_version(const char *titleId, char *out, size_t cap, char *source, size_t scap);
/* Starts a save-install / save-download job. An install that needs the
 * re-sign engine checks Garlic Save Manager (starting it when needed) before
 * anything is downloaded and fails with errorCode "garlic_unavailable". */
int cx_savestore_install(const char *id, uint32_t uid, int backup_first, int download_only, char *job_id, size_t idcap, char *code, size_t ccap, char *err, size_t errcap);
/* Export (synchronous): writes zipPath, manifest, sha256, size, format, tier,
 * shareable, accountIdStripped, accountId ("removed" | "none"). A decrypted
 * export comes from the native engine or Garlic Save Manager; when neither is
 * available no zip is kept and shareable:false carries reason + detail. */
int cx_savestore_export(uint32_t uid, const char *titleId, const char *dirName, jw *w, char *code, size_t ccap, char *err, size_t errcap);
/* Upload (synchronous): writes url, sha256, size, host (+ storagePath for the
 * "site" host: the zip goes into the database backend's bucket as
 * xeno/<yyyymmdd>-<16 hex>.zip). Refuses zips that cannot be shared (raw
 * images, sealed keys, same-console manifests, an ACCOUNT_ID left in
 * param.sfo) with code "not_shareable"; host "site" without the database
 * backend fails with "site_unavailable". */
int cx_savestore_upload(const char *zipPath, const char *host, jw *w, char *code, size_t ccap, char *err, size_t errcap);
/* Submit metadata. With the database backend (the XenoKing site) and a zip
 * uploaded there (storagePath), one row of the site's own columns is inserted:
 * method "site", status "published", id / storeId, and the index is rebuilt
 * so the save is listed at once. Otherwise (or when the site refuses, reported
 * in siteError) method "github" with issueUrl + qr, or "custom". */
int cx_savestore_submit(jv meta, jw *w, char *code, size_t ccap, char *err, size_t errcap);
/* Cover redirect target for a store row ("" when none). */
int cx_savestore_cover_url(const char *id, char *out, size_t cap);
void cx_savestore_tick(void);

#endif
