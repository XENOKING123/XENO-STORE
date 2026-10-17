/*
 * cx_feeds.h — owner-curated JSON feeds published next to the catalog
 * (updates/featured.json, messages.json, companions.json, saves-backend.json)
 * plus a generic "cached fetch" used by the homebrew listing and the save
 * index. Feeds are refreshed with the catalog (catalog_on_refresh) and
 * cached under <data>/feeds/ so they survive offline starts. Copies cached
 * by another build (<data>/feeds/.build names the build they belong to) are
 * due whatever their age: on the first refresh after start while catalog
 * auto-refresh is on (retried every 10 minutes until it succeeds), and in
 * cx_feed_ensure in any case (saves-backend.json for the Save Store,
 * companions.json for the payload list, a payload install and the Garlic
 * start). The marker is written once this build has fetched every one of them.
 */
#ifndef XENO_CX_FEEDS_H
#define XENO_CX_FEEDS_H

#include "json.h"
#include <stddef.h>
#include <time.h>

enum { CX_FEED_FEATURED = 0, CX_FEED_MESSAGES, CX_FEED_COMPANIONS, CX_FEED_SAVES_BACKEND, CX_FEED_COUNT };

void cx_feeds_init(void);
/* Fetches every feed (blocking, ≤ ~15 s each); force ignores the max age. */
void cx_feeds_refresh(int force);
void cx_feeds_tick(void);
/* Writes the parsed feed value as JSON (null when absent). */
void cx_feed_json(int feed, jw *w);
/* Parsed copy of a feed (caller frees with json_free); NULL when absent. */
jdoc *cx_feed_doc(int feed);
time_t cx_feed_fetched_at(int feed);
/* Loads one feed when its copy is missing, older than max_age_s or cached by
 * another build (blocking: the cached file when still fresh, else the
 * network, else the old copy).
 * Used by the save index so a console that never refreshed its feeds (or has
 * catalog auto-refresh off) still follows the published saves-backend.json,
 * and by a payload install and the Garlic start for companions.json. */
void cx_feed_ensure(int feed, int max_age_s);
/* cx_feed_ensure(feed, 6 h) on a thread of its own (one per feed at a time),
 * waited for up to wait_ms: for a listing that must not hold its request for
 * a whole fetch (the payload list). */
void cx_feed_ensure_wait(int feed, int wait_ms);

/* Generic cache: GET url into <data>/feeds/<name>.json when the cached copy
 * is older than max_age_s (or force). Returns the text (malloc'd) or NULL;
 * *from_cache tells where it came from. */
char *cx_cached_fetch(const char *name, const char *url, int max_age_s, int force, size_t max_bytes,
                      size_t *len, int *from_cache, time_t *fetched_at);
/* Hostname allowlist for feed URLs (github raw/releases + loopback on host). */
int cx_feed_host_allowed(const char *url);

#endif
