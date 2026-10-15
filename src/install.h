/*
 * install.h — PKG installation (SPEC §8): AppInstUtil local-path recipe with
 * DPI fallbacks, PKG header parsing, install polling, the /pkgsrv/ staging
 * tokens and the ShadowMountPlus probe/scan ping. Host build simulates installs.
 */
#ifndef XENO_INSTALL_H
#define XENO_INSTALL_H

#include <stddef.h>
#include <time.h>

typedef struct install_result {
  int ok;                 /* 1 → installing, 0 → failed */
  int rc;                 /* last Sony / DPI return code */
  char contentId[40];
  char titleId[16];
  char token[24];         /* pkgsrv token (valid while the job exists) */
  char message[256];
  char via[24];           /* appinstutil | dpi9040 | dpi12800 | host-sim */
} install_result;

/* Reads the PKG header (magic 7F 43 4E 54 / 7F 46 49 48). Returns 0 and the
 * content id (may be "" for a fake-signed PS5 pkg), -1 when not a PKG. */
int install_read_pkg_header(const char *path, char *content_id, size_t cap, char *title_id, size_t tcap);
void install_start(const char *path, int port, install_result *out);
/* 1 installed, 0 still pending, -1 failed (sets msg). */
int install_poll(const char *titleId, const char *contentId, time_t startedAt, char *msg, size_t cap);
void install_release_token(const char *token);
int install_pkgsrv_lookup(const char *token, char *path, size_t cap);
/* ShadowMountPlus rescans its scan roots (/data/homebrew among them) on its
 * own every scan_interval_seconds (60 s before 1.7, 15 s since; recent
 * versions also soon after a file lands in one). Only 1.7 and newer serve
 * an HTTP API (127.0.0.1:10101), so its process tells whether it runs.
 * 1 running, 0 not running, -1 cannot tell (process list unreadable). */
int install_shadowmount_running(void);
/* Best-effort POST http://127.0.0.1:10101/api/v1/scan; nothing depends on it.
 * Returns 1 when ShadowMountPlus runs (the API answered or its process was
 * found), 0 when it does not or that cannot be told; status says which. */
int install_shadowmount_scan(char *status, size_t cap);

#endif
