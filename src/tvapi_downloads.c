/*
 * tvapi_downloads.c — GET|POST /api/v1/downloads, POST
 * /api/v1/downloads/{id}/{action}, GET /api/v1/storage, POST
 * /api/v1/storage/preferred, POST /api/v1/settings/downloads|extraction,
 * GET /api/v1/debrid and GET|POST /api/v1/browser (PORT-SPEC 1.5.3;
 * map-downloads §3-§9). The drive snapshot it reads lives in tvapi_storage.c.
 *
 * Copyright (C) 2026 XENO STORE contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The TV app's download queue is XENO STORE's own queue (dl.c), seen through
 * the reference backend's job model:
 *  - GET /downloads copies the queue (dl_brief_list, one queue_mu hold), then,
 *    outside every dl.c lock, projects each visible job (a download, not a
 *    Library install, not being removed) and echoes the catalogue fields of
 *    its release (tvcat_release) so the app's release_job() match holds;
 *  - POST /downloads runs the reference's pre-checks, then dl_add() with an
 *    explicit delivery ("direct" or "torbox"), never overwriting a file;
 *  - the job actions translate the app's verbs into dl_action() calls and
 *    answer the full jobs array.
 * Nothing here touches a drive on a polled path: drives come from the
 * tv-storage snapshot, pending bytes from the queue, TorBox from memory.
 *
 * Ported from Orbit Store backend/queue.c, backend/api.c and backend/debrid.c
 * (GPL-3.0-or-later): the create pre-checks and their order, the duplicate and
 * history rules, the per-action state rules, the archive password validator,
 * the TorBox host rule and every sentence below (the app translates exact
 * matches; map-downloads §12 lists them).
 */
#include "tvapi.h"

#include "catalog.h"
#include "dlx_debrid.h"
#include "http_client.h"
#include "log.h"
#include "settings.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

/* ---- sentences (map-downloads §12; reference i18n keys unless noted) ------- */
#define S_UNKNOWN_RELEASE "Unknown curated release."
#define S_DRIVE "Selected storage is not connected or writable."
#define S_FILENAME_BUSY "Another download uses this filename on that drive. Open Downloads to manage it."
#define S_RELEASE_HAS_JOB "This release already has a job on that drive. Open Downloads to resume or retry it."
#define S_CANCELLED_PARTIAL                                                                                       \
  "Open Downloads to resume this cancelled download, or reconnect its original drive and delete the partial file " \
  "before downloading again."
#define S_QUEUE_FULL "Queue is full. Clear finished or cancelled history to make room."
#define S_NO_SPACE "Not enough space after unfinished downloads. Free space or cancel a queued download."
#define S_SOURCE "Enable this source and acknowledge the download notice in Sources first."
#define S_TB_CONNECT "Connect TorBox in App settings first."
#define S_TB_REFRESH "Refresh TorBox in App settings before starting this download."
#define S_TB_OPTION "This option is not supported by TorBox."
#define S_TB_HOST "TorBox currently cannot download this host or file size. Choose the original source."
#define S_PROVIDER "This provider does not support direct console downloads yet."
#define S_DELIVERY "Choose direct or torbox delivery."
#define S_STATE_SAVE "Queue state cannot be saved. Check console storage."
#define S_NOT_RUNNING "This job is not running."
#define S_CANNOT_RESUME "This job cannot be resumed in its current state."
#define S_RECONNECT_DRIVE "Reconnect the original destination drive."
#define S_RESUME_SOURCE "Enable a supported source in Sources before resuming this download."
#define S_COMPLETE_CANCEL "Completed files cannot be deleted through cancellation."
#define S_ONLY_CANCELLED "Only cancelled downloads can be deleted."
#define S_REMOVE \
  "Only completed or cancelled history can be removed. Reconnect the original drive and delete any kept partial file first."
#define S_PASSWORD_STATE "Pause extraction or wait for it to finish before entering a password."
#define S_PASSWORD_LEN "Enter an archive password of up to 128 bytes."
#define S_PASSWORD_CTRL "The archive password cannot contain control characters."
#define S_PARTIAL_DRIVE "Reconnect the original drive before deleting the partial file."
#define S_UNKNOWN_DOWNLOAD "Unknown download."
#define S_UNKNOWN_ACTION "Unknown queue action."
#define S_UNKNOWN_DL_ACTION "Unknown download action."
#define S_EXPECTED_STORAGE "Expected storageId."
#define S_EXPECTED_DELETE "Expected deleteArchives."
#define S_SAVE_SETTING "Could not save the setting."
#define S_SAVE_DRIVE "Could not save the default drive."
#define S_BROWSER_GONE "This verification session is no longer active."
#define S_BROWSER_ACTION "Unknown browser action."
#define S_PREPARING_TB "Preparing with TorBox"
/* New sentences (no reference key yet; Appendix A of PORT-SPEC). */
#define S_LIBRARY_COPY "Wait for the Library copy to finish before downloading."
#define S_FILE_EXISTS "A file with this name is already on that drive. XENO STORE will not overwrite it."
#define S_FIRMWARE "Downloads are unavailable on this console's firmware."
#define S_EXTRACT_PAUSE "Extraction cannot be paused. Wait for it to finish."
#define S_STILL_STOPPING "This download is still stopping. Try again in a moment."
#define S_INSTALL_WAIT "Wait for the install to finish before removing it from history."
#define S_NO_LINK "This download has no link to refresh."
#define S_ARCHIVE_GONE "The archive is no longer on the drive. Remove this download and download it again."
#define S_LIMIT "Choose a whole number from 1 to 4."
#define S_INSTALL_FAILED "Install failed: "
#define S_DOWNLOAD_FAILED "Download failed."
#define S_BROWSER                                                                                  \
  "Vikingfile asks for a human check, so XENO STORE cannot fetch it by itself. Download it with " \
  "TorBox, or open this game in the browser version."

/* The app's own margin after a download (APPO/model.hpp:456). */
#define APP_MARGIN_BYTES (16LL << 20)
/* The reference's job-id and action captures are %23[^/] (backend/api.c). */
#define REF_SEGMENT_MAX 23
/* The reference allows 128 bytes. A password is never cut to fit: while
 * dl_job.extractPassword (and the extractor's copy) hold fewer, the longest
 * storable one is the limit and a longer one is refused with the sentence. */
