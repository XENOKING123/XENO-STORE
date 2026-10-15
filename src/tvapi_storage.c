/*
 * tvapi_storage.c — the tv-storage thread and the drive vocabulary of the
 * /api/v1 layer (PORT-SPEC 1.6.2, R4, R5). See tvapi.h for the contract.
 *
 * Copyright (C) 2026 XENO STORE contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One thread keeps a snapshot of every mounted root (read-only ones too, with
 * their writable flag) so that no HTTP worker ever waits on a drive: the
 * /api/v1 handlers only copy the snapshot under tvdl_mu (a leaf lock).
 *
 * The drive labels ("USB storage N", "M.2 SSD", "Extended storage N",
 * "Internal storage") and the /mnt mount test are the app's own vocabulary.
 * Ported from Orbit Store backend/storage.c (GPL-3.0-or-later): the label
 * table, 1-based numbering and the st_dev(root) != st_dev("/mnt") test.
 */
#include "tvapi.h"

#include "config.h"
#include "log.h"
#include "settings.h"
#include "util.h"
#include "xthread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SIG_EVERY_MS 2000      /* presence signature (about a dozen stat calls) */
#define GET_EVERY_MS 2000      /* storage_get_roots(): one statvfs per root */
#define SCAN_EVERY_MS 60000    /* storage_scan_roots(): rediscovery + writability probe */
#define LOOP_TICK_MS 100       /* granularity of the 1 s loop; a kick is served within it */

static pthread_mutex_t tvdl_mu = PTHREAD_MUTEX_INITIALIZER;   /* leaf lock */
static tv_drive g_drives[TVDL_MAX_DRIVES];
static int g_ndrives;
static time_t g_updated_at, g_scanned_at;
static int64_t g_updated_ms;
static unsigned g_gen;
static uint64_t g_content_hash;
static int g_kick, g_kick_rescan;
static volatile int g_thread_started;

/* ---- the vocabulary (pure functions of a path) ----------------------------- */

static void strip_slashes(const char *in, char *out, size_t cap) {
  xstrlcpy(out, in ? in : "", cap);
  size_t n = strlen(out);
  while (n > 1 && out[n - 1] == '/') out[--n] = 0;
}

/* "<prefix><digits>" exactly (after trailing slashes are stripped): the number, else -1. */
static int numbered(const char *root, const char *prefix) {
  size_t pl = strlen(prefix);
  if (strncmp(root, prefix, pl) != 0) return -1;
  const char *p = root + pl;
  if (!*p) return -1;
  int n = 0;
  for (; *p; p++) {
    if (*p < '0' || *p > '9') return -1;
    n = n * 10 + (*p - '0');
    if (n > 999) return -1;
  }
  return n;
}

/* The same test for a path inside a root: "<prefix><digits>/..." or exactly the root. */
static int numbered_prefix(const char *path, const char *prefix) {
  size_t pl = strlen(prefix);
  if (strncmp(path, prefix, pl) != 0) return -1;
  const char *p = path + pl;
  int n = 0, digits = 0;
  for (; *p >= '0' && *p <= '9'; p++) { n = n * 10 + (*p - '0'); if (++digits > 3) return -1; }
  if (!digits || (*p && *p != '/')) return -1;
  return n;
}

static int under(const char *path, const char *root) {
  size_t n = strlen(root);
  if (n == 0 || strncmp(path, root, n) != 0) return 0;
  return path[n] == 0 || path[n] == '/' || (n == 1 && root[0] == '/');
}

#ifdef XENO_HOST
static void host_downloads_dir(char *out, size_t cap) {
  char d[600];
  xsnprintf(d, sizeof d, "%s/downloads", g_cfg.data_dir);
  strip_slashes(d, out, cap);
}

/* The --root index of `root` (exact path), else -1. */
static int host_extra_index(const char *root) {
  for (int i = 0; i < g_cfg.n_extra_roots; i++) {
    char r[512];
    strip_slashes(g_cfg.extra_roots[i], r, sizeof r);
    if (strcmp(r, root) == 0) return i;
  }
  return -1;
}

static void host_extra_label(int i, char *out, size_t cap) {
  char r[512];
  strip_slashes(g_cfg.extra_roots[i], r, sizeof r);
  const char *b = util_basename(r);
  if (b && *b) xstrlcpy(out, b, cap);
  else xsnprintf(out, cap, "Root %d", i + 1);
}
#endif

