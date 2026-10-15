/*
 * tvapi_update.c — GET|POST /api/v1/updates (the store's own update, and the
 * stop that hands over to tvsys_stop_begin) and GET|POST /api/v1/tv-app (the
 * TV app's own installer) with the tv-update worker (PORT-SPEC 1.5.5;
 * map-system §5-§6).
 *
 * Copyright (C) 2026 XENO STORE contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One tv-update worker runs the store's release check and install and the TV
 * app's release check; the TV app's install itself is an "app-install" job
 * (app_install_start_pinned), so it excludes the web UI's install. Nothing on
 * a request path touches the network: GETs answer from memory plus a couple
 * of lstat()/small reads (the saved copy's version, the install record);
 * POST /tv-app install also asks statvfs() once for the 507 check.
 *
 * Phases. /updates: idle, checking, checked, downloading, saving, saved,
 * error, stopping. /tv-app: idle, checking, checked, downloading, verifying,
 * installing, installed, error.
 *
 * Cooldowns. /updates: checkAfter = now + 60 after a check, retryAt = now + 60
 * after an install; /tv-app: 30 s for both (60 s after the release host
 * answered 403/429/503). Automatic checks (the app sends one per launch)
 * share a 6 h window, and do nothing when Settings -> "Check for updates
 * automatically" is off or the store runs in safe mode.
 *
 * The request validators, their order, the cooldowns, the sentences and the
 * version rules follow the reference backend. Ported from Orbit Store
 * backend/updates.c and backend/tvapp.c (GPL-3.0-or-later).
 *
 * Host build: both routes report available:false unless XENO_TVAPI_UPDATES=1.
 * With it, the store's feed is settings updates.feed (tests point it at a
 * loopback fixture server), the TV app's feed is tv-app.json next to it, the
 * GitHub fallback is $XENO_GITHUB_API_BASE (update_release_api_url) and
 * loopback http:// release URLs are accepted (app_install_release_url_ok).
 */
#include "tvapi.h"

#include "app_install.h"
#include "config.h"
#include "cx_common.h"
#include "http_client.h"
#include "log.h"
#include "settings.h"
#include "storage.h"
#include "update.h"
#include "util.h"
#include "xthread.h"

#include <stdlib.h>
#include <string.h>

#define TVUP_CHECK_COOLDOWN 60          /* /updates checkAfter */
#define TVUP_RETRY_COOLDOWN 60          /* /updates retryAt after an install */
#define TVUP_AUTO_INTERVAL (6 * 3600)   /* automatic checks, both routes */
#define TVUP_FRESH_S 900                /* an install needs a check this recent */
#define TVAPP_COOLDOWN 30               /* /tv-app checkAfter and retryAt */
#define TVAPP_THROTTLE 60               /* /tv-app retryAt after a 403/429/503 */
#define TVAPP_SPARE (64LL << 20)        /* left free on /data after the image */
#define TVAPP_FILE APP_TITLE_ID ".ffpkg"
#define TVAPP_FEED "https://raw.githubusercontent.com/XENOKING123/XENO-STORE/main/updates/tv-app.json"
#define TVUP_POLL_MS 100                /* the worker's idle poll */

/* /updates sentences */
#define S_AUTO_ONLY "Automatic mode is only valid for release checks."
#define S_UNKNOWN "Unknown update action."
#define S_UNAVAILABLE "Updates work with XENO STORE running on your PS5."
#define S_BUSY "A XENO STORE update is in progress. Wait for it to finish."
#define S_WAIT "Please wait before checking or downloading again."
#define S_CONFIRM "Confirm stopping XENO STORE and pausing downloads first."
#define S_CHECK_AGAIN "Check for updates again before installing this release."
#define S_NO_CHECKSUM "This release has no published checksum. Install it from the web version."
#define S_OLDER "An older release will not replace this build."
#define S_SAVE_QUEUE "Could not save the queue. XENO STORE has not been stopped."
#define S_NO_NEWER "No newer XENO STORE release was found. An older release will not replace this build."
#define S_INVALID_RELEASE "No compatible XENO STORE release found. An older or invalid release will not replace this build."
#define S_INSTALL_FAILED "The release failed SHA-256 or ELF validation. The saved copy was not replaced."
#define S_INTERRUPTED "Update interrupted. Try again."
/* /tv-app sentences (paths spelled out, PPSA99500 = APP_TITLE_ID, so app/tv's
 * i18n-coverage.py --backend finds every key literally) */
#define S_TV_UNKNOWN "Unknown TV app action."
#define S_TV_REPLACE "Replace is only valid, as true or false, when installing."
#define S_TV_AUTO "Automatic mode is only valid for TV app checks."
#define S_TV_UNAVAILABLE "The TV app is installed from XENO STORE running on your PS5."
#define S_TV_BUSY "The TV app is already being checked or installed. Wait for it."
#define S_TV_CHECK_AGAIN "Check for the TV app again before installing it."
#define S_TV_FOLDER "The TV app is already installed as the folder /data/homebrew/PPSA99500. Remove that folder to install the release, or keep updating it yourself."
#define S_TV_BLOCKED "Something other than the TV app is at /data/homebrew/PPSA99500.ffpkg. Move it away to install updates here."
#define S_TV_NEWER "A newer TV app is already installed. Nothing was replaced."
#define S_TV_MANUAL "A copy you placed yourself is already in /data/homebrew. Confirm replacing it with this release."
#define S_TV_INVALID "The TV app release information is invalid. Nothing was downloaded."
#define S_TV_UNPUBLISHED "The TV app hasn't been published yet. Check again after the next release."
#define S_TV_START "Could not start the TV app download."

