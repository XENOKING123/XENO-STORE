/*
 * http_sce.c — PS5 transport (SPEC §4.2, §17.19): libSceNet / libSceSsl /
 * libSceHttp resolved at runtime with dlopen/dlsym so the payload never
 * carries a DT_NEEDED on libraries that may be absent or renamed on a
 * firmware.
 *
 * Two backends share one code path through a function table:
 *   - "sceHttp"  (primary, §17.19 — what shipped PS5 payloads use):
 *     sceNetInit → sceNetPoolCreate → sceSslInit → sceHttpInit(net, ssl, 8 MiB)
 *     then per request: sceHttpCreateTemplate(ctx, ua, 2 = HTTP/1.1, 1),
 *     sceHttpSetResponseHeaderMaxSize(tmpl, 0x2000), timeouts, auto-redirect off,
 *     sceHttpCreateConnectionWithURL(tmpl, url, 0), sceHttpCreateRequestWithURL,
 *     sceHttpAddRequestHeader, sceHttpSendRequest, sceHttpGetStatusCode,
 *     sceHttpGetAllResponseHeaders, sceHttpGetResponseContentLength,
 *     sceHttpReadData, sceHttpAbortRequest.
 *   - "sceHttp2" (fallback when the v1 library is missing): the Http2 API,
 *     which needs no connection object between template and request.
 * Every optional symbol (timeouts, auto-redirect, abort, SSL callback,
 * content length) is looked up by name, logged once when missing and skipped.
 *
 * Templates are never shared: every xhttp_open() (= every segment thread
 * request) creates its own template/connection/request triple and deletes it
 * in xhttp_close(), so contexts never see two requests in flight.
 *
 * Request slots: the net/SSL/HTTP pools are sized for SCE_HTTP_MAX_INFLIGHT
 * requests and xhttp_open() takes one of that many slots (transfers wait until
 * their cancel flag is raised, other requests a bounded time) until
 * xhttp_close(), so a full download queue cannot run the pools dry and turn
 * every create/send into an out-of-memory failure.
 *
 * TLS: the accept-any certificate callback is installed only when the caller
 * asked for insecure TLS (settings.downloads.allowInsecureTls) or, once per
 * request, as a logged automatic retry after the library rejected the
 * certificate (expired/unknown roots on older firmware). Out-of-memory and
 * other TLS-range codes never trigger it.
 *
 * Redirects: sceHttpSetAutoRedirect is turned off, so every 301/302/303/307/
 * 308 comes back to the manual loop in xhttp_open(): it re-sends the Range
 * header on the new location, re-checks the URL policy per hop and leaves the
 * real final URL in resp->location. Without the symbol the library follows
 * redirects itself and the final URL stays the requested one (logged).
 */
#include "http_client.h"
#include "log.h"
#include "util.h"
#include "xthread.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Sony prototypes (as function pointer types) ------------------------- */
typedef int (*fn_sceNetInit)(void);
typedef int (*fn_sceNetPoolCreate)(const char *, int, int);
typedef int (*fn_sceSslInit)(size_t);
/* shared signatures (identical between sceHttp and sceHttp2) */
typedef int (*fn_DeleteTemplate)(int);
typedef int (*fn_DeleteRequest)(int);
typedef int (*fn_AddRequestHeader)(int, const char *, const char *, uint32_t);
typedef int (*fn_SendRequest)(int, const void *, size_t);
typedef int (*fn_GetStatusCode)(int, int *);
typedef int (*fn_GetAllResponseHeaders)(int, char **, size_t *);
typedef int (*fn_ReadData)(int, void *, size_t);
typedef int (*fn_AbortRequest)(int);
typedef int (*fn_SetTimeout)(int, uint32_t);          /* usec */
typedef int (*fn_SetAutoRedirect)(int, int);
/* Http (v1) */
typedef int (*fn_h1_Init)(int, int, size_t);
typedef int (*fn_h1_CreateTemplate)(int, const char *, int, int);
typedef int (*fn_h1_CreateConnectionWithURL)(int, const char *, int);
typedef int (*fn_h1_DeleteConnection)(int);
typedef int (*fn_h1_CreateRequestWithURL)(int, int, const char *, uint64_t);
typedef int (*fn_h1_SslCallback)(int, void *const *, int, void *);
typedef int (*fn_h1_SetSslCallback)(int, fn_h1_SslCallback, void *);
typedef int (*fn_h1_SetResponseHeaderMaxSize)(int, size_t);
typedef int (*fn_h1_GetResponseContentLength)(int, int *, uint64_t *);
/* Http2 */
typedef int (*fn_h2_Init)(int, int, size_t, int);
typedef int (*fn_h2_CreateTemplate)(int, const char *, int, int);
typedef int (*fn_h2_CreateRequestWithURL)(int, const char *, const char *, uint64_t);

enum { SCE_HTTP_METHOD_GET = 0, SCE_HTTP_METHOD_POST = 1, SCE_HTTP_METHOD_HEAD = 2, SCE_HTTP_METHOD_OPTIONS = 3,
       SCE_HTTP_METHOD_PUT = 4, SCE_HTTP_METHOD_DELETE = 5 };
