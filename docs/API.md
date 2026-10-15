# XENO STORE — HTTP API reference

Base URL: `http://<ps5-ip>:9095` (or `http://xeno.local:9095`). Every endpoint lives under `/api` except the
static routes in §1. This file follows the route tables in `src/api.c` (core, SPEC §6), `src/ext_downloads.c`
(download extensions, §17.5 / §17.13 / §17.14 / §17.16) and `src/ext_console.c` (console features, §17.2 /
§17.6 / §17.16–§17.22); the module name is given with each group so the code can be checked.

**Envelope.** Success → the documented object, always with `"ok": true`. Failure →
`{"ok":false,"error":"<code>","message":"<human text>"}` with a matching status (400 bad request, 401 bad key,
403 forbidden, 404 not found, 409 exists, 413 body too large, 422 cannot do it now, 428 confirm required,
429 busy, 500 io/oom, 502 upstream, 503 feature unavailable on this firmware). Timestamps are Unix seconds,
sizes are bytes. Bodies are JSON (`Content-Type: application/json`), limit 1 MiB. CORS is open
(`Access-Control-Allow-Origin: *`); `OPTIONS` answers preflights.

**Pairing PIN.** When `settings.pairing.pin` is set, every mutating request must carry
`X-XENO-PIN: 1234`; otherwise `401 {"error":"pin_required","pinRequired":true}`.

Routes with `{id}` style parameters are matched by segment; `id` values are the ones returned by the API.

---

## 1. Static routes (`src/server.c`)

| Route | Returns |
|:--|:--|
| `GET /`, `GET /index.html` | The embedded UI (`text/html`, `Cache-Control: no-cache`, ETag = build hash, gzip when accepted). |
| `GET /catalog.json` | The current catalog (embedded or the newer cached copy), gzip when accepted, ETag = revision. |
| `GET /logo.jpg`, `GET /icon.png` | Header badge / 512×512 icon. |
| `GET /manifest.webmanifest` | PWA manifest so phones can "Add to home screen". |
| `GET /i18n/<code>.json` | An embedded locale file. |
| `GET /pkgsrv/<token>` | Range-capable serving of a staged `.pkg` for the etaHEN DPI fallback (loopback only). |

---

## 2. System (`src/api.c`)

### `GET /api/status`

```json
{"ok":true,"app":"XENO STORE","version":"1.0.0","build":"a1b2c3d","platform":"ps5","fw":"11.00",
 "ip":"192.168.1.20","port":9095,"uptime":1234,"downloadsAvailable":true,"transport":"sceHttp",
 "catalog":{"revision":2,"games":1965,"releases":2592,"source":"embedded","updatedAt":1760313600},
 "queue":{"active":1,"queued":2,"paused":0,"failed":0,"done":5,"speed":52428800,"remaining":10737418240,"eta":205,"keepAwake":true},
 "storage":[{"path":"/data","label":"Internal","kind":"internal","free":412316860416,"total":825000000000,"writable":true,"default":true}],
 "update":{"available":false,"latest":"1.0.0","current":"1.0.0","url":"","sha256":"","notes":"","checkedAt":1760313600},
 "featured":{"hero":["ghost-of-yotei-ps5"],"rows":[{"title":{"en":"Staff picks"},"ids":["elden-ring-ps5"]}],"homebrew":["RetroArch_1_19"],"banner":{"text":{"en":"…"},"url":""}},
 "messages":[{"id":"welcome-1.0.0","ts":1760313600,"url":"","texts":{"en":"Welcome…"}}],
 "setup":{"checked":true,"total":8,"failing":1,"at":1760313600},
 "savesEngine":"native","mdns":{"enabled":true,"running":true,"names":["xeno.local","xenostore.local"],"ip":"192.168.1.20","port":9095,"url":"http://xeno.local:9095","error":""},
 "consoleJobs":0,
 "notices":[{"id":"downloads_unavailable","text":"…"}]}
```

`featured`, `messages`, `setup`, `savesEngine`, `mdns` and `consoleJobs` come from `api_status_extra()` in `ext_console.c`. `platform` is `ps5` or `host`; `transport` names the HTTP backend in use.

### `GET /api/log?lines=200` → `{"ok":true,"lines":["…"]}` (ring buffer, max 400).
### `POST /api/system/toast {"text":"Hello"}` → `{"ok":true}` (debug; shows a console notification).
### `POST /api/system/stop` → `{"ok":true,"stopping":true}` — saves the queue, toasts, exits (used before sending a new ELF).
### `GET|POST /api/update/check`

```json
{"ok":true,"current":"1.0.0","latest":"1.0.1","url":"https://github.com/…/xeno-store.elf","sha256":"…","notes":"…","available":true,"checkedAt":1760313600}
```
Reads `settings.updates.feed` (`updates/payloads.json`), falling back to the GitHub release API + `SHA256SUMS`.

### `POST /api/update/apply` → `{"ok":true,"path":"/data/xeno-store/xeno-store.elf","restartRequired":true}` (downloads to `.elf.new`, verifies the sha256, renames). Failure: `502 update_failed` with the reason (no update, hash mismatch, network).

---

## 3. Catalog & links (`src/api.c`)

### `GET /api/catalog/info`
```json
{"ok":true,"revision":2,"generatedAt":"2026-10-13T04:19:46Z","games":1965,"releases":2592,"sources":[{"id":"archive","name":"Archive.org","kind":"direct"}],"embeddedRevision":2,"cachedRevision":0}
```
### `POST /api/catalog/refresh` → `{"ok":true,"updated":true,"revision":3}` (blocking: up to ~15 s for `revision.json` plus up to 120 s for the catalogue; also refreshes the feeds). Refreshes never overlap: while one runs (housekeeping, web UI or TV app) another answers `409 refresh_busy`, and `GET /api/status` shows `catalog.refreshing:true`.
### `GET /api/catalog/search?q=elden&platform=PS5&limit=20` → `{"ok":true,"items":[<game summary>…]}` (server-side search for phones and as a fallback).
### `POST /api/links/check {"urls":["https://…"]}`
```json
{"ok":true,"results":[{"url":"https://…","status":206,"size":156011528192,"acceptRanges":true,"finalUrl":"https://…","ms":184}]}
```
≤ 8 URLs, 10 s each (`HEAD`, then a 1-byte Range `GET`).
### `POST /api/links/resolve {"url":"https://dlpsgame.com/…"}` → `{"ok":true,"links":[{"url":"…","label":"Base game (EUR)","host":"1fichier.com","size":0}]}`. Only allow-listed hosts are fetched (dlpsgame.com, archive.org, raw.githubusercontent.com, github.com, api.github.com, prosperopatches.com, orbispatches.com, vikingfile.com); unknown hosts answer `links:[]`.

### `GET /api/game/best?id=<gameId>&fw=11.00` (`src/ext_downloads.c`, §17.5)
Picks the release to download: same kind → compatible with the console firmware → direct → verified → preferred format → newest → smallest.
```json
{"ok":true,"fw":"11.00","releaseId":"ppsa04029-ffpfsc","release":{…catalog release…},
 "reasons":[{"code":"compat","vars":{"fw":"11.00","requires":"9.00"}},{"code":"verified","vars":{}},{"code":"newest","vars":{"v":"01.003.002"}}],
 "alternatives":["ppsa04029-exfat"],"candidates":3}
```
Reason codes: `compat`, `verified`, `direct`, `hash`, `newest`, `smallest`, `preferred`, `onlyOne`, `pageOnly`. Errors: `not_found`, `no_release`.

### `GET /api/game/enrich?id=<gameId>&refresh=0` (`src/ext_console.c`, §17.2)
```json
{"ok":true,"id":"elden-ring-ps5","cached":true,"fetchedAt":1760313600,"stale":false,"offline":false,"source":"steam",
 "screenshots":["https://…"],"about":"…","developer":"FromSoftware","publisher":"Bandai Namco","metacritic":96,
 "features":["Single-player","Online multiplayer"],"genres":["Action","RPG"],"languages":["English","…"],"releaseDate":"2022-02-25","ageRating":16,"steamId":1245620,"headerImage":"https://…","website":""}
```
Steam `storesearch` + `appdetails`, cached 30 days at `/data/xeno-store/enrich/<gameId>.json`, merged over the catalog. Offline → `{"ok":true,"cached":false,"offline":true,"screenshots":[]}`.