/* Every field under SV.mu, a leaf lock: nothing else is called while it is
 * held (snapshots of update.c and app_install.c are taken before it). */
static struct {
  pthread_mutex_t mu;
  int started;
  int stop;                       /* a stop was accepted (or is being accepted) */
  /* /updates */
  int busy;
  char action[12];                /* queued for the worker: "check" | "install" | "" */
  char phase[16];
  char error[256];
  char want_version[32], want_sha[65];
  int64_t received;
  time_t retry_at, check_after, automatic_check_after;
  int saved_this_run;
  /* /tv-app */
  struct {
    int busy;                     /* a check is queued or running (an install is the job's business) */
    int check_queued;
    char phase[16];
    char error[256];
    char version[32], url[700], sha256[65];
    int64_t size;
    time_t checked, retry_at, check_after, automatic_check_after;
    int last_install;             /* the TV app's last action was an install */
    char job[16];                 /* ... with this job id */
  } tv;
} SV = { .mu = PTHREAD_MUTEX_INITIALIZER };

/* ---- versions --------------------------------------------------------------
 * major.minor.patch or major.minor.patch-beta.number, 1-6 digits each, an
 * optional leading "v". compare: 1, 0, -1, or -2 when either is no version.
 * Ported from Orbit Store backend/updates.c version_parts/compare_version
 * (GPL-3.0-or-later). */
static int version_parts(const char *v, unsigned parts[5]) {
  if (*v == 'v') v++;
  parts[3] = 1;
  parts[4] = 0;
  for (int i = 0; i < 5; i++) {
    if (i == 3) {
      if (!*v) return 1;
      if (strncmp(v, "-beta.", 6) != 0) return 0;
      v += 6;
      parts[3] = 0;
      continue;
    }
    unsigned digits = 0, value = 0;
    while (*v >= '0' && *v <= '9') {
      if (++digits > 6) return 0;
      value = value * 10 + (unsigned)(*v++ - '0');
    }
    if (!digits) return 0;
    parts[i] = value;
    if (i < 2 && *v++ != '.') return 0;
  }
  return *v == 0;
}

static int version_compare(const char *a, const char *b) {
  unsigned x[5], y[5];
  if (!version_parts(a, x) || !version_parts(b, y)) return -2;
  for (int i = 0; i < 5; i++)
    if (x[i] != y[i]) return x[i] > y[i] ? 1 : -1;
  return 0;
}

/* Plain N.N.N (1-6 digits each, no "v", no beta suffix): the only form the
 * app's starter, version.txt and .version agree on. */
static int plain_version(const char *v) {
  unsigned p[5];
  return v[0] >= '0' && v[0] <= '9' && strlen(v) < 32 && version_parts(v, p) && p[3] == 1;
}

static int hex64_lower(const char *s) {
  if (strlen(s) != 64) return 0;
  for (int i = 0; i < 64; i++)
    if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
  return 1;
}

static int tvup_available(void) {
  if (!xhttp_available()) return 0;
#ifdef XENO_HOST
  const char *e = getenv("XENO_TVAPI_UPDATES");
  return e && strcmp(e, "1") == 0;
#else
  return 1;
#endif
}

static int automatic_allowed(void) { return settings_updates_auto_check() && !g_cfg.safe_mode; }

/* ---- /updates status --------------------------------------------------------- */

typedef struct up_view {
  char latest[32], checksum[65], saved[32];
  time_t checked_at, automatic_after;
  int applying;
} up_view;

/* Everything /updates reads outside SV: update.c's cache and the saved copy. */
static void up_view_load(up_view *v, int checking) {
  memset(v, 0, sizeof *v);
  update_info u;
  update_snapshot(&u);
  const char *l = u.latest[0] == 'v' ? u.latest + 1 : u.latest;
  /* "" while checking and until a check completed (update_init seeds this
   * build's version, which is no checked result); never an invalid or older
   * release, so latestVersion >= runningVersion always holds */
  if (!checking && u.checked_at > 0 && plain_version(l) && version_compare(l, XENO_VERSION) >= 0) {
    xstrlcpy(v->latest, l, sizeof v->latest);
    v->checked_at = u.checked_at;
    if (hex64_lower(u.sha256)) xstrlcpy(v->checksum, u.sha256, sizeof v->checksum);
  }
  v->automatic_after = u.checked_at > 0 ? u.checked_at + TVUP_AUTO_INTERVAL : 0;
  v->applying = u.applying;
  char saved[32];
  if (update_saved_version(saved, sizeof saved) && plain_version(saved) && version_compare(saved, XENO_VERSION) >= 0)
    xstrlcpy(v->saved, saved, sizeof v->saved);
}

