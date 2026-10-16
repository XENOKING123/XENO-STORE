/*
 * dl.h — download engine: queue, worker threads, segmented transfer, resume
 * sidecars, retries/failover, verification, completion steps (SPEC §7).
 *
 * All job state lives behind queue_mu. Worker threads never hold the lock
 * across I/O; they take it briefly to publish progress and to pick chunks.
 */
#ifndef XENO_DL_H
#define XENO_DL_H

#include "json.h"
#include <pthread.h>
#include <stdint.h>
#include <time.h>

#define DL_MAX_JOBS 256
#define DL_MAX_MIRRORS 8
#define DL_MAX_SEGMENTS 16
#define DL_HISTORY 60
#define DL_JOB_LOG_LINES 40
#define DL_JOB_LOG_LEN 160
#define DL_RESERVE_BYTES (256LL << 20)

typedef enum dl_state {
  DL_QUEUED = 0, DL_CONNECTING, DL_DOWNLOADING, DL_VERIFYING, DL_DONE,
  DL_PAUSED, DL_FAILED, DL_CANCELLED, DL_INSTALLING, DL_INSTALLED,
  DL_EXTRACTING, DL_EXTRACTED,          /* ZIP archives after download (SPEC §17.19) */
  DL_STATE_COUNT
} dl_state;

/* Why a running worker was asked to stop. */
enum { DL_STOP_NONE = 0, DL_STOP_PAUSE = 1, DL_STOP_CANCEL = 2, DL_STOP_REMOVE = 3, DL_STOP_SHUTDOWN = 4 };

typedef struct dl_chunk {
  int64_t start, end;   /* [start, end) ; end == -1 → until EOF (unknown size) */
  int64_t cur;          /* next byte to write (live progress) */
  int64_t durable;      /* bytes up to here are fsynced; only the sidecar reads this */
  int active;           /* a segment thread owns it */
  uint32_t gen;         /* changes whenever the slot is reused, so a checkpoint
                           snapshot is only applied to the chunk it was taken from */
} dl_chunk;

typedef struct dl_install {
  char state[12];       /* idle | installing | installed | failed */
  char message[256];
  char contentId[40];
  char titleId[16];
  char token[24];       /* /pkgsrv/<token> for the etaHEN DPI fallback */
  char via[24];
  time_t startedAt;
  int rc;
  int pkgDeleted;       /* downloads.deletePkgAfterInstall removed the .pkg once it installed */
} dl_install;

/* Turbo start (SPEC §17.5): the first 1 MiB of up to 3 mirrors is raced. */
#define DL_TURBO_MAX 3
typedef struct dl_turbo_probe {
  char url[1024];
  int ms;              /* time to the first MiB (or to the error) */
  double mbps;         /* megabits per second over the probe */
  int status;          /* HTTP status, 0 = transport error */
  int64_t bytes;
} dl_turbo_probe;
typedef struct dl_turbo {
  dl_turbo_probe tested[DL_TURBO_MAX];
  int ntested;
  char picked[1024];
  int done;            /* the race ran (or was skipped) for this job */
} dl_turbo;

/* ZIP extraction result (SPEC §17.19). */
typedef struct dl_extract {
  char state[12];      /* idle | extracting | extracted | failed | skipped */
  char dir[1024];      /* destination folder or the extracted image path */
  char message[160];
  int files;
  int64_t bytes;
  int archiveDeleted;
} dl_extract;