#define SCE_HTTP_VERSION_1_1 2
#define SCE_HTTP_HEADER_MAX 0x2000
/* Pools are sized for SCE_HTTP_MAX_INFLIGHT requests at once (the SDK's
 * single-request sample gives SSL and HTTP 256 KiB each). */
#define SCE_HTTP_MAX_INFLIGHT 24      /* request slots, see slot_enter() */
#define SCE_HTTP_JOB_INFLIGHT 20      /* of those, long transfers (xhttp_req.transfer) */
#define SCE_HTTP_POOL (8u << 20)      /* ~340 KiB per request */
#define SCE_HTTP2_POOL (8u << 20)
#define SCE_NET_POOL (2 << 20)
#define SCE_SSL_POOL (4u << 20)       /* ~170 KiB per TLS connection */
#define SCE_HTTP2_MAX_CONCURRENT 32   /* >= SCE_HTTP_MAX_INFLIGHT */
#define SLOT_POLL_MS 50
#define READ_CHUNK (256u << 10)

typedef struct sce_api {
  int v1;                    /* 1 = sceHttp (primary), 0 = sceHttp2 */
  int ctx;                   /* http context id */
  /* common */
  fn_DeleteTemplate DeleteTemplate;
  fn_DeleteRequest DeleteRequest;
  fn_AddRequestHeader AddRequestHeader;
  fn_SendRequest SendRequest;
  fn_GetStatusCode GetStatusCode;
  fn_GetAllResponseHeaders GetAllResponseHeaders;
  fn_ReadData ReadData;
  fn_AbortRequest AbortRequest;                             /* optional */
  fn_SetTimeout SetConnectTimeOut, SetRecvTimeOut, SetSendTimeOut, SetResolveTimeOut;   /* optional */
  fn_SetAutoRedirect SetAutoRedirect;                       /* optional */
  /* v1 only */
  fn_h1_CreateTemplate h1_CreateTemplate;
  fn_h1_CreateConnectionWithURL h1_CreateConnectionWithURL;
  fn_h1_DeleteConnection h1_DeleteConnection;
  fn_h1_CreateRequestWithURL h1_CreateRequestWithURL;
  fn_h1_SetSslCallback h1_SetSslCallback;                   /* optional */
  fn_h1_SetResponseHeaderMaxSize h1_SetResponseHeaderMaxSize;   /* optional */
  fn_h1_GetResponseContentLength h1_GetResponseContentLength;   /* optional */
  /* v2 only */
  fn_h2_CreateTemplate h2_CreateTemplate;
  fn_h2_CreateRequestWithURL h2_CreateRequestWithURL;
} sce_api;

struct xhttp_conn {
  int tmpl, conn, req;
  volatile int *cancel;
  volatile int aborted;
  int head_only;
  int eof;
  int slot_job;              /* holds a transfer slot (see slot_enter) */
};

static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static int g_available;
static int g_init_rc = -1;
static sce_api A;
static char g_backend[16] = "none";
static int g_net_mem = -1, g_ssl_ctx = -1;
static int g_ssl_retry_logged;

/* ---- loading -------------------------------------------------------------- */

static void *load_lib(const char *name) {
  const char *const dirs[] = {"", "/system/common/lib/", "/system_ex/common/lib/", "/system/priv/lib/"};
  for (size_t i = 0; i < XS_ARRAY_LEN(dirs); i++) {
    char path[160];
    xsnprintf(path, sizeof path, "%s%s", dirs[i], name);
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (h) { LOGI("http: loaded %s", path); return h; }
  }
  const char *e = dlerror();
  LOGW("http: dlopen(%s) failed: %s", name, e ? e : "unknown");
  return NULL;
}

static void *sym(void *lib, const char *name, int required) {
  void *p = lib ? dlsym(lib, name) : NULL;
  if (!p) p = dlsym(RTLD_DEFAULT, name);
  if (!p) {
    if (required) LOGW("http: required symbol %s is missing", name);
    else LOGI("http: optional symbol %s not available", name);
  }
  return p;
}

