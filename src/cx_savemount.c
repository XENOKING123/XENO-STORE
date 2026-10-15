/*
 * cx_savemount.c — see cx_savemount.h.
 *
 * Native tier: disabled on the console. The SceSaveData prototypes this file
 * used were the PS4 ones and do not match the PS5 library
 * (sceSaveDataMount2 takes no title id, so it can only mount the calling
 * process's own saves; sceSaveDataTransferringMount takes a parameter struct,
 * not positional arguments), and the tier is unavailable on 12.x firmware
 * anyway. Until a verified PS5 binding exists, imports and decrypted exports
 * go through Garlic Save Manager (cx_garlic.c) and everything else is a raw
 * same-console copy. The host build keeps a temp-directory simulation of the
 * mount tier (XENO_SIM_NATIVE=0 turns it off) so the flows stay testable.
 */
#include "cx_savemount.h"
#include "cx_accounts.h"
#include "cx_common.h"
#include "cx_garlic.h"
#include "cx_jobs.h"
#include "log.h"
#include "settings.h"
#include "storage.h"
#include "util.h"
#include "zip.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef XENO_HOST
#include <dlfcn.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#endif

#define SAVE_MAX_BYTES (4LL << 30)
#define SAVE_MAX_ENTRIES 4096

/* ---- native tier -------------------------------------------------------- */

#ifndef XENO_HOST
#define NATIVE_DISABLED_DETAIL "disabled: the SceSaveData mount prototypes are not verified for PS5 firmware (and the API is unavailable on 12.x); saves are re-signed through Garlic Save Manager"
static int native_available(char *detail, size_t cap) {
  if (detail) xstrlcpy(detail, NATIVE_DISABLED_DETAIL, cap);
  return 0;
}
#else
static int native_available(char *detail, size_t cap) {
  const char *ev = getenv("XENO_SIM_NATIVE");
  if (ev && strcmp(ev, "0") == 0) { if (detail) xstrlcpy(detail, "host simulation turned off (XENO_SIM_NATIVE=0)", cap); return 0; }
  if (detail) xstrlcpy(detail, "host simulation (temp-dir mounts)", cap);
  return 1;
}
#endif

/* ---- running title ------------------------------------------------------ */

#ifndef XENO_HOST
/* sceKernelGetAppInfo fills an app-info record whose title id sits at offset
 * 16 or 20 depending on the firmware (ps5-payload-dev SDK samples ps/ and
 * test_privileges/); the id is searched in the record's first 64 bytes so
 * either layout matches. */
typedef int (*kgetappinfo_fn)(int pid, void *info);

int cx_title_running(const char *titleId) {
  if (!cx_titleid_valid(titleId)) return 0;
  kgetappinfo_fn getinfo = (kgetappinfo_fn)dlsym(RTLD_DEFAULT, "sceKernelGetAppInfo");
  if (!getinfo) return -1;
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || size == 0) return -1;
  size += size / 4;   /* processes started between the two calls */
  unsigned char *buf = malloc(size);
  if (!buf) return -1;
  if (sysctl(mib, 4, buf, &size, NULL, 0) != 0) { free(buf); return -1; }
  int found = 0, procs = 0;
  size_t pid_off = offsetof(struct kinfo_proc, ki_pid);
  for (size_t off = 0; off + sizeof(int) <= size && !found;) {
    int ks;
    memcpy(&ks, buf + off, sizeof ks);
    if (ks <= (int)(pid_off + sizeof(pid_t)) || (size_t)ks > size - off) break;
    pid_t pid;
    memcpy(&pid, buf + off + pid_off, sizeof pid);
    unsigned char info[512];
    memset(info, 0, sizeof info);
    if (pid > 0 && getinfo((int)pid, info) == 0) {
      for (size_t i = 0; i + 9 <= 64 && !found; i++) if (memcmp(info + i, titleId, 9) == 0) found = 1;
    }
    procs++;
    off += (size_t)ks;
  }
  free(buf);
  return found ? 1 : procs > 0 ? 0 : -1;
}
#else
int cx_title_running(const char *titleId) {
  const char *e = getenv("XENO_HOST_RUNNING_TITLE");   /* tests: "PPSA01234[,CUSA…]" */
  if (!e || !*e || !cx_titleid_valid(titleId)) return 0;
  return strstr(e, titleId) != NULL;
}
#endif

/* ---- tier selection ----------------------------------------------------- */

void cx_savemount_init(void) {
  char d[200];
  int n = native_available(d, sizeof d);
  if (!n) LOGI("saves: native SaveData tier %s", d);
  LOGI("saves: engine tier %s (Garlic Save Manager on port %d)", cx_tier_name(cx_savemount_tier()), cx_garlic_port());
}

int cx_savemount_tier(void) {
  if (native_available(NULL, 0)) return CX_TIER_NATIVE;
  if (cx_garlic_running()) return CX_TIER_GARLIC;
  return CX_TIER_RAW;
}

