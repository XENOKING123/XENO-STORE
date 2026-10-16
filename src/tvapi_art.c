/*
 * tvapi_art.c — GET /api/v1/art/{gameId}/{cover|hero} and
 * /api/v1/art/{gameId}/shot/{n}: game artwork and screenshots for the native
 * TV app, fetched off the HTTP workers and kept in a bounded memory cache
 * (PORT-SPEC 1.7, R8, R9; map-catalog-art §8).
 *
 * Copyright (C) 2026 XENO STORE contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The app decodes the pictures itself (WebP through libwebp, scaled while it
 * decodes; PNG and JPEG through stb, fully decoded into its fixed 128 MiB
 * heap first), so the original bytes pass through untouched: no resize, no
 * transcode. This file picks a usable URL from our own catalogue, fetches it
 * on two tv-art workers and refuses what the app cannot draw or would spend
 * too much memory on.
 *
 *  - The art index maps every catalogue game id (R9: any catalogue game, not
 *    only the TV-visible ones) to at most two cover and two hero candidate
 *    URLs and its screenshot URLs (TV_ART_MAX_SHOTS, one candidate each)
 *    that already passed the URL policy. A worker rebuilds it whenever
 *    the catalogue changes; the request path reads only the index and the
 *    cache under art_mu (a leaf lock), never catalog_mu, the network or a
 *    drive, so the first answer is immediate.
 *  - The first request for gameId#kind queues the key and answers 202. A
 *    worker takes the newest queued key, fetches its candidates in order,
 *    probes each body without decoding it and keeps the first one the app
 *    can draw. Later requests answer 200 with a copy of the bytes, 502 for
 *    120 s after a failed fetch, and 404 when no candidate is usable.
 *
 * Ported from Orbit Store backend/art.c (GPL-3.0-or-later): the 202-then-200
 * protocol, the two workers, the 256-entry cache with least-recently-used
 * eviction, the 64-key queue, the 12 MiB fetch cap with three redirects, the
 * byte sniff (the server's Content-Type is never trusted), the 120 s failure
 * window, the identifier check and the three reply sentences. Our changes:
 * the web UI's candidate rule (_artOk), a 24 MiB budget, a newest-first
 * queue, header probes with the pixel caps and the JPEG frame check, the
 * per-URL-and-kind refusal memory and the art index.
 */
#include "tvapi.h"

#include "catalog.h"
#include "config.h"
#include "http_client.h"
#include "log.h"
#include "util.h"
#include "xthread.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

/* ---- limits (PORT-SPEC 1.7) ------------------------------------------------ */
#define ART_ENTRIES 256                 /* cache slots, every state */
#define ART_BUDGET ((size_t)24 << 20)   /* bytes held by READY entries */
#define ART_QUEUE 64                    /* keys waiting for a worker */
#define ART_WORKERS 2                   /* downloads may hold 20 of the PS5's 24 request slots */
#define ART_MAX_BYTES ((size_t)12 << 20)
#define ART_REDIRECTS 3
#define ART_TIMEOUT_MS 10000            /* connect, and every receive */
#define ART_TOTAL_MS 30000              /* one whole transfer, so a slow drip cannot hold a worker */
#define ART_RETRY_FAILED_MS 120000      /* 502 window after a failed fetch */
#define ART_STALE_MS 10000              /* a queued key the app stopped asking for (it re-asks every 0.6 s) */
#define ART_REV_CHECK_MS 2000           /* catalogue swap detection */
#define ART_MAX_SIDE 8192               /* the app refuses larger sides */
#define ART_COVER_PIXELS 1048576LL      /* PNG/JPEG: stb decodes the full picture first */
#define ART_HERO_PIXELS 3686400LL       /* 2560 x 1440 */
#define ART_SHOT_PIXELS ART_HERO_PIXELS /* a screenshot: the hero cap, any shape */
#define ART_COVER_WIDTH 512             /* image.api.playstation.com w= */
#define ART_HERO_WIDTH 1920
#define ART_SHOT_WIDTH 1920
#define ART_ID_MAX 95                   /* route captures hold at most 95 bytes */
#define ART_KEY 104                     /* "<id>#cover", "<id>#shot9" */
#define ART_URL_MAX 2048
#define ART_REFUSED_SLOTS 8192          /* open addressing, at most half used */
/* Never "image/" + "*": content-negotiating CDNs answer that with AVIF. */
#define ART_ACCEPT "Accept: image/webp,image/png,image/jpeg;q=0.9\r\n"

/* Sentences: the app has translation keys for exactly these. */
#define ART_MSG_UNKNOWN "Unknown artwork."
#define ART_MSG_FAILED "This artwork could not be loaded."
#define ART_MSG_BUSY "Artwork is busy. Try again shortly."

enum { ART_EMPTY = 0, ART_PENDING, ART_READY, ART_FAILED, ART_NONE };
enum { KIND_COVER = 0, KIND_HERO, KIND_SHOT };   /* what a picture is for: its caps and its width */
enum { FETCH_OK = 0, FETCH_REFUSED, FETCH_FAILED };
enum { PROBE_MORE = 0, PROBE_OK, PROBE_BAD };

/* One cache slot, keyed by "gameId#kind". */
typedef struct art_entry {
  char key[ART_KEY];
  uint64_t khash;
  int state;
  int queued;            /* waiting in the queue (PENDING and not yet taken by a worker) */
  unsigned char *data;   /* READY: the original bytes */
  size_t size;
  char mime[12];         /* image/webp | image/png | image/jpeg */
  int64_t used_ms;       /* last answer (least recently used goes first) */
  int64_t asked_ms;      /* last request while queued */
  int64_t failed_ms;     /* FAILED: when */
} art_entry;

/* The art index: candidate URLs per game id, rebuilt per catalogue. */
typedef struct art_game {
  uint64_t hash;
  uint32_t id;           /* offsets into pool; 0 = none */
  uint32_t url[4];       /* cover candidates [0..1], hero candidates [2..3] */
  uint32_t shot0;        /* its screenshots: shots[shot0 .. shot0 + nshots) */
  uint8_t nshots;
} art_game;

typedef struct art_idx {
  const void *doc;       /* identity of the catalogue it was built from */
  int64_t revision;
  art_game *games;
  uint32_t ngames, gcap;
  uint32_t *slots;       /* game index + 1, 0 = empty */
  uint32_t nslots;
  char *pool;
  size_t plen, pcap;
  uint32_t *shots;       /* screenshot URLs as pool offsets, 0 = outside the URL policy */
  uint32_t nshots, shcap;
  int oom;
  int covers, heroes;    /* games with a cover / hero candidate */
  int shot_games;        /* games with a usable screenshot */
} art_idx;

static pthread_mutex_t art_mu = PTHREAD_MUTEX_INITIALIZER;   /* leaf lock: guards A */
static pthread_cond_t art_cv;                                  /* initialised by tvart_start() */
static int art_cv_mono;

static struct {
  int started;           /* workers running */
  art_idx *idx;          /* NULL until the first build */
  int building;
  int64_t check_due;
  unsigned gen;          /* +1 when the cache is cleared for a new catalogue */
  art_entry e[ART_ENTRIES];
  int queue[ART_QUEUE];  /* entry indexes, oldest first; workers take the last */
  int nqueue;
  size_t bytes;          /* held by READY entries */
  int nready;
  uint64_t refused[ART_REFUSED_SLOTS];   /* refusal_key()s of the URLs refused for a kind */
  int nrefused;
  int refused_full_logged;
  unsigned long fetched, refusals, failures, evictions, busy;
} A;

