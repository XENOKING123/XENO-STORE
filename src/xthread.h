/*
 * xthread.h — every thread the store starts goes through xthread_start():
 * an explicit stack size (the console's default thread stack is far smaller
 * than a desktop's, and Sony's TLS stack plus our request handlers do not fit
 * in it), a name, an alternate signal stack and a "what am I doing" note.
 *
 * crash_init() installs fatal-signal handlers that write crash.txt (signal,
 * thread, last note, fault address, code offset, uptime) and append the same
 * line to log.txt, then exit. On the next start crash_init() reads the report;
 * the copy that keeps running (after the update hand-off) moves it to
 * crash.prev.txt with crash_consume(), logs it and serves it at
 * GET /api/diagnostics.
 */
#ifndef XENO_XTHREAD_H
#define XENO_XTHREAD_H

#include <pthread.h>
#include <stddef.h>

#include "util.h"

#define XT_STACK_DEFAULT (1u << 20)   /* 1 MiB: request handlers, housekeeping, HTTP */
#define XT_STACK_SMALL (512u << 10)   /* download segments, probes */
#define CRASH_EARLY_S 300             /* a run that stopped this soon: safe start, update crash guard */

/* Starts fn(arg) on a new thread named `name` with `stack` bytes of stack
 * (0 = XT_STACK_DEFAULT). detached != 0 starts it detached (out may be NULL).
 * Returns pthread_create's result. */
int xthread_start(pthread_t *out, const char *name, void *(*fn)(void *), void *arg, size_t stack, int detached);

/* Records what the calling thread is doing (kept until the next note). */
void xthread_note(const char *fmt, ...) XS_PRINTF(1, 2);
/* Records the network step in progress ("sceHttp SendRequest", host); NULL
 * stage clears it. Cheap enough for every read. */
void xthread_net(const char *stage, const char *host);

/* Installs the crash handlers, registers the calling (main) thread and reads
 * crash.txt (left in place). Returns 1 when the previous run left a crash
 * report, else 0. */
int crash_init(const char *data_dir);
/* Moves crash.txt to crash.prev.txt; called once this copy keeps running, so
 * a copy it hands over to still sees the report as fresh. */
void crash_consume(void);
/* Last crash report ("" when none); uptime_s gets that run's uptime or -1 and
 * fresh is set when it was the run right before this one. */
const char *crash_previous(long *uptime_s, int *fresh);
/* Default thread stack the platform would have used, in bytes (0 = unknown). */
size_t xthread_platform_default_stack(void);

#endif