const char *cx_tier_name(int tier) { return tier == CX_TIER_NATIVE ? "native" : tier == CX_TIER_GARLIC ? "garlic" : "raw"; }

void cx_savemount_engine_json(jw *w) {
  char nd[200];
  int native = native_available(nd, sizeof nd);
  int garlic = !native && cx_garlic_running();
  int tier = native ? CX_TIER_NATIVE : garlic ? CX_TIER_GARLIC : CX_TIER_RAW;
  jw_kv_str(w, "tier", cx_tier_name(tier));
  const char *detail = tier == CX_TIER_NATIVE ? "Host simulation of the save mount (temp directories)."
                       : tier == CX_TIER_GARLIC ? "Garlic Save Manager is running on this console: community saves are re-signed for your profile through it."
                       : "Raw copy only: restores of backups made on this console and profile work. Community saves need Garlic Save Manager, which XENO STORE starts on demand when it can.";
  jw_kv_str(w, "detail", detail);
  jw_key(w, "native"); jw_obj_begin(w); jw_kv_bool(w, "available", native); jw_kv_str(w, "detail", nd); jw_obj_end(w);
  cx_garlic_status_json(w);
  jw_key(w, "raw"); jw_obj_begin(w); jw_kv_bool(w, "available", 1); jw_kv_str(w, "detail", "Same console and profile only"); jw_obj_end(w);
  jw_kv_bool(w, "simulated", cx_is_host() && native);
}

/* ---- paths -------------------------------------------------------------- */

int cx_save_title_dir(uint32_t uid, const char *titleId, const char *kind, char *out, size_t cap, char *kind_out, size_t kcap) {
  char home[700];
  if (!cx_titleid_valid(titleId)) return -1;
  if (cx_user_home(uid, home, sizeof home) != 0) return -1;
  const char *k = kind && *kind ? kind : (titleId[0] == 'P' ? "ps5" : "ps4");
  if (strcmp(k, "ps5") != 0 && strcmp(k, "ps4") != 0) return -1;
  xsnprintf(out, cap, "%s/%s/%s", home, strcmp(k, "ps5") == 0 ? "savedata_prospero" : "savedata", titleId);
  if (kind_out) xstrlcpy(kind_out, k, kcap);
  return 0;
}

/* ---- param.sfo ---------------------------------------------------------- */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Finds an entry; returns the data offset/len (0 when absent). */
static int sfo_find(const uint8_t *buf, size_t len, const char *key, uint32_t *doff, uint32_t *dlen, uint32_t *dmax, uint16_t *fmt) {
  if (len < 20 || memcmp(buf, "\0PSF", 4) != 0) return -1;
  uint32_t key_start = rd32(buf + 8), data_start = rd32(buf + 12), n = rd32(buf + 16);
  if (n > 512 || key_start > len || data_start > len) return -1;
  for (uint32_t i = 0; i < n; i++) {
    size_t e = 20 + (size_t)i * 16;
    if (e + 16 > len) return -1;
    uint16_t koff = rd16(buf + e), f = rd16(buf + e + 2);
    uint32_t l = rd32(buf + e + 4), mx = rd32(buf + e + 8), off = rd32(buf + e + 12);
    size_t kpos = (size_t)key_start + koff;
    if (kpos >= len) return -1;
    size_t klen = strnlen((const char *)buf + kpos, len - kpos);
    if (klen == strlen(key) && memcmp(buf + kpos, key, klen) == 0) {
      if ((size_t)data_start + off + mx > len) return -1;
      *doff = data_start + off; *dlen = l; *dmax = mx; *fmt = f;
      return 0;
    }
  }
  return -1;
}

int cx_sfo_set_account_id(uint8_t *buf, size_t len, uint64_t id) {
  uint32_t off, l, mx;
  uint16_t fmt;
  if (sfo_find(buf, len, "ACCOUNT_ID", &off, &l, &mx, &fmt) != 0) return -1;
  memset(buf + off, 0, mx);
  if (fmt == 0x0204) {               /* utf-8 string: hex id */
    char hex[20];
    xsnprintf(hex, sizeof hex, "%016llx", (unsigned long long)id);
    size_t n = strlen(hex);
    if (n + 1 > mx) n = mx > 0 ? mx - 1 : 0;
    memcpy(buf + off, hex, n);
  } else {                            /* binary / int: little-endian 8 bytes */
    for (int i = 0; i < 8 && (uint32_t)i < mx; i++) buf[off + i] = (uint8_t)(id >> (8 * i));
  }
  return 0;
}

