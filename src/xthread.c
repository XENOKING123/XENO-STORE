/*
 * xthread.c — see xthread.h.
 */
#include "xthread.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#ifndef XENO_HOST
#include <pthread_np.h>
#endif

#define XT_SLOTS 192
#define XT_NOTE 120
#define XT_ALTSTACK (64u << 10)
#define XT_REPORT 2048

typedef struct xt_slot {
  volatile int used;
  pthread_t tid;
  char name[24];
  char note[XT_NOTE];
  char net[XT_NOTE];
  uintptr_t stack_hi;   /* an address near the top of the thread's stack */
  size_t stack_size;
} xt_slot;

typedef struct xt_start {
  void *(*fn)(void *);
  void *arg;
  size_t stack;
  char name[24];
} xt_start;

static xt_slot slots[XT_SLOTS];
static pthread_mutex_t slot_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;
static pthread_key_t slot_key;
static char crash_path[1024], crash_tmp[1024], prev_path[1024], log_path[1024];
static char prev_report[XT_REPORT];
static long prev_uptime = -1;
static int prev_fresh;
static time_t boot_time;
static volatile sig_atomic_t in_crash;
static size_t platform_stack;

static void make_key(void) { pthread_key_create(&slot_key, NULL); }

static xt_slot *slot_claim(const char *name, size_t stack, uintptr_t hi) {
  pthread_once(&key_once, make_key);
  xt_slot *s = NULL;
  pthread_mutex_lock(&slot_mu);
  for (int i = 0; i < XT_SLOTS; i++) {
    if (!slots[i].used) { s = &slots[i]; break; }
  }
  if (s) {
    s->tid = pthread_self();
    xstrlcpy(s->name, name, sizeof s->name);
    s->note[0] = 0;
    s->net[0] = 0;
    s->stack_hi = hi;
    s->stack_size = stack;
    s->used = 1;
  }
  pthread_mutex_unlock(&slot_mu);
  if (s) pthread_setspecific(slot_key, s);
  return s;
}

static void slot_release(xt_slot *s) {
  if (!s) return;
  pthread_setspecific(slot_key, NULL);
  pthread_mutex_lock(&slot_mu);
  s->used = 0;
  pthread_mutex_unlock(&slot_mu);
}

static void *altstack_on(void) {
  void *mem = malloc(XT_ALTSTACK);
  if (!mem) return NULL;
  stack_t ss;
  memset(&ss, 0, sizeof ss);
  ss.ss_sp = mem;
  ss.ss_size = XT_ALTSTACK;
  if (sigaltstack(&ss, NULL) != 0) { free(mem); return NULL; }
  return mem;
}

static void altstack_off(void *mem) {
  if (!mem) return;
  stack_t ss;
  memset(&ss, 0, sizeof ss);
  ss.ss_flags = SS_DISABLE;
  sigaltstack(&ss, NULL);
  free(mem);
}

static void *trampoline(void *p) {
  xt_start st = *(xt_start *)p;
  free(p);
  volatile int top = 0;
  xt_slot *s = slot_claim(st.name, st.stack, (uintptr_t)&top);
#ifndef XENO_HOST
  pthread_set_name_np(pthread_self(), st.name);
#endif
  void *alt = altstack_on();
  void *r = st.fn(st.arg);
  altstack_off(alt);
  slot_release(s);
  (void)top;
  return r;
}

int xthread_start(pthread_t *out, const char *name, void *(*fn)(void *), void *arg, size_t stack, int detached) {
  xt_start *st = malloc(sizeof *st);
  if (!st) return ENOMEM;
  st->fn = fn;
  st->arg = arg;
  st->stack = stack ? stack : XT_STACK_DEFAULT;
#ifdef XENO_HOST
  /* host test hook: squeeze every thread to prove the console sizes have headroom */
  const char *env = getenv("XENO_THREAD_STACK_KB");
  if (env && atoi(env) >= 64) st->stack = (size_t)atoi(env) << 10;
#endif
  xstrlcpy(st->name, name && *name ? name : "thread", sizeof st->name);
  pthread_attr_t at;
  pthread_attr_init(&at);
  int src = pthread_attr_setstacksize(&at, st->stack);
  if (src != 0) LOGW("thread %s: stack size %zu KiB refused (%d)", st->name, st->stack >> 10, src);
  if (detached) pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  pthread_t th;
  int rc = pthread_create(&th, &at, trampoline, st);
  pthread_attr_destroy(&at);
  if (rc != 0) {
    LOGW("thread %s: cannot start (%d)", st->name, rc);
    free(st);
    return rc;
  }
  if (out) *out = th;
  return 0;
}