/* Declared for tvapi_diag_json (tvapi.c) to call; not otherwise used here. */
void tvart_diag_json(jw *w);

/* ---- small helpers ------------------------------------------------------------ */

static uint64_t fnv1a(const char *s) {
  uint64_t h = 1469598103934665603ULL;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    h ^= *p;
    h *= 1099511628211ULL;
  }
  return h ? h : 1;   /* 0 marks an empty refusal slot */
}

/* [A-Za-z0-9._-]{1,95}, not "." or "..". Ported from Orbit Store
 * backend/art.c art_identifier (GPL-3.0-or-later); 95 instead of 63 because
 * our game ids run to 78 characters. */
static int art_id_ok(const char *id) {
  size_t n = strlen(id);
  if (n == 0 || n > ART_ID_MAX) return 0;
  for (const char *p = id; *p; p++) {
    char c = *p;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
      return 0;
  }
  return strcmp(id, ".") != 0 && strcmp(id, "..") != 0;
}

static unsigned be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static unsigned le16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t le24(const unsigned char *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }
static uint32_t le32(const unsigned char *p) { return le24(p) | (uint32_t)p[3] << 24; }

/* ---- URL policy ------------------------------------------------------------------
 * Only the catalogue's own URLs for that game are ever fetched (the client
 * never supplies one), so there is no host allow-list: https only, no
 * userinfo, no loopback/private address literal, no .avif/.gif. */

/* Host build: tests serve art over plain http from this one origin. */
static int fixture_origin(const xurl *u) {
#ifdef XENO_HOST
  const char *f = getenv("XENO_HOST_ART_FIXTURE");
  if (!f || !*f || strcmp(u->scheme, "http") != 0) return 0;
  char hp[300];
  xsnprintf(hp, sizeof hp, "%s:%d", u->host, u->port);
  return util_strieq(hp, f);
#else
  (void)u;
  return 0;
#endif
}

/* 0/8, 10/8, 100.64/10, 127/8, 169.254/16, 172.16/12, 192.0.0/24,
 * 192.168/16, 198.18/15, multicast and reserved (224/3). */
static int ipv4_private(uint32_t a) {
  unsigned b0 = a >> 24, b1 = (a >> 16) & 0xFF, b2 = (a >> 8) & 0xFF;
  return b0 == 0 || b0 == 10 || b0 == 127 || (b0 == 100 && (b1 & 0xC0) == 64) || (b0 == 169 && b1 == 254) ||
         (b0 == 172 && (b1 & 0xF0) == 16) || (b0 == 192 && b1 == 0 && b2 == 0) || (b0 == 192 && b1 == 168) ||
         (b0 == 198 && (b1 & 0xFE) == 18) || b0 >= 224;
}

/* 1 for a loopback, private, link-local or otherwise non-public address
 * literal, and for names that only resolve on the local network. */
static int host_blocked(const char *host) {
  char h[256];
  xstrlcpy(h, host, sizeof h);
  size_t n = strlen(h);
  if (n && h[n - 1] == '.') h[--n] = 0;
  if (n == 0) return 1;
  if (h[0] == '[') {
    if (n < 3 || h[n - 1] != ']') return 1;
    h[n - 1] = 0;
    struct in6_addr a6;
    if (inet_pton(AF_INET6, h + 1, &a6) != 1) return 1;
    return (a6.s6_addr[0] & 0xE0) != 0x20;   /* global unicast 2000::/3 only */
  }
  const char *last = strrchr(h, '.');
  last = last ? last + 1 : h;
  if ((last[0] >= '0' && last[0] <= '9') || util_istarts_with(h, "0x")) {
    struct in_addr a4;
    if (inet_pton(AF_INET, h, &a4) != 1) return 1;   /* octal, hex and short forms */
    return ipv4_private(ntohl(a4.s_addr));
  }
  static const char *const LOCAL_ONLY[] = {"localhost", "local", "home.arpa", "internal", "lan"};
  for (size_t i = 0; i < XS_ARRAY_LEN(LOCAL_ONLY); i++)
    if (util_host_matches(h, LOCAL_ONLY[i])) return 1;
  return 0;
}

/* The web UI's sharpArt (web/src/js/00-util.js): drop the first ?w=N / &w=N
 * (and an '&' right after it) and a trailing '?' or '&', then append
 * w=<width>. 0, or -1 when it does not fit. */
static int sharp_art(char *url, size_t cap, int width) {
  for (char *p = url; (p = strstr(p, "w=")) != NULL; p++) {
    if (p == url || (p[-1] != '?' && p[-1] != '&') || p[2] < '0' || p[2] > '9') continue;
    char *d = p + 2;
    while (*d >= '0' && *d <= '9') d++;
    if (*d == '&') d++;
    memmove(p, d, strlen(d) + 1);
    break;
  }
  size_t n = strlen(url);
  if (n && (url[n - 1] == '?' || url[n - 1] == '&')) url[--n] = 0;
  char sep = strchr(url, '?') ? '&' : '?';
  int w = xsnprintf(url + n, cap - n, "%cw=%d", sep, width);
  return n + (size_t)w + 1 < cap ? 0 : -1;
}

/* Applies the URL policy to a catalogue URL and writes the URL to fetch
 * (image.api.playstation.com sized for the kind). With final = 1 it checks
 * where the redirects ended instead: no extension rule, no rewrite.
 * Returns 1 when usable. */
static int art_url_ok(const char *url, int kind, int final, char *out, size_t cap) {
  if (!url || !*url || strlen(url) >= ART_URL_MAX) return 0;
  for (const unsigned char *p = (const unsigned char *)url; *p; p++)
    if (*p <= 0x20 || *p == 0x7F) return 0;   /* never a raw space or CR/LF into a request line */
  xurl u;
  if (util_url_parse(url, &u) != 0 || u.has_userinfo) return 0;
  if (!fixture_origin(&u) && (strcmp(u.scheme, "https") != 0 || host_blocked(u.host))) return 0;
  if (final) return 1;
  char path[ART_URL_MAX];
  xstrlcpy(path, u.path, sizeof path);
  char *q = strchr(path, '?');
  if (q) *q = 0;
  if (util_iends_with(path, ".avif") || util_iends_with(path, ".gif")) return 0;   /* the app has no decoder */
  if (xstrlcpy(out, url, cap) >= cap) return 0;
  int width = kind == KIND_COVER ? ART_COVER_WIDTH : kind == KIND_HERO ? ART_HERO_WIDTH : ART_SHOT_WIDTH;
  if (strcmp(u.host, "image.api.playstation.com") == 0 && sharp_art(out, cap, width) != 0) return 0;
  return 1;
}

/* ---- the probe: format and size from the first bytes, without decoding ----------
 * Sniff: the same three checks as the app (APP image.cpp) and Orbit Store
 * backend/art.c; the server's Content-Type is ignored. */

typedef struct art_probe {
  const char *mime;
  int64_t w, h;
} art_probe;

static const char *kind_name(int kind) { return kind == KIND_COVER ? "cover" : kind == KIND_HERO ? "hero" : "screenshot"; }