### `GET /api/feeds` / `POST /api/feeds/refresh` (`src/ext_console.c`)
```json
{"ok":true,"featured":{…},"messages":{…},"companions":{…},"savesBackend":{…},"fetchedAt":{"featured":1760313600,"messages":1760313600,"companions":1760313600,"savesBackend":1760313600}}
```
The four owner-curated feeds (`updates/featured.json`, `messages.json`, `companions.json`, `saves-backend.json`), cached at `/data/xeno-store/feeds/`, refreshed every 6 h with the catalog; `null` when never fetched.

---

## 4. Queue (`src/api.c`, `src/dl.c`)

### Job object
```json
{"id":"j_1a2b3c","type":"download","gameId":"cyberpunk-2077-ps5","title":"Cyberpunk 2077","titleId":"PPSA04029","platform":"PS5",
 "cover":"https://…","releaseId":"ppsa04029-ffpfsc","kind":"game","format":"FFPFSC","provider":"Archive.org","sourceId":"archive",
 "url":"https://…","activeUrl":"https://…","mirrors":["https://…"],"mirrorIndex":0,"filename":"PPSA04029 Cyberpunk 2077.ffpfsc",
 "root":"/data","rootLabel":"Internal","path":"/data/homebrew/PPSA04029 Cyberpunk 2077.ffpfsc",
 "state":"downloading","phase":"","message":"","error":"","errorCode":"",
 "total":156011528192,"written":12345678,"speed":52428800,"peakSpeed":61000000,"eta":2740,
 "connections":8,"segmented":true,"activeSegments":8,"window":8,"segments":[[0,16777216,16777216],[16777216,33554432,8000000]],
 "sha256":"","sha256Expected":"","verified":false,"verify":"size",
 "addedAt":1760000000,"startedAt":1760000010,"finishedAt":0,"attempts":1,"nextRetryAt":0,"priority":0,
 "autoInstall":true,"install":{"state":"idle","message":"","contentId":""},
 "resumable":true,"etag":"\"abc\"","lastModified":"","synthetic":false,
 "group":"g_9f8e7d","groupOrder":1,"via":"","viaService":"","viaRequested":"auto",
 "turbo":{"done":true,"picked":"https://…","tested":[{"url":"https://…","ms":120,"mbps":48.2,"status":206,"bytes":1048576}]},
 "scheduledFor":"","startNow":false,"srcPath":"","move":false,
 "extract":{"state":"idle","message":"","dir":"","files":0,"archiveDeleted":false},
 "restResumes":0,"restResuming":false,"speedHistory":[…60 samples when ?history=1…]}
```
States: `queued → connecting → downloading → verifying → done`, side states `paused`, `failed`, `cancelled`, `installing`, `installed`, plus `extracting` / `copying` for the matching job types. `type` is `download`, `copy` (drive transfer), `backup` (saves) or `save-install` / `save-download`. Error codes: `no_space`, `exists`, `bad_release`, `root_unavailable`, `downloads_unavailable`, `source_changed`, `provider_page`, `hash_mismatch`, `drive_missing`, `network`.

### `GET /api/queue?history=1`
```json
{"ok":true,"jobs":[…],"summary":{"active":1,"queued":2,"paused":0,"failed":0,"done":5,"speed":52428800,"remaining":10737418240,"eta":205,"keepAwake":true,"extracting":0,"copying":0,
 "restMode":{"pill":"awake","autoResume":true,"resuming":false,"lastWakeAt":0},
 "schedule":{"configured":true,"open":false,"start":"22:00","end":"07:00","nextStart":1760400000,"waiting":2},
 "groups":[{"id":"g_9f8e7d","gameId":"…","title":"…","total":3,"downloaded":1,"installed":0,"failed":0,"pending":2,"status":"downloading","pausedAt":"","pausedJobId":""}],
 "lifetime":{"totalBytes":2200000000000,"totalJobs":140,"bestSpeed":98000000,"avgSpeed":41000000}}}
```

### `POST /api/queue`
```json
{"gameId":"cyberpunk-2077-ps5","releaseId":"ppsa04029-ffpfsc","root":"/data","connections":8,"verify":"auto","autoInstall":true,"overwrite":false,"via":"auto"}
```
or a raw link: `{"url":"https://…/file.pkg","filename":"file.pkg","format":"PKG","title":"…","gameId":"…","root":"/data"}`.
Checks: release exists, drive writable, free ≥ total + 256 MiB, not already queued, final file absent (or `overwrite`). `via` is `auto`, `direct` or a debrid service id. → `{"ok":true,"job":{…}}`.

### `POST /api/queue/set` (`src/ext_downloads.c`, §17.13 install sets)
```json
{"gameId":"elden-ring-ps5","base":"ppsa05890-pkg","also":{"update":true,"dlc":false,"backport":false},"root":"/data","connections":8,"verify":"auto","autoInstall":true}
```
or an explicit list: `{"gameId":"…","items":["rel-a","rel-b"]}` / `{"items":[{"releaseId":"…","kind":"dlc"}]}`.
Orders the items by `settings.downloads.installOrder`, checks space for the whole set, creates every job with a shared `group` (rolls back on any failure) → `{"ok":true,"group":"g_9f8e7d","jobs":[…]}`.

### `GET /api/downloads/groups` → `{"ok":true,"groups":[…as in summary.groups…]}`.

### `POST /api/queue/{id}/{action}` → `{"ok":true,"job":{…}}`

| action | body | effect |
|:--|:--|:--|
| `pause` / `resume` | — | checkpoints / re-probes validators and continues |
| `cancel` | `{"deleteFiles":false}` | stops, state `cancelled`, keeps the partial unless asked |
| `retry` | — | clears the failure and requeues |
| `restart` | — | discards the partial, from zero |
| `remove` | `{"deleteFiles":false}` | forgets the entry; with `deleteFiles` also deletes partial/final + sidecar (job-owned paths only) |
| `install` | — | starts the PKG install for a `done` package |
| `verify` | — | computes the sha256 in the background, sets `verified` |
| `extract` | — | (re)runs the ZIP extraction of a finished archive |
| `start` | — | "Start now anyway" for a job waiting on the schedule window |
| `url` | `{"url":"https://…"}` | replace the source URL (Enter file link); the partial is kept only when the size matches |
| `move` | `{"direction":"up"\|"down"\|"top"\|"bottom"}` | reorder queued jobs |

### `POST /api/queue/clear {"states":["done","cancelled","failed"]}` → `{"ok":true,"removed":3}` (keeps files).
### `GET /api/queue/{id}/log` → `{"ok":true,"lines":[…]}`.
### `GET /api/space?root=/data&bytes=156011528192` → `{"ok":true,"root":"/data","free":412316860416,"afterQueue":300000000000,"afterThis":144000000000,"fits":true}`.

### `GET /api/stats` / `POST /api/stats/reset` (`src/ext_downloads.c`, §17.5)
```json
{"ok":true,"v":1,"totalBytes":2200000000000,"totalJobs":140,"bestSpeed":98000000,"avgSpeed":41000000,"totalSeconds":53000,
 "byProvider":{"Archive.org":{"bytes":1900000000000,"jobs":120}},"since":1759000000,"updatedAt":1760313600}
```

### `POST /api/library/transfer` (`src/ext_downloads.c`, §17.16 copy/move)
`{"path":"/data/homebrew/X.ffpfsc","toRoot":"/mnt/usb0","move":true,"overwrite":false}` → `{"ok":true,"job":{"type":"copy","srcPath":"…","move":true,…}}`. Errors: `same_drive`, `no_space`, `root_unavailable`, `forbidden` (path not from the last library scan).