void xthread_note(const char *fmt, ...) {
  pthread_once(&key_once, make_key);
  xt_slot *s = pthread_getspecific(slot_key);
  if (!s) return;
  char buf[XT_NOTE];
  va_list ap;
  va_start(ap, fmt);
  xvsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  memcpy(s->note, buf, sizeof buf);
}

void xthread_net(const char *stage, const char *host) {
  pthread_once(&key_once, make_key);
  xt_slot *s = pthread_getspecific(slot_key);
  if (!s) return;
  if (!stage) { s->net[0] = 0; return; }
  char buf[XT_NOTE];
  xsnprintf(buf, sizeof buf, "%s %s", stage, host ? host : "");
  memcpy(s->net, buf, sizeof buf);
}

size_t xthread_platform_default_stack(void) { return platform_stack; }

const char *crash_previous(long *uptime_s, int *fresh) {
  if (uptime_s) *uptime_s = prev_uptime;
  if (fresh) *fresh = prev_fresh;
  return prev_report;
}

/* ---- the fatal-signal handler (async-signal-safe: no stdio, no malloc) ---- */

static void put_s(char *b, size_t cap, size_t *n, const char *s) {
  while (s && *s && *n + 1 < cap) b[(*n)++] = *s++;
}

static void put_u(char *b, size_t cap, size_t *n, uint64_t v, int base) {
  char tmp[24];
  int k = 0;
  do { unsigned d = (unsigned)(v % (unsigned)base); tmp[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= (unsigned)base; } while (v && k < (int)sizeof tmp);
  if (base == 16) put_s(b, cap, n, "0x");
  while (k && *n + 1 < cap) b[(*n)++] = tmp[--k];
}

static const char *sig_name(int sig) {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV (bad memory access)";
    case SIGBUS: return "SIGBUS (bad memory access)";
    case SIGILL: return "SIGILL (illegal instruction)";
    case SIGFPE: return "SIGFPE (arithmetic error)";
    case SIGABRT: return "SIGABRT (abort)";
#ifdef SIGSYS
    case SIGSYS: return "SIGSYS (bad system call)";
#endif
    default: return "fatal signal";
  }
}

static void reg_ip_sp(void *uctx, uint64_t *ip, uint64_t *sp) {
  *ip = *sp = 0;
  if (!uctx) return;
  ucontext_t *uc = uctx;
#if defined(XENO_HOST) && defined(__linux__) && defined(__x86_64__)
  *ip = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
  *sp = (uint64_t)uc->uc_mcontext.gregs[REG_RSP];
#elif !defined(XENO_HOST)
  *ip = (uint64_t)uc->uc_mcontext.mc_rip;
  *sp = (uint64_t)uc->uc_mcontext.mc_rsp;
#else
  (void)uc;
#endif
}

