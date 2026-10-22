/*
 * install.c — see install.h.
 *
 * Primary path: sceAppInstUtilInstallByPackage() with meta.uri = bare local
 * path (the etaHEN DPI v2 / ezremote-dpi recipe; installs full content).
 * Fallbacks: the DPI daemons on 127.0.0.1:9040 (XENO TOOL / ezremote) and
 * :12800 (etaHEN) using their one-line protocol. Progress is observed by
 * polling /user/app/<TITLEID> (plus sceAppInstUtilGetInstallStatus when the
 * symbol resolves) from the housekeeping thread.
 */
#include "install.h"
#include "cx_common.h"
#include "http_client.h"
#include "log.h"
#include "storage.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef XENO_HOST
#include <dlfcn.h>
#include <sys/sysctl.h>
/* ---- AppInstUtil ABI (copied from payload/dpi/sceAppInstUtil.h) ---------- */
#define PLAYGOSCENARIOID_SIZE 3
#define CONTENTID_SIZE 0x30
#define LANGUAGE_SIZE 8
#define SCE_NUM_LANGUAGES 30
#define SCE_NUM_IDS 64
typedef char playgo_scenario_id_t[PLAYGOSCENARIOID_SIZE];
typedef char language_t[LANGUAGE_SIZE];
typedef char content_id_t[CONTENTID_SIZE];
typedef struct {
  content_id_t content_id;
  int content_type;
  int content_platform;
} SceAppInstallPkgInfo;
typedef struct {
  const char *uri;
  const char *ex_uri;
  const char *playgo_scenario_id;
  const char *content_id;
  const char *content_name;
  const char *icon_url;
} MetaInfo;
typedef struct {
  language_t languages[SCE_NUM_LANGUAGES];
  playgo_scenario_id_t playgo_scenario_ids[SCE_NUM_IDS];
  content_id_t content_ids[SCE_NUM_IDS];
  unsigned char unknown[6480];
} PlayGoInfo;
extern int sceAppInstUtilInitialize(void);
extern int sceAppInstUtilInstallByPackage(MetaInfo *meta, SceAppInstallPkgInfo *pkg_info, PlayGoInfo *playgo);
/* Optional (resolved at runtime): status query — layout per psdevwiki. */
typedef struct {
  int status;        /* 0 none, 1 installing, 2 installed? (treated as hints only) */
  int error_code;
  int64_t total_size;
  int64_t downloaded_size;
  char pad[64];
} AppInstStatus;
typedef int (*get_install_status_fn)(const char *content_id, AppInstStatus *out);
static get_install_status_fn g_get_status;
static int g_get_status_probed;
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;
static int g_init_rc;

static void appinst_init_once(void) {
  g_init_rc = sceAppInstUtilInitialize();
  if (g_init_rc != 0) LOGW("install: sceAppInstUtilInitialize -> 0x%08x (continuing)", (unsigned)g_init_rc);
  g_get_status = (get_install_status_fn)dlsym(RTLD_DEFAULT, "sceAppInstUtilGetInstallStatus");
  if (!g_get_status) LOGI("install: sceAppInstUtilGetInstallStatus not available (polling /user/app only)");
  g_get_status_probed = 1;
}
#endif

/* ---- pkgsrv tokens ------------------------------------------------------ */

#define MAX_TOKENS 16
static pthread_mutex_t tok_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { char token[24]; char path[1024]; } tokens[MAX_TOKENS];

static void token_add(const char *token, const char *path) {
  pthread_mutex_lock(&tok_mu);
  int slot = -1;
  for (int i = 0; i < MAX_TOKENS; i++) if (!tokens[i].token[0]) { slot = i; break; }
  if (slot < 0) slot = 0;   /* recycle the oldest */
  xstrlcpy(tokens[slot].token, token, sizeof tokens[slot].token);
  xstrlcpy(tokens[slot].path, path, sizeof tokens[slot].path);
  pthread_mutex_unlock(&tok_mu);
}

void install_release_token(const char *token) {
  if (!token || !*token) return;
  pthread_mutex_lock(&tok_mu);
  for (int i = 0; i < MAX_TOKENS; i++) if (strcmp(tokens[i].token, token) == 0) tokens[i].token[0] = 0;
  pthread_mutex_unlock(&tok_mu);
}

