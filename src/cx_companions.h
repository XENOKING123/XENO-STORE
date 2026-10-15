/*
 * cx_companions.h — companion payloads (SPEC §17.16/§17.18): owner-curated
 * list (updates/companions.json), sha256-verified download to
 * <data>/payloads/<id>.elf and launch by sending the raw bytes to the local
 * ELF loader on 127.0.0.1:9021. Every entry points at its author's own
 * release asset; an entry with "unavailable": true (no upstream asset to
 * pin) is listed with its reason and refuses to install, and so is an entry
 * whose sha256 is a placeholder instead of a digest ("FILLED-BY-OWNER").
 */
#ifndef XENO_CX_COMPANIONS_H
#define XENO_CX_COMPANIONS_H

#include "json.h"
#include <stddef.h>
#include <stdint.h>

void cx_companions_init(void);
/* Writes "payloads": [...], "loaderPort", "loaderAvailable". */
void cx_companions_json(jw *w);
/* Starts the download/verify job; writes the job id. */
int cx_companions_install(const char *id, char *job_id, size_t idcap, char *code, size_t ccap, char *err, size_t errcap);
/* Sends the installed ELF to the loader; writes "bytes", "port". */
int cx_companions_launch(const char *id, jw *w, char *code, size_t ccap, char *err, size_t errcap);
/* Finds an installed payload whose id/name/kind mentions `needle`. */
int cx_companions_find_installed(const char *needle, char *id_out, size_t cap);

/* 1 when the ELF loader accepts a connection on its port. */
int cx_companions_loader_available(void);
/* Sends an ELF file (checked: ELF magic, size limit) to the loader.
 * Returns 0 and the byte count, or -1 with code+err. */
int cx_companions_send_elf(const char *path, int64_t *bytes, char *code, size_t ccap, char *err, size_t errcap);

typedef struct cx_companion_file {
  char id[48];
  char version[32];
  char path[700];
  char sha256[65];
  int downloaded;          /* fetched now (else an existing verified copy) */
} cx_companion_file;

/* Makes sure the payload of feed `kind` (fallback: the given id/url/sha256/
 * version when the feed has no such entry, or one whose sha256 is a
 * placeholder or missing while the fallback is a full pin) is on the console as
 * <data>/payloads/<id>.elf: an existing copy whose sha256 matches the pinned
 * one is reused, otherwise it is downloaded (GitHub hosts only, sha256 and
 * ELF magic checked) and recorded as installed. Synchronous. 0 or -1 + err. */
int cx_companions_fetch(const char *kind, const char *fb_id, const char *fb_url, const char *fb_sha256, const char *fb_version,
                        cx_companion_file *out, char *err, size_t errcap);
/* Feed entry of `kind` (id/version/url) for display; -1 when absent (an entry
 * whose sha256 is a placeholder counts as absent: the caller's pin stands). */
int cx_companions_by_kind(const char *kind, char *id, size_t idcap, char *version, size_t vcap, int *unavailable);

#endif
