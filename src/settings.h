/*
 * settings.h — settings/favorites/collections/recent persistence (SPEC §9).
 *
 * The settings live in a plain C struct guarded by settings_mu; callers take
 * a snapshot with settings_get() (cheap memcpy) instead of holding the lock.
 */
#ifndef XENO_SETTINGS_H
#define XENO_SETTINGS_H

#include "json.h"
#include <stdint.h>
#include <time.h>

#define SET_MAX_SOURCES 24
#define SET_MAX_FAVORITES 4000
#define SET_MAX_COLLECTIONS 32
#define SET_MAX_COLLECTION_IDS 1000
#define SET_RECENT_KEEP 30
#define SET_ID_LEN 96
#define SET_DEBRID_KEY_LEN 160
#define SET_MAX_INDEX_URLS 8
#define SET_ANONKEY_LEN 512
#define SET_MAX_PRESETS 16
#define SET_PRESET_FILTERS_LEN 1024

typedef struct settings_preset { char name[64]; char filters[SET_PRESET_FILTERS_LEN]; char sort[32]; } settings_preset;

typedef struct settings {
  /* general */
  char language[12];
  char theme[24];
  char cardSize[4];
  char designPack[16];   /* broadcast | gallery | minimal | arcade */
  int reduceMotion, sounds, hints, showPs4, adultFilter;
  int openOnStart;       /* open the store in the PS5 browser when it starts */
  /* downloads */
  char defaultRoot[256];
  int connections, concurrentJobs;
  int64_t maxBytesPerSec;
  int verifyHashes, autoInstallPkg, shadowMountScan, keepAwake, retryMax, allowInsecureTls;
  char scheduleStart[8], scheduleEnd[8];
  /* sources */
  struct { char id[32]; int enabled; } sources[SET_MAX_SOURCES];
  int n_sources;
  int sourcesAcknowledged;
  /* catalog */
  int catalogAutoRefresh;
  char catalogUrl[512], catalogRevisionUrl[512];
  time_t catalogLastCheck;
  /* notifications */
  int toasts, onComplete, onFail, onInstall;
  /* updates */
  int updatesAutoCheck;
  char updatesFeed[512];
  time_t updatesLastCheck;
  /* pairing */
  char pin[8];

  /* ---- Phase 2 (SPEC §17) ------------------------------------------------ */
  /* general */
  int largeText, highContrast, safeMode, screensaver;
  int screensaverMinutes;            /* 1..30, default 3 */
  int controllerSwapConfirm;
  int mdns;                          /* default 1: answer xeno.local / xenostore.local */
  /* downloads */
  char preferFormat[12];             /* auto | exFAT | FFPFSC | PKG */
  int turboStart, autoInstallSets;
  char installOrder[48];             /* comma list of game|backport|update|dlc */
  int restModeAutoResume, extractZip, deleteArchiveAfterExtract;
  /* debrid: keys stay on the console; "" = not connected */
  struct {
    struct { char apiKey[SET_DEBRID_KEY_LEN]; } realdebrid, alldebrid, torbox, premiumize;
    char preferred[16];              /* auto | direct | realdebrid | alldebrid | torbox | premiumize */
  } debrid;
  /* catalog feeds (owner-curated JSON next to the catalog) */
  char featuredUrl[512], messagesUrl[512], companionsUrl[512], savesBackendUrl[512];
  /* saves hub + Save Data Store */
  struct {
    char backupRoot[256];            /* "" = downloads.defaultRoot */
    int autoBackup;
    char indexUrls[SET_MAX_INDEX_URLS][512];
    int n_indexUrls;
    struct {
      char kind[12];                 /* supabase | github | none */
      char url[256], anonKey[SET_ANONKEY_LEN], bucket[64], garlicFunction[64];
    } backend;
    char uploadHost[12];             /* site (default) | pixeldrain | catbox | custom */
    char uploadEndpoint[512], submitEndpoint[512];
    char nickname[64];
  } saves;
  /* browse presets: filters are kept as their JSON text (opaque to C) */
  struct {
    settings_preset presets[SET_MAX_PRESETS];
    int n_presets;
  } browse;
} settings_t;