### `POST /api/downloads/rest-mode/simulate` — **host build only**: fires the wake-up handler for tests → `{"ok":true,"simulated":true}`.
### `POST /api/debug/queue/pause-all` (`src/api.c`) — **host build only**: runs the TV app stop's pause step on its own (every queued and active job paused with "Paused for the XENO STORE restart", queue saved, nothing new starts afterwards) → `{"ok":true,"paused":2,"workersRunning":1,"stateHealthy":true}`; `503 save_failed` when `queue.json` cannot be written (the jobs are left as they were).

---

## 5. Debrid (`src/ext_downloads.c`, `src/dlx_debrid.c`, §17.14)

### `GET /api/debrid`
```json
{"ok":true,"preferred":"auto","services":[
 {"id":"realdebrid","name":"Real-Debrid","connected":true,"user":"nick","expires":1765000000,"hostsKnown":312,"hosts":["1fichier.com","mediafire.com"],"hostsUpdatedAt":1760300000,"error":""},
 {"id":"alldebrid","name":"AllDebrid","connected":false,"user":"","expires":0,"hostsKnown":0,"hosts":[],"hostsUpdatedAt":0,"error":""},
 {"id":"torbox","name":"TorBox","connected":false,…},{"id":"premiumize","name":"Premiumize","connected":false,…}]}
```
### `PUT|POST /api/debrid {"service":"realdebrid","apiKey":"…"}` → validates with the service's user endpoint, stores the key, answers like `GET`. Errors: `bad_key` (401), `network` / `service_error` (502), `not_found`.
### `DELETE /api/debrid/{service}` → forgets the key, answers like `GET`.

---

## 6. Storage & Library (`src/api.c`, `src/ext_console.c`)

### `GET /api/storage` → `{"ok":true,"roots":[{"path":"/data","label":"Internal","kind":"internal","free":…,"total":…,"writable":true,"default":true},{"path":"/mnt/usb0","label":"USB 1","kind":"usb",…}]}`.
### `POST /api/storage/default {"root":"/mnt/ext0"}` → `{"ok":true}`.
### `GET /api/library?scan=1`
```json
{"ok":true,"files":[{"path":"/data/homebrew/PPSA04029 Cyberpunk 2077.ffpfsc","name":"…","size":156011528192,"mtime":1760000000,"format":"FFPFSC","root":"/data","titleId":"PPSA04029","gameId":"cyberpunk-2077-ps5","title":"Cyberpunk 2077","cover":"https://…","platform":"PS5","partial":false,"jobId":"j_1a2b3c"}],
 "installed":[{"titleId":"PPSA04029","gameId":"cyberpunk-2077-ps5","title":"Cyberpunk 2077","cover":"https://…","path":"/user/app/PPSA04029"}],"scannedAt":1760313600}
```
### `POST /api/library/delete {"path":"…"}` → `{"ok":true}` (only paths from the last scan).
### `POST /api/library/install {"path":"/data/xeno-store/pkg/x.pkg"}` → `{"ok":true,"job":{…synthetic installing job…}}`.
### `POST /api/shadowmount/scan` → `{"ok":true,"available":true,"status":200}` (`POST http://127.0.0.1:10101/api/v1/scan`).
### `POST /api/library/launch {"titleId":"PPSA04029"}` → `{"ok":true,"titleId":"PPSA04029","launched":true,"simulated":false,"at":1760313600}` (`sceLncUtilLaunchApp` via dlsym; `not_installed` 404).
### `POST /api/library/uninstall {"titleId":"PPSA04029"}` — first call answers `428 {"error":"confirm_required","confirmRequired":true,"token":"…","expiresIn":60,"title":"…"}`; repeat with `{"titleId":"…","token":"…"}` → `{"ok":true,"uninstalled":true,"titleId":"…","message":"…"}` (`sceAppInstUtilAppUnInstall` via dlsym).
### `GET /api/library/usage` (Disk analyzer)
```json
{"ok":true,"roots":[{"path":"/data","label":"Internal","kind":"internal","free":…,"total":…,"used":…,"default":true,
 "categories":{"games":…,"dumps":…,"packages":…,"files":…,"saves":…,"partials":…},"xenoTotal":…}],"scannedAt":1760313600}
```
### `POST /api/library/usage/clean {"root":"/data"}` → `{"ok":true,"removed":[…],"skipped":[…],"removedCount":3,"freed":5368709120}` (orphan `.xpart` files and sidecars only).

---

## 7. Settings, favorites, collections, recent (`src/api.c`, `src/settings.c`)

### `GET /api/settings` → `{"ok":true,"settings":{…SPEC §9 object + Phase 2 fields…}}`
Phase 2 keys: `general.{largeText,highContrast,safeMode,screensaver,screensaverMinutes,controllerSwapConfirm,mdns}`,
`downloads.{preferFormat,turboStart,autoInstallSets,installOrder,scheduleStart,scheduleEnd,restModeAutoResume,extractZip,deleteArchiveAfterExtract}`,
`debrid.{realdebrid:{apiKey},alldebrid:{apiKey},torbox:{apiKey},premiumize:{apiKey},preferred}` (keys are returned masked),
`catalog.{featuredUrl,messagesUrl,companionsUrl,savesBackendUrl}`,
`saves.{backupRoot,autoBackup,indexUrls[],backend:{kind,url,anonKey,bucket,garlicFunction},uploadHost,uploadEndpoint,submitEndpoint,nickname}`,
`browse.presets[{name,filters,sort}]`.
### `PUT /api/settings {"downloads":{"connections":16}}` → deep merge, ranges clamped, atomic save → `{"ok":true,"settings":{…}}`.
### `GET /api/favorites` → `{"ok":true,"ids":[…]}` · `PUT /api/favorites {"ids":[…]}` · `POST /api/favorites/toggle {"id":"…"}` → `{"ok":true,"favorite":true}`.
### `GET /api/collections` → `{"ok":true,"collections":[{"id":"wishlist","name":"Wishlist","icon":"star","ids":[]}]}` · `PUT /api/collections {"collections":[…]}`.
### `GET /api/recent` → `{"ok":true,"ids":[…]}` · `POST /api/recent {"id":"…"}` (keeps 30).
### `GET /api/export` → one JSON with settings, favorites, collections, recent · `POST /api/import {…}` → `{"ok":true,"imported":true}`.
### `GET /api/pair` → `{"ok":true,"url":"http://192.168.1.20:9095/","ip":"192.168.1.20","port":9095,"pinRequired":false,"code":"418203","cooldown":0}`
The UI renders the QR and shows the `xeno.local` address from `/api/mdns`. `code` is the live 6-digit pairing code **only when the caller is the console's own browser (loopback)** — a LAN device gets `"code":""` and must read it off the PS5 toast. `cooldown` is the seconds left before `/api/pair/show` may be called again. `pinRequired` reflects `settings.pairing.pin`.
### `POST /api/pair/show` → `{"ok":true,"shown":true,"retryAfter":30}` — re-shows the current code as a console toast. Within the 30 s cooldown it answers `429 {"error":"cooldown","retryAfter":<s>}`; `-1` from the notifier answers `500`.
### `POST /api/pair {"code":"418203"}` → `{"ok":true,"token":"<64-hex>","paired":true}` — redeems the code for a bearer token (constant-time compare; up to 16 tokens kept, persisted to `<data>/pairing.json`). Send the token as `Authorization: Bearer <token>` on later mutating requests. Errors: `429 locked_out` (too many wrong tries, `retryAfter` set), `403 bad_code`, `409 slots_full`, `503 save_failed`.
### `GET /api/session` (loopback only) → `{"ok":true,"address":"192.168.1.20","port":9095,"token":"<64-hex>","pairUrl":"http://192.168.1.20:9095/","code":"418203"}` — the console browser's own trusted token (created on first call). `403 forbidden` for any non-loopback caller.
### `GET /api/mdns` → `{"ok":true,"mdns":{"enabled":true,"running":true,"names":["xeno.local","xenostore.local"],"ip":"…","port":9095,"url":"http://xeno.local:9095","error":""}}`. `POST /api/mdns/build` exists in the host build only (packet builder test hook).

---

## 8. Setup, uninstall, ghost icons, companions (`src/ext_console.c`, §17.6 / §17.16)

