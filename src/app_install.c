/*
 * app_install.c — see app_install.h.
 *
 * The job: read the PPSA99500.ffpkg line of <base>/SHA256SUMS, stream
 * <base>/PPSA99500.ffpkg into <homebrew>/PPSA99500.ffpkg.part while hashing
 * it, check the hash, fsync and rename over PPSA99500.ffpkg; ShadowMountPlus
 * registers it on its own rescan (1.7+ also gets a best-effort scan request).
 * The .part is removed on every failure and on cancel; the
 * old package stays in place until the new one is complete and verified.
 * GitHub answers .../releases/latest/download/<asset> with a redirect to the
 * newest release's asset, so the URL never changes between versions.
 *
 * The pinned TV app install (app_install_start_pinned), the installed-state
 * test, the install record and their sentences follow the reference backend's
 * TV app installer. Ported from Orbit Store backend/tvapp.c (GPL-3.0-or-later).
 */
#include "app_install.h"
#include "config.h"
#include "cx_common.h"
#include "cx_jobs.h"
#include "cx_savestore.h"
#include "http_client.h"
#include "install.h"
#include "log.h"
#include "sha256.h"
#include "storage.h"
#include "util.h"
#include "xthread.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define APP_BASE "https://github.com/XENOKING123/XENO-STORE/releases/latest/download"
#define APP_FILE APP_TITLE_ID ".ffpkg"
#define APP_JOB_TYPE "app-install"
#define APP_SPACE_SLACK (16LL << 20)   /* headroom left on /data after the write */
#define APP_BUF (512u << 10)
#define APP_RECORD "tv-app.json"        /* <data>/tv-app.json: the install record */

static const char *const APP_HOSTS[] = {"github.com", "githubusercontent.com"};
static pthread_mutex_t app_mu = PTHREAD_MUTEX_INITIALIZER;   /* order: app_mu, then cx_jobs' lock */
static char app_job[16];   /* last job started (guarded by app_mu) */
static app_install_info g_info;   /* what the last job is doing (guarded by app_mu) */

/* A job's parameters. The web UI path fills base; the pinned path fills url,
 * sha256, size, version and replace. */
typedef struct app_arg {
  char base[512], dest[700];
  int pinned, replace;
  char url[700], sha256[65], version[32];
  int64_t size;
} app_arg;

/* ---- the job as the TV app reads it ------------------------------------- */

static void info_phase(const char *phase, int64_t received, int64_t total) {
  pthread_mutex_lock(&app_mu);
  xstrlcpy(g_info.phase, phase, sizeof g_info.phase);
  if (received >= 0) g_info.received = received;
  if (total >= 0) g_info.total = total;
  pthread_mutex_unlock(&app_mu);
}

static void info_error(const char *sentence) {
  pthread_mutex_lock(&app_mu);
  xstrlcpy(g_info.phase, "error", sizeof g_info.phase);
  xstrlcpy(g_info.error, sentence, sizeof g_info.error);
  pthread_mutex_unlock(&app_mu);
}

void app_install_snapshot(app_install_info *out) {
  int running = cx_jobs_active(APP_JOB_TYPE) > 0;   /* cx_jobs' own lock, before app_mu */
  pthread_mutex_lock(&app_mu);
  *out = g_info;
  pthread_mutex_unlock(&app_mu);
  out->running = running;
}

static const char *app_base(void) {
  const char *o = cx_base_override("APP");
  return o[0] ? o : APP_BASE;
}

void app_install_pkg_path(char *out, size_t cap) { xsnprintf(out, cap, "%s/%s", cx_data_homebrew_dir(), APP_FILE); }

static int host_allowed(const char *url) {
  char host[256];
  util_url_host(url, host, sizeof host);
  return host[0] && (cx_host_in(host, APP_HOSTS, XS_ARRAY_LEN(APP_HOSTS)) || cx_host_is_loopback(host));
}

static void record_path(char *out, size_t cap) { xsnprintf(out, cap, "%s/%s", cx_data_dir(), APP_RECORD); }

const char *app_install_state_name(int state) {
  static const char *const names[] = {"none", "orbit", "manual", "folder", "blocked"};
  return state >= 0 && state < (int)XS_ARRAY_LEN(names) ? names[state] : "blocked";
}

