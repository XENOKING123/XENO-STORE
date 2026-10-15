/* XENO STORE — eboot_main.c: the Games-row app (PPSA99500) launcher.
 *
 * Pressing the XENO STORE tile runs this tiny program as the app's eboot.bin. It is freestanding
 * (raw syscalls, no libc, no .bss — see eboot.x) because an app eboot cannot link the payload SDK.
 *
 *   1. The store already runs (127.0.0.1:9095 answers): ask it to open the browser full-screen on
 *      itself (POST /api/launcher/open, loopback only) and exit. Only "HTTP/1.x 200" counts; any
 *      other answer (an older store without the route says 404) is logged and nothing more happens.
 *   2. Otherwise hand the store that ships inside the app (/app0/xeno-store.elf) to the
 *      jailbreak's ELF loader on 127.0.0.1:9021, wait for it to answer and ask it to open the
 *      browser (the store opens once even when its own "open on start" setting also fires). A
 *      newer version downloaded from Settings takes over by itself at start.
 *   3. No loader either: log it and exit (the tile does nothing until the jailbreak is loaded).
 *
 * Built by app/build_eboot.sh: prospero-clang -c, prospero-lld --static -T eboot.x, make_fself.py.
 */

#define SYS_exit 1
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_socket 97
#define SYS_connect 98
#define SYS_setsockopt 105
#define SYS_nanosleep 240
#define SYS_klog 0x259

#define STORE_PORT 9095
#define LOADER_PORT 9021
#define STORE_ELF "/app0/xeno-store.elf"

static long sys6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
  unsigned long ret;
  char iserror;
  register long r10 __asm__("r10") = a4;
  register long r8 __asm__("r8") = a5;
  register long r9 __asm__("r9") = a6;
  __asm__ __volatile__("syscall"
                       : "=a"(ret), "=@ccc"(iserror), "+r"(r10), "+r"(r8), "+r"(r9)
                       : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                       : "rcx", "r11", "memory");
  return iserror ? -(long)ret : (long)ret;
}
#define SYS3(n, a, b, c) sys6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define SYS5(n, a, b, c, d, e) sys6((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)

static void klog(const char *s) { SYS3(SYS_klog, 7, s, 0); }
static void sleep_ms(long ms) { long ts[2]; ts[0] = ms / 1000; ts[1] = (ms % 1000) * 1000000L; SYS3(SYS_nanosleep, ts, 0, 0); }

/* BSD sockaddr_in: len, family, port (network order), address (network order), zero[8]. */
static int tcp_local(int port, int timeout_s) {
  int fd = (int)SYS3(SYS_socket, 2 /* AF_INET */, 1 /* SOCK_STREAM */, 0);
  if (fd < 0) return -1;
  long tv[2]; tv[0] = timeout_s; tv[1] = 0;
  SYS5(SYS_setsockopt, fd, 0xffff /* SOL_SOCKET */, 0x1005 /* SO_SNDTIMEO */, tv, sizeof tv);
  SYS5(SYS_setsockopt, fd, 0xffff, 0x1006 /* SO_RCVTIMEO */, tv, sizeof tv);
  unsigned char sa[16];
  for (int i = 0; i < 16; i++) sa[i] = 0;
  sa[0] = 16; sa[1] = 2;
  sa[2] = (unsigned char)(port >> 8); sa[3] = (unsigned char)(port & 0xff);
  sa[4] = 127; sa[5] = 0; sa[6] = 0; sa[7] = 1;
  if (SYS3(SYS_connect, fd, sa, 16) < 0) { SYS3(SYS_close, fd, 0, 0); return -1; }
  return fd;
}

static int write_all(int fd, const char *p, long n) {
  while (n > 0) {
    long w = SYS3(SYS_write, fd, p, n);
    if (w <= 0) return -1;
    p += w; n -= w;
  }
  return 0;
}

/* Reads until the status line is complete (or EOF / timeout / full); returns the bytes read. */
static long read_status_line(int fd, char *buf, long cap) {
  long got = 0;
  while (got < cap) {
    long n = SYS3(SYS_read, fd, buf + got, cap - got);
    if (n <= 0) break;
    for (long i = got; i < got + n; i++) if (buf[i] == '\n') return got + n;
    got += n;
  }
  return got;
}

