/*
 * cx_companions.c — see cx_companions.h.
 */
#include "cx_companions.h"
#include "cx_common.h"
#include "cx_feeds.h"
#include "cx_garlic.h"
#include "cx_jobs.h"
#include "http_client.h"
#include "log.h"
#include "util.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOADER_PORT_DEFAULT 9021
#define ELF_MAX_BYTES (64LL << 20)
#define COMP_MAX 64

static pthread_mutex_t comp_mu = PTHREAD_MUTEX_INITIALIZER;
static const char *const PAYLOAD_HOSTS[] = {"github.com", "githubusercontent.com", "raw.githubusercontent.com"};

typedef struct comp {
  char id[48], name[96], version[32], url[1024], sha256[65], notes[640], kind[24], homepage[256], filename[128];
  char reason[320];
  int64_t size;
  int unavailable;         /* no upstream asset to pin: listed, never installed */
  int placeholder;         /* unavailable because its sha256 is no digest ("FILLED-BY-OWNER") */
} comp;

static int loader_port(void) { return cx_local_port("LOADER", LOADER_PORT_DEFAULT); }

static void payload_path(const char *id, char *out, size_t cap) { xsnprintf(out, cap, "%s/payloads/%s.elf", cx_data_dir(), id); }
static void installed_path(char *out, size_t cap) { xsnprintf(out, cap, "%s/payloads/installed.json", cx_data_dir()); }

static int is_hex64(const char *s) {
  if (strlen(s) != 64) return 0;
  for (int i = 0; i < 64; i++) if (!isxdigit((unsigned char)s[i])) return 0;
  return 1;
}

static int load_feed(comp *out, int max) {
  jdoc *d = cx_feed_doc(CX_FEED_COMPANIONS);
  if (!d) return 0;
  jv root = json_root(d);
  jv arr = jv_get(root, "payloads");
  if (jv_type(arr) != JV_ARR) arr = jv_get(root, "companions");
  if (jv_type(arr) != JV_ARR && jv_type(root) == JV_ARR) arr = root;
  int n = 0;
  JV_FOREACH(it, arr) {
    if (n >= max) break;
    comp *c = &out[n];
    memset(c, 0, sizeof *c);
    const char *id = jv_get_str(it, "id", "");
    const char *name = jv_get_str(it, "name", "");
    const char *url = jv_get_str(it, "url", "");
    char derived[48] = "";
    if (!*id && *name) {
      size_t o = 0;
      for (const char *p = name; *p && o + 1 < sizeof derived; p++) {
        char ch = *p;
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) derived[o++] = ch;
        else if (ch >= 'A' && ch <= 'Z') derived[o++] = (char)(ch + 32);
        else if (o && derived[o - 1] != '-') derived[o++] = '-';
      }
      derived[o] = 0;
      id = derived;
    }
    int unavailable = jv_get_bool(it, "unavailable", 0) || !jv_get_bool(it, "available", 1);
    if (!cx_id_valid(id, 47) || !*name || (!unavailable && !util_istarts_with(url, "http"))) continue;
    const char *sha = jv_get_str(it, "sha256", ""), *version = jv_get_str(it, "version", ""), *kind = jv_get_str(it, "kind", "payload");
    int64_t size = jv_get_i64(it, "size", 0);
    /* Garlic's entry without a real digest (feeds before 1.3 said
     * "FILLED-BY-OWNER") is the release this build pins, the one cx_garlic
     * starts: listed, installed and started from the same file */
    char pin[300] = "";
    if (!unavailable && !is_hex64(sha) && strcmp(kind, "saves") == 0) cx_garlic_pin_url(pin, sizeof pin);
    if (pin[0]) { url = pin; sha = CX_GARLIC_ELF_SHA256; version = CX_GARLIC_ELF_VERSION; size = CX_GARLIC_ELF_SIZE; }
    /* any other sha256 that is no digest could only fail the download after
     * it: not available instead */
    c->placeholder = !unavailable && *sha && !is_hex64(sha);
    c->unavailable = unavailable || c->placeholder;
    if (c->placeholder) xstrlcpy(c->reason, "the companions list on this console has a placeholder instead of its sha256, so the download cannot be verified (the payload list fetches the published list when it is opened while online)", sizeof c->reason);
    else xstrlcpy(c->reason, jv_get_str(it, "unavailableReason", ""), sizeof c->reason);
    xstrlcpy(c->id, id, sizeof c->id);
    xstrlcpy(c->name, name, sizeof c->name);
    xstrlcpy(c->version, version, sizeof c->version);
    xstrlcpy(c->url, url, sizeof c->url);
    xstrlcpy(c->sha256, sha, sizeof c->sha256);
    util_lower(c->sha256);
    xstrlcpy(c->notes, jv_get_str(it, "notes", jv_get_str(it, "description", "")), sizeof c->notes);
    xstrlcpy(c->kind, kind, sizeof c->kind);
    xstrlcpy(c->homepage, jv_get_str(it, "homepage", ""), sizeof c->homepage);
    xstrlcpy(c->filename, jv_get_str(it, "filename", ""), sizeof c->filename);
    c->size = size;
    n++;
  }
  json_free(d);
  return n;
}