static void sanitized_basename(const char *root, char *out, size_t cap) {
  const char *b = util_basename(root);
  size_t o = 0;
  for (const char *p = b ? b : ""; *p && o + 1 < cap && o < 63; p++) {
    char ch = *p;
    int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    out[o++] = ok ? ch : '_';
  }
  if (cap) out[o] = 0;
  if (o == 0) xstrlcpy(out, "drive", cap);
}

void tvdl_id_for_root(const char *root_in, char *out, size_t cap) {
  char root[512];
  strip_slashes(root_in, root, sizeof root);
#ifdef XENO_HOST
  char dl[600];
  host_downloads_dir(dl, sizeof dl);
  if (strcmp(root, dl) == 0) { xstrlcpy(out, "internal", cap); return; }
  int x = host_extra_index(root);
  if (x >= 0) { xsnprintf(out, cap, "root%d", x); return; }
#endif
  int n;
  if ((n = numbered(root, "/mnt/usb")) >= 0) { xsnprintf(out, cap, "usb%d", n); return; }
  if ((n = numbered(root, "/mnt/ext")) >= 0) { xsnprintf(out, cap, "ext%d", n); return; }
  if (strcmp(root, "/data") == 0 || strcmp(root, "/user") == 0) { xstrlcpy(out, "internal", cap); return; }
  sanitized_basename(root, out, cap);
}

void tvdl_label_for_root(const char *root_in, const char *xs_label, char *out, size_t cap) {
  char root[512];
  strip_slashes(root_in, root, sizeof root);
#ifdef XENO_HOST
  char dl[600];
  host_downloads_dir(dl, sizeof dl);
  if (strcmp(root, dl) == 0) { xstrlcpy(out, "Internal storage", cap); return; }
  int x = host_extra_index(root);
  if (x >= 0) {
    if (xs_label && *xs_label) xstrlcpy(out, xs_label, cap);
    else host_extra_label(x, out, cap);
    return;
  }
#endif
  int n;
  if ((n = numbered(root, "/mnt/usb")) >= 0) { xsnprintf(out, cap, "USB storage %d", n + 1); return; }
  if ((n = numbered(root, "/mnt/ext")) >= 0) {
    /* /mnt/ext1 is the M.2 slot on the consoles the reference was tested on
     * (open point: confirm on hardware, then fix storage.c's web labels too) */
    if (n == 1) xstrlcpy(out, "M.2 SSD", cap);
    else xsnprintf(out, cap, "Extended storage %d", n + 1);
    return;
  }
  if (strcmp(root, "/data") == 0 || strcmp(root, "/user") == 0) { xstrlcpy(out, "Internal storage", cap); return; }
  if (xs_label && *xs_label) xstrlcpy(out, xs_label, cap);
  else sanitized_basename(root, out, cap);
}

void tvdl_location_for_path(const char *path, char *out, size_t cap) {
  const char *p = path ? path : "";
#ifdef XENO_HOST
  char dl[600];
  host_downloads_dir(dl, sizeof dl);
  if (under(p, dl)) { xstrlcpy(out, "Internal storage", cap); return; }
  for (int i = 0; i < g_cfg.n_extra_roots; i++) {
    char r[512];
    strip_slashes(g_cfg.extra_roots[i], r, sizeof r);
    if (under(p, r)) { host_extra_label(i, out, cap); return; }
  }
#endif
  int n;
  if ((n = numbered_prefix(p, "/mnt/usb")) >= 0) { xsnprintf(out, cap, "USB storage %d", n + 1); return; }
  if ((n = numbered_prefix(p, "/mnt/ext")) >= 0) {
    if (n == 1) xstrlcpy(out, "M.2 SSD", cap);
    else xsnprintf(out, cap, "Extended storage %d", n + 1);
    return;
  }
  if (under(p, "/data") || under(p, "/user")) { xstrlcpy(out, "Internal storage", cap); return; }
  if (under(p, "/mnt/shadowmnt")) { xstrlcpy(out, "ShadowMount", cap); return; }
  xstrlcpy(out, "Other location", cap);
}

/* Sort key: group (0 USB / host roots, 1 ext, 2 other, 3 internal), then the number. */
static void order_for(const char *id, const char *kind, int *group, int *num) {
  int n;
  *num = 0;
  if (strcmp(id, "internal") == 0 || strcmp(kind, "internal") == 0) { *group = 3; return; }
  if ((n = numbered(id, "usb")) >= 0) { *group = 0; *num = n; return; }
  if ((n = numbered(id, "root")) >= 0) { *group = 0; *num = 100 + n; return; }
  if ((n = numbered(id, "ext")) >= 0) { *group = 1; *num = n; return; }
  *group = 2;
}