/* The record ties an image to the store's own install by size, time and
 * inode, so a copy the owner swapped in later reads as their own. An image
 * may exist beside a folder copy (even one copied in while an install was
 * downloading): the folder wins, so the store never adds a duplicate. */
int app_install_state(char *version, size_t cap) {
  if (version && cap) version[0] = 0;
  char p[760];
  struct stat st;
  xsnprintf(p, sizeof p, "%s/%s/eboot.bin", cx_data_homebrew_dir(), APP_TITLE_ID);
  if (lstat(p, &st) == 0) return APP_STATE_FOLDER;
  if (errno != ENOENT && errno != ENOTDIR) return APP_STATE_BLOCKED;
  app_install_pkg_path(p, sizeof p);
  if (lstat(p, &st) != 0) return errno == ENOENT ? APP_STATE_NONE : APP_STATE_BLOCKED;
  if (!S_ISREG(st.st_mode)) return APP_STATE_BLOCKED;
  int state = APP_STATE_MANUAL;
  char rp[700];
  record_path(rp, sizeof rp);
  size_t len = 0;
  char *txt = util_read_file(rp, &len, 4096);
  jdoc *d = txt ? json_parse(txt, len, 64, 4) : NULL;
  free(txt);
  if (d) {
    jv r = json_root(d), sz = jv_get(r, "size"), mt = jv_get(r, "modified"), in = jv_get(r, "inode");
    if (jv_type(r) == JV_OBJ && jv_type(sz) == JV_NUM && jv_type(mt) == JV_NUM && jv_type(in) == JV_NUM &&
        jv_i64(sz, -1) == (int64_t)st.st_size && jv_i64(mt, -1) == (int64_t)st.st_mtime && jv_i64(in, -1) == (int64_t)st.st_ino) {
      state = APP_STATE_STORE;
      if (version && cap) xstrlcpy(version, jv_get_str(r, "version", ""), cap);
    }
    json_free(d);
  }
  return state;
}

/* Writes the install record for the image now at dest (after the rename). */
static int write_record(const char *dest, const char *version, const char *sha256) {
  struct stat st;
  if (lstat(dest, &st) != 0) return -1;
  jw w;
  jw_init(&w);
  jw_obj_begin(&w);
  jw_kv_str(&w, "version", version);
  jw_kv_str(&w, "checksum", sha256);
  jw_kv_i64(&w, "size", (int64_t)st.st_size);
  jw_kv_i64(&w, "modified", (int64_t)st.st_mtime);
  jw_kv_i64(&w, "inode", (int64_t)st.st_ino);
  jw_obj_end(&w);
  char rp[700];
  record_path(rp, sizeof rp);
  return cx_write_json(rp, &w);
}

int app_install_release_url_ok(const char *url) {
  static const char tail[] = "/" APP_TITLE_ID ".ffpkg";
  size_t n = url ? strlen(url) : 0, tl = sizeof tail - 1;
  if (n <= tl || n >= 700 || strcmp(url + n - tl, tail) != 0) return 0;
  if (strpbrk(url, "?#\\ ") || strstr(url, "..")) return 0;
  for (const unsigned char *c = (const unsigned char *)url; *c; c++) if (*c < 0x21 || *c >= 0x7f) return 0;
  if (n > sizeof APP_RELEASE_BASE - 1 && strncmp(url, APP_RELEASE_BASE, sizeof APP_RELEASE_BASE - 1) == 0) return 1;
  xurl u;   /* host build: a loopback test server */
  return util_url_parse(url, &u) == 0 && !u.has_userinfo && cx_host_is_loopback(u.host);
}

void app_install_status_json(jw *w) {
  char path[700], appdir[700], ver[32] = "", job[16] = "", last[16];
  app_install_pkg_path(path, sizeof path);
  xsnprintf(appdir, sizeof appdir, "%s/%s", storage_user_app_dir(), APP_TITLE_ID);
  int64_t size = util_file_size(path);
  int registered = util_dir_exists(appdir);
  cx_installed_version(APP_TITLE_ID, ver, sizeof ver, NULL, 0);
  pthread_mutex_lock(&app_mu);
  xstrlcpy(last, app_job, sizeof last);
  if (app_job[0] && cx_jobs_active(APP_JOB_TYPE) > 0) xstrlcpy(job, app_job, sizeof job);
  pthread_mutex_unlock(&app_mu);
  jw_kv_bool(w, "installed", size > 0 || registered);
  jw_kv_str(w, "path", path);
  jw_kv_i64(w, "size", size > 0 ? size : 0);
  jw_kv_str(w, "version", ver);
  jw_kv_str(w, "job", job);
  jw_kv_str(w, "lastJob", last);
  jw_kv_str(w, "titleId", APP_TITLE_ID);
  jw_kv_bool(w, "file", size > 0);
  jw_kv_bool(w, "registered", registered);
}

