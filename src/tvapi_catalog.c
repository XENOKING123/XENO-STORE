/*
 * tvapi_catalog.c — the TV catalogue index (tvcat_*) and GET|POST
 * /api/v1/sources, GET /api/v1/catalog, GET|POST /api/v1/catalog/updates,
 * GET|POST /api/v1/favorites (PORT-SPEC 1.5.2, 1.6.1; map-catalog-art §2-§7, §9).
 *
 * Copyright (C) 2026 XENO STORE contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The index. One catalog_visit_root() walk turns the game-centric catalogue
 * into an immutable, reference-counted snapshot of everything the TV app can
 * be shown (tier A, R1): the exposed sources with their TV release counts,
 * the visible games in catalogue order with their game-level fields already
 * mapped (lowercase platform, joined genres, checked dates, ranks, cheat and
 * screenshot counts), and each game's visible releases in R2 order with
 * their option fields already mapped (title ID, TV format spelling,
 * filename, region, kind, notes). Two hash
 * indexes (release id, catalogue filename) serve the lookups of the
 * downloads and library subsystems. GET /api/v1/catalog renders from the
 * snapshot, so it never holds catalog_mu; only a rebuild walks the catalogue.
 *
 * The snapshot is keyed on catalog_refresh_runs() (every refresh that ends,
 * so every catalogue swap), the featured feed's load time and the compile
 * switches; a stale key is rebuilt by the next request that needs fresh
 * data (/sources is read on every poll of the app), never by GET /system.
 * Lookups (tvcat_release, tvcat_by_filename, tv_find_release) use whatever
 * snapshot exists and build one only when there is none yet.
 *
 * The mapping rules are tests/host/tv_golden.py's, which the host tests
 * compare byte for byte. The /sources, /catalog/updates and /favorites
 * validators, sentences, cooldown and set semantics are ported from the
 * reference backend: Ported from Orbit Store backend/sources.c,
 * backend/favorites.c, backend/catalog.c and backend/api.c
 * (GPL-3.0-or-later), with our own sentences where PORT-SPEC rebrands them.
 */
#include "tvapi.h"

#include "catalog.h"
#include "config.h"
#include "cx_feeds.h"
#include "http_client.h"
#include "log.h"
#include "settings.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

/* ---- compile switches (PORT-SPEC R1; owner decision 0.2.1) ------------------
 * TV_SHOW_ZIP 1 also shows ZIP options (format "Unknown" + compression "zip");
 * the host build can turn it on with XENO_TV_SHOW_ZIP=1. TV_REQUIRE_TITLE_ID
 * 1 hides options without a title ID. Both are off for tier A. */
#ifndef TV_SHOW_ZIP
#define TV_SHOW_ZIP 0
#endif
#ifndef TV_REQUIRE_TITLE_ID
#define TV_REQUIRE_TITLE_ID 0
#endif

#define TVCAT_NOTICE_VERSION 1          /* reference ORBIT_SOURCE_NOTICE_VERSION; not persisted */
#define TVCAT_MAX_RANK 100
#define TVCAT_MAX_SOURCES 32
#define TVCAT_MAX_HERO 512              /* featured hero ids considered for ranks (the hash covers all) */
#define TVCAT_GAME_ID_MAX 95            /* R1: our router's capture limit */
#define TVCAT_POOL_MAX (64u << 20)
#define TVCAT_REFRESH_COOLDOWN_S 60     /* reference catalog.c: check_after = now + 60 */
#define TVCAT_REV_BITS 33               /* (revision << 20) stays below 2^53 */

/* Sentences. Ported from Orbit Store backend/sources.c, favorites.c,
 * catalog.c and api.c (GPL-3.0-or-later); the refresh-network one is ours
 * (PORT-SPEC 1.5.2). The app translates exact matches. */
#define MSG_SOURCES_SELECT "Select the sources you want to enable."
#define MSG_SOURCES_UNKNOWN "Unknown or duplicate source."
#define MSG_SOURCES_NOTICE "Read and acknowledge the current download notice before saving sources."
#define MSG_SOURCES_SAVE "Could not save source settings. Check console storage."
#define MSG_SOURCES_RETRY "Could not save source settings. Try again."
#define MSG_REFRESH_ACTION "Expected refresh action."
#define MSG_REFRESH_NETWORK "Catalogue refresh needs the console's network. Try again shortly."
#define MSG_REFRESH_BUSY "The catalogue is already refreshing."
#define MSG_REFRESH_WAIT "Please wait before refreshing the catalogue again."
#define MSG_FAV_BAD "Expected a game and favourite choice."
#define MSG_FAV_UNKNOWN "Game is not in this catalogue."
#define MSG_FAV_FULL "Favourites are full."
#define MSG_FAV_SAVE "Could not save favourites."

/* Option notes for the non-game kinds (map-catalog-art §5). */
#define NOTE_UPDATE_PRE "Update "
#define NOTE_UPDATE_POST ". Install the base game first."
#define NOTE_UPDATE_NO_VERSION "Update. Install the base game first."
#define NOTE_DLC "Add-on content. Needs the base game."
#define NOTE_BACKPORT "Backport for older firmware."

#define FNV_OFFSET 2166136261u
#define FNV_PRIME 16777619u

static uint32_t fnv1a_add(uint32_t h, const void *data, size_t n) {
  const unsigned char *p = data;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= FNV_PRIME; }
  return h;
}

/* ==== switches ============================================================== */

static int show_zip(void) {
  static volatile int cached = -1;
  int v = cached;
  if (v < 0) {
    v = TV_SHOW_ZIP ? 1 : 0;
#ifdef XENO_HOST
    const char *e = getenv("XENO_TV_SHOW_ZIP");
    if (e && strcmp(e, "1") == 0) v = 1;
#endif
    cached = v;
  }
  return v;
}

/* ==== the index ============================================================ */

typedef struct pstr { uint32_t off, len; } pstr;   /* a string in the index pool */

typedef struct tc_src {
  pstr id, name;
  int browser;          /* source kind "browser" (else "direct") */
  int count;            /* TV-visible releases, whatever the enabled state */
} tc_src;

typedef struct tc_game {
  pstr id, title, platform, genre, tagline, description, publisher, releaseDate, addedAt, artworkLayout;
  int rank;             /* newOnOrbitRank 1..100, 0 = unranked */
  int popular_rank;     /* popularRank 1..game_count (1 = the most popular) */
  int shots;            /* screenshotCount 0..TV_ART_MAX_SHOTS */
  int64_t cheats;       /* catalogue cheats, 0 = none */
  double popularity;    /* catalogue popularity: the order of popularRank */
  uint32_t first, n;    /* its releases: rels[first .. first + n), R2 order */
  uint32_t cat_index;   /* position in the catalogue's games[] */
} tc_game;

typedef struct tc_rel {
  pstr id, titleId, format, filename, version, region, notes, provider, sourceId, kind;
  int64_t size;
  uint32_t game;        /* index into games[] */
  uint32_t seq;         /* catalogue order over every visible release */
  uint16_t src;         /* index into src[] */
  uint8_t zip;          /* a ZIP option (TV_SHOW_ZIP) */
  uint8_t sort;         /* R2 keys 1-4 packed: kind << 3 | source << 2 | flaky << 1 | unverified */
} tc_rel;

typedef struct htab { uint32_t *slot; uint32_t mask; } htab;   /* slot = index + 1, 0 = empty */

typedef struct tcat_idx {
  int refs;
  /* key */
  unsigned runs;        /* catalog_refresh_runs() before the walk */
  time_t feed_at;       /* cx_feed_fetched_at(CX_FEED_FEATURED) before the feed was read */
  int zip;              /* show_zip() */
  /* content */
  int64_t revision;     /* the walked catalogue's "revision" */
  uint32_t hero_hash;
  int64_t composite;    /* GET /system catalogueRevision */
  char *pool;
  size_t pool_len, pool_cap;
  tc_src src[TVCAT_MAX_SOURCES];
  int nsrc;
  tc_game *games;
  uint32_t ngames, games_cap;
  tc_rel *rels;
  uint32_t nrels, rels_cap;
  int game_count;       /* distinct visible game ids (/catalog/updates gameCount) */
  int ranked;
  htab by_id, by_file;
  int oom;
} tcat_idx;

/* tvcat_mu (a leaf lock) guards g_idx, the refcounts, g_rev and g_refresh.
 * build_mu serialises rebuilds; it is only taken with no other lock held and
 * is held across catalog_visit_root() (catalog_mu) and cx_feed_doc(). */
static pthread_mutex_t tvcat_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t build_mu = PTHREAD_MUTEX_INITIALIZER;
static tcat_idx *g_idx;

static struct { int valid; unsigned runs; time_t feed_at; int64_t value; } g_rev;   /* /system */