static void updates_status(jw *w) {
  pthread_mutex_lock(&SV.mu);
  int checking = strcmp(SV.phase, "checking") == 0;
  pthread_mutex_unlock(&SV.mu);
  up_view v;
  up_view_load(&v, checking);
  pthread_mutex_lock(&SV.mu);
  int available = tvup_available() && SV.started;
  int busy = SV.busy || v.applying;
  char phase[16], error[256];
  xstrlcpy(phase, SV.phase[0] ? SV.phase : "idle", sizeof phase);
  xstrlcpy(error, SV.error, sizeof error);
  int64_t received = SV.received;
  time_t retry_at = SV.retry_at, check_after = SV.check_after;
  time_t auto_after = XS_MAX(SV.automatic_check_after, v.automatic_after);
  int restart = SV.saved_this_run || (v.saved[0] && strcmp(v.saved, XENO_VERSION) != 0);
  pthread_mutex_unlock(&SV.mu);
  jw_kv_bool(w, "available", available);
  jw_kv_str(w, "runningVersion", XENO_VERSION);
  jw_kv_str(w, "instanceId", tv_instance_id());
  jw_kv_str(w, "savedVersion", v.saved);
  jw_kv_str(w, "latestVersion", v.latest);
  jw_kv_str(w, "checksum", v.checksum);
  jw_kv_bool(w, "updateAvailable", v.latest[0] && version_compare(v.latest, XENO_VERSION) > 0);
  jw_kv_i64(w, "automaticCheckAfter", (int64_t)auto_after);
  jw_kv_str(w, "phase", phase);
  jw_kv_str(w, "error", error);
  jw_kv_bool(w, "busy", busy);
  jw_kv_bool(w, "restartRequired", restart);
  jw_kv_i64(w, "received", received);
  jw_kv_i64(w, "retryAt", (int64_t)retry_at);
  jw_kv_i64(w, "checkedAt", (int64_t)v.checked_at);
  jw_kv_i64(w, "checkAfter", (int64_t)check_after);
}

static void reply_updates(api_ctx *c, int status) {
  jw w;
  tv_obj_begin(&w);
  updates_status(&w);
  tv_obj_send(c, status, &w);
}

/* POST /api/v1/updates: {"action":"check"[,"automatic":true]},
 * {"action":"install","version","checksum"}, {"action":"stop","confirmed":true}.
 * The rules and their order follow the reference (updates_action). */
static void updates_post(api_ctx *c) {
  const char *action = "";
  tv_body_str(c, "action", &action);
  int checking = strcmp(action, "check") == 0, installing = strcmp(action, "install") == 0, stopping = strcmp(action, "stop") == 0;
  int auto_present = tv_body_has(c, "automatic"), auto_val = 0, auto_bool = tv_body_bool(c, "automatic", &auto_val);
  int automatic = checking && auto_bool && auto_val;
  int confirmed = 0;
  tv_body_bool(c, "confirmed", &confirmed);
  const char *version = "", *checksum = "";
  tv_body_str(c, "version", &version);
  tv_body_str(c, "checksum", &checksum);

  if (auto_present && (!checking || !auto_bool)) { tv_error(c, 400, S_AUTO_ONLY); return; }
  if (!checking && !installing && !stopping) { tv_error(c, 400, S_UNKNOWN); return; }
  /* facts from other modules, gathered before SV.mu */
  up_view v;
  up_view_load(&v, 0);
  int auto_ok = automatic_allowed();
  time_t now = util_now();

  pthread_mutex_lock(&SV.mu);
  const char *refuse = NULL;
  int status = 202;
  time_t auto_after = XS_MAX(SV.automatic_check_after, v.automatic_after);
  if (!tvup_available() || !SV.started) { status = 409; refuse = S_UNAVAILABLE; }
  else if (automatic && !SV.stop &&
           (SV.busy || v.applying || auto_after > now || SV.retry_at > now || SV.check_after > now || !auto_ok)) {
    /* every launch of the app, and every other client, shares the console's cooldown */
    pthread_mutex_unlock(&SV.mu);
    reply_updates(c, 202);
    return;
  }
  else if (SV.busy || SV.stop || v.applying) { status = 409; refuse = S_BUSY; }
  else if ((!stopping && SV.retry_at > now) || (checking && SV.check_after > now)) { status = 429; refuse = S_WAIT; }
  else if (stopping && !confirmed) { status = 400; refuse = S_CONFIRM; }
  else if (installing && (!v.checked_at || now - v.checked_at > TVUP_FRESH_S || strcmp(version, v.latest) != 0 ||
                          strcmp(checksum, v.checksum) != 0)) { status = 409; refuse = S_CHECK_AGAIN; }
  else if (installing && !v.checksum[0]) { status = 409; refuse = S_NO_CHECKSUM; }
  else if (installing && version_compare(v.latest, XENO_VERSION) < 0) { status = 409; refuse = S_OLDER; }
  if (refuse) {
    pthread_mutex_unlock(&SV.mu);
    tv_error(c, status, refuse);
    return;
  }
  if (stopping) {
    /* Claim the stop so no check or install starts meanwhile; the pause and
     * the synchronous queue save run outside SV.mu (they take dl.c's locks). */
    SV.stop = 1;
    pthread_mutex_unlock(&SV.mu);
    int rc = tvsys_stop_begin();
    pthread_mutex_lock(&SV.mu);
    if (rc == TVSYS_STOP_OK) {
      xstrlcpy(SV.phase, "stopping", sizeof SV.phase);
      SV.error[0] = 0;
    } else {
      SV.stop = 0;
    }
    pthread_mutex_unlock(&SV.mu);
    if (rc == TVSYS_STOP_OK) { LOGI("tvapi: stop accepted; the drain runs, the app starts the next copy"); reply_updates(c, 202); }
    else if (rc == TVSYS_STOP_SAVE_FAILED) tv_error(c, 503, S_SAVE_QUEUE);
    else if (rc == TVSYS_STOP_ALREADY) tv_error(c, 503, TV_MSG_STOPPING);
    else tv_error(c, 503, TV_MSG_FAILED);
    return;
  }
  SV.busy = 1;
  SV.error[0] = 0;
  SV.received = 0;
  if (checking) {
    SV.check_after = now + TVUP_CHECK_COOLDOWN;
    SV.automatic_check_after = now + TVUP_AUTO_INTERVAL;
  } else {
    SV.retry_at = now + TVUP_RETRY_COOLDOWN;
    xstrlcpy(SV.want_version, v.latest, sizeof SV.want_version);
    xstrlcpy(SV.want_sha, v.checksum, sizeof SV.want_sha);
  }
  xstrlcpy(SV.action, action, sizeof SV.action);
  xstrlcpy(SV.phase, checking ? "checking" : "downloading", sizeof SV.phase);
  pthread_mutex_unlock(&SV.mu);
  LOGI("tvapi: update %s queued%s", action, automatic ? " (automatic)" : "");
  reply_updates(c, 202);
}