/* Reads the release's SHA256SUMS ("<64 hex>  <file>" per line, sha256sum
 * format, "*" binary marker allowed) and returns the PPSA99500.ffpkg hash. */
static int fetch_sha256(const char *url, char out[65], char *err, size_t errcap) {
  cx_http o = {0};
  o.timeout_ms = 20000;
  o.max_bytes = 64 << 10;
  int status = 0;
  char *body = cx_fetch(url, &o, &status, NULL, NULL);
  if (!body || status != 200) {
    if (body) xsnprintf(err, errcap, "cannot read SHA256SUMS (HTTP %d)", status);
    else xsnprintf(err, errcap, "cannot read SHA256SUMS (%s)", xhttp_last_error());
    free(body);
    return -1;
  }
  int ok = 0;
  char *save = NULL;
  for (char *line = strtok_r(body, "\r\n", &save); line && !ok; line = strtok_r(NULL, "\r\n", &save)) {
    size_t n = 0;
    while (n < 64 && isxdigit((unsigned char)line[n])) n++;
    if (n != 64 || (line[64] != ' ' && line[64] != '\t')) continue;
    const char *name = line + 64;
    while (*name == ' ' || *name == '\t' || *name == '*') name++;
    if (strcmp(name, APP_FILE) != 0) continue;
    memcpy(out, line, 64);
    out[64] = 0;
    util_lower(out);
    ok = 1;
  }
  if (!ok) xsnprintf(err, errcap, "SHA256SUMS lists no hash for %s", APP_FILE);
  free(body);
  return ok ? 0 : -1;
}

static int fail(cx_job *j, const char *tmp, const char *code, const char *msg) {
  if (tmp) util_unlink_quiet(tmp);
  cx_job_fail(j, code, msg);
  info_error(msg);
  LOGW("app install: %s", msg);
  xthread_net(NULL, NULL);
  return -1;
}