static int bind_v1(void *lib) {
  memset(&A, 0, sizeof A);
  A.v1 = 1;
  A.h1_CreateTemplate = (fn_h1_CreateTemplate)sym(lib, "sceHttpCreateTemplate", 1);
  A.DeleteTemplate = (fn_DeleteTemplate)sym(lib, "sceHttpDeleteTemplate", 1);
  A.h1_CreateConnectionWithURL = (fn_h1_CreateConnectionWithURL)sym(lib, "sceHttpCreateConnectionWithURL", 1);
  A.h1_DeleteConnection = (fn_h1_DeleteConnection)sym(lib, "sceHttpDeleteConnection", 1);
  A.h1_CreateRequestWithURL = (fn_h1_CreateRequestWithURL)sym(lib, "sceHttpCreateRequestWithURL", 1);
  A.DeleteRequest = (fn_DeleteRequest)sym(lib, "sceHttpDeleteRequest", 1);
  A.AddRequestHeader = (fn_AddRequestHeader)sym(lib, "sceHttpAddRequestHeader", 1);
  A.SendRequest = (fn_SendRequest)sym(lib, "sceHttpSendRequest", 1);
  A.GetStatusCode = (fn_GetStatusCode)sym(lib, "sceHttpGetStatusCode", 1);
  A.GetAllResponseHeaders = (fn_GetAllResponseHeaders)sym(lib, "sceHttpGetAllResponseHeaders", 1);
  A.ReadData = (fn_ReadData)sym(lib, "sceHttpReadData", 1);
  A.AbortRequest = (fn_AbortRequest)sym(lib, "sceHttpAbortRequest", 0);
  A.SetConnectTimeOut = (fn_SetTimeout)sym(lib, "sceHttpSetConnectTimeOut", 0);
  A.SetRecvTimeOut = (fn_SetTimeout)sym(lib, "sceHttpSetRecvTimeOut", 0);
  A.SetSendTimeOut = (fn_SetTimeout)sym(lib, "sceHttpSetSendTimeOut", 0);
  A.SetResolveTimeOut = (fn_SetTimeout)sym(lib, "sceHttpSetResolveTimeOut", 0);
  A.SetAutoRedirect = (fn_SetAutoRedirect)sym(lib, "sceHttpSetAutoRedirect", 0);
  A.h1_SetSslCallback = (fn_h1_SetSslCallback)sym(lib, "sceHttpsSetSslCallback", 0);
  A.h1_SetResponseHeaderMaxSize = (fn_h1_SetResponseHeaderMaxSize)sym(lib, "sceHttpSetResponseHeaderMaxSize", 0);
  A.h1_GetResponseContentLength = (fn_h1_GetResponseContentLength)sym(lib, "sceHttpGetResponseContentLength", 0);
  return A.h1_CreateTemplate && A.DeleteTemplate && A.h1_CreateConnectionWithURL && A.h1_DeleteConnection &&
         A.h1_CreateRequestWithURL && A.DeleteRequest && A.AddRequestHeader && A.SendRequest && A.GetStatusCode &&
         A.GetAllResponseHeaders && A.ReadData;
}

static int bind_v2(void *lib) {
  memset(&A, 0, sizeof A);
  A.v1 = 0;
  A.h2_CreateTemplate = (fn_h2_CreateTemplate)sym(lib, "sceHttp2CreateTemplate", 1);
  A.DeleteTemplate = (fn_DeleteTemplate)sym(lib, "sceHttp2DeleteTemplate", 1);
  A.h2_CreateRequestWithURL = (fn_h2_CreateRequestWithURL)sym(lib, "sceHttp2CreateRequestWithURL", 1);
  A.DeleteRequest = (fn_DeleteRequest)sym(lib, "sceHttp2DeleteRequest", 1);
  A.AddRequestHeader = (fn_AddRequestHeader)sym(lib, "sceHttp2AddRequestHeader", 1);
  A.SendRequest = (fn_SendRequest)sym(lib, "sceHttp2SendRequest", 1);
  A.GetStatusCode = (fn_GetStatusCode)sym(lib, "sceHttp2GetStatusCode", 1);
  A.GetAllResponseHeaders = (fn_GetAllResponseHeaders)sym(lib, "sceHttp2GetAllResponseHeaders", 1);
  A.ReadData = (fn_ReadData)sym(lib, "sceHttp2ReadData", 1);
  A.AbortRequest = (fn_AbortRequest)sym(lib, "sceHttp2AbortRequest", 0);
  A.SetConnectTimeOut = (fn_SetTimeout)sym(lib, "sceHttp2SetConnectTimeOut", 0);
  A.SetRecvTimeOut = (fn_SetTimeout)sym(lib, "sceHttp2SetRecvTimeOut", 0);
  A.SetSendTimeOut = (fn_SetTimeout)sym(lib, "sceHttp2SetSendTimeOut", 0);
  A.SetResolveTimeOut = (fn_SetTimeout)sym(lib, "sceHttp2SetResolveTimeOut", 0);
  A.SetAutoRedirect = (fn_SetAutoRedirect)sym(lib, "sceHttp2SetAutoRedirect", 0);
  return A.h2_CreateTemplate && A.DeleteTemplate && A.h2_CreateRequestWithURL && A.DeleteRequest && A.AddRequestHeader &&
         A.SendRequest && A.GetStatusCode && A.GetAllResponseHeaders && A.ReadData;
}