int install_pkgsrv_lookup(const char *token, char *path, size_t cap) {
  int rc = -1;
  pthread_mutex_lock(&tok_mu);
  for (int i = 0; i < MAX_TOKENS; i++)
    if (tokens[i].token[0] && strcmp(tokens[i].token, token) == 0) { xstrlcpy(path, tokens[i].path, cap); rc = 0; break; }
  pthread_mutex_unlock(&tok_mu);
  return rc;
}

/* ---- PKG header --------------------------------------------------------- */

int install_read_pkg_header(const char *path, char *content_id, size_t cap, char *title_id, size_t tcap) {
  if (cap) content_id[0] = 0;
  if (tcap) title_id[0] = 0;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  unsigned char hdr[0x80];
  ssize_t n = read(fd, hdr, sizeof hdr);
  close(fd);
  if (n < 0x44) return -1;
  int cnt = memcmp(hdr, "\x7f" "CNT", 4) == 0;
  int fih = memcmp(hdr, "\x7f" "FIH", 4) == 0;   /* PS5 fake-signed package */
  if (!cnt && !fih) return -1;
  if (cnt && n >= 0x40 + 36) {
    char cid[40];
    memcpy(cid, hdr + 0x40, 36);
    cid[36] = 0;
    /* content ids are printable ASCII; anything else means an odd header */
    int printable = 1;
    for (int i = 0; i < 36 && cid[i]; i++) if ((unsigned char)cid[i] < 0x20 || (unsigned char)cid[i] > 0x7e) printable = 0;
    if (printable) {
      xstrlcpy(content_id, cid, cap);
      if (strlen(cid) >= 16) {
        char tid[10];
        memcpy(tid, cid + 7, 9);
        tid[9] = 0;
        char chk[16];
        if (util_find_titleid(tid, chk, sizeof chk) == 0) xstrlcpy(title_id, chk, tcap);
      }
    }
  }
  if (!title_id[0]) util_find_titleid(util_basename(path), title_id, tcap);
  return 0;
}

/* ---- DPI line protocol (console only; the host build simulates installs) -- */

#ifndef XENO_HOST
/* Connects to 127.0.0.1:port, sends "<line>\n" and reads a decimal rc.
 * Returns 0 when the daemon accepted (rc 0), -1 when unreachable (ENOENT) or
 * rejected (*rc_out set). */
static int dpi_send_line(int port, const char *line, int *rc_out, int timeout_ms) {
  *rc_out = -1;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  a.sin_addr.s_addr = htonl(0x7f000001u);
  struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
  char buf[1200];
  int n = xsnprintf(buf, sizeof buf, "%s\n", line);
  if (send(fd, buf, (size_t)n, 0) != n) { close(fd); return -1; }
  char resp[32];
  ssize_t r = recv(fd, resp, sizeof resp - 1, 0);
  close(fd);
  if (r <= 0) return -1;
  resp[r] = 0;
  util_trim(resp);
  *rc_out = atoi(resp);
  return *rc_out == 0 ? 0 : -1;
}
#endif

/* ---- start -------------------------------------------------------------- */

