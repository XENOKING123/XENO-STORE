/*
 * tvapi_system.c — GET /api/v1/system, GET /api/v1/session,
 * /api/v1/zero/companion and the TV app's stop drain (PORT-SPEC 1.3, 1.5.1,
 * 1.5.5; map-system §2-§4).
 *
 * Copyright (C) 2026 XENO STORE contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The /system field set, the loopback-only session and its refusal sentence
 * follow the reference backend. Ported from Orbit Store backend/api.c
 * (GET /api/v1/system and /api/v1/session; GPL-3.0-or-later).
 *
 * The stop drain (PORT-SPEC 1.5.5 "Stop"): pause and save the queue
 * synchronously, say stopping:true, let the job workers wind down on a
 * detached tv-drain thread, save again and leave. The order follows the
 * reference updates.c stop (GPL-3.0-or-later); unlike the reference we never
 * relaunch ourselves: the TV app sends the next copy once the port closes.
 */
#include "tvapi.h"

#include "config.h"
#include "log.h"
#include "pairing.h"
#include "settings.h"
#include "sys.h"
#include "util.h"

#include <string.h>

/* The drain waits this long at most for the job workers (PORT-SPEC 1.5.5 step 4),
 * polling every DRAIN_POLL_MS. */
#define DRAIN_MAX_MS 10000
#define DRAIN_POLL_MS 100

void tvsys_start(void) {}

/* GET /api/v1/system: answered from memory in well under 1 ms (polled every
 * 0.5-3 s). No network, no update_mu, no drive access, no catalogue walk.
 * Always 200, also while stopping (then stopping:true). The app reads every
 * value with a strict type, so: bools with jw_kv_bool, numbers with jw_kv_i64. */
void tvsys_h_system(api_ctx *c) {
  char pref[64];
  tvdl_preferred_storage_id(pref, sizeof pref);
  jw w;
  tv_obj_begin(&w);
  jw_kv_str(&w, "name", XENO_APP_NAME);
  jw_kv_str(&w, "version", XENO_VERSION);              /* plain N.N.N, as version.txt and .version */
  jw_kv_str(&w, "instanceId", tv_instance_id());
  jw_kv_bool(&w, "stopping", tv_stopping());
  jw_kv_str(&w, "platform", sys_platform());           /* R12: "ps5" or "host" */
  jw_kv_bool(&w, "paired", tv_authed(c->req));
  jw_kv_bool(&w, "localSessionAvailable", c->req->client_is_loopback);
  jw_kv_bool(&w, "stateHealthy", dl_state_healthy());
  jw_kv_i64(&w, "catalogueRevision", tvcat_catalogue_revision());
  jw_kv_str(&w, "preferredStorage", pref);
  jw_kv_bool(&w, "deleteArchives", settings_delete_archives());
  jw_kv_i64(&w, "concurrentDownloads", settings_concurrent_jobs());
  /* not read by the app; kept for diagnostics (the reference sends httpPort too) */
  jw_kv_i64(&w, "httpPort", g_cfg.port);
  jw_kv_bool(&w, "safeMode", g_cfg.safe_mode);
  tv_obj_send(c, 200, &w);
}

/* GET /api/v1/session: the console's own TV app gets the same token the
 * console browser gets from /api/session (the console's own session, "local"
 * in <data>/pairing.json: never listed or unpaired under Pair a device).
 * A web page in the PS5 browser cannot read it (foreign Origin, no CORS for
 * it), and other LAN callers must pair with the code. */
void tvsys_h_session(api_ctx *c) {
  if (!c->req->client_is_loopback) { tv_error(c, 403, TV_MSG_SESSION_REMOTE); return; }
  char token[72];
  if (pairing_ensure_token(token, sizeof token) != 0) { tv_error(c, 503, TV_MSG_PAIRING_SAVE); return; }
  char ip[48];
  int have_ip = sys_local_ip(ip, sizeof ip) == 0;   /* never the 127.0.0.1 fallback */
  jw w;
  tv_obj_begin(&w);
  jw_kv_str(&w, "token", token);
  jw_kv_str(&w, "pairCode", pairing_code());
  jw_kv_str(&w, "address", have_ip ? ip : "");
  tv_obj_send(c, 200, &w);
}