static int find_comp(const char *id, comp *out) {
  comp *all = calloc(COMP_MAX, sizeof *all);
  if (!all) return -1;
  int n = load_feed(all, COMP_MAX);
  int rc = -1;
  for (int i = 0; i < n; i++) if (strcmp(all[i].id, id) == 0) { *out = all[i]; rc = 0; break; }
  free(all);
  return rc;
}

static jdoc *installed_doc(void) {
  char p[700];
  installed_path(p, sizeof p);
  return cx_json_file(p, 1 << 20, 5000);
}

static void record_installed(const comp *c, const char *sha) {
  char p[700];
  installed_path(p, sizeof p);
  jdoc *d = installed_doc();
  jw w;
  jw_init(&w);
  jw_obj_begin(&w);
  if (d) {
    JV_FOREACH(m, json_root(d)) {
      const char *k = jv_key(m);
      if (!k || strcmp(k, c->id) == 0) continue;
      jw_key(&w, k);
      jw_value(&w, m);
    }
    json_free(d);
  }
  jw_key(&w, c->id); jw_obj_begin(&w);
  jw_kv_str(&w, "version", c->version);
  jw_kv_str(&w, "sha256", sha);
  jw_kv_str(&w, "name", c->name);
  jw_kv_str(&w, "kind", c->kind);
  jw_kv_i64(&w, "installedAt", (int64_t)util_now());
  jw_obj_end(&w);
  jw_obj_end(&w);
  pthread_mutex_lock(&comp_mu);
  cx_write_json(p, &w);
  pthread_mutex_unlock(&comp_mu);
}

void cx_companions_init(void) {
  char dir[700];
  xsnprintf(dir, sizeof dir, "%s/payloads", cx_data_dir());
  util_mkdir_p(dir);
}

void cx_companions_json(jw *w) {
  /* the published list, catalog auto-refresh on or off: a copy cached by
   * another build (1.2.1's placeholder hashes) or 6 h old is fetched first,
   * waited for a few seconds at most */
  cx_feed_ensure_wait(CX_FEED_COMPANIONS, 5000);
  comp *all = calloc(COMP_MAX, sizeof *all);
  int n = all ? load_feed(all, COMP_MAX) : 0;
  jdoc *inst = installed_doc();
  jw_key(w, "payloads");
  jw_arr_begin(w);
  for (int i = 0; i < n; i++) {
    comp *c = &all[i];
    char p[700];
    payload_path(c->id, p, sizeof p);
    int64_t sz = util_file_size(p);
    jv rec = inst ? jv_get(json_root(inst), c->id) : jv_get(json_root(inst), "");
    const char *iv = inst && jv_ok(rec) ? jv_get_str(rec, "version", "") : "";
    jw_obj_begin(w);
    jw_kv_str(w, "id", c->id);
    jw_kv_str(w, "name", c->name);
    jw_kv_str(w, "version", c->version);
    jw_kv_str(w, "url", c->url);
    jw_kv_str(w, "sha256", c->sha256);
    jw_kv_str(w, "notes", c->notes);
    jw_kv_str(w, "kind", c->kind);
    jw_kv_str(w, "homepage", c->homepage);
    jw_kv_i64(w, "size", c->size);
    jw_kv_bool(w, "available", !c->unavailable);
    if (c->unavailable) jw_kv_str(w, "unavailableReason", c->reason[0] ? c->reason : "No upstream release asset to download.");
    jw_kv_bool(w, "installed", sz > 0);
    jw_kv_str(w, "localPath", sz > 0 ? p : "");
    jw_kv_i64(w, "localSize", sz > 0 ? sz : 0);
    jw_kv_str(w, "installedVersion", sz > 0 ? iv : "");
    jw_kv_bool(w, "updateAvailable", sz > 0 && iv[0] && c->version[0] && util_semver_cmp(c->version, iv) > 0);
    jw_obj_end(w);
  }
  jw_arr_end(w);
  if (inst) json_free(inst);
  free(all);
  jw_kv_int(w, "loaderPort", loader_port());
  jw_kv_bool(w, "loaderAvailable", cx_tcp_probe(loader_port(), 300));
  jw_kv_i64(w, "feedFetchedAt", (int64_t)cx_feed_fetched_at(CX_FEED_COMPANIONS));
  jw_kv_str(w, "note", "Payloads are downloaded to the console (sha256 checked) and sent to the local ELF loader on :9021. XENO STORE never loads code in-process.");
}