void tvup_h_updates(api_ctx *c) {
  if (tv_is_post(c)) updates_post(c);
  else reply_updates(c, 200);
}

/* ---- /tv-app status ---------------------------------------------------------- */

static void tvapp_status(jw *w) {
  int available = tvup_available();
  char current[40] = "";
  /* reference: the state is read only where the installer works */
  int state = available ? app_install_state(current, sizeof current) : APP_STATE_NONE;
  if (!plain_version(current)) current[0] = 0;   /* the web UI's record has none */
  app_install_info ai;
  app_install_snapshot(&ai);
  pthread_mutex_lock(&SV.mu);
  available = available && SV.started;
  char latest[32], checksum[65], phase[16], error[256];
  xstrlcpy(latest, SV.tv.version, sizeof latest);
  xstrlcpy(checksum, SV.tv.sha256, sizeof checksum);
  int64_t size = SV.tv.size, received = 0;
  int busy = SV.tv.busy || ai.running;
  if (SV.tv.busy) {   /* a check is queued or running */
    xstrlcpy(phase, SV.tv.phase, sizeof phase);
    error[0] = 0;
  } else if (ai.running) {   /* an install runs (this app's, or the web UI's) */
    xstrlcpy(phase, ai.phase[0] ? ai.phase : "downloading", sizeof phase);
    error[0] = 0;
    received = ai.received;
  } else if (SV.tv.last_install && SV.tv.job[0] && strcmp(ai.job, SV.tv.job) == 0 && ai.phase[0]) {
    xstrlcpy(phase, ai.phase, sizeof phase);
    xstrlcpy(error, strcmp(ai.phase, "error") == 0 ? ai.error : "", sizeof error);
    received = ai.received;
  } else {
    xstrlcpy(phase, SV.tv.phase[0] ? SV.tv.phase : "idle", sizeof phase);
    xstrlcpy(error, SV.tv.error, sizeof error);
  }
  time_t retry_at = SV.tv.retry_at, checked = SV.tv.checked, check_after = SV.tv.check_after, auto_after = SV.tv.automatic_check_after;
  pthread_mutex_unlock(&SV.mu);
  jw_kv_bool(w, "available", available);
  jw_kv_str(w, "installed", app_install_state_name(state));
  jw_kv_str(w, "installedVersion", current);
  jw_kv_str(w, "latestVersion", latest);
  jw_kv_str(w, "checksum", checksum);
  jw_kv_i64(w, "size", size);
  jw_kv_bool(w, "updateAvailable", state == APP_STATE_STORE && version_compare(latest, current) > 0);
  jw_kv_str(w, "phase", phase);
  jw_kv_str(w, "error", error);
  jw_kv_bool(w, "busy", busy);
  jw_kv_i64(w, "received", received);
  jw_kv_i64(w, "retryAt", (int64_t)retry_at);
  jw_kv_i64(w, "checkedAt", (int64_t)checked);
  jw_kv_i64(w, "checkAfter", (int64_t)check_after);
  jw_kv_i64(w, "automaticCheckAfter", (int64_t)auto_after);
}

static void reply_tvapp(api_ctx *c, int status) {
  jw w;
  tv_obj_begin(&w);
  tvapp_status(&w);
  tv_obj_send(c, status, &w);
}

/* Free bytes for the image on the console's internal storage (statvfs; the
 * homebrew dir may not exist yet, then its data dir). -1 when unknown. */