static int write_all(int fd, const char *p, size_t n) {
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return -1;
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

/* Step 4 of both paths: ShadowMountPlus registers the tile on its own (it
 * rescans /data/homebrew every 15-60 s, recent versions also as soon as a
 * file lands there); 1.7+ takes a scan request on its API too. The job never
 * depends on it. */
static void finish(cx_job *j, const app_arg *a, int64_t done, const char *hex) {
  xthread_note("app install: shadowmount scan");
  cx_job_progress(j, done, done, "Asking ShadowMountPlus to register the app");
  char sm[160] = "";
  int sm_ok = install_shadowmount_scan(sm, sizeof sm);
  LOGI("app install: shadowmount scan: %s", sm);
  jw rw;
  jw_init(&rw);
  jw_obj_begin(&rw);
  jw_kv_str(&rw, "path", a->dest);
  jw_kv_i64(&rw, "size", done);
  jw_kv_str(&rw, "sha256", hex);
  jw_kv_bool(&rw, "verified", 1);
  if (a->pinned) jw_kv_str(&rw, "version", a->version);
  jw_kv_bool(&rw, "shadowMount", sm_ok);
  jw_kv_str(&rw, "shadowMountStatus", sm);
  jw_obj_end(&rw);
  cx_job_set_result(j, &rw);
  cx_job_progress(j, done, done, sm_ok ? "Installed; ShadowMountPlus adds the tile within about a minute" : "Installed; the tile appears once ShadowMountPlus scans /data/homebrew");
  info_phase("installed", done, -1);
}

static int app_worker(cx_job *j, void *p) {
  app_arg *a = p;
  char url[700], tmp[720], want[65], hex[65], err[256];
  xsnprintf(tmp, sizeof tmp, "%s.part", a->dest);

  /* 1. the published hash */
  xthread_note("app install: sha256");
  cx_job_progress(j, 0, 0, "Checking the latest release");
  xsnprintf(url, sizeof url, "%s/SHA256SUMS", a->base);
  if (fetch_sha256(url, want, err, sizeof err) != 0) return fail(j, NULL, "sha256_unavailable", err);
  if (cx_job_cancelled(j)) return fail(j, NULL, "cancelled", "cancelled");

  /* 2. the package */
  xthread_note("app install: download");
  xsnprintf(url, sizeof url, "%s/%s", a->base, APP_FILE);
  xhttp_req r;
  memset(&r, 0, sizeof r);
  r.method = "GET";
  r.url = url;
  r.range_start = r.range_end = -1;
  r.connect_timeout_ms = 15000;
  r.recv_timeout_ms = 30000;
  r.follow_redirects = 5;
  r.cancel_flag = &j->cancel;
  r.transfer = 1;
  xhttp_conn *c = NULL;
  xhttp_resp resp;
  if (xhttp_open(&r, &c, &resp) != 0) {
    xsnprintf(err, sizeof err, "download failed: %s", xhttp_last_error());
    return fail(j, NULL, cx_job_cancelled(j) ? "cancelled" : "download_failed", err);
  }
  const char *code = NULL;
  int64_t total = resp.content_length > 0 ? resp.content_length : 0, free_b = 0;
  if (resp.status != 200) { code = "download_failed"; xsnprintf(err, sizeof err, "%s: HTTP %d", APP_FILE, resp.status); }
  else if (resp.location[0] && !host_allowed(resp.location)) { code = "forbidden"; xsnprintf(err, sizeof err, "the download was redirected to a host that is not allowed"); }
  else if (util_istarts_with(resp.content_type, "text/html")) { code = "not_package"; xsnprintf(err, sizeof err, "the server returned a web page instead of %s", APP_FILE); }
  else if (total > APP_MAX_BYTES) { code = "too_large"; xsnprintf(err, sizeof err, "%s is larger than the %lld MiB limit", APP_FILE, (long long)(APP_MAX_BYTES >> 20)); }
  else if (storage_free_space(cx_data_homebrew_dir(), &free_b, NULL) == 0 && free_b < total + APP_SPACE_SLACK) {
    char need_s[32], free_s[32];
    util_fmt_bytes(total + APP_SPACE_SLACK, need_s, sizeof need_s);
    util_fmt_bytes(free_b, free_s, sizeof free_s);
    code = "no_space";
    xsnprintf(err, sizeof err, "not enough free space in %s (%s needed, %s free)", cx_data_homebrew_dir(), need_s, free_s);
  }
  if (code) { xhttp_close(c); return fail(j, NULL, code, err); }
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) { xsnprintf(err, sizeof err, "cannot write %s: %s", tmp, strerror(errno)); xhttp_close(c); return fail(j, NULL, "io", err); }
  char *buf = malloc(APP_BUF);
  if (!buf) { close(fd); xhttp_close(c); return fail(j, tmp, "oom", "out of memory"); }
  sha256_ctx h;
  sha256_init(&h);
  int64_t done = 0;
  cx_job_progress(j, 0, total, "Downloading");
  info_phase("downloading", 0, total);
  for (;;) {
    if (cx_job_cancelled(j)) { code = "cancelled"; xsnprintf(err, sizeof err, "cancelled"); break; }
    int n = xhttp_read(c, buf, APP_BUF);
    if (n < 0) { code = cx_job_cancelled(j) ? "cancelled" : "download_failed"; xsnprintf(err, sizeof err, "download failed: %s", xhttp_last_error()); break; }
    if (n == 0) break;
    if (done + n > APP_MAX_BYTES) { code = "too_large"; xsnprintf(err, sizeof err, "%s is larger than the %lld MiB limit", APP_FILE, (long long)(APP_MAX_BYTES >> 20)); break; }
    if (write_all(fd, buf, (size_t)n) != 0) { code = errno == ENOSPC ? "no_space" : "io"; xsnprintf(err, sizeof err, "write failed: %s", strerror(errno)); break; }
    sha256_update(&h, buf, (size_t)n);
    done += n;
    cx_job_progress(j, done, total, "Downloading");
    info_phase("downloading", done, -1);
  }
  free(buf);
  xhttp_close(c);
  xthread_net(NULL, NULL);
  if (!code && fsync(fd) != 0) { code = "io"; xsnprintf(err, sizeof err, "fsync failed: %s", strerror(errno)); }
  if (close(fd) != 0 && !code) { code = "io"; xsnprintf(err, sizeof err, "close failed: %s", strerror(errno)); }
  if (code) return fail(j, tmp, code, err);
  if (total > 0 && done != total) { xsnprintf(err, sizeof err, "short download (%lld of %lld bytes)", (long long)done, (long long)total); return fail(j, tmp, "download_failed", err); }
  if (done == 0) return fail(j, tmp, "download_failed", "the download was empty");

  /* 3. verify and swap in */
  xthread_note("app install: verify");
  cx_job_progress(j, done, done, "Verifying");
  info_phase("verifying", done, -1);
  uint8_t dg[32];
  sha256_final(&h, dg);
  sha256_digest_hex(dg, hex);
  if (strcmp(hex, want) != 0) {
    xsnprintf(err, sizeof err, "sha256 mismatch: got %.16s..., the release publishes %.16s...", hex, want);
    return fail(j, tmp, "sha256_mismatch", err);
  }
  info_phase("installing", done, -1);
  if (rename(tmp, a->dest) != 0) { xsnprintf(err, sizeof err, "cannot rename to %s: %s", a->dest, strerror(errno)); return fail(j, tmp, "io", err); }
  util_fsync_dir(a->dest);
  LOGI("app install: %s in place (%lld bytes, sha256 %s)", a->dest, (long long)done, hex);
  /* The owner asked for this release from the web UI: the store installed it,
   * so the TV app sees it as the store's copy (its version is not known here). */
  if (write_record(a->dest, "", hex) != 0) LOGW("app install: cannot write the install record %s", APP_RECORD);

  /* 4. ShadowMountPlus */
  finish(j, a, done, hex);
  return 0;
}