static struct {
  int busy;               /* a tv-catalog refresh thread is running */
  time_t check_after;     /* manual refresh cooldown end (0 = none yet) */
  time_t finished_at;     /* last tv-catalog refresh end */
  char err[320];          /* last tv-catalog refresh failure as a sentence, "" = none */
} g_refresh;

static inline const char *P(const tcat_idx *x, pstr s) { return x->pool + s.off; }

static void idx_free(tcat_idx *x) {
  if (!x) return;
  free(x->pool);
  free(x->games);
  free(x->rels);
  free(x->by_id.slot);
  free(x->by_file.slot);
  free(x);
}

static void idx_unref(tcat_idx *x) {
  if (!x) return;
  pthread_mutex_lock(&tvcat_mu);
  int last = --x->refs == 0;
  pthread_mutex_unlock(&tvcat_mu);
  if (last) idx_free(x);
}

/* ---- pool ----------------------------------------------------------------- */

static int pool_reserve(tcat_idx *x, size_t extra) {
  if (x->oom) return -1;
  if (x->pool_len + extra + 1 <= x->pool_cap) return 0;
  size_t ncap = x->pool_cap ? x->pool_cap : 256u << 10;
  while (ncap < x->pool_len + extra + 1) ncap *= 2;
  if (ncap > TVCAT_POOL_MAX) { x->oom = 1; return -1; }
  char *np = realloc(x->pool, ncap);
  if (!np) { x->oom = 1; return -1; }
  x->pool = np;
  x->pool_cap = ncap;
  return 0;
}

static void pool_put(tcat_idx *x, const void *s, size_t n) {
  if (!n || pool_reserve(x, n) != 0) return;
  memcpy(x->pool + x->pool_len, s, n);
  x->pool_len += n;
}

static void pool_putc(tcat_idx *x, char ch) { pool_put(x, &ch, 1); }

static void pool_puts(tcat_idx *x, const char *s) { pool_put(x, s, strlen(s)); }

static uint32_t pool_mark(const tcat_idx *x) { return (uint32_t)x->pool_len; }

/* Ends the string begun at `mark` (adds its NUL). Offset 0 is always "". */
static pstr pool_end(tcat_idx *x, uint32_t mark) {
  pstr r = { 0, 0 };
  if (pool_reserve(x, 1) != 0) return r;
  if (x->pool_len == mark) return r;   /* empty: share the "" at offset 0 */
  x->pool[x->pool_len++] = 0;
  r.off = mark;
  r.len = (uint32_t)(x->pool_len - 1 - mark);
  return r;
}

static pstr pool_add(tcat_idx *x, const char *s, size_t n) {
  uint32_t m = pool_mark(x);
  pool_put(x, s, n);
  return pool_end(x, m);
}

/* ---- hash indexes ---------------------------------------------------------- */

typedef pstr (*key_fn)(const tcat_idx *x, uint32_t i);
static pstr key_rel_id(const tcat_idx *x, uint32_t i) { return x->rels[i].id; }
static pstr key_rel_file(const tcat_idx *x, uint32_t i) { return x->rels[i].filename; }
static pstr key_game_id(const tcat_idx *x, uint32_t i) { return x->games[i].id; }

static int ht_init(htab *t, uint32_t n) {
  uint32_t cap = 16;
  while (cap < n * 2u && cap < (1u << 30)) cap <<= 1;
  t->slot = calloc(cap, sizeof *t->slot);
  t->mask = cap - 1;
  return t->slot ? 0 : -1;
}

/* Index of the entry whose key is k (klen bytes), or -1. */
static int64_t ht_get(const tcat_idx *x, const htab *t, key_fn kf, const char *k, size_t klen) {
  if (!t->slot) return -1;
  uint32_t h = fnv1a_add(FNV_OFFSET, k, klen) & t->mask;
  for (uint32_t probe = 0; probe <= t->mask; probe++, h = (h + 1) & t->mask) {
    uint32_t s = t->slot[h];
    if (!s) return -1;
    pstr key = kf(x, s - 1);
    if (key.len == klen && memcmp(P(x, key), k, klen) == 0) return (int64_t)(s - 1);
  }
  return -1;
}

/* Adds entry i under its key unless the key is there already; with
 * first_seq, a release that comes earlier in catalogue order replaces it. */
static void ht_put(tcat_idx *x, htab *t, key_fn kf, uint32_t i, int first_seq) {
  pstr key = kf(x, i);
  uint32_t h = fnv1a_add(FNV_OFFSET, P(x, key), key.len) & t->mask;
  for (uint32_t probe = 0; probe <= t->mask; probe++, h = (h + 1) & t->mask) {
    uint32_t s = t->slot[h];
    if (!s) { t->slot[h] = i + 1; return; }
    pstr k2 = kf(x, s - 1);
    if (k2.len == key.len && memcmp(P(x, k2), P(x, key), key.len) == 0) {
      if (first_seq && x->rels[i].seq < x->rels[s - 1].seq) t->slot[h] = i + 1;
      return;
    }
  }
}

/* ---- reading the catalogue (sparse "defaults" encoding) ---------------------
 * A member missing from a game or release reads from the root's
 * defaults.game / defaults.release, then as catalog.c reads it: "" for
 * strings, 0 for numbers, except kind "game" and artworkLayout "wide"
 * (tools/catalog/schema.py expand_catalog; tests/host/tv_golden.py expand). */

static jv mem(jv obj, jv defs, const char *k) {
  jv v = jv_get(obj, k);
  if (!jv_ok(v)) v = jv_get(defs, k);
  return v;
}

/* A string member: the text, "" for a non-string, `absent` when missing. */
static const char *mstr(jv obj, jv defs, const char *k, const char *absent, size_t *len) {
  jv v = mem(obj, defs, k);
  if (!jv_ok(v)) { *len = strlen(absent); return absent; }
  if (jv_type(v) != JV_STR) { *len = 0; return ""; }
  *len = jv_strlen(v);
  return jv_str(v, "");
}

static int str_eq(const char *s, size_t n, const char *lit) { return n == strlen(lit) && memcmp(s, lit, n) == 0; }

/* An integer member, read exactly as catalog.c reads it (jv_i64 with 0: a
 * float is truncated, a bool is 0/1, a numeric string is parsed). */
static int64_t mint(jv obj, jv defs, const char *k) { return jv_i64(mem(obj, defs, k), 0); }

/* R1 rule 5: [A-Za-z0-9._-]{1,95}, not "." or "..". */
static int safe_game_id(const char *s, size_t n) {
  if (n < 1 || n > TVCAT_GAME_ID_MAX) return 0;
  if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-'))
      return 0;
  }
  return 1;
}

static int is_digit(char ch) { return ch >= '0' && ch <= '9'; }

/* Exactly YYYY-MM-DD (ASCII digits). */
static int is_date(const char *s, size_t n) {
  if (n != 10 || s[4] != '-' || s[7] != '-') return 0;
  for (size_t i = 0; i < 10; i++) if (i != 4 && i != 7 && !is_digit(s[i])) return 0;
  return 1;
}

/* R1 rule 3. */
static int tv_format_ok(const char *f, size_t n, int zip_on, int *is_zip) {
  static const char *const FORMATS[] = { "PKG", "FPKG", "FFPKG", "exFAT", "FFPFSC", "FFPFS" };
  *is_zip = 0;
  for (size_t i = 0; i < XS_ARRAY_LEN(FORMATS); i++) if (str_eq(f, n, FORMATS[i])) return 1;
  if (zip_on && str_eq(f, n, "ZIP")) { *is_zip = 1; return 1; }
  return 0;   /* RAR, 7z, PAGE and anything else */
}

/* First (CUSA|PPSA|PLAS|PCAS|PCJS|PLJS|PLJM|PCJM)[0-9]{5} in s; 9 chars into out. */
static int find_title_id(const char *s, size_t n, char out[10]) {
  static const char *const PREFIX[] = { "CUSA", "PPSA", "PLAS", "PCAS", "PCJS", "PLJS", "PLJM", "PCJM" };
  for (size_t i = 0; i + 9 <= n; i++) {
    for (size_t k = 0; k < XS_ARRAY_LEN(PREFIX); k++) {
      if (memcmp(s + i, PREFIX[k], 4) != 0) continue;
      if (is_digit(s[i + 4]) && is_digit(s[i + 5]) && is_digit(s[i + 6]) && is_digit(s[i + 7]) && is_digit(s[i + 8])) {
        memcpy(out, s + i, 9);
        out[9] = 0;
        return 1;
      }
    }
  }
  return 0;
}

/* Appends the UTF-8 decoding of bytes b[0..n) with invalid sequences replaced
 * by U+FFFD the way Python's bytes.decode("utf-8", "replace") does (one
 * replacement per maximal invalid subpart). */