/* The sides the app can draw for this kind: PNG and JPEG (capped) within the
 * kind's pixel cap, and a hero wide enough for a background. */
static int probe_dims(art_probe *pr, int64_t w, int64_t h, int kind, int capped, char *why, size_t cap) {
  pr->w = w;
  pr->h = h;
  if (w <= 0 || h <= 0) { xsnprintf(why, cap, "%s with no size", pr->mime); return PROBE_BAD; }
  if (w > ART_MAX_SIDE || h > ART_MAX_SIDE) {
    xsnprintf(why, cap, "%s %lldx%lld: a side is over %d px", pr->mime, (long long)w, (long long)h, ART_MAX_SIDE);
    return PROBE_BAD;
  }
  int64_t limit = kind == KIND_COVER ? ART_COVER_PIXELS : kind == KIND_HERO ? ART_HERO_PIXELS : ART_SHOT_PIXELS;
  if (capped && w * h > limit) {
    xsnprintf(why, cap, "%s %lldx%lld is over the %lld-pixel %s cap", pr->mime, (long long)w, (long long)h, (long long)limit,
              kind_name(kind));
    return PROBE_BAD;
  }
  if (kind == KIND_HERO && w * 10 < h * 14) {
    xsnprintf(why, cap, "hero %lldx%lld is not wide (needs width >= 1.4 x height)", (long long)w, (long long)h);
    return PROBE_BAD;
  }
  return PROBE_OK;
}

static int probe_webp(const unsigned char *d, size_t n, int kind, art_probe *pr, char *why, size_t cap) {
  if (n < 16) return PROBE_MORE;
  if (memcmp(d + 12, "VP8 ", 4) == 0) {   /* lossy: key frame start code, 14-bit sides */
    if (n < 30) return PROBE_MORE;
    if (d[23] != 0x9D || d[24] != 0x01 || d[25] != 0x2A) { xsnprintf(why, cap, "WebP VP8 without a key frame"); return PROBE_BAD; }
    return probe_dims(pr, le16(d + 26) & 0x3FFF, le16(d + 28) & 0x3FFF, kind, 0, why, cap);
  }
  if (memcmp(d + 12, "VP8L", 4) == 0) {   /* lossless: signature 0x2f, then 14-bit width-1 and height-1 */
    if (n < 25) return PROBE_MORE;
    if (d[20] != 0x2F) { xsnprintf(why, cap, "WebP VP8L without its signature"); return PROBE_BAD; }
    uint32_t bits = le32(d + 21);
    return probe_dims(pr, (int64_t)(bits & 0x3FFF) + 1, (int64_t)((bits >> 14) & 0x3FFF) + 1, kind, 0, why, cap);
  }
  if (memcmp(d + 12, "VP8X", 4) == 0) {   /* extended: flags, 24-bit canvas width-1 and height-1 */
    if (n < 30) return PROBE_MORE;
    if (d[20] & 0x02) { xsnprintf(why, cap, "animated WebP"); return PROBE_BAD; }
    return probe_dims(pr, (int64_t)le24(d + 24) + 1, (int64_t)le24(d + 27) + 1, kind, 0, why, cap);
  }
  xsnprintf(why, cap, "WebP with an unknown first chunk");
  return PROBE_BAD;
}

static int probe_png(const unsigned char *d, size_t n, int kind, art_probe *pr, char *why, size_t cap) {
  if (n < 24) return PROBE_MORE;
  if (memcmp(d + 12, "IHDR", 4) != 0) { xsnprintf(why, cap, "PNG without IHDR"); return PROBE_BAD; }
  return probe_dims(pr, be32(d + 16), be32(d + 20), kind, 1, why, cap);
}

/* Walks the markers up to the frame header. stb decodes only SOF0/SOF1/SOF2
 * (baseline, extended and progressive Huffman) with 8-bit samples; the app
 * cannot draw CMYK/YCCK (4 components), 12-bit, arithmetic or lossless. */
static int probe_jpeg(const unsigned char *d, size_t n, int kind, art_probe *pr, char *why, size_t cap) {
  size_t p = 2;
  for (;;) {
    if (p >= n) return PROBE_MORE;
    if (d[p] != 0xFF) { xsnprintf(why, cap, "JPEG with a broken marker"); return PROBE_BAD; }
    while (p < n && d[p] == 0xFF) p++;   /* fill bytes */
    if (p >= n) return PROBE_MORE;
    unsigned m = d[p++];
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;   /* markers without a segment */
    if (m == 0x00 || m == 0xD9 || m == 0xDA) { xsnprintf(why, cap, "JPEG without a frame header"); return PROBE_BAD; }
    if (p + 2 > n) return PROBE_MORE;
    size_t len = be16(d + p);
    if (len < 2) { xsnprintf(why, cap, "JPEG with a broken segment"); return PROBE_BAD; }
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      if (p + 8 > n) return PROBE_MORE;
      unsigned precision = d[p + 2], comps = d[p + 7];
      int64_t h = be16(d + p + 3), w = be16(d + p + 5);
      if (m > 0xC2) { xsnprintf(why, cap, "JPEG coding SOF%u (arithmetic, lossless or hierarchical)", m - 0xC0); return PROBE_BAD; }
      if (precision != 8) { xsnprintf(why, cap, "%u-bit JPEG", precision); return PROBE_BAD; }
      if (comps != 1 && comps != 3) { xsnprintf(why, cap, "JPEG with %u components (CMYK or YCCK)", comps); return PROBE_BAD; }
      return probe_dims(pr, w, h, kind, 1, why, cap);
    }
    p += len;
  }
}

/* PROBE_OK with pr filled, PROBE_BAD with why, or PROBE_MORE (needs more bytes). */
static int art_probe_bytes(const unsigned char *d, size_t n, int kind, art_probe *pr, char *why, size_t cap) {
  memset(pr, 0, sizeof *pr);
  if (n < 12) return PROBE_MORE;
  if (memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0) {
    pr->mime = "image/webp";
    return probe_webp(d, n, kind, pr, why, cap);
  }
  if (memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) {
    pr->mime = "image/png";
    return probe_png(d, n, kind, pr, why, cap);
  }
  if (d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) {
    pr->mime = "image/jpeg";
    return probe_jpeg(d, n, kind, pr, why, cap);
  }
  xsnprintf(why, cap, "not a WebP, PNG or JPEG image");
  return PROBE_BAD;
}

/* At the end of the body: the decoders give up on a PNG or WebP that was cut
 * short, so refuse it here and let the next candidate have its turn. */
static int art_complete(const unsigned char *d, size_t n, const char *mime, char *why, size_t cap) {
  if (strcmp(mime, "image/webp") == 0) {
    uint64_t riff = (uint64_t)le32(d + 4) + 8;
    if (riff <= n) return 1;
    xsnprintf(why, cap, "WebP cut short (%zu of %llu bytes)", n, (unsigned long long)riff);
    return 0;
  }
  if (strcmp(mime, "image/png") == 0) {   /* the IEND chunk closes every PNG (n >= 24 here) */
    size_t lo = n > 4096 ? n - 4096 : 8;
    for (size_t i = n - 4;; i--) {
      if (memcmp(d + i, "IEND", 4) == 0) return 1;
      if (i <= lo) break;
    }
    xsnprintf(why, cap, "PNG cut short (no IEND chunk)");
    return 0;
  }
  return 1;   /* stb decodes a JPEG without its EOI marker */
}