/* ---- the TV app's pinned install ------------------------------------------ */

/* The reference installer's sentences (the TV app shows them as they are). */
#define S_LARGER "The download was larger than the release says. Nothing was installed."
#define S_SAVE "Could not save the TV app on the console's internal storage."
#define S_SHA "The TV app failed its SHA-256 check. Nothing was installed."
#define S_NOT_IMAGE "The download isn't a TV app image. Nothing was installed."
#define S_REDIRECT "The TV app release redirected outside XENO STORE's allowed download hosts."
#define S_CLOSING "The TV app download stopped because XENO STORE is closing."
#define S_CANCELLED "The TV app install was cancelled. Nothing was installed."
#define S_APPEARED "Another copy of the TV app appeared while downloading. Nothing was replaced; check again to see what's installed."
#define S_NEWER "A newer TV app is already installed. Nothing was replaced."
#define S_RECORD "The TV app was installed, but XENO STORE could not record its version. It will show as a copy you placed yourself."

/* major.minor.patch[-beta.N] compare as the reference does: -2 when either
 * side is not a version. Ported from Orbit Store backend/updates.c
 * compare_version (GPL-3.0-or-later). */
static int version_parts(const char *v, unsigned parts[5]) {
  if (*v == 'v') v++;
  parts[3] = 1;
  parts[4] = 0;
  for (int i = 0; i < 5; i++) {
    if (i == 3) {
      if (!*v) return 1;
      if (strncmp(v, "-beta.", 6) != 0) return 0;
      v += 6;
      parts[3] = 0;
      continue;
    }
    unsigned digits = 0, value = 0;
    while (*v >= '0' && *v <= '9') {
      if (++digits > 6) return 0;
      value = value * 10 + (unsigned)(*v++ - '0');
    }
    if (!digits) return 0;
    parts[i] = value;
    if (i < 2 && *v++ != '.') return 0;
  }
  return *v == 0;
}

static int version_compare(const char *a, const char *b) {
  unsigned x[5], y[5];
  if (!version_parts(a, x) || !version_parts(b, y)) return -2;
  for (int i = 0; i < 5; i++)
    if (x[i] != y[i]) return x[i] > y[i] ? 1 : -1;
  return 0;
}

static int pinned_fail(cx_job *j, const char *tmp, const char *code, const char *sentence, const char *detail) {
  if (detail && detail[0]) LOGW("app install (TV): %s (%s)", sentence, detail);
  return fail(j, tmp, code, sentence);
}

