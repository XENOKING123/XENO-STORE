/*
 * cx_garlic.c — see cx_garlic.h.
 *
 * Every route below was read from earthonion/garlic-savemgr src/main.c:
 * request_thread() reads the request with ONE recv() (main.c:1466) and the
 * upload handler looks for Content-Length and the blank line only in that
 * buffer (main.c:3223-3229), so requests go out through cx_local_http(),
 * whose request line and headers leave in a single send(). Garlic keeps one
 * global mount, so this module runs one import/export at a time and never
 * touches a save the user opened in Garlic's own page (status "mounted").
 */
#include "cx_garlic.h"
#include "config.h"
#include "cx_common.h"
#include "cx_companions.h"
#include "cx_jobs.h"
#include "cx_savemount.h"
#include "http_client.h"
#include "log.h"
#include "util.h"
#include "zip.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define GARLIC_PORT_DEFAULT 8082
#define GARLIC_START_WAIT_MS 20000
#define GARLIC_PROBE_TTL_MS 4000
#define GARLIC_BUSY_GRACE_MS 120000    /* a confirmed Garlic that stops answering is busy, not gone */
#define GARLIC_FIND_TTL_MS 60000
#define GARLIC_FIND_BUDGET_MS 2500
#define GARLIC_FIND_BUDGET_ENTRIES 6000
#define GARLIC_MAX_FILES 4096
#define GARLIC_BLOCK 32768             /* PFS save block: every file and folder takes at least one */
#define GARLIC_NAME_MAX 511            /* Garlic copies the encoded name into char[512] (main.c:3209) */
#define GARLIC_EXPORT_MAX (4LL << 30)
#define GARLIC_ELF_MIN 4096
#define GARLIC_ELF_MAX (64LL << 20)

enum { GP_DOWN = 0, GP_RUNNING = 1, GP_BUSY = 2, GP_OTHER = 3 };

static pthread_mutex_t probe_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t op_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ensure_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t find_mu = PTHREAD_MUTEX_INITIALIZER;

static struct {
  int state, mounted;
  int64_t at_ms, ok_ms;
  char mount_point[256];
} probe;
static volatile int op_active;

typedef struct found_elf { char path[700]; char source[12]; int64_t size; } found_elf;
static struct { int valid; int64_t at_ms; found_elf f; } found_cache;
static struct { char path[700]; char source[12]; int64_t at; } last_start;

int cx_garlic_port(void) { return cx_local_port("GARLIC", GARLIC_PORT_DEFAULT); }

/* ---- HTTP --------------------------------------------------------------- */

static void route_of(const char *pathq, char *out, size_t cap) {
  xstrlcpy(out, pathq, cap);
  char *q = strchr(out, '?');
  if (q) *q = 0;
}

/* One Garlic call answered with a JSON object; NULL with err otherwise. */
static jdoc *gcall(const char *method, const char *pathq, const char *body_file, int timeout_ms, size_t max_resp, size_t max_nodes,
                   char *err, size_t errcap) {
  int status = 0;
  size_t len = 0;
  char e2[200] = "", route[64];
  route_of(pathq, route, sizeof route);
  char *body = cx_local_http(cx_garlic_port(), method, pathq, NULL, 0, body_file, timeout_ms, max_resp, &status, &len, e2, sizeof e2);
  if (!body) { xsnprintf(err, errcap, "Garlic Save Manager did not answer %s (%s)", route, e2); return NULL; }
  jdoc *d = json_parse(body, len, max_nodes, 16);
  free(body);
  if (!d || jv_type(json_root(d)) != JV_OBJ) {
    if (d) json_free(d);
    xsnprintf(err, errcap, "Garlic Save Manager sent an unexpected answer to %s (HTTP %d)", route, status);
    return NULL;
  }
  return d;
}

/* 1 when the answer carries an "error" (copied into err). */
static int gerr(jdoc *d, char *err, size_t errcap) {
  jv e = jv_get(json_root(d), "error");
  if (!jv_ok(e)) return 0;
  xsnprintf(err, errcap, "Garlic Save Manager: %s", jv_str(e, "unknown error"));
  return 1;
}

/* Success the way Garlic means it: "ok":true and no "error". */
static int gok(jdoc *d, const char *what, char *err, size_t errcap) {
  if (gerr(d, err, errcap)) return 0;
  if (!jv_get_bool(json_root(d), "ok", 0)) { xsnprintf(err, errcap, "Garlic Save Manager did not confirm %s", what); return 0; }
  return 1;
}

static void gunmount(void) {
  char e[160];
  jdoc *d = gcall("GET", "/api/unmount", NULL, 30000, 4096, 64, e, sizeof e);
  if (d) { LOGI("garlic: unmount -> ok=%d", jv_get_bool(json_root(d), "ok", 0)); json_free(d); }
  else LOGW("garlic: unmount failed: %s", e);
}

/* Query value encoding for upload names: Garlic decodes %XX only, '/' may
 * stay literal (keeps names under its 511-byte limit). */
static int enc_name(const char *in, char *out, size_t cap) {
  static const char hexd[] = "0123456789ABCDEF";
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
    unsigned char c = *p;
    int keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
    if (o + (keep ? 1 : 3) >= cap) return -1;
    if (keep) out[o++] = (char)c;
    else { out[o++] = '%'; out[o++] = hexd[c >> 4]; out[o++] = hexd[c & 15]; }
  }
  out[o] = 0;
  return (int)o;
}

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int gconnect(int port, int timeout_ms) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
  if (rc != 0 && errno != EINPROGRESS) { close(fd); return -1; }
  if (rc != 0) {
    struct pollfd p = { fd, POLLOUT, 0 };
    if (poll(&p, 1, timeout_ms) <= 0) { close(fd); return -1; }
    int soerr = 0;
    socklen_t sl = sizeof soerr;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr != 0) { close(fd); return -1; }
  }
  fcntl(fd, F_SETFL, fl);
  struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  return fd;
}

/* GET streamed into a file (export zips and single files: never held in
 * RAM). Garlic answers zips without Content-Length and closes. */