static void do_init(void) {
  void *net = load_lib("libSceNet.sprx");
  void *ssl = load_lib("libSceSsl.sprx");
  fn_sceNetInit NetInit = (fn_sceNetInit)sym(net, "sceNetInit", 1);
  fn_sceNetPoolCreate NetPoolCreate = (fn_sceNetPoolCreate)sym(net, "sceNetPoolCreate", 1);
  fn_sceSslInit SslInit = (fn_sceSslInit)sym(ssl, "sceSslInit", 1);
  if (!NetInit || !NetPoolCreate || !SslInit) { xhttp_set_error("libSceNet/libSceSsl unavailable"); return; }
  int rc = NetInit();
  if (rc != 0 && (unsigned)rc != 0x804101C0u /* already initialized */) LOGW("http: sceNetInit -> 0x%08x", (unsigned)rc);
  g_net_mem = NetPoolCreate("xeno-store", SCE_NET_POOL, 0);
  if (g_net_mem < 0) { xhttp_set_error("sceNetPoolCreate failed 0x%08x", (unsigned)g_net_mem); return; }
  g_ssl_ctx = SslInit(SCE_SSL_POOL);
  if (g_ssl_ctx < 0) { xhttp_set_error("sceSslInit failed 0x%08x", (unsigned)g_ssl_ctx); return; }

  /* Primary: sceHttp v1 (§17.19). */
  void *h1 = load_lib("libSceHttp.sprx");
  fn_h1_Init H1Init = (fn_h1_Init)sym(h1, "sceHttpInit", h1 != NULL);
  if (h1 && H1Init && bind_v1(h1)) {
    A.ctx = H1Init(g_net_mem, g_ssl_ctx, SCE_HTTP_POOL);
    if (A.ctx >= 0) {
      xstrlcpy(g_backend, "sceHttp", sizeof g_backend);
      g_available = 1;
      g_init_rc = 0;
      LOGI("http: sceHttp ready (ctx %d, pool %u KiB, ssl %u KiB, %d request slots)", A.ctx, SCE_HTTP_POOL >> 10,
           SCE_SSL_POOL >> 10, SCE_HTTP_MAX_INFLIGHT);
      if (!A.SetAutoRedirect) LOGW("http: auto-redirect cannot be turned off — final URLs after redirects are not visible");
      return;
    }
    LOGW("http: sceHttpInit -> 0x%08x, trying sceHttp2", (unsigned)A.ctx);
  }
  /* Fallback: sceHttp2. */
  void *h2 = load_lib("libSceHttp2.sprx");
  fn_h2_Init H2Init = (fn_h2_Init)sym(h2, "sceHttp2Init", h2 != NULL);
  if (h2 && H2Init && bind_v2(h2)) {
    A.ctx = H2Init(g_net_mem, g_ssl_ctx, SCE_HTTP2_POOL, SCE_HTTP2_MAX_CONCURRENT);
    if (A.ctx >= 0) {
      xstrlcpy(g_backend, "sceHttp2", sizeof g_backend);
      g_available = 1;
      g_init_rc = 0;
      LOGI("http: sceHttp2 ready (ctx %d, pool %u KiB, ssl %u KiB, %d request slots)", A.ctx, SCE_HTTP2_POOL >> 10,
           SCE_SSL_POOL >> 10, SCE_HTTP_MAX_INFLIGHT);
      if (!A.SetAutoRedirect) LOGW("http: auto-redirect cannot be turned off — final URLs after redirects are not visible");
      return;
    }
    LOGW("http: sceHttp2Init -> 0x%08x", (unsigned)A.ctx);
  }
  memset(&A, 0, sizeof A);
  xhttp_set_error("no usable Sony HTTP library on this firmware");
  LOGE("http: downloads unavailable — neither sceHttp nor sceHttp2 could be initialized");
}

int xhttp_global_init(void) {
  pthread_once(&init_once, do_init);
  return g_init_rc;
}

int xhttp_available(void) { return g_available; }
const char *xhttp_backend_name(void) { return g_backend; }

/* ---- error mapping -------------------------------------------------------- */

#define SCE_HTTP_ERROR_OUT_OF_MEMORY 0x80431022u
#define SCE_SSL_ERROR_OUT_OF_MEMORY 0x80435022u
#define SCE_HTTPS_ERROR_CERT 0x80435060u
#define SCE_HTTPS_ERROR_HANDSHAKE 0x80435061u

static int is_oom(int rc) { return (unsigned)rc == SCE_HTTP_ERROR_OUT_OF_MEMORY || (unsigned)rc == SCE_SSL_ERROR_OUT_OF_MEMORY; }

/* SCE_NET_ERROR_* carry the BSD errno in the low byte (0x804100xx). */
static int sce_error_class(int rc) {
  unsigned u = (unsigned)rc;
  if (is_oom(rc)) return XHTTP_ERR_OTHER;
  if ((u & 0xFFFF0000u) == 0x80410000u) {
    switch (u & 0xFF) {
      case 49: /* EADDRNOTAVAIL */ case 50: /* ENETDOWN */ case 51: /* ENETUNREACH */ case 64: /* EHOSTDOWN */ case 65: /* EHOSTUNREACH */
        return XHTTP_ERR_NETDOWN;
      case 60: return XHTTP_ERR_TIMEOUT;     /* ETIMEDOUT */
      case 61: return XHTTP_ERR_REFUSED;     /* ECONNREFUSED */
      default: return XHTTP_ERR_OTHER;
    }
  }
  if ((u & 0xFFFFF000u) == 0x80435000u) return XHTTP_ERR_TLS;      /* SCE_HTTPS_ERROR_* / SCE_SSL_ERROR_* */
  if ((u & 0xFFFFF000u) == 0x80436000u) return XHTTP_ERR_DNS;      /* SCE_HTTP_ERROR_RESOLVER_* */
  if (u == 0x80431068u) return XHTTP_ERR_TIMEOUT;                   /* SCE_HTTP_ERROR_TIMEOUT */
  if (u == 0x80431080u) return XHTTP_ERR_CANCELLED;                 /* SCE_HTTP_ERROR_ABORTED */
  if (u == 0x80431063u || u == 0x80431064u) return XHTTP_ERR_NETDOWN;   /* SCE_HTTP_ERROR_NETWORK / ..._TRANSPORT */
  return XHTTP_ERR_OTHER;
}