typedef struct dl_job {
  /* identity + catalog data */
  char id[16], gameId[128], title[256], titleId[16], platform[8], cover[512];
  char releaseId[128], format[16], provider[64], sourceId[32];
  char url[2048];                      /* source URL (primary) */
  char activeUrl[2048];                /* what is being fetched now (url or a mirror) */
  char mirrors[DL_MAX_MIRRORS][1024];
  int nmirrors, mirror_idx;            /* mirror_idx -1 = primary url */
  char filename[256], root[256], rootLabel[32], path[1024], xpart[1040], sidecar[1056];
  /* state */
  dl_state state;
  char phase[64], message[256], error[256], errorCode[32];
  int64_t total, written;
  double speed;
  int64_t eta;
  int connections;
  char sha256[65], sha256Expected[65], verify[8];
  int verified;
  time_t addedAt, startedAt, finishedAt, nextRetryAt;
  int attempts, priority, autoInstall, resumable;
  dl_install install;
  char etag[128], lastModified[64];
  int synthetic;                       /* library install job: no transfer */
  /* ---- Phase 2 (SPEC §17) --------------------------------------------- */
  char type[12];                       /* download | copy */
  char group[16];                      /* install set id "g_xxxxxx" or "" */
  int groupOrder;                      /* position inside the set */
  char kind[12];                       /* game | update | dlc | backport | app */
  char via[16];                        /* requested: direct | auto | <service id> */
  char viaService[16];                 /* service that unrestricted the link ("" = direct) */
  char viaLabel[32];                   /* "Real-Debrid" … for the UI */
  dl_turbo turbo;
  char scheduledFor[8];                /* "22:00" while the schedule window holds the job */
  int startNow;                        /* "Start now anyway" override */
  char extractPassword[129];           /* catalog-supplied / user override ZIP password (SPEC §17.19) */
  char srcPath[1024];                  /* copy jobs: source file */
  int move;                            /* copy jobs: unlink the source afterwards */
  dl_extract extract;
  double peakSpeed;                    /* best 1 s sample of this job (stats) */
  int restResumes;                     /* sleep/wake cycles survived (no attempts consumed) */
  int stopPaused;                      /* paused by dl_pause_all_for_stop(), not by the user: queued again on the next start */
  /* runtime (never persisted) */
  volatile int rest_hit;               /* a wake-up event interrupted the transfer */
  int last_err_class;                  /* XHTTP_ERR_* of the last network error */
  int via_active;                      /* activeUrl is an unrestricted (ephemeral) link */
  int64_t transfer_ms;                 /* active transfer time for the stats */
  int64_t transfer_started_ms;
  volatile int cancel;                 /* DL_STOP_* (user intent) */
  volatile int stop_io;                /* segments must abort now (cancel or fatal error) */
  volatile int running;                /* a worker thread owns the job */
  volatile int remove_pending, delete_files_pending, discard_partial;
  int fd;
  unsigned slot_mask;                  /* segment slots in use */
  int seg_retry_now, seg_err_streak, drive_missing, insecure_tls;
  dl_state verify_prev_state;
  dl_chunk *chunks;
  int nchunks;
  uint32_t chunk_gen;                  /* source of dl_chunk.gen */
  int64_t chunk_size;
  int segmented, window, active_segments;
  int usb;                             /* exFAT/USB root: bounded 8 MiB in-flight window, no preallocation */
  int64_t clean_bytes, last_fsync, last_space_check;
  int seg_fatal, seg_status;           /* set by a segment thread on a non-retryable error */
  char seg_error[160];
  char seg_code[32];
  int retry_after;
  int64_t last_seg_error_ms;
  int64_t last_sample_written, last_sample_ms;
  int64_t hist[DL_HISTORY];
  int hist_n, hist_head;
  char log[DL_JOB_LOG_LINES][DL_JOB_LOG_LEN];
  int log_n, log_head;
  int64_t last_sidecar_ms;
  int probe_status;
  int url_replaced;
  void *seg_conns[DL_MAX_SEGMENTS];    /* xhttp_conn* per slot for prompt abort */
  /* downloads.deletePkgAfterInstall: this install may remove the .pkg once its
   * status showed it in progress (of pkg_del_total bytes) and then complete */
  int pkg_del_wait, pkg_del_seen;
  int64_t pkg_del_total;
} dl_job;

typedef struct dl_add_req {
  const char *gameId, *releaseId, *root, *url, *filename, *format, *title, *sha256;
  jv mirrors;               /* optional JSON array of extra mirror URLs */
  int64_t size;             /* expected size for raw urls (0 = unknown) */
  int connections;          /* 0 = settings default */
  int verify;               /* -1 default, 0/1 */
  int autoInstall;          /* -1 default, 0/1 */
  int overwrite;
  /* Phase 2 (optional; NULL/0 = defaults). `body` is the whole request
   * object when the caller has it — dl_add() reads via/kind/group from it
   * (and falls back to the document behind `mirrors`). */
  jv body;
  const char *via;          /* direct | auto | realdebrid | alldebrid | torbox | premiumize */
  const char *kind;         /* game | update | dlc | backport | app */
  const char *group;        /* install set id, "" = none */
  const char *password;     /* ZIP extraction password (catalog or user override), "" = none */
  int groupOrder;
} dl_add_req;

typedef struct dl_summary {
  int active, queued, paused, failed, done;
  int64_t speed, remaining, eta;
  int keepAwake;
  /* Phase 2 */
  int scheduledWaiting;     /* queued jobs held by the schedule window */
  int restResuming;         /* jobs waiting for the network after a wake-up */
  int extracting, copying;
} dl_summary;

/* Install-set status (SPEC §17.13), computed from the jobs of one group. */
typedef struct dl_group_status {
  char id[16], gameId[128], title[256];
  int total, downloaded, installed, failed, pending;
  char status[16];          /* downloading | installing | installed | paused | cancelled */
  char pausedAt[300];       /* "update: Game Title" when paused */
  char pausedJobId[16];
} dl_group_status;

/* A lean copy of one job (about 6 KB) for readers that must not hold queue_mu
 * while they look things up elsewhere (the TV app's /api/v1 layer). Strings
 * are copied as the job holds them; `index` is the job's position in the
 * queue array (queue order), `type` is "download" or "copy". */
typedef struct dl_brief {
  char id[16], type[12], kind[12];
  char gameId[128], releaseId[128], titleId[16], title[256], format[16], filename[256], sourceId[32];
  char root[256], path[1024], srcPath[1024];          /* srcPath: copy jobs only */
  char phase[64], message[256], error[256], errorCode[32];
  char via[16], viaService[16];
  char extractState[12], extractDir[1024], extractMessage[160];
  char installState[12], installMessage[256];
  dl_state state;
  int64_t total, written;
  double speed;
  time_t nextRetryAt, addedAt, finishedAt;
  int verified, synthetic, running, cancel, removePending, restHit, move, priority, index;
} dl_brief;