void install_start(const char *path, int port, install_result *out) {
  memset(out, 0, sizeof *out);
  if (install_read_pkg_header(path, out->contentId, sizeof out->contentId, out->titleId, sizeof out->titleId) != 0) {
    xsnprintf(out->message, sizeof out->message, "Not a PS4/PS5 package (bad header)");
    return;
  }
  util_rand_hex(out->token, 16);
  token_add(out->token, path);
#ifdef XENO_HOST
  (void)port;
  out->ok = 1;
  /* XENO_HOST_INSTALL_VIA: tests report another route (e.g. dpi12800) */
  const char *via = getenv("XENO_HOST_INSTALL_VIA");
  xstrlcpy(out->via, via && *via ? via : "host-sim", sizeof out->via);
  xsnprintf(out->message, sizeof out->message, "Installing (simulated) %s", out->contentId[0] ? out->contentId : util_basename(path));
  LOGI("install: simulated start for %s (content %s, title %s)", path, out->contentId, out->titleId);
  return;
#else
  pthread_once(&g_init_once, appinst_init_once);
  PlayGoInfo *playgo = calloc(1, sizeof *playgo);
  SceAppInstallPkgInfo pkg_info;
  MetaInfo meta;
  memset(&pkg_info, 0, sizeof pkg_info);
  memset(&meta, 0, sizeof meta);
  int rc = -1;
  if (playgo) {
    meta.uri = path;
    meta.ex_uri = "";
    meta.playgo_scenario_id = "";
    meta.content_id = "";
    meta.content_name = util_basename(path);
    meta.icon_url = "";
    rc = sceAppInstUtilInstallByPackage(&meta, &pkg_info, playgo);
    if (rc == 0 && pkg_info.content_id[0] && !out->contentId[0]) {
      xstrlcpy(out->contentId, pkg_info.content_id, sizeof out->contentId);
      if (strlen(out->contentId) >= 16 && !out->titleId[0]) { memcpy(out->titleId, out->contentId + 7, 9); out->titleId[9] = 0; }
    }
    free(playgo);
  }
  out->rc = rc;
  if (rc == 0) {
    out->ok = 1;
    xstrlcpy(out->via, "appinstutil", sizeof out->via);
    xsnprintf(out->message, sizeof out->message, "Installing %s", out->titleId[0] ? out->titleId : util_basename(path));
    LOGI("install: InstallByPackage accepted %s", path);
    return;
  }
  LOGW("install: InstallByPackage(%s) -> 0x%08x, trying DPI daemons", path, (unsigned)rc);
  int drc = 0;
  if (dpi_send_line(9040, path, &drc, 15000) == 0) {
    out->ok = 1;
    out->rc = 0;
    xstrlcpy(out->via, "dpi9040", sizeof out->via);
    xsnprintf(out->message, sizeof out->message, "Installing via DPI (:9040)");
    return;
  }
  if (drc != -1) LOGW("install: DPI :9040 rejected with rc %d", drc);
  char url[128];
  xsnprintf(url, sizeof url, "http://127.0.0.1:%d/pkgsrv/%s", port, out->token);
  if (dpi_send_line(12800, url, &drc, 15000) == 0) {
    out->ok = 1;
    out->rc = 0;
    xstrlcpy(out->via, "dpi12800", sizeof out->via);
    xsnprintf(out->message, sizeof out->message, "Installing via etaHEN DPI (:12800)");
    return;
  }
  if (drc != -1) LOGW("install: DPI :12800 rejected with rc %d", drc);
  xsnprintf(out->message, sizeof out->message,
            "Install failed (0x%08X). Install it from XENO TOOL → Install, or with etaHEN's DPI.", (unsigned)rc);
#endif
}

/* ---- poll --------------------------------------------------------------- */

int install_poll(const char *titleId, const char *contentId, time_t startedAt, char *msg, size_t cap) {
  time_t now = util_now();
  if (cap) msg[0] = 0;
#ifdef XENO_HOST
  (void)contentId;
  const char *dc = getenv("XENO_HOST_INSTALL_DIRCHECK");   /* tests: the console's /user/app check */
  if (dc && strcmp(dc, "1") == 0 && titleId && *titleId && storage_title_installed(titleId)) {
    xsnprintf(msg, cap, "Installed");
    return 1;
  }
  const char *secs = getenv("XENO_HOST_INSTALL_SECS");   /* tests: a longer simulated install */
  if (now - startedAt < (secs && *secs ? atoi(secs) : 3)) return 0;
  if (titleId && *titleId) {
    char dir[700];
    xsnprintf(dir, sizeof dir, "%s/%s", storage_user_app_dir(), titleId);
    util_mkdir_p(dir);
  }
  xsnprintf(msg, cap, "Installed (simulated)");
  return 1;
#else
  if (titleId && *titleId && storage_title_installed(titleId)) {
    xsnprintf(msg, cap, "Installed");
    return 1;
  }
  if (g_get_status_probed && g_get_status && contentId && *contentId) {
    AppInstStatus st;
    memset(&st, 0, sizeof st);
    int rc = g_get_status(contentId, &st);
    if (rc == 0 && st.error_code != 0) {
      xsnprintf(msg, cap, "Install failed (0x%08X)", (unsigned)st.error_code);
      return -1;
    }
  }
  if (now - startedAt > 30 * 60) {
    xsnprintf(msg, cap, "Install did not finish within 30 minutes. Check Notifications on the console.");
    return -1;
  }
  return 0;
#endif
}