/* Only a rejected certificate earns the retry without certificate checks;
 * pool exhaustion, handshake and I/O failures are reported as they are. */
static int is_cert_error(int rc) { return (unsigned)rc == SCE_HTTPS_ERROR_CERT; }

static const char *sce_error_name(int rc) {
  switch ((unsigned)rc) {
    case SCE_HTTP_ERROR_OUT_OF_MEMORY: return " (HTTP pool out of memory)";
    case SCE_SSL_ERROR_OUT_OF_MEMORY: return " (SSL pool out of memory)";
    case SCE_HTTPS_ERROR_CERT: return " (certificate rejected)";
    case SCE_HTTPS_ERROR_HANDSHAKE: return " (TLS handshake failed)";
    case 0x80431068u: return " (timeout)";
    case 0x80431080u: return " (aborted)";
    default: return "";
  }
}

static int slots_in_use(void);

static void set_sce_error(const char *what, int rc) {
  const char *api = A.v1 ? "sceHttp" : "sceHttp2";
  xhttp_set_error("%s%s 0x%08x%s", api, what, (unsigned)rc, sce_error_name(rc));
  xhttp_set_error_class(sce_error_class(rc));
  if (is_oom(rc)) LOGW("http: %s%s 0x%08x%s with %d requests in flight", api, what, (unsigned)rc, sce_error_name(rc), slots_in_use());
}

/* ---- request slots -------------------------------------------------------- */

/* Every request holds pool memory from xhttp_open() to xhttp_close(), and
 * the settings allow 16 connections × 4 jobs next to the server workers'
 * image and catalog fetches. Long transfers (xhttp_req.transfer + a cancel
 * flag: download segments, installs) keep a slot for minutes, so they get at
 * most SCE_HTTP_JOB_INFLIGHT and the rest stays free for the short fetches the
 * UI is waiting on. Transfers wait for a slot with no deadline (every holder is
 * a transfer that ends, and a timeout would only burn the job's retries while
 * the others run) and never overtake transfers that are already waiting, so a
 * segment re-opening after its chunk cannot keep its slot from another job. */
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_slots, g_slots_job, g_job_waiters;

static int slots_in_use(void) {
  pthread_mutex_lock(&g_slot_mu);
  int n = g_slots;
  pthread_mutex_unlock(&g_slot_mu);
  return n;
}

static int slot_enter(const xhttp_req *req, int job, const char *host) {
  int64_t wait_ms = req->connect_timeout_ms > 0 ? req->connect_timeout_ms : 10000;
  int64_t deadline = util_now_ms() + wait_ms;
  int waiting = 0, queued = 0;
  for (;;) {
    pthread_mutex_lock(&g_slot_mu);
    int busy = g_slots >= SCE_HTTP_MAX_INFLIGHT ||
               (job && (g_slots_job >= SCE_HTTP_JOB_INFLIGHT || (!queued && g_job_waiters > 0)));
    int stop = busy && req->cancel_flag && *req->cancel_flag;
    if (!busy) { g_slots++; if (job) g_slots_job++; }
    if (job && queued != (busy && !stop)) { queued = !queued; g_job_waiters += queued ? 1 : -1; }
    int used = g_slots, used_job = g_slots_job;
    pthread_mutex_unlock(&g_slot_mu);
    if (!busy) return 0;
    if (stop) { xhttp_set_error("cancelled"); xhttp_set_error_class(XHTTP_ERR_CANCELLED); return -1; }
    if (!job && util_now_ms() >= deadline) {
      xhttp_set_error("all request slots busy (%d in flight, %d transfers)", used, used_job);
      LOGW("http: no request slot for %s after %lld ms (%d/%d in flight, %d/%d transfers)", host, (long long)wait_ms, used,
           SCE_HTTP_MAX_INFLIGHT, used_job, SCE_HTTP_JOB_INFLIGHT);
      return -1;
    }
    if (!waiting) {
      waiting = 1;
      LOGD("http: waiting for a request slot for %s (%d in flight, %d transfers)", host, used, used_job);
      xthread_net("sceHttp wait for a request slot", host);
    }
    util_sleep_ms(SLOT_POLL_MS);
  }
}

static void slot_leave(int job) {
  pthread_mutex_lock(&g_slot_mu);
  g_slots--;
  if (job) g_slots_job--;
  pthread_mutex_unlock(&g_slot_mu);
}

/* ---- abort watcher -------------------------------------------------------- */

/* sceHttpSendRequest resolves, connects, does the TLS handshake and waits for
 * the response headers in one call, before xhttp_open() can hand the conn to
 * anyone who could xhttp_abort() it. Requests that carry a cancel flag are
 * listed here for that call, and a watcher thread aborts the ones whose flag
 * is set, so pause/remove do not wait out the connect and receive timeouts.
 * An entry leaves the table (under the lock) before its request is deleted,
 * so a deleted or reused id is never aborted. */
