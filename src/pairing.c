/* pairing.c — see pairing.h. */
#include "pairing.h"
#include "json.h"
#include "log.h"
#include "sha256.h"
#include "sys.h"
#include "util.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAIR_MAX_TOKENS 16
#define PAIR_TOKEN_HEX 64
#define PAIR_ID_HEX 16
#define PAIR_NAME_MAX 48
#define PAIR_SEEN_SAVE_S 60   /* a device's last seen is written to disk at most this often */
#define PAIR_SHOW_COOLDOWN_S 30
#define PAIR_LOCKOUT_S 60
#define PAIR_MAX_STRIKES 5

/* One paired device. */
typedef struct pair_dev {
  char token[PAIR_TOKEN_HEX + 1];
  char name[PAIR_NAME_MAX];   /* from its User-Agent, "" when unknown */
  time_t first, last;         /* paired (or first seen) and last request; 0 = unknown */
  time_t saved;               /* when touch last wrote its last seen (memory only) */
} pair_dev;

static pthread_mutex_t pair_mu = PTHREAD_MUTEX_INITIALIZER;
static char pair_dir[512];
static char pair_path[560];
static char pair_code_val[7];                         /* 6 digits + NUL */
static char pair_local[PAIR_TOKEN_HEX + 1];           /* the console's own session, "" until asked for */
static pair_dev pair_devs[PAIR_MAX_TOKENS];
static int pair_ntokens;
static time_t pair_locked_until;
static int pair_strikes;
static time_t pair_show_after;
static int pair_ready;

/* Constant-time equality over the first n bytes (no early exit on mismatch). */
static int ct_eq(const char *a, const char *b, size_t n) {
  unsigned char d = 0;
  for (size_t i = 0; i < n; i++) d |= (unsigned char)a[i] ^ (unsigned char)b[i];
  return d == 0;
}

/* Fills out[0..5] with six decimal digits from the strongest source available. */
static void gen_code(char out[7]) {
  uint8_t pool[6];
  size_t have = 0;
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) { ssize_t r = read(fd, pool, sizeof pool); if (r > 0) have = (size_t)r; close(fd); }
  for (int i = 0; i < 6; i++) {
    uint32_t r;
    if ((size_t)i < have) r = pool[i];
    else { char hx[3]; util_rand_hex(hx, 2); r = (uint32_t)strtoul(hx, NULL, 16); }
    out[i] = (char)('0' + (r % 10));
  }
  out[6] = 0;
}

static int is_hex64(const char *s) {
  if (!s) return 0;
  size_t n = 0;
  for (; s[n]; n++) {
    char c = s[n];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
  }
  return n == PAIR_TOKEN_HEX;
}

/* The id a device is listed and unpaired by: the first 16 hex of SHA-256(token). */
static void device_id(const char *token, char out[PAIR_ID_HEX + 1]) {
  char hex[65];
  sha256_hex(token, PAIR_TOKEN_HEX, hex);
  memcpy(out, hex, PAIR_ID_HEX);
  out[PAIR_ID_HEX] = 0;
}

/* Caller holds pair_mu. The device unused the longest: the oldest last seen,
 * then the oldest pairing, then the first listed. */
static int oldest_locked(void) {
  int o = 0;
  for (int i = 1; i < pair_ntokens; i++) {
    const pair_dev *a = &pair_devs[i], *b = &pair_devs[o];
    if (a->last < b->last || (a->last == b->last && a->first < b->first)) o = i;
  }
  return o;
}

/* Caller holds pair_mu. Writes <data>/pairing.json atomically. 0 or -1. */
static int persist_locked(void) {
  jw w;
  jw_init(&w);
  jw_obj_begin(&w);
  jw_kv_int(&w, "v", 2);
  jw_kv_str(&w, "local", pair_local);
  jw_key(&w, "devices");
  jw_arr_begin(&w);
  for (int i = 0; i < pair_ntokens; i++) {
    const pair_dev *d = &pair_devs[i];
    jw_obj_begin(&w);
    jw_kv_str(&w, "token", d->token);
    jw_kv_str(&w, "name", d->name);
    jw_kv_i64(&w, "first", (int64_t)d->first);
    jw_kv_i64(&w, "last", (int64_t)d->last);
    jw_obj_end(&w);
  }
  jw_arr_end(&w);
  /* An older store reads only this list, keeps its first 16 and hands its
   * first entry out as the console's session: next to the console's session
   * the device unused the longest is left out, not the newest. */
  int skip = pair_local[0] && pair_ntokens >= PAIR_MAX_TOKENS ? oldest_locked() : -1;
  jw_key(&w, "tokens");
  jw_arr_begin(&w);
  if (pair_local[0]) jw_str(&w, pair_local);
  for (int i = 0; i < pair_ntokens; i++)
    if (i != skip) jw_str(&w, pair_devs[i].token);
  jw_arr_end(&w);
  jw_obj_end(&w);
  size_t len = 0;
  char *txt = jw_detach(&w, &len);
  if (!txt) return -1;
  int rc = util_write_file_atomic(pair_path, txt, len);
  free(txt);
  return rc;
}