static int app_worker_pinned(cx_job *j, void *p) {
  app_arg *a = p;
  char tmp[720], hex[65], err[256], detail[200];
  xsnprintf(tmp, sizeof tmp, "%s.part", a->dest);
  util_unlink_quiet(tmp);

  /* 1. download exactly the checked release */
  xthread_note("app install (TV): download %s", a->version);
  info_phase("downloading", 0, a->size);
  cx_job_progress(j, 0, a->size, "Downloading");
  xhttp_req r;
  memset(&r, 0, sizeof r);
  r.method = "GET";
  r.url = a->url;
  r.range_start = r.range_end = -1;
  r.connect_timeout_ms = 15000;
  r.recv_timeout_ms = 30000;
  r.follow_redirects = 5;
  r.cancel_flag = &j->cancel;
  r.transfer = 1;
  xhttp_conn *c = NULL;
  xhttp_resp resp;
  if (xhttp_open(&r, &c, &resp) != 0) {
    if (cx_job_cancelled(j)) return pinned_fail(j, NULL, "cancelled", g_cfg.stop_requested ? S_CLOSING : S_CANCELLED, NULL);
    xsnprintf(err, sizeof err, "TV app download failed (%s). Try again later.", xhttp_last_error());
    return pinned_fail(j, NULL, "download_failed", err, NULL);
  }
  int64_t free_b = 0;
  const char *code = NULL, *sentence = NULL;
  detail[0] = 0;
  if (resp.status != 200) {
    xsnprintf(err, sizeof err, "TV app download failed (HTTP %d). Try again later.", resp.status);
    code = "download_failed";
    sentence = err;
  } else if (resp.location[0] && !host_allowed(resp.location)) {
    code = "forbidden";
    sentence = S_REDIRECT;
    xstrlcpy(detail, resp.location, sizeof detail);
  } else if (util_istarts_with(resp.content_type, "text/html")) {
    code = "not_package";
    sentence = S_NOT_IMAGE;
  } else if (resp.content_length > a->size) {
    code = "too_large";
    sentence = S_LARGER;
  } else if (storage_free_space(cx_data_homebrew_dir(), &free_b, NULL) == 0 && free_b < a->size + APP_SPACE_SLACK) {
    xsnprintf(err, sizeof err, "Not enough free space on the console's internal storage. The TV app needs about %lld MB.",
              (long long)((a->size + APP_SPACE_SLACK) / (1024 * 1024)));
    code = "no_space";
    sentence = err;
  }
  if (code) { xhttp_close(c); return pinned_fail(j, NULL, code, sentence, detail); }
  int fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0644);   /* read back for the UFS2 magic */
  if (fd < 0) { xsnprintf(detail, sizeof detail, "%s: %s", tmp, strerror(errno)); xhttp_close(c); return pinned_fail(j, NULL, "io", S_SAVE, detail); }
  char *buf = malloc(APP_BUF);
  if (!buf) { close(fd); xhttp_close(c); return pinned_fail(j, tmp, "oom", S_SAVE, "out of memory"); }
  sha256_ctx h;
  sha256_init(&h);
  int64_t done = 0;
  for (;;) {
    if (cx_job_cancelled(j)) { code = "cancelled"; sentence = g_cfg.stop_requested ? S_CLOSING : S_CANCELLED; break; }
    int n = xhttp_read(c, buf, APP_BUF);
    if (n < 0) {
      if (cx_job_cancelled(j)) { code = "cancelled"; sentence = g_cfg.stop_requested ? S_CLOSING : S_CANCELLED; break; }
      xsnprintf(err, sizeof err, "TV app download failed (%s). Try again later.", xhttp_last_error());
      code = "download_failed";
      sentence = err;
      break;
    }
    if (n == 0) break;
    if (done + n > a->size) { code = "too_large"; sentence = S_LARGER; break; }
    if (write_all(fd, buf, (size_t)n) != 0) { code = errno == ENOSPC ? "no_space" : "io"; sentence = S_SAVE; xsnprintf(detail, sizeof detail, "write: %s", strerror(errno)); break; }
    sha256_update(&h, buf, (size_t)n);
    done += n;
    cx_job_progress(j, done, a->size, "Downloading");
    info_phase("downloading", done, -1);
  }
  free(buf);
  xhttp_close(c);
  xthread_net(NULL, NULL);

  /* 2. verify: the length, the SHA-256 and the UFS2 superblock magic */
  if (!code) {
    xthread_note("app install (TV): verify");
    info_phase("verifying", done, -1);
    cx_job_progress(j, done, a->size, "Verifying");
    uint8_t dg[32];
    sha256_final(&h, dg);
    sha256_digest_hex(dg, hex);
    unsigned char magic[4] = {0};
    if (done != a->size || strcmp(hex, a->sha256) != 0) {
      code = "sha256_mismatch";
      sentence = S_SHA;
      xsnprintf(detail, sizeof detail, "%lld of %lld bytes, sha256 %.16s...", (long long)done, (long long)a->size, hex);
    } else if (pread(fd, magic, sizeof magic, APP_UFS2_MAGIC_OFFSET) != (ssize_t)sizeof magic) {
      code = "not_package";
      sentence = S_NOT_IMAGE;
    } else if (magic[0] != 0x19 || magic[1] != 0x01 || magic[2] != 0x54 || magic[3] != 0x19) {
      code = "not_package";
      sentence = S_NOT_IMAGE;
      xsnprintf(detail, sizeof detail, "magic %02x %02x %02x %02x at %d", magic[0], magic[1], magic[2], magic[3], APP_UFS2_MAGIC_OFFSET);
    }
  }
  if (!code && fsync(fd) != 0) { code = "io"; sentence = S_SAVE; xsnprintf(detail, sizeof detail, "fsync: %s", strerror(errno)); }
  if (close(fd) != 0 && !code) { code = "io"; sentence = S_SAVE; xsnprintf(detail, sizeof detail, "close: %s", strerror(errno)); }
  if (!code && cx_job_cancelled(j)) { code = "cancelled"; sentence = g_cfg.stop_requested ? S_CLOSING : S_CANCELLED; }
  if (code) return pinned_fail(j, tmp, code, sentence, detail);

  /* 3. the owner may have copied something in while the download ran */
  xthread_note("app install (TV): install");
  info_phase("installing", done, -1);
  cx_job_progress(j, done, done, "Installing");
  char current[40];
  int now = app_install_state(current, sizeof current);
  if (now == APP_STATE_FOLDER || now == APP_STATE_BLOCKED || (now == APP_STATE_MANUAL && !a->replace))
    return pinned_fail(j, tmp, "conflict", S_APPEARED, app_install_state_name(now));
  if (now == APP_STATE_STORE && version_compare(a->version, current) == -1) return pinned_fail(j, tmp, "conflict", S_NEWER, current);
  if (rename(tmp, a->dest) != 0) {
    xsnprintf(err, sizeof err, "Could not move the TV app into /data/homebrew (%s).", strerror(errno));
    return pinned_fail(j, tmp, "io", err, a->dest);
  }
  util_fsync_dir(a->dest);
  LOGI("app install (TV): %s %s in place (%lld bytes, sha256 %s)", a->dest, a->version, (long long)done, a->sha256);
  if (write_record(a->dest, a->version, a->sha256) != 0) return pinned_fail(j, NULL, "io", S_RECORD, APP_RECORD);

  /* 4. ShadowMountPlus */
  finish(j, a, done, a->sha256);
  return 0;
}