int cx_sfo_get_str(const uint8_t *buf, size_t len, const char *key, char *out, size_t cap) {
  uint32_t off, l, mx;
  uint16_t fmt;
  out[0] = 0;
  if (sfo_find(buf, len, key, &off, &l, &mx, &fmt) != 0) return -1;
  if (fmt == 0x0404) { xsnprintf(out, cap, "%u", rd32(buf + off)); return 0; }
  size_t n = strnlen((const char *)buf + off, mx);
  if (n >= cap) n = cap - 1;
  memcpy(out, buf + off, n);
  out[n] = 0;
  return 0;
}

/* ---- mount / unmount ---------------------------------------------------- */

#ifdef XENO_HOST
static void sim_store_dir(const cx_mount *m, char *out, size_t cap) { xsnprintf(out, cap, "%s/.sim/%s", m->title_dir, m->dirName); }
#endif

int cx_savemount_open(uint32_t uid, const char *titleId, const char *dirName, int rdwr, cx_mount *m, char *err, size_t errcap) {
  memset(m, 0, sizeof *m);
  if (!cx_titleid_valid(titleId)) { xsnprintf(err, errcap, "invalid title id"); return -1; }
  if (!cx_dirname_valid(dirName)) { xsnprintf(err, errcap, "invalid save directory name"); return -1; }
  if (cx_user_find(uid, NULL) != 0) { xsnprintf(err, errcap, "unknown user"); return -1; }
  m->uid = uid;
  m->rdwr = rdwr;
  xstrlcpy(m->titleId, titleId, sizeof m->titleId);
  xstrlcpy(m->dirName, dirName, sizeof m->dirName);
  char kind[8];
  if (cx_save_title_dir(uid, titleId, "", m->title_dir, sizeof m->title_dir, kind, sizeof kind) != 0) { xsnprintf(err, errcap, "user home not found"); return -1; }
  char nd[200];
  if (!native_available(nd, sizeof nd)) { xsnprintf(err, errcap, "the save mount tier is %s", nd); return -1; }
#ifdef XENO_HOST
  m->tier = CX_TIER_NATIVE;
  char store[1024], rnd[12];
  sim_store_dir(m, store, sizeof store);
  util_rand_hex(rnd, 8);
  xsnprintf(m->point, sizeof m->point, "%s/sim/savemount/%s-%s-%s", cx_data_dir(), titleId, dirName, rnd);
  if (util_mkdir_p(m->point) != 0) { xsnprintf(err, errcap, "cannot create mount directory"); return -1; }
  if (util_dir_exists(store)) { int64_t b = 0; if (cx_copy_tree(store, m->point, 8, &b) != 0) { xsnprintf(err, errcap, "cannot mirror the save"); return -1; } }
  else if (!rdwr) { cx_rm_rf(m->point, 4); xsnprintf(err, errcap, "save %s/%s not found", titleId, dirName); return -1; }
  else m->created = 1;
  return 0;
#else
  return -1;   /* not reached: native_available() is 0 on the console */
#endif
}

int cx_savemount_close(cx_mount *m, int commit) {
  if (!m->point[0]) return 0;
#ifdef XENO_HOST
  int rc = 0;
  if (commit) {
    char store[1024];
    sim_store_dir(m, store, sizeof store);
    cx_rm_rf(store, 8);
    util_mkdir_p(m->title_dir);
    int64_t b = 0;
    if (cx_copy_tree(m->point, store, 8, &b) != 0) rc = -1;
    /* raw-side image marker + sce_sys copy so the hub lists the save */
    char img[1200], sys_dir[1200];
    xsnprintf(img, sizeof img, "%s/sdimg_%s", m->title_dir, m->dirName);
    char marker[96];
    xsnprintf(marker, sizeof marker, "XENO-SIM-IMAGE %lld\n", (long long)b);
    util_write_file_atomic(img, marker, strlen(marker));
    xsnprintf(sys_dir, sizeof sys_dir, "%s/sce_sys/%s", m->title_dir, m->dirName);
    util_mkdir_p(sys_dir);
    char sfo_src[1200], sfo_dst[1300];
    xsnprintf(sfo_src, sizeof sfo_src, "%s/sce_sys/param.sfo", m->point);
    xsnprintf(sfo_dst, sizeof sfo_dst, "%s/param.sfo", sys_dir);
    if (util_file_exists(sfo_src)) cx_copy_file(sfo_src, sfo_dst);
  }
  cx_rm_rf(m->point, 8);
  m->point[0] = 0;
  return rc;
#else
  (void)commit;
  m->point[0] = 0;
  return 0;
#endif
}

/* ---- import ------------------------------------------------------------- */

typedef struct zip_layout {
  int style;              /* 0 decrypted, 1 raw (same-console backup) */
  int entries;
  char prefix[300];       /* stripped wrapper folder(s) with a trailing '/', "" when none */
  char dirHint[32];       /* folder name a "sdimg_<dir>" component gives */
} zip_layout;