/* Caller holds pair_mu. Appends a device (a free slot is the caller's job). */
static pair_dev *add_locked(const char *token, const char *name, time_t first, time_t last) {
  pair_dev *p = &pair_devs[pair_ntokens++];
  memset(p, 0, sizeof *p);
  memcpy(p->token, token, PAIR_TOKEN_HEX);
  p->token[PAIR_TOKEN_HEX] = 0;
  xstrlcpy(p->name, name ? name : "", sizeof p->name);
  p->first = first;
  p->last = last;
  p->saved = last;
  return p;
}

static void load_locked(void) {
  pair_ntokens = 0;
  pair_local[0] = 0;
  size_t len = 0;
  char *txt = util_read_file(pair_path, &len, 1u << 20);
  if (!txt) return;
  jdoc *d = json_parse(txt, len, 4096, 8);
  free(txt);
  if (!d) return;
  jv root = json_root(d), devs = jv_get(root, "devices");
  if (jv_type(devs) == JV_ARR) {
    const char *l = jv_get_str(root, "local", "");
    if (is_hex64(l)) xstrlcpy(pair_local, l, sizeof pair_local);
    JV_FOREACH(e, devs) {
      const char *s = jv_get_str(e, "token", "");
      if (is_hex64(s) && pair_ntokens < PAIR_MAX_TOKENS)
        add_locked(s, jv_get_str(e, "name", ""), (time_t)jv_i64(jv_get(e, "first"), 0), (time_t)jv_i64(jv_get(e, "last"), 0));
    }
  } else {
    /* A file from before names and times were kept. Its first token is what
     * /api/session and /api/v1/session handed out, but that was a phone's
     * whenever one paired before the console first asked: every token is
     * listed (and can be unpaired) then, and the console's session is made
     * afresh (the TV app asks for it again once its token stops working). */
    JV_FOREACH(t, jv_get(root, "tokens")) {
      const char *s = jv_str(t, "");
      if (is_hex64(s) && pair_ntokens < PAIR_MAX_TOKENS) add_locked(s, "", 0, 0);
    }
  }
  json_free(d);
}

/* Caller holds pair_mu. */
static void remove_locked(int i) {
  memmove(&pair_devs[i], &pair_devs[i + 1], (size_t)(pair_ntokens - i - 1) * sizeof pair_devs[0]);
  pair_ntokens--;
}

static int has(const char *s, const char *needle) { return strstr(s, needle) != NULL; }

void pairing_device_name(const char *ua, char *out, size_t cap) {
  if (!out || !cap) return;
  out[0] = 0;
  if (!ua || !*ua) return;
  /* XENO STORE Desktop names itself; its releases before that sent Go's default. */
  if (has(ua, "XENO-STORE-Desktop") || util_starts_with(ua, "Go-http-client/")) { xstrlcpy(out, "XENO STORE Desktop", cap); return; }
  if (has(ua, "PlayStation 5")) { xstrlcpy(out, "PS5 browser", cap); return; }
  /* iPhones say "like Mac OS X" and Android says Linux: the specific ones first. */
  const char *os = has(ua, "iPhone") ? "iPhone" : has(ua, "iPad") ? "iPad" : has(ua, "Android") ? "Android"
                   : has(ua, "CrOS") ? "Chromebook" : has(ua, "Windows") ? "Windows" : has(ua, "Macintosh") ? "Mac"
                   : has(ua, "Linux") ? "Linux" : "";
  /* Edge, Opera and Samsung Internet also say Chrome, and Chrome says Safari. */
  const char *br = has(ua, "Edg/") || has(ua, "EdgA/") || has(ua, "EdgiOS/") || has(ua, "Edge/") ? "Edge"
                   : has(ua, "OPR/") || has(ua, "OPiOS/") ? "Opera"
                   : has(ua, "SamsungBrowser/") ? "Samsung Internet"
                   : has(ua, "Firefox/") || has(ua, "FxiOS/") ? "Firefox"
                   : has(ua, "Chrome/") || has(ua, "CriOS/") ? "Chrome"
                   : has(ua, "Safari/") ? "Safari" : "";
  xsnprintf(out, cap, "%s%s%s", os, os[0] && br[0] ? " " : "", br);
}