static void on_fatal(int sig, siginfo_t *si, void *uctx) {
  if (__sync_lock_test_and_set(&in_crash, 1)) {
    /* another thread is writing the report: let it finish before the process ends */
    struct timespec ts = { 0, 200 * 1000 * 1000 };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
    _exit(128 + sig);
  }
  xt_slot *s = NULL;
  pthread_t me = pthread_self();
  for (int i = 0; i < XT_SLOTS; i++) {
    if (slots[i].used && pthread_equal(slots[i].tid, me)) { s = &slots[i]; break; }
  }
  uint64_t ip = 0, sp = 0;
  reg_ip_sp(uctx, &ip, &sp);
  uintptr_t fault = si ? (uintptr_t)si->si_addr : 0;
  char b[XT_REPORT];
  size_t n = 0;
  put_s(b, sizeof b, &n, "XENO STORE " XENO_VERSION " stopped: ");
  put_s(b, sizeof b, &n, sig_name(sig));
  put_s(b, sizeof b, &n, "\nthread: ");
  put_s(b, sizeof b, &n, s ? s->name : "unknown");
  put_s(b, sizeof b, &n, "\nactivity: ");
  put_s(b, sizeof b, &n, s && s->note[0] ? s->note : "-");
  if (s && s->net[0]) { put_s(b, sizeof b, &n, "\nnetwork: "); put_s(b, sizeof b, &n, s->net); }
  if (s && s->stack_hi && sp && sp <= s->stack_hi) {
    uint64_t used = s->stack_hi - sp;
    put_s(b, sizeof b, &n, "\nstack: ");
    put_u(b, sizeof b, &n, used >> 10, 10);
    put_s(b, sizeof b, &n, " KiB used of ");
    put_u(b, sizeof b, &n, s->stack_size >> 10, 10);
    put_s(b, sizeof b, &n, " KiB");
    if (s->stack_size && used + (16u << 10) >= s->stack_size) put_s(b, sizeof b, &n, " (stack overflow)");
  }
  put_s(b, sizeof b, &n, "\nfault address: ");
  put_u(b, sizeof b, &n, (uint64_t)fault, 16);
  put_s(b, sizeof b, &n, "\ncode address: ");
  put_u(b, sizeof b, &n, ip, 16);
  if (ip) {
    put_s(b, sizeof b, &n, " (crash_init");
    uintptr_t anchor = (uintptr_t)&crash_init;
    if (ip >= anchor) { put_s(b, sizeof b, &n, "+"); put_u(b, sizeof b, &n, ip - anchor, 16); }
    else { put_s(b, sizeof b, &n, "-"); put_u(b, sizeof b, &n, anchor - ip, 16); }
    put_s(b, sizeof b, &n, ")");
  }
  put_s(b, sizeof b, &n, "\nuptime: ");
  time_t now = time(NULL);
  put_u(b, sizeof b, &n, (uint64_t)(now > boot_time ? now - boot_time : 0), 10);
  put_s(b, sizeof b, &n, " s\n");
  if (crash_path[0]) {   /* temp file + rename: a cut-short write never leaves a partial crash.txt */
    int fd = open(crash_tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
      ssize_t w = write(fd, b, n);
      close(fd);
      if (w == (ssize_t)n) rename(crash_tmp, crash_path);
    }
  }
  if (log_path[0]) {
    int fd = open(log_path, O_WRONLY | O_APPEND);
    if (fd >= 0) {
      ssize_t w = write(fd, "CRASH ", 6);
      w = write(fd, b, n);
      (void)w;
      close(fd);
    }
  }
  _exit(128 + sig);
}

static void read_report(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return;
  ssize_t r = read(fd, prev_report, sizeof prev_report - 1);
  close(fd);
  prev_report[r > 0 ? r : 0] = 0;
  const char *u = strstr(prev_report, "\nuptime: ");
  prev_uptime = u ? strtol(u + 9, NULL, 10) : -1;
}

int crash_init(const char *data_dir) {
  boot_time = time(NULL);
  int fresh = 0;
  if (data_dir && *data_dir) {
    xsnprintf(crash_path, sizeof crash_path, "%s/crash.txt", data_dir);
    xsnprintf(crash_tmp, sizeof crash_tmp, "%s/crash.txt.tmp", data_dir);
    xsnprintf(prev_path, sizeof prev_path, "%s/crash.prev.txt", data_dir);
    xsnprintf(log_path, sizeof log_path, "%s/log.txt", data_dir);
    struct stat st;
    if (stat(crash_path, &st) == 0) {
      read_report(crash_path);   /* moved aside by crash_consume() */
      fresh = prev_report[0] != 0;
    } else if (stat(prev_path, &st) == 0) {
      read_report(prev_path);   /* an older report, still shown in Diagnostics */
    }
    prev_fresh = fresh;
  }
  /* the platform's default thread stack, for the log */
  pthread_attr_t a;
  if (pthread_attr_init(&a) == 0) {
    size_t sz = 0;
    if (pthread_attr_getstacksize(&a, &sz) == 0) platform_stack = sz;
    pthread_attr_destroy(&a);
  }
  /* register the main thread */
  volatile int top = 0;
  uintptr_t hi = (uintptr_t)&top;
  size_t main_stack = 0;
#ifndef XENO_HOST
  if (pthread_attr_init(&a) == 0) {
    void *addr = NULL;
    size_t sz = 0;
    if (pthread_attr_get_np(pthread_self(), &a) == 0 && pthread_attr_getstack(&a, &addr, &sz) == 0 && addr) { hi = (uintptr_t)addr + sz; main_stack = sz; }
    pthread_attr_destroy(&a);
  }
#endif
  (void)top;
  slot_claim("main", main_stack, main_stack ? hi : 0);
  altstack_on();   /* kept for the life of the process */
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = on_fatal;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT,
#ifdef SIGSYS
                      SIGSYS,
#endif
  };
  for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, NULL);
  return fresh;
}

void crash_consume(void) {
  if (crash_path[0]) rename(crash_path, prev_path);
}