typedef struct inst_arg { comp c; char dest[700]; } inst_arg;

static int dl_cb(void *user, int64_t done, int64_t total) {
  cx_job *j = user;
  cx_job_progress(j, done, total, "Downloading");
  return cx_job_cancelled(j);
}

static int install_worker(cx_job *j, void *p) {
  inst_arg *a = p;
  char host[256];
  util_url_host(a->c.url, host, sizeof host);
  if (!cx_host_in(host, PAYLOAD_HOSTS, XS_ARRAY_LEN(PAYLOAD_HOSTS)) && !cx_host_is_loopback(host)) { cx_job_fail(j, "forbidden", "Payload URL host is not allowed (GitHub releases only)"); return -1; }
  char err[256] = "";
  cx_job_progress(j, 0, a->c.size, "Downloading");
  if (cx_download_file(a->c.url, NULL, a->dest, a->c.sha256, ELF_MAX_BYTES, dl_cb, j, &j->cancel, err, sizeof err) != 0) { cx_job_fail(j, "download_failed", err); return -1; }
  /* must look like an ELF */
  int is_elf = 0;
  {
    unsigned char head[4] = {0, 0, 0, 0};
    FILE *f = fopen(a->dest, "rb");
    if (f) { is_elf = fread(head, 1, 4, f) == 4 && memcmp(head, "\x7f" "ELF", 4) == 0; fclose(f); }
  }
  if (!is_elf) { util_unlink_quiet(a->dest); cx_job_fail(j, "not_elf", "The downloaded file is not an ELF payload"); return -1; }
  char sha[65] = "";
  cx_sha256_file(a->dest, sha);
  record_installed(&a->c, sha);
  jw rw;
  jw_init(&rw);
  jw_obj_begin(&rw);
  jw_kv_str(&rw, "id", a->c.id);
  jw_kv_str(&rw, "localPath", a->dest);
  jw_kv_str(&rw, "sha256", sha);
  jw_kv_i64(&rw, "size", util_file_size(a->dest));
  jw_kv_bool(&rw, "verified", a->c.sha256[0] != 0);
  jw_obj_end(&rw);
  cx_job_set_result(j, &rw);
  cx_job_progress(j, 1, 1, "Ready to launch");
  LOGI("companions: installed %s (%s)", a->c.id, sha);
  return 0;
}

int cx_companions_install(const char *id, char *job_id, size_t idcap, char *code, size_t ccap, char *err, size_t errcap) {
  xstrlcpy(code, "bad_request", ccap);
  if (!cx_id_valid(id, 47)) { xsnprintf(err, errcap, "invalid payload id"); return -1; }
  cx_feed_ensure(CX_FEED_COMPANIONS, 6 * 3600);   /* installs from the published list (cx_companions_json) */
  inst_arg *a = calloc(1, sizeof *a);
  if (!a) { xstrlcpy(code, "oom", ccap); xsnprintf(err, errcap, "out of memory"); return -1; }
  if (find_comp(id, &a->c) != 0) { free(a); xstrlcpy(code, "not_found", ccap); xsnprintf(err, errcap, "unknown payload id (refresh the catalog to update the list)"); return -1; }
  if (a->c.unavailable) {
    xstrlcpy(code, "unavailable", ccap);
    xsnprintf(err, errcap, "%s is not available for download: %s", a->c.name, a->c.reason[0] ? a->c.reason : "its author publishes no release asset");
    free(a);
    return -1;
  }
  if (!xhttp_available()) { free(a); xstrlcpy(code, "downloads_unavailable", ccap); xsnprintf(err, errcap, "network transport unavailable"); return -1; }
  payload_path(id, a->dest, sizeof a->dest);
  if (cx_job_start("payload-install", a->c.name, install_worker, a, job_id, idcap) != 0) { free(a); xstrlcpy(code, "busy", ccap); xsnprintf(err, errcap, "too many operations running"); return -1; }
  return 0;
}