static int is_http_200(const char *r, long n) {
  static const char v[] = "HTTP/1.";
  if (n < 12) return 0;
  for (int i = 0; i < 7; i++) if (r[i] != v[i]) return 0;
  return r[8] == ' ' && r[9] == '2' && r[10] == '0' && r[11] == '0' && (n == 12 || r[12] == ' ' || r[12] == '\r');
}

/* One klog line quoting the store's status line (printable bytes only; n >= 12). */
static void klog_refused(const char *r, long n) {
  static const char head[] = "[XENO STORE] the store on 127.0.0.1:9095 answered \"";
  static const char tail[] = "\" - browser not opened\n";
  static const char tail404[] = "\" - browser not opened (the running store predates /api/launcher/open, update it in Settings)\n";
  const char *t = r[9] == '4' && r[10] == '0' && r[11] == '4' ? tail404 : tail;
  char msg[sizeof head + 64 + sizeof tail404];
  long at = 0;
  for (long i = 0; head[i]; i++) msg[at++] = head[i];
  for (long i = 0; i < n && i < 64 && r[i] >= 0x20 && r[i] < 0x7f; i++) msg[at++] = r[i];
  for (long i = 0; t[i]; i++) msg[at++] = t[i];
  msg[at] = 0;
  klog(msg);
}

/* 0: the store answered 200 (browser opened). 1: a store answers on :9095 but not with a 200
 * (logged; never start a second store next to it). -1: no store there. */
static int open_store(void) {
  int fd = tcp_local(STORE_PORT, 5);
  if (fd < 0) return -1;
  static const char req[] = "POST /api/launcher/open HTTP/1.0\r\nHost: 127.0.0.1:9095\r\n"
                            "Content-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
  int rc = write_all(fd, req, (long)sizeof req - 1);
  char reply[512];
  long n = rc == 0 ? read_status_line(fd, reply, sizeof reply) : 0;
  SYS3(SYS_close, fd, 0, 0);
  if (rc != 0) return -1;
  if (n < 12) { klog("[XENO STORE] the store on 127.0.0.1:9095 did not answer within 5 s (busy?) - the browser may still open\n"); return 1; }
  if (is_http_200(reply, n)) return 0;
  klog_refused(reply, n);
  return 1;
}

static int start_store(void) {
  int in = (int)SYS3(SYS_open, STORE_ELF, 0 /* O_RDONLY */, 0);
  if (in < 0) { klog("[XENO STORE] cannot open " STORE_ELF "\n"); return -1; }
  int out = -1;
  for (int i = 0; i < 6 && out < 0; i++) { out = tcp_local(LOADER_PORT, 15); if (out < 0) sleep_ms(500); }
  if (out < 0) { klog("[XENO STORE] no ELF loader on 127.0.0.1:9021 - load your jailbreak first\n"); SYS3(SYS_close, in, 0, 0); return -1; }
  char buf[16 * 1024];   /* small: the stack of an app's main thread is not ours to size */
  long n, total = 0;
  int rc = 0;
  while ((n = SYS3(SYS_read, in, buf, sizeof buf)) > 0) {
    if (write_all(out, buf, n) != 0) { rc = -1; break; }
    total += n;
  }
  if (n < 0 || total == 0) rc = -1;
  SYS3(SYS_close, out, 0, 0);
  SYS3(SYS_close, in, 0, 0);
  klog(rc == 0 ? "[XENO STORE] store handed to the ELF loader\n" : "[XENO STORE] sending the store failed\n");
  return rc;
}

int main(void) {
  klog("[XENO STORE] launcher\n");
  int st = open_store();
  if (st == 0) { klog("[XENO STORE] store is running - browser opened\n"); return 0; }
  if (st > 0) return 1;
  if (start_store() != 0) return 1;
  /* The store binds :9095 about a second after the loader starts it (longer when it hands over to
   * a downloaded update); then ask it to open the browser. */
  for (int i = 0; i < 60; i++) {
    sleep_ms(500);
    st = open_store();
    if (st == 0) { klog("[XENO STORE] store started - browser opened\n"); return 0; }
    if (st > 0) return 1;
  }
  klog("[XENO STORE] the store did not answer on 127.0.0.1:9095\n");
  return 1;
}

void _start(void) {
  SYS3(SYS_exit, main(), 0, 0);
  __builtin_trap();
}
