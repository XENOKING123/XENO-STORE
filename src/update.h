/*
 * update.h — self-update (SPEC §6.1): reads updates/payloads.json from the
 * configured feed (fallback: GitHub Releases "latest", also consulted when
 * the feed is older than this build), compares versions, and stages a
 * verified ELF in the data dir.
 *
 * Locks (PORT-SPEC 1.8 E5): update_mu guards the cached release and is never
 * held over I/O, so update_status_json() and update_snapshot() answer from
 * memory while a check is on the network. check_mu serialises checks (a
 * second caller waits for the running check and shares its result); apply_mu
 * lets one apply run at a time (a second one is refused with UPDATE_BUSY).
 */
#ifndef XENO_UPDATE_H
#define XENO_UPDATE_H

#include "json.h"
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* update_apply() / update_apply_pinned(): another apply is running (apply_mu). */
#define UPDATE_BUSY (-2)
/* update_apply_pinned() only; nothing was replaced unless it says so: */
#define UPDATE_NET (-3)       /* the download failed or was cancelled (transport, HTTP, write) */
#define UPDATE_STALE (-4)     /* the checked release changed or its URL is not https: check again */
#define UPDATE_INVALID (-5)   /* the file is not the release: SHA-256, size or ELF test failed */
#define UPDATE_SAVE (-6)      /* the saved copy could not be replaced (old copy and .version kept) */

void update_init(const char *data_dir);
/* Fetches the feed, and GitHub's latest release when the feed fails or is
 * older than this build (the higher of the two wins). Blocking: up to about
 * 55 s (20 s feed, 20 s API, 15 s SHA256SUMS). 0 ok / -1 with err filled.
 * The result is cached for update_status_json() and update_snapshot(). */
int update_check(char *err, size_t errcap);
/* Writes current, latest, url, sha256, notes, available, checkedAt, staged.
 * Memory and two tiny file reads only; never waits for a running check. */
void update_status_json(jw *w);

/* A copy of the cached release (memory only, never waits for the network). */
typedef struct update_info {
  char latest[32];       /* no leading "v"; seeded with XENO_VERSION until a check succeeds */
  char url[1024];
  char sha256[65];       /* lowercase hex, "" when the feed published none */
  time_t checked_at;     /* wall clock of the last successful check, 0 = never */
  int available;         /* checked and newer than this build */
  int checking;          /* a check is on the network now */
  int applying;          /* an apply (web UI or TV app) is running now */
  time_t throttled_until; /* the release host answered 403/429/503: wait until then (0 = no) */
} update_info;
void update_snapshot(update_info *out);

/* The version recorded next to <data>/xeno-store.elf, when that file passes
 * the ELF64 / little-endian / x86-64 test the TV app's starter applies (so
 * this is the copy the starter would send). Any version, older ones included;
 * "" and 0 when there is no such copy, 1 otherwise. Two small file reads. */
int update_saved_version(char *ver, size_t cap);

/* Downloads the latest ELF to <data>/xeno-store.elf.new, verifies sha256
 * (when the feed provides one), renames to <data>/xeno-store.elf. Returns 0,
 * -1 with err, or UPDATE_BUSY while another apply runs. */
int update_apply(char *out_path, size_t cap, char *err, size_t errcap);

/* Progress of update_apply_pinned(): bytes so far while downloading (total -1
 * when unknown); called once more with done = total = -1 when the download
 * passed every check and the saved copy is about to be replaced ("saving").
 * A non-zero return cancels the download. */
typedef int (*update_progress_fn)(void *user, int64_t done, int64_t total);

/* The TV app's install (PORT-SPEC 1.5.5): installs exactly the release the
 * last check found, pinned by its version and SHA-256 (64 hex). Refuses when
 * the cached release no longer matches; downloads it (64 MiB cap, streaming
 * SHA-256), requires an ELF64 x86-64 image, then replaces <data>/xeno-store.elf
 * and writes its .version. *cancel (may be NULL) aborts the download. Returns
 * 0, UPDATE_BUSY while another apply runs, or UPDATE_NET, UPDATE_STALE,
 * UPDATE_INVALID or UPDATE_SAVE with err (see above). */
int update_apply_pinned(const char *version, const char *sha256, update_progress_fn progress, void *user,
                        volatile int *cancel, char *err, size_t errcap);

/* GitHub's releases/latest API URL of this project (host build: below
 * $XENO_GITHUB_API_BASE when set, so tests never reach the network). */
const char *update_release_api_url(void);
/* Finds `name` in a SHA256SUMS text ("<64 hex>  <name>" per line, sha256sum
 * format, "*" binary marker allowed). 0 and the lowercase hash, or -1. */
int update_sums_lookup(const char *sums, const char *name, char out[65]);

/* 1 when a downloaded update newer than this build waits in
 * <data>/xeno-store.elf (its version recorded in xeno-store.elf.version);
 * fills its path. */
int update_staged(char *path, size_t cap);

/* Called after the server and the downloads have shut down: writes
 * <data>/handoff.pending ("<time> <this version> <why>"; why: "restart" for
 * Settings → Updates → Restart now, "start" for update_handoff_newer) and
 * sends the staged ELF to the local ELF loader so the new version starts on
 * the freed port. 0 sent, -1 nothing staged or the loader did not take it
 * (err filled, marker removed). */
int update_relaunch(int loader_port, const char *why, char *err, size_t errcap);

/* At start, before the server binds: when a downloaded update newer than this
 * build waits in the data dir, hand it to the ELF loader and return 1 so this
 * older copy exits. 0 = keep running: nothing newer (a stale ELF is renamed
 * to *.old), the loader refused, this copy was just handed the ELF, another
 * copy of this build handed it off moments ago, or the update stopped right
 * after its last start / never came up after the last hand-off — then it is
 * renamed to *.failed and the built-in version keeps the console usable. */
int update_handoff_newer(int loader_port);

/* 1 when another build handed over to this copy moments ago for any reason
 * but its own start: Settings → Updates → Restart now, or an older build,
 * whose marker does not say why. Read before update_handoff_done(). */
int update_restarted(void);

/* Clears the hand-off marker once this copy's server is up. */
void update_handoff_done(void);

#endif
