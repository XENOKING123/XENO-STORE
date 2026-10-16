/*
 * pairing.h — device pairing (SPEC §6.6).
 *
 * At start a random 6-digit code is picked and, once the server is up,
 * shown on the console as a toast; a phone proves it is in the room by
 * entering that code and receives a 64-hex bearer token (up to 16 paired
 * devices, persisted to <data>/pairing.json with a name taken from the
 * device's User-Agent and when it was first and last seen; pairing one more
 * replaces the device unused the longest). A valid token lets a mutating api
 * request through without the optional PIN. The console's own session (the
 * token GET /api/session and /api/v1/session hand out, shared by the console
 * browser and the TV app) is kept apart: never listed, unpaired or replaced.
 * All state lives here under its own mutex — settings.c is never touched.
 */
#ifndef XENO_PAIRING_H
#define XENO_PAIRING_H

#include "json.h"
#include <stddef.h>

/* Loads persisted tokens and picks the session code. */
void pairing_init(const char *data_dir);

/* Shows the session code as a console toast (main, once the server is up). */
void pairing_announce(void);

/* The current 6-digit code (always 6 chars + NUL). */
const char *pairing_code(void);

/* POST /api/pair/show: re-shows the code as a console toast.
 * 0 shown, 1 within the 30 s cooldown (sets *retry_after to seconds left),
 * -1 the notification could not be shown. */
int pairing_show(int *retry_after);

/* Seconds until /api/pair/show may be called again (0 = ready now). */
int pairing_show_cooldown(void);

/* POST /api/pair: redeem a code for a bearer token (constant-time compare)
 * for a device called `name` (pairing_device_name). With all 16 slots taken
 * the device unused the longest (oldest last seen) is unpaired to make room.
 * 0 and a 64-hex token in token_out (cap >= 65) on success, 1 likewise when
 * a device made room (its name, "" when it has none, in `replaced`, which
 * may be NULL); otherwise a negated HTTP status: 429 locked out (sets
 * *retry_after), 403 wrong code, 503 could not persist. */
int pairing_redeem(const char *code, const char *name, char *token_out, size_t cap, char *replaced, size_t rcap, int *retry_after);

/* 1 when `token` (64 hex) matches a paired token or the console's own
 * session (constant-time). */
int pairing_token_valid(const char *token);

/* 1 when `token` (64 hex) is the console's own session (constant-time). */
int pairing_token_local(const char *token);

/* Every api request from the LAN with a bearer token (the console itself is
 * no paired device): a paired device's last seen moves to now (saved at most
 * once a minute), and a device without a name yet (paired before names were
 * kept) takes one from user_agent. */
void pairing_touch(const char *token, const char *user_agent);

/* A short name for the device behind a User-Agent: "iPhone Safari",
 * "Windows Chrome", "XENO STORE Desktop", "PS5 browser"...; "" when nothing
 * in it is known. */
void pairing_device_name(const char *user_agent, char *out, size_t cap);

/* GET /api/pair/devices: writes "devices": [{id, name, firstSeen, lastSeen,
 * current}] in pairing order, then "max". id is the first 16 hex digits of
 * the token's SHA-256 (the token itself never leaves the console); current
 * marks `self`, the caller's bearer token (may be NULL). Times are Unix
 * seconds, 0 when unknown. The console's own session is not listed. */
void pairing_devices_json(jw *w, const char *self);

/* POST /api/pair/unpair: 0 unpaired, -404 no paired device has that id,
 * -503 could not persist (nothing changed). */
int pairing_unpair(const char *id);

/* POST /api/pair/unpair-others: unpairs every device but `self` (may be
 * NULL). The number unpaired, or -503 when it could not persist. */
int pairing_unpair_others(const char *self);

/* /api/session and /api/v1/session (loopback only): the console's own
 * session token, created the first time. 0 and token_out filled, or -1. */
int pairing_ensure_token(char *token_out, size_t cap);

#endif