static int64_t tvapp_free_bytes(void) {
  int64_t fr = -1;
  if (storage_free_space(cx_data_homebrew_dir(), &fr, NULL) == 0) return fr;
  if (storage_free_space(cx_data_dir(), &fr, NULL) == 0) return fr;
  return -1;
}

/* POST /api/v1/tv-app: {"action":"check"[,"automatic":true]} or
 * {"action":"install","version","checksum"[,"replace":true]}. The rules and
 * their order follow the reference (tvapp_action). */
static void tvapp_post(api_ctx *c) {
  const char *action = "";
  tv_body_str(c, "action", &action);
  int checking = strcmp(action, "check") == 0, installing = strcmp(action, "install") == 0;
  int replace_present = tv_body_has(c, "replace"), replace = 0, replace_bool = tv_body_bool(c, "replace", &replace);
  int auto_present = tv_body_has(c, "automatic"), auto_val = 0, auto_bool = tv_body_bool(c, "automatic", &auto_val);
  int automatic = checking && auto_bool && auto_val;
  const char *version = "", *checksum = "";
  tv_body_str(c, "version", &version);
  tv_body_str(c, "checksum", &checksum);

  if (!checking && !installing) { tv_error(c, 400, S_TV_UNKNOWN); return; }
  if (replace_present && (!installing || !replace_bool)) { tv_error(c, 400, S_TV_REPLACE); return; }
  if (auto_present && (!checking || !auto_bool)) { tv_error(c, 400, S_TV_AUTO); return; }
  /* facts from other modules, gathered before SV.mu */
  char current[40] = "";
  int state = installing ? app_install_state(current, sizeof current) : APP_STATE_NONE;
  int64_t free_b = installing ? tvapp_free_bytes() : -1;
  app_install_info ai;
  app_install_snapshot(&ai);
  int auto_ok = automatic_allowed();
  time_t now = util_now();

  pthread_mutex_lock(&SV.mu);
  int busy = SV.tv.busy || ai.running;
  const char *refuse = NULL;
  char space_msg[160];
  int status = 202;
  if (!tvup_available() || !SV.started) { status = 409; refuse = S_TV_UNAVAILABLE; }
  else if (automatic && !SV.stop &&
           (busy || SV.tv.automatic_check_after > now || SV.tv.retry_at > now || SV.tv.check_after > now || !auto_ok)) {
    /* opening the TV app again, or another client, shares the console's cooldown */
    pthread_mutex_unlock(&SV.mu);
    reply_tvapp(c, 202);
    return;
  }
  else if (busy || SV.stop) { status = 409; refuse = S_TV_BUSY; }
  else if (SV.tv.retry_at > now || (checking && SV.tv.check_after > now)) { status = 429; refuse = S_WAIT; }
  else if (installing && (!SV.tv.checked || now - SV.tv.checked > TVUP_FRESH_S || strcmp(version, SV.tv.version) != 0 ||
                          strcmp(checksum, SV.tv.sha256) != 0)) { status = 409; refuse = S_TV_CHECK_AGAIN; }
  else if (installing) {
    if (state == APP_STATE_FOLDER) { status = 409; refuse = S_TV_FOLDER; }
    else if (state == APP_STATE_BLOCKED) { status = 409; refuse = S_TV_BLOCKED; }
    else if (state == APP_STATE_STORE && version_compare(SV.tv.version, current) == -1) { status = 409; refuse = S_TV_NEWER; }
    else if (state == APP_STATE_MANUAL && !(replace_bool && replace)) { status = 409; refuse = S_TV_MANUAL; }
    else if (free_b >= 0 && free_b < SV.tv.size + TVAPP_SPARE) {
      xsnprintf(space_msg, sizeof space_msg,
                "Not enough free space on the console's internal storage. The TV app needs about %lld MB.",
                (long long)((SV.tv.size + TVAPP_SPARE) / (1024 * 1024)));
      status = 507;
      refuse = space_msg;
    }
  }
  if (refuse) {
    pthread_mutex_unlock(&SV.mu);
    tv_error(c, status, refuse);
    return;
  }
  SV.tv.error[0] = 0;
  if (checking) {
    SV.tv.busy = 1;
    SV.tv.check_queued = 1;
    SV.tv.last_install = 0;
    SV.tv.checked = 0;
    SV.tv.version[0] = 0;
    SV.tv.sha256[0] = 0;
    SV.tv.url[0] = 0;
    SV.tv.size = 0;
    SV.tv.check_after = now + TVAPP_COOLDOWN;
    SV.tv.automatic_check_after = now + TVUP_AUTO_INTERVAL;
    xstrlcpy(SV.tv.phase, "checking", sizeof SV.tv.phase);
    pthread_mutex_unlock(&SV.mu);
    LOGI("tvapi: TV app check queued%s", automatic ? " (automatic)" : "");
    reply_tvapp(c, 202);
    return;
  }
  /* install: the job starts outside SV.mu; busy keeps a second POST out meanwhile */
  char url[700], sha[65], ver[32];
  int64_t size = SV.tv.size;
  xstrlcpy(url, SV.tv.url, sizeof url);
  xstrlcpy(sha, SV.tv.sha256, sizeof sha);
  xstrlcpy(ver, SV.tv.version, sizeof ver);
  SV.tv.retry_at = now + TVAPP_COOLDOWN;
  SV.tv.busy = 1;
  xstrlcpy(SV.tv.phase, "downloading", sizeof SV.tv.phase);
  pthread_mutex_unlock(&SV.mu);
  char id[16] = "", code[32] = "", err[256] = "";
  int rc = app_install_start_pinned(url, sha, size, ver, replace_bool && replace, id, sizeof id, code, sizeof code, err, sizeof err);
  pthread_mutex_lock(&SV.mu);
  SV.tv.busy = 0;
  if (rc == 0) {
    /* from now on the job's phase shows (app_install_snapshot); "checked" is
     * what remains once another job (the web UI's) replaces it */
    SV.tv.last_install = 1;
    xstrlcpy(SV.tv.job, id, sizeof SV.tv.job);
    xstrlcpy(SV.tv.phase, "checked", sizeof SV.tv.phase);
  } else {
    SV.tv.last_install = 0;
    xstrlcpy(SV.tv.phase, "error", sizeof SV.tv.phase);
    xstrlcpy(SV.tv.error, S_TV_START, sizeof SV.tv.error);
  }
  pthread_mutex_unlock(&SV.mu);
  if (rc != 0) {
    LOGW("tvapi: TV app install did not start: %s (%s)", err, code);
    if (strcmp(code, "in_progress") == 0) tv_error(c, 409, S_TV_BUSY);
    else if (strcmp(code, "downloads_unavailable") == 0) tv_error(c, 409, S_TV_UNAVAILABLE);
    else tv_error(c, 503, S_TV_START);
    return;
  }
  LOGI("tvapi: TV app %s install started (job %s%s)", ver, id, replace_bool && replace ? ", replacing the owner's copy" : "");
  reply_tvapp(c, 202);
}