/* ---- fetch --------------------------------------------------------------------
 * cx_fetch's parameters (10 s per operation, 12 MiB, 3 redirects, short-request
 * transfer slot) on the transport it wraps, read by hand so that the probe can
 * refuse a picture from its first bytes, a slow drip is cut after 30 s and a
 * stop cancels the transfer. Every redirect target is checked against the URL
 * policy (https, no userinfo, not a local or private host) before it is
 * contacted; where the hops ended is checked once more before a byte of the
 * body is used. */

typedef struct art_buf {
  unsigned char *data;
  size_t len;
  art_probe pr;
} art_buf;

typedef struct art_hop {
  int kind, refused;
} art_hop;

/* xhttp_req.redirect_ok: a hop outside the policy is refused before the
 * transport connects to it. */
static int art_hop_ok(const char *next, void *user) {
  art_hop *h = user;
  char tmp[ART_URL_MAX];
  if (art_url_ok(next, h->kind, 1, tmp, sizeof tmp)) return 1;
  h->refused = 1;
  return 0;
}

static int art_fetch(const char *url, int kind, art_buf *out, char *why, size_t cap) {
  memset(out, 0, sizeof *out);
  why[0] = 0;
  xhttp_req r;
  memset(&r, 0, sizeof r);
  r.method = "GET";
  r.url = url;
  r.range_start = r.range_end = -1;
  r.extra_headers = ART_ACCEPT;
  r.connect_timeout_ms = r.recv_timeout_ms = ART_TIMEOUT_MS;
  r.follow_redirects = ART_REDIRECTS;
  art_hop hop = {kind, 0};
  r.redirect_ok = art_hop_ok;
  r.redirect_user = &hop;
  r.cancel_flag = &g_cfg.stop_requested;
  xhttp_resp resp;
  xhttp_conn *conn = NULL;
  if (xhttp_open(&r, &conn, &resp) != 0) {
    if (hop.refused) {
      xsnprintf(why, cap, "redirected outside the art URL policy");
      return FETCH_REFUSED;
    }
    xsnprintf(why, cap, "%s", xhttp_last_error()[0] ? xhttp_last_error() : "connection failed");
    return FETCH_FAILED;
  }
  int rc = FETCH_FAILED;
  unsigned char *buf = NULL;
  size_t len = 0, bcap;
  char final_url[ART_URL_MAX];
  if (!art_url_ok(resp.location, kind, 1, final_url, sizeof final_url)) {
    xsnprintf(why, cap, "redirected outside the art URL policy");
    rc = FETCH_REFUSED;
    goto out;
  }
  if (resp.status != 200) { xsnprintf(why, cap, "HTTP %d", resp.status); goto out; }
  if (resp.content_length > (int64_t)ART_MAX_BYTES) {
    xsnprintf(why, cap, "%lld bytes is over the 12 MiB limit", (long long)resp.content_length);
    rc = FETCH_REFUSED;
    goto out;
  }
  bcap = (resp.content_length > 0 ? (size_t)resp.content_length : (size_t)64 << 10) + 1;
  buf = malloc(bcap);
  if (!buf) { xsnprintf(why, cap, "out of memory"); goto out; }
  int64_t deadline = util_now_ms() + ART_TOTAL_MS;
  int probe = PROBE_MORE;
  for (;;) {
    if (len == bcap) {
      size_t nc = bcap * 2 > ART_MAX_BYTES + 1 ? ART_MAX_BYTES + 1 : bcap * 2;
      unsigned char *nb = realloc(buf, nc);
      if (!nb) { xsnprintf(why, cap, "out of memory"); goto out; }
      buf = nb;
      bcap = nc;
    }
    int got = xhttp_read(conn, buf + len, bcap - len);
    if (got < 0) { xsnprintf(why, cap, "%s", xhttp_last_error()[0] ? xhttp_last_error() : "receive failed"); goto out; }
    if (got == 0) break;
    len += (size_t)got;
    if (len > ART_MAX_BYTES) { xsnprintf(why, cap, "over the 12 MiB limit"); rc = FETCH_REFUSED; goto out; }
    if (probe == PROBE_MORE) {
      probe = art_probe_bytes(buf, len, kind, &out->pr, why, cap);
      if (probe == PROBE_BAD) { rc = FETCH_REFUSED; goto out; }
    }
    if (util_now_ms() > deadline) { xsnprintf(why, cap, "took longer than %d s", ART_TOTAL_MS / 1000); goto out; }
  }
  if (resp.content_length >= 0 && (int64_t)len != resp.content_length) {
    xsnprintf(why, cap, "cut short (%zu of %lld bytes)", len, (long long)resp.content_length);
    goto out;
  }
  if (probe != PROBE_OK) {
    if (!why[0]) xsnprintf(why, cap, "not a WebP, PNG or JPEG image");
    rc = FETCH_REFUSED;
    goto out;
  }
  if (!art_complete(buf, len, out->pr.mime, why, cap)) { rc = FETCH_REFUSED; goto out; }
  unsigned char *fit = realloc(buf, len);   /* the cache budget counts len */
  out->data = fit ? fit : buf;
  out->len = len;
  buf = NULL;
  rc = FETCH_OK;
out:
  free(buf);
  xhttp_close(conn);
  return rc;
}

/* ---- the art index ----------------------------------------------------------------- */

typedef struct use_slot {
  uint64_t h;
  uint32_t n;
} use_slot;

typedef struct art_build {
  art_idx *x;
  use_slot *use;   /* cover URL use counts (the web UI's coverUse) */
  size_t ucap;
} art_build;

static uint32_t use_count(art_build *b, const char *url, int add) {
  uint64_t h = fnv1a(url);
  for (size_t s = (size_t)h & (b->ucap - 1);; s = (s + 1) & (b->ucap - 1)) {
    if (b->use[s].h == h) return add ? ++b->use[s].n : b->use[s].n;
    if (!b->use[s].h) {
      if (!add) return 0;
      b->use[s].h = h;
      return b->use[s].n = 1;
    }
  }
}

static uint32_t pool_add(art_idx *x, const char *s) {
  size_t n = strlen(s) + 1;
  if (x->oom) return 0;
  if (x->plen + n > x->pcap) {
    size_t nc = x->pcap ? x->pcap * 2 : (size_t)256 << 10;
    while (nc < x->plen + n) nc *= 2;
    char *np = nc < 0x7FFFFFFFu ? realloc(x->pool, nc) : NULL;
    if (!np) { x->oom = 1; return 0; }
    x->pool = np;
    x->pcap = nc;
  }
  memcpy(x->pool + x->plen, s, n);
  uint32_t off = (uint32_t)x->plen;
  x->plen += n;
  return off;
}

/* Appends one screenshot slot (a pool offset, 0 = unusable). 0, or -1 when out of memory. */
static int shot_push(art_idx *x, uint32_t off) {
  if (x->nshots == x->shcap) {
    uint32_t nc = x->shcap ? x->shcap * 2 : 1024;
    uint32_t *ns = realloc(x->shots, (size_t)nc * sizeof *ns);
    if (!ns) { x->oom = 1; return -1; }
    x->shots = ns;
    x->shcap = nc;
  }
  x->shots[x->nshots++] = off;
  return 0;
}