#define REF_PASSWORD_MAX 128
#define PASSWORD_FIELD_MAX ((int)sizeof(((dl_job *)0)->extractPassword) - 1)
#define PASSWORD_MAX (PASSWORD_FIELD_MAX < REF_PASSWORD_MAX ? PASSWORD_FIELD_MAX : REF_PASSWORD_MAX)

/* ---- TorBox hosts per TV source (map-downloads §8) --------------------------
 * The catalogue sources the TV app can see, with the hosts TorBox lists them
 * under. Ported from Orbit Store backend/debrid.c (GPL-3.0-or-later). */
typedef struct tb_source { const char *sourceId; const char *domains[3]; } tb_source;
static const tb_source TB_SOURCES[] = {
  { "archive", { "archive.org", NULL, NULL } },
  { "vikingfile", { "vikingfile.com", "vik1ngfile.site", NULL } },
};

/* ---- constant dl_action() bodies --------------------------------------------- */
static pthread_once_t g_bodies_once = PTHREAD_ONCE_INIT;
static jdoc *g_body_empty, *g_body_keep, *g_body_delete;

static void bodies_init(void) {
  static const char empty[] = "{}", keep[] = "{\"deleteFiles\":false}", del[] = "{\"deleteFiles\":true}";
  g_body_empty = json_parse(empty, sizeof empty - 1, 8, 4);
  g_body_keep = json_parse(keep, sizeof keep - 1, 8, 4);
  g_body_delete = json_parse(del, sizeof del - 1, 8, 4);
  if (!g_body_empty || !g_body_keep || !g_body_delete) LOGE("tvapi downloads: out of memory for the action bodies");
}

/* 0 and *out, or -1 (out of memory at start). */
static int action_body(int kind, jv *out) {
  pthread_once(&g_bodies_once, bodies_init);
  jdoc *d = kind == 0 ? g_body_empty : kind == 1 ? g_body_keep : g_body_delete;
  if (!d) return -1;
  *out = json_root(d);
  return 0;
}
enum { BODY_EMPTY = 0, BODY_KEEP_FILES = 1, BODY_DELETE_FILES = 2 };

void tvdl_start(void) { pthread_once(&g_bodies_once, bodies_init); }

/* ---- small helpers ------------------------------------------------------------ */

static int st_active(dl_state s) { return s == DL_CONNECTING || s == DL_DOWNLOADING || s == DL_VERIFYING || s == DL_EXTRACTING; }
static int st_unfinished(dl_state s) { return s == DL_QUEUED || s == DL_PAUSED || st_active(s); }
static int st_complete(dl_state s) { return s == DL_DONE || s == DL_INSTALLED || s == DL_EXTRACTED; }

/* What the TV app may see: downloads only (not Library copies or installs),
 * never a row that is being removed. */
static int brief_visible(const dl_brief *b) { return strcmp(b->type, "download") == 0 && !b->synthetic && !b->removePending; }
/* A finished ZIP download whose extraction failed: the app shows an error row. */
static int extract_failed(const dl_brief *b) { return b->state == DL_DONE && strcmp(b->extractState, "failed") == 0; }
static int install_failed(const dl_brief *b) { return strcmp(b->installState, "failed") == 0; }
/* "complete" in the app's eyes (map-downloads §3.3): the download is finished. */
static int tv_complete(const dl_brief *b) { return (st_complete(b->state) && !extract_failed(b)) || b->state == DL_INSTALLING; }
/* TorBox fetches this job: chosen for it, resolved by it, or (web UI "auto")
 * resolving its link through it right now. */
static int is_torbox(const dl_brief *b) {
  return util_strieq(b->via, "torbox") || util_strieq(b->viaService, "torbox") ||
         (b->state == DL_CONNECTING && util_starts_with(b->phase, "unrestricting via TorBox"));
}

static int roots_equal(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  while (la > 1 && a[la - 1] == '/') la--;
  while (lb > 1 && b[lb - 1] == '/') lb--;
  return la == lb && strncmp(a, b, la) == 0;
}

/* The job's .xpart is still on the drive (action paths only: one stat). */
static int partial_exists(const dl_brief *b) {
  if (!b->path[0]) return 0;
  char xpart[1100];
  xsnprintf(xpart, sizeof xpart, "%s.xpart", b->path);
  return util_file_exists(xpart);
}

/* Copies `in` as valid UTF-8: invalid or truncated sequences become '?', so a
 * string cut by a fixed-size field never fails the app's strict parser. */
static void utf8_clean(const char *in, char *out, size_t cap) {
  size_t o = 0;
  const unsigned char *p = (const unsigned char *)(in ? in : "");
  while (*p && o + 1 < cap) {
    unsigned c = p[0];
    int n = 0;
    unsigned lo = 0x80, hi = 0xBF;
    if (c < 0x80) n = 1;
    else if (c >= 0xC2 && c <= 0xDF) n = 2;
    else if (c == 0xE0) { n = 3; lo = 0xA0; }
    else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) n = 3;
    else if (c == 0xED) { n = 3; hi = 0x9F; }
    else if (c == 0xF0) { n = 4; lo = 0x90; }
    else if (c >= 0xF1 && c <= 0xF3) n = 4;
    else if (c == 0xF4) { n = 4; hi = 0x8F; }
    int ok = n > 0;
    for (int k = 1; ok && k < n; k++) {
      unsigned d = p[k];
      if (d == 0) ok = 0;
      else if (k == 1 ? (d < lo || d > hi) : (d < 0x80 || d > 0xBF)) ok = 0;
    }
    if (!ok) { out[o++] = '?'; p++; continue; }
    if (o + (size_t)n >= cap) break;   /* no room for the whole character */
    memcpy(out + o, p, (size_t)n);
    o += (size_t)n;
    p += n;
  }
  if (cap) out[o] = 0;
}

static void kv_text(jw *w, const char *key, const char *s) {
  char buf[1200];
  utf8_clean(s, buf, sizeof buf);
  jw_kv_str(w, key, buf);
}

/* `msg` as a sentence: trimmed, with a final period unless it already ends in
 * one ('.', '!', '?', or a closing quote after one). */