### `GET|POST /api/setup/check`
```json
{"ok":true,"checks":[
 {"id":"shadowmount","ok":false,"optional":false,"detail":"Nothing answers on 127.0.0.1:10101","fix":"Start ShadowMountPlus …","action":"launch","actionId":"shadowmount-plus"},
 {"id":"dpi","ok":true,"optional":false,"detail":"Package installer daemon answers on 127.0.0.1:9040","fix":""},
 {"id":"etahen","ok":false,"optional":true,"detail":"No etaHEN DPI on 127.0.0.1:12800","fix":"…"},
 {"id":"internet","ok":true,"optional":false,"detail":"raw.githubusercontent.com answered in 184 ms","fix":""},
 {"id":"drive","ok":true,"optional":false,"detail":"Internal (/data) is writable","fix":""},
 {"id":"space","ok":true,"optional":false,"detail":"384 GB free on Internal","fix":""},
 {"id":"firmware","ok":true,"optional":false,"detail":"Firmware 11.00 — FPKG installs work with kstuff / etaHEN","fix":""},
 {"id":"payload-manager","ok":false,"optional":true,"detail":"…","fix":"Add the source URL below …"}],
 "payloadsUrl":"https://raw.githubusercontent.com/XENOKING123/XENO-STORE/main/updates/payloads.json","fw":"11.00",
 "summary":{"checked":true,"total":8,"failing":2,"at":1760313600}}
```
`action` is `launch` or `install` with `actionId` = a companion id.

### `POST /api/uninstall {"data":true,"homebrew":true,"payloadManager":true,"downloads":false}`
→ `{"ok":true,"removed":["/data/xeno-store","/data/homebrew/XENO-STORE"],"kept":["/data/homebrew"],"failed":[],"removedCount":2,"failedCount":0,"stopping":true}` then the server exits. At least one flag must be true.

### `GET /api/homebrew-dirs` → `{"ok":true,"dirs":[{"name":"XENO-STORE","path":"/data/homebrew/XENO-STORE","title":"XENO STORE","size":5478000,"hasEboot":true,"hasIcon":true,"isXenoStore":true,"mtime":…}],"root":"/data/homebrew","note":"…"}`.
### `POST /api/homebrew-dirs/delete {"name":"OldApp","confirm":"OldApp"}` → `{"ok":true,"deleted":"OldApp"}` (name must be repeated; `confirm_required` 428 otherwise).

### `GET /api/payloads`
```json
{"ok":true,"payloads":[{"id":"shadowmount-plus","name":"ShadowMountPlus","version":"1.03","kind":"shadowmount","url":"https://…","sha256":"…","notes":"…","homepage":"…","size":0,
   "installed":true,"localPath":"/data/xeno-store/payloads/shadowmount-plus.elf","localSize":412000,"installedVersion":"1.03","installedAt":1760300000,"updateAvailable":false}],
 "loaderPort":9021,"loaderAvailable":true,"feedFetchedAt":1760313600,"note":"Payloads are downloaded to the console (sha256 checked) and sent to the local ELF loader on :9021. XENO STORE never loads code in-process."}
```
### `POST /api/payloads/{id}/install` → `{"ok":true,"job":{"id":"cx_…","type":"payload","title":"ShadowMountPlus v1.03","state":"running","done":0,"total":412000,"percent":0,…}}` (download + sha256; a placeholder hash fails with `download_failed`).
### `POST /api/payloads/{id}/launch` → `{"ok":true,"id":"…","bytes":412000,"port":9021,"sent":true}`. Errors: `not_installed` 404, `loader_unavailable` 503.

---

## 9. Saves hub (`src/ext_console.c`, `src/cx_saves.c`, §17.17)

### `GET /api/saves/users` → `{"ok":true,"users":[{"id":268435456,"name":"XENO","foreground":true}]}`.
### `GET /api/saves?user=268435456` → `{"ok":true,"user":268435456,"saves":[{"titleId":"PPSA04029","title":"Cyberpunk 2077","cover":"…","gameId":"…","path":"/user/home/10000000/savedata_prospero/PPSA04029","size":12400000,"mtime":…,"kind":"ps5","dirs":["SAVEDATA00"],"installed":true}],"count":1}`.
### `POST /api/saves/backup {"user":268435456,"titleId":"PPSA04029","kind":"ps5","wait":false}` → `{"ok":true,"job":{"id":"cx_…","type":"backup",…,"result":{"zipPath":"/data/xeno-store/saves/XENO/PPSA04029_ps5_20261013-1200.zip","files":12,"bytes":12400000}}}`. `{"user":…,"all":true}` starts one job per title → `{"ok":true,"jobs":[…],"started":n}`.
### `GET /api/saves/backups` → `{"ok":true,"backups":[{"zipPath":"…","titleId":"…","kind":"ps5","size":…,"createdAt":…,"root":"/data","user":"XENO"}]}`.
### `POST /api/saves/restore {"user":…,"zipPath":"…","overwrite":true}` → `{"ok":true,"titleId":"…","files":12,"bytes":…,"preRestore":"…/PPSA04029_ps5_….pre-restore.zip","tier":"raw"}`.
### `POST /api/saves/transfer {"fromUser":…,"toUser":…,"titleId":"…","kind":"ps5","move":false}` → `{"ok":true,"moved":false,"files":…,"bytes":…,"preBackup":"…","note":"Saves are sealed to this console …"}`.
### `GET /api/saves/jobs` → `{"ok":true,"jobs":[…]}` · `GET /api/saves/jobs/{id}` → `{"ok":true,"job":{…}}` · `POST /api/saves/jobs/{id}/cancel`.
Console job object: `{"id","type":"backup|payload|save-install|save-download|export|upload","title","state":"queued|running|done|failed|cancelled","message","error","errorCode","done","total","percent","createdAt","startedAt","finishedAt","result":{…}}`.
### `GET /api/saves/engine` → `{"ok":true,"tier":"native","detail":"libSceSaveData loaded; mount test ok","native":{"available":true},"garlic":{"available":false,"port":8082},"raw":{"available":true},"simulated":false}` (tier ∈ `native` | `garlic` | `raw`).

---

## 10. Save Data Store (`src/ext_console.c`, `src/cx_savestore.c`, §17.22)

### `GET /api/saves/store?titleId=PPSA04029&category=starter&q=&refresh=0`
```json
{"ok":true,"backend":"github","engine":"native","cached":true,"updatedAt":1760313600,"count":1,
 "sources":[{"kind":"github","url":"https://raw.githubusercontent.com/…/saves.json","rows":40,"ok":true,"cached":true}],
 "saves":[{"id":"sv_1","titleId":"PPSA04029","gameId":"cyberpunk-2077-ps5","title":"Cyberpunk 2077","category":"starter","name":"Chapter 4 — all weapons","description":"…","author":"nick",
   "gameVersion":"01.004.000","region":"EUR","dirName":"SAVEDATA00","size":1234567,"sha256":"…","url":"https://pixeldrain.com/api/file/…","mirror":"","format":"xeno-save-zip","createdAt":"…","downloads":12,"status":"published","tags":["ng+"],"screenshot":"","source":"github","cover":"/api/saves/store/sv_1/cover","needsResign":true,
   "installed":true,"installedVersion":"01.004.000","otherRegion":false,"installedTitleId":"PPSA04029","versionCompare":0,"compat":"ok","compatText":"Matches your 01.004.000"}]}
```
`compat` ∈ `ok` | `older` | `newer` | `not_installed` | `other_region`. Supabase rows add `storagePath` / `garlicId`.
### `POST /api/saves/store/{id}/install {"user":…,"backupFirst":true,"wait":false}` → `{"ok":true,"job":{"type":"save-install",…}}` (download → sha256 → engine import).
### `POST /api/saves/store/{id}/download {"user":…}` → `{"ok":true,"job":{"type":"save-download",…}}` (ZIP only, to `<root>/xeno-store/saves/community/`).
### `GET /api/saves/store/{id}/cover` → `302` to the cover (signed Supabase URL or catalog art).
### `POST /api/saves/export {"user":…,"titleId":"PPSA04029","dirName":"SAVEDATA00"}` → `{"ok":true,"zipPath":"…","size":…,"sha256":"…","tier":"native","accountIdStripped":true,"manifest":{…xeno-save.json…},"shareable":true}`.
### `POST /api/saves/upload {"zipPath":"…","host":"pixeldrain"}` → `{"ok":true,"url":"https://pixeldrain.com/api/file/abc","sha256":"…","size":…,"host":"pixeldrain"}`.
### `POST /api/saves/submit {"titleId":"…","name":"…","category":"starter","description":"…","tags":[…],"nickname":"…","url":"…","sha256":"…","size":…,"gameVersion":"…","region":"…"}` → `{"ok":true,"method":"github-issue","issueUrl":"https://github.com/XENOKING123/XENO-STORE/issues/new?template=save-submission.md&title=…&body=…","qr":"<svg…>","submission":{…},"note":"…"}` (or `method:"custom"` / `"supabase"`).
### `GET /api/saves/installed-version?titleId=PPSA04029` → `{"ok":true,"titleId":"PPSA04029","installed":true,"version":"01.004.000","source":"appmeta"}`.