/* The candidates of one game (map-catalog-art §8.2):
 *  cover: the primary cover is bad when it is missing, an archive.org
 *         services/img collection thumbnail or used by another game too (the
 *         web UI's _artOk rule); then [coverFallback, cover unless shared],
 *         else [cover, coverFallback];
 *  hero:  [hero unless it equals cover, heroFallback unless it equals
 *         coverFallback]; the probe then requires a wide picture;
 *  shot n: the n-th screenshot (TV_ART_MAX_SHOTS in tvapi.h) alone; one
 *         outside the URL policy keeps its place, so n means the same here
 *         as in /catalog's screenshotCount, and answers 404 at once.
 * A game without any usable candidate is left out (404 at once). */
static void index_game(art_build *b, jv g) {
  art_idx *x = b->x;
  const char *id = jv_get_str(g, "id", "");
  if (!art_id_ok(id) || x->ngames >= x->gcap) return;
  const char *c = jv_get_str(g, "cover", ""), *cf = jv_get_str(g, "coverFallback", "");
  const char *h = jv_get_str(g, "hero", ""), *hf = jv_get_str(g, "heroFallback", "");
  int shared = *c && use_count(b, c, 0) > 1;
  int bad = !*c || shared || strstr(c, "archive.org/services/img") != NULL;
  const char *cand[4] = {bad ? cf : c, bad ? (shared ? "" : c) : cf, strcmp(h, c) != 0 ? h : "", strcmp(hf, cf) != 0 ? hf : ""};
  art_game ag;
  memset(&ag, 0, sizeof ag);
  char url[ART_URL_MAX];
  int any = 0;
  for (int k = 0; k < 4; k++) {
    if (!*cand[k] || !art_url_ok(cand[k], k >= 2 ? KIND_HERO : KIND_COVER, 0, url, sizeof url)) continue;
    if ((k & 1) && ag.url[k - 1] && strcmp(x->pool + ag.url[k - 1], url) == 0) continue;   /* same URL twice */
    if (!(ag.url[k] = pool_add(x, url))) return;
    any = 1;
  }
  int shots = 0;
  ag.shot0 = x->nshots;
  jv list = jv_get(g, "screenshots");
  if (jv_type(list) == JV_ARR) {   /* JV_FOREACH would walk an object's members too */
    JV_FOREACH(s, list) {
      if (ag.nshots >= TV_ART_MAX_SHOTS) break;
      if (jv_type(s) != JV_STR || jv_strlen(s) == 0) continue;
      uint32_t off = 0;
      if (art_url_ok(jv_str(s, ""), KIND_SHOT, 0, url, sizeof url) && !(off = pool_add(x, url))) return;
      if (shot_push(x, off) != 0) return;
      ag.nshots++;
      if (off) shots = 1;
    }
  }
  if (!any && !shots) {
    x->nshots = ag.shot0;
    return;
  }
  if (!(ag.id = pool_add(x, id))) return;
  ag.hash = fnv1a(id);
  x->games[x->ngames++] = ag;
  if (ag.url[0] || ag.url[1]) x->covers++;
  if (ag.url[2] || ag.url[3]) x->heroes++;
  if (shots) x->shot_games++;
}

/* catalog_visit_root callback: holds catalog_mu, so it calls no catalog_* or
 * settings_* function and takes no other lock. */
static void build_visit(jv root, void *user) {
  art_build *b = user;
  art_idx *x = b->x;
  x->doc = root.d;
  x->revision = jv_get_i64(root, "revision", 0);
  jv games = jv_get(root, "games");
  size_t n = jv_len(games);
  size_t cap = 16;
  while (cap < n * 2) cap *= 2;
  b->use = calloc(cap, sizeof *b->use);
  x->games = calloc(n ? n : 1, sizeof *x->games);
  if (!b->use || !x->games) { x->oom = 1; return; }
  b->ucap = cap;
  x->gcap = (uint32_t)n;
  JV_FOREACH(g, games) {
    const char *c = jv_get_str(g, "cover", "");
    if (*c) use_count(b, c, 1);
  }
  JV_FOREACH(g, games) {
    if (x->oom) break;
    index_game(b, g);
  }
}

static void index_free(art_idx *x) {
  if (!x) return;
  free(x->games);
  free(x->slots);
  free(x->pool);
  free(x->shots);
  free(x);
}

/* The hash table over game ids; the first of two equal ids wins, as in catalog.c. */
static int index_finish(art_idx *x) {
  uint32_t cap = 16;
  while (cap < x->ngames * 2u) cap *= 2;
  x->slots = calloc(cap, sizeof *x->slots);
  if (!x->slots) return -1;
  x->nslots = cap;
  for (uint32_t i = 0; i < x->ngames; i++) {
    const art_game *g = &x->games[i];
    for (uint32_t s = (uint32_t)g->hash & (cap - 1);; s = (s + 1) & (cap - 1)) {
      if (!x->slots[s]) { x->slots[s] = i + 1; break; }
      const art_game *o = &x->games[x->slots[s] - 1];
      if (o->hash == g->hash && strcmp(x->pool + o->id, x->pool + g->id) == 0) break;
    }
  }
  if (x->ngames < x->gcap) {
    art_game *fit = realloc(x->games, (x->ngames ? x->ngames : 1) * sizeof *x->games);
    if (fit) x->games = fit;
  }
  char *pfit = realloc(x->pool, x->plen);
  if (pfit) x->pool = pfit;
  if (x->nshots && x->nshots < x->shcap) {
    uint32_t *sfit = realloc(x->shots, x->nshots * sizeof *x->shots);
    if (sfit) x->shots = sfit;
  }
  return 0;
}

static const art_game *index_find(const art_idx *x, const char *id) {
  if (!x || !x->nslots) return NULL;
  uint64_t h = fnv1a(id);
  for (uint32_t s = (uint32_t)h & (x->nslots - 1);; s = (s + 1) & (x->nslots - 1)) {
    uint32_t v = x->slots[s];
    if (!v) return NULL;
    const art_game *g = &x->games[v - 1];
    if (g->hash == h && strcmp(x->pool + g->id, id) == 0) return g;
  }
}

/* The candidate URLs of one picture of g (NULL: none), as pool offsets in
 * the order they are tried; 0 = no candidate. */
static void game_cands(const art_idx *x, const art_game *g, int kind, int shot, uint32_t c[2]) {
  c[0] = c[1] = 0;
  if (!g) return;
  if (kind == KIND_SHOT) {
    if (shot >= 0 && shot < g->nshots) c[0] = x->shots[g->shot0 + (uint32_t)shot];
    return;
  }
  c[0] = g->url[kind == KIND_HERO ? 2 : 0];
  c[1] = g->url[kind == KIND_HERO ? 3 : 1];
}

/* A cache key ("<id>#cover", "<id>#hero", "<id>#shot<n>") split into the
 * game id (ART_KEY bytes) and the kind; *shot = n for a screenshot. */
static int key_split(const char *key, char *id, int *shot) {
  xstrlcpy(id, key, ART_KEY);
  *shot = 0;
  char *hash = strrchr(id, '#');   /* never part of a game id (art_id_ok) */
  if (!hash) return KIND_COVER;
  *hash = 0;
  if (strcmp(hash + 1, "hero") == 0) return KIND_HERO;
  if (strncmp(hash + 1, "shot", 4) == 0) {
    *shot = atoi(hash + 5);
    return KIND_SHOT;
  }
  return KIND_COVER;
}