static void sentence_from(const char *msg, char *out, size_t cap) {
  char tmp[400];
  xstrlcpy(tmp, msg ? msg : "", sizeof tmp);
  util_trim(tmp);
  if (!tmp[0]) { xstrlcpy(out, S_DOWNLOAD_FAILED, cap); return; }
  size_t n = strlen(tmp);
  char last = tmp[n - 1];
  if (last == '.' || last == '!' || last == '?') xstrlcpy(out, tmp, cap);
  else xsnprintf(out, cap, "%s.", tmp);
}

/* The TV error text of a XENO job error (map-downloads §5.3). */
static void tvdl_sentence(const dl_brief *b, char *out, size_t cap) {
  const char *code = b->errorCode;
  if (strcmp(code, "drive_missing") == 0) { xstrlcpy(out, S_RECONNECT_DRIVE, cap); return; }
  if (strcmp(code, "no_space") == 0) { xstrlcpy(out, S_NO_SPACE, cap); return; }
  if (is_torbox(b) && (strcmp(code, "debrid_not_connected") == 0 || strcmp(code, "debrid_bad_key") == 0)) {
    xstrlcpy(out, S_TB_REFRESH, cap);
    return;
  }
  sentence_from(b->error, out, cap);
}

/* `cat` is the job's TV release when it still is one, from the same game. */
static int release_of(const dl_brief *b, tvcat_rel *cat) {
  if (!b->releaseId[0] || tvcat_release(b->releaseId, cat) != 0) return 0;
  if (b->gameId[0] && strcmp(b->gameId, cat->gameId) != 0) return 0;   /* the same id in another game */
  return 1;
}

static int title_id_ok(const char *s) {
  if (strlen(s) != 9) return 0;
  for (int i = 0; i < 4; i++) if (s[i] < 'A' || s[i] > 'Z') return 0;
  for (int i = 4; i < 9; i++) if (s[i] < '0' || s[i] > '9') return 0;
  return 1;
}

/* ---- the job projection (map-downloads §3) ---------------------------------- */

static void job_json(jw *w, const dl_brief *b, const tvcat_rel *cat) {
  const char *status = "queued";
  const char *phase = "";
  char err[400] = "";
  int can_pw = 0, done_like = 0;
  int64_t total = cat ? cat->sizeBytes : (b->total > 0 ? b->total : 0);
  int64_t written = b->written > 0 ? b->written : 0;
  int64_t speed = 0, retry_at = 0, extracted = 0, extract_total = 0;

  switch (b->state) {
    case DL_QUEUED:
      if (b->nextRetryAt > 0) { status = "retrying"; retry_at = (int64_t)b->nextRetryAt; }
      break;
    case DL_CONNECTING:
      if (is_torbox(b) && util_starts_with(b->phase, "unrestricting")) { status = "queued"; phase = S_PREPARING_TB; }
      else if (b->restHit) status = "retrying";   /* waiting for the network after a wake-up: "Waiting to retry…" */
      else status = "downloading";
      break;
    case DL_DOWNLOADING:
      status = "downloading";
      speed = b->speed > 0 ? (int64_t)b->speed : 0;
      break;
    case DL_VERIFYING: status = "verifying"; break;
    case DL_EXTRACTING:
      status = "extracting";
      done_like = 1;   /* the download itself is complete */
      extracted = written;
      extract_total = b->total > 0 ? b->total : 0;
      if (extracted > extract_total) extracted = extract_total;
      break;
    case DL_PAUSED:
      status = "paused";
      if (b->error[0] || b->errorCode[0]) tvdl_sentence(b, err, sizeof err);
      break;
    case DL_FAILED:
      status = "error";
      tvdl_sentence(b, err, sizeof err);
      break;
    case DL_CANCELLED: status = "cancelled"; break;
    case DL_DONE:
      done_like = 1;
      if (extract_failed(b)) {
        status = "error";
        sentence_from(b->extractMessage[0] ? b->extractMessage : b->error, err, sizeof err);
        can_pw = strstr(b->extractMessage, "password") != NULL;
      } else {
        status = "complete";
        if (install_failed(b)) {
          char m[300];
          sentence_from(b->installMessage[0] ? b->installMessage : b->error, m, sizeof m);
          xsnprintf(err, sizeof err, "%s%s", S_INSTALL_FAILED, m);
        }
      }
      break;
    case DL_INSTALLING:
    case DL_INSTALLED:
    case DL_EXTRACTED:
      status = "complete";
      done_like = 1;
      if (b->state == DL_INSTALLING && install_failed(b)) {
        char m[300];
        sentence_from(b->installMessage[0] ? b->installMessage : b->error, m, sizeof m);
        xsnprintf(err, sizeof err, "%s%s", S_INSTALL_FAILED, m);
      }
      break;
    default: break;
  }

  int64_t received;
  if (done_like) received = total > 0 ? total : written;
  else {
    received = written;
    /* XENO keeps the server's size in total; the app compares the catalogue size */
    if (cat && b->total > 0 && b->total != cat->sizeBytes) received = (int64_t)((double)written * (double)cat->sizeBytes / (double)b->total);
  }
  if (received < 0) received = 0;
  if (total > 0 && received > total) received = total;
  if (done_like && total == 0) total = received;

  char title_id[16];
  xstrlcpy(title_id, cat ? cat->titleId : b->titleId, sizeof title_id);
  util_upper(title_id);
  if (!title_id_ok(title_id)) title_id[0] = 0;
  char storage_id[64];
  tvdl_id_for_root(b->root, storage_id, sizeof storage_id);
  const char *path = (b->state == DL_EXTRACTED && b->extractDir[0]) ? b->extractDir : b->path;

  jw_obj_begin(w);
  jw_kv_str(w, "id", b->id);
  kv_text(w, "releaseId", b->releaseId);
  kv_text(w, "gameId", b->gameId);
  jw_kv_str(w, "titleId", title_id);
  kv_text(w, "title", cat && !b->title[0] ? cat->title : b->title);
  kv_text(w, "format", cat ? cat->format : b->format);
  kv_text(w, "filename", cat ? cat->filename : b->filename);
  jw_kv_str(w, "storageId", storage_id);
  kv_text(w, "path", path);
  jw_kv_str(w, "status", status);
  kv_text(w, "error", err);
  jw_kv_str(w, "verification", b->verified ? "sha256" : "size");
  jw_kv_str(w, "delivery", is_torbox(b) ? "torbox" : "direct");
  jw_kv_str(w, "deliveryPhase", phase);
  jw_kv_bool(w, "canRefreshLink", 0);
  jw_kv_bool(w, "canEnterArchivePassword", can_pw);
  jw_kv_i64(w, "received", received);
  jw_kv_i64(w, "total", total);
  jw_kv_i64(w, "speed", speed);
  jw_kv_i64(w, "retryAt", retry_at);
  jw_kv_i64(w, "extracted", extracted);
  jw_kv_i64(w, "extractTotal", extract_total);
  jw_kv_int(w, "order", b->index + 1);
  jw_obj_end(w);
}