---

## 11. Accounts (`src/ext_console.c`, `src/cx_accounts.c`, §17.18)

### `GET /api/accounts`
```json
{"ok":true,"supported":true,"simulated":false,"foregroundUser":268435456,
 "users":[{"id":268435456,"name":"XENO","foreground":true}],
 "slots":[{"slot":1,"name":"XENO","id":"0x1234abcd5678ef90","type":1,"flags":0,"empty":false,"activated":false,"foreground":true,"derivedId":"0x…"}]}
```
### `POST /api/accounts/{slot}/activate {"id":"0x1234abcd5678ef90"}` (id optional; derived from the name when omitted) → `{"ok":true,"slot":1,"id":"0x…","activated":true,"rebootRequired":true,"note":"Reboot the console for the activation to take effect. Back up your saves first."}`. Errors: `registry_unavailable` 503, `activate_failed` 422.
### `POST /api/accounts/{slot}/clear` → `{"ok":true,"slot":1,"activated":false,"rebootRequired":true}`.
### `POST /api/accounts/reboot` → `{"ok":true,"rebooting":true,"simulated":false}` (saves the queue, `sceSystemServiceRequestReboot`).

---

## 12. Homebrew store (`src/ext_console.c`, `src/cx_homebrew.c`, §17.21)

### `GET /api/homebrew?refresh=0&q=retro&category=Emulator`
```json
{"ok":true,"cached":true,"live":false,"fetchedAt":1760313600,"source":"pkg-zone","count":1,"pkgZoneItems":220,"categories":["Emulator","Tool","Game","Media"],
 "items":[{"id":"RetroArch_1_19","title":"RetroArch","version":"1.19","author":"Libretro","supportsPs5":true,"cover":"/api/image?url=https%3A%2F%2Fpkg-zone.com%2Fimages%2FRetroArch_1_19%2Fcover.png",
   "details":"https://pkg-zone.com/details/RetroArch_1_19","downloadPs5":"https://pkg-zone.com/download/ps5/RetroArch_1_19/latest","downloadPs4":"https://pkg-zone.com/download/ps4/RetroArch_1_19/latest",
   "category":"Emulator","installed":{"titleId":"","version":"","installedAt":0}}]}
```
Live listing cached 7 days at `/data/xeno-store/homebrew.json`; falls back to the catalog's `HOMEBREW` entries.
### `POST /api/homebrew/{id}/installed {"titleId":"…","version":"1.19"}` → `{"ok":true,"id":"…"}` (records the installed version for "Update available").
### `GET /api/image?url=https://pkg-zone.com/images/<id>/cover.png` → the image bytes, allow-listed hosts only (404/403 otherwise).

---

## 13. `/api/v1` — the TV app dialect (`src/tvapi*.c`)