int cx_companions_loader_available(void) { return cx_tcp_probe(loader_port(), 300); }

int cx_companions_send_elf(const char *path, int64_t *bytes, char *code, size_t ccap, char *err, size_t errcap) {
  if (bytes) *bytes = 0;
  size_t len = 0;
  char *data = util_read_file(path, &len, (size_t)ELF_MAX_BYTES);
  if (!data) { xstrlcpy(code, "not_found", ccap); xsnprintf(err, errcap, "payload is not installed yet"); return -1; }
  if (len < 4 || memcmp(data, "\x7f" "ELF", 4) != 0) { free(data); xstrlcpy(code, "not_elf", ccap); xsnprintf(err, errcap, "stored file is not an ELF"); return -1; }
  int port = loader_port();
  char e2[160];
  if (cx_tcp_send_all(port, data, len, 10000, e2, sizeof e2) != 0) {
    free(data);
    xstrlcpy(code, "loader_unavailable", ccap);
    xsnprintf(err, errcap, "ELF loader not answering on 127.0.0.1:%d — start your payload loader (etaHEN/elfldr) and try again", port);
    return -1;
  }
  free(data);
  if (bytes) *bytes = (int64_t)len;
  LOGI("companions: sent %s (%zu bytes) to :%d", path, len, port);
  return 0;
}

int cx_companions_launch(const char *id, jw *w, char *code, size_t ccap, char *err, size_t errcap) {
  xstrlcpy(code, "bad_request", ccap);
  if (!cx_id_valid(id, 47)) { xsnprintf(err, errcap, "invalid payload id"); return -1; }
  char p[700];
  payload_path(id, p, sizeof p);
  int64_t len = 0;
  char sid[48] = "";
  if (strcmp(id, CX_GARLIC_COMPANION_ID) == 0 || (cx_companions_by_kind("saves", sid, sizeof sid, NULL, 0, NULL) == 0 && strcmp(id, sid) == 0)) {
    /* never a second Garlic: a starting copy unmounts /data/mount_sd before it finds its port taken */
    if (cx_tcp_probe(cx_garlic_port(), 300)) { xstrlcpy(code, "already_running", ccap); xsnprintf(err, errcap, "Garlic Save Manager is already running (port %d)", cx_garlic_port()); return -1; }
    if (!cx_garlic_ensure(1, err, errcap)) { xstrlcpy(code, "loader_unavailable", ccap); return -1; }
    len = util_file_size(p);
    if (len < 0) len = 0;   /* started from the pinned release or a copy found on the console */
  } else if (cx_companions_send_elf(p, &len, code, ccap, err, errcap) != 0) return -1;
  jw_kv_str(w, "id", id);
  jw_kv_i64(w, "bytes", len);
  jw_kv_int(w, "port", loader_port());
  jw_kv_bool(w, "sent", 1);
  return 0;
}

int cx_companions_by_kind(const char *kind, char *id, size_t idcap, char *version, size_t vcap, int *unavailable) {
  comp *all = calloc(COMP_MAX, sizeof *all);
  if (!all) return -1;
  int n = load_feed(all, COMP_MAX);
  int rc = -1;
  for (int i = 0; i < n; i++) {
    if (strcmp(all[i].kind, kind) != 0) continue;
    /* a placeholder entry counts as none: the caller's own pin stands (cx_companions_fetch) */
    if (all[i].placeholder) break;
    if (id) xstrlcpy(id, all[i].id, idcap);
    if (version) xstrlcpy(version, all[i].version, vcap);
    if (unavailable) *unavailable = all[i].unavailable;
    rc = 0;
    break;
  }
  free(all);
  return rc;
}

static int file_is_elf(const char *path) {
  unsigned char head[4] = {0, 0, 0, 0};
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  int ok = fread(head, 1, 4, f) == 4 && memcmp(head, "\x7f" "ELF", 4) == 0;
  fclose(f);
  return ok;
}