#define WATCH_POLL_MS 100
static pthread_mutex_t g_watch_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_watch_once = PTHREAD_ONCE_INIT;
static int g_watch_ok;
static struct { volatile int *cancel; int req, aborted; } g_watch[SCE_HTTP_MAX_INFLIGHT];

static void *watch_thread(void *arg) {
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&g_watch_mu);
    for (int i = 0; i < SCE_HTTP_MAX_INFLIGHT; i++)
      if (g_watch[i].cancel && *g_watch[i].cancel && !g_watch[i].aborted) { g_watch[i].aborted = 1; A.AbortRequest(g_watch[i].req); }
    pthread_mutex_unlock(&g_watch_mu);
    util_sleep_ms(WATCH_POLL_MS);
  }
  return NULL;
}

static void watch_start(void) {
  g_watch_ok = A.AbortRequest && xthread_start(NULL, "http-abort", watch_thread, NULL, XT_STACK_SMALL, 1) == 0;
  if (A.AbortRequest && !g_watch_ok) LOGW("http: cannot start the abort watcher — cancels wait for sceHttpSendRequest to return");
}

/* Returns the table index, or -1 when the request is not watched. */
static int watch_add(int req_id, volatile int *cancel) {
  if (!cancel) return -1;
  pthread_once(&g_watch_once, watch_start);
  if (!g_watch_ok) return -1;
  int k = -1;
  pthread_mutex_lock(&g_watch_mu);
  for (int i = 0; i < SCE_HTTP_MAX_INFLIGHT && k < 0; i++)
    if (!g_watch[i].cancel) { g_watch[i].cancel = cancel; g_watch[i].req = req_id; g_watch[i].aborted = 0; k = i; }
  pthread_mutex_unlock(&g_watch_mu);
  return k;
}

static void watch_remove(int k) {
  if (k < 0) return;
  pthread_mutex_lock(&g_watch_mu);
  g_watch[k].cancel = NULL;
  pthread_mutex_unlock(&g_watch_mu);
}

/* ---- request ------------------------------------------------------------- */

/* Accept-any certificate callback (SPEC §17.19): only ever installed for
 * allowInsecureTls or the logged automatic retry after an SSL error. */
static int ssl_accept_any(int verify_err, void *const *certs, int n, void *arg) {
  (void)verify_err; (void)certs; (void)n; (void)arg;
  return 0;
}

static void conn_release(xhttp_conn *c) {
  if (!c) return;
  if (c->req >= 0 && A.DeleteRequest) A.DeleteRequest(c->req);
  if (c->conn >= 0 && A.v1 && A.h1_DeleteConnection) A.h1_DeleteConnection(c->conn);
  if (c->tmpl >= 0 && A.DeleteTemplate) A.DeleteTemplate(c->tmpl);
  free(c);
}

static int method_code(const char *m) {
  if (strcmp(m, "POST") == 0) return SCE_HTTP_METHOD_POST;
  if (strcmp(m, "HEAD") == 0) return SCE_HTTP_METHOD_HEAD;
  if (strcmp(m, "PUT") == 0) return SCE_HTTP_METHOD_PUT;
  if (strcmp(m, "DELETE") == 0) return SCE_HTTP_METHOD_DELETE;
  if (strcmp(m, "OPTIONS") == 0) return SCE_HTTP_METHOD_OPTIONS;
  return SCE_HTTP_METHOD_GET;
}

static void apply_timeouts(int tmpl, const xhttp_req *req) {
  uint32_t cms = (uint32_t)(req->connect_timeout_ms > 0 ? req->connect_timeout_ms : 10000);
  uint32_t rms = (uint32_t)(req->recv_timeout_ms > 0 ? req->recv_timeout_ms : 30000);
  if (A.SetConnectTimeOut) A.SetConnectTimeOut(tmpl, cms * 1000u);
  if (A.SetResolveTimeOut) A.SetResolveTimeOut(tmpl, cms * 1000u);
  if (A.SetRecvTimeOut) A.SetRecvTimeOut(tmpl, rms * 1000u);
  if (A.SetSendTimeOut) A.SetSendTimeOut(tmpl, rms * 1000u);
  /* redirects come back to xhttp_open(), which re-sends Range and records the final URL */
  if (A.SetAutoRedirect) A.SetAutoRedirect(tmpl, 0);
}