int install_progress(const char *contentId, time_t startedAt, int64_t *total) {
  *total = 0;
#ifdef XENO_HOST
  (void)contentId;
  const char *e = getenv("XENO_HOST_INSTALL_STATUS");
  if (e && strcmp(e, "none") == 0) return INSTALL_NO_STATUS;
  if (e && strcmp(e, "error") == 0) return INSTALL_ERROR;
  *total = 1LL << 30;
  if (e && strcmp(e, "done") == 0) return INSTALL_DONE;
  const char *secs = getenv("XENO_HOST_INSTALL_SECS");
  return util_now() - startedAt <= (secs && *secs ? atoi(secs) : 3) ? INSTALL_RUNNING : INSTALL_DONE;
#else
  (void)startedAt;
  if (!g_get_status_probed || !g_get_status || !contentId || !*contentId) return INSTALL_NO_STATUS;
  AppInstStatus st;
  memset(&st, 0, sizeof st);
  if (g_get_status(contentId, &st) != 0) return INSTALL_UNKNOWN;
  if (st.error_code != 0) return INSTALL_ERROR;
  if (st.total_size <= 0 || st.downloaded_size < 0) return INSTALL_UNKNOWN;
  *total = st.total_size;
  return st.downloaded_size >= st.total_size ? INSTALL_DONE : INSTALL_RUNNING;
#endif
}

/* ---- ShadowMount -------------------------------------------------------- */

#define SM_API_PORT 10101

#ifndef XENO_HOST
/* ShadowMountPlus names its main thread "shadowmountplus.elf" and finds a
 * running copy of itself by that name in kern.proc.proc (its main.c
 * find_pid_by_name); 447 is the name offset it and the SDK samples read. */
#define SM_THREAD_NAME "shadowmountplus.elf"
#define KINFO_TDNAME_OFFSET 447
#endif

int install_shadowmount_running(void) {
#ifdef XENO_HOST
  const char *e = getenv("XENO_HOST_SHADOWMOUNT");   /* tests: "running" / "stopped" */
  if (!e || !*e) return -1;
  return strcmp(e, "running") == 0;
#else
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || size == 0) return -1;
  size += size / 4;   /* processes started between the two calls */
  unsigned char *buf = malloc(size);
  if (!buf) return -1;
  if (sysctl(mib, 4, buf, &size, NULL, 0) != 0) { free(buf); return -1; }
  int found = 0, syscore = 0;
  for (size_t off = 0; off + sizeof(int) <= size;) {
    int ks;
    memcpy(&ks, buf + off, sizeof ks);
    if (ks <= KINFO_TDNAME_OFFSET || (size_t)ks > size - off) break;
    const char *name = (const char *)buf + off + KINFO_TDNAME_OFFSET;
    size_t ncap = (size_t)ks - KINFO_TDNAME_OFFSET;
    if (strnlen(name, ncap) < ncap) {
      if (strcmp(name, SM_THREAD_NAME) == 0) found = 1;
      else if (strcmp(name, "SceSysCore.elf") == 0) syscore = 1;
    }
    off += (size_t)ks;
  }
  free(buf);
  /* a list without SceSysCore is not the whole process table */
  return found ? 1 : syscore ? 0 : -1;
#endif
}

int install_shadowmount_scan(char *status, size_t cap) {
  /* a plain connect first: nothing listens before 1.7 (or when it is not
   * running), and that answer must not take an HTTP transport slot */
  if (cx_tcp_probe(SM_API_PORT, 400)) {
    char url[64];
    xsnprintf(url, sizeof url, "http://127.0.0.1:%d/api/v1/scan", SM_API_PORT);
    xhttp_req r;
    memset(&r, 0, sizeof r);
    r.method = "POST";
    r.url = url;
    r.range_start = r.range_end = -1;
    r.connect_timeout_ms = 2000;
    r.recv_timeout_ms = 5000;
    r.body = "{}";
    r.body_len = 2;
    r.extra_headers = "Content-Type: application/json\r\n";
    xhttp_resp resp;
    size_t len;
    char *body = xhttp_fetch(&r, &resp, 64 << 10, &len);
    if (body) {
      char snippet[96];
      xstrlcpy(snippet, body, sizeof snippet);
      util_trim(snippet);
      xsnprintf(status, cap, "HTTP %d %s", resp.status, snippet);
      free(body);
      return 1;
    }
  }
  int run = install_shadowmount_running();
  if (run == 1) xsnprintf(status, cap, "ShadowMountPlus is running (no scan API before 1.7); it picks new files up on its own within about a minute");
  else if (run == 0) xsnprintf(status, cap, "ShadowMountPlus is not running");
  else xsnprintf(status, cap, "ShadowMountPlus API not answering on :%d; if it runs, it picks new files up on its own within about a minute", SM_API_PORT);
  return run == 1;
}