/* Writes the bare jobs array into a fresh writer. 0, or -1 out of memory. */
static int jobs_writer(jw *w) {
  dl_brief *b = NULL;
  int n = dl_brief_list(&b);
  if (n < 0) return -1;
  tvcat_rel *cat = malloc(sizeof *cat);
  if (!cat) { free(b); return -1; }
  tv_arr_begin(w);
  for (int i = 0; i < n; i++) {
    if (!brief_visible(&b[i])) continue;
    int have = release_of(&b[i], cat);
    job_json(w, &b[i], have ? cat : NULL);
  }
  free(cat);
  free(b);
  return 0;
}

static void send_jobs(api_ctx *c) {
  jw w;
  if (jobs_writer(&w) != 0) { tv_error(c, 503, TV_MSG_LOW_MEMORY); return; }
  tv_arr_send(c, 200, &w);
}

void tvdl_h_jobs(api_ctx *c) { send_jobs(c); }

/* ---- TorBox (map-downloads §8; PORT-SPEC 1.5.3) ------------------------------ */

static const tb_source *tb_source_for(const char *sourceId) {
  for (size_t i = 0; i < XS_ARRAY_LEN(TB_SOURCES); i++)
    if (strcmp(TB_SOURCES[i].sourceId, sourceId) == 0) return &TB_SOURCES[i];
  return NULL;
}

/* 1 listed (limit = TorBox's per-file limit for it, 0 = none), 0 not listed,
 * -1 unknown (no TorBox host list yet: optimistic). */
static int tb_host_state(const tb_source *s, int64_t *limit) {
  int unknown = 0;
  *limit = 0;
  for (int i = 0; i < 3 && s->domains[i]; i++) {
    int64_t l = 0;
    int r = dlx_debrid_host_info("torbox", s->domains[i], &l);
    if (r == 1) { *limit = l > 0 ? l : 0; return 1; }
    if (r < 0) unknown = 1;
  }
  return unknown ? -1 : 0;
}

/* NULL when TorBox may fetch this release, else the refusal sentence (409).
 * Ported from Orbit Store backend/debrid.c debrid_prepare_locked (GPL-3.0-or-later). */
static const char *tb_refusal(const tvcat_rel *cat) {
  if (!dlx_debrid_connected("torbox")) return S_TB_CONNECT;
  if (dlx_debrid_busy("torbox")) return S_TB_REFRESH;
  const tb_source *s = tb_source_for(cat->sourceId);
  if (!s) return S_TB_OPTION;
  int64_t limit = 0;
  int st = tb_host_state(s, &limit);
  if (st == 0 || (st == 1 && limit > 0 && cat->sizeBytes > limit)) return S_TB_HOST;
  return NULL;
}

void tvdl_h_debrid(api_ctx *c) {
  int connected = dlx_debrid_connected("torbox");
  jw w;
  tv_obj_begin(&w);
  jw_kv_str(&w, "provider", "torbox");
  jw_kv_bool(&w, "available", xhttp_available());
  jw_kv_bool(&w, "connected", connected);
  jw_kv_bool(&w, "busy", dlx_debrid_busy("torbox"));
  jw_key(&w, "hosts");
  jw_arr_begin(&w);
  for (size_t i = 0; i < XS_ARRAY_LEN(TB_SOURCES); i++) {
    int64_t limit = 0;
    int st = tb_host_state(&TB_SOURCES[i], &limit);
    jw_obj_begin(&w);
    jw_kv_str(&w, "sourceId", TB_SOURCES[i].sourceId);
    jw_kv_bool(&w, "available", connected && st != 0);
    jw_kv_i64(&w, "maxFileSize", st == 1 ? limit : 0);
    jw_obj_end(&w);
  }
  jw_arr_end(&w);
  tv_obj_send(c, 200, &w);
}

/* ---- settings helpers ------------------------------------------------------------ */

/* settings_merge_checked({"downloads":{<key>:<value>}}); kind 's' string, 'i' int, 'b' bool.
 * 0, or a SETTINGS_ERR_*: on SETTINGS_ERR_SAVE settings.c has already put the
 * previous values back, so a "could not save" reply leaves nothing changed. */
static int merge_download_setting(const char *key, char kind, const char *s, int64_t i) {
  jw p;
  jw_init(&p);
  jw_obj_begin(&p);
  jw_key(&p, "downloads");
  jw_obj_begin(&p);
  if (kind == 's') jw_kv_str(&p, key, s);
  else if (kind == 'b') jw_kv_bool(&p, key, i != 0);
  else jw_kv_i64(&p, key, i);
  jw_obj_end(&p);
  jw_obj_end(&p);
  size_t len = 0;
  char *txt = jw_detach(&p, &len);
  if (!txt) return SETTINGS_ERR_INVALID;
  jdoc *d = json_parse(txt, len, 32, 8);
  free(txt);
  if (!d) return SETTINGS_ERR_INVALID;
  char err[160] = "";
  int rc = settings_merge_checked(json_root(d), err, sizeof err);
  json_free(d);
  if (rc != 0) LOGW("tvapi downloads: downloads.%s not saved (%s)", key, err[0] ? err : "invalid");
  return rc;
}

/* Sets downloads.defaultRoot to `root` unless it already is. 0 or a SETTINGS_ERR_*. */
static int set_default_root(const char *root) {
  char cur[256];
  settings_default_root(cur, sizeof cur);
  if (roots_equal(cur, root)) return 0;
  return merge_download_setting("defaultRoot", 's', root, 0);
}

/* ---- POST /api/v1/downloads (map-downloads §4) -------------------------------- */

/* Ported from Orbit Store backend/queue.c create_locked (GPL-3.0-or-later): the
 * duplicate rules. Returns NULL, or the 409 sentence. History records of this
 * release on this drive that may go are listed in hist[]. */