/* Adds Range, Accept and the caller's extra headers. */
static int add_headers(int req_id, const xhttp_req *req) {
  char v[64];
  if (req->range_start >= 0) {
    if (req->range_end >= 0) xsnprintf(v, sizeof v, "bytes=%lld-%lld", (long long)req->range_start, (long long)req->range_end);
    else xsnprintf(v, sizeof v, "bytes=%lld-", (long long)req->range_start);
    int rc = A.AddRequestHeader(req_id, "Range", v, 0);
    if (rc != 0) { set_sce_error("AddRequestHeader(Range)", rc); return -1; }
  }
  A.AddRequestHeader(req_id, "Accept", "*/*", 0);
  if (req->extra_headers) {
    char copy[2048];
    xstrlcpy(copy, req->extra_headers, sizeof copy);
    char *save = NULL;
    for (char *line = strtok_r(copy, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
      char *colon = strchr(line, ':');
      if (!colon) continue;
      *colon = 0;
      char *val = colon + 1;
      while (*val == ' ') val++;
      util_trim(line);
      if (line[0]) A.AddRequestHeader(req_id, line, val, 0);
    }
  }
  return 0;
}

/* Parses the raw response header block (status line + headers). */
static void parse_response_headers(xhttp_conn *c, xhttp_resp *resp) {
  char *hdr = NULL;
  size_t len = 0;
  if (A.GetAllResponseHeaders(c->req, &hdr, &len) != 0 || !hdr || !len) return;
  if (len > (64u << 10)) len = 64u << 10;
  char *copy = malloc(len + 1);
  if (!copy) return;
  memcpy(copy, hdr, len);
  copy[len] = 0;
  char *line = copy;
  if (util_istarts_with(line, "HTTP/")) { char *nl = strchr(line, '\n'); line = nl ? nl + 1 : line + strlen(line); }
  while (*line) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    size_t l = strlen(line);
    if (l && line[l - 1] == '\r') line[l - 1] = 0;
    char *colon = strchr(line, ':');
    if (colon) xhttp_resp_header(resp, line, (size_t)(colon - line), colon + 1, strlen(colon + 1));
    if (!nl) break;
    line = nl + 1;
  }
  free(copy);
  if (resp->content_length < 0 && A.v1 && A.h1_GetResponseContentLength) {
    int result = 0;
    uint64_t cl = 0;
    if (A.h1_GetResponseContentLength(c->req, &result, &cl) == 0 && result == 0 /* SCE_HTTP_CONTENTLEN_EXIST */)
      resp->content_length = (int64_t)cl;
  }
}