/* A single top-level wrapper folder ("MySave/sce_sys/param.sfo") is stripped
 * the way tools/saves/intake.py does (one level, never "sce_sys"); Garlic's
 * own decrypted export ("<TID>/sdimg_<dir>/…") has a second level that is
 * stripped too. Junk entries (__MACOSX, .DS_Store) do not count. */
static void zip_wrapper(const zip_entry_info *list, int n, zip_layout *lay) {
  char prev[256] = "";
  for (int level = 0; level < 2; level++) {
    size_t pl = strlen(lay->prefix);
    char top[256] = "";
    int ok = 1, deeper = 0;
    for (int i = 0; i < n && ok; i++) {
      const char *nm = list[i].name;
      if (zip_name_is_junk(nm)) continue;
      if (strncmp(nm, lay->prefix, pl) != 0) { ok = 0; break; }
      const char *rest = nm + pl;
      if (!*rest) continue;                          /* the wrapper's own directory entry */
      const char *sl = strchr(rest, '/');
      if (!sl) { ok = 0; break; }                    /* a file at this level: no wrapper */
      size_t tl = (size_t)(sl - rest);
      if (tl == 0 || tl >= sizeof top) { ok = 0; break; }
      if (!top[0]) { memcpy(top, rest, tl); top[tl] = 0; }
      else if (strlen(top) != tl || strncmp(top, rest, tl) != 0) { ok = 0; break; }
      if (sl[1]) deeper = 1;
    }
    if (!ok || !top[0] || !deeper || util_strieq(top, "sce_sys")) return;
    if (level == 1 && !(cx_titleid_valid(prev) && util_starts_with(top, "sdimg_"))) return;
    if (pl + strlen(top) + 2 > sizeof lay->prefix) return;
    xstrlcat(lay->prefix, top, sizeof lay->prefix);
    xstrlcat(lay->prefix, "/", sizeof lay->prefix);
    if (util_starts_with(top, "sdimg_") && cx_dirname_valid(top + 6)) xstrlcpy(lay->dirHint, top + 6, sizeof lay->dirHint);
    xstrlcpy(prev, top, sizeof prev);
  }
}

/* raw: the zip carries a sealed image file (a component "sdimg_<dir>" that is
 * a file, not a folder) or the backup manifest; anything else is decrypted. */
static int zip_style(const char *zipPath, zip_layout *lay, char *err, size_t errcap) {
  memset(lay, 0, sizeof *lay);
  zip_entry_info *list = calloc(SAVE_MAX_ENTRIES, sizeof *list);
  if (!list) { xsnprintf(err, errcap, "out of memory"); return -1; }
  zip_stats st;
  int n = zip_list(zipPath, list, SAVE_MAX_ENTRIES, &st);
  if (n < 0) { xsnprintf(err, errcap, "cannot read zip: %s", st.error); free(list); return -1; }
  zip_wrapper(list, n, lay);
  size_t pl = strlen(lay->prefix);
  int raw = 0;
  for (int i = 0; i < n; i++) {
    const char *nm = list[i].name;
    if (zip_name_is_junk(nm)) continue;
    const char *rel = strncmp(nm, lay->prefix, pl) == 0 ? nm + pl : nm;
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    if (!list[i].is_dir && util_starts_with(base, "sdimg_")) {
      raw = 1;
      if (!lay->dirHint[0] && cx_dirname_valid(base + 6)) xstrlcpy(lay->dirHint, base + 6, sizeof lay->dirHint);
    } else if (strcmp(rel, "xeno-backup.json") == 0) raw = 1;
  }
  free(list);
  lay->entries = n;
  if (n == 0) { xsnprintf(err, errcap, "the zip is empty"); return -1; }
  lay->style = raw;
  return 0;
}

/* "<base>/<prefix without its trailing '/'>" */
static void prefixed(const char *base, const char *prefix, char *out, size_t cap) {
  if (!prefix[0]) { xstrlcpy(out, base, cap); return; }
  xsnprintf(out, cap, "%s/%s", base, prefix);
  size_t l = strlen(out);
  if (l && out[l - 1] == '/') out[l - 1] = 0;
}

static int do_pre_backup(uint32_t uid, const char *title_dir, const char *titleId, const char *kind, const char *backup_dir, char *out, size_t cap) {
  out[0] = 0;
  if (!util_dir_exists(title_dir) || cx_count_files(title_dir, 8) == 0) return 0;
  cx_user u;
  if (cx_user_find(uid, &u) != 0) return -1;
  char uname[64];
  if (storage_sanitize_filename(u.name, uname, sizeof uname) != 0) xsnprintf(uname, sizeof uname, "user-%x", (unsigned)uid);
  char dir[1100], stamp[24];
  xsnprintf(dir, sizeof dir, "%s/%s", backup_dir, uname);
  if (util_mkdir_p(dir) != 0) return -1;
  cx_stamp(stamp, sizeof stamp);
  xsnprintf(out, cap, "%s/%s_%s_%s.pre-restore.zip", dir, titleId, kind, stamp);
  zip_stats st;
  if (zip_create_from_dir(out, title_dir, NULL, NULL, &st) != 0) { LOGW("saves: pre-backup failed: %s", st.error); out[0] = 0; return -1; }
  LOGI("saves: pre-backup %s", out);
  return 0;
}