void tvup_h_tvapp(api_ctx *c) {
  if (tv_is_post(c)) tvapp_post(c);
  else reply_tvapp(c, 200);
}

/* ---- the worker: the store's check and install -------------------------------- */

static void store_check(void) {
  char err[256] = "";
  int rc = update_check(err, sizeof err);
  update_info u;
  update_snapshot(&u);
  const char *l = u.latest[0] == 'v' ? u.latest + 1 : u.latest;
  char msg[300] = "";
  if (rc != 0) xsnprintf(msg, sizeof msg, "Could not check for updates (%s).", err[0] ? err : "no answer");
  else if (!plain_version(l)) xstrlcpy(msg, S_INVALID_RELEASE, sizeof msg);
  else if (version_compare(l, XENO_VERSION) < 0) xstrlcpy(msg, S_NO_NEWER, sizeof msg);
  pthread_mutex_lock(&SV.mu);
  xstrlcpy(SV.phase, msg[0] ? "error" : "checked", sizeof SV.phase);
  xstrlcpy(SV.error, msg, sizeof SV.error);
  if (u.throttled_until > SV.retry_at) SV.retry_at = u.throttled_until;
  SV.busy = 0;
  pthread_mutex_unlock(&SV.mu);
  if (msg[0]) LOGW("tvapi: update check: %s", msg);
  else LOGI("tvapi: update check: latest %s (running %s)", l, XENO_VERSION);
}

static int store_progress(void *user, int64_t done, int64_t total) {
  (void)user;
  (void)total;
  pthread_mutex_lock(&SV.mu);
  if (done < 0) xstrlcpy(SV.phase, "saving", sizeof SV.phase);
  else SV.received = done;
  int stop = SV.stop;
  pthread_mutex_unlock(&SV.mu);
  return stop || g_cfg.stop_requested;
}

static void store_install(const char *version, const char *sha) {
  char err[256] = "";
  int rc = update_apply_pinned(version, sha, store_progress, NULL, NULL, err, sizeof err);
  update_info u;
  update_snapshot(&u);
  pthread_mutex_lock(&SV.mu);
  if (rc == 0) {
    xstrlcpy(SV.phase, "saved", sizeof SV.phase);
    SV.error[0] = 0;
    SV.saved_this_run = 1;
  } else {
    xstrlcpy(SV.phase, "error", sizeof SV.phase);
    xstrlcpy(SV.error, rc == UPDATE_BUSY ? S_BUSY : SV.stop || g_cfg.stop_requested ? S_INTERRUPTED : S_INSTALL_FAILED, sizeof SV.error);
  }
  if (u.throttled_until > SV.retry_at) SV.retry_at = u.throttled_until;
  SV.busy = 0;
  pthread_mutex_unlock(&SV.mu);
  if (rc == 0) LOGI("tvapi: update %s saved; it runs after the next start", version);
  else LOGW("tvapi: update %s not installed: %s", version, err);
}

/* ---- the worker: the TV app's release check ------------------------------------ */

typedef struct tv_rel {
  char version[32], url[700], sha256[65];
  int64_t size;
} tv_rel;

static void tv_throttle(int status) {
  if (status != 403 && status != 429 && status != 503) return;
  pthread_mutex_lock(&SV.mu);
  time_t t = util_now() + TVAPP_THROTTLE;
  if (t > SV.tv.retry_at) SV.tv.retry_at = t;
  pthread_mutex_unlock(&SV.mu);
}