static int is_redirect(int s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

/* One request/response exchange on url; fills resp, returns the conn.
 * `insecure` installs the accept-any certificate callback (v1 only). */
static xhttp_conn *do_request(const xhttp_req *req, const char *method, const char *url, int insecure, xhttp_resp *resp, int *rc_out) {
  *rc_out = 0;
  xhttp_conn *c = calloc(1, sizeof *c);
  if (!c) { xhttp_set_error("out of memory"); return NULL; }
  c->tmpl = c->conn = c->req = -1;
  c->cancel = req->cancel_flag;
  c->head_only = strcmp(method, "HEAD") == 0;
  const char *ua = req->user_agent ? req->user_agent : XHTTP_DEFAULT_UA;
  char host[128];
  util_url_host(url, host, sizeof host);
  int rc;
  xthread_net("sceHttp CreateTemplate", host);
  if (A.v1) {
    c->tmpl = A.h1_CreateTemplate(A.ctx, ua, SCE_HTTP_VERSION_1_1, 1);
    if (c->tmpl < 0) { *rc_out = c->tmpl; set_sce_error("CreateTemplate", c->tmpl); conn_release(c); return NULL; }
    if (A.h1_SetResponseHeaderMaxSize) A.h1_SetResponseHeaderMaxSize(c->tmpl, SCE_HTTP_HEADER_MAX);
    apply_timeouts(c->tmpl, req);
    if (insecure && A.h1_SetSslCallback) A.h1_SetSslCallback(c->tmpl, ssl_accept_any, NULL);
    xthread_net("sceHttp CreateConnection", host);
    c->conn = A.h1_CreateConnectionWithURL(c->tmpl, url, 0);
    if (c->conn < 0) { *rc_out = c->conn; set_sce_error("CreateConnectionWithURL", c->conn); conn_release(c); return NULL; }
    c->req = A.h1_CreateRequestWithURL(c->conn, method_code(method), url, (uint64_t)req->body_len);
    if (c->req < 0) { *rc_out = c->req; set_sce_error("CreateRequestWithURL", c->req); conn_release(c); return NULL; }
  } else {
    c->tmpl = A.h2_CreateTemplate(A.ctx, ua, 3, 1);
    if (c->tmpl < 0) { *rc_out = c->tmpl; set_sce_error("CreateTemplate", c->tmpl); conn_release(c); return NULL; }
    apply_timeouts(c->tmpl, req);
    c->req = A.h2_CreateRequestWithURL(c->tmpl, method, url, (uint64_t)req->body_len);
    if (c->req < 0) { *rc_out = c->req; set_sce_error("CreateRequestWithURL", c->req); conn_release(c); return NULL; }
  }
  if (add_headers(c->req, req) != 0) { conn_release(c); return NULL; }
  xthread_net("sceHttp SendRequest", host);
  int w = watch_add(c->req, req->cancel_flag);
  rc = A.SendRequest(c->req, req->body_len ? req->body : NULL, req->body_len);
  if (rc != 0) { watch_remove(w); *rc_out = rc; set_sce_error("SendRequest", rc); conn_release(c); return NULL; }
  int status = 0;
  xthread_net("sceHttp GetStatusCode", host);
  rc = A.GetStatusCode(c->req, &status);
  watch_remove(w);
  if (rc != 0) { *rc_out = rc; set_sce_error("GetStatusCode", rc); conn_release(c); return NULL; }
  xhttp_resp_reset(resp);
  resp->status = status;
  xthread_net("sceHttp GetAllResponseHeaders", host);
  parse_response_headers(c, resp);
  xthread_net("sceHttp ReadData", host);
  return c;
}

/* Runs the request and follows redirects by hand (auto-redirect is off): one
 * template/connection/request per hop, Range re-sent, every hop's URL checked
 * like the first one, the final URL left in resp->location. */
static xhttp_conn *open_follow(const xhttp_req *req, xhttp_resp *resp) {
  char url[2048];
  xstrlcpy(url, req->url, sizeof url);
  const char *method = req->method ? req->method : "GET";
  xhttp_req cur = *req;
  int insecure = req->insecure_tls ? 1 : 0;
  int ssl_retried = 0;
  for (int hops = 0;;) {
    if (req->cancel_flag && *req->cancel_flag) { xhttp_set_error("cancelled"); xhttp_set_error_class(XHTTP_ERR_CANCELLED); return NULL; }
    int rc = 0;
    xhttp_conn *c = do_request(&cur, method, url, insecure, resp, &rc);
    if (!c) {
      /* Logged automatic retry after a rejected certificate (SPEC §17.19). */
      if (A.v1 && A.h1_SetSslCallback && !insecure && !ssl_retried && is_cert_error(rc) && util_istarts_with(url, "https")) {
        ssl_retried = 1;
        insecure = 1;
        char host[256];
        util_url_host(url, host, sizeof host);
        if (!g_ssl_retry_logged) { g_ssl_retry_logged = 1; LOGW("http: TLS verification failed for %s (0x%08x) — retrying once without certificate checks", host, (unsigned)rc); }
        else LOGD("http: TLS retry without certificate checks for %s (0x%08x)", host, (unsigned)rc);
        continue;
      }
      return NULL;
    }
    if (is_redirect(resp->status) && resp->location[0] && hops < req->follow_redirects) {
      char next[2048];
      xurl nu;
      if (util_url_resolve(url, resp->location, next, sizeof next) != 0 || util_url_parse(next, &nu) != 0 || nu.has_userinfo) {
        xhttp_set_error("bad redirect");
        conn_release(c);
        return NULL;
      }
      conn_release(c);
      hops++;
      xstrlcpy(url, next, sizeof url);
      if (resp->status == 303 && strcmp(method, "HEAD") != 0) { method = "GET"; cur.body = NULL; cur.body_len = 0; }
      /* the next host's certificate is verified again (with its own retry) */
      insecure = req->insecure_tls ? 1 : 0;
      ssl_retried = 0;
      LOGD("http: redirect %d -> %s", resp->status, url);
      continue;
    }
    xstrlcpy(resp->location, url, sizeof resp->location);
    if (c->head_only || resp->status == 204 || resp->status == 304) c->eof = 1;
    return c;
  }
}

int xhttp_open(const xhttp_req *req, xhttp_conn **out, xhttp_resp *resp) {
  *out = NULL;
  xhttp_resp_reset(resp);
  xhttp_set_error("");
  if (xhttp_global_init() != 0 || !g_available) { xhttp_set_error("Sony HTTP library unavailable"); return -1; }
  xurl u;
  if (util_url_parse(req->url, &u) != 0) { xhttp_set_error("malformed URL"); return -1; }
  if (u.has_userinfo) { xhttp_set_error("URLs with credentials are not allowed"); return -1; }
  int job = req->transfer && req->cancel_flag != NULL;
  if (slot_enter(req, job, u.host) != 0) return -1;
  xhttp_conn *c = open_follow(req, resp);
  if (!c) { slot_leave(job); return -1; }
  c->slot_job = job;
  *out = c;
  return 0;
}

int xhttp_read(xhttp_conn *c, void *buf, size_t cap) {
  if (!c || cap == 0) return -1;
  if (c->eof) return 0;
  if (c->aborted || (c->cancel && *c->cancel)) { xhttp_set_error("cancelled"); xhttp_set_error_class(XHTTP_ERR_CANCELLED); return -1; }
  if (cap > READ_CHUNK) cap = READ_CHUNK;
  int n = A.ReadData(c->req, buf, cap);
  if (n > 0) return n;
  if (n == 0) { c->eof = 1; return 0; }
  if (c->aborted || (c->cancel && *c->cancel)) { xhttp_set_error("cancelled"); xhttp_set_error_class(XHTTP_ERR_CANCELLED); }
  else set_sce_error("ReadData", n);
  return -1;
}

void xhttp_abort(xhttp_conn *c) {
  if (!c) return;
  c->aborted = 1;
  if (c->req >= 0 && A.AbortRequest) A.AbortRequest(c->req);
}

void xhttp_close(xhttp_conn *c) {
  if (c) {
    int job = c->slot_job;
    xthread_net("sceHttp Delete", "");
    conn_release(c);
    slot_leave(job);
  }
  xthread_net(NULL, NULL);
}