int cx_companions_fetch(const char *kind, const char *fb_id, const char *fb_url, const char *fb_sha256, const char *fb_version,
                        cx_companion_file *out, char *err, size_t errcap) {
  memset(out, 0, sizeof *out);
  comp *all = calloc(COMP_MAX, sizeof *all);
  comp *c = calloc(1, sizeof *c);
  if (!all || !c) { free(all); free(c); xsnprintf(err, errcap, "out of memory"); return -1; }
  int n = load_feed(all, COMP_MAX), found = 0;
  for (int i = 0; i < n && !found; i++) if (strcmp(all[i].kind, kind) == 0) { *c = all[i]; found = 1; }
  free(all);
  if (found && c->unavailable) {
    xsnprintf(err, errcap, "%s is not available for download: %s", c->name, c->reason[0] ? c->reason : "its author publishes no release asset");
    free(c);
    return -1;
  }
  if (!found) {
    if (!fb_id || !cx_id_valid(fb_id, 47) || !fb_url || !util_istarts_with(fb_url, "http")) {
      xsnprintf(err, errcap, "no download is known for this payload (refresh the catalog)");
      free(c);
      return -1;
    }
    xstrlcpy(c->id, fb_id, sizeof c->id);
    xstrlcpy(c->name, fb_id, sizeof c->name);
    xstrlcpy(c->kind, kind, sizeof c->kind);
    xstrlcpy(c->url, fb_url, sizeof c->url);
    xstrlcpy(c->sha256, fb_sha256 ? fb_sha256 : "", sizeof c->sha256);
    util_lower(c->sha256);
    xstrlcpy(c->version, fb_version ? fb_version : "", sizeof c->version);
  }
  xstrlcpy(out->id, c->id, sizeof out->id);
  xstrlcpy(out->version, c->version, sizeof out->version);
  payload_path(c->id, out->path, sizeof out->path);
  /* an existing copy is reused when it is the pinned file */
  if (util_file_size(out->path) > 0 && file_is_elf(out->path)) {
    char sha[65] = "";
    cx_sha256_file(out->path, sha);
    if (!c->sha256[0] || strcmp(sha, c->sha256) == 0) {
      xstrlcpy(out->sha256, sha, sizeof out->sha256);
      free(c);
      return 0;
    }
    LOGW("companions: %s on the console is not the pinned file (sha256 %s), downloading it again", c->id, sha);
  }
  char host[256];
  util_url_host(c->url, host, sizeof host);
  if (!cx_host_in(host, PAYLOAD_HOSTS, XS_ARRAY_LEN(PAYLOAD_HOSTS)) && !cx_host_is_loopback(host)) {
    xsnprintf(err, errcap, "payload URL host is not allowed (GitHub releases only)");
    free(c);
    return -1;
  }
  if (!xhttp_available()) { xsnprintf(err, errcap, "network transport unavailable"); free(c); return -1; }
  cx_companions_init();   /* <data>/payloads/ */
  char e2[256] = "";
  if (cx_download_file(c->url, NULL, out->path, c->sha256, ELF_MAX_BYTES, NULL, NULL, NULL, e2, sizeof e2) != 0) {
    xsnprintf(err, errcap, "download of %s failed: %s", c->id, e2);
    free(c);
    return -1;
  }
  if (!file_is_elf(out->path)) {
    util_unlink_quiet(out->path);
    xsnprintf(err, errcap, "the downloaded %s is not an ELF payload", c->id);
    free(c);
    return -1;
  }
  cx_sha256_file(out->path, out->sha256);
  record_installed(c, out->sha256);
  out->downloaded = 1;
  LOGI("companions: fetched %s %s (%s)", c->id, c->version, out->sha256);
  free(c);
  return 0;
}

int cx_companions_find_installed(const char *needle, char *id_out, size_t cap) {
  jdoc *inst = installed_doc();
  if (!inst) return -1;
  int rc = -1;
  JV_FOREACH(m, json_root(inst)) {
    const char *k = jv_key(m);
    if (!k) continue;
    if (util_strcasestr(k, needle) || util_strcasestr(jv_get_str(m, "name", ""), needle) || util_strcasestr(jv_get_str(m, "kind", ""), needle)) {
      char p[700];
      payload_path(k, p, sizeof p);
      if (util_file_exists(p)) { xstrlcpy(id_out, k, cap); rc = 0; break; }
    }
  }
  json_free(inst);
  return rc;
}