static size_t utf8_replace(const unsigned char *b, size_t n, char *out) {
  size_t o = 0, i = 0;
  while (i < n) {
    unsigned char c = b[i];
    if (c < 0x80) { out[o++] = (char)c; i++; continue; }
    int need;
    unsigned char lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) need = 1;
    else if (c == 0xE0) { need = 2; lo = 0xA0; }
    else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) need = 2;
    else if (c == 0xED) { need = 2; hi = 0x9F; }
    else if (c == 0xF0) { need = 3; lo = 0x90; }
    else if (c >= 0xF1 && c <= 0xF3) need = 3;
    else if (c == 0xF4) { need = 3; hi = 0x8F; }
    else need = 0;
    size_t k = 1;
    if (need) {
      while ((int)k <= need && i + k < n) {
        unsigned char cb = b[i + k];
        unsigned char l = k == 1 ? lo : 0x80, h = k == 1 ? hi : 0xBF;
        if (cb < l || cb > h) break;
        k++;
      }
    }
    if (need && (int)k == need + 1) {
      memcpy(out + o, b + i, k);
      o += k;
      i += k;
    } else {
      out[o++] = (char)0xEF; out[o++] = (char)0xBF; out[o++] = (char)0xBD;
      i += k;
    }
  }
  return o;
}