static int gstream(const char *pathq, const char *dest, int64_t max, int timeout_ms, cx_job *j, int64_t *got_out,
                   char *ctype, size_t ccap, char *err, size_t errcap) {
  if (got_out) *got_out = 0;
  if (ctype && ccap) ctype[0] = 0;
  int port = cx_garlic_port();
  int fd = gconnect(port, timeout_ms < 3000 ? timeout_ms : 3000);
  if (fd < 0) { xsnprintf(err, errcap, "Garlic Save Manager is not listening on 127.0.0.1:%d", port); return -1; }
  struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  char head[2600];
  int hl = xsnprintf(head, sizeof head, "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUser-Agent: XENO-STORE/%s\r\nAccept: */*\r\nConnection: close\r\n\r\n",
                     pathq, port, XENO_VERSION);
  if ((size_t)hl >= sizeof head - 1) { close(fd); xsnprintf(err, errcap, "request too long"); return -1; }
  for (int off = 0; off < hl;) {
    ssize_t n = send(fd, head + off, (size_t)(hl - off), MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { close(fd); xsnprintf(err, errcap, "sending to Garlic Save Manager failed: %s", strerror(errno)); return -1; }
    off += (int)n;
  }
  const size_t chunk = 256 << 10;
  char *buf = malloc(chunk + 1);
  if (!buf) { close(fd); xsnprintf(err, errcap, "out of memory"); return -1; }
  size_t have = 0;
  char *he = NULL;
  while (!he) {
    if (have >= 16384) break;
    ssize_t n = recv(fd, buf + have, 16384 - have, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    have += (size_t)n;
    buf[have] = 0;
    he = strstr(buf, "\r\n\r\n");
  }
  if (!he || have < 12 || !util_starts_with(buf, "HTTP/1.")) { free(buf); close(fd); xsnprintf(err, errcap, "Garlic Save Manager sent no HTTP answer"); return -1; }
  int status = atoi(buf + 9);
  int64_t clen = -1;
  const char *cl = util_strcasestr(buf, "\r\nContent-Length:");
  if (cl && cl < he) clen = strtoll(cl + 17, NULL, 10);
  const char *ct = util_strcasestr(buf, "\r\nContent-Type:");
  if (ct && ct < he && ctype && ccap) {
    ct += 15;
    while (*ct == ' ') ct++;
    size_t k = 0;
    while (ct[k] && ct[k] != '\r' && ct[k] != ';' && k + 1 < ccap) { ctype[k] = ct[k]; k++; }
    ctype[k] = 0;
  }
  if (status != 200) { free(buf); close(fd); xsnprintf(err, errcap, "Garlic Save Manager answered HTTP %d", status); return -1; }
  int out = open(dest, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (out < 0) { free(buf); close(fd); xsnprintf(err, errcap, "cannot create %s: %s", dest, strerror(errno)); return -1; }
  int64_t got = 0;
  int rc = 0;
  size_t off = (size_t)(he + 4 - buf);
  if (have > off) {
    size_t n = have - off;
    if (write(out, buf + off, n) != (ssize_t)n) rc = -1;
    got += (int64_t)n;
  }
  while (rc == 0 && (clen < 0 || got < clen)) {
    if (got > max) { xsnprintf(err, errcap, "Garlic's answer is larger than %lld bytes", (long long)max); rc = -1; break; }
    if (j && cx_job_cancelled(j)) { xsnprintf(err, errcap, "cancelled"); rc = -1; break; }
    ssize_t n = recv(fd, buf, chunk, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) { xsnprintf(err, errcap, "reading from Garlic Save Manager failed: %s", strerror(errno)); rc = -1; break; }
    if (n == 0) break;
    if (write(out, buf, (size_t)n) != n) { xsnprintf(err, errcap, "cannot write %s: %s", dest, strerror(errno)); rc = -1; break; }
    got += n;
  }
  if (rc == 0 && clen >= 0 && got != clen) { xsnprintf(err, errcap, "short answer from Garlic Save Manager (%lld of %lld bytes)", (long long)got, (long long)clen); rc = -1; }
  close(out);
  free(buf);
  close(fd);
  if (got_out) *got_out = got;
  if (rc != 0) util_unlink_quiet(dest);
  return rc;
}

/* ---- probe -------------------------------------------------------------- */

/* GET /api/status → {"mounted":bool,"mount_point":"..","save_path":".."} */
static int status_call(int timeout_ms, int *mounted, char *mp, size_t mcap) {
  int port = cx_garlic_port();
  if (!cx_tcp_probe(port, 300)) return GP_DOWN;
  int status = 0;
  size_t len = 0;
  char e[160];
  char *body = cx_local_http(port, "GET", "/api/status", NULL, 0, NULL, timeout_ms, 8192, &status, &len, e, sizeof e);
  if (!body) return cx_tcp_probe(port, 300) ? GP_BUSY : GP_DOWN;
  int st = GP_OTHER;
  jdoc *d = json_parse(body, len, 256, 8);
  if (d && jv_type(json_root(d)) == JV_OBJ && jv_type(jv_get(json_root(d), "mounted")) == JV_BOOL) {
    st = GP_RUNNING;
    if (mounted) *mounted = jv_get_bool(json_root(d), "mounted", 0);
    if (mp) xstrlcpy(mp, jv_get_str(json_root(d), "mount_point", ""), mcap);
  }
  if (d) json_free(d);
  free(body);
  return st;
}

static int garlic_probe(int force) {
  if (op_active) return GP_RUNNING;                 /* our own import/export holds Garlic's lock */
  if (force) pthread_mutex_lock(&probe_mu);
  else if (pthread_mutex_trylock(&probe_mu) != 0) return probe.state;   /* a refresh is running: use the last answer */
  int64_t now = util_now_ms();
  if (!force && probe.at_ms && now - probe.at_ms < GARLIC_PROBE_TTL_MS) { int s = probe.state; pthread_mutex_unlock(&probe_mu); return s; }
  int mounted = 0;
  char mp[256] = "";
  int s = status_call(force ? 5000 : 1500, &mounted, mp, sizeof mp);
  if (s == GP_RUNNING) { probe.ok_ms = util_now_ms(); probe.mounted = mounted; xstrlcpy(probe.mount_point, mp, sizeof probe.mount_point); }
  if (s == GP_DOWN && probe.state == GP_RUNNING) last_start.path[0] = 0;   /* the copy the store started is gone */
  if (s != probe.state) LOGI("garlic: port %d %s", cx_garlic_port(), s == GP_RUNNING ? "answers" : s == GP_BUSY ? "is open but /api/status does not answer" : s == GP_OTHER ? "answers but is not Garlic Save Manager" : "is closed");
  probe.state = s;
  probe.at_ms = util_now_ms();
  pthread_mutex_unlock(&probe_mu);
  return s;
}

static int busy_but_known(void) { return probe.ok_ms && util_now_ms() - probe.ok_ms < GARLIC_BUSY_GRACE_MS; }

/* Garlic before 1.6 has no import API: unknown routes answer HTTP 404 there,
 * while 1.6 and later answer import_check with 200 ({"error":"No save
 * mounted"} when nothing is open). 1 = usable (or unknown: busy, not old). */
static int api_ok(char *err, size_t errcap) {
  if (op_active) return 1;
  int status = 0;
  size_t len = 0;
  char e[160];
  char *body = cx_local_http(cx_garlic_port(), "GET", "/api/import_check", NULL, 0, NULL, 5000, 4096, &status, &len, e, sizeof e);
  free(body);
  if (status != 404) return 1;   /* no answer = busy, not old */
  xsnprintf(err, errcap, "Garlic Save Manager on port %d is older than 1.6 and has no import API — close it (or restart the console) so the store can start %s",
            cx_garlic_port(), CX_GARLIC_ELF_VERSION);
  return 0;
}

int cx_garlic_running(void) {
  int s = garlic_probe(0);
  return s == GP_RUNNING || (s == GP_BUSY && busy_but_known());
}

/* ---- finding an ELF ----------------------------------------------------- */

static int file_is_elf(const char *path) {
  unsigned char h[4] = {0, 0, 0, 0};
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  int ok = fread(h, 1, 4, f) == 4 && memcmp(h, "\x7f" "ELF", 4) == 0;
  fclose(f);
  return ok;
}

static int garlic_elf_name(const char *name) { return util_istarts_with(name, "garlic") && util_iends_with(name, ".elf"); }

static int candidate_ok(const char *path, int64_t *size) {
  struct stat sb;
  if (lstat(path, &sb) != 0 || !S_ISREG(sb.st_mode)) return 0;
  if (sb.st_size < GARLIC_ELF_MIN || sb.st_size > GARLIC_ELF_MAX) return 0;
  if (!file_is_elf(path)) return 0;
  if (size) *size = (int64_t)sb.st_size;
  return 1;
}

typedef struct scan_ctx { int64_t deadline; int budget; found_elf *out; int found; } scan_ctx;

static void scan_dir(const char *dir, int depth, const char *source, scan_ctx *sc) {
  if (sc->found || sc->budget <= 0 || util_now_ms() > sc->deadline) return;
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  char *sub = NULL;   /* sub-folders are visited after the files of this one */
  size_t nsub = 0, subcap = 0;
  while (!sc->found && (e = readdir(d)) != NULL) {
    if (--sc->budget <= 0 || util_now_ms() > sc->deadline) break;
    if (e->d_name[0] == '.') continue;
    char p[700];
    if (xsnprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p - 1) continue;
    struct stat sb;
    if (lstat(p, &sb) != 0) continue;
    if (S_ISREG(sb.st_mode) && garlic_elf_name(e->d_name)) {
      int64_t sz = 0;
      if (candidate_ok(p, &sz)) {
        xstrlcpy(sc->out->path, p, sizeof sc->out->path);
        xstrlcpy(sc->out->source, source, sizeof sc->out->source);
        sc->out->size = sz;
        sc->found = 1;
      }
    } else if (S_ISDIR(sb.st_mode) && depth > 0) {
      size_t need = strlen(e->d_name) + 1;
      if (nsub + need > subcap) {
        size_t nc = subcap ? subcap * 2 : 1024;
        while (nc < nsub + need) nc *= 2;
        char *nb = realloc(sub, nc);
        if (!nb) continue;
        sub = nb;
        subcap = nc;
      }
      memcpy(sub + nsub, e->d_name, need);
      nsub += need;
    }
  }
  closedir(d);
  for (size_t o = 0; o < nsub && !sc->found; o += strlen(sub + o) + 1) {
    char p[700];
    if (xsnprintf(p, sizeof p, "%s/%s", dir, sub + o) >= (int)sizeof p - 1) continue;
    scan_dir(p, depth - 1, source, sc);
  }
  free(sub);
}

/* The store's own companion copy first (its version is known), then the
 * places users keep payloads; bounded in depth, entries and time. */
static int find_elf_uncached(found_elf *f) {
  memset(f, 0, sizeof *f);
  char id[48] = CX_GARLIC_COMPANION_ID;
  cx_companions_by_kind("saves", id, sizeof id, NULL, 0, NULL);
  char p[700];
  xsnprintf(p, sizeof p, "%s/payloads/%s.elf", cx_data_dir(), id);
  if (candidate_ok(p, &f->size)) { xstrlcpy(f->path, p, sizeof f->path); xstrlcpy(f->source, "store", sizeof f->source); return 0; }
  scan_ctx sc = { util_now_ms() + GARLIC_FIND_BUDGET_MS, GARLIC_FIND_BUDGET_ENTRIES, f, 0 };
  xsnprintf(p, sizeof p, "%s/payloads", cx_data_dir());
  scan_dir(p, 0, "store", &sc);
  static const struct { const char *dir; int depth; } CONSOLE[] = {
    {"/data", 0}, {"/data/payloads", 1}, {"/data/homebrew", 3}, {"/data/etaHEN", 3},
  };
  for (size_t i = 0; i < XS_ARRAY_LEN(CONSOLE) && !sc.found; i++) {
    cx_console_path(CONSOLE[i].dir, p, sizeof p);
    scan_dir(p, CONSOLE[i].depth, "console", &sc);
  }
  for (int u = 0; u < 8 && !sc.found; u++) {
    static const char *const SUB[] = {"homebrew", "payloads"};
    for (size_t k = 0; k < XS_ARRAY_LEN(SUB) && !sc.found; k++) {
      char cp[64];
      xsnprintf(cp, sizeof cp, "/mnt/usb%d/%s", u, SUB[k]);
      cx_console_path(cp, p, sizeof p);
      scan_dir(p, 3, "usb", &sc);
    }
  }
  for (int u = 0; u < 2 && !sc.found; u++) {
    char cp[64];
    xsnprintf(cp, sizeof cp, "/mnt/ext%d/homebrew", u);
    cx_console_path(cp, p, sizeof p);
    scan_dir(p, 3, "ext", &sc);
  }
  return sc.found ? 0 : -1;
}

static int find_elf(found_elf *f, int fresh) {
  pthread_mutex_lock(&find_mu);
  int64_t now = util_now_ms();
  if (!fresh && found_cache.at_ms && now - found_cache.at_ms < GARLIC_FIND_TTL_MS && (!found_cache.valid || candidate_ok(found_cache.f.path, NULL))) {
    int rc = found_cache.valid ? 0 : -1;
    if (rc == 0) *f = found_cache.f;
    pthread_mutex_unlock(&find_mu);
    return rc;
  }
  int rc = find_elf_uncached(f);
  found_cache.valid = rc == 0;
  if (rc == 0) found_cache.f = *f;
  found_cache.at_ms = util_now_ms();
  pthread_mutex_unlock(&find_mu);
  if (rc == 0) LOGD("garlic: ELF candidate %s (%s)", f->path, f->source);
  return rc;
}

/* Compiled-in download of the pinned release; off on the host build. */
static const char *default_url(void) { return cx_is_host() ? "" : CX_GARLIC_ELF_URL; }

static int can_download(void) {
  if (!xhttp_available()) return 0;
  int unavailable = 0;
  if (cx_companions_by_kind("saves", NULL, 0, NULL, 0, &unavailable) == 0) return !unavailable;
  return default_url()[0] != 0;
}

/* ---- ensure / start ----------------------------------------------------- */

static int ensure_locked(char *err, size_t errcap) {
  int port = cx_garlic_port();
  int s = garlic_probe(1);
  if (s == GP_RUNNING) return 1;
  /* never a second copy while anything listens: a starting Garlic unmounts
   * /data/mount_sd before it finds its port taken (main.c:5486-5491) */
  if (s == GP_BUSY) { xsnprintf(err, errcap, "Garlic Save Manager is busy (port %d is open but does not answer) — wait for it to finish and try again", port); return 0; }
  if (s == GP_OTHER) { xsnprintf(err, errcap, "Port %d is used by a program that is not Garlic Save Manager", port); return 0; }
  if (!cx_companions_loader_available()) {
    xsnprintf(err, errcap, "Garlic Save Manager is not running and the ELF loader is not answering on 127.0.0.1:%d — start etaHEN or elfldr, then try again",
              cx_local_port("LOADER", 9021));
    return 0;
  }
  found_elf f;
  memset(&f, 0, sizeof f);
  cx_companion_file cf;
  char e2[300] = "";
  /* the pinned release first: a garlic*.elf the user keeps may predate the
   * import API (1.6); a scanned copy is only used when the pinned one cannot
   * be had (offline), and never from USB or an external drive */
  if (cx_companions_fetch("saves", CX_GARLIC_COMPANION_ID, default_url(), CX_GARLIC_ELF_SHA256, CX_GARLIC_ELF_VERSION, &cf, e2, sizeof e2) == 0) {
    xstrlcpy(f.path, cf.path, sizeof f.path);
    xstrlcpy(f.source, cf.downloaded ? "download" : "store", sizeof f.source);
    f.size = util_file_size(f.path);
    pthread_mutex_lock(&find_mu);
    found_cache.valid = 1;
    found_cache.f = f;
    found_cache.at_ms = util_now_ms();
    pthread_mutex_unlock(&find_mu);
  } else if (find_elf(&f, 1) != 0 || strcmp(f.source, "usb") == 0 || strcmp(f.source, "ext") == 0) {
    xsnprintf(err, errcap, "Garlic Save Manager could not be downloaded (%s) and no copy was found on the console's internal storage", e2);
    return 0;
  } else LOGW("garlic: pinned %s not available (%s): starting %s, whose version is unknown", CX_GARLIC_ELF_VERSION, e2, f.path);
  if (cx_tcp_probe(port, 300)) { xsnprintf(err, errcap, "Port %d opened while Garlic Save Manager was being prepared — not starting a second copy", port); return 0; }
  char code[32], e3[256] = "";
  int64_t bytes = 0;
  if (cx_companions_send_elf(f.path, &bytes, code, sizeof code, e3, sizeof e3) != 0) { xsnprintf(err, errcap, "%s", e3); return 0; }
  xstrlcpy(last_start.path, f.path, sizeof last_start.path);
  xstrlcpy(last_start.source, f.source, sizeof last_start.source);
  last_start.at = util_now();
  LOGI("garlic: sent %s (%s, %lld bytes) to the ELF loader, waiting for port %d", f.path, f.source, (long long)bytes, port);
  int64_t until = util_now_ms() + GARLIC_START_WAIT_MS;
  while (util_now_ms() < until) {
    util_sleep_ms(500);
    if (garlic_probe(1) == GP_RUNNING) { LOGI("garlic: Garlic Save Manager answers on port %d", port); return 1; }
  }
  xsnprintf(err, errcap, "Garlic Save Manager was sent to the ELF loader but did not answer on port %d within %d s", port, GARLIC_START_WAIT_MS / 1000);
  return 0;
}

int cx_garlic_ensure(int start_if_missing, char *err, size_t errcap) {
  if (err && errcap) err[0] = 0;
  int s = garlic_probe(1);
  if (s == GP_RUNNING) {
    if (api_ok(err, errcap)) return 1;
    LOGW("garlic: not usable: %s", err);
    return 0;
  }
  if (!start_if_missing) {
    if (s == GP_BUSY) xsnprintf(err, errcap, "Garlic Save Manager is busy (port %d is open but does not answer)", cx_garlic_port());
    else if (s == GP_OTHER) xsnprintf(err, errcap, "Port %d is used by a program that is not Garlic Save Manager", cx_garlic_port());
    else xsnprintf(err, errcap, "Garlic Save Manager is not running on this console (port %d)", cx_garlic_port());
    return 0;
  }
  pthread_mutex_lock(&ensure_mu);
  int rc = ensure_locked(err, errcap);
  if (rc) rc = api_ok(err, errcap);
  pthread_mutex_unlock(&ensure_mu);
  if (!rc) LOGW("garlic: not available: %s", err);
  return rc;
}

void cx_garlic_status_json(jw *w) {
  int s = garlic_probe(0);
  int running = s == GP_RUNNING || (s == GP_BUSY && busy_but_known());
  int loader = running ? 1 : cx_companions_loader_available();
  found_elf f;
  int have_elf = !running && find_elf(&f, 0) == 0;
  /* a copy on USB or an external drive is never started (its version is unknown) */
  if (have_elf && (strcmp(f.source, "usb") == 0 || strcmp(f.source, "ext") == 0)) have_elf = 0;
  int store_copy = have_elf && strcmp(f.source, "store") == 0;
  /* the pinned release goes before a copy found on the console (ensure_locked) */
  int dl = !running && !store_copy && can_download();
  int can_start = !running && s == GP_DOWN && loader && (have_elf || dl);
  int starting = cx_jobs_active("garlic-start") > 0;
  char id[48] = "", version[32] = "";
  cx_companions_by_kind("saves", id, sizeof id, version, sizeof version, NULL);
  jw_key(w, "garlic");
  jw_obj_begin(w);
  jw_kv_bool(w, "available", running || can_start);
  jw_kv_bool(w, "running", running);
  jw_kv_bool(w, "busy", s == GP_BUSY);
  jw_kv_int(w, "port", cx_garlic_port());
  jw_kv_bool(w, "canStart", can_start);
  jw_kv_bool(w, "starting", starting);
  jw_kv_bool(w, "loaderAvailable", loader);
  if (running) jw_kv_bool(w, "saveOpen", probe.mounted);
  if (have_elf && !dl) {
    jw_kv_str(w, "elfPath", f.path);
    jw_kv_str(w, "source", f.source);
    if (store_copy && version[0]) jw_kv_str(w, "version", version);
  } else if (running && last_start.path[0]) {
    jw_kv_str(w, "elfPath", last_start.path);
    jw_kv_str(w, "source", last_start.source);
    if (strcmp(last_start.source, "download") == 0) jw_kv_str(w, "version", version[0] ? version : CX_GARLIC_ELF_VERSION);
    else if (strcmp(last_start.source, "store") == 0 && version[0]) jw_kv_str(w, "version", version);
  } else if (dl) {
    jw_kv_str(w, "source", "download");
    jw_kv_str(w, "version", version[0] ? version : CX_GARLIC_ELF_VERSION);
  }
  char detail[300];
  if (running) xsnprintf(detail, sizeof detail, "Garlic Save Manager is running on port %d%s.", cx_garlic_port(), probe.mounted ? " (a save is open in its page)" : "");
  else if (s == GP_BUSY) xsnprintf(detail, sizeof detail, "Port %d is open but Garlic Save Manager does not answer (busy?).", cx_garlic_port());
  else if (s == GP_OTHER) xsnprintf(detail, sizeof detail, "Port %d is used by a program that is not Garlic Save Manager.", cx_garlic_port());
  else if (starting) xstrlcpy(detail, "Starting Garlic Save Manager…", sizeof detail);
  else if (can_start && have_elf && !dl) xsnprintf(detail, sizeof detail, "Not running; it starts on demand from %s.", f.path);
  else if (can_start) xstrlcpy(detail, "Not running; it is downloaded from its author's GitHub release (sha256 checked) and started on demand.", sizeof detail);
  else if (!loader) xsnprintf(detail, sizeof detail, "Not running, and the ELF loader is not answering on port %d to start it.", cx_local_port("LOADER", 9021));
  else xstrlcpy(detail, "Not running and not found on this console; refresh the catalog or copy garlic-savemgr.elf to /data/homebrew.", sizeof detail);
  jw_kv_str(w, "detail", detail);
  jw_obj_end(w);
}

/* ---- import ------------------------------------------------------------- */

typedef struct gfile { char *rel; int64_t size; } gfile;
typedef struct gfiles { gfile *v; int n, cap, dirs; int64_t bytes, blocks_bytes; int truncated; } gfiles;

static void gfiles_free(gfiles *g) {
  for (int i = 0; i < g->n; i++) free(g->v[i].rel);
  free(g->v);
  memset(g, 0, sizeof *g);
}

static int skip_upload(const char *rel) {
  if (strcmp(rel, "xeno-save.json") == 0 || strcmp(rel, "xeno-backup.json") == 0) return 1;
  if (util_strieq(rel, "sce_sys/sealedkey") || util_strieq(rel, "sce_sys/keystone")) return 1;
  return zip_name_is_junk(rel);
}

static int collect(const char *root, const char *rel, gfiles *g, int depth) {
  if (depth > 16) return 0;
  char dir[1100];
  if (rel[0]) xsnprintf(dir, sizeof dir, "%s/%s", root, rel); else xstrlcpy(dir, root, sizeof dir);
  DIR *d = opendir(dir);
  if (!d) return -1;
  struct dirent *e;
  int rc = 0;
  while (rc == 0 && (e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char r[700], full[1800];
    if (rel[0]) { if (xsnprintf(r, sizeof r, "%s/%s", rel, e->d_name) >= (int)sizeof r - 1) continue; }
    else xstrlcpy(r, e->d_name, sizeof r);
    xsnprintf(full, sizeof full, "%s/%s", root, r);
    struct stat sb;
    if (lstat(full, &sb) != 0) continue;
    if (S_ISDIR(sb.st_mode)) {
      if (zip_name_is_junk(r)) continue;
      g->dirs++;
      rc = collect(root, r, g, depth + 1);
      continue;
    }
    if (!S_ISREG(sb.st_mode) || skip_upload(r)) continue;
    if (g->n >= GARLIC_MAX_FILES) { g->truncated = 1; rc = -1; break; }
    if (g->n == g->cap) {
      int nc = g->cap ? g->cap * 2 : 64;
      gfile *nv = realloc(g->v, (size_t)nc * sizeof *nv);
      if (!nv) { rc = -1; break; }
      g->v = nv;
      g->cap = nc;
    }
    g->v[g->n].rel = util_strdup(r);
    if (!g->v[g->n].rel) { rc = -1; break; }
    g->v[g->n].size = (int64_t)sb.st_size;
    g->n++;
    g->bytes += (int64_t)sb.st_size;
    g->blocks_bytes += ((int64_t)sb.st_size + GARLIC_BLOCK - 1) / GARLIC_BLOCK * GARLIC_BLOCK;
  }
  closedir(d);
  return rc;
}

static void jprog(cx_job *j, int64_t done, int64_t total, const char *msg) { if (j) cx_job_progress(j, done, total, msg); }

static int import_locked(const char *dir, const char *title_id, uint32_t user_id, const char *dir_name, int64_t total_bytes,
                         cx_job *j, cx_garlic_result *res, char *err, size_t errcap) {
  /* the user's own Garlic session must not be cut off: create_pfs
   * unmounts whatever is open (main.c:3314) */
  int mounted = 0;
  char mp[256] = "";
  int s = status_call(10000, &mounted, mp, sizeof mp);
  if (s != GP_RUNNING) { xsnprintf(err, errcap, "Garlic Save Manager is not answering on port %d", cx_garlic_port()); return -1; }
  if (mounted) { xsnprintf(err, errcap, "Garlic Save Manager has a save open (%s) — close it in Garlic's page and try again", mp[0] ? mp : "mounted"); return -1; }
  gfiles g;
  memset(&g, 0, sizeof g);
  if (collect(dir, "", &g, 0) != 0) {
    if (g.truncated) xsnprintf(err, errcap, "the save has more than %d files", GARLIC_MAX_FILES);
    else xsnprintf(err, errcap, "cannot read the save folder");
    gfiles_free(&g);
    return -1;
  }
  int has_sfo = 0;
  for (int i = 0; i < g.n; i++) if (strcmp(g.v[i].rel, "sce_sys/param.sfo") == 0) has_sfo = 1;
  if (!has_sfo) { gfiles_free(&g); xsnprintf(err, errcap, "the save has no sce_sys/param.sfo at its root"); return -1; }
  for (int i = 0; i < g.n; i++) {
    char enc[GARLIC_NAME_MAX + 1];
    if (enc_name(g.v[i].rel, enc, sizeof enc) < 0) { xsnprintf(err, errcap, "file name too long for Garlic Save Manager: %s", g.v[i].rel); gfiles_free(&g); return -1; }
  }
  int64_t total = total_bytes > g.bytes ? total_bytes : g.bytes;
  if (total <= 0) total = 1;
  /* every file and folder takes whole PFS blocks; Garlic adds its own 25% + 4 MiB */
  int64_t want = g.blocks_bytes + (int64_t)(g.dirs + 1) * GARLIC_BLOCK;
  if (want < total_bytes) want = total_bytes;
  char q[2048];
  xsnprintf(q, sizeof q, "/api/create_pfs?size=%lld", (long long)want);
  jprog(j, 0, total, "Garlic: creating the save image");
  LOGI("garlic: import %s (%d files, %lld bytes) for user %x: create_pfs size=%lld", title_id, g.n, (long long)g.bytes, (unsigned)user_id, (long long)want);
  jdoc *d = gcall("GET", q, NULL, 180000, 4096, 64, err, errcap);
  if (!d) { gunmount(); gfiles_free(&g); return -1; }
  int ok = gok(d, "the new save image", err, errcap);
  json_free(d);
  if (!ok) { gunmount(); gfiles_free(&g); return -1; }
  int64_t done = 0;
  for (int i = 0; i < g.n; i++) {
    if (j && cx_job_cancelled(j)) { xsnprintf(err, errcap, "cancelled"); gunmount(); gfiles_free(&g); return -1; }
    char enc[GARLIC_NAME_MAX + 1], full[1800], msg[200];
    enc_name(g.v[i].rel, enc, sizeof enc);
    xsnprintf(full, sizeof full, "%s/%s", dir, g.v[i].rel);
    xsnprintf(q, sizeof q, "/api/upload_file?name=%s", enc);
    xsnprintf(msg, sizeof msg, "Garlic: uploading %s (%d/%d)", util_basename(g.v[i].rel), i + 1, g.n);
    jprog(j, done, total, msg);
    d = gcall("POST", q, full, 120000, 4096, 64, err, errcap);
    if (!d) { gunmount(); gfiles_free(&g); return -1; }
    ok = gok(d, g.v[i].rel, err, errcap);
    int64_t sz = jv_get_i64(json_root(d), "size", -1);
    json_free(d);
    if (ok && sz >= 0 && sz != g.v[i].size) { ok = 0; xsnprintf(err, errcap, "Garlic Save Manager stored %lld of %lld bytes of %s", (long long)sz, (long long)g.v[i].size, g.v[i].rel); }
    if (!ok) { gunmount(); gfiles_free(&g); return -1; }
    done += g.v[i].size;
  }
  int nfiles = g.n;
  int64_t nbytes = g.bytes;
  gfiles_free(&g);
  char uidq[24] = "";
  if (user_id) xsnprintf(uidq, sizeof uidq, "uid=%x", (unsigned)user_id);
  /* import_check reads the uploaded param.sfo the way import_finish will */
  xsnprintf(q, sizeof q, "/api/import_check%s%s", uidq[0] ? "?" : "", uidq);
  jprog(j, done, total, "Garlic: checking the save");
  d = gcall("GET", q, NULL, 30000, 8192, 256, err, errcap);
  if (!d) { gunmount(); return -1; }
  if (gerr(d, err, errcap)) { json_free(d); gunmount(); return -1; }
  const char *ctid = jv_get_str(json_root(d), "title_id", "");
  if (ctid[0] && strcmp(ctid, title_id) != 0) {
    xsnprintf(err, errcap, "this save's param.sfo is for %s, not %s", ctid, title_id);
    json_free(d);
    gunmount();
    return -1;
  }
  res->existed = jv_get_bool(json_root(d), "exists", 0);
  json_free(d);
  if (res->existed) LOGI("garlic: %s already has this save for user %x: it is replaced (the store took a backup first when enabled)", title_id, (unsigned)user_id);
  jprog(j, done, total, "Garlic: re-signing and installing");
  xsnprintf(q, sizeof q, "/api/import_finish%s%s", uidq[0] ? "?" : "", uidq);
  d = gcall("GET", q, NULL, 300000, 8192, 256, err, errcap);
  if (!d) { gunmount(); return -1; }
  if (jv_get_bool(json_root(d), "sfo_zeroed", 0)) {
    json_free(d);
    if (!cx_titleid_valid(title_id) || !cx_dirname_valid(dir_name)) { xsnprintf(err, errcap, "the save's param.sfo is zeroed and its folder name is unknown"); gunmount(); return -1; }
    LOGW("garlic: %s param.sfo is zeroed: import_finish again with force=1 (%s/%s)", title_id, title_id, dir_name);
    xsnprintf(q, sizeof q, "/api/import_finish?%s%sforce=1&title_id=%s&dir_name=%s", uidq, uidq[0] ? "&" : "", title_id, dir_name);
    d = gcall("GET", q, NULL, 300000, 8192, 256, err, errcap);
    if (!d) { gunmount(); return -1; }
    res->forced = 1;
  }
  ok = gok(d, "the import", err, errcap);
  if (ok) {
    jv r = json_root(d);
    xstrlcpy(res->titleId, jv_get_str(r, "title_id", title_id), sizeof res->titleId);
    xstrlcpy(res->dirName, jv_get_str(r, "dir_name", dir_name ? dir_name : ""), sizeof res->dirName);
    xstrlcpy(res->mainTitle, jv_get_str(r, "main_title", ""), sizeof res->mainTitle);
    xstrlcpy(res->warning, jv_get_str(r, "warning", ""), sizeof res->warning);
    if (jv_get_bool(r, "exists", 0)) res->existed = 1;
  }
  json_free(d);
  if (!ok) { gunmount(); return -1; }
  res->files = nfiles;
  res->bytes = nbytes;
  if (res->warning[0]) LOGW("garlic: import warning: %s", res->warning);
  LOGI("garlic: imported %s/%s for user %x (%d files, %s)", res->titleId, res->dirName, (unsigned)user_id, nfiles, res->existed ? "replaced" : "new");
  jprog(j, total, total, "Garlic: save installed");
  return 0;
}

int cx_garlic_import_dir_ex(const char *dir, const char *title_id, uint32_t user_id, const char *dir_name, int64_t total_bytes,
                            cx_job *j, cx_garlic_result *res, char *err, size_t errcap) {
  cx_garlic_result tmp;
  if (!res) res = &tmp;
  memset(res, 0, sizeof *res);
  if (!dir || !util_dir_exists(dir)) { xsnprintf(err, errcap, "save folder not found"); return -1; }
  if (!cx_titleid_valid(title_id)) { xsnprintf(err, errcap, "invalid title id"); return -1; }
  char sfo[1200];
  xsnprintf(sfo, sizeof sfo, "%s/sce_sys/param.sfo", dir);
  if (!util_file_exists(sfo)) { xsnprintf(err, errcap, "the save has no sce_sys/param.sfo at its root"); return -1; }
  /* the decrypted import formats a PS5 image (create_pfs without ps4=1, as
   * Garlic's own import page does): a PS4 save would land unusable */
  char fmt[8] = "";
  size_t sl = 0;
  char *sb = util_read_file(sfo, &sl, 1 << 20);
  if (sb) { cx_sfo_get_str((const uint8_t *)sb, sl, "FORMAT", fmt, sizeof fmt); free(sb); }
  if (util_starts_with(title_id, "CUSA") || strcmp(fmt, "obs") == 0) {
    xsnprintf(err, errcap, "Garlic Save Manager's import builds PS5 saves only; this is a PS4 save");
    return -1;
  }
  pthread_mutex_lock(&op_mu);
  if (garlic_probe(1) != GP_RUNNING) {
    pthread_mutex_unlock(&op_mu);
    xsnprintf(err, errcap, "Garlic Save Manager is not running on port %d", cx_garlic_port());
    return -1;
  }
  op_active = 1;
  int rc = import_locked(dir, title_id, user_id, dir_name ? dir_name : "", total_bytes, j, res, err, errcap);
  op_active = 0;
  pthread_mutex_unlock(&op_mu);
  if (rc != 0) LOGW("garlic: import of %s failed: %s", title_id, err);
  return rc;
}

int cx_garlic_import_dir(const char *dir, const char *title_id, uint32_t user_id, const char *dir_name, int64_t total_bytes,
                         cx_job *j, char *err, size_t errcap) {
  return cx_garlic_import_dir_ex(dir, title_id, user_id, dir_name, total_bytes, j, NULL, err, errcap);
}

/* ---- export ------------------------------------------------------------- */

static void stage_path(const char *tag, char *out, size_t cap) {
  char rnd[12];
  util_rand_hex(rnd, 8);
  xsnprintf(out, cap, "%s/%s/garlic-%s-%s", cx_data_dir(), cx_is_host() ? "sim" : "stage", tag, rnd);
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

/* Garlic's mount + download_file path for one exact save (user given):
 * GET /api/saves → index, /api/mount?idx=N → file list, /api/download_file
 * per file (streamed), /api/unmount. */
static int export_via_mount(const char *tid, const char *dn, uint32_t uid, const char *out_dir, cx_job *j, char *err, size_t errcap) {
  if (!uid) { xsnprintf(err, errcap, "several profiles hold %s/%s: pick the profile to export", tid, dn); return -1; }
  jdoc *d = gcall("GET", "/api/saves", NULL, 120000, 8u << 20, 200000, err, errcap);
  if (!d) return -1;
  if (gerr(d, err, errcap)) { json_free(d); return -1; }
  char sname[48], uhex[16];
  xsnprintf(sname, sizeof sname, "sdimg_%s", dn);
  xsnprintf(uhex, sizeof uhex, "%x", (unsigned)uid);
  int idx = -1, i = 0;
  JV_FOREACH(it, jv_get(json_root(d), "saves")) {
    const char *u = jv_get_str(it, "uid", "");
    if (idx < 0 && !jv_get_bool(it, "usb", 0) && strcmp(jv_get_str(it, "title_id", ""), tid) == 0 &&
        (strcmp(jv_get_str(it, "save_name", ""), sname) == 0 || strcmp(jv_get_str(it, "save_name", ""), dn) == 0) &&
        u[0] && strtoul(u, NULL, 16) == uid) idx = i;
    i++;
  }
  json_free(d);
  if (idx < 0) { xsnprintf(err, errcap, "Garlic Save Manager does not list %s/%s for this profile", tid, dn); return -1; }
  char q[2048];
  xsnprintf(q, sizeof q, "/api/mount?idx=%d", idx);
  d = gcall("GET", q, NULL, 120000, 8u << 20, 100000, err, errcap);
  if (!d) { gunmount(); return -1; }
  if (gerr(d, err, errcap)) { json_free(d); gunmount(); return -1; }
  if (jv_get_bool(json_root(d), "sfo_zeroed", 0)) { json_free(d); gunmount(); xsnprintf(err, errcap, "this save's param.sfo is zeroed: it cannot be shared"); return -1; }
  int rc = 0, files = 0;
  JV_FOREACH(it, jv_get(json_root(d), "files")) {
    if (rc != 0) break;
    if (jv_get_bool(it, "dir", 0)) continue;
    const char *rel = jv_get_str(it, "name", "");
    if (!zip_name_is_safe(rel) || strlen(rel) > 600) { LOGW("garlic: skipping unsafe name %s", rel); continue; }
    char enc[GARLIC_NAME_MAX + 1], dest[1800];
    if (enc_name(rel, enc, sizeof enc) < 0) { xsnprintf(err, errcap, "file name too long: %s", rel); rc = -1; break; }
    xsnprintf(dest, sizeof dest, "%s/%s", out_dir, rel);
    char parent[1800];
    xstrlcpy(parent, dest, sizeof parent);
    char *sl = strrchr(parent, '/');
    if (sl) { *sl = 0; util_mkdir_p(parent); }
    xsnprintf(q, sizeof q, "/api/download_file?name=%s", enc);
    char ct[64];
    int64_t got = 0;
    if (gstream(q, dest, GARLIC_EXPORT_MAX, 120000, j, &got, ct, sizeof ct, err, errcap) != 0) { rc = -1; break; }
    if (util_istarts_with(ct, "application/json")) {
      size_t l = 0;
      char *t = util_read_file(dest, &l, 4096);
      jdoc *e = t ? json_parse(t, l, 64, 4) : NULL;
      if (!e || !gerr(e, err, errcap)) xsnprintf(err, errcap, "Garlic Save Manager did not send %s", rel);
      if (e) json_free(e);
      free(t);
      rc = -1;
      break;
    }
    files++;
  }
  json_free(d);
  gunmount();
  if (rc == 0 && files == 0) { xsnprintf(err, errcap, "Garlic Save Manager listed no files for %s/%s", tid, dn); rc = -1; }
  return rc;
}

static int export_locked(const char *tid, const char *dn, uint32_t uid, const char *out_dir, cx_job *j, char *err, size_t errcap) {
  int mounted = 0;
  char mp[256] = "";
  if (status_call(10000, &mounted, mp, sizeof mp) != GP_RUNNING) { xsnprintf(err, errcap, "Garlic Save Manager is not answering on port %d", cx_garlic_port()); return -1; }
  /* /api/mount and export_title_dec unmount whatever is open (main.c:2089, 4993) */
  if (mounted) { xsnprintf(err, errcap, "Garlic Save Manager has a save open (%s) — close it in Garlic's page and try again", mp[0] ? mp : "mounted"); return -1; }
  if (uid) {
    /* export_title_dec zips every profile's (and USB) copy of the title and
     * silently skips images it cannot mount, and it mounts every save of the
     * title: only the mount route reads exactly this profile's own save */
    cx_rm_rf(out_dir, 8);
    if (util_mkdir_p(out_dir) != 0) { xsnprintf(err, errcap, "cannot create %s", out_dir); return -1; }
    return export_via_mount(tid, dn, uid, out_dir, j, err, errcap);
  }
  char tmpzip[1100], xdir[1100], q[256];
  stage_path("export", xdir, sizeof xdir);
  xsnprintf(tmpzip, sizeof tmpzip, "%s.zip", xdir);
  util_mkdir_p(xdir);
  xsnprintf(q, sizeof q, "/api/export_title_dec?id=%s", tid);
  if (j) cx_job_progress(j, 0, 0, "Garlic: decrypting the save");
  char ct[64];
  int64_t got = 0;
  if (gstream(q, tmpzip, GARLIC_EXPORT_MAX, 600000, j, &got, ct, sizeof ct, err, errcap) != 0) { cx_rm_rf(xdir, 4); return -1; }
  if (util_istarts_with(ct, "application/json")) {
    size_t l = 0;
    char *t = util_read_file(tmpzip, &l, 64 << 10);
    jdoc *e = t ? json_parse(t, l, 256, 8) : NULL;
    if (!e || !gerr(e, err, errcap)) xsnprintf(err, errcap, "Garlic Save Manager did not send a zip");
    if (e) json_free(e);
    free(t);
    util_unlink_quiet(tmpzip);
    cx_rm_rf(xdir, 4);
    return -1;
  }
  zip_entry_info *list = calloc(GARLIC_MAX_FILES, sizeof *list);
  const char **names = calloc(GARLIC_MAX_FILES, sizeof *names);
  if (!list || !names) { free(list); free(names); util_unlink_quiet(tmpzip); cx_rm_rf(xdir, 4); xsnprintf(err, errcap, "out of memory"); return -1; }
  zip_stats st;
  int n = zip_list(tmpzip, list, GARLIC_MAX_FILES, &st);
  char pfx[2][96];
  xsnprintf(pfx[0], sizeof pfx[0], "%s/sdimg_%s/", tid, dn);
  xsnprintf(pfx[1], sizeof pfx[1], "%s/%s/", tid, dn);
  int which = -1, count = 0;
  for (int k = 0; k < 2 && which < 0; k++) {
    count = 0;
    for (int i = 0; i < n; i++) if (!list[i].is_dir && util_starts_with(list[i].name, pfx[k])) names[count++] = list[i].name;
    if (count) which = k;
  }
  int dup = 0;
  if (which >= 0) {
    qsort(names, (size_t)count, sizeof *names, cmp_names);
    for (int i = 1; i < count && !dup; i++) if (strcmp(names[i - 1], names[i]) == 0) dup = 1;
  }
  int rc = 0;
  if (n < 0) { xsnprintf(err, errcap, "Garlic's export is not a readable zip: %s", st.error); rc = -1; }
  else if (which < 0) { xsnprintf(err, errcap, "Garlic Save Manager exported no save %s for %s (it skips saves it cannot mount)", dn, tid); rc = -1; }
  else if (dup) {
    /* the same save of several profiles (or a USB copy): Garlic's zip cannot
     * tell them apart, its mount route can */
    LOGI("garlic: %s/%s exists more than once in Garlic's export, exporting the profile's own copy", tid, dn);
    util_unlink_quiet(tmpzip);
    cx_rm_rf(xdir, 4);
    free(list);
    free(names);
    cx_rm_rf(out_dir, 8);
    if (util_mkdir_p(out_dir) != 0) { xsnprintf(err, errcap, "cannot create %s", out_dir); return -1; }
    return export_via_mount(tid, dn, uid, out_dir, j, err, errcap);
  }
  free(list);
  free(names);
  if (rc == 0 && zip_extract_to_dir(tmpzip, xdir, (uint64_t)GARLIC_EXPORT_MAX, NULL, NULL, &st) != 0) { xsnprintf(err, errcap, "cannot unpack Garlic's export: %s", st.error); rc = -1; }
  util_unlink_quiet(tmpzip);
  if (rc == 0) {
    char src[1300];
    xsnprintf(src, sizeof src, "%s/%s", xdir, pfx[which]);
    src[strlen(src) - 1] = 0;   /* trailing '/' */
    cx_rm_rf(out_dir, 8);
    if (rename(src, out_dir) != 0) {
      int64_t b = 0;
      if (util_mkdir_p(out_dir) != 0 || cx_copy_tree(src, out_dir, 16, &b) != 0) { xsnprintf(err, errcap, "cannot move the exported save into place"); rc = -1; }
    }
  }
  cx_rm_rf(xdir, 16);
  return rc;
}

int cx_garlic_export_dec_dir(const char *title_id, const char *dir_name, uint32_t user_id, const char *out_dir,
                             cx_job *j, char *err, size_t errcap) {
  if (!cx_titleid_valid(title_id)) { xsnprintf(err, errcap, "invalid title id"); return -1; }
  if (!cx_dirname_valid(dir_name)) { xsnprintf(err, errcap, "invalid save directory name"); return -1; }
  if (!out_dir || !cx_path_is_clean(out_dir)) { xsnprintf(err, errcap, "invalid output folder"); return -1; }
  /* Garlic mounts a /data copy of each image it reads and writes it back over
   * the original on unmount: never under a running (or suspended) game */
  if (cx_title_running(title_id) == 1) { xsnprintf(err, errcap, "%s is running: close the game (not just suspend it) before its save is exported", title_id); return -1; }
  if (user_id) {
    /* Garlic exports every profile's saves of the title: only the user's own one is taken */
    char tdir[1024], img[1100];
    if (cx_save_title_dir(user_id, title_id, "", tdir, sizeof tdir, NULL, 0) != 0) { xsnprintf(err, errcap, "user home not found"); return -1; }
    xsnprintf(img, sizeof img, "%s/sdimg_%s", tdir, dir_name);
    int found = util_file_exists(img);
    if (!found) { xsnprintf(img, sizeof img, "%s/%s", tdir, dir_name); found = util_file_exists(img); }
    if (!found) { xsnprintf(err, errcap, "no save %s for %s on this profile", dir_name, title_id); return -1; }
  }
  pthread_mutex_lock(&op_mu);
  if (garlic_probe(1) != GP_RUNNING) {
    pthread_mutex_unlock(&op_mu);
    xsnprintf(err, errcap, "Garlic Save Manager is not running on port %d", cx_garlic_port());
    return -1;
  }
  op_active = 1;
  int rc = export_locked(title_id, dir_name, user_id, out_dir, j, err, errcap);
  op_active = 0;
  pthread_mutex_unlock(&op_mu);
  if (rc == 0) {
    char p[1200];
    xsnprintf(p, sizeof p, "%s/sce_sys/param.sfo", out_dir);
    if (!util_file_exists(p)) { xsnprintf(err, errcap, "Garlic's export has no sce_sys/param.sfo"); rc = -1; }
  }
  if (rc == 0) {
    char p[1200];
    xsnprintf(p, sizeof p, "%s/sce_sys/sealedkey", out_dir); util_unlink_quiet(p);
    xsnprintf(p, sizeof p, "%s/sce_sys/keystone", out_dir); util_unlink_quiet(p);
    xsnprintf(p, sizeof p, "%s/xeno-save.json", out_dir); util_unlink_quiet(p);
    xsnprintf(p, sizeof p, "%s/sce_sys/param.sfo", out_dir);
    size_t sl = 0;
    char *sfo = util_read_file(p, &sl, 1 << 20);
    if (sfo) {
      if (cx_sfo_set_account_id((uint8_t *)sfo, sl, 0) == 0 && util_write_file_atomic(p, sfo, sl) != 0) { xsnprintf(err, errcap, "cannot clear the account id"); rc = -1; }
      free(sfo);
    }
  }
  if (rc != 0) { LOGW("garlic: export of %s/%s failed: %s", title_id, dir_name, err); cx_rm_rf(out_dir, 16); }
  else LOGI("garlic: exported %s/%s (decrypted) for user %x", title_id, dir_name, (unsigned)user_id);
  return rc;
}

int cx_garlic_export_dec(const char *title_id, const char *dir_name, uint32_t user_id, const char *out_zip,
                         cx_job *j, char *err, size_t errcap) {
  if (!out_zip || !cx_path_is_clean(out_zip)) { xsnprintf(err, errcap, "invalid output path"); return -1; }
  char stage[1100];
  stage_path("save", stage, sizeof stage);
  if (cx_garlic_export_dec_dir(title_id, dir_name, user_id, stage, j, err, errcap) != 0) return -1;
  char parent[1100];
  xstrlcpy(parent, out_zip, sizeof parent);
  char *sl = strrchr(parent, '/');
  if (sl && sl != parent) { *sl = 0; util_mkdir_p(parent); }
  zip_stats st;
  int rc = zip_create_from_dir(out_zip, stage, NULL, NULL, &st);
  if (rc != 0) xsnprintf(err, errcap, "zip failed: %s", st.error);
  cx_rm_rf(stage, 16);
  return rc == 0 ? 0 : -1;
}