The native TV app (`PPSA99500`, ported from Orbit Store's TV app, GPL-3.0-or-later) talks to the store on
`127.0.0.1:9095` through a separate compatibility layer under `/api/v1`. It speaks the reference Orbit Store
backend's dialect, not the `/api` envelope above. The web UI and phones never use it.

### 13.1 Dialect

- Success bodies are the bare object or **bare array** the app expects (`/catalog`, `/favorites`, `/downloads` and
  `/storage` are arrays), with no `ok` key. Statuses: 200, 201 (`POST /downloads`), 202 (accepted background work).
- Errors are `{"error":"<sentence>"}`. The app shows the sentence and translates exact matches (its catalogues are
  keyed by the English text, `app/tv/i18n/`), so it is always a human sentence, never a code, and a backend
  sentence changes together with its key in `app/tv/tools/i18n-external-keys.txt` and the 11 catalogues.
  The dispatcher's own refusals use the same envelope under `/api/v1`: unknown path `404` and wrong method `405`
  "This request is not available in this version of XENO STORE.", invalid JSON `400` "Expected a JSON object.",
  a token the PIN gate refused `401` "Pair this device again from Settings.", out of memory `503`
  "XENO STORE is low on memory. Try again." and any other failure `500` "XENO STORE could not complete this request.".
- JSON types are strict (the app does not coerce): booleans are `true`/`false`, numbers are numbers, strings are
  strings. Timestamps are Unix seconds, sizes bytes.
- Every reply carries `Content-Length`, `Connection: close` and `Cache-Control: no-store`; nothing is gzipped. The
  app sends one request per connection.

### 13.2 Gate

Every `/api/v1` route runs the same checks, in this order, before its handler:

| Step | When | Reply |
|:--|:--|:--|
| Stopping (`S`) | the store is stopping (signal, web UI stop/restart, or the TV app's stop drain, §13.9) | `503` "XENO STORE is stopping. It will be back in a moment." |
| Bearer (`B`) | no `Authorization: Bearer <64 hex>` with a paired token | `401` "Pair this device with your PS5 to continue." |
| Body (`O`, POST only) | `Content-Type` present but not `application/json` | `415` "Use application/json." |
| | the body is not a JSON object (empty, `[]`, `"x"`, `1`, `null`) | `400` "Expected a JSON object." |

`GET /system` is never refused (it answers `stopping:true`); `GET /updates` is readable while stopping (Bearer still
required). With a pairing PIN set, the core PIN gate lets the app's Bearer requests through; without a PIN the
`/api/v1` gate alone protects the mutating routes. The core request gate still applies first: a foreign `Origin`
and a non-local `Host` are refused (403) before any route runs.

### 13.3 Routes

31 method + path entries, one table in `src/tvapi.c` (`TV_ROUTES`):

| Method | Path | Gate | Success | Module |
|:--|:--|:--|:--|:--|
| GET | `/system` | — | 200 object | `tvapi_system.c` |
| GET | `/session` | S, loopback only | 200 object | `tvapi_system.c` |
| GET, POST | `/zero/companion` | — | always `404` "Orbit Zero is not part of XENO STORE." | `tvapi_system.c` |
| GET | `/sources` | S | 200 object | `tvapi_catalog.c` |
| POST | `/sources` | S B O | 200 object (as GET) | `tvapi_catalog.c` |
| GET | `/catalog` | S | 200 array | `tvapi_catalog.c` |
| GET | `/catalog/updates` | S | 200 object | `tvapi_catalog.c` |
| POST | `/catalog/updates` | S B O | 202 object | `tvapi_catalog.c` |
| GET | `/art/{gameId}/{cover\|hero}` | S B | 200 image / 202 | `tvapi_art.c` |
| GET | `/favorites` | S B | 200 array of game ids | `tvapi_catalog.c` |
| POST | `/favorites` | S B O | 200 array | `tvapi_catalog.c` |
| GET | `/downloads` | S B | 200 array of jobs | `tvapi_downloads.c` |
| POST | `/downloads` | S B O | 201 `{"id"}` | `tvapi_downloads.c` |
| POST | `/downloads/{id}/{action}` | S B O | 200 array of jobs | `tvapi_downloads.c` |
| GET | `/storage` | S B | 200 array of drives | `tvapi_downloads.c` |
| POST | `/storage/preferred` | S B O | 200 `{"preferredStorage"}` | `tvapi_downloads.c` |
| POST | `/settings/downloads` | S B O | 200 `{"concurrentDownloads"}` | `tvapi_downloads.c` |
| POST | `/settings/extraction` | S B O | 200 `{"deleteArchives"}` | `tvapi_downloads.c` |
| GET | `/debrid` | S B | 200 object | `tvapi_downloads.c` |
| GET, POST | `/browser` | S B (O) | GET 200 idle; POST 409 | `tvapi_downloads.c` |
| GET, POST | `/library` | S B (O) | 200 snapshot | `tvapi_library.c` |
| GET, POST | `/library/storage` | S B (O) | 200 object | `tvapi_library.c` |
| POST | `/library/actions` | S B O | 202 snapshot | `tvapi_library.c` |
| GET | `/updates` | B (not S) | 200 status | `tvapi_update.c` |
| POST | `/updates` | S B O | 202 status | `tvapi_update.c` |
| GET, POST | `/tv-app` | S B (O) | 200 / 202 status | `tvapi_update.c` |

Captures are at most 95 bytes. Nothing of the layer runs for web-only users: its threads (`tv-storage`, `tv-art`
×2, `tvlib`, `tv-catalog`, `tv-update`) start on the first `/api/v1` request. `GET /api/diagnostics` reports the
layer under `"tvapi"` (`started`, `threads`, `routes`, `clientSeenAgoMs`, `stopping`, `preferredAuto`, the drive
snapshot) and the queue's health under `"queue":{"stateHealthy","workersRunning"}`.

### 13.4 Session and system

- `GET /api/v1/session` answers loopback callers only (the TV app on the console):
  `{"token":"<64 hex>","pairCode":"418203","address":"192.168.1.20"}`. The token is the same `tokens[0]` the
  console browser gets from `GET /api/session` (persisted in `<data>/pairing.json`); `address` is `""` when the
  console has no LAN address (never `127.0.0.1`). Other callers get `403` "Pair this device using the code shown
  on your PS5."; a failed save of the pairing state gives `503` "Could not save pairing.".
- A web page in the PS5 browser cannot read the token: its request carries a foreign `Origin`, which the request
  gate refuses (403) before any route runs, and DNS rebinding is refused by the Host check. Any native process on
  the console can obtain it, the same trust boundary as `/api/session`. Forgetting paired devices in the web UI
  revokes the token; the app then gets `401`, drops it and asks `/session` again.
- `GET /api/v1/system` is answered from memory in well under 1 ms (no network, drive or catalogue walk; the app
  polls it every 0.5–3 s):
  `{"name":"XENO STORE","version":"1.2.1","instanceId":"<32 hex, new every start>","stopping":false,"platform":"ps5",
  "paired":true,"localSessionAvailable":true,"stateHealthy":true,"catalogueRevision":10485760123,
  "preferredStorage":"usb0","deleteArchives":true,"concurrentDownloads":1,"httpPort":9095,"safeMode":false}`.
  `paired` is true when the request carried a valid Bearer, `localSessionAvailable` when the caller is loopback,
  `stateHealthy` false after `queue.json` could not be written (the app shows a banner). `platform` is `"ps5"`
  or `"host"`. `catalogueRevision` is `(catalog revision << 20) | (hash of the featured hero ids & 0xFFFFF)`, so a
  feed change also makes the app reload the catalogue. `preferredStorage` is `""` while "Choose automatically" is on.
- The first `/api/v1` request also tells the store that the TV app is in front: when it arrives within 2 s of
  start-up, the store does not open the PS5 browser or show its "ready" notification (the TV app is the screen).

### 13.5 Catalogue, sources and favourites

- `GET /sources` → `{"enabled":["archive","vikingfile"],"options":[{"id":"archive","label":"Archive.org",
  "releaseCount":3560,"downloadReady":true},{"id":"vikingfile","label":"Vikingfile","releaseCount":732,
  "downloadReady":false}],"acknowledged":true,"noticeVersion":1}`. Options are the catalogue sources the TV app can
  show with at least one TV-visible release; `downloadReady` is false for browser-only sources. It never fails (a
  failure would end the app's whole poll).
- `POST /sources {"enabled":["archive"],"acknowledged":true,"noticeVersion":1}` saves the choice and the download
  notice (shared with the web wizard: accepting it in either UI accepts it in both) and answers like GET. Refusals:
  `400` "Select the sources you want to enable.", "Unknown or duplicate source.", "Read and acknowledge the current
  download notice before saving sources."; `503` "Could not save source settings. Check console storage.".
- `GET /catalog` → a bare array of **options** (one per release), each repeating its game's fields, `[]` until the
  notice is acknowledged, filtered to the enabled sources, in catalogue game order:
  `{"id":"<releaseId>","gameId":"…","titleId":"CUSA04622","platform":"ps4","title":"…","genre":"Action / RPG",
  "tagline":"","description":"…","publisher":"…","releaseDate":"2016-03-22","addedAt":"2025-03-30T00:00:00Z",
  "artworkLayout":"wide","provider":"Archive.org","sourceId":"archive","format":"PKG","region":"USA",
  "version":"1.03","filename":"….pkg","notes":"","sizeBytes":4052484096,"browserAvailable":false,
  "directAvailable":true}` (plus `newOnOrbitRank` 1–100 for the featured games, from the featured feed's hero order).
  Scope (tier A): PS4/PS5 games only, direct and browser sources, formats PKG, FPKG, FFPKG, exFAT, FFPFSC and FFPFS,
  a known size. ZIP options are hidden (compile switch `TV_SHOW_ZIP`, host `XENO_TV_SHOW_ZIP=1`); RAR and 7z always.
  Options within a game are ordered by kind (game, backport, update, DLC), direct before browser, healthy before
  flaky, verified first, then catalogue order. URLs, mirrors, hashes, passwords and art URLs are never sent. The
  list is rendered from an immutable snapshot rebuilt when the catalogue or the featured feed changes.
- `GET /catalog/updates` → `{"available":true,"busy":false,"cached":true,"revision":10,"gameCount":2593,
  "checkedAt":…,"checkAfter":…,"error":""}`. `POST /catalog/updates {"action":"refresh"}` → `202` and the refresh
  runs on the `tv-catalog` thread (the web UI's refresh and housekeeping never overlap it). Refusals: `400`
  "Expected refresh action."; `409` "Catalogue refresh needs the console's network. Try again shortly." and "The
  catalogue is already refreshing."; `429` "Please wait before refreshing the catalogue again." (60 s cooldown).
- `GET /favorites` → `["gameId",…]` (the web UI's favourites). `POST /favorites {"gameId":"…","favorite":true}`
  **sets** the state and answers the full array. Refusals: `400` "Expected a game and favourite choice."; `404`
  "Game is not in this catalogue." (adding only); `409` "Favourites are full."; `503` "Could not save favourites.".

### 13.6 Art

`GET /api/v1/art/{gameId}/{cover|hero}` serves game artwork asynchronously. The app decodes the pictures itself
(WebP scaled while decoding; PNG and JPEG fully decoded into its fixed 128 MiB heap first), so the store passes
the original bytes through and polices what it serves:

- **Candidates** come only from our own catalogue entry for that game (any catalogue game, not only TV-visible
  ones): cover = `cover` unless it is an archive.org `services/img` thumbnail or shared with another game, else
  `coverFallback`; hero = `hero` (when it differs from the cover), then `heroFallback`. Nothing usable → `404` at
  once (the app draws its title placeholder or the cover wash).
- **URL policy:** `https://` only (host build: also `http://$XENO_HOST_ART_FIXTURE`), no userinfo, no loopback or
  private literals, no `.avif` or `.gif`; `image.api.playstation.com` gets `w=512` (cover) or `w=1920` (hero). The
  client never supplies a URL, so there is no host allow-list.
- **Fetch:** 2 `tv-art` workers, 10 s per connect/receive and 30 s per transfer, at most 12 MiB, 3 redirects (each
  hop re-checked), `Accept: image/webp,image/png,image/jpeg;q=0.9`. The server's `Content-Type` is never trusted:
  the body is sniffed.
- **Accepted:** WebP (still; animated refused), sides ≤ 8192; PNG and JPEG at most 1,048,576 px for a cover and
  3,686,400 px (2560×1440) for a hero; JPEG only baseline/extended/progressive, 8-bit, 1 or 3 components. A hero
  must be at least 1.4× as wide as it is tall. A refused picture makes the worker try the next candidate; refusals
  are remembered per URL until the catalogue changes.
- **Cache:** key `gameId#kind`, 256 entries / 24 MiB of pictures, least-recently-used eviction, a 64-key queue
  served newest first; cleared when the catalogue revision changes.
- **Replies:** `202 {"pending":true}` while queued or fetching (the app asks again every 0.6 s); `200` with the
  bytes and `Content-Type: image/webp|png|jpeg`; `404` "Unknown artwork." (bad id or kind, no game, no usable
  candidate); `502` "This artwork could not be loaded." for 120 s after a failed fetch; `503` "Artwork is busy. Try
  again shortly." when the queue is full; `401` when unpaired.

### 13.7 Downloads, drives and download settings

The TV app's queue is the store's own queue (`dl.c`): a download started on the TV shows in the web UI and the
other way round. Library installs and jobs being removed are not listed.

- `GET /downloads` → `[{"id":"j_ab12cd","releaseId":"…","gameId":"…","titleId":"PPSA01234","title":"…",
  "format":"FFPKG","filename":"….ffpkg","storageId":"usb0","path":"/mnt/usb0/homebrew/….ffpkg",
  "status":"downloading","error":"","verification":"sha256","delivery":"direct","deliveryPhase":"",
  "canRefreshLink":false,"canEnterArchivePassword":false,"received":…,"total":…,"speed":…,"retryAt":0,
  "extracted":0,"extractTotal":0,"order":1}]`. `status` is one of `queued`, `downloading`, `verifying`,
  `extracting`, `paused`, `retrying` (a retry wait, with `retryAt`), `error`, `cancelled` and `complete` (an
  installed package is `complete`). TorBox link preparation is `queued` with `deliveryPhase` "Preparing with TorBox".
  `titleId`, `format`, `filename` and `total` repeat the catalogue option so the app can match the job to it.
- `POST /downloads {"releaseId":"…","storageId":"usb0","delivery":"direct"|"torbox"}` → `201 {"id":"j_ab12cd"}`
  and the drive becomes the default. Checks, in order: delivery (`400` "Choose direct or torbox delivery."), the
  release (`404` "Unknown curated release."), source enabled and acknowledged (`409` "Enable this source and
  acknowledge the download notice in Sources first."), a browser-only source sent direct (`409`, the Vikingfile
  sentence of `/browser`), TorBox (`409` "Connect TorBox in App settings first." and the other TorBox sentences),
  the drive (`409` "Selected storage is not connected or writable."), a Library copy in progress (`409` "Wait for the
  Library copy to finish before downloading."), duplicates (`409` "This release already has a job on that drive.
  Open Downloads to resume or retry it.", "Another download uses this filename on that drive. Open Downloads to
  manage it.", "A file with this name is already on that drive. XENO STORE will not overwrite it."), queue room
  (`409` "Queue is full. Clear finished or cancelled history to make room."), space (`409` "Not enough space after
  unfinished downloads. Free space or cancel a queued download."), and `503` "Downloads are unavailable on this
  console's firmware.".
- `POST /downloads/{id}/{action}` with `pause`, `resume`, `retry` (a failed extraction retries the extraction),
  `cancel` (`{"deletePartial":true}` removes the partial file), `delete` (cancelled jobs only), `forget`, `remove`
  (finished jobs; the file stays), `archive-password` (`{"password":"…"}`, at most 128 bytes, no control characters)
  and `refresh-link` (always `409` "This download has no link to refresh."). Every success answers the full jobs
  array. Unknown id `404` "Unknown download.", unknown verb `400` "Unknown download action."; the per-state refusals
  are the reference sentences ("This job is not running.", "This job cannot be resumed in its current state.",
  "Reconnect the original destination drive.", "Only cancelled downloads can be deleted.", …).
- `GET /storage` → `[{"id":"usb0","label":"USB storage 1","path":"/mnt/usb0/homebrew","freeBytes":…,
  "totalBytes":…,"pendingBytes":…,"projectedFreeBytes":…,"external":true}]`: the writable drives from the
  `tv-storage` snapshot (re-read every 2 s, rescanned when the mount set changes). Drive ids are `usb0`…`usb7`,
  `ext0`, `ext1` and `internal` (host build: `internal` for the downloads folder and `root<i>` for `--root`
  folders), labelled "USB storage N", "M.2 SSD", "Extended storage N" and "Internal storage".
  `projectedFreeBytes` = free − bytes still to download − 240 MiB.
- `POST /storage/preferred {"storageId":"usb0"}` → `{"preferredStorage":"usb0"}`; `""` means "Choose
  automatically" (the first external drive, else internal), kept in `<data>/tvapi.json` `{"preferredAuto":true}`.
  Refusals: `400` "Expected storageId.", `409` "Selected storage is not connected or writable.", `503` "Could not
  save the default drive.".
- `POST /settings/downloads {"concurrentDownloads":2}` → `{"concurrentDownloads":2}` (1–4, else `400` "Choose a
  whole number from 1 to 4."). `POST /settings/extraction {"deleteArchives":true}` → `{"deleteArchives":true}`
  (`400` "Expected deleteArchives."). Both write the web UI's settings; `503` "Could not save the setting.".
- `GET /debrid` → `{"provider":"torbox","available":true,"connected":false,"busy":false,"hosts":[{"sourceId":
  "archive","available":false,"maxFileSize":0},…]}` (TorBox is connected in the web UI's Settings → Debrid).
- `GET /browser` → `{"available":false,"active":false,"id":"","state":"idle","title":"","jobId":"","message":
  "Vikingfile asks for a human check, so XENO STORE cannot fetch it by itself. Download it with TorBox, or open
  this game in the browser version."}`. `POST /browser {"action":"start"}` answers `409` with that message,
  `{"action":"cancel"}` `409` "This verification session is no longer active.", anything else `400` "Unknown browser
  action.". The route is never `404`.

### 13.8 Library

Rows come from the store's own drive scan, cached by the `tvlib` worker with change detection; no request walks a
drive.

- `GET|POST /library` → `{"status":"ready","message":"","provider":"XENO STORE","providerVersion":"1.2.1",
  "busy":false,"stale":false,"checkedAt":…,"updatedAt":…,"refreshAfter":5,"games":[…],"capabilities":["rescan",
  "copy_game_source","move_game_source","storage_job_status","storage_job_cancel","storage_space"],"action":null,
  "storageJob":null,"storageError":"","storageBusy":false}`. Each game row carries its title id (from a finished
  download, the catalogue file name, `PPSA`/`CUSA` in the name or path, or an exact title match; otherwise the file
  is skipped), `sourceKey` = sha256 of `titleId\npath\nformat`, the drive's `location` label, `installed`
  (`/user/app/<id>` exists), `onDrive`, and `managed` / `canManageSource` (image formats; PKG and FPKG cannot be
  copied or moved). `status` is `idle` until the first scan, then `ready` (or `error`, with `message` "Library is
  unavailable. Restart XENO STORE to try again." when the worker could not start). Our own `PPSA99500` is hidden. Mount, unmount and measure
  are not offered, so the app hides those buttons. A GET schedules a re-check at most every 30 s, a POST every 5 s;
  the snapshot is `stale` after 90 s without a successful scan.
- `GET|POST /library/storage` → `{"drives":[{"id":"<sha256 of drive\npath>","path","label","mountPoint",
  "filesystem","readOnly","totalBytes","freeBytes","usedBytes"}],"destinations":[{"id":"<sha256 of
  dest\npath/homebrew>",…}],"busy":false,"stale":false,"updatedAt":…,"error":""}`: every mounted drive, read-only
  ones too. Problems are reported inside the 200 (`error`, `stale`, "No drives reported."), never as an HTTP error
  other than 401.
- `POST /library/actions {"action":"scan"|"copy"|"move"|"cancel","requestId":"<8-64 chars>","titleId",
  "sourceKey","destinationId","jobId","confirmed":true}` (`titleId` + `sourceKey` for copy and move, plus
  `destinationId`; `jobId` for cancel; `confirmed:true` for scan and move) → `202` with the full snapshot. Copy and
  move run as the store's own copy jobs, started at once at the top of the queue; `storageJob` reports their
  progress with a numeric `id`. Replaying the same body with the same `requestId` answers `202` again and runs
  nothing; a reused `requestId` with another body answers `409` "This request ID was already used for another
  action.". Refusals: `400` "Expected a supported action and its current Library identifiers." and "Confirm this
  action first."; `409` "Another Library operation is in progress. Wait for it to finish." and "Refresh Library with
  a compatible ShadowMount version before using this action." (no completed scan yet, or mount/unmount/measure);
  `503` "Library is unavailable.". What goes wrong after the `202` is reported in the snapshot's `action` and
  `storageError`: "Pause active and queued downloads before copying or moving games.", "This game's source changed.
  Refresh Library and select it again.", "The selected destination changed or is no longer writable. Refresh
  storage.", "Choose a different destination outside the source folder.", "A file with this name is already on that
  drive. XENO STORE will not overwrite it.", "Not enough free space after unfinished downloads on the destination
  drive.".

### 13.9 Updates, stop and drain

- `GET /updates` → `{"available":true,"runningVersion":"1.2.1","instanceId":"…","savedVersion":"","latestVersion":
  "1.3.0","checksum":"<64 hex>","updateAvailable":true,"automaticCheckAfter":…,"phase":"checked","error":"",
  "busy":false,"restartRequired":false,"received":0,"retryAt":0,"checkedAt":…,"checkAfter":…}`. Phases: `idle`,
  `checking`, `checked`, `downloading`, `saving`, `saved`, `error`, `stopping`. `savedVersion` is the version of the
  copy the updater saved (`/data/xeno-store/xeno-store.elf` + `.version`, checked as an x86-64 ELF), newer or equal.
  On the host build both update routes say `available:false` unless `XENO_TVAPI_UPDATES=1`.
- `POST /updates {"action":"check"[,"automatic":true]}` → `202`, the `tv-update` worker checks
  `updates/payloads.json` (then GitHub). An automatic check (the app sends one per launch) is a no-op `202` inside the
  6 h window, when Settings → "Check for updates automatically" is off, or in safe mode.
- `POST /updates {"action":"install","version":"1.3.0","checksum":"<64 hex>"}` → `202`; the worker downloads the
  ELF, checks SHA-256 and ELF, and saves it as the copy the TV app's starter prefers. Refusals: `400` "Unknown update
  action." / "Automatic mode is only valid for release checks."; `409` "Updates work with XENO STORE running on your
  PS5.", "A XENO STORE update is in progress. Wait for it to finish.", "Check for updates again before installing
  this release.", "This release has no published checksum. Install it from the web version.", "An older release
  will not replace this build."; `429` "Please wait before checking or downloading again." (60 s cooldowns). Worker
  errors land in `error`: "No newer XENO STORE release was found. An older release will not replace this build.",
  "No compatible XENO STORE release found. An older or invalid release will not replace this build.", "The release
  failed SHA-256 or ELF validation. The saved copy was not replaced." (only a SHA-256, size or ELF check failure),
  "Update interrupted. Try again." (a network or save failure, nothing was replaced), and "Check for updates again
  before installing this release." (the release changed since the check).
- **Stop** — `POST /updates {"action":"stop","confirmed":true}` (Settings → Restart download service, and the
  hand-over after an update):
  1. under the queue lock, every queued, retry-waiting and active job is paused ("Paused for the XENO STORE
     restart") and `queue.json` is saved synchronously; a failed save puts the jobs back and answers `503` "Could not
     save the queue. XENO STORE has not been stopped.";
  2. the reply is `202` with the status above (`phase:"stopping"`, `instanceId`, `runningVersion`, `savedVersion`);
     from now on `/system` says `stopping:true` and the gate answers `503` "XENO STORE is stopping. It will be back
     in a moment." (a second stop gets the same `503`);
  3. a detached `tv-drain` thread waits until no job worker runs (at most 10 s), saves the queue again with the final
     byte counts and stops the process; the port closes;
  4. the store **never relaunches itself** after a TV stop: the TV app sees the port close and sends the next copy
     to the ELF loader itself (the saved update when there is one, else its bundled copy; SPEC §19). It then waits
     for a new `instanceId` (and the promised `savedVersion`, when one was promised).
  `400` "Confirm stopping XENO STORE and pausing downloads first." without `confirmed:true`; `409` "A game is still
  installing from XENO STORE. Wait for it to finish, then restart." while a finished download is still being
  installed (stopping then would cut the install short; nothing is paused).
- `GET /tv-app` → `{"available":true,"installed":"orbit","installedVersion":"1.3.0","latestVersion":"1.3.0",
  "checksum":"…","size":…,"updateAvailable":false,"phase":"idle","error":"","busy":false,"received":0,"retryAt":0,
  "checkedAt":…,"checkAfter":…,"automaticCheckAfter":…}`. `installed` is `none`, `orbit` (installed by the store; the
  wire value is kept from the reference dialect), `manual` (a `PPSA99500.ffpkg` placed by hand), `folder` (an
  unpacked `/data/homebrew/PPSA99500/`) or `blocked` (something else at that path), read from
  `/data/homebrew/PPSA99500.ffpkg`, `/data/homebrew/PPSA99500/eboot.bin` and the install record `<data>/tv-app.json`
  `{version, checksum, size, modified, inode}`.
- `POST /tv-app {"action":"check"[,"automatic":true]}` → `202`: the worker reads `updates/tv-app.json` (FORMATS §8)
  and falls back to GitHub `releases/latest` plus its `SHA256SUMS`. `POST /tv-app {"action":"install","version",
  "checksum"[,"replace":true]}` → `202`: one `app-install` job downloads the image from the release URL, checks size,
  SHA-256 and the UFS2 magic `19 01 54 19` at 65536+1372, re-checks what is installed, renames it into place and
  writes the record. It opens on the next launch. Refusals: `400` "Unknown TV app action.", "Replace is only valid,
  as true or false, when installing.", "Automatic mode is only valid for TV app checks."; `409` "The TV app is
  installed from XENO STORE running on your PS5.", "The TV app is already being checked or installed. Wait for it.",
  "Check for the TV app again before installing it.", "The TV app is already installed as the folder
  /data/homebrew/PPSA99500. Remove that folder to install the release, or keep updating it yourself.", "Something
  other than the TV app is at /data/homebrew/PPSA99500.ffpkg. Move it away to install updates here.", "A newer TV
  app is already installed. Nothing was replaced.", "A copy you placed yourself is already in /data/homebrew.
  Confirm replacing it with this release."; `429` "Please wait before checking or downloading again." (30 s, 60 s
  after the release host answered 403/429/503); `507` "Not enough free space on the console's internal storage. The
  TV app needs about N MB." (size + 64 MiB). Check errors: "The TV app release information is invalid. Nothing was
  downloaded.", "The TV app hasn't been published yet. Check again after the next release.".

---

## 14. Errors quick reference

| code | meaning |
|:--|:--|
| `bad_request` | missing/invalid field; message says which |
| `pin_required` | pairing PIN set and header missing/wrong |
| `not_found` / `bad_release` / `no_release` / `not_installed` | unknown id / release / title |
| `exists` | file already queued or present (use `overwrite`) |
| `no_space` / `root_unavailable` / `same_drive` | destination problems |
| `downloads_unavailable` | network libraries did not load on this firmware |
| `provider_page` / `source_changed` / `hash_mismatch` / `drive_missing` | job failure reasons (see GUIDE) |
| `bad_key` / `not_connected` / `unsupported_host` / `limit` / `service_error` | debrid |
| `confirm_required` | repeat the call with the `token` / `confirm` value |
| `loader_unavailable` / `engine_unavailable` / `registry_unavailable` | the console feature is missing on this firmware or setup |
| `forbidden` | a path that was not produced by the store (deletes/transfers accept only recorded paths) |