int cx_savemount_import_zip(uint32_t uid, const char *titleId, const char *dirName, const char *zipPath,
                            int backup_first, const char *backup_dir, cx_import_result *out,
                            char *code, size_t ccap, char *err, size_t errcap) {
  return cx_savemount_import_zip_job(uid, titleId, dirName, zipPath, backup_first, backup_dir, NULL, out, code, ccap, err, errcap);
}

int cx_savemount_import_zip_job(uint32_t uid, const char *titleId, const char *dirName, const char *zipPath,
                                int backup_first, const char *backup_dir, cx_job *j, cx_import_result *out,
                                char *code, size_t ccap, char *err, size_t errcap) {
  memset(out, 0, sizeof *out);
  xstrlcpy(code, "import_failed", ccap);
  char title_dir[1024], kind[8];
  if (!cx_titleid_valid(titleId)) { xstrlcpy(code, "bad_request", ccap); xsnprintf(err, errcap, "invalid title id"); return -1; }
  if (cx_save_title_dir(uid, titleId, "", title_dir, sizeof title_dir, kind, sizeof kind) != 0) { xstrlcpy(code, "not_found", ccap); xsnprintf(err, errcap, "user home not found"); return -1; }
  char dn[32] = "";
  if (dirName && *dirName) xstrlcpy(dn, dirName, sizeof dn);
  /* manifest (xeno-save.json) may name the dir */
  void *mf = NULL;
  size_t mflen = 0;
  if (zip_extract_to_memory(zipPath, "xeno-save.json", &mf, &mflen, 64 << 10) == 0) {
    jdoc *d = json_parse(mf, mflen, 2000, 8);
    if (d) {
      const char *mt = jv_get_str(json_root(d), "titleId", "");
      if (*mt && strcmp(mt, titleId) != 0) { json_free(d); free(mf); xstrlcpy(code, "title_mismatch", ccap); xsnprintf(err, errcap, "this save is for %s, not %s", mt, titleId); return -1; }
      if (!dn[0]) xstrlcpy(dn, jv_get_str(json_root(d), "dirName", ""), sizeof dn);
      json_free(d);
    }
    free(mf);
  }
  if (dn[0] && !cx_dirname_valid(dn)) dn[0] = 0;
  zip_layout lay;
  if (zip_style(zipPath, &lay, err, errcap) != 0) return -1;
  if (!dn[0]) xstrlcpy(dn, lay.dirHint, sizeof dn);
  xstrlcpy(out->style, lay.style == 1 ? "raw" : "decrypted", sizeof out->style);
  /* a running (or suspended) game keeps its save mounted */
  if (cx_title_running(titleId) == 1) {
    xstrlcpy(code, "title_running", ccap);
    xsnprintf(err, errcap, "%s is running: close the game (not just suspend it) before its save is replaced", titleId);
    return -1;
  }
  int native = native_available(NULL, 0);
  if (lay.style != 1 && !native) {
    /* decrypted saves are re-signed by Garlic: start it before anything changes */
    if (util_starts_with(titleId, "CUSA")) {
      xstrlcpy(code, "unsupported", ccap);
      xsnprintf(err, errcap, "Decrypted PS4 saves cannot be installed: Garlic Save Manager's import builds PS5 saves only");
      return -1;
    }
    char e2[300] = "";
    if (!cx_garlic_ensure(1, e2, sizeof e2)) {
      xstrlcpy(code, "engine_unavailable", ccap);
      xsnprintf(err, errcap, "This save has to be re-signed for your profile by Garlic Save Manager: %s", e2);
      return -1;
    }
  }
  if (backup_first && do_pre_backup(uid, title_dir, titleId, kind, backup_dir, out->preBackup, sizeof out->preBackup) != 0) {
    xsnprintf(err, errcap, "could not back up the current save first — nothing was changed");
    return -1;
  }
  if (lay.style == 1) {
    /* raw backup made on this console: restore the title folder as-is */
    xstrlcpy(out->tier, "raw", sizeof out->tier);
    void *bm = NULL;
    size_t bmlen = 0;
    if (zip_extract_to_memory(zipPath, "xeno-backup.json", &bm, &bmlen, 64 << 10) == 0) {
      jdoc *d = json_parse(bm, bmlen, 1000, 8);
      int64_t from = d ? jv_get_i64(jv_get(json_root(d), "user"), "id", 0) : 0;
      if (from && (uint32_t)from != uid) {
        xsnprintf(out->note, sizeof out->note, "This backup was made on another profile: a raw restore keeps that profile's account in the save (same-profile only), so the game may refuse it.");
        LOGW("saves: raw restore of %s from profile %x into %x — same-profile only, not re-signed", titleId, (unsigned)from, (unsigned)uid);
      }
      if (d) json_free(d);
      free(bm);
    }
    char tmp[1100], src[1500];
    xsnprintf(tmp, sizeof tmp, "%s.restore-tmp", title_dir);
    cx_rm_rf(tmp, 8);
    zip_stats st;
    if (zip_extract_to_dir(zipPath, tmp, (uint64_t)SAVE_MAX_BYTES, NULL, NULL, &st) != 0) { cx_rm_rf(tmp, 8); xsnprintf(err, errcap, "extract failed: %s", st.error); return -1; }
    prefixed(tmp, lay.prefix, src, sizeof src);
    char manifest[1600];
    xsnprintf(manifest, sizeof manifest, "%s/xeno-backup.json", src);
    util_unlink_quiet(manifest);
    cx_rm_rf(title_dir, 8);
    if (rename(src, title_dir) != 0) { xsnprintf(err, errcap, "cannot move the restored save into place: %s", strerror(errno)); cx_rm_rf(tmp, 8); return -1; }
    if (lay.prefix[0]) cx_rm_rf(tmp, 8);
    out->files = st.extracted;
    out->bytes = (int64_t)st.bytes;
    return 0;
  }
  char stage[1100], rnd[12];
  util_rand_hex(rnd, 8);
  xsnprintf(stage, sizeof stage, "%s/%s/stage-%s-%s", cx_data_dir(), cx_is_host() ? "sim" : "stage", titleId, rnd);
  cx_rm_rf(stage, 8);
  zip_stats st;
  if (zip_extract_to_dir(zipPath, stage, (uint64_t)SAVE_MAX_BYTES, NULL, NULL, &st) != 0) { cx_rm_rf(stage, 8); xsnprintf(err, errcap, "extract failed: %s", st.error); return -1; }
  char root[1500], p[1700];
  prefixed(stage, lay.prefix, root, sizeof root);
  xsnprintf(p, sizeof p, "%s/sce_sys/param.sfo", root);
  if (!util_file_exists(p)) {
    cx_rm_rf(stage, 8);
    xstrlcpy(code, "invalid_save", ccap);
    xsnprintf(err, errcap, "this zip has no sce_sys/param.sfo at the save root, so it is not a decrypted save — nothing was installed");
    return -1;
  }
  /* never carry sealed keys / manifests into the save */
  xsnprintf(p, sizeof p, "%s/xeno-save.json", root); util_unlink_quiet(p);
  xsnprintf(p, sizeof p, "%s/sce_sys/sealedkey", root); util_unlink_quiet(p);
  xsnprintf(p, sizeof p, "%s/sce_sys/keystone", root); util_unlink_quiet(p);
  /* ACCOUNT_ID → this user's account id (0 when PSN-less and not activated) */
  char acct[20] = "";
  cx_user_account_id(uid, acct, sizeof acct);
  uint64_t aid = acct[0] ? strtoull(acct, NULL, 16) : 0;
  xstrlcpy(out->accountId, acct[0] ? acct : "0", sizeof out->accountId);
  char sfo_path[1600];
  xsnprintf(sfo_path, sizeof sfo_path, "%s/sce_sys/param.sfo", root);
  char sfo_tid[16] = "", sfo_dir[64] = "";
  size_t sl = 0;
  char *sfo = util_read_file(sfo_path, &sl, 1 << 20);
  if (sfo) {
    if (cx_sfo_set_account_id((uint8_t *)sfo, sl, aid) == 0) util_write_file_atomic(sfo_path, sfo, sl);
    cx_sfo_get_str((uint8_t *)sfo, sl, "TITLE_ID", sfo_tid, sizeof sfo_tid);
    cx_sfo_get_str((uint8_t *)sfo, sl, "SAVEDATA_DIRECTORY", sfo_dir, sizeof sfo_dir);
    free(sfo);
  }
  if (!dn[0] && cx_dirname_valid(sfo_dir)) xstrlcpy(dn, sfo_dir, sizeof dn);
  if (!dn[0]) xstrlcpy(dn, "SAVEDATA00", sizeof dn);
  if (!native) {
    /* Garlic installs under the param.sfo's TITLE_ID and SAVEDATA_DIRECTORY */
    xstrlcpy(out->tier, "garlic", sizeof out->tier);
    if (cx_titleid_valid(sfo_tid) && strcmp(sfo_tid, titleId) != 0) {
      cx_rm_rf(stage, 8);
      xstrlcpy(code, "title_mismatch", ccap);
      xsnprintf(err, errcap, "this save's param.sfo is for %s, not %s", sfo_tid, titleId);
      return -1;
    }
    cx_garlic_result gr;
    int rc = cx_garlic_import_dir_ex(root, titleId, uid, dn, (int64_t)st.bytes, j, &gr, err, errcap);
    cx_rm_rf(stage, 8);
    if (rc != 0) return -1;
    out->files = gr.files;
    out->bytes = gr.bytes;
    if (gr.warning[0]) xsnprintf(out->note, sizeof out->note, "Garlic Save Manager: %s", gr.warning);
    else if (gr.forced) xstrlcpy(out->note, "The save's param.sfo was empty; Garlic Save Manager installed it with the folder name from the save store.", sizeof out->note);
    return 0;
  }
  xstrlcpy(out->tier, "native", sizeof out->tier);
  cx_mount m;
  if (cx_savemount_open(uid, titleId, dn, 1, &m, err, errcap) != 0) { cx_rm_rf(stage, 8); return -1; }
  /* replace the mounted content with the staged files */
  int64_t bytes = 0;
  int rc = 0;
  DIR *d = opendir(m.point);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 || strcmp(e->d_name, "sce_sys") == 0) continue;
      char q[1200];
      xsnprintf(q, sizeof q, "%s/%s", m.point, e->d_name);
      cx_rm_rf(q, 8);
    }
    closedir(d);
  }
  d = opendir(root);
  if (!d) rc = -1;
  else {
    struct dirent *e;
    while (rc == 0 && (e = readdir(d)) != NULL) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
      char src[1800], dst[1200];
      xsnprintf(src, sizeof src, "%s/%s", root, e->d_name);
      xsnprintf(dst, sizeof dst, "%s/%s", m.point, e->d_name);
      struct stat sb;
      if (lstat(src, &sb) != 0) continue;
      if (S_ISDIR(sb.st_mode)) {
        if (cx_copy_tree(src, dst, 8, &bytes) != 0) rc = -1;
      } else if (S_ISREG(sb.st_mode)) {
        if (cx_copy_file(src, dst) != 0) rc = -1; else { bytes += (int64_t)sb.st_size; out->files++; }
      }
    }
    closedir(d);
  }
  if (rc != 0) xsnprintf(err, errcap, "copying files into the mounted save failed");
  if (cx_savemount_close(&m, rc == 0) != 0 && rc == 0) { rc = -1; xsnprintf(err, errcap, "unmount failed — the save may be incomplete"); }
  cx_rm_rf(stage, 8);
  out->bytes = bytes;
  return rc;
}