/* Starts a job of either kind under the in-progress guard (caller filled a). */
static int start_job(app_arg *a, const char *title, cx_job_fn fn, char *job_id, size_t idcap, char *code, size_t ccap,
                     char *err, size_t errcap) {
  pthread_mutex_lock(&app_mu);
  if (cx_jobs_active(APP_JOB_TYPE) > 0) {
    pthread_mutex_unlock(&app_mu);
    free(a);
    xstrlcpy(code, "in_progress", ccap);
    xsnprintf(err, errcap, "The XENO STORE app is already being installed");
    return -1;
  }
  /* no job runs: a .part left by an interrupted run is stale */
  char tmp[720];
  xsnprintf(tmp, sizeof tmp, "%s.part", a->dest);
  util_unlink_quiet(tmp);
  int pinned = a->pinned;
  int64_t size = a->size;
  char version[32];
  xstrlcpy(version, a->version, sizeof version);
  if (cx_job_start(APP_JOB_TYPE, title, fn, a, job_id, idcap) != 0) {
    pthread_mutex_unlock(&app_mu);
    free(a);
    xstrlcpy(code, "busy", ccap);
    xsnprintf(err, errcap, "too many operations running");
    return -1;
  }
  xstrlcpy(app_job, job_id, sizeof app_job);
  memset(&g_info, 0, sizeof g_info);
  xstrlcpy(g_info.job, job_id, sizeof g_info.job);
  g_info.pinned = pinned;
  xstrlcpy(g_info.version, version, sizeof g_info.version);
  xstrlcpy(g_info.phase, "downloading", sizeof g_info.phase);
  g_info.total = size;
  pthread_mutex_unlock(&app_mu);
  return 0;
}