void settings_init(const char *data_dir);
void settings_get(settings_t *out);
/* Cheap single-field reads (no 30 KB settings_t copy): use these on polled
 * paths (the TV app asks /api/v1/system every 0.5-3 s). */
int settings_open_on_start(void);
int settings_concurrent_jobs(void);                 /* downloads.concurrentJobs, clamped 1..4 */
void settings_default_root(char *out, size_t cap);  /* downloads.defaultRoot (a path) */
int settings_delete_archives(void);                 /* downloads.deleteArchiveAfterExtract, 0/1 */
int settings_updates_auto_check(void);              /* updates.autoCheck, 0/1 */
int settings_sources_acknowledged(void);            /* sources.acknowledged, 0/1 */

/* Status codes of the *_checked / *_set functions below. */
#define SETTINGS_ERR_INVALID (-1)   /* rejected, nothing changed (err filled where there is one) */
#define SETTINGS_ERR_SAVE (-2)      /* applied in memory, but the file could not be written */
#define SETTINGS_ERR_FULL (-3)      /* no room (favourites) */

/* Partial merge of a JSON object (unknown keys ignored, ranges clamped), then
 * atomic save. Returns 0 or -1 with err filled. A failed settings.json write
 * is only logged (the change stays in memory). */
int settings_merge(jv obj, char *err, size_t errcap);
/* Same merge, but reports a failed write: 0, SETTINGS_ERR_INVALID (err says
 * why) or SETTINGS_ERR_SAVE (applied in memory; the next successful save
 * writes it). For the /api/v1 replies that need "Could not save ..." (503). */
int settings_merge_checked(jv obj, char *err, size_t errcap);
void settings_write_json(jw *w);
int settings_source_enabled(const char *source_id);
void settings_touch_catalog_check(void);
void settings_touch_updates_check(void);
/* Effective backup root for saves: saves.backupRoot when set, else downloads.defaultRoot. */
void settings_saves_root(const settings_t *s, char *out, size_t cap);

/* favorites */
void settings_favorites_json(jw *w);          /* {"ok":true,"ids":[...]} body part: writes "ids" key */
int settings_favorites_set(jv ids);
int settings_favorite_toggle(const char *id);  /* returns new state 0/1 */
/* Sets one favourite on or off (idempotent, set semantics): 1 changed and
 * saved, 0 already in that state (nothing written), SETTINGS_ERR_INVALID bad
 * id, SETTINGS_ERR_FULL no room to add, SETTINGS_ERR_SAVE changed in memory
 * but favorites.json could not be written. */
int settings_favorite_set(const char *id, int on);
int settings_favorites_count(void);
int settings_is_favorite(const char *id);
int settings_favorites_list(char (*out)[SET_ID_LEN], int max);

/* collections */
void settings_collections_json(jw *w);        /* writes "collections" key */
int settings_collections_set(jv list, char *err, size_t errcap);

/* recent */
void settings_recent_json(jw *w);             /* writes "ids" key */
void settings_recent_add(const char *id);

/* export / import */
void settings_export_json(jw *w);             /* whole object incl. settings/favorites/collections/recent */
int settings_import_json(jv obj, char *err, size_t errcap);

/* catalog.lastCheck (the start of the last catalogue check), cheap. */
time_t settings_catalog_last_check(void);

/* 1 when an API key is stored for that debrid service (cheap, for polled paths). */
int settings_debrid_key_set(const char *service);

/* Shutdown, once the port is closed: settings.json, favorites, collections and
 * recent are never written again, so a late write from this process (a
 * housekeeping check that finishes after the next copy started on the same
 * data directory) cannot revert what the next copy saved. A save already
 * running finishes first. */
void settings_freeze(void);

#endif