/* Loads queue.json. A download that was running when the store stopped (an
 * update, a restart, a crash) or that the TV app's stop paused is queued again
 * while downloads.resumeAfterRestart is on (the scheduler starts it under the
 * concurrent-jobs limit), else it comes back paused; one the user paused stays
 * paused. */
int dl_init(const char *data_dir, int port);
/* Stops every running job (at most ~5 s), holds the scheduler and saves
 * queue.json a last time. */
void dl_shutdown(void);
/* Shutdown, once the port is closed: queue.json and stats.json are never
 * written again (the next copy may own them already). A save already running
 * finishes first. */
void dl_freeze_saves(void);
/* Called by the housekeeping thread every ~500 ms. */
void dl_housekeeping_tick(void);
/* Writes queue.json now. Returns 0, or -1 when the file could not be written
 * (the queue stays dirty and the housekeeping tick retries every 5 s). */
int dl_save_queue(void);
/* 0 after a queue.json write failed, 1 again after the next one succeeded
 * (GET /api/v1/system "stateHealthy"). */
int dl_state_healthy(void);
/* The TV app's stop, steps 1-2 (PORT-SPEC 1.5.5): under the queue lock, pauses
 * every queued job (retry waits included) and every active one (connecting,
 * downloading, verifying, extracting; copy jobs too) with `msg`, holds the
 * scheduler so nothing new starts, then saves queue.json synchronously.
 * Returns the number of jobs paused (>= 0) once the queue is saved, or -1 when
 * it could not be saved: then every job is put back as it was (a job whose
 * worker was already told to stop goes back to queued) and nothing is held.
 * The paused jobs are marked stopPaused, so the next start queues them again.
 * Returns DL_PAUSE_INSTALLING, changing nothing, while a package install is
 * being started or reads its package from /pkgsrv/ (etaHEN DPI :12800): the
 * token lives only in this process, so a stop would break that install. */
#define DL_PAUSE_INSTALLING (-2)
int dl_pause_all_for_stop(const char *msg);
/* Jobs whose worker thread still owns them (j->running), whatever their state:
 * download, verify, extract, copy and install workers. The stop drain waits
 * for 0 before the process exits. */
int dl_workers_running(void);
/* Copies every job into a malloc'd array in queue order (caller frees *out;
 * *out is NULL when there are no jobs). Returns the count, or -1 on OOM.
 * Never call it while holding a lock the caller also takes inside a dl_*
 * callback; look the copies up (catalogue, settings) after it returns. */
int dl_brief_list(dl_brief **out);
/* One job by id: 0 and *out filled, or -1 when unknown. */
int dl_brief_get(const char *id, dl_brief *out);

/* Returns 0 and the new job id, or -1 with errcode/errmsg (SPEC §6.3 codes). */
int dl_add(const dl_add_req *req, char *job_id, size_t idcap, char *errcode, size_t ccap, char *errmsg, size_t mcap);
/* action ∈ pause resume cancel retry remove restart install verify move url
 * startNow extract. cancel also accepts a failed job; cancel {deleteFiles:true}
 * leaves written = 0 once the partial is gone; extract takes an optional
 * {"password":"..."} that is tried first (validate it before the call). */
int dl_action(const char *id, const char *action, jv body, char *errcode, size_t ccap, char *errmsg, size_t mcap);
int dl_clear(jv states);
/* Library install: synthetic job in state installing for a scanned .pkg. */
int dl_add_install_job(const char *path, char *job_id, size_t idcap, char *errmsg, size_t mcap);
/* Copy/move between drives (SPEC §17.16): a job of type "copy" in the same
 * queue. `src` must be a path from the last library scan or a job-owned
 * final file. Returns 0 and the id, or -1 with errcode/errmsg. */
int dl_add_copy_job(const char *src, const char *toRoot, int move, int overwrite, char *job_id, size_t idcap,
                    char *errcode, size_t ccap, char *errmsg, size_t mcap);
/* Install sets: status of every group (returns the count written). */
int dl_group_statuses(dl_group_status *out, int max);
/* Host-only test hook: makes the engine treat the next network failures as a
 * sleep/wake cycle (SPEC §17.15). No-op on the console. */
void dl_simulate_wake(void);

void dl_queue_json(jw *w, int history);             /* writes "jobs" + "summary" */
int dl_job_json(jw *w, const char *id, int history);/* writes the job object (key set by caller) */
int dl_job_log_json(jw *w, const char *id);          /* writes "lines" */
void dl_summary_get(dl_summary *out);
int64_t dl_remaining_on_root(const char *root);
int dl_any_downloading(void);
int dl_find_job_by_path(const char *path, char *id, size_t cap);
const char *dl_state_name(dl_state s);

#endif