/* tv-app.json next to the store's payloads.json feed (so a test or a fork
 * that moves the feed moves both), else the project's own. */
static void tvapp_feed_url(char *out, size_t cap) {
  xstrlcpy(out, TVAPP_FEED, cap);
  settings_t *s = malloc(sizeof *s);
  if (!s) return;
  settings_get(s);
  static const char tail[] = "/payloads.json";
  size_t n = strlen(s->updatesFeed), tl = sizeof tail - 1;
  if (n > tl && strcmp(s->updatesFeed + n - tl, tail) == 0 && n - tl + sizeof "/tv-app.json" <= cap) {
    memcpy(out, s->updatesFeed, n - tl);
    xstrlcpy(out + (n - tl), "/tv-app.json", cap - (n - tl));
  }
  free(s);
}

/* A whole-number JSON size within the image limits, else -1. */
static int64_t json_size(jv v) {
  if (jv_type(v) != JV_NUM) return -1;
  double d = jv_dbl(v, -1);
  if (!(d >= (double)APP_MIN_BYTES && d <= (double)APP_MAX_BYTES)) return -1;
  int64_t i = (int64_t)d;
  return (double)i == d ? i : -1;
}

/* The release checks of the reference's TV app check: the file name, a plain
 * version, an allowed URL, a 64-hex SHA-256 and a size within the limits. */
static int tv_rel_valid(const tv_rel *r) {
  return plain_version(r->version) && app_install_release_url_ok(r->url) && hex64_lower(r->sha256) && r->size >= APP_MIN_BYTES &&
         r->size <= APP_MAX_BYTES;
}

/* Feed: {"version","filename","url","sha256" (or "checksum"),"size"}.
 * 0 parsed (validity checked by the caller), 1 the feed answered but is
 * unusable, -1 not reachable / not published (try GitHub). */
static int tv_from_feed(tv_rel *out, char *detail, size_t dcap) {
  char url[640];
  tvapp_feed_url(url, sizeof url);
  cx_http o = {0};
  o.timeout_ms = 20000;
  o.max_bytes = 64 << 10;
  o.headers = "Accept: application/json\r\nCache-Control: no-cache\r\n";
  int status = 0;
  size_t len = 0;
  char *body = cx_fetch(url, &o, &status, &len, NULL);
  tv_throttle(status);
  if (!body || status != 200) {
    if (body) xsnprintf(detail, dcap, "feed HTTP %d", status);
    else xsnprintf(detail, dcap, "%s", xhttp_last_error());
    free(body);
    return -1;
  }
  jdoc *d = json_parse(body, len, 200, 8);
  free(body);
  if (!d) return 1;
  jv r = json_root(d);
  const char *v = jv_get_str(r, "version", "");
  if (v[0] == 'v') v++;
  xstrlcpy(out->version, v, sizeof out->version);
  const char *u = jv_get_str(r, "url", "");
  if (strlen(u) < sizeof out->url) xstrlcpy(out->url, u, sizeof out->url);
  const char *h = jv_get_str(r, "sha256", jv_get_str(r, "checksum", ""));
  if (strlen(h) == 64) xstrlcpy(out->sha256, h, sizeof out->sha256);
  out->size = json_size(jv_get(r, "size"));
  int ok = jv_type(r) == JV_OBJ && strcmp(jv_get_str(r, "filename", ""), TVAPP_FILE) == 0;
  json_free(d);
  return ok ? 0 : 1;
}

/* GitHub releases/latest: tag_name, the PPSA99500.ffpkg asset (size, URL) and
 * its line in SHA256SUMS. 0 found, 1 the release has no TV app, -1 failure. */
static int tv_from_github(tv_rel *out, char *detail, size_t dcap) {
  cx_http o = {0};
  o.timeout_ms = 20000;
  o.max_bytes = 4u << 20;
  o.headers = "Accept: application/vnd.github+json\r\n";
  int status = 0;
  size_t len = 0;
  char *body = cx_fetch(update_release_api_url(), &o, &status, &len, NULL);
  tv_throttle(status);
  if (!body || status != 200) {
    if (body) xsnprintf(detail, dcap, "GitHub HTTP %d", status);
    else xsnprintf(detail, dcap, "%s", xhttp_last_error());
    free(body);
    return status == 404 ? 1 : -1;
  }
  jdoc *d = json_parse(body, len, 50000, 16);
  free(body);
  if (!d) { xsnprintf(detail, dcap, "GitHub answered something that is not JSON"); return -1; }
  jv r = json_root(d);
  const char *tag = jv_get_str(r, "tag_name", "");
  if (util_starts_with(tag, "xeno-store-v")) tag += 12;
  else if (tag[0] == 'v') tag++;
  xstrlcpy(out->version, tag, sizeof out->version);
  char sums_url[1024] = "";
  int found = 0;
  JV_FOREACH(a, jv_get(r, "assets")) {
    const char *name = jv_get_str(a, "name", "");
    const char *u = jv_get_str(a, "browser_download_url", "");
    if (strcmp(name, TVAPP_FILE) == 0 && strlen(u) < sizeof out->url) {
      xstrlcpy(out->url, u, sizeof out->url);
      out->size = json_size(jv_get(a, "size"));
      found = 1;
    }
    if (strcmp(name, "SHA256SUMS") == 0) xstrlcpy(sums_url, u, sizeof sums_url);
  }
  json_free(d);
  if (!found) return 1;
  if (!sums_url[0]) return 0;   /* no hash: invalid (the caller says so) */
  cx_http so = {0};
  so.timeout_ms = 15000;
  so.max_bytes = 64 << 10;
  char *sums = cx_fetch(sums_url, &so, &status, &len, NULL);
  tv_throttle(status);
  if (sums && status == 200 && update_sums_lookup(sums, TVAPP_FILE, out->sha256) != 0) out->sha256[0] = 0;
  if (!sums || status != 200) {
    if (sums) xsnprintf(detail, dcap, "SHA256SUMS HTTP %d", status);
    else xsnprintf(detail, dcap, "%s", xhttp_last_error());
    free(sums);
    return -1;
  }
  free(sums);
  return 0;
}