typedef struct create_ctx {
  const char *rid;
  const tvcat_rel *cat;
  const tv_drive *drive;
  const char *dest;            /* the file dl_add() will write */
  char hist[8][16];
  int nhist;
} create_ctx;

static const char *duplicate_refusal(const dl_brief *b, int n, create_ctx *x) {
  for (int i = 0; i < n; i++) {
    if (strcmp(b[i].type, "copy") == 0 && !b[i].removePending && (b[i].state == DL_QUEUED || st_active(b[i].state)))
      return S_LIBRARY_COPY;
  }
  for (int i = 0; i < n; i++) {
    const dl_brief *j = &b[i];
    if (j->removePending) continue;
    int same_release = brief_visible(j) && strcmp(j->releaseId, x->rid) == 0 && (!j->gameId[0] || strcmp(j->gameId, x->cat->gameId) == 0);
    if (same_release && roots_equal(j->root, x->drive->path)) {
      if (j->state == DL_CANCELLED) {
        /* a kept partial is resumable; without one the record is history */
        if (j->written > 0 || partial_exists(j)) return S_CANCELLED_PARTIAL;
      } else if (st_complete(j->state) && !extract_failed(j)) {
        /* finished: history, unless its file is still there (never overwrite) */
        if (util_file_exists(j->path)) return S_FILE_EXISTS;
      } else {
        return S_RELEASE_HAS_JOB;
      }
      if (x->nhist < (int)XS_ARRAY_LEN(x->hist)) xstrlcpy(x->hist[x->nhist++], j->id, sizeof x->hist[0]);
      continue;
    }
    /* another job writing the same file on that drive */
    if (strcmp(j->path, x->dest) == 0 && !st_complete(j->state) && j->state != DL_CANCELLED && j->state != DL_INSTALLING)
      return S_FILENAME_BUSY;
  }
  return NULL;
}

/* dl_add() refusal -> (status, sentence) (map-downloads §4.3). */
static int add_error(const char *code, const char *msg, const char **sentence, char *buf, size_t cap) {
  if (strcmp(code, "downloads_unavailable") == 0) { *sentence = S_FIRMWARE; return 503; }
  if (strcmp(code, "oom") == 0) { *sentence = TV_MSG_LOW_MEMORY; return 503; }
  if (strcmp(code, "bad_release") == 0) { *sentence = S_UNKNOWN_RELEASE; return 404; }
  if (strcmp(code, "root_unavailable") == 0) { *sentence = S_DRIVE; return 409; }
  if (strcmp(code, "exists") == 0) {
    *sentence = strstr(msg, "already exists") ? S_FILE_EXISTS : S_FILENAME_BUSY;
    return 409;
  }
  if (strcmp(code, "queue_full") == 0) { *sentence = S_QUEUE_FULL; return 409; }
  if (strcmp(code, "no_space") == 0) { *sentence = S_NO_SPACE; return 409; }
  if (strcmp(code, "bad_request") == 0) { *sentence = S_DELIVERY; return 400; }
  sentence_from(msg, buf, cap);
  *sentence = buf;
  return 409;
}

