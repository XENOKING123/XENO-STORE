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
 */
#include "app_install.h"
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
#include <unistd.h>

#define APP_BASE "https://github.com/XENOKING123/XENO-STORE/releases/latest/download"
#define APP_FILE APP_TITLE_ID ".ffpkg"
#define APP_JOB_TYPE "app-install"
#define APP_MAX_BYTES (512LL << 20)
#define APP_SPACE_SLACK (16LL << 20)   /* headroom left on /data after the write */
#define APP_BUF (512u << 10)

static const char *const APP_HOSTS[] = {"github.com", "githubusercontent.com"};
static pthread_mutex_t app_mu = PTHREAD_MUTEX_INITIALIZER;
static char app_job[16];   /* last job started (guarded by app_mu) */

typedef struct app_arg { char base[512], dest[700]; } app_arg;

static const char *app_base(void) {
  const char *o = cx_base_override("APP");
  return o[0] ? o : APP_BASE;
}

static void pkg_path(char *out, size_t cap) { xsnprintf(out, cap, "%s/%s", cx_data_homebrew_dir(), APP_FILE); }

static int host_allowed(const char *url) {
  char host[256];
  util_url_host(url, host, sizeof host);
  return host[0] && (cx_host_in(host, APP_HOSTS, XS_ARRAY_LEN(APP_HOSTS)) || cx_host_is_loopback(host));
}

void app_install_status_json(jw *w) {
  char path[700], appdir[700], ver[32] = "", job[16] = "", last[16];
  pkg_path(path, sizeof path);
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
  uint8_t dg[32];
  sha256_final(&h, dg);
  sha256_digest_hex(dg, hex);
  if (strcmp(hex, want) != 0) {
    xsnprintf(err, sizeof err, "sha256 mismatch: got %.16s..., the release publishes %.16s...", hex, want);
    return fail(j, tmp, "sha256_mismatch", err);
  }
  if (rename(tmp, a->dest) != 0) { xsnprintf(err, sizeof err, "cannot rename to %s: %s", a->dest, strerror(errno)); return fail(j, tmp, "io", err); }
  util_fsync_dir(a->dest);
  LOGI("app install: %s in place (%lld bytes, sha256 %s)", a->dest, (long long)done, hex);

  /* 4. ShadowMountPlus registers the tile on its own: it rescans /data/homebrew
   *    every 15-60 s (recent versions also as soon as a file lands there).
   *    1.7+ takes a scan request on its API too; the job never depends on it. */
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
  jw_kv_bool(&rw, "shadowMount", sm_ok);
  jw_kv_str(&rw, "shadowMountStatus", sm);
  jw_obj_end(&rw);
  cx_job_set_result(j, &rw);
  cx_job_progress(j, done, done, sm_ok ? "Installed; ShadowMountPlus adds the tile within about a minute" : "Installed; the tile appears once ShadowMountPlus scans /data/homebrew");
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
  pkg_path(a->dest, sizeof a->dest);
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
  if (cx_job_start(APP_JOB_TYPE, "XENO STORE app (Games row)", app_worker, a, job_id, idcap) != 0) {
    pthread_mutex_unlock(&app_mu);
    free(a);
    xstrlcpy(code, "busy", ccap);
    xsnprintf(err, errcap, "too many operations running");
    return -1;
  }
  xstrlcpy(app_job, job_id, sizeof app_job);
  pthread_mutex_unlock(&app_mu);
  LOGI("app install: job %s started (%s)", job_id, base);
  return 0;
}