static void drive_from_root(tv_drive *d, const storage_root *r) {
  memset(d, 0, sizeof *d);
  char root[512];
  strip_slashes(r->path, root, sizeof root);
  tvdl_id_for_root(root, d->id, sizeof d->id);
  tvdl_label_for_root(root, r->label, d->label, sizeof d->label);
  xstrlcpy(d->path, r->path, sizeof d->path);
  xstrlcpy(d->kind, r->kind, sizeof d->kind);
  d->external = strcmp(r->kind, "internal") != 0;
  d->writable = r->writable != 0;
  d->is_default = r->is_default != 0;
  d->free = r->free;
  d->total = r->total;
  int group, num;
  order_for(d->id, d->kind, &group, &num);
  d->order = group;
}

static int drive_cmp(const void *a, const void *b) {
  const tv_drive *x = a, *y = b;
  int gx, nx, gy, ny;
  order_for(x->id, x->kind, &gx, &nx);
  order_for(y->id, y->kind, &gy, &ny);
  if (gx != gy) return gx - gy;
  if (nx != ny) return nx - ny;
  return strcmp(x->path, y->path);
}

/* ---- the snapshot ---------------------------------------------------------- */

static uint64_t fnv1a(uint64_t h, const void *data, size_t n) {
  const unsigned char *p = data;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
  return h;
}

/* Maps, sorts and publishes `n` roots. `refreshed` = 0 for the start-up seed.
 * The work buffer is static (5 KB, off the stack); publish_mu guards it. */
static pthread_mutex_t publish_mu = PTHREAD_MUTEX_INITIALIZER;
static tv_drive g_publish_buf[TVDL_MAX_DRIVES];

static void publish(const storage_root *roots, int n, int refreshed, int scanned) {
  pthread_mutex_lock(&publish_mu);
  tv_drive *tmp = g_publish_buf;
  if (n > TVDL_MAX_DRIVES) n = TVDL_MAX_DRIVES;
  if (n < 0) n = 0;
  for (int i = 0; i < n; i++) drive_from_root(&tmp[i], &roots[i]);
  qsort(tmp, (size_t)n, sizeof tmp[0], drive_cmp);
  uint64_t h = 1469598103934665603ULL;
  for (int i = 0; i < n; i++) {
    h = fnv1a(h, tmp[i].id, strlen(tmp[i].id) + 1);
    h = fnv1a(h, tmp[i].path, strlen(tmp[i].path) + 1);
    h = fnv1a(h, tmp[i].label, strlen(tmp[i].label) + 1);
    h = fnv1a(h, &tmp[i].writable, sizeof tmp[i].writable);
  }
  pthread_mutex_lock(&tvdl_mu);
  memcpy(g_drives, tmp, (size_t)n * sizeof tmp[0]);
  g_ndrives = n;
  if (h != g_content_hash) { g_content_hash = h; g_gen++; }
  if (refreshed) { g_updated_at = util_now(); g_updated_ms = util_now_ms(); }
  if (scanned) g_scanned_at = util_now();
  pthread_mutex_unlock(&tvdl_mu);
  pthread_mutex_unlock(&publish_mu);
}

int tvdl_snapshot(tv_drive *out, int max, int writable_only, tvdl_snap_info *info) {
  int n = 0;
  pthread_mutex_lock(&tvdl_mu);
  for (int i = 0; i < g_ndrives && n < max; i++) {
    if (writable_only && !g_drives[i].writable) continue;
    out[n++] = g_drives[i];
  }
  if (info) {
    info->updated_at = g_updated_at;
    info->updated_ms = g_updated_ms;
    info->scanned_at = g_scanned_at;
    info->gen = g_gen;
    info->kick_pending = g_kick;
    info->count = g_ndrives;
  }
  pthread_mutex_unlock(&tvdl_mu);
  return n;
}

static int find_locked(const char *id, const char *root, tv_drive *out) {
  char want[512];
  if (root) strip_slashes(root, want, sizeof want);
  for (int i = 0; i < g_ndrives; i++) {
    int hit;
    if (id) hit = strcmp(g_drives[i].id, id) == 0;
    else { char p[512]; strip_slashes(g_drives[i].path, p, sizeof p); hit = strcmp(p, want) == 0; }
    if (!hit) continue;
    if (out) *out = g_drives[i];
    return g_drives[i].writable ? 0 : -2;
  }
  return -1;
}