void tvdl_h_create(api_ctx *c) {
  /* 1. delivery: absent, "", "direct" or "torbox" (backend/api.c + queue.c) */
  int torbox = 0;
  if (tv_body_has(c, "delivery")) {
    const char *d = NULL;
    if (!tv_body_str(c, "delivery", &d)) { tv_error(c, 400, S_DELIVERY); return; }
    if (strcmp(d, "torbox") == 0) torbox = 1;
    else if (d[0] && strcmp(d, "direct") != 0) { tv_error(c, 400, S_DELIVERY); return; }
  }
  /* 2. a TV-visible release */
  const char *rid = NULL;
  tvcat_rel cat;
  if (!tv_body_str(c, "releaseId", &rid) || !rid[0] || strlen(rid) >= sizeof cat.id || tvcat_release(rid, &cat) != 0) {
    tv_error(c, 404, S_UNKNOWN_RELEASE);
    return;
  }
  /* 3. its source is enabled and the download notice acknowledged */
  if (!tvcat_sources_acknowledged() || !settings_source_enabled(cat.sourceId)) { tv_error(c, 409, S_SOURCE); return; }
  /* 4. a browser-only provider (Vikingfile) cannot be fetched directly */
  if (!torbox && !cat.directAvailable) { tv_error(c, 409, S_PROVIDER); return; }
  /* 5. TorBox can take this host and size */
  if (torbox) {
    const char *why = tb_refusal(&cat);
    if (why) { tv_error(c, 409, why); return; }
  }
  if (!dl_state_healthy()) { tv_error(c, 503, S_STATE_SAVE); return; }
  /* 6. the drive is in the snapshot and writable */
  const char *sid = NULL;
  tv_drive drive;
  if (!tv_body_str(c, "storageId", &sid) || tvdl_drive_for_id(sid, &drive) != 0) { tv_error(c, 409, S_DRIVE); return; }

  /* the catalogue release itself: raw format (destination folder) and password */
  cat_game *g = malloc(sizeof *g);
  cat_release *r = malloc(sizeof *r);
  dl_brief *b = NULL;
  int n = -1;
  int status = 0;
  const char *sentence = NULL;
  char buf[400];
  char dest[1100], fname[256];
  if (!g || !r) { status = 503; sentence = TV_MSG_LOW_MEMORY; goto out; }
  if (catalog_find_release(cat.gameId, rid, g, r) != 0) { status = 404; sentence = S_UNKNOWN_RELEASE; goto out; }
  {
    char raw[256];
    if (r->filename[0]) xstrlcpy(raw, r->filename, sizeof raw);
    else util_url_filename(r->url, raw, sizeof raw);
    if (storage_sanitize_filename(raw, fname, sizeof fname) != 0) { status = 404; sentence = S_UNKNOWN_RELEASE; goto out; }
  }
  if (storage_join(drive.path, storage_category_for_format(r->format), fname, dest, sizeof dest) != 0) {
    status = 409;
    sentence = S_DRIVE;
    goto out;
  }

  /* 7-8. Library copy, duplicates and history (on one queue copy) */
  n = dl_brief_list(&b);
  if (n < 0) { status = 503; sentence = TV_MSG_LOW_MEMORY; goto out; }
  create_ctx x;
  memset(&x, 0, sizeof x);
  x.rid = rid;
  x.cat = &cat;
  x.drive = &drive;
  x.dest = dest;
  sentence = duplicate_refusal(b, n, &x);
  if (sentence) { status = 409; goto out; }
  if (n - x.nhist >= DL_MAX_JOBS) { status = 409; sentence = S_QUEUE_FULL; goto out; }
  /* space after the unfinished downloads, as the app's Download button computes it */
  {
    int64_t pending = dl_remaining_on_root(drive.path);
    int64_t need = cat.sizeBytes + (cat.extractedBytes > 0 ? cat.extractedBytes : 0);
    if (tvdl_projected_free(&drive, pending) - need < APP_MARGIN_BYTES) { status = 409; sentence = S_NO_SPACE; goto out; }
  }
  /* never overwrite: neither the file nor a partial of it */
  {
    char xpart[1120];
    xsnprintf(xpart, sizeof xpart, "%s.xpart", dest);
    if (util_file_exists(dest) || util_file_exists(xpart)) { status = 409; sentence = S_FILE_EXISTS; goto out; }
  }

  /* 9. the history records of this release on this drive make room (the
   * reference reuses their slot), then the job goes in */
  for (int i = 0; i < x.nhist; i++) {
    jv keep;
    char code[32] = "", msg[256] = "";
    if (action_body(BODY_KEEP_FILES, &keep) != 0) { status = 503; sentence = TV_MSG_LOW_MEMORY; goto out; }
    if (dl_action(x.hist[i], "remove", keep, code, sizeof code, msg, sizeof msg) != 0 && strcmp(code, "not_found") != 0)
      LOGW("tvapi downloads: history record %s not removed (%s)", x.hist[i], msg);
  }
  {
    dl_add_req req;
    memset(&req, 0, sizeof req);
    jv none = { NULL, 0xFFFFFFFFu };
    req.body = none;
    req.mirrors = none;
    req.gameId = cat.gameId;
    req.releaseId = rid;
    req.root = drive.path;
    req.via = torbox ? "torbox" : "direct";   /* never the debrid.preferred fallback */
    req.password = r->password;
    req.verify = -1;
    req.autoInstall = -1;
    req.connections = 0;
    req.overwrite = 0;
    char id[16] = "", code[32] = "", msg[256] = "";
    if (dl_add(&req, id, sizeof id, code, sizeof code, msg, sizeof msg) != 0) {
      status = add_error(code, msg, &sentence, buf, sizeof buf);
      LOGI("tvapi downloads: %s refused (%s: %s)", rid, code, msg);
      goto out;
    }
    LOGI("tvapi downloads: %s queued as %s on %s (%s)", rid, id, drive.id, torbox ? "TorBox" : "direct");
    /* the chosen drive becomes the default one (backend/queue.c) */
    if (set_default_root(drive.path) != 0) LOGW("tvapi downloads: the default drive was not saved");
    if (tv_pref_auto()) tv_pref_auto_set(0);
    jw w;
    tv_obj_begin(&w);
    jw_kv_str(&w, "id", id);
    tv_obj_send(c, 201, &w);
  }

out:
  free(b);
  free(g);
  free(r);
  if (sentence) tv_error(c, status, sentence);
}

/* ---- POST /api/v1/downloads/{id}/{action} (map-downloads §5) --------------------- */

typedef struct act_result { int status; const char *sentence; } act_result;

#define REFUSE(st, s) do { res->status = (st); res->sentence = (s); return -1; } while (0)

/* One dl_action() call. 0 ok; else *code says why ("not_found", "bad_state", ...). */
static int xeno_action(const char *id, const char *action, jv body, char *code, size_t ccap) {
  char msg[256] = "";
  code[0] = 0;
  int rc = dl_action(id, action, body, code, ccap, msg, sizeof msg);
  if (rc != 0) LOGD("tvapi downloads: %s %s refused (%s: %s)", action, id, code, msg);
  return rc;
}

static int run_xeno(const dl_brief *b, const char *action, int body_kind, const char *bad_state, act_result *res) {
  jv body;
  if (action_body(body_kind, &body) != 0) REFUSE(503, TV_MSG_LOW_MEMORY);
  char code[32];
  if (xeno_action(b->id, action, body, code, sizeof code) == 0) return 0;
  if (strcmp(code, "not_found") == 0) REFUSE(404, S_UNKNOWN_DOWNLOAD);
  REFUSE(409, bad_state);
}

static int act_pause(const dl_brief *b, act_result *res) {
  /* pausing mid-extraction would leave the extraction marked as running */
  if (b->state == DL_EXTRACTING) REFUSE(409, S_EXTRACT_PAUSE);
  return run_xeno(b, "pause", BODY_EMPTY, S_NOT_RUNNING, res);
}

/* resume and retry are one action in the reference (backend/queue.c). */
static int act_resume(const dl_brief *b, int retry, act_result *res) {
  if (extract_failed(b)) {
    /* the archive is downloaded: extract it again */
    if (b->running) REFUSE(409, S_CANNOT_RESUME);
    if (!tvdl_root_present(b->root)) REFUSE(409, S_RECONNECT_DRIVE);
    return run_xeno(b, "extract", BODY_EMPTY, S_ARCHIVE_GONE, res);
  }
  if (b->releaseId[0] && b->sourceId[0] && (!tvcat_sources_acknowledged() || !settings_source_enabled(b->sourceId)))
    REFUSE(409, S_RESUME_SOURCE);
  if (is_torbox(b) && !dlx_debrid_connected("torbox")) REFUSE(409, S_TB_CONNECT);
  int resumable = b->state == DL_PAUSED || b->state == DL_FAILED || b->state == DL_CANCELLED || (b->state == DL_QUEUED && b->nextRetryAt > 0);
  if (!resumable) REFUSE(409, S_CANNOT_RESUME);
  if (!tvdl_root_present(b->root)) REFUSE(409, S_RECONNECT_DRIVE);
  tv_drive d;
  if (tvdl_drive_for_root(b->root, &d) == 0) {
    int64_t remaining = b->total > b->written ? b->total - b->written : 0;
    int64_t pending = dl_remaining_on_root(d.path) - (st_unfinished(b->state) ? remaining : 0);
    if (pending < 0) pending = 0;
    if (remaining > 0 && d.free - pending < remaining + DL_RESERVE_BYTES) REFUSE(409, S_NO_SPACE);
  }
  return run_xeno(b, retry || b->state == DL_FAILED ? "retry" : "resume", BODY_EMPTY, S_CANNOT_RESUME, res);
}

