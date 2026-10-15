/* XENO STORE - launcher.c: the Games-row app (PPSA99500) eboot.bin.
 * Copyright (C) 2026 XENO STORE contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A native PS5 app built with the ps5-native-app-boilerplate toolchain
 * (Copyright (C) 2026 BlackBearReloaded, GPL-3.0-or-later; see ../NOTICE):
 * tooling/native/app_crt.cpp supplies _start; the C library calls go to the
 * system's libSceLibcInternal, files and sockets to libkernel, the exit to
 * libSceSystemService. The app folder also carries sce_module/libc.prx, the
 * kit's clean-room libc module the app loader expects next to eboot.bin.
 * It replaces the freestanding raw-syscall launcher in app/eboot/eboot_main.c
 * and does the same job:
 *
 *   1. The store already runs (127.0.0.1:9095 answers "HTTP/1.x 200" to
 *      POST /api/launcher/open, loopback only): it opens the browser
 *      full-screen on itself, and the app closes.
 *   2. Otherwise hand the store that ships inside the app (/app0/xeno-store.elf)
 *      to the jailbreak's ELF loader on 127.0.0.1:9021, poll 9095 every 500 ms
 *      for up to 30 s and ask the store to open once it answers. A newer
 *      version downloaded from Settings takes over by itself at start.
 *   3. No loader either: log it and close (the tile does nothing until the
 *      jailbreak is loaded).
 *
 * Every step goes to the kernel log (sceKernelDebugOutText), prefixed
 * "[XENO STORE] launcher:".
 *
 * Ending: main returns its status to app_crt.cpp, which hands it to
 * catchReturnFromMain() below. That asks the system to close the title with
 * sceSystemServiceLoadExec("exit", NULL), as ps5-homebrew-ui's sys::quit()
 * does (Copyright (C) 2026 BlackBearReloaded, GPL-3.0-or-later): the kit
 * notes that exit() and _Exit() end a native title in the crash reporter. If
 * the request is refused the app stays idle (the kit's park()) until it is
 * closed from the home screen.
 */

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* libkernel and libSceSystemService; the SDK headers do not declare them. */
int sceKernelDebugOutText(int channel, const char *text);
int sceKernelUsleep(unsigned int microseconds);
int sceSystemServiceLoadExec(const char *path, char *const argv[]);

/* app_crt.cpp declares it weak and calls it with main's status. */
__attribute__((noreturn)) void catchReturnFromMain(int status);

/* Overridable only for the host test (tests/run-host-test.py). */
#ifndef STORE_PORT
#define STORE_PORT 9095
#endif
#ifndef LOADER_PORT
#define LOADER_PORT 9021
#endif
#ifndef STORE_ELF
#define STORE_ELF "/app0/xeno-store.elf"
#endif

#define STORE_TIMEOUT_MS 5000    /* one request to a store that answers */
#define LOADER_TIMEOUT_MS 15000  /* the loader's own read deadline is 15 s */
#define LOADER_ATTEMPTS 6        /* connect tries, 500 ms apart */
#define POLL_INTERVAL_MS 500
#define POLL_LIMIT_MS 30000

#define LOG_PREFIX "[XENO STORE] launcher: "
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

/* ---- log ------------------------------------------------------------------ */

static void xlog(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void xlog(const char *format, ...) {
  char line[320];
  const size_t prefix = sizeof LOG_PREFIX - 1;
  memcpy(line, LOG_PREFIX, prefix);
  va_list args;
  va_start(args, format);
  int n = vsnprintf(line + prefix, sizeof line - prefix - 1, format, args);
  va_end(args);
  if (n < 0) line[prefix] = '\0';
  size_t used = strlen(line);   /* <= sizeof line - 2: room for "\n" */
  line[used] = '\n';
  line[used + 1] = '\0';
  sceKernelDebugOutText(0, line);
}

/* ---- time ----------------------------------------------------------------- */

static int64_t now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(unsigned ms) { sceKernelUsleep(ms * 1000u); }

/* ---- sockets -------------------------------------------------------------- */

/* A TCP connection to 127.0.0.1:port with send/receive deadlines, or -1 with
 * the connect error in *err (ECONNREFUSED: nothing listens there). */
static int tcp_local(int port, int timeout_ms, int *err) {
  *err = 0;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    *err = errno;
    return -1;
  }
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  int one = 1;   /* a peer that goes away must fail the write, not end the app */
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);

  struct sockaddr_in peer;
  memset(&peer, 0, sizeof peer);
  peer.sin_len = sizeof peer;
  peer.sin_family = AF_INET;
  peer.sin_port = __builtin_bswap16((uint16_t)port);       /* network order */
  peer.sin_addr.s_addr = __builtin_bswap32(INADDR_LOOPBACK);
  if (connect(fd, (const struct sockaddr *)&peer, sizeof peer) != 0) {
    *err = errno;
    close(fd);
    return -1;
  }
  return fd;
}