static void tvapp_check(void) {
  tv_rel rel;
  memset(&rel, 0, sizeof rel);
  char detail[200] = "", gh_detail[200] = "", msg[300] = "";
  const char *from = "feed";
  int rc = tv_from_feed(&rel, detail, sizeof detail);
  if (rc < 0) {
    /* the feed is not there (not published yet, or not reachable): GitHub */
    memset(&rel, 0, sizeof rel);
    from = "GitHub";
    rc = tv_from_github(&rel, gh_detail, sizeof gh_detail);
    if (rc == 1) xstrlcpy(msg, S_TV_UNPUBLISHED, sizeof msg);
    else if (rc < 0) xsnprintf(msg, sizeof msg, "Could not check for the TV app (%s). Try again later.", gh_detail[0] ? gh_detail : detail);
  }
  if (!msg[0] && (rc != 0 || !tv_rel_valid(&rel))) xstrlcpy(msg, S_TV_INVALID, sizeof msg);
  time_t now = util_now();
  pthread_mutex_lock(&SV.mu);
  if (!msg[0]) {
    xstrlcpy(SV.tv.version, rel.version, sizeof SV.tv.version);
    xstrlcpy(SV.tv.url, rel.url, sizeof SV.tv.url);
    xstrlcpy(SV.tv.sha256, rel.sha256, sizeof SV.tv.sha256);
    SV.tv.size = rel.size;
    SV.tv.checked = now;
  }
  xstrlcpy(SV.tv.phase, msg[0] ? "error" : "checked", sizeof SV.tv.phase);
  xstrlcpy(SV.tv.error, msg, sizeof SV.tv.error);
  SV.tv.busy = 0;
  pthread_mutex_unlock(&SV.mu);
  if (msg[0]) LOGW("tvapi: TV app check: %s (%s)", msg, detail[0] ? detail : gh_detail);
  else LOGI("tvapi: TV app check: release %s from the %s (%lld bytes)", rel.version, from, (long long)rel.size);
}

/* tv-update: one worker owns the store's check and install and the TV app's
 * check, one at a time. It polls its queue (no condition variable to wake),
 * starts nothing once the process is stopping and exits with it. */
static void *tvup_worker(void *arg) {
  (void)arg;
  while (!g_cfg.stop_requested) {
    char action[12] = "", version[32] = "", sha[65] = "";
    int tv_check = 0;
    pthread_mutex_lock(&SV.mu);
    if (SV.action[0]) {
      xstrlcpy(action, SV.action, sizeof action);
      xstrlcpy(version, SV.want_version, sizeof version);
      xstrlcpy(sha, SV.want_sha, sizeof sha);
      SV.action[0] = 0;
    } else if (SV.tv.check_queued) {
      SV.tv.check_queued = 0;
      tv_check = 1;
    }
    int stop = SV.stop;
    if (stop && action[0]) {   /* accepted just before the stop: never runs */
      SV.busy = 0;
      xstrlcpy(SV.phase, "stopping", sizeof SV.phase);
      action[0] = 0;
    }
    if (stop && tv_check) { SV.tv.busy = 0; xstrlcpy(SV.tv.phase, "idle", sizeof SV.tv.phase); tv_check = 0; }
    pthread_mutex_unlock(&SV.mu);
    if (strcmp(action, "check") == 0) { xthread_note("tv-update: release check"); store_check(); }
    else if (strcmp(action, "install") == 0) { xthread_note("tv-update: install %s", version); store_install(version, sha); }
    else if (tv_check) { xthread_note("tv-update: TV app check"); tvapp_check(); }
    else util_sleep_ms(TVUP_POLL_MS);
  }
  return NULL;
}

void tvup_start(void) {
  pthread_t th;
  int rc = tv_thread_start(&th, "tv-update", tvup_worker, NULL, 0, 1);
  pthread_mutex_lock(&SV.mu);
  SV.started = rc == 0;
  if (rc != 0) xstrlcpy(SV.error, "Could not start the update worker. Restart XENO STORE to retry.", sizeof SV.error);
  pthread_mutex_unlock(&SV.mu);
}
