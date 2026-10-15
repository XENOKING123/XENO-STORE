/*
 * api.h — every /api handler (SPEC §6) behind one dispatcher.
 *
 * Routes live in a table (method + pattern with {captures}) so the Phase 2
 * endpoints can be added without touching the dispatcher.
 */
#ifndef XENO_API_H
#define XENO_API_H

#include "server.h"
#include "json.h"
#include <stddef.h>

/* ---- Extension API (Phase 2 modules add routes without editing api.c) ----
 * A module defines handlers with this signature, builds a static api_route
 * table and registers it from its *_init() (called via api_ext_init()).
 * Captured path segments ({id}, {action}) land in ctx->params in order. */
typedef struct api_ctx {
  http_req *req;
  jdoc *doc;          /* parsed body (NULL when absent/invalid) */
  jv body;
  char params[3][96]; /* {captures} in route order */
  int nparams;
} api_ctx;

typedef void (*api_handler)(api_ctx *c);

typedef struct api_route {
  const char *method;
  const char *pattern;   /* "/api/queue/{id}/{action}" */
  api_handler fn;
} api_route;

/* Registers an extra route table (up to 8 tables). Returns 0, or -1 when full. */
int api_register_routes(const api_route *table, size_t n);

/* Envelope helpers shared with extension modules. */
void api_begin(jw *w);                     /* {"ok":true, ... */
void api_send(api_ctx *c, jw *w);          /* closes the object and sends 200 */
void api_error(api_ctx *c, int status, const char *code, const char *msg);
const char *api_body_str(api_ctx *c, const char *key, const char *dflt);

/* Optional hooks implemented by extension modules (weak: absent = no-op).
 * api_status_extra: writes extra keys into GET /api/status (e.g. "featured", "setup").
 * ext_*_init: called once after api_init(); ext_*_tick: called every ~500 ms from housekeeping. */
void api_status_extra(jw *w) __attribute__((weak));
void ext_downloads_init(void) __attribute__((weak));
void ext_console_init(void) __attribute__((weak));
void ext_downloads_tick(void) __attribute__((weak));
void ext_console_tick(void) __attribute__((weak));

/* ---- /api/v1: the TV app's compatibility dialect (src/tvapi*.c) ----------
 * tvapi_init: called once by api_ext_init() before the server starts; makes
 *   the per-process instance id, loads <data>/tvapi.json and registers the
 *   single /api/v1 route table. Starts no thread (the TV services start on
 *   the first /api/v1 request).
 * tvapi_client_seen: 1 when a loopback caller (the TV app on the console)
 *   asked GET /api/v1/system within the last 10 s, else 0. main.c reads it
 *   before opening the PS5 browser.
 * tvapi_diag_json: writes the TV layer's keys into GET /api/diagnostics
 *   ("tvapi" object: started, threads, drives...). */
void tvapi_init(void) __attribute__((weak));
int tvapi_client_seen(void) __attribute__((weak));
void tvapi_diag_json(jw *w) __attribute__((weak));

/* Sentences of the /api/v1 error envelope ({"error":"<sentence>"}) that the
 * dispatcher itself sends (PORT-SPEC 1.3). The TV app shows them as is and
 * translates exact matches, so never reword one without its app i18n key. */
#define API_V1_MSG_NOT_AVAILABLE "This request is not available in this version of XENO STORE."
#define API_V1_MSG_PAIR_AGAIN "Pair this device again from Settings."
#define API_V1_MSG_NOT_OBJECT "Expected a JSON object."
#define API_V1_MSG_FAILED "XENO STORE could not complete this request."
/* 1 when the request path is under /api/v1 (dispatcher errors use sentences). */
int api_is_v1_path(const char *path);

void api_ext_init(void);

void api_init(void);
/* Always answers the request (JSON). */
void api_handle(http_req *req);

#endif