/* GET and POST /api/v1/zero/companion: XENO STORE has no Orbit Zero. Only a
 * 404 makes the app switch Zero off and clear it (a 401 or 503 would keep its
 * previous state), so this route is registered and always answers 404. */
void tvsys_h_zero(api_ctx *c) { tv_error(c, 404, TV_MSG_ZERO); }

/* ---- the stop drain ---------------------------------------------------------- */

static pthread_mutex_t stop_mu = PTHREAD_MUTEX_INITIALIZER;   /* serialises tvsys_stop_begin */
static int g_stop_accepted;                                   /* under stop_mu; never cleared */

/* tv-drain: step 4. The jobs were paused and saved by tvsys_stop_begin; wait
 * until no worker thread owns a job any more (a paused download leaves its
 * IO loop within about 100 ms; an install cannot be paused, the cap bounds
 * it), save the queue again with the final byte counts and stop the process.
 * main() then closes the port and dl_shutdown() finds nothing running. */
static void *drain_thread(void *arg) {
  (void)arg;
  int64_t t0 = util_now_ms();
  int running = dl_workers_running();
  while (running > 0 && util_now_ms() - t0 < DRAIN_MAX_MS) {
    util_sleep_ms(DRAIN_POLL_MS);
    running = dl_workers_running();
  }
  if (running > 0) LOGW("tvapi: stop drain: %d job worker(s) still running after %d s, stopping anyway", running, DRAIN_MAX_MS / 1000);
  if (dl_save_queue() != 0) LOGW("tvapi: stop drain: the final queue save failed (the paused queue saved before stays)");
  LOGI("tvapi: stop drain done after %lld ms; XENO STORE stops (the TV app starts the next copy)",
       (long long)(util_now_ms() - t0));
  /* Never g_cfg.restart_requested: our own relaunch would race the app's start. */
  g_cfg.stop_requested = 1;
  return NULL;
}

/* POST /api/v1/updates {"action":"stop","confirmed":true}, after tvapi_update.c
 * validated it: PORT-SPEC 1.5.5 steps 1-4 (see tvapi.h). */
int tvsys_stop_begin(void) {
  pthread_mutex_lock(&stop_mu);
  if (g_stop_accepted || tv_drain_active() || g_cfg.stop_requested) {
    pthread_mutex_unlock(&stop_mu);
    return TVSYS_STOP_ALREADY;
  }
  /* An etaHEN DPI install reads its package from our /pkgsrv/ while it runs:
   * a stop would hold server_stop() and then lose the token, so wait for it. */
  if (server_pkgsrv_streams() > 0) {
    pthread_mutex_unlock(&stop_mu);
    LOGI("tvapi: stop refused: a package install is reading from /pkgsrv/");
    return TVSYS_STOP_INSTALLING;
  }
  /* 1-2: under the queue lock, pause queued (retry waits included) and active
   * jobs, hold the scheduler and save queue.json; a failed save puts every
   * job back as it was and holds nothing. dl.c refuses, changing nothing,
   * while a package install is starting or reads from /pkgsrv/. */
  int paused = dl_pause_all_for_stop(TVSYS_STOP_PAUSE_MESSAGE);
  if (paused == DL_PAUSE_INSTALLING) {
    pthread_mutex_unlock(&stop_mu);
    return TVSYS_STOP_INSTALLING;
  }
  if (paused < 0) {
    pthread_mutex_unlock(&stop_mu);
    LOGW("tvapi: stop refused: the queue could not be saved");
    return TVSYS_STOP_SAVE_FAILED;
  }
  /* 3: from now on GET /system says stopping:true and the gate answers 503. */
  g_stop_accepted = 1;
  tv_drain_set_active(1);
  pthread_mutex_unlock(&stop_mu);
  LOGI("tvapi: stop accepted from the TV app: %d job(s) paused, queue saved", paused);
  /* 4 */
  if (tv_thread_start(NULL, "tv-drain", drain_thread, NULL, 0, 1) != 0) {
    LOGW("tvapi: tv-drain could not start; stopping at once (the shutdown stops the workers)");
    g_cfg.stop_requested = 1;
  }
  return TVSYS_STOP_OK;
}