void pairing_init(const char *data_dir) {
  pthread_mutex_lock(&pair_mu);
  xstrlcpy(pair_dir, data_dir ? data_dir : ".", sizeof pair_dir);
  xsnprintf(pair_path, sizeof pair_path, "%s/pairing.json", pair_dir);
  load_locked();
  gen_code(pair_code_val);
  pair_locked_until = 0;
  pair_strikes = 0;
  pair_show_after = 0;
  pair_ready = 1;
  int n = pair_ntokens;
  pthread_mutex_unlock(&pair_mu);
  LOGI("pairing: code ready (%d paired device%s)", n, n == 1 ? "" : "s");
}

void pairing_announce(void) { sys_toast("XENO STORE pairing code: %s", pair_code_val); }

const char *pairing_code(void) { return pair_code_val; }

int pairing_show_cooldown(void) {
  pthread_mutex_lock(&pair_mu);
  time_t now = util_now();
  int left = pair_show_after > now ? (int)(pair_show_after - now) : 0;
  pthread_mutex_unlock(&pair_mu);
  return left;
}

int pairing_show(int *retry_after) {
  pthread_mutex_lock(&pair_mu);
  time_t now = util_now();
  if (pair_show_after > now) {
    int left = (int)(pair_show_after - now);
    pthread_mutex_unlock(&pair_mu);
    if (retry_after) *retry_after = left;
    return 1;
  }
  /* Failed shows are throttled too, so repeated requests cannot flood. */
  pair_show_after = now + PAIR_SHOW_COOLDOWN_S;
  char code[7];
  memcpy(code, pair_code_val, sizeof code);
  pthread_mutex_unlock(&pair_mu);
  if (retry_after) *retry_after = PAIR_SHOW_COOLDOWN_S;
  sys_toast("XENO STORE pairing code: %s", code);
  return 0;
}

int pairing_redeem(const char *code, const char *name, char *token_out, size_t cap, char *replaced, size_t rcap, int *retry_after) {
  if (token_out && cap) token_out[0] = 0;
  if (replaced && rcap) replaced[0] = 0;
  pthread_mutex_lock(&pair_mu);
  time_t now = util_now();
  if (pair_locked_until > now) {
    if (retry_after) *retry_after = (int)(pair_locked_until - now);
    pthread_mutex_unlock(&pair_mu);
    return -429;
  }
  if (!code || strlen(code) != 6 || !ct_eq(code, pair_code_val, 6)) {
    if (++pair_strikes >= PAIR_MAX_STRIKES) { pair_locked_until = now + PAIR_LOCKOUT_S; pair_strikes = 0; }
    pthread_mutex_unlock(&pair_mu);
    return -403;
  }
  pair_dev keep[PAIR_MAX_TOKENS];
  int keep_n = pair_ntokens;
  memcpy(keep, pair_devs, sizeof keep);
  char gone[PAIR_NAME_MAX] = "";
  int full = pair_ntokens >= PAIR_MAX_TOKENS;
  if (full) {   /* all slots taken: the device unused the longest makes room */
    int o = oldest_locked();
    xstrlcpy(gone, pair_devs[o].name, sizeof gone);
    remove_locked(o);
  }
  char token[PAIR_TOKEN_HEX + 1];
  util_rand_hex(token, PAIR_TOKEN_HEX);
  add_locked(token, name, now, now);
  pair_strikes = 0;
  if (persist_locked() != 0) { memcpy(pair_devs, keep, sizeof keep); pair_ntokens = keep_n; pthread_mutex_unlock(&pair_mu); return -503; }
  pthread_mutex_unlock(&pair_mu);
  if (full) LOGI("pairing: all %d slots were in use, replaced %s (unused the longest)", PAIR_MAX_TOKENS, gone[0] ? gone : "a device without a name");
  if (token_out && cap) xstrlcpy(token_out, token, cap);
  if (full && replaced && rcap) xstrlcpy(replaced, gone, rcap);
  return full;
}

int pairing_token_valid(const char *token) {
  if (!is_hex64(token)) return 0;
  int ok = 0;
  pthread_mutex_lock(&pair_mu);
  if (pair_local[0] && ct_eq(token, pair_local, PAIR_TOKEN_HEX)) ok = 1;
  for (int i = 0; i < pair_ntokens; i++) if (ct_eq(token, pair_devs[i].token, PAIR_TOKEN_HEX)) ok = 1;
  pthread_mutex_unlock(&pair_mu);
  return ok;
}

