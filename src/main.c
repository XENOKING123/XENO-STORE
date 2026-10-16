/*
 * main.c — argv parsing (SPEC §3.1), startup order (§3.2), the housekeeping
 * thread (persist, catalog refresh, keep-awake, retry timers, update checks)
 * and graceful shutdown.
 *
 *   xeno-store [--port N] [--data DIR] [--root PATH]... [--no-browser] [--log-level debug|info|warn]
 *
 * On the console the loader passes no arguments and the defaults apply
 * (data dir /data/xeno-store, port 9095). Once the server is up the XENO
 * STORE app (or, without it, the PS5 browser) opens on the store unless
 * Settings → "Open XENO STORE when it starts" is off (or --no-browser).
 */
#include "api.h"
#include "app_install.h"
#include "catalog.h"
#include "config.h"
#include "dl.h"
#include "http_client.h"
#include "launcher_icon.h"   /* [launcher builder] deep-link home-screen icon */
#include "log.h"
#include "pairing.h"
#include "server.h"
#include "settings.h"
#include "storage.h"
#include "sys.h"
#include "update.h"
#include "cx_common.h"
#include "cx_library.h"
#include "util.h"
#include "xthread.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

xeno_config g_cfg;

#define CATALOG_REFRESH_S (6 * 3600)
#define UPDATE_CHECK_S (24 * 3600)
#define FIRST_SYNC_DELAY_S 20

static void usage(void) {
  fprintf(stderr, "usage: xeno-store [--port N] [--data DIR] [--root PATH]... [--no-browser] [--log-level debug|info|warn]\n");
}

static int parse_args(int argc, char **argv) {
  memset(&g_cfg, 0, sizeof g_cfg);
  g_cfg.port = XENO_DEFAULT_PORT;
  g_cfg.log_level = LOG_INFO;
#ifdef XENO_HOST
  xstrlcpy(g_cfg.data_dir, "./xeno-data", sizeof g_cfg.data_dir);
#else
  xstrlcpy(g_cfg.data_dir, "/data/xeno-store", sizeof g_cfg.data_dir);
#endif
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : NULL;
    if (strcmp(a, "--port") == 0 && v) { g_cfg.port = util_parse_int(v, -1); i++; if (g_cfg.port <= 0 || g_cfg.port > 65535) { usage(); return -1; } }
    else if (strcmp(a, "--data") == 0 && v) { xstrlcpy(g_cfg.data_dir, v, sizeof g_cfg.data_dir); i++; }
    else if (strcmp(a, "--root") == 0 && v) {
      if (g_cfg.n_extra_roots < XENO_MAX_EXTRA_ROOTS) xstrlcpy(g_cfg.extra_roots[g_cfg.n_extra_roots++], v, sizeof g_cfg.extra_roots[0]);
      i++;
    }
    else if (strcmp(a, "--no-browser") == 0) g_cfg.no_browser = 1;
    else if (strcmp(a, "--log-level") == 0 && v) { g_cfg.log_level = log_level_from_name(v); i++; }
    else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { usage(); return -1; }
    else { fprintf(stderr, "unknown argument: %s\n", a); usage(); return -1; }
  }
  /* strip a trailing slash so joined paths stay tidy */
  size_t n = strlen(g_cfg.data_dir);
  while (n > 1 && g_cfg.data_dir[n - 1] == '/') g_cfg.data_dir[--n] = 0;
  return 0;
}

static void on_signal(int sig) {
  (void)sig;
  g_cfg.stop_requested = 1;
}