static int act_cancel(const api_ctx *c, const dl_brief *b, act_result *res) {
  int del = 0;
  tv_body_bool(c, "deletePartial", &del);   /* only a JSON true deletes (reference: cJSON_IsTrue) */
  if (tv_complete(b)) REFUSE(409, S_COMPLETE_CANCEL);
  if (extract_failed(b)) {
    /* the archive is a finished file: the row goes, and the archive only when asked */
    if (b->running) REFUSE(409, S_STILL_STOPPING);
    if (del && !tvdl_root_present(b->root)) REFUSE(409, S_PARTIAL_DRIVE);
    return run_xeno(b, "remove", del ? BODY_DELETE_FILES : BODY_KEEP_FILES, S_STILL_STOPPING, res);
  }
  if (b->state == DL_CANCELLED) {
    /* already cancelled: nothing to stop; a kept partial goes only with the row */
    if (!del || (b->written <= 0 && !partial_exists(b))) return 0;
    if (!tvdl_root_present(b->root)) REFUSE(409, S_PARTIAL_DRIVE);
    return run_xeno(b, "remove", BODY_DELETE_FILES, S_STILL_STOPPING, res);
  }
  /* a worker that was asked to stop has not exited yet */
  if (b->running && !st_active(b->state)) REFUSE(409, S_STILL_STOPPING);
  if (del && b->written > 0 && !tvdl_root_present(b->root)) REFUSE(409, S_PARTIAL_DRIVE);
  return run_xeno(b, "cancel", del ? BODY_DELETE_FILES : BODY_KEEP_FILES, S_STILL_STOPPING, res);
}

static int act_delete(const dl_brief *b, int forget, act_result *res) {
  if (b->state != DL_CANCELLED) REFUSE(409, S_ONLY_CANCELLED);
  if (forget) return run_xeno(b, "remove", BODY_KEEP_FILES, S_ONLY_CANCELLED, res);   /* never touches a drive */
  if (!tvdl_root_present(b->root)) REFUSE(409, S_PARTIAL_DRIVE);
  return run_xeno(b, "remove", BODY_DELETE_FILES, S_ONLY_CANCELLED, res);
}

static int act_remove(const dl_brief *b, act_result *res) {
  /* the install still owns the file (and its /pkgsrv token) */
  if (b->state == DL_INSTALLING && !install_failed(b)) REFUSE(409, S_INSTALL_WAIT);
  int removable = tv_complete(b) || (b->state == DL_CANCELLED && b->written <= 0 && !partial_exists(b));
  if (!removable) REFUSE(409, S_REMOVE);
  return run_xeno(b, "remove", BODY_KEEP_FILES, S_REMOVE, res);
}

/* Ported from Orbit Store backend/queue.c job_password_locked (GPL-3.0-or-later):
 * the state rule first, then the password rules. */
static int act_password(const api_ctx *c, const dl_brief *b, act_result *res) {
  if (!extract_failed(b) || b->running) REFUSE(409, S_PASSWORD_STATE);
  const char *pw = NULL;
  if (!tv_body_str(c, "password", &pw) || !pw[0] || strlen(pw) > (size_t)PASSWORD_MAX) REFUSE(400, S_PASSWORD_LEN);
  for (const unsigned char *p = (const unsigned char *)pw; *p; p++)
    if (*p < 32 || *p == 127) REFUSE(400, S_PASSWORD_CTRL);
  if (!tvdl_root_present(b->root)) REFUSE(409, S_RECONNECT_DRIVE);
  /* the request body carries {"password"}: dl_action stores it and extracts again
   * (the password is never logged) */
  char code[32];
  if (xeno_action(b->id, "extract", c->body, code, sizeof code) == 0) return 0;
  if (strcmp(code, "not_found") == 0) REFUSE(404, S_UNKNOWN_DOWNLOAD);
  REFUSE(409, S_ARCHIVE_GONE);
}

void tvdl_h_action(api_ctx *c) {
  const char *id = c->nparams > 0 ? c->params[0] : "";
  const char *action = c->nparams > 1 ? c->params[1] : "";
  if (!id[0] || !action[0] || strlen(id) > REF_SEGMENT_MAX || strlen(action) > REF_SEGMENT_MAX) {
    tv_error(c, 404, S_UNKNOWN_DL_ACTION);
    return;
  }
  dl_brief *b = malloc(sizeof *b);
  if (!b) { tv_error(c, 503, TV_MSG_LOW_MEMORY); return; }
  if (dl_brief_get(id, b) != 0 || !brief_visible(b)) { free(b); tv_error(c, 404, S_UNKNOWN_DOWNLOAD); return; }
  act_result res = { 0, NULL };
  int rc;
  if (strcmp(action, "pause") == 0) rc = act_pause(b, &res);
  else if (strcmp(action, "resume") == 0) rc = act_resume(b, 0, &res);
  else if (strcmp(action, "retry") == 0) rc = act_resume(b, 1, &res);
  else if (strcmp(action, "cancel") == 0) rc = act_cancel(c, b, &res);
  else if (strcmp(action, "delete") == 0) rc = act_delete(b, 0, &res);
  else if (strcmp(action, "forget") == 0) rc = act_delete(b, 1, &res);
  else if (strcmp(action, "remove") == 0) rc = act_remove(b, &res);
  else if (strcmp(action, "archive-password") == 0) rc = act_password(c, b, &res);
  else if (strcmp(action, "refresh-link") == 0) { res.status = 409; res.sentence = S_NO_LINK; rc = -1; }
  else { res.status = 404; res.sentence = S_UNKNOWN_ACTION; rc = -1; }
  if (rc == 0) LOGI("tvapi downloads: %s %s", action, id);
  free(b);
  if (rc != 0) { tv_error(c, res.status, res.sentence); return; }
  send_jobs(c);
}

/* POST /api/v1/sources side effect: the downloads of a source that was just
 * turned off stop (queued and active ones are paused; an extraction finishes). */