/* ---- export ------------------------------------------------------------- */

static int hash_tree(const char *root, const char *rel, jw *w, int depth, int *files) {
  if (depth > 8) return 0;
  char dir[1100];
  if (rel[0]) xsnprintf(dir, sizeof dir, "%s/%s", root, rel); else xstrlcpy(dir, root, sizeof dir);
  DIR *d = opendir(dir);
  if (!d) return -1;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char relp[1024], full[1200];
    if (rel[0]) xsnprintf(relp, sizeof relp, "%s/%s", rel, e->d_name); else xstrlcpy(relp, e->d_name, sizeof relp);
    xsnprintf(full, sizeof full, "%s/%s", root, relp);
    struct stat sb;
    if (lstat(full, &sb) != 0) continue;
    if (S_ISDIR(sb.st_mode)) { hash_tree(root, relp, w, depth + 1, files); continue; }
    if (!S_ISREG(sb.st_mode)) continue;
    char hex[65] = "";
    cx_sha256_file(full, hex);
    jw_obj_begin(w);
    jw_kv_str(w, "path", relp);
    jw_kv_i64(w, "size", (int64_t)sb.st_size);
    jw_kv_str(w, "sha256", hex);
    jw_obj_end(w);
    (*files)++;
  }
  closedir(d);
  return 0;
}

