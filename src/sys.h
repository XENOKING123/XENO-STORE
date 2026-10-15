/*
 * sys.h — PS5 glue (toasts, keep-awake, firmware version, browser launch,
 * local IP). The host build logs toasts instead of showing them.
 */
#ifndef XENO_SYS_H
#define XENO_SYS_H

#include "util.h"

void sys_toast(const char *fmt, ...) XS_PRINTF(1, 2);
void sys_keep_awake_tick(void);
const char *sys_fw_version(void);      /* "13.60" on PS5, "host" otherwise */
const char *sys_platform(void);        /* "ps5" | "host" */
int sys_launch_browser(const char *url);  /* PS5 only; a logged no-op on host */
/* Milliseconds since the store last opened the browser (a large value when never). */
int64_t sys_browser_opened_ago_ms(void);
/* The first non-loopback, non-link-local IPv4 address that is up: 0 and out
 * filled, or -1 when there is none (out then holds "127.0.0.1", which callers
 * that publish an address, such as /api/v1/session, must not send).
 * Thread-safe (inet_ntop into a local buffer). */
int sys_local_ip(char *out, size_t cap);

#endif