int tvdl_drive_for_id(const char *id, tv_drive *out) {
  if (!id || !*id) return -1;
  pthread_mutex_lock(&tvdl_mu);
  int rc = find_locked(id, NULL, out);
  pthread_mutex_unlock(&tvdl_mu);
  return rc;
}

int tvdl_drive_for_root(const char *root, tv_drive *out) {
  if (!root || !*root) return -1;
  pthread_mutex_lock(&tvdl_mu);
  int rc = find_locked(NULL, root, out);
  pthread_mutex_unlock(&tvdl_mu);
  return rc;
}

int tvdl_root_present(const char *root) { return tvdl_drive_for_root(root, NULL) != -1; }

void tvdl_preferred_storage_id(char *out, size_t cap) {
  if (tv_pref_auto()) { xstrlcpy(out, "", cap); return; }
  char def[256];
  settings_default_root(def, sizeof def);
  if (!def[0]) { xstrlcpy(out, "", cap); return; }
  tvdl_id_for_root(def, out, cap);
}

int tvdl_auto_drive(tv_drive *out) {
  int rc = -1;
  pthread_mutex_lock(&tvdl_mu);
  for (int pass = 0; pass < 2 && rc != 0; pass++) {
    for (int i = 0; i < g_ndrives; i++) {
      const tv_drive *d = &g_drives[i];
      if (!d->writable || (pass == 0 ? !d->external : d->external)) continue;
      if (out) *out = *d;
      rc = 0;
      break;
    }
  }
  pthread_mutex_unlock(&tvdl_mu);
  return rc;
}

int64_t tvdl_projected_free(const tv_drive *d, int64_t pending) { return d->free - pending - TVDL_PROJECTION_MARGIN; }

void tvdl_storage_kick(int rescan) {
  pthread_mutex_lock(&tvdl_mu);
  g_kick = 1;
  if (rescan) g_kick_rescan = 1;
  pthread_mutex_unlock(&tvdl_mu);
}

/* ---- the tv-storage thread ------------------------------------------------- */

/* A hash of which candidate roots exist (and, under /mnt, are mounted), with
 * their device and inode, so a swapped drive or a re-created folder counts as
 * a change too. About a dozen stat() calls. */
static uint64_t presence_signature(void) {
  uint64_t h = 1469598103934665603ULL;
#ifdef XENO_HOST
  char dl[600];
  host_downloads_dir(dl, sizeof dl);
  int ncand = 1 + g_cfg.n_extra_roots;
  for (int i = 0; i < ncand; i++) {
    const char *p = i == 0 ? dl : g_cfg.extra_roots[i - 1];
    struct stat st;
    int present = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
    uint64_t rec[3] = { (uint64_t)i, present ? (uint64_t)st.st_dev : 0, present ? (uint64_t)st.st_ino : 0 };
    h = fnv1a(h, &present, sizeof present);
    h = fnv1a(h, rec, sizeof rec);
  }
#else
  static const char *const cand[] = { "/data", "/mnt/ext0", "/mnt/ext1", "/mnt/usb0", "/mnt/usb1", "/mnt/usb2",
                                      "/mnt/usb3", "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7" };
  struct stat mnt;
  int have_mnt = stat("/mnt", &mnt) == 0;
  for (size_t i = 0; i < XS_ARRAY_LEN(cand); i++) {
    struct stat st;
    int present = stat(cand[i], &st) == 0 && S_ISDIR(st.st_mode);
    if (present && util_starts_with(cand[i], "/mnt/") && have_mnt && st.st_dev == mnt.st_dev) present = 0;   /* empty mount point */
    uint64_t rec[3] = { (uint64_t)i, present ? (uint64_t)st.st_dev : 0, present ? (uint64_t)st.st_ino : 0 };
    h = fnv1a(h, &present, sizeof present);
    h = fnv1a(h, rec, sizeof rec);
  }
#endif
  return h;
}

/* Only this thread uses it: kept off the stack. */
static storage_root g_scratch[STORAGE_MAX_ROOTS];

