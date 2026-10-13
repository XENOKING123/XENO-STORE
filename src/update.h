/*
 * update.h — self-update (SPEC §6.1): reads updates/payloads.json from the
 * configured feed (fallback: GitHub Releases "latest"), compares versions,
 * and stages a verified ELF in the data dir.
 */
#ifndef XENO_UPDATE_H
#define XENO_UPDATE_H

#include "json.h"
#include <stddef.h>

void update_init(const char *data_dir);
/* Fetches the feed (blocking, ≤ 30 s). 0 ok / -1 with err filled. The
 * result is cached for update_status_json(). */
int update_check(char *err, size_t errcap);
/* Writes current, latest, url, sha256, notes, available, checkedAt. */
void update_status_json(jw *w);
/* Downloads the latest ELF to <data>/xeno-store.elf.new, verifies sha256
 * (when the feed provides one), renames to <data>/xeno-store.elf. */
int update_apply(char *out_path, size_t cap, char *err, size_t errcap);

/* 1 when a downloaded update waits in <data>/xeno-store.elf; fills its path. */
int update_staged(char *path, size_t cap);

/* Called after the server and the downloads have shut down: sends the staged
 * ELF to the local ELF loader so the new version starts on the freed port.
 * 0 sent, -1 nothing staged or the loader did not take it (err filled). */
int update_relaunch(int loader_port, char *err, size_t errcap);

#endif