static int send_all(int fd, const void *data, size_t size) {
  const char *p = data;
  while (size > 0) {
    ssize_t n = send(fd, p, size, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    p += n;
    size -= (size_t)n;
  }
  return 0;
}

/* Close a connection so the peer sees a reset rather than a clean end of
 * stream: the loader must not start a store that was cut short. */
static void abort_connection(int fd) {
  struct linger lg;
  lg.l_onoff = 1;
  lg.l_linger = 0;
  setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
  close(fd);
}

/* ---- 1. ask a running store to open --------------------------------------- */

enum { STORE_DOWN = -1, STORE_FAILED = 0, STORE_OPENED = 1 };

/* HTTP status of the last answer from the store (0 = none or unreadable). */
static int last_status;

/* "HTTP/1.x 200" followed by a space or the end of the line. */
static int status_is_200(const char *line, size_t len) {
  return len >= 12 && memcmp(line, "HTTP/1.", 7) == 0 && line[7] >= '0' && line[7] <= '9' &&
         memcmp(line + 8, " 200", 4) == 0 &&
         (len == 12 || line[12] == ' ' || line[12] == '\r' || line[12] == '\n');
}

/* Printable copy of up to `cap - 1` bytes, CR/LF/control bytes as spaces. */
static void printable(char *out, size_t cap, const char *in, size_t len) {
  size_t i = 0;
  for (; i < len && i + 1 < cap; i++) {
    unsigned char c = (unsigned char)in[i];
    out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : ' ';
  }
  out[i] = '\0';
}

/* POST /api/launcher/open. STORE_OPENED only for an "HTTP/1.x 200" answer;
 * STORE_DOWN when nothing accepts the connection; STORE_FAILED otherwise.
 * `quiet` keeps a refused connection out of the log (the start-up poll). */
static int open_store(int quiet) {
  int err;
  last_status = 0;
  int fd = tcp_local(STORE_PORT, STORE_TIMEOUT_MS, &err);
  if (fd < 0) {
    if (!quiet) xlog("store: nothing answers on 127.0.0.1:%d (errno %d)", STORE_PORT, err);
    return STORE_DOWN;
  }
  static const char request[] =
      "POST /api/launcher/open HTTP/1.0\r\n"
      "Host: 127.0.0.1:" STRINGIFY(STORE_PORT) "\r\n"
      "User-Agent: XENO-STORE-launcher\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: 2\r\n"
      "Connection: close\r\n"
      "\r\n"
      "{}";
  if (send_all(fd, request, sizeof request - 1) != 0) {
    xlog("store: sending POST /api/launcher/open failed (errno %d)", errno);
    close(fd);
    return STORE_DOWN;   /* the connection went away: nothing usable answers */
  }

  /* Read the whole (small) answer so the store's side closes cleanly; keep the
   * first bytes for the status line and the log. */
  char head[256];
  size_t kept = 0, total = 0;
  for (;;) {
    char chunk[512];
    ssize_t n = recv(fd, chunk, sizeof chunk, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      if (n < 0 && total == 0) err = errno;
      break;
    }
    if (kept < sizeof head) {
      size_t take = (size_t)n < sizeof head - kept ? (size_t)n : sizeof head - kept;
      memcpy(head + kept, chunk, take);
      kept += take;
    }
    total += (size_t)n;
    if (total > 64 * 1024) break;   /* not our store: stop reading */
  }
  close(fd);

  if (total == 0) {
    xlog("store: connected to 127.0.0.1:%d but got no answer (errno %d)", STORE_PORT, err);
    return STORE_FAILED;
  }
  size_t line_len = 0;
  while (line_len < kept && head[line_len] != '\r' && head[line_len] != '\n') line_len++;
  char status[64];
  printable(status, sizeof status, head, line_len);
  const char *body = NULL;
  for (size_t i = 0; i + 3 < kept; i++) {
    if (memcmp(head + i, "\r\n\r\n", 4) == 0) {
      body = head + i + 4;
      break;
    }
  }
  char body_text[128];
  printable(body_text, sizeof body_text, body ? body : "", body ? (size_t)(head + kept - body) : 0);
  if (kept >= 12 && memcmp(head, "HTTP/1.", 7) == 0 && head[8] == ' ')
    last_status = (head[9] - '0') * 100 + (head[10] - '0') * 10 + (head[11] - '0');
  if (!status_is_200(head, kept)) {
    xlog("store: answered \"%s\" - not 200, treated as not running", status);
    return STORE_FAILED;
  }
  xlog("store: \"%s\" %s", status, body_text);
  return STORE_OPENED;
}

/* ---- 2. hand the bundled store to the ELF loader -------------------------- */

/* read() that retries when interrupted. */
static ssize_t read_some(int fd, char *buffer, size_t size) {
  for (;;) {
    ssize_t n = read(fd, buffer, size);
    if (n >= 0 || errno != EINTR) return n;
  }
}

static int start_store(void) {
  static char buffer[64 * 1024];   /* .bss: the main thread's stack is not ours to size */

  int in = open(STORE_ELF, O_RDONLY);
  if (in < 0) {
    xlog("cannot open %s (errno %d)", STORE_ELF, errno);
    return -1;
  }
  struct stat info;
  long long size = fstat(in, &info) == 0 ? (long long)info.st_size : -1;
  xlog("store payload %s: %lld bytes", STORE_ELF, size);

  /* The first chunk is checked before the loader is contacted: an x86-64
   * ELF64, as the loader expects. */
  ssize_t n = read_some(in, buffer, sizeof buffer);
  if (n < 0) {
    xlog("reading %s failed (errno %d)", STORE_ELF, errno);
    close(in);
    return -1;
  }
  if (n < 64 || memcmp(buffer, "\177ELF\2\1", 6) != 0 || (unsigned char)buffer[18] != 62 ||
      buffer[19] != 0) {
    xlog("%s is not an x86-64 ELF - not sent", STORE_ELF);
    close(in);
    return -1;
  }

  int out = -1, err = 0;
  for (int attempt = 1; attempt <= LOADER_ATTEMPTS; attempt++) {
    out = tcp_local(LOADER_PORT, LOADER_TIMEOUT_MS, &err);
    if (out >= 0) break;
    xlog("ELF loader 127.0.0.1:%d: connect %d/%d failed (errno %d)", LOADER_PORT, attempt,
         LOADER_ATTEMPTS, err);
    if (attempt < LOADER_ATTEMPTS) sleep_ms(POLL_INTERVAL_MS);
  }
  if (out < 0) {
    xlog("no ELF loader on 127.0.0.1:%d - load your jailbreak first", LOADER_PORT);
    close(in);
    return -1;
  }
  xlog("ELF loader connected, sending the store");

  long long sent = 0;
  int rc = 0;
  while (n > 0) {
    if (send_all(out, buffer, (size_t)n) != 0) {
      xlog("the ELF loader stopped taking the store after %lld bytes (errno %d)", sent, errno);
      rc = -1;
      break;
    }
    sent += n;
    n = read_some(in, buffer, sizeof buffer);
    if (n < 0) {
      xlog("reading %s failed after %lld bytes (errno %d)", STORE_ELF, sent, errno);
      rc = -1;
    }
  }
  close(in);

  if (rc != 0) {
    abort_connection(out);   /* the loader must not start a truncated store */
    return -1;
  }
  /* End of file: the loader starts the store now. The store inherits this
   * connection as its stdout, so do not keep it open. */
  shutdown(out, SHUT_WR);
  close(out);
  xlog("store handed to the ELF loader (%lld bytes)", sent);
  return 0;
}

/* ---- main ----------------------------------------------------------------- */

int main(void) {
  xlog("start");
  int state = open_store(0);
  if (state == STORE_OPENED) {
    xlog("store is running - browser opened");
    return 0;
  }

  if (state == STORE_FAILED) {
    /* Something already serves 127.0.0.1:9095 (e.g. an older store without
     * /api/launcher/open): never start a second store next to it. */
    xlog("a store answers on 127.0.0.1:%d but did not open - not starting a second one (update it in Settings)",
         STORE_PORT);
    return 1;
  }

  xlog("starting the store from %s", STORE_ELF);
  if (start_store() != 0) return 1;

  /* The store binds :9095 about a second after the loader starts it (longer
   * when it hands over to a downloaded update); then ask it to open. */
  xlog("waiting up to %d s for the store on 127.0.0.1:%d", POLL_LIMIT_MS / 1000, STORE_PORT);
  const int64_t started = now_ms();
  for (int attempt = 1;; attempt++) {
    sleep_ms(POLL_INTERVAL_MS);
    state = open_store(attempt % 10 != 0);   /* log a refused poll every 5 s */
    int64_t waited = now_ms() - started;
    if (state == STORE_OPENED) {
      xlog("store started after %lld ms - browser opened", (long long)waited);
      return 0;
    }
    /* 5xx: still starting, ask again; any other answer is final. */
    if (state == STORE_FAILED && !(last_status >= 500 && last_status <= 599)) {
      xlog("the store answered %d - giving up", last_status);
      return 1;
    }
    if (waited >= POLL_LIMIT_MS) break;
  }
  xlog("the store did not open within %d s on 127.0.0.1:%d", POLL_LIMIT_MS / 1000, STORE_PORT);
  return 1;
}

/* Called by app_crt.cpp's _start with main's status, in place of exit(). */
__attribute__((noreturn)) void catchReturnFromMain(int status) {
  xlog("done (status %d) - closing the app", status);
  int rc = sceSystemServiceLoadExec("exit", NULL);
  if (rc == 0) xlog("exit requested - waiting for the system");
  else xlog("exit refused (rc 0x%x) - asking again every 5 s", (unsigned)rc);
  for (int i = 1;; i++) {
    sleep_ms(1000);
    /* A refused request (e.g. while the app sits behind the browser) is retried, so the
     * app closes on its next resume instead of the next tile press resuming a parked app. */
    if (rc != 0 && i % 5 == 0) {
      rc = sceSystemServiceLoadExec("exit", NULL);
      if (rc == 0) xlog("exit requested - waiting for the system");
    }
  }
}