int cx_savemount_export(uint32_t uid, const char *titleId, const char *dirName, const char *dest_zip,
                        cx_export_result *out, char *err, size_t errcap) {
  memset(out, 0, sizeof *out);
  char title_dir[1024], kind[8];
  if (cx_save_title_dir(uid, titleId, "", title_dir, sizeof title_dir, kind, sizeof kind) != 0) { xsnprintf(err, errcap, "user home not found"); return -1; }
  if (!util_dir_exists(title_dir)) { xsnprintf(err, errcap, "no save for %s on this profile", titleId); return -1; }
  int tier = cx_savemount_tier();
  char stage[1100];
  xsnprintf(stage, sizeof stage, "%s/%s/export-%s", cx_data_dir(), cx_is_host() ? "sim" : "stage", titleId);
  cx_rm_rf(stage, 8);
  if (util_mkdir_p(stage) != 0) { xsnprintf(err, errcap, "cannot create staging directory"); return -1; }
  int64_t bytes = 0;
  int decrypted = 0, via_garlic = 0;
  if (tier == CX_TIER_NATIVE && dirName && *dirName) {
    cx_mount m;
    if (cx_savemount_open(uid, titleId, dirName, 0, &m, err, errcap) == 0) {
      if (cx_copy_tree(m.point, stage, 8, &bytes) == 0) decrypted = 1;
      cx_savemount_close(&m, 0);
    } else LOGW("saves: native export mount failed (%s), falling back to raw", err);
  } else if (tier == CX_TIER_GARLIC && dirName && cx_dirname_valid(dirName)) {
    char e2[300] = "";
    if (cx_title_running(titleId) == 1) xsnprintf(e2, sizeof e2, "%s is running", titleId);
    else if (cx_garlic_export_dec_dir(titleId, dirName, uid, stage, NULL, e2, sizeof e2) == 0) { decrypted = 1; via_garlic = 1; bytes = cx_dir_size(stage, 16); }
    if (!decrypted) {
      LOGW("saves: decrypted export through Garlic failed (%s), falling back to raw", e2);
      xsnprintf(out->note, sizeof out->note, "Garlic Save Manager could not decrypt the save (%s): this is a raw backup for this console and profile only.", e2);
      if (util_mkdir_p(stage) != 0) { xsnprintf(err, errcap, "cannot create staging directory"); return -1; }
    }
  }
  if (!decrypted) {
    cx_rm_rf(stage, 8);
    if (cx_copy_tree(title_dir, stage, 8, &bytes) != 0) { xsnprintf(err, errcap, "cannot copy the save folder"); cx_rm_rf(stage, 8); return -1; }
    char sim[1200];
    xsnprintf(sim, sizeof sim, "%s/.sim", stage);
    cx_rm_rf(sim, 8);
  }
  /* strip sealed keys and the account id */
  char p[1200];
  xsnprintf(p, sizeof p, "%s/sce_sys/sealedkey", stage); util_unlink_quiet(p);
  xsnprintf(p, sizeof p, "%s/sce_sys/keystone", stage); util_unlink_quiet(p);
  if (decrypted) {
    xsnprintf(p, sizeof p, "%s/sce_sys/param.sfo", stage);
    size_t sl = 0;
    char *sfo = util_read_file(p, &sl, 1 << 20);
    if (sfo) {
      if (cx_sfo_set_account_id((uint8_t *)sfo, sl, 0) == 0) { util_write_file_atomic(p, sfo, sl); out->accountIdStripped = 1; }
      free(sfo);
    } else out->accountIdStripped = 1;   /* nothing to strip */
  }
  jw mw;
  jw_init(&mw);
  jw_obj_begin(&mw);
  jw_kv_str(&mw, "titleId", titleId);
  jw_kv_str(&mw, "dirName", dirName ? dirName : "");
  jw_kv_str(&mw, "kind", kind);
  jw_kv_str(&mw, "format", decrypted ? "xeno-save-zip" : "xeno-raw-zip");
  jw_kv_bool(&mw, "accountIdStripped", out->accountIdStripped);
  jw_kv_bool(&mw, "sameConsoleOnly", !decrypted);
  jw_kv_str(&mw, "createdBy", "XENO STORE " XENO_VERSION);
  char iso[40];
  util_iso8601(util_now(), iso, sizeof iso);
  jw_kv_str(&mw, "createdAt", iso);
  jw_key(&mw, "files"); jw_arr_begin(&mw);
  int files = 0;
  hash_tree(stage, "", &mw, 0, &files);
  jw_arr_end(&mw);
  jw_obj_end(&mw);
  size_t ml;
  char *mtxt = jw_detach(&mw, &ml);
  if (mtxt) {
    xsnprintf(p, sizeof p, "%s/xeno-save.json", stage);
    util_write_file_atomic(p, mtxt, ml);
    if (ml < sizeof out->manifest) memcpy(out->manifest, mtxt, ml + 1); else xstrlcpy(out->manifest, "{}", sizeof out->manifest);
    free(mtxt);
  }
  storage_ensure_parent_dir(dest_zip);
  zip_stats st;
  if (zip_create_from_dir(dest_zip, stage, NULL, NULL, &st) != 0) { xsnprintf(err, errcap, "zip failed: %s", st.error); cx_rm_rf(stage, 8); return -1; }
  cx_rm_rf(stage, 8);
  xstrlcpy(out->tier, decrypted ? cx_tier_name(via_garlic ? CX_TIER_GARLIC : CX_TIER_NATIVE) : "raw", sizeof out->tier);
  xstrlcpy(out->zipPath, dest_zip, sizeof out->zipPath);
  xstrlcpy(out->format, decrypted ? "xeno-save-zip" : "xeno-raw-zip", sizeof out->format);
  out->size = util_file_size(dest_zip);
  out->files = files;
  cx_sha256_file(dest_zip, out->sha256);
  return 0;
}