static int hexval(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

/* Python's urllib.parse.unquote(): %XX inside each run of ASCII characters
 * becomes a byte and the run is decoded as UTF-8 with replacement; other
 * characters (and a '%' without two hex digits) stay as they are; '+' stays.
 * out needs room for n + 1 bytes. Returns the length. */
static size_t py_unquote(const char *s, size_t n, char *out, unsigned char *tmp) {
  if (!memchr(s, '%', n)) { memcpy(out, s, n); out[n] = 0; return n; }
  size_t o = 0, i = 0;
  while (i < n) {
    if ((unsigned char)s[i] >= 0x80) { out[o++] = s[i++]; continue; }
    size_t j = i, t = 0;
    while (j < n && (unsigned char)s[j] < 0x80) {
      if (s[j] == '%' && j + 2 < n && hexval(s[j + 1]) >= 0 && hexval(s[j + 2]) >= 0) {
        tmp[t++] = (unsigned char)((hexval(s[j + 1]) << 4) | hexval(s[j + 2]));
        j += 3;
      } else {
        tmp[t++] = (unsigned char)s[j++];
      }
    }
    o += utf8_replace(tmp, t, out + o);
    i = j;
  }
  out[o] = 0;
  return o;
}

static int scheme_char(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || is_digit(ch) || ch == '+' || ch == '-' || ch == '.';
}

/* The percent-decoded path of a URL as tv_golden.py's _url_path() reads it:
 * unquote(urlsplit(url).path) — leading C0/space stripped, TAB/CR/LF removed,
 * scheme and netloc dropped, query and fragment cut, "" for a malformed IPv6
 * netloc. Returns a malloc'd string (NULL on OOM) and its length. */
static char *url_path_decoded(const char *url, size_t n, size_t *out_len) {
  char *clean = malloc(n + 1);
  if (!clean) return NULL;
  size_t m = 0, i = 0;
  while (i < n && (unsigned char)url[i] <= 0x20) i++;
  for (; i < n; i++) if (url[i] != '\t' && url[i] != '\r' && url[i] != '\n') clean[m++] = url[i];
  size_t p = 0;
  const char *colon = memchr(clean, ':', m);
  if (colon && colon > clean && ((clean[0] >= 'a' && clean[0] <= 'z') || (clean[0] >= 'A' && clean[0] <= 'Z'))) {
    size_t c = (size_t)(colon - clean), k = 0;
    while (k < c && scheme_char(clean[k])) k++;
    if (k == c) p = c + 1;
  }
  size_t ps = p, pe;
  if (m - p >= 2 && clean[p] == '/' && clean[p + 1] == '/') {
    size_t j = p + 2;
    while (j < m && clean[j] != '/' && clean[j] != '?' && clean[j] != '#') j++;
    int lb = memchr(clean + p + 2, '[', j - p - 2) != NULL, rb = memchr(clean + p + 2, ']', j - p - 2) != NULL;
    ps = j;
    if (lb != rb) ps = m;   /* urlsplit raises: the golden reads "" */
  }
  pe = ps;
  while (pe < m && clean[pe] != '?' && clean[pe] != '#') pe++;
  char *out = malloc(pe - ps + 1);
  unsigned char *tmp = malloc(pe - ps + 1);
  if (!out || !tmp) { free(out); free(tmp); free(clean); return NULL; }
  *out_len = py_unquote(clean + ps, pe - ps, out, tmp);
  free(tmp);
  free(clean);
  return out;
}

/* The title ID source (see tvapi.h tv_release_title_id): fills *s and *n with
 * the text to upper-case. Uses scratch for a match found in a decoded URL path. */
static void title_id_source(jv game, jv rel, jv dg, jv dr, char scratch[10], const char **s, size_t *n) {
  size_t len;
  const char *t = mstr(rel, dr, "titleId", "", &len);
  if (len) { *s = t; *n = len; return; }
  jv tids = mem(game, dg, "titleIds");
  if (jv_type(tids) == JV_ARR) {
    jv first = jv_first(tids);
    if (jv_type(first) == JV_STR && jv_strlen(first) > 0) { *s = jv_str(first, ""); *n = jv_strlen(first); return; }
  }
  const char *fn = mstr(rel, dr, "filename", "", &len);
  if (find_title_id(fn, len, scratch)) { *s = scratch; *n = 9; return; }
  const char *url = mstr(rel, dr, "url", "", &len);
  if (len) {
    size_t plen = 0;
    char *path = url_path_decoded(url, len, &plen);
    int hit = path && find_title_id(path, plen, scratch);
    free(path);
    if (hit) { *s = scratch; *n = 9; return; }
  }
  *s = "";
  *n = 0;
}

static char ascii_upper(char ch) { return (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch; }

void tv_release_title_id(jv game, jv rel, char *out, size_t cap) {
  if (!cap) return;
  jv none = { NULL, 0xFFFFFFFFu };
  char scratch[10];
  const char *s;
  size_t n;
  title_id_source(game, rel, none, none, scratch, &s, &n);
  size_t i = 0;
  for (; i < n && i + 1 < cap; i++) out[i] = ascii_upper(s[i]);
  out[i] = 0;
}

/* ---- the build walk --------------------------------------------------------- */

typedef struct feat_cand { pstr id; int64_t featured; uint32_t cat_index; } feat_cand;

typedef struct build_ctx {
  tcat_idx *x;
  int zip;
  int walked;
  feat_cand *feat;
  uint32_t nfeat, feat_cap;
  uint32_t seq;
} build_ctx;

/* Returns arr grown to hold `need` elements (the same pointer when it already
 * does), or NULL when out of memory (arr is then still valid and owned). */
static void *grow_to(void *arr, uint32_t *cap, uint32_t need, size_t elem) {
  if (need <= *cap) return arr;
  uint32_t ncap = *cap ? *cap * 2 : 1024;
  while (ncap < need) ncap *= 2;
  void *na = realloc(arr, (size_t)ncap * elem);
  if (na) *cap = ncap;
  return na;
}

static int src_find(const tcat_idx *x, const char *s, size_t n) {
  for (int i = 0; i < x->nsrc; i++) if (x->src[i].id.len == n && memcmp(P(x, x->src[i].id), s, n) == 0) return i;
  return -1;
}

/* Exposed sources (R1 rule 2): kind "direct" or "browser", not "pkgzone". */
static void build_sources(tcat_idx *x, jv root) {
  JV_FOREACH(s, jv_get(root, "sources")) {
    if (jv_type(s) != JV_OBJ) continue;
    jv id = jv_get(s, "id"), kind = jv_get(s, "kind");
    if (jv_type(id) != JV_STR || jv_type(kind) != JV_STR) continue;
    const char *k = jv_str(kind, "");
    int browser = strcmp(k, "browser") == 0;
    if (!browser && strcmp(k, "direct") != 0) continue;
    if (str_eq(jv_str(id, ""), jv_strlen(id), "pkgzone")) continue;   /* homebrew only */
    if (src_find(x, jv_str(id, ""), jv_strlen(id)) >= 0 || x->nsrc >= TVCAT_MAX_SOURCES || jv_strlen(id) >= 32) continue;
    tc_src *e = &x->src[x->nsrc++];
    e->id = pool_add(x, jv_str(id, ""), jv_strlen(id));
    jv name = jv_get(s, "name");
    e->name = jv_type(name) == JV_STR ? pool_add(x, jv_str(name, ""), jv_strlen(name)) : (pstr){ 0, 0 };
    e->browser = browser;
    e->count = 0;
  }
}

static int kind_rank(const char *k, size_t n) {
  if (n == 0 || str_eq(k, n, "game")) return 0;
  if (str_eq(k, n, "backport")) return 1;
  if (str_eq(k, n, "update")) return 2;
  if (str_eq(k, n, "dlc")) return 3;
  return 9;
}

/* One visible release: maps every option field into the pool. */
static void add_release(build_ctx *b, jv g, jv r, jv dg, jv dr, int src, int is_zip, int64_t size, uint32_t game_index) {
  tcat_idx *x = b->x;
  tc_rel *nr = grow_to(x->rels, &x->rels_cap, x->nrels + 1, sizeof *x->rels);
  if (!nr) { x->oom = 1; return; }
  x->rels = nr;
  tc_rel *e = &x->rels[x->nrels];
  memset(e, 0, sizeof *e);
  size_t len;
  const char *s;

  s = mstr(r, dr, "id", "", &len);
  e->id = pool_add(x, s, len);

  char scratch[10];
  const char *tid;
  size_t tlen;
  title_id_source(g, r, dg, dr, scratch, &tid, &tlen);
  uint32_t m = pool_mark(x);
  for (size_t i = 0; i < tlen; i++) pool_putc(x, ascii_upper(tid[i]));
  e->titleId = pool_end(x, m);
#if TV_REQUIRE_TITLE_ID
  if (!tlen) return;   /* hidden: the slot is reused by the next release */
#endif

  if (is_zip) e->format = pool_add(x, "Unknown", 7);
  else { s = mstr(r, dr, "format", "", &len); e->format = pool_add(x, s, len); }
  e->zip = (uint8_t)is_zip;

  s = mstr(r, dr, "filename", "", &len);
  if (len) e->filename = pool_add(x, s, len);
  else {
    size_t ulen;
    const char *url = mstr(r, dr, "url", "", &ulen);
    size_t plen = 0;
    char *path = url_path_decoded(url, ulen, &plen);
    if (!path) { x->oom = 1; return; }
    const char *base = path + plen;
    while (base > path && base[-1] != '/') base--;
    e->filename = pool_add(x, base, plen - (size_t)(base - path));
    free(path);
  }

  const char *ver = mstr(r, dr, "version", "", &len);
  size_t vlen = len;
  e->version = pool_add(x, ver, vlen);

  s = mstr(r, dr, "region", "", &len);
  if (!len) s = mstr(g, dg, "region", "", &len);
  e->region = pool_add(x, s, len);

  size_t klen;
  const char *kind = mstr(r, dr, "kind", "game", &klen);
  e->kind = klen ? pool_add(x, kind, klen) : pool_add(x, "game", 4);

  size_t nlen;
  const char *notes = mstr(r, dr, "notes", "", &nlen);
  const char *pre = NULL;
  m = pool_mark(x);
  if (str_eq(kind, klen, "update")) {
    if (vlen) { pool_puts(x, NOTE_UPDATE_PRE); pool_put(x, ver, vlen); pool_puts(x, NOTE_UPDATE_POST); }
    else pool_puts(x, NOTE_UPDATE_NO_VERSION);
    pre = "";
  } else if (str_eq(kind, klen, "dlc")) {
    pool_puts(x, NOTE_DLC);
    pre = "";
  } else if (str_eq(kind, klen, "backport")) {
    pool_puts(x, NOTE_BACKPORT);
    pre = "";
  }
  if (pre && nlen) pool_putc(x, ' ');
  pool_put(x, notes, nlen);
  e->notes = pool_end(x, m);

  s = mstr(r, dr, "provider", "", &len);
  e->provider = pool_add(x, s, len);
  e->sourceId = x->src[src].id;
  e->size = size;
  e->game = game_index;
  e->seq = b->seq++;
  e->src = (uint16_t)src;

  size_t hlen, valen;
  const char *health = mstr(r, dr, "health", "", &hlen);
  mstr(r, dr, "verifiedAt", "", &valen);
  e->sort = (uint8_t)((kind_rank(kind, klen) << 3) | (x->src[src].browser << 2) | (str_eq(health, hlen, "flaky") << 1) | (valen ? 0 : 1));
  x->src[src].count++;
  x->nrels++;
}

static void add_game(build_ctx *b, jv g, jv dg, uint32_t first, uint32_t cat_index) {
  tcat_idx *x = b->x;
  tc_game *grown = grow_to(x->games, &x->games_cap, x->ngames + 1, sizeof *x->games);
  if (!grown) { x->oom = 1; return; }
  x->games = grown;
  tc_game *e = &x->games[x->ngames];
  memset(e, 0, sizeof *e);
  size_t len;
  const char *s;
  s = mstr(g, dg, "id", "", &len);
  e->id = pool_add(x, s, len);
  s = mstr(g, dg, "title", "", &len);
  e->title = pool_add(x, s, len);
  s = mstr(g, dg, "platform", "", &len);
  e->platform = pool_add(x, str_eq(s, len, "PS5") ? "ps5" : "ps4", 3);
  uint32_t m = pool_mark(x);
  int ng = 0;
  jv genres = mem(g, dg, "genres");
  if (jv_type(genres) == JV_ARR) {
    JV_FOREACH(it, genres) {
      if (jv_type(it) != JV_STR) continue;
      if (ng++) pool_puts(x, " / ");
      pool_put(x, jv_str(it, ""), jv_strlen(it));
    }
  }
  e->genre = pool_end(x, m);
  s = mstr(g, dg, "tagline", "", &len);
  e->tagline = pool_add(x, s, len);
  s = mstr(g, dg, "description", "", &len);
  e->description = pool_add(x, s, len);
  s = mstr(g, dg, "publisher", "", &len);
  e->publisher = pool_add(x, s, len);
  s = mstr(g, dg, "releaseDate", "", &len);
  e->releaseDate = is_date(s, len) ? pool_add(x, s, len) : (pstr){ 0, 0 };
  s = mstr(g, dg, "addedAt", "", &len);
  if (is_date(s, len)) {
    m = pool_mark(x);
    pool_put(x, s, len);
    pool_puts(x, "T00:00:00Z");
    e->addedAt = pool_end(x, m);
  }
  s = mstr(g, dg, "artworkLayout", "wide", &len);
  e->artworkLayout = pool_add(x, s, len);
  e->popularity = jv_dbl(mem(g, dg, "popularity"), 0);   /* as catalog.c reads it */
  int64_t cheats = mint(g, dg, "cheats");
  e->cheats = cheats > 0 ? cheats : 0;
  jv shots = mem(g, dg, "screenshots");
  if (jv_type(shots) == JV_ARR) {   /* the rule of tvapi.h TV_ART_MAX_SHOTS, as tvapi_art.c indexes them */
    JV_FOREACH(it, shots) {
      if (e->shots >= TV_ART_MAX_SHOTS) break;
      if (jv_type(it) == JV_STR && tvart_shot_usable(jv_str(it, ""))) e->shots++;
    }
  }
  e->first = first;
  e->n = x->nrels - first;
  e->cat_index = cat_index;
  x->ngames++;
}

/* catalog_visit_root callback: runs under catalog_mu. Only this file's own
 * data structures are touched here (no catalog_* / settings_* calls). */
static void build_walk(jv root, void *user) {
  build_ctx *b = user;
  tcat_idx *x = b->x;
  b->walked = 1;
  x->revision = jv_get_i64(root, "revision", 0);   /* as catalog.c reads it */
  jv defs = jv_get(root, "defaults");
  jv dg = jv_get(defs, "game"), dr = jv_get(defs, "release");
  build_sources(x, root);
  uint32_t cat_index = 0;
  JV_FOREACH(g, jv_get(root, "games")) {
    uint32_t gi = cat_index++;
    if (x->oom) return;
    if (jv_type(g) != JV_OBJ) continue;
    size_t idlen, plen;
    const char *gid = mstr(g, dg, "id", "", &idlen);
    if (!safe_game_id(gid, idlen)) continue;
    int64_t featured = mint(g, dg, "featured");
    if (featured > 0) {
      feat_cand *nf = grow_to(b->feat, &b->feat_cap, b->nfeat + 1, sizeof *b->feat);
      if (!nf) { x->oom = 1; return; }
      b->feat = nf;
      b->feat[b->nfeat].id = pool_add(x, gid, idlen);
      b->feat[b->nfeat].featured = featured;
      b->feat[b->nfeat].cat_index = gi;
      b->nfeat++;
    }
    const char *plat = mstr(g, dg, "platform", "", &plen);
    if (!str_eq(plat, plen, "PS5") && !str_eq(plat, plen, "PS4")) continue;   /* rule 1 */
    jv rels = jv_get(g, "releases");
    if (jv_type(rels) != JV_ARR) continue;
    uint32_t first = x->nrels;
    JV_FOREACH(r, rels) {
      if (jv_type(r) != JV_OBJ) continue;
      size_t len;
      const char *sid = mstr(r, dr, "sourceId", "", &len);
      int src = src_find(x, sid, len);
      if (src < 0) continue;                                                   /* rule 2 */
      const char *fmt = mstr(r, dr, "format", "", &len);
      int is_zip;
      if (!tv_format_ok(fmt, len, b->zip, &is_zip)) continue;                  /* rule 3 */
      int64_t size = mint(r, dr, "sizeBytes");
      if (size <= 0) continue;                                                 /* rule 4 */
      add_release(b, g, r, dg, dr, src, is_zip, size, x->ngames);
      if (x->oom) return;
    }
    if (x->nrels > first) add_game(b, g, dg, first, gi);
  }
}

static int rel_cmp(const void *a, const void *b) {
  const tc_rel *x = a, *y = b;
  if (x->sort != y->sort) return x->sort < y->sort ? -1 : 1;
  return x->seq < y->seq ? -1 : (x->seq > y->seq ? 1 : 0);
}

static int feat_cmp(const void *a, const void *b) {
  const feat_cand *x = a, *y = b;
  if (x->featured != y->featured) return x->featured < y->featured ? -1 : 1;
  return x->cat_index < y->cat_index ? -1 : (x->cat_index > y->cat_index ? 1 : 0);
}

/* The featured feed's hero ids: the hash covers every string item in feed
 * order (each id followed by one 0 byte), the copy keeps those that can be a
 * game id. ids may be NULL (hash only). */
static uint32_t hero_read(char (*ids)[TVCAT_GAME_ID_MAX + 1], int *n) {
  uint32_t h = FNV_OFFSET;
  if (n) *n = 0;
  jdoc *d = cx_feed_doc(CX_FEED_FEATURED);
  if (!d) return h;
  jv hero = jv_get(json_root(d), "hero");
  if (jv_type(hero) == JV_ARR) {
    JV_FOREACH(it, hero) {
      if (jv_type(it) != JV_STR) continue;
      const char *s = jv_str(it, "");
      size_t len = jv_strlen(it);
      h = fnv1a_add(h, s, len);
      h = fnv1a_add(h, "", 1);
      if (ids && n && len >= 1 && len <= TVCAT_GAME_ID_MAX && strlen(s) == len && *n < TVCAT_MAX_HERO) {
        memcpy(ids[*n], s, len);
        ids[*n][len] = 0;
        (*n)++;
      }
    }
  }
  json_free(d);
  return h;
}

static int64_t compose_revision(int64_t revision, uint32_t hero_hash) {
  return ((revision & ((INT64_C(1) << TVCAT_REV_BITS) - 1)) << 20) | (int64_t)(hero_hash & 0xFFFFFu);
}

typedef struct pop_key { double popularity; int ps4; uint32_t game; } pop_key;

/* popularRank order: the catalogue popularity the web UI ranks its Top PS5
 * and Trending rows and its showcase by (web/src/js/10-state.js, 40-discover.js),
 * highest first; at equal popularity PS5 before PS4 (Trending), then
 * catalogue order. */
static int pop_cmp(const void *a, const void *b) {
  const pop_key *x = a, *y = b;
  if (x->popularity != y->popularity) return x->popularity > y->popularity ? -1 : 1;
  if (x->ps4 != y->ps4) return x->ps4 - y->ps4;
  return x->game < y->game ? -1 : (x->game > y->game ? 1 : 0);
}

/* Ranks (newOnOrbitRank): the feed's hero ids in feed order, then games with
 * catalogue featured > 0 by (featured, catalogue order); TV-visible games
 * only, each once, at most 100, over every exposed source. popularRank: every
 * TV-visible game id once, 1..game_count in pop_cmp order (a repeated id
 * takes its first visible entry's). */
static int build_ranks(tcat_idx *x, build_ctx *b, char (*hero)[TVCAT_GAME_ID_MAX + 1], int nhero) {
  htab gids = { NULL, 0 };
  int *rank_of = calloc(x->ngames ? x->ngames : 1, sizeof *rank_of);   /* by first game index of an id */
  int *pop_of = calloc(x->ngames ? x->ngames : 1, sizeof *pop_of);
  pop_key *pk = malloc((x->ngames ? x->ngames : 1) * sizeof *pk);
  if (!rank_of || !pop_of || !pk || ht_init(&gids, x->ngames) != 0) {
    free(rank_of);
    free(pop_of);
    free(pk);
    free(gids.slot);
    return -1;
  }
  for (uint32_t i = 0; i < x->ngames; i++) {
    const tc_game *g = &x->games[i];
    if (ht_get(x, &gids, key_game_id, P(x, g->id), g->id.len) >= 0) continue;
    ht_put(x, &gids, key_game_id, i, 0);
    pk[x->game_count].popularity = g->popularity;
    pk[x->game_count].ps4 = !str_eq(P(x, g->platform), g->platform.len, "ps5");
    pk[x->game_count].game = i;
    x->game_count++;
  }
  if (x->game_count > 1) qsort(pk, (size_t)x->game_count, sizeof *pk, pop_cmp);
  for (int k = 0; k < x->game_count; k++) pop_of[pk[k].game] = k + 1;
  int next = 1;
  for (int i = 0; i < nhero && next <= TVCAT_MAX_RANK; i++) {
    int64_t gi = ht_get(x, &gids, key_game_id, hero[i], strlen(hero[i]));
    if (gi >= 0 && !rank_of[gi]) rank_of[gi] = next++;
  }
  if (b->nfeat) qsort(b->feat, b->nfeat, sizeof *b->feat, feat_cmp);
  for (uint32_t i = 0; i < b->nfeat && next <= TVCAT_MAX_RANK; i++) {
    int64_t gi = ht_get(x, &gids, key_game_id, P(x, b->feat[i].id), b->feat[i].id.len);
    if (gi >= 0 && !rank_of[gi]) rank_of[gi] = next++;
  }
  x->ranked = next - 1;
  for (uint32_t i = 0; i < x->ngames; i++) {
    int64_t gi = ht_get(x, &gids, key_game_id, P(x, x->games[i].id), x->games[i].id.len);
    x->games[i].rank = gi >= 0 ? rank_of[gi] : 0;
    x->games[i].popular_rank = gi >= 0 ? pop_of[gi] : 0;
  }
  free(rank_of);
  free(pop_of);
  free(pk);
  free(gids.slot);
  return 0;
}

static tcat_idx *idx_build(unsigned runs, time_t feed_at, int zip) {
  int64_t t0 = util_now_ms();
  tcat_idx *x = calloc(1, sizeof *x);
  char (*hero)[TVCAT_GAME_ID_MAX + 1] = malloc(TVCAT_MAX_HERO * sizeof *hero);
  if (!x || !hero) { free(x); free(hero); return NULL; }
  x->runs = runs;
  x->feed_at = feed_at;
  x->zip = zip;
  pool_putc(x, 0);   /* offset 0: the shared "" */
  int nhero = 0;
  x->hero_hash = hero_read(hero, &nhero);
  build_ctx b;
  memset(&b, 0, sizeof b);
  b.x = x;
  b.zip = zip;
  if (catalog_visit_root(build_walk, &b) != 0) LOGW("tvapi: no catalogue loaded; the TV catalogue is empty");
  int64_t t_walk = util_now_ms() - t0;
  if (!x->oom) {
    for (uint32_t i = 0; i < x->ngames; i++)
      if (x->games[i].n > 1) qsort(x->rels + x->games[i].first, x->games[i].n, sizeof *x->rels, rel_cmp);
    if (ht_init(&x->by_id, x->nrels) != 0 || ht_init(&x->by_file, x->nrels) != 0) x->oom = 1;
  }
  if (!x->oom) {
    for (uint32_t i = 0; i < x->nrels; i++) {
      if (x->rels[i].id.len) ht_put(x, &x->by_id, key_rel_id, i, 1);
      if (x->rels[i].filename.len) ht_put(x, &x->by_file, key_rel_file, i, 1);
    }
    if (build_ranks(x, &b, hero, nhero) != 0) x->oom = 1;
  }
  free(b.feat);
  free(hero);
  if (x->oom) {
    LOGE("tvapi: out of memory building the TV catalogue index");
    idx_free(x);
    return NULL;
  }
  x->composite = compose_revision(x->revision, x->hero_hash);
  LOGI("tvapi: TV catalogue index: revision %lld, %u option(s), %d game(s), %d ranked, %zu KB (walk %lld ms, total %lld ms)",
       (long long)x->revision, x->nrels, x->game_count, x->ranked, x->pool_len >> 10, (long long)t_walk,
       (long long)(util_now_ms() - t0));
  return x;
}

/* ---- getting a snapshot ----------------------------------------------------- */

enum { IDX_FRESH = 0, IDX_ANY = 1 };

static int idx_fresh(const tcat_idx *x, unsigned runs, time_t feed_at, int zip) {
  return x && x->runs == runs && x->feed_at == feed_at && x->zip == zip;
}

/* A referenced snapshot (idx_unref() it), or NULL when none could be built.
 * IDX_FRESH rebuilds a stale one; IDX_ANY builds only when there is none. */
static tcat_idx *idx_get(int mode) {
  unsigned runs = catalog_refresh_runs();
  time_t feed_at = cx_feed_fetched_at(CX_FEED_FEATURED);
  int zip = show_zip();
  pthread_mutex_lock(&tvcat_mu);
  tcat_idx *x = g_idx;
  if (x && (mode == IDX_ANY || idx_fresh(x, runs, feed_at, zip))) { x->refs++; pthread_mutex_unlock(&tvcat_mu); return x; }
  pthread_mutex_unlock(&tvcat_mu);

  pthread_mutex_lock(&build_mu);
  pthread_mutex_lock(&tvcat_mu);
  x = g_idx;
  if (x && (mode == IDX_ANY || idx_fresh(x, runs, feed_at, zip))) {   /* another caller just built it */
    x->refs++;
    pthread_mutex_unlock(&tvcat_mu);
    pthread_mutex_unlock(&build_mu);
    return x;
  }
  pthread_mutex_unlock(&tvcat_mu);
  tcat_idx *nx = idx_build(runs, feed_at, zip);
  tcat_idx *old = NULL;
  pthread_mutex_lock(&tvcat_mu);
  if (nx) {
    old = g_idx;
    g_idx = nx;
    nx->refs = 2;   /* the current one + the caller's */
    g_rev.valid = 1;
    g_rev.runs = runs;
    g_rev.feed_at = feed_at;
    g_rev.value = nx->composite;
    if (old && --old->refs > 0) old = NULL;   /* a reader still holds it */
  } else if (g_idx) {
    nx = g_idx;   /* out of memory: keep serving the previous snapshot */
    nx->refs++;
  }
  pthread_mutex_unlock(&tvcat_mu);
  pthread_mutex_unlock(&build_mu);
  idx_free(old);
  return nx;
}

/* ==== cross-subsystem API (tvapi.h) ========================================= */

static void ensure_any(void) {
  pthread_mutex_lock(&tvcat_mu);
  int have = g_idx != NULL;
  pthread_mutex_unlock(&tvcat_mu);
  if (!have) idx_unref(idx_get(IDX_ANY));
}

/* Caller holds tvcat_mu. */
static void rel_fill_locked(const tcat_idx *x, const tc_rel *r, tvcat_rel *out) {
  const tc_game *g = &x->games[r->game];
  memset(out, 0, sizeof *out);
  xstrlcpy(out->id, P(x, r->id), sizeof out->id);
  xstrlcpy(out->gameId, P(x, g->id), sizeof out->gameId);
  xstrlcpy(out->title, P(x, g->title), sizeof out->title);
  xstrlcpy(out->platform, P(x, g->platform), sizeof out->platform);
  xstrlcpy(out->titleId, P(x, r->titleId), sizeof out->titleId);
  xstrlcpy(out->format, P(x, r->format), sizeof out->format);
  xstrlcpy(out->compression, r->zip ? "zip" : "", sizeof out->compression);
  xstrlcpy(out->filename, P(x, r->filename), sizeof out->filename);
  xstrlcpy(out->sourceId, P(x, r->sourceId), sizeof out->sourceId);
  xstrlcpy(out->kind, P(x, r->kind), sizeof out->kind);
  xstrlcpy(out->version, P(x, r->version), sizeof out->version);
  xstrlcpy(out->region, P(x, r->region), sizeof out->region);
  out->sizeBytes = r->size;
  out->extractedBytes = r->zip ? r->size : 0;
  out->browserAvailable = x->src[r->src].browser;
  out->directAvailable = !x->src[r->src].browser;
}

static int lookup(const htab *(*which)(const tcat_idx *), key_fn kf, const char *key, tvcat_rel *out, char *gid, size_t gcap) {
  if (!key || !*key) return -1;
  ensure_any();
  int rc = -1;
  pthread_mutex_lock(&tvcat_mu);
  const tcat_idx *x = g_idx;
  if (x) {
    int64_t i = ht_get(x, which(x), kf, key, strlen(key));
    if (i >= 0) {
      rc = 0;
      if (out) rel_fill_locked(x, &x->rels[i], out);
      if (gid) xstrlcpy(gid, P(x, x->games[x->rels[i].game].id), gcap);
    }
  }
  pthread_mutex_unlock(&tvcat_mu);
  return rc;
}

static const htab *tab_by_id(const tcat_idx *x) { return &x->by_id; }
static const htab *tab_by_file(const tcat_idx *x) { return &x->by_file; }

int tvcat_release(const char *releaseId, tvcat_rel *out) {
  return lookup(tab_by_id, key_rel_id, releaseId, out, NULL, 0);
}

int tvcat_by_filename(const char *filename, tvcat_rel *out) {
  return lookup(tab_by_file, key_rel_file, filename, out, NULL, 0);
}

int tv_find_release(const char *releaseId, char *gameId_out, size_t cap) {
  if (cap) gameId_out[0] = 0;
  return lookup(tab_by_id, key_rel_id, releaseId, NULL, cap ? gameId_out : NULL, cap);
}

int tvcat_sources_acknowledged(void) { return settings_sources_acknowledged(); }

/* GET /system: memory only. Recomputed (catalog_revision() and a parse of the
 * in-memory featured feed) only after a refresh ended or the feed reloaded;
 * a rebuild of the index refreshes it as well. Never walks the catalogue,
 * and never waits behind a walk: while another thread rebuilds the index
 * (holding catalog_mu for the walk) the previous value is answered, and the
 * rebuild stores the new one for the next poll. */
int64_t tvcat_catalogue_revision(void) {
  unsigned runs = catalog_refresh_runs();
  time_t feed_at = cx_feed_fetched_at(CX_FEED_FEATURED);
  pthread_mutex_lock(&tvcat_mu);
  int valid = g_rev.valid;
  int64_t v = g_rev.value;
  int fresh = valid && g_rev.runs == runs && g_rev.feed_at == feed_at;
  pthread_mutex_unlock(&tvcat_mu);
  if (fresh) return v;
  if (pthread_mutex_trylock(&build_mu) != 0) {
    if (valid) return v;                      /* a rebuild is running: it brings the new value */
    pthread_mutex_lock(&build_mu);            /* the very first value: wait for that build */
    pthread_mutex_lock(&tvcat_mu);
    valid = g_rev.valid;
    v = g_rev.value;
    pthread_mutex_unlock(&tvcat_mu);
    if (valid) { pthread_mutex_unlock(&build_mu); return v; }
  }
  /* build_mu held: no walk of ours holds catalog_mu now */
  uint32_t h = hero_read(NULL, NULL);
  v = compose_revision(catalog_revision(), h);
  pthread_mutex_lock(&tvcat_mu);
  g_rev.valid = 1;
  g_rev.runs = runs;
  g_rev.feed_at = feed_at;
  g_rev.value = v;
  pthread_mutex_unlock(&tvcat_mu);
  pthread_mutex_unlock(&build_mu);
  return v;
}

void tvcat_start(void) {
  /* Nothing to start: the index is built by the first request that needs it
   * (a warm-up walk here would hold catalog_mu under the first GET /system). */
  LOGD("tvapi: catalogue ready (TV_SHOW_ZIP %d, TV_REQUIRE_TITLE_ID %d)", show_zip(), TV_REQUIRE_TITLE_ID);
}

/* ==== /api/v1/sources ======================================================= */

static void kv_p(jw *w, const char *k, const tcat_idx *x, pstr s) {
  jw_key(w, k);
  jw_strn(w, P(x, s), s.len);
}

/* The GET object (also the POST reply). Never fails short of memory: with no
 * catalogue there are simply no options. Key order of tv_golden.sources(). */
static void sources_reply(api_ctx *c) {
  tcat_idx *x = idx_get(IDX_FRESH);
  int on[TVCAT_MAX_SOURCES] = { 0 };
  if (x) for (int i = 0; i < x->nsrc; i++) on[i] = x->src[i].count > 0 && settings_source_enabled(P(x, x->src[i].id));
  int ack = settings_sources_acknowledged();
  jw w;
  tv_obj_begin(&w);
  jw_key(&w, "enabled");
  jw_arr_begin(&w);
  if (x) for (int i = 0; i < x->nsrc; i++) if (on[i]) jw_strn(&w, P(x, x->src[i].id), x->src[i].id.len);
  jw_arr_end(&w);
  jw_key(&w, "options");
  jw_arr_begin(&w);
  if (x) {
    for (int i = 0; i < x->nsrc; i++) {
      const tc_src *s = &x->src[i];
      if (s->count <= 0) continue;
      jw_obj_begin(&w);
      kv_p(&w, "id", x, s->id);
      kv_p(&w, "label", x, s->name);
      jw_kv_i64(&w, "releaseCount", s->count);
      jw_kv_bool(&w, "downloadReady", !s->browser);
      jw_obj_end(&w);
    }
  }
  jw_arr_end(&w);
  jw_kv_bool(&w, "acknowledged", ack);
  jw_kv_int(&w, "noticeVersion", TVCAT_NOTICE_VERSION);
  idx_unref(x);
  tv_obj_send(c, 200, &w);
}

/* Writes {"sources":{"enabled":{<id>:<bool>,...},"acknowledged":<bool>}} for
 * the exposed option ids only, and merges it. Returns the merge result. */
static int sources_merge(const tcat_idx *x, const int *want, int nopt_idx[], int nopt, int ack, int checked) {
  jw w;
  jw_init(&w);
  jw_obj_begin(&w);
  jw_key(&w, "sources");
  jw_obj_begin(&w);
  jw_key(&w, "enabled");
  jw_obj_begin(&w);
  for (int k = 0; k < nopt; k++) jw_kv_bool(&w, P(x, x->src[nopt_idx[k]].id), want[k]);
  jw_obj_end(&w);
  jw_kv_bool(&w, "acknowledged", ack);
  jw_obj_end(&w);
  jw_obj_end(&w);
  size_t len = 0;
  char *txt = jw_detach(&w, &len);
  if (!txt) return SETTINGS_ERR_INVALID;
  jdoc *d = json_parse(txt, len, 1000, 8);
  free(txt);
  if (!d) return SETTINGS_ERR_INVALID;
  char err[160];
  int rc = checked ? settings_merge_checked(json_root(d), err, sizeof err) : settings_merge(json_root(d), err, sizeof err);
  if (rc != 0) LOGW("tvapi: source settings not saved: %s", err);
  json_free(d);
  return rc;
}

/* POST /api/v1/sources. Validation order, sentences and the "a failed write
 * changes nothing" rule ported from Orbit Store backend/sources.c
 * sources_set_locked (GPL-3.0-or-later); the size limit is our option count. */
static void sources_post(api_ctx *c) {
  tcat_idx *x = idx_get(IDX_FRESH);
  int opt[TVCAT_MAX_SOURCES], nopt = 0;
  if (x) for (int i = 0; i < x->nsrc; i++) if (x->src[i].count > 0) opt[nopt++] = i;
  jv en = jv_get(c->body, "enabled");
  if (jv_type(en) != JV_ARR || jv_len(en) > (size_t)nopt) { idx_unref(x); tv_error(c, 400, MSG_SOURCES_SELECT); return; }
  int want[TVCAT_MAX_SOURCES] = { 0 };
  JV_FOREACH(it, en) {
    int hit = -1;
    if (jv_type(it) == JV_STR) {
      for (int k = 0; k < nopt; k++) {
        pstr id = x->src[opt[k]].id;
        if (id.len == jv_strlen(it) && memcmp(P(x, id), jv_str(it, ""), id.len) == 0) { hit = k; break; }
      }
    }
    if (hit < 0 || want[hit]) { idx_unref(x); tv_error(c, 400, MSG_SOURCES_UNKNOWN); return; }
    want[hit] = 1;
  }
  int64_t nv = 0;
  int ack = 0;
  if (!tv_body_int(c, "noticeVersion", &nv) || nv != TVCAT_NOTICE_VERSION || !tv_body_bool(c, "acknowledged", &ack) || !ack) {
    idx_unref(x);
    tv_error(c, 400, MSG_SOURCES_NOTICE);
    return;
  }
  int was[TVCAT_MAX_SOURCES] = { 0 };
  for (int k = 0; k < nopt; k++) was[k] = settings_source_enabled(P(x, x->src[opt[k]].id));
  int was_ack = settings_sources_acknowledged();
  int rc = sources_merge(x, want, opt, nopt, 1, 1);
  if (rc == SETTINGS_ERR_SAVE) {
    sources_merge(x, was, opt, nopt, was_ack, 0);   /* put the previous choice back in memory */
    idx_unref(x);
    tv_error(c, 503, MSG_SOURCES_SAVE);
    return;
  }
  if (rc != 0) { idx_unref(x); tv_error(c, 503, MSG_SOURCES_RETRY); return; }
  /* The app promises that turning a source off pauses its unfinished downloads. */
  for (int k = 0; k < nopt; k++) {
    if (!was[k] || want[k]) continue;
    int n = tvdl_pause_source(P(x, x->src[opt[k]].id));
    if (n > 0) LOGI("tvapi: source %s turned off, %d download(s) paused", P(x, x->src[opt[k]].id), n);
  }
  LOGI("tvapi: download sources saved from the TV app (%d of %d on)", (int)jv_len(en), nopt);
  idx_unref(x);
  sources_reply(c);
}

void tvcat_h_sources(api_ctx *c) {
  if (tv_is_post(c)) sources_post(c);
  else sources_reply(c);
}

/* ==== /api/v1/catalog ======================================================= */

/* What the option is, for the app to group a game's options by (R2 key 1);
 * any other catalogue kind reads as a game, as its notes do. */
static const char *wire_kind(const tc_rel *r) {
  static const char *const KINDS[] = { "game", "backport", "update", "dlc" };
  unsigned k = (unsigned)(r->sort >> 3);
  return k < XS_ARRAY_LEN(KINDS) ? KINDS[k] : "game";
}

static void write_option(jw *w, const tcat_idx *x, const tc_game *g, const tc_rel *r) {
  const tc_src *s = &x->src[r->src];
  jw_obj_begin(w);
  kv_p(w, "id", x, r->id);
  kv_p(w, "gameId", x, g->id);
  kv_p(w, "titleId", x, r->titleId);
  kv_p(w, "platform", x, g->platform);
  kv_p(w, "title", x, g->title);
  kv_p(w, "genre", x, g->genre);
  kv_p(w, "tagline", x, g->tagline);
  kv_p(w, "description", x, g->description);
  kv_p(w, "publisher", x, g->publisher);
  kv_p(w, "releaseDate", x, g->releaseDate);
  kv_p(w, "addedAt", x, g->addedAt);
  if (g->rank) jw_kv_int(w, "newOnOrbitRank", g->rank);
  jw_kv_int(w, "popularRank", g->popular_rank);
  if (g->cheats) jw_kv_i64(w, "cheats", g->cheats);
  if (g->shots) jw_kv_int(w, "screenshotCount", g->shots);
  kv_p(w, "artworkLayout", x, g->artworkLayout);
  kv_p(w, "provider", x, r->provider);
  kv_p(w, "sourceId", x, r->sourceId);
  kv_p(w, "format", x, r->format);
  if (r->zip) {
    jw_kv_str(w, "compression", "zip");
    jw_kv_str(w, "archiveContents", "unverified");
    jw_kv_i64(w, "extractedBytes", r->size);
  }
  kv_p(w, "region", x, r->region);
  kv_p(w, "version", x, r->version);
  kv_p(w, "filename", x, r->filename);
  jw_kv_str(w, "kind", wire_kind(r));
  kv_p(w, "notes", x, r->notes);
  jw_kv_i64(w, "sizeBytes", r->size);
  jw_kv_bool(w, "browserAvailable", s->browser);   /* R6: from the source kind */
  jw_kv_bool(w, "directAvailable", !s->browser);
  jw_obj_end(w);
}

/* GET /api/v1/catalog: a bare array, [] until the notice is acknowledged,
 * filtered to the enabled sources. Rendered from the snapshot into one
 * buffer (about 2.6 MB today) and freed; the app asks only when its
 * catalog_key changes. Never sent: url, browserUrl, mirrors, hashes,
 * password, health, art or screenshot URLs (the snapshot does not hold
 * them: screenshots are counts, served by /art/{gameId}/shot/{n}). */
void tvcat_h_catalog(api_ctx *c) {
  jw w;
  if (!settings_sources_acknowledged()) { tv_arr_begin(&w); tv_arr_send(c, 200, &w); return; }
  tcat_idx *x = idx_get(IDX_FRESH);
  if (!x) { tv_error(c, 503, TV_MSG_LOW_MEMORY); return; }
  int on[TVCAT_MAX_SOURCES];
  for (int i = 0; i < x->nsrc; i++) on[i] = settings_source_enabled(P(x, x->src[i].id));
  tv_arr_begin(&w);
  for (uint32_t gi = 0; gi < x->ngames && !w.err; gi++) {
    const tc_game *g = &x->games[gi];
    for (uint32_t k = 0; k < g->n; k++) {
      const tc_rel *r = &x->rels[g->first + k];
      if (on[r->src]) write_option(&w, x, g, r);
    }
  }
  idx_unref(x);
  tv_arr_send(c, 200, &w);
}

/* ==== /api/v1/catalog/updates =============================================== */

/* catalog.c's own status: the current revision and whether it is the cached
 * (refreshed) copy rather than the embedded one. */
static void catalogue_status(int64_t *revision, int *cached) {
  *revision = catalog_revision();
  *cached = 0;
  jw w;
  jw_init(&w);
  jw_obj_begin(&w);
  catalog_status_json(&w);
  jw_obj_end(&w);
  size_t len = 0;
  char *txt = jw_detach(&w, &len);
  if (!txt) return;
  jdoc *d = json_parse(txt, len, 200, 4);
  free(txt);
  if (!d) return;
  *revision = jv_get_i64(json_root(d), "revision", *revision);
  *cached = strcmp(jv_get_str(json_root(d), "source", ""), "cached") == 0;
  json_free(d);
}

/* The status object of GET (and the 202 of POST). Field set of the reference
 * catalog_status() (Orbit Store backend/catalog.c, GPL-3.0-or-later). */
static void updates_reply(api_ctx *c, int status) {
  tcat_idx *x = idx_get(IDX_FRESH);
  int game_count = x ? x->game_count : 0;
  idx_unref(x);
  int64_t revision;
  int cached;
  catalogue_status(&revision, &cached);
  int other_busy = catalog_refreshing();
  time_t last_check = settings_catalog_last_check();
  int available = xhttp_available();
  pthread_mutex_lock(&tvcat_mu);
  int busy = g_refresh.busy || other_busy;
  time_t checked = last_check > g_refresh.finished_at ? last_check : g_refresh.finished_at;
  time_t check_after = g_refresh.check_after;
  char err[sizeof g_refresh.err];
  xstrlcpy(err, g_refresh.err, sizeof err);
  pthread_mutex_unlock(&tvcat_mu);
  jw w;
  tv_obj_begin(&w);
  jw_kv_bool(&w, "available", available);
  jw_kv_bool(&w, "busy", busy);
  jw_kv_bool(&w, "cached", cached);
  jw_kv_i64(&w, "revision", revision);      /* the real revision ("Catalogue 10"), not the composite */
  jw_kv_i64(&w, "gameCount", game_count);   /* TV-visible games over every exposed source */
  jw_kv_i64(&w, "checkedAt", checked > 0 ? (int64_t)checked : 0);
  jw_kv_i64(&w, "checkAfter", check_after > 0 ? (int64_t)check_after : 0);
  jw_kv_str(&w, "error", err);
  tv_obj_send(c, status, &w);
}

/* The tv-catalog thread: one catalog_refresh(), then the index follows so
 * GET /system's catalogueRevision changes at once when a newer catalogue
 * was installed. A refresh that is "not newer" counts as success. */
static void *refresh_thread(void *arg) {
  (void)arg;
  char err[256] = "";
  int updated = 0;
  int rc = catalog_refresh(0, &updated, err, sizeof err);
  char msg[sizeof g_refresh.err] = "";
  if (rc == CATALOG_REFRESH_BUSY) LOGI("tvapi: catalogue refresh joined one that was already running");
  else if (rc != 0) {
    xsnprintf(msg, sizeof msg, "Catalogue refresh failed (%s). Using your current catalogue.", err[0] ? err : "unknown error");
    LOGW("tvapi: %s", msg);
  } else LOGI("tvapi: catalogue refresh finished (%s)", updated ? "updated" : "already current");
  if (updated && !g_cfg.stop_requested) idx_unref(idx_get(IDX_FRESH));
  pthread_mutex_lock(&tvcat_mu);
  g_refresh.busy = 0;
  g_refresh.finished_at = util_now();
  xstrlcpy(g_refresh.err, msg, sizeof g_refresh.err);
  pthread_mutex_unlock(&tvcat_mu);
  return NULL;
}

/* POST /api/v1/catalog/updates {"action":"refresh"}: refusal order and
 * sentences ported from Orbit Store backend/api.c and backend/catalog.c
 * catalog_refresh() (GPL-3.0-or-later): 400, 409 (network), 409 (busy),
 * 429 (60 s cooldown), else 202 and the refresh runs on tv-catalog. */
static void updates_post(api_ctx *c) {
  const char *action = NULL;
  if (!tv_body_str(c, "action", &action) || strcmp(action, "refresh") != 0) { tv_error(c, 400, MSG_REFRESH_ACTION); return; }
  if (!xhttp_available()) { tv_error(c, 409, MSG_REFRESH_NETWORK); return; }
  int other_busy = catalog_refreshing();
  time_t now = util_now();
  pthread_mutex_lock(&tvcat_mu);
  if (g_refresh.busy || other_busy) { pthread_mutex_unlock(&tvcat_mu); tv_error(c, 409, MSG_REFRESH_BUSY); return; }
  if (now < g_refresh.check_after) { pthread_mutex_unlock(&tvcat_mu); tv_error(c, 429, MSG_REFRESH_WAIT); return; }
  g_refresh.busy = 1;
  g_refresh.check_after = now + TVCAT_REFRESH_COOLDOWN_S;
  g_refresh.err[0] = 0;
  pthread_mutex_unlock(&tvcat_mu);
  if (tv_thread_start(NULL, "tv-catalog", refresh_thread, NULL, 0, 1) != 0) {
    pthread_mutex_lock(&tvcat_mu);
    g_refresh.busy = 0;
    g_refresh.check_after = 0;
    pthread_mutex_unlock(&tvcat_mu);
    tv_error(c, 500, TV_MSG_FAILED);
    return;
  }
  LOGI("tvapi: catalogue refresh started from the TV app");
  updates_reply(c, 202);
}

void tvcat_h_updates(api_ctx *c) {
  if (tv_is_post(c)) updates_post(c);
  else updates_reply(c, 200);
}

/* ==== /api/v1/favorites ===================================================== */

/* The full list as a bare array (GET, and every POST reply). */
static void favorites_reply(api_ctx *c) {
  int cap = settings_favorites_count() + 64;
  if (cap > SET_MAX_FAVORITES) cap = SET_MAX_FAVORITES;
  char (*ids)[SET_ID_LEN] = malloc((size_t)cap * SET_ID_LEN);
  if (!ids) { tv_error(c, 503, TV_MSG_LOW_MEMORY); return; }
  int n = settings_favorites_list(ids, cap);
  jw w;
  tv_arr_begin(&w);
  for (int i = 0; i < n; i++) jw_str(&w, ids[i]);
  free(ids);
  tv_arr_send(c, 200, &w);
}

/* POST {"gameId","favorite":bool} sets the state (not a toggle). Checks and
 * sentences ported from Orbit Store backend/favorites.c favorite_set_locked
 * (GPL-3.0-or-later): an id our settings accept (1..95 bytes, no control
 * bytes) and a JSON bool, else 400; nothing to change is 200; adding a game
 * the catalogue does not have is 404; no room is 409; a failed write puts
 * the previous state back and answers 503. */
static void favorites_post(api_ctx *c) {
  const char *id = NULL;
  int on = 0;
  int ok = tv_body_str(c, "gameId", &id) && tv_body_bool(c, "favorite", &on);
  if (ok) {
    jv v = jv_get(c->body, "gameId");
    size_t n = jv_strlen(v);
    ok = n >= 1 && n < SET_ID_LEN && strlen(id) == n;
    for (size_t i = 0; ok && i < n; i++) if ((unsigned char)id[i] < 0x20) ok = 0;
  }
  if (!ok) { tv_error(c, 400, MSG_FAV_BAD); return; }
  on = on ? 1 : 0;
  if ((settings_is_favorite(id) ? 1 : 0) == on) { favorites_reply(c); return; }
  if (on) {
    cat_game *g = malloc(sizeof *g);
    if (!g) { tv_error(c, 503, TV_MSG_LOW_MEMORY); return; }
    int known = catalog_find_game(id, g) == 0;
    free(g);
    if (!known) { tv_error(c, 404, MSG_FAV_UNKNOWN); return; }
  }
  int rc = settings_favorite_set(id, on);
  if (rc == SETTINGS_ERR_FULL) { tv_error(c, 409, MSG_FAV_FULL); return; }
  if (rc == SETTINGS_ERR_INVALID) { tv_error(c, 400, MSG_FAV_BAD); return; }
  if (rc == SETTINGS_ERR_SAVE) {
    settings_favorite_set(id, !on);
    tv_error(c, 503, MSG_FAV_SAVE);
    return;
  }
  favorites_reply(c);
}

void tvcat_h_favorites(api_ctx *c) {
  if (tv_is_post(c)) favorites_post(c);
  else favorites_reply(c);
}