/* ---- cache (everything below *_locked runs under art_mu) --------------------------
 * art_mu is a leaf lock and the log writes a file, so lines are composed under
 * the lock into an art_notes and written once it is released. */

#define ART_NOTES 6

typedef struct art_notes {
  int n, more;
  int level[ART_NOTES];
  char line[ART_NOTES][300];
} art_notes;

static void note(art_notes *nt, int level, const char *fmt, ...) XS_PRINTF(3, 4);
static void note(art_notes *nt, int level, const char *fmt, ...) {
  if (nt->n >= ART_NOTES) {
    nt->more++;
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  xvsnprintf(nt->line[nt->n], sizeof nt->line[0], fmt, ap);
  va_end(ap);
  nt->level[nt->n++] = level;
}

static void notes_flush(const art_notes *nt) {
  for (int i = 0; i < nt->n; i++) log_write(nt->level[i], "%s", nt->line[i]);
  if (nt->more) log_write(LOG_DEBUG, "tvart: %d more cache event(s)", nt->more);
}

static void entry_reset(art_entry *e) {
  if (e->state == ART_READY) {
    A.bytes -= e->size;
    A.nready--;
  }
  free(e->data);
  memset(e, 0, sizeof *e);
}

static art_entry *entry_find_locked(const char *key, uint64_t kh) {
  for (int i = 0; i < ART_ENTRIES; i++) {
    art_entry *e = &A.e[i];
    if (e->state != ART_EMPTY && e->khash == kh && strcmp(e->key, key) == 0) return e;
  }
  return NULL;
}

/* A free slot, else the least recently used one that is not pending. */
static art_entry *entry_take_locked(art_notes *nt) {
  art_entry *best = NULL;
  for (int i = 0; i < ART_ENTRIES; i++) {
    art_entry *e = &A.e[i];
    if (e->state == ART_EMPTY) return e;
    if (e->state != ART_PENDING && (!best || e->used_ms < best->used_ms)) best = e;
  }
  if (best && best->state == ART_READY) {
    A.evictions++;
    note(nt, LOG_DEBUG, "tvart: evicted %s (%zu bytes, entry limit)", best->key, best->size);
  }
  if (best) entry_reset(best);
  return best;
}

/* Drops the least recently served pictures until READY bytes fit the budget. */
static void trim_locked(const art_entry *keep, art_notes *nt) {
  while (A.bytes > ART_BUDGET) {
    art_entry *old = NULL;
    for (int i = 0; i < ART_ENTRIES; i++) {
      art_entry *e = &A.e[i];
      if (e->state == ART_READY && e != keep && (!old || e->used_ms < old->used_ms)) old = e;
    }
    if (!old) return;
    A.evictions++;
    note(nt, LOG_DEBUG, "tvart: evicted %s (%zu bytes, budget)", old->key, old->size);
    entry_reset(old);
  }
}

/* Room for one more queued key: the queue holds at most 64; when full, the
 * oldest key the app stopped asking for (it re-asks a wanted one every
 * 0.6 s and drops its own requests past 48) makes way. */
static int queue_room_locked(int64_t now, art_notes *nt) {
  if (A.nqueue < ART_QUEUE) return 1;
  for (int i = 0; i < A.nqueue; i++) {
    art_entry *d = &A.e[A.queue[i]];
    if (now - d->asked_ms < ART_STALE_MS) continue;
    note(nt, LOG_DEBUG, "tvart: dropped %s from the queue (not asked for %lld ms)", d->key, (long long)(now - d->asked_ms));
    memmove(&A.queue[i], &A.queue[i + 1], (size_t)(A.nqueue - i - 1) * sizeof A.queue[0]);
    A.nqueue--;
    entry_reset(d);
    return 1;
  }
  return 0;
}

/* The refusal memory's key: a URL with the kind it was refused for. The kinds
 * have their own caps and shape rule (a 1920x1080 PNG is too big for a cover
 * and a square picture is no hero, yet both make a fine screenshot), so a
 * refusal holds for that kind only. */
static uint64_t refusal_key(const char *url, int kind) {
  uint64_t h = (fnv1a(url) ^ (uint64_t)(kind + 1)) * 1099511628211ULL;
  return h ? h : 1;   /* 0 marks an empty refusal slot */
}

static int refused_has_locked(uint64_t h) {
  for (uint32_t s = (uint32_t)h & (ART_REFUSED_SLOTS - 1);; s = (s + 1) & (ART_REFUSED_SLOTS - 1)) {
    if (!A.refused[s]) return 0;
    if (A.refused[s] == h) return 1;
  }
}

static void refused_add_locked(uint64_t h, art_notes *nt) {
  if (refused_has_locked(h)) return;
  if (A.nrefused >= ART_REFUSED_SLOTS / 2) {
    if (!A.refused_full_logged)
      note(nt, LOG_WARN, "tvart: refusal memory full (%d URLs); further refusals are not remembered", A.nrefused);
    A.refused_full_logged = 1;
    return;
  }
  for (uint32_t s = (uint32_t)h & (ART_REFUSED_SLOTS - 1);; s = (s + 1) & (ART_REFUSED_SLOTS - 1)) {
    if (!A.refused[s]) {
      A.refused[s] = h;
      A.nrefused++;
      return;
    }
  }
}

/* A new catalogue: every finished entry, the queue and the refusal memory
 * go. Entries in flight stay PENDING until their worker returns, sees the
 * new generation and drops the result. */
static void cache_clear_locked(void) {
  for (int i = 0; i < ART_ENTRIES; i++) {
    art_entry *e = &A.e[i];
    if (e->state == ART_PENDING && !e->queued) continue;
    entry_reset(e);
  }
  A.nqueue = 0;
  memset(A.refused, 0, sizeof A.refused);
  A.nrefused = 0;
  A.refused_full_logged = 0;
  A.gen++;
}

/* ---- index refresh (a worker, without art_mu) --------------------------------------- */

typedef struct art_peek {
  const void *doc;
  int64_t revision;
} art_peek;

static void peek_visit(jv root, void *user) {
  art_peek *p = user;
  p->doc = root.d;
  p->revision = jv_get_i64(root, "revision", 0);
}

static void index_refresh(void) {
  art_peek pk = {NULL, 0};
  if (catalog_visit_root(peek_visit, &pk) != 0) pk.doc = NULL;
  pthread_mutex_lock(&art_mu);
  int need = !A.idx || A.idx->doc != pk.doc || A.idx->revision != pk.revision;
  pthread_mutex_unlock(&art_mu);
  if (!need) return;
  xthread_note("tv-art: building the art index");
  int64_t t0 = util_now_ms();
  art_idx *x = calloc(1, sizeof *x);
  if (!x) return;
  pool_add(x, "");   /* offset 0 means "none" */
  art_build b = {x, NULL, 0};
  if (catalog_visit_root(build_visit, &b) != 0) {   /* no catalogue: every request is a 404 */
    x->doc = NULL;
    x->revision = 0;
  }
  free(b.use);
  if (x->oom || index_finish(x) != 0) {
    LOGW("tvart: out of memory building the art index; trying again shortly");
    index_free(x);
    return;
  }
  pthread_mutex_lock(&art_mu);
  art_idx *old = A.idx;
  A.idx = x;
  if (old) cache_clear_locked();   /* the first index is not a change */
  pthread_cond_broadcast(&art_cv);
  pthread_mutex_unlock(&art_mu);
  index_free(old);
  LOGI("tvart: art index for catalogue revision %lld: %u games with art (%d covers, %d heroes), %d with screenshots, %zu KiB, %lld ms%s",
       (long long)x->revision, x->ngames, x->covers, x->heroes, x->shot_games,
       (x->plen + x->ngames * sizeof(art_game) + x->nslots * 4u + x->nshots * 4u) >> 10, (long long)(util_now_ms() - t0),
       old ? "; cache cleared" : "");
}

/* ---- workers ----------------------------------------------------------------------- */

typedef struct art_job {
  int slot;
  unsigned gen;
  int kind;
  char key[ART_KEY];
  int n;
  char url[2][ART_URL_MAX];
} art_job;

typedef struct art_result {
  int state;
  art_buf img;
  int from;
  int nrefused;
  uint64_t refused[2];
} art_result;

/* Takes the newest queued key. 1 with the job filled, 0 when the key needs
 * no fetch (unknown game, no candidate, every candidate refused: NONE). */
static int job_take_locked(art_job *j, int64_t now) {
  int slot = A.queue[--A.nqueue];
  art_entry *e = &A.e[slot];
  e->queued = 0;
  memset(j, 0, sizeof *j);
  j->slot = slot;
  j->gen = A.gen;
  xstrlcpy(j->key, e->key, sizeof j->key);
  char id[ART_KEY];
  int shot;
  j->kind = key_split(e->key, id, &shot);
  uint32_t cand[2];
  game_cands(A.idx, index_find(A.idx, id), j->kind, shot, cand);
  for (int k = 0; k < 2; k++) {
    uint32_t off = cand[k];
    if (!off) continue;
    const char *u = A.idx->pool + off;
    if (refused_has_locked(refusal_key(u, j->kind))) continue;
    xstrlcpy(j->url[j->n++], u, ART_URL_MAX);
  }
  if (j->n) return 1;
  e->state = ART_NONE;
  e->used_ms = now;
  return 0;
}

/* Tries the candidates in order (no lock held). */
static void job_run(const art_job *j, art_result *r) {
  memset(r, 0, sizeof *r);
  int failed = 0;
  for (int i = 0; i < j->n; i++) {
    char why[200];
    xthread_note("tv-art: %s", j->key);
    int rc = art_fetch(j->url[i], j->kind, &r->img, why, sizeof why);
    if (rc == FETCH_OK) {
      r->state = ART_READY;
      r->from = i;
      return;
    }
    if (rc == FETCH_REFUSED) {
      r->refused[r->nrefused++] = refusal_key(j->url[i], j->kind);
      LOGI("tvart: refused for %s: %s: %s", j->key, why, j->url[i]);
    } else {
      failed = 1;
      LOGW("tvart: could not fetch for %s: %s: %s", j->key, why, j->url[i]);
    }
  }
  r->state = failed ? ART_FAILED : ART_NONE;
}

static void job_store_locked(const art_job *j, art_result *r, int64_t now, art_notes *nt) {
  art_entry *e = &A.e[j->slot];
  int current = j->gen == A.gen && e->state == ART_PENDING && !e->queued && strcmp(e->key, j->key) == 0;
  if (j->gen == A.gen) {
    for (int i = 0; i < r->nrefused; i++) refused_add_locked(r->refused[i], nt);
  }
  A.refusals += (unsigned long)r->nrefused;
  if (r->state == ART_FAILED) A.failures++;
  if (!current) {   /* a new catalogue arrived meanwhile: the next request starts over */
    free(r->img.data);
    if (j->gen != A.gen && e->state == ART_PENDING && !e->queued && strcmp(e->key, j->key) == 0) entry_reset(e);
    return;
  }
  e->used_ms = now;
  if (r->state == ART_READY) {
    e->state = ART_READY;
    e->data = r->img.data;
    e->size = r->img.len;
    xstrlcpy(e->mime, r->img.pr.mime, sizeof e->mime);
    A.bytes += e->size;
    A.nready++;
    A.fetched++;
    trim_locked(e, nt);
    note(nt, LOG_DEBUG, "tvart: cached %s (%s %lldx%lld, %zu bytes, candidate %d); cache %d image(s), %zu bytes; %s", e->key, e->mime,
         (long long)r->img.pr.w, (long long)r->img.pr.h, e->size, r->from + 1, A.nready, A.bytes, j->url[r->from]);
  } else if (r->state == ART_FAILED) {
    e->state = ART_FAILED;
    e->failed_ms = now;
  } else {
    e->state = ART_NONE;
    note(nt, LOG_INFO, "tvart: no usable artwork for %s (%lu refusal(s) so far)", e->key, A.refusals);
  }
}

static void cv_wait_ms(int64_t ms) {
  if (ms < 10) ms = 10;
  if (ms > 1000) ms = 1000;
  struct timespec ts;
  clock_gettime(art_cv_mono ? CLOCK_MONOTONIC : CLOCK_REALTIME, &ts);
  ts.tv_sec += (time_t)(ms / 1000);
  ts.tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000L;
  }
  pthread_cond_timedwait(&art_cv, &art_mu, &ts);
}