int tvdl_pause_source(const char *sourceId) {
  if (!sourceId || !sourceId[0]) return 0;
  dl_brief *b = NULL;
  int n = dl_brief_list(&b);
  int paused = 0;
  jv body;
  if (n > 0 && action_body(BODY_EMPTY, &body) == 0) {
    for (int i = 0; i < n; i++) {
      const dl_brief *j = &b[i];
      if (!brief_visible(j) || strcmp(j->sourceId, sourceId) != 0) continue;
      if (j->state != DL_QUEUED && (!st_active(j->state) || j->state == DL_EXTRACTING)) continue;
      char code[32];
      if (xeno_action(j->id, "pause", body, code, sizeof code) == 0) paused++;
    }
  }
  free(b);
  if (paused) LOGI("tvapi downloads: %d download(s) of %s paused (source turned off)", paused, sourceId);
  return paused;
}

/* ---- GET /api/v1/storage (map-downloads §6) ----------------------------------------- */

void tvdl_h_storage(api_ctx *c) {
  tv_drive *d = malloc(TVDL_MAX_DRIVES * sizeof *d);
  if (!d) { tv_error(c, 503, TV_MSG_LOW_MEMORY); return; }
  int n = tvdl_snapshot(d, TVDL_MAX_DRIVES, 1, NULL);
  jw w;
  tv_arr_begin(&w);
  for (int i = 0; i < n; i++) {
    char root[256], path[300];
    xstrlcpy(root, d[i].path, sizeof root);
    size_t l = strlen(root);
    while (l > 1 && root[l - 1] == '/') root[--l] = 0;
    xsnprintf(path, sizeof path, "%s/homebrew", root);
    int64_t free_b = d[i].free > 0 ? d[i].free : 0;
    int64_t total_b = d[i].total > 0 ? d[i].total : 0;
    int64_t pending = dl_remaining_on_root(d[i].path);
    if (pending < 0) pending = 0;
    jw_obj_begin(&w);
    jw_kv_str(&w, "id", d[i].id);
    kv_text(&w, "label", d[i].label);
    kv_text(&w, "path", path);
    jw_kv_i64(&w, "freeBytes", free_b);
    jw_kv_i64(&w, "totalBytes", total_b);
    jw_kv_i64(&w, "pendingBytes", pending);
    jw_kv_i64(&w, "projectedFreeBytes", free_b - pending - TVDL_PROJECTION_MARGIN);
    jw_kv_bool(&w, "external", d[i].external);
    jw_obj_end(&w);
  }
  free(d);
  tv_arr_send(c, 200, &w);
}

/* ---- download settings (map-downloads §7) ------------------------------------------- */

/* POST /api/v1/storage/preferred {"storageId":"<id>"|""} */
void tvdl_h_preferred(api_ctx *c) {
  const char *sid = NULL;
  if (!tv_body_str(c, "storageId", &sid) || strlen(sid) > 63) { tv_error(c, 400, S_EXPECTED_STORAGE); return; }
  tv_drive d;
  if (!sid[0]) {
    /* "Choose automatically": the first external drive, else the internal one
     * (APPO/model.cpp default_drive). XENO keeps a path, so store that path and
     * remember that the choice is automatic. */
    if (tvdl_auto_drive(&d) == 0 && set_default_root(d.path) == SETTINGS_ERR_SAVE) { tv_error(c, 503, S_SAVE_DRIVE); return; }
    if (!tv_pref_auto()) tv_pref_auto_set(1);   /* a failed tvapi.json write keeps the flag until restart (logged) */
  } else {
    if (tvdl_drive_for_id(sid, &d) != 0) { tv_error(c, 409, S_DRIVE); return; }
    if (set_default_root(d.path) == SETTINGS_ERR_SAVE) { tv_error(c, 503, S_SAVE_DRIVE); return; }
    if (tv_pref_auto()) tv_pref_auto_set(0);
  }
  jw w;
  tv_obj_begin(&w);
  jw_kv_str(&w, "preferredStorage", sid);
  tv_obj_send(c, 200, &w);
}

/* POST /api/v1/settings/downloads {"concurrentDownloads":1..4} (R7). */
void tvdl_h_limit(api_ctx *c) {
  int64_t v = 0;
  if (!tv_body_int(c, "concurrentDownloads", &v) || v < 1 || v > 4) { tv_error(c, 400, S_LIMIT); return; }
  if (merge_download_setting("concurrentJobs", 'i', NULL, v) != 0) { tv_error(c, 503, S_SAVE_SETTING); return; }
  jw w;
  tv_obj_begin(&w);
  jw_kv_int(&w, "concurrentDownloads", settings_concurrent_jobs());
  tv_obj_send(c, 200, &w);
}

/* POST /api/v1/settings/extraction {"deleteArchives":bool} */
void tvdl_h_extraction(api_ctx *c) {
  int on = 0;
  if (!tv_body_bool(c, "deleteArchives", &on)) { tv_error(c, 400, S_EXPECTED_DELETE); return; }
  if (merge_download_setting("deleteArchiveAfterExtract", 'b', NULL, on) != 0) { tv_error(c, 503, S_SAVE_SETTING); return; }
  jw w;
  tv_obj_begin(&w);
  jw_kv_bool(&w, "deleteArchives", settings_delete_archives());
  tv_obj_send(c, 200, &w);
}

/* ---- /api/v1/browser: version 1 has no provider capture (map-downloads §9.2) ----------
 * Never 404: the app would tell the player to update. GET is always the idle
 * state with the explanation; start and cancel are refused with a sentence. */
void tvdl_h_browser(api_ctx *c) {
  if (tv_is_post(c)) {
    const char *action = "";
    tv_body_str(c, "action", &action);
    if (strcmp(action, "start") == 0) tv_error(c, 409, S_BROWSER);
    else if (strcmp(action, "cancel") == 0) tv_error(c, 409, S_BROWSER_GONE);
    else tv_error(c, 400, S_BROWSER_ACTION);
    return;
  }
  jw w;
  tv_obj_begin(&w);
  jw_kv_bool(&w, "available", 0);
  jw_kv_bool(&w, "active", 0);
  jw_kv_str(&w, "id", "");
  jw_kv_str(&w, "state", "idle");
  jw_kv_str(&w, "title", "");
  jw_kv_str(&w, "jobId", "");
  jw_kv_str(&w, "message", S_BROWSER);
  tv_obj_send(c, 200, &w);
}