int app_install_start(char *job_id, size_t idcap, char *code, size_t ccap, char *err, size_t errcap) {
  xstrlcpy(code, "bad_request", ccap);
  if (!xhttp_available()) { xstrlcpy(code, "downloads_unavailable", ccap); xsnprintf(err, errcap, "network transport unavailable"); return -1; }
  const char *base = app_base();
  if (!host_allowed(base)) { xstrlcpy(code, "forbidden", ccap); xsnprintf(err, errcap, "download host is not allowed"); return -1; }
  const char *dir = cx_data_homebrew_dir();
  if (util_mkdir_p(dir) != 0) { xstrlcpy(code, "io", ccap); xsnprintf(err, errcap, "cannot create %s", dir); return -1; }
  app_arg *a = calloc(1, sizeof *a);
  if (!a) { xstrlcpy(code, "oom", ccap); xsnprintf(err, errcap, "out of memory"); return -1; }
  xstrlcpy(a->base, base, sizeof a->base);
  size_t bl = strlen(a->base);
  while (bl > 0 && a->base[bl - 1] == '/') a->base[--bl] = 0;
  app_install_pkg_path(a->dest, sizeof a->dest);
  if (start_job(a, "XENO STORE app (Games row)", app_worker, job_id, idcap, code, ccap, err, errcap) != 0) return -1;
  LOGI("app install: job %s started (%s)", job_id, base);
  return 0;
}

int app_install_start_pinned(const char *url, const char *sha256, int64_t size, const char *version, int replace,
                             char *job_id, size_t idcap, char *code, size_t ccap, char *err, size_t errcap) {
  xstrlcpy(code, "bad_request", ccap);
  if (!xhttp_available()) { xstrlcpy(code, "downloads_unavailable", ccap); xsnprintf(err, errcap, "network transport unavailable"); return -1; }
  if (!app_install_release_url_ok(url)) { xstrlcpy(code, "forbidden", ccap); xsnprintf(err, errcap, "the release URL is not allowed"); return -1; }
  size_t hl = sha256 ? strlen(sha256) : 0;
  int hex_ok = hl == 64;
  for (size_t i = 0; hex_ok && i < hl; i++) hex_ok = (sha256[i] >= '0' && sha256[i] <= '9') || (sha256[i] >= 'a' && sha256[i] <= 'f');
  if (!hex_ok) { xsnprintf(err, errcap, "the release has no SHA-256"); return -1; }
  if (size < APP_MIN_BYTES || size > APP_MAX_BYTES) { xsnprintf(err, errcap, "the release size is out of range"); return -1; }
  if (!version || !version[0] || strlen(version) >= 32) { xsnprintf(err, errcap, "the release has no version"); return -1; }
  const char *dir = cx_data_homebrew_dir();
  if (util_mkdir_p(dir) != 0) { xstrlcpy(code, "io", ccap); xsnprintf(err, errcap, "cannot create %s", dir); return -1; }
  app_arg *a = calloc(1, sizeof *a);
  if (!a) { xstrlcpy(code, "oom", ccap); xsnprintf(err, errcap, "out of memory"); return -1; }
  a->pinned = 1;
  a->replace = replace ? 1 : 0;
  a->size = size;
  xstrlcpy(a->url, url, sizeof a->url);
  xstrlcpy(a->sha256, sha256, sizeof a->sha256);
  xstrlcpy(a->version, version, sizeof a->version);
  app_install_pkg_path(a->dest, sizeof a->dest);
  char title[96];
  xsnprintf(title, sizeof title, "XENO STORE TV app %s", version);
  if (start_job(a, title, app_worker_pinned, job_id, idcap, code, ccap, err, errcap) != 0) return -1;
  LOGI("app install (TV): job %s started (%s, %lld bytes)", job_id, version, (long long)size);
  return 0;
}