static void *art_worker(void *arg) {
  (void)arg;
  art_job *j = malloc(sizeof *j);
  art_notes nt;
  if (!j) {
    LOGE("tvart: out of memory starting an art worker");
    return NULL;
  }
  pthread_mutex_lock(&art_mu);
  while (!g_cfg.stop_requested) {
    int64_t now = util_now_ms();
    if (!A.building && now >= A.check_due) {   /* one worker at a time follows the catalogue */
      A.building = 1;
      A.check_due = now + ART_REV_CHECK_MS;   /* the other worker sleeps meanwhile (woken by the install) */
      pthread_mutex_unlock(&art_mu);
      index_refresh();
      pthread_mutex_lock(&art_mu);
      A.building = 0;
      A.check_due = util_now_ms() + ART_REV_CHECK_MS;
      continue;
    }
    if (A.idx && A.nqueue > 0) {
      if (!job_take_locked(j, now)) continue;
      pthread_mutex_unlock(&art_mu);
      art_result r;
      job_run(j, &r);
      xthread_note("tv-art: idle");
      pthread_mutex_lock(&art_mu);
      nt.n = nt.more = 0;
      job_store_locked(j, &r, util_now_ms(), &nt);
      if (nt.n || nt.more) {
        pthread_mutex_unlock(&art_mu);
        notes_flush(&nt);
        pthread_mutex_lock(&art_mu);
      }
      continue;
    }
    cv_wait_ms(A.check_due - now);
  }
  pthread_mutex_unlock(&art_mu);
  free(j);
  return NULL;
}

void tvart_start(void) {
  pthread_condattr_t ca;
  int mono = 0, ok = 0;
  if (pthread_condattr_init(&ca) == 0) {
    mono = pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) == 0;
    ok = pthread_cond_init(&art_cv, mono ? &ca : NULL) == 0;
    pthread_condattr_destroy(&ca);
  }
  if (!ok) {
    mono = 0;
    pthread_cond_init(&art_cv, NULL);
  }
  art_cv_mono = mono;
  int n = 0;
  for (int i = 0; i < ART_WORKERS; i++)
    if (tv_thread_start(NULL, "tv-art", art_worker, NULL, 0, 1) == 0) n++;
  pthread_mutex_lock(&art_mu);
  A.started = n;
  pthread_mutex_unlock(&art_mu);
  if (n == 0) LOGE("tvart: no art worker could start; /api/v1/art answers 503");
}