/* Periodic duties that must not block the HTTP workers. */
static void *housekeeping(void *arg) {
  (void)arg;
  time_t last_catalog = 0, last_update = 0;
  time_t boot = util_now();
  while (!g_cfg.stop_requested) {
    dl_housekeeping_tick();
    if (ext_downloads_tick) ext_downloads_tick();
    if (ext_console_tick) ext_console_tick();
    time_t now = util_now();
    settings_t s;
    settings_get(&s);
    xthread_note("housekeeping");
    if (xhttp_available() && !g_cfg.safe_mode && now - boot >= FIRST_SYNC_DELAY_S) {
      if (s.catalogAutoRefresh && now - last_catalog >= CATALOG_REFRESH_S && (now - s.catalogLastCheck >= CATALOG_REFRESH_S || last_catalog == 0)) {
        last_catalog = now;
        if (now - s.catalogLastCheck >= CATALOG_REFRESH_S) {
          xthread_note("catalog auto-refresh");
          char err[256];
          int updated = 0;
          if (catalog_refresh(0, &updated, err, sizeof err) != 0) LOGW("catalog auto-refresh: %s", err);
          else if (updated) sys_toast("Catalog updated to revision %lld", (long long)catalog_revision());
        }
      }
      if (g_cfg.stop_requested) break;   /* no update check on the way out */
      if (s.updatesAutoCheck && now - last_update >= UPDATE_CHECK_S) {
        last_update = now;
        if (now - s.updatesLastCheck >= UPDATE_CHECK_S) {
          xthread_note("update check");
          char err[256];
          update_check(err, sizeof err);
        }
      }
    }
    util_sleep_ms(500);
  }
  return NULL;
}