void pairing_touch(const char *token, const char *user_agent) {
  if (!is_hex64(token)) return;
  pthread_mutex_lock(&pair_mu);
  for (int i = 0; i < pair_ntokens; i++) {
    pair_dev *d = &pair_devs[i];
    if (!ct_eq(token, d->token, PAIR_TOKEN_HEX)) continue;
    time_t now = util_now();
    int save = now - d->saved >= PAIR_SEEN_SAVE_S;
    if (!d->first) { d->first = now; save = 1; }
    if (!d->name[0]) { pairing_device_name(user_agent, d->name, sizeof d->name); if (d->name[0]) save = 1; }
    d->last = now;
    if (save) {
      d->saved = now;   /* also on a failed write: the next try waits its minute too */
      if (persist_locked() != 0) LOGW("pairing: could not save when %s was last seen", d->name[0] ? d->name : "a device");
    }
    break;
  }
  pthread_mutex_unlock(&pair_mu);
}

void pairing_devices_json(jw *w, const char *self) {
  int mine = is_hex64(self);
  pthread_mutex_lock(&pair_mu);
  jw_key(w, "devices");
  jw_arr_begin(w);
  for (int i = 0; i < pair_ntokens; i++) {
    const pair_dev *d = &pair_devs[i];
    char id[PAIR_ID_HEX + 1];
    device_id(d->token, id);
    jw_obj_begin(w);
    jw_kv_str(w, "id", id);
    jw_kv_str(w, "name", d->name);
    jw_kv_i64(w, "firstSeen", (int64_t)d->first);
    jw_kv_i64(w, "lastSeen", (int64_t)d->last);
    jw_kv_bool(w, "current", mine && ct_eq(self, d->token, PAIR_TOKEN_HEX));
    jw_obj_end(w);
  }
  jw_arr_end(w);
  pthread_mutex_unlock(&pair_mu);
  jw_kv_int(w, "max", PAIR_MAX_TOKENS);
}

int pairing_unpair(const char *id) {
  if (!id || strlen(id) != PAIR_ID_HEX) return -404;
  pthread_mutex_lock(&pair_mu);
  int at = -1;
  for (int i = 0; i < pair_ntokens && at < 0; i++) {
    char did[PAIR_ID_HEX + 1];
    device_id(pair_devs[i].token, did);
    if (strcmp(did, id) == 0) at = i;
  }
  if (at < 0) { pthread_mutex_unlock(&pair_mu); return -404; }
  pair_dev keep[PAIR_MAX_TOKENS];
  int keep_n = pair_ntokens;
  memcpy(keep, pair_devs, sizeof keep);
  char name[PAIR_NAME_MAX];
  xstrlcpy(name, pair_devs[at].name[0] ? pair_devs[at].name : "a device without a name", sizeof name);
  remove_locked(at);
  if (persist_locked() != 0) { memcpy(pair_devs, keep, sizeof keep); pair_ntokens = keep_n; pthread_mutex_unlock(&pair_mu); return -503; }
  pthread_mutex_unlock(&pair_mu);
  LOGI("pairing: unpaired %s", name);
  return 0;
}

int pairing_unpair_others(const char *self) {
  int mine = is_hex64(self);
  pthread_mutex_lock(&pair_mu);
  pair_dev keep[PAIR_MAX_TOKENS];
  int keep_n = pair_ntokens;
  memcpy(keep, pair_devs, sizeof keep);
  int n = 0;
  for (int i = 0; i < keep_n; i++)
    if (mine && ct_eq(self, keep[i].token, PAIR_TOKEN_HEX)) pair_devs[n++] = keep[i];
  pair_ntokens = n;
  int gone = keep_n - n;
  if (gone && persist_locked() != 0) { memcpy(pair_devs, keep, sizeof keep); pair_ntokens = keep_n; pthread_mutex_unlock(&pair_mu); return -503; }
  pthread_mutex_unlock(&pair_mu);
  if (gone) LOGI("pairing: unpaired %d other device%s", gone, gone == 1 ? "" : "s");
  return gone;
}

int pairing_ensure_token(char *token_out, size_t cap) {
  if (!token_out || !cap) return -1;
  token_out[0] = 0;
  pthread_mutex_lock(&pair_mu);
  if (!pair_local[0]) {
    util_rand_hex(pair_local, PAIR_TOKEN_HEX);
    if (persist_locked() != 0) { pair_local[0] = 0; pthread_mutex_unlock(&pair_mu); return -1; }
  }
  xstrlcpy(token_out, pair_local, cap);
  pthread_mutex_unlock(&pair_mu);
  return 0;
}