/* ---- GET /api/v1/art/{gameId}/{cover|hero} and /art/{gameId}/shot/{n} -------------------
 * 404 unknown id, kind, game, no candidate or NONE; 200 bytes; 202 pending
 * or queued now; 502 within 120 s of a failure; 503 when the queue is full
 * (or no worker runs). The gate already answered 503 while stopping and 401
 * without a paired Bearer token (the app retries 401, 202 and 503). */

typedef struct art_answer {
  int status;
  unsigned char *copy;
  size_t len;
  char mime[12];
} art_answer;

static void art_lookup_locked(const char *id, int kind, int shot, const char *key, uint64_t kh, int64_t now,
                              art_answer *an, art_notes *nt) {
  if (!A.started) { an->status = 503; return; }
  if (A.idx) {   /* until the first index exists every key is queued and resolved by the worker */
    uint32_t c[2];
    game_cands(A.idx, index_find(A.idx, id), kind, shot, c);
    if (!c[0] && !c[1]) { an->status = 404; return; }
    int open0 = c[0] && !refused_has_locked(refusal_key(A.idx->pool + c[0], kind));
    int open1 = c[1] && !refused_has_locked(refusal_key(A.idx->pool + c[1], kind));
    if (!open0 && !open1) { an->status = 404; return; }   /* every candidate refused for this catalogue */
  }
  art_entry *e = entry_find_locked(key, kh);
  if (e) {
    switch (e->state) {
    case ART_READY:
      an->copy = malloc(e->size);
      if (!an->copy) { an->status = 503; return; }
      memcpy(an->copy, e->data, e->size);   /* copied under the lock: an eviction cannot free it mid-send */
      an->len = e->size;
      xstrlcpy(an->mime, e->mime, sizeof an->mime);
      e->used_ms = now;
      an->status = 200;
      return;
    case ART_PENDING:
      e->asked_ms = now;
      an->status = 202;
      return;
    case ART_NONE:
      e->used_ms = now;
      an->status = 404;
      return;
    case ART_FAILED:
      if (now - e->failed_ms < ART_RETRY_FAILED_MS) {
        e->used_ms = now;
        an->status = 502;
        return;
      }
      break;   /* the window is over: fetch again */
    }
  }
  if (!queue_room_locked(now, nt)) { A.busy++; an->status = 503; return; }
  if (!e) {
    e = entry_take_locked(nt);
    if (!e) { an->status = 503; return; }
    xstrlcpy(e->key, key, sizeof e->key);
    e->khash = kh;
  }
  e->state = ART_PENDING;
  e->queued = 1;
  e->used_ms = e->asked_ms = now;
  A.queue[A.nqueue++] = (int)(e - A.e);
  pthread_cond_signal(&art_cv);
  an->status = 202;
}

/* Answers one picture of a checked game id (both routes). */
static void art_reply(api_ctx *c, const char *id, int kind, int shot) {
  char key[ART_KEY];
  if (kind == KIND_SHOT) xsnprintf(key, sizeof key, "%s#shot%d", id, shot);
  else xsnprintf(key, sizeof key, "%s#%s", id, kind == KIND_HERO ? "hero" : "cover");
  art_answer an;
  memset(&an, 0, sizeof an);
  art_notes nt;
  nt.n = nt.more = 0;
  pthread_mutex_lock(&art_mu);
  art_lookup_locked(id, kind, shot, key, fnv1a(key), util_now_ms(), &an, &nt);
  pthread_mutex_unlock(&art_mu);
  notes_flush(&nt);
  switch (an.status) {
  case 200:
    tv_send_bytes(c, 200, an.mime, an.copy, an.len);
    free(an.copy);
    return;
  case 202: {
    jw w;
    tv_obj_begin(&w);
    jw_kv_bool(&w, "pending", 1);
    tv_obj_send(c, 202, &w);
    return;
  }
  case 404: tv_error(c, 404, ART_MSG_UNKNOWN); return;
  case 502: tv_error(c, 502, ART_MSG_FAILED); return;
  default: tv_error(c, 503, ART_MSG_BUSY); return;
  }
}

void tvart_h_art(api_ctx *c) {
  if (c->nparams < 2) {
    tv_error(c, 404, ART_MSG_UNKNOWN);
    return;
  }
  const char *id = c->params[0], *kind = c->params[1];
  int hero = strcmp(kind, "hero") == 0;
  if (!art_id_ok(id) || (!hero && strcmp(kind, "cover") != 0)) {
    tv_error(c, 404, ART_MSG_UNKNOWN);
    return;
  }
  art_reply(c, id, hero ? KIND_HERO : KIND_COVER, 0);
}

_Static_assert(TV_ART_MAX_SHOTS <= 10, "shot/{n} takes one digit");

/* The n-th screenshot (0-based) of those /catalog counts in screenshotCount:
 * n is one digit below TV_ART_MAX_SHOTS ("01", "+1" or " 1" are unknown). */
void tvart_h_shot(api_ctx *c) {
  if (c->nparams < 2) {
    tv_error(c, 404, ART_MSG_UNKNOWN);
    return;
  }
  const char *id = c->params[0], *n = c->params[1];
  if (!art_id_ok(id) || n[0] < '0' || n[0] > '9' || n[1] || n[0] - '0' >= TV_ART_MAX_SHOTS) {
    tv_error(c, 404, ART_MSG_UNKNOWN);
    return;
  }
  art_reply(c, id, KIND_SHOT, n[0] - '0');
}

/* GET /api/diagnostics "tvapi" extras: the art cache at a glance (read under
 * art_mu, written after it). */
void tvart_diag_json(jw *w) {
  pthread_mutex_lock(&art_mu);
  int workers = A.started, games = A.idx ? (int)A.idx->ngames : 0, images = A.nready, queued = A.nqueue;
  int refused = A.nrefused;
  int64_t rev = A.idx ? A.idx->revision : -1, bytes = (int64_t)A.bytes;
  int64_t fetched = (int64_t)A.fetched, refusals = (int64_t)A.refusals, failures = (int64_t)A.failures;
  int64_t evictions = (int64_t)A.evictions, busy = (int64_t)A.busy;
  pthread_mutex_unlock(&art_mu);
  jw_key(w, "art");
  jw_obj_begin(w);
  jw_kv_int(w, "workers", workers);
  jw_kv_i64(w, "indexRevision", rev);
  jw_kv_int(w, "indexGames", games);
  jw_kv_int(w, "images", images);
  jw_kv_i64(w, "bytes", bytes);
  jw_kv_int(w, "queued", queued);
  jw_kv_int(w, "refusedUrls", refused);
  jw_kv_i64(w, "fetched", fetched);
  jw_kv_i64(w, "refusals", refusals);
  jw_kv_i64(w, "failures", failures);
  jw_kv_i64(w, "evictions", evictions);
  jw_kv_i64(w, "busy", busy);
  jw_obj_end(w);
}