int main(int argc, char **argv) {
  if (parse_args(argc, argv) != 0) return 2;
  signal(SIGPIPE, SIG_IGN);
#ifdef XENO_HOST
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
#else
  signal(SIGTERM, on_signal);
#endif
  g_cfg.started_at = util_now();
  if (util_mkdir_p(g_cfg.data_dir) != 0) {
    fprintf(stderr, "cannot create data dir %s: %s\n", g_cfg.data_dir, strerror(errno));
    sys_toast("XENO STORE: cannot create %s", g_cfg.data_dir);
    return 1;
  }
  log_init(g_cfg.data_dir, g_cfg.log_level);
  int crashed = crash_init(g_cfg.data_dir);
  LOGI("%s %s starting (%s, data %s, port %d)", XENO_APP_NAME, XENO_VERSION, sys_platform(), g_cfg.data_dir, g_cfg.port);
  LOGI("threads: platform default stack %zu KiB, store threads use %u KiB", xthread_platform_default_stack() >> 10, XT_STACK_DEFAULT >> 10);

  /* A newer version downloaded from Settings takes over before anything binds,
   * resumes a download or touches the queue (host: only with XENO_HANDOFF=1).
   * crash.txt stays in place until then, so that copy sees the report too. */
  update_init(g_cfg.data_dir);
  int try_handoff = 1;
#ifdef XENO_HOST
  try_handoff = getenv("XENO_HANDOFF") != NULL;
#endif
  if (try_handoff && update_handoff_newer(cx_local_port("LOADER", 9021))) {
    log_shutdown();
    return 0;
  }
  int restarted = update_restarted();   /* before update_handoff_done() clears the marker */
  crash_consume();
  if (crashed) {
    long up = -1;
    const char *rep = crash_previous(&up, NULL);
    LOGW("the previous run stopped unexpectedly after %ld s:", up);
    char line[200];
    for (const char *q = rep; *q;) {
      const char *nl = strchr(q, '\n');
      size_t l = nl ? (size_t)(nl - q) : strlen(q);
      if (l >= sizeof line) l = sizeof line - 1;
      memcpy(line, q, l);
      line[l] = 0;
      if (line[0]) LOGW("  %s", line);
      if (!nl) break;
      q = nl + 1;
    }
    /* Crashed soon after start: keep this session up by skipping the automatic
     * network duties (catalog/update/feed refresh, setup internet probe). Every
     * action the user starts still runs. */
    if (up >= 0 && up < CRASH_EARLY_S) {
      g_cfg.safe_mode = 1;
      LOGW("safe start: automatic background network checks are off for this session");
    }
  }

  settings_init(g_cfg.data_dir);
  pairing_init(g_cfg.data_dir);
  if (xhttp_global_init() != 0) LOGW("transport init failed: %s — downloads unavailable", xhttp_last_error());
  else LOGI("transport: %s", xhttp_backend_name());
  catalog_init(g_cfg.data_dir);
  storage_init();
  dl_init(g_cfg.data_dir, g_cfg.port);
  update_init(g_cfg.data_dir);
  api_init();
  api_ext_init();

  int srv = server_start(g_cfg.port);
  if (srv == -1) {
    LOGE("cannot bind :%d (%s)", g_cfg.port, strerror(errno));
    update_handoff_done();   /* another copy holds the port; this one did come up */
    sys_toast("XENO STORE already running on :%d", g_cfg.port);
    log_shutdown();
    return 0;
  }
  if (srv != 0) {
    /* The hand-off marker stays: an update that cannot start is set aside at the next start. */
    LOGE("cannot start the server threads");
    sys_toast("XENO STORE could not start its server · send the payload again");
    log_shutdown();
    return 1;
  }
  update_handoff_done();
  pthread_t hk;
  if (xthread_start(&hk, "housekeeping", housekeeping, NULL, 0, 0) != 0) { LOGE("cannot start housekeeping thread"); return 1; }

  /* [launcher builder] Server is bound — publish the deep-link home-screen icon
   * (/user/app/XENO00002). Idempotent, never fatal; a logged no-op on host. */
  launcher_icon_register();

  /* The native TV app (PPSA99500) starts this copy through the ELF loader,
   * which takes raw bytes only, so it cannot pass --no-browser. It polls
   * GET /api/v1/system every 0.5 s while it waits, so it shows up within 2 s:
   * then the PS5 browser and the toasts would only cover it (it shows the
   * pairing code itself, under Pair a device). PORT-SPEC 1.8 E8. */
  int tv_app = 0;
  if (tvapi_client_seen) {
    for (int i = 0; i < 20 && !g_cfg.stop_requested; i++) {
      if ((tv_app = tvapi_client_seen()) != 0) break;
      util_sleep_ms(100);
    }
  }
  if (tv_app) LOGI("the TV app is polling: no browser and no start-up toasts");
  else pairing_announce();

  char ip[48];
  sys_local_ip(ip, sizeof ip);   /* "127.0.0.1" stays when there is no LAN address */
  char url[96];
  xsnprintf(url, sizeof url, "http://%s:%d/", ip, g_cfg.port);
  /* Open the store on the TV (Settings → General → "Open XENO STORE when it
   * starts"): the XENO STORE app when it is installed and registered (it finds
   * this copy running), otherwise, or when the app does not start, the
   * browser. Skipped when the Games-row app already asked for the browser,
   * and when the TV app started this copy (it is open already); the browser
   * again when Settings → Updates → Restart now started this copy. */
  int open = app_install_open_choice(tv_app, !g_cfg.no_browser && settings_open_on_start(), sys_browser_opened_ago_ms() <= 15000,
                                     restarted, app_install_launchable());
  if (open == APP_OPEN_APP) {
    char err[256];
    if (cx_library_app_launch(APP_TITLE_ID, err, sizeof err) == 0) LOGI("start-up: opened the XENO STORE app (%s)", APP_TITLE_ID);
    else {
      LOGW("start-up: the XENO STORE app did not open (%s), opening the browser", err);
      open = APP_OPEN_BROWSER;
    }
  }
  if (open == APP_OPEN_BROWSER) {
    char local[64];
    xsnprintf(local, sizeof local, "http://127.0.0.1:%d/?tv=1", g_cfg.port);
    sys_launch_browser(local);
  }
  if (crashed) sys_toast("XENO STORE restarted after a problem · details in Settings → Diagnostics");
  if (!tv_app) sys_toast("XENO STORE ready · %s", url);
  LOGI("ready: %s (fw %s)", url, sys_fw_version());

  while (!g_cfg.stop_requested) util_sleep_ms(250);

  /* The port closes before main waits for housekeeping (E9): a catalogue
   * refresh or update check in flight can take minutes, and the TV app's
   * restart waits at most 30 s for the old copy to go away, then starts the
   * next copy on the same data directory. So everything this copy saves is
   * saved before the port closes: dl_shutdown() stops whatever still runs
   * (the TV stop drained the downloads already) and writes queue.json a last
   * time, and once the port is closed nothing is persisted any more, so a
   * late housekeeping step cannot revert what the next copy saved. */
  LOGI("shutting down");
  dl_shutdown();
  server_stop();
  settings_freeze();
  dl_freeze_saves();
  pthread_join(hk, NULL);
  if (g_cfg.restart_requested) {
    char err[256];
    if (update_relaunch(cx_local_port("LOADER", 9021), "restart", err, sizeof err) != 0) sys_toast("XENO STORE update: %s — send the payload again", err);
  }
  LOGI("bye");
  log_shutdown();
  return 0;
}