static void *storage_main(void *arg) {
  (void)arg;
  xthread_note("tv-storage");
  uint64_t last_sig = 0;
  int have_sig = 0;
  int64_t last_sig_ms = 0, last_scan_ms = 0, last_get_ms = 0;
  while (!g_cfg.stop_requested) {
    int64_t now = util_now_ms();
    pthread_mutex_lock(&tvdl_mu);
    int kick = g_kick, kick_rescan = g_kick_rescan;
    pthread_mutex_unlock(&tvdl_mu);
    int scan = kick_rescan || !have_sig || now - last_scan_ms >= SCAN_EVERY_MS;
    if (kick || !have_sig || now - last_sig_ms >= SIG_EVERY_MS) {
      xthread_note("tv-storage: presence");
      uint64_t sig = presence_signature();
      last_sig_ms = now;
      if (have_sig && sig != last_sig) { scan = 1; LOGI("tv-storage: the set of drives changed, rescanning"); }
      last_sig = sig;
      have_sig = 1;
    }
    if (scan || kick || now - last_get_ms >= GET_EVERY_MS) {
      if (scan) { xthread_note("tv-storage: scan"); storage_scan_roots(); last_scan_ms = now; }
      xthread_note("tv-storage: statvfs");
      int n = storage_get_roots(g_scratch, STORAGE_MAX_ROOTS);
      publish(g_scratch, n, 1, scan);
      last_get_ms = now;
      if (kick) {
        pthread_mutex_lock(&tvdl_mu);
        g_kick = 0;
        if (kick_rescan) g_kick_rescan = 0;
        pthread_mutex_unlock(&tvdl_mu);
      }
    }
    xthread_note("tv-storage: idle");
    for (int i = 0; i < 1000 / LOOP_TICK_MS && !g_cfg.stop_requested; i++) {
      util_sleep_ms(LOOP_TICK_MS);
      pthread_mutex_lock(&tvdl_mu);
      int k = g_kick;
      pthread_mutex_unlock(&tvdl_mu);
      if (k) break;
    }
  }
  return NULL;
}

void tvdl_storage_start(void) {
  /* Seed from what storage.c already knows (no filesystem access), so the
   * first /storage reply is not empty while the thread takes its first look. */
  storage_root *seed = calloc(STORAGE_MAX_ROOTS, sizeof *seed);
  if (seed) {
    int n = storage_peek_roots(seed, STORAGE_MAX_ROOTS);
    publish(seed, n, 0, 0);
    free(seed);
  }
  if (tv_thread_start(NULL, "tv-storage", storage_main, NULL, 0, 1) == 0) g_thread_started = 1;
  else LOGE("tv-storage: thread not started; the drive list stays as it was at start");
}

/* ---- diagnostics ----------------------------------------------------------- */

void tvdl_storage_diag_json(jw *w) {
  tv_drive *d = calloc(TVDL_MAX_DRIVES, sizeof *d);
  tvdl_snap_info info;
  memset(&info, 0, sizeof info);
  int n = d ? tvdl_snapshot(d, TVDL_MAX_DRIVES, 0, &info) : 0;
  jw_key(w, "storage"); jw_obj_begin(w);
  jw_kv_bool(w, "thread", g_thread_started);
  jw_kv_i64(w, "updatedAt", (int64_t)info.updated_at);
  jw_kv_i64(w, "updatedAgoMs", info.updated_ms ? util_now_ms() - info.updated_ms : -1);
  jw_kv_i64(w, "scannedAt", (int64_t)info.scanned_at);
  jw_kv_i64(w, "gen", (int64_t)info.gen);
  jw_kv_bool(w, "kickPending", info.kick_pending);
  jw_kv_int(w, "count", info.count);
  jw_obj_end(w);
  char pref[64];
  tvdl_preferred_storage_id(pref, sizeof pref);
  jw_kv_str(w, "preferredStorage", pref);
  jw_key(w, "drives"); jw_arr_begin(w);
  for (int i = 0; i < n; i++) {
    jw_obj_begin(w);
    jw_kv_str(w, "id", d[i].id);
    jw_kv_str(w, "label", d[i].label);
    jw_kv_str(w, "path", d[i].path);
    jw_kv_str(w, "kind", d[i].kind);
    jw_kv_bool(w, "external", d[i].external);
    jw_kv_bool(w, "writable", d[i].writable);
    jw_kv_bool(w, "default", d[i].is_default);
    jw_kv_int(w, "order", d[i].order);
    jw_kv_i64(w, "free", d[i].free);
    jw_kv_i64(w, "total", d[i].total);
    jw_kv_i64(w, "pending", dl_remaining_on_root(d[i].path));
    jw_obj_end(w);
  }
  jw_arr_end(w);
  free(d);
}
