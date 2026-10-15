# XENO STORE — Formats and file layouts

Reference for every on-disk and published format the store reads or writes. The authoritative
definitions are in `docs/SPEC.md` (§3, §7.2, §7.5, §9, §10, §12, §17.1, §17.22); this file is the
practical version with examples.

Contents: [Download formats](#1-download-formats) · [Destinations](#2-destination-layout) · [Partial files and sidecars](#3-partial-files-and-sidecars) ·
[Data directory](#4-data-directory) · [Queue file](#5-queuejson) · [Settings](#6-settingsjson) · [Catalog](#7-catalog-schema) ·
[Published feeds](#8-published-feeds-updates) · [Save archives](#9-xeno-save-zip) · [Games-row app](#10-games-row-app-ppsa99500ffpkg) · [Release assets](#11-release-assets)

---

## 1. Download formats

`release.format` values and what the console does with each:

| format | extension(s) | kind of file | after download | needs |
|:--|:--|:--|:--|:--|
| `FFPFSC` | `.ffpfsc` | compressed PS5 game image | saved to `homebrew/`, ShadowMountPlus scan | ShadowMountPlus |
| `exFAT` | `.exfat` | PS5 game image (exFAT container) | same | ShadowMountPlus |
| `FFPKG` | `.ffpkg` | PS5 image (FF package container) | same | ShadowMountPlus |
| `FFPFS` | `.ffpfs` | uncompressed PS5 image | same | ShadowMountPlus |
| `PKG` | `.pkg` | package (PS4 or homebrew; header `7F 43 4E 54` = `\x7FCNT`) | saved to `xeno-store/pkg/`, auto-install | native installer / DPI |
| `FPKG` | `.pkg`, `.fpkg` | fake package (PS5 fake header `\x7FFIH` accepted) | same | firmware ≤ 11.60 + kstuff / etaHEN (Oct 2026) |
| `ZIP` | `.zip` | archive | saved to `xeno-store/files/`, extracted on the console (settings) | — |
| `RAR` / `7z` | `.rar`, `.7z` | archive | saved to `xeno-store/files/`, **not extracted** | XENO TOOL on a PC |
| `PAGE` | — | a web page listing mirrors | `Get links` (server-side resolve on allow-listed hosts) or open in the browser, then `Enter file link` | — |

Format detection for raw links (`Enter file link`): by extension of the final filename, else by the `Content-Type` / `Content-Disposition` of the first response; HTML answers are rejected as `provider_page`.

Package header fields read before an install: magic at offset 0, `content_id` (36 bytes) at offset `0x40`; the title id is `content_id[7..15]` (`UP1234-PPSA04029_00-…` → `PPSA04029`).

---

## 2. Destination layout

Roots (download drives) are discovered at start and on `GET /api/storage`: `/data` (Internal), `/mnt/ext0`, `/mnt/ext1` (M.2), `/mnt/usb0` … `/mnt/usb7` (USB). A root is listed when it is a directory with a working `statvfs`; `writable` is tested by creating and removing `<root>/xeno-store/.wtest`.

```
<root>/
  homebrew/                      image dumps (ShadowMountPlus scans this folder)
    PPSA04029 Cyberpunk 2077.ffpfsc
    <name>/                      folder dumps extracted from ZIP archives
  xeno-store/
    pkg/                         .pkg / .fpkg (installed from here)
    files/                       archives and anything else
    saves/
      <userName>/                save backups  <TITLEID>_<ps5|ps4>_<YYYYMMDD-HHMM>.zip (+ .pre-restore.zip)
      community/<title>/         community saves downloaded as files
    .wtest                       transient writability probe
```

File names are sanitized before they touch the file system: empty names, `..`, `/`, `\`, NUL, control characters and a leading `.` are rejected, names are truncated to 200 bytes, and the root must be one of the known roots (`storage_join(root, category, filename)`). Delete / transfer / install actions accept only paths the store produced itself (job paths or the last library scan).

---

## 3. Partial files and sidecars

While a job runs the data goes to `<final>.xpart` (preallocated with `ftruncate` when the size is known, written with `pwrite` per segment, `fsync` every 256 MiB and on pause/complete). Next to it, `<final>.xpart.json`:

```json
{"v":1,"url":"https://archive.org/download/…","total":156011528192,"etag":"\"3f2a…\"","lastModified":"Wed, 01 Oct 2026 10:00:00 GMT",
 "chunk":16777216,"done":[[0,16777216],[33554432,50331648]],"written":33554432,
 "createdAt":1760000000,"updatedAt":1760000300,"sha256Expected":"","root":"/data","rootId":"dev:ino"}
```

Written every 5 s and on every state change. On resume the server is re-probed: a different `total` or a changed validator (ETag / Last-Modified) fails the job with `source_changed` — the partial is never appended to. Servers without validators resume by size after the last 64 KiB of the partial is compared against a Range fetch of the same bytes. On completion `.xpart` is renamed to the final name, the sidecar is deleted and the directory is `fsync`ed. `Library → Disk analyzer → Clean up partials` removes orphaned `.xpart` / `.xpart.json` pairs that no job owns.

---

## 4. Data directory

Console: `/data/xeno-store/` (host build: `./xeno-data/` or `--data DIR`).

| file | content |
|:--|:--|
| `settings.json` | §6 below |
| `favorites.json` | `{"ids":[gameId…]}` |
| `collections.json` | `{"collections":[{"id":"wishlist","name":"Wishlist","icon":"star","ids":[]}]}` |
| `recent.json` | `{"ids":[…]}` (30 most recent game pages) |
| `queue.json` | §5 below |
| `stats.json` | lifetime download statistics (`/api/stats`) |
| `catalog.json`, `catalog.meta.json` | refreshed catalog when its revision is newer than the embedded one, plus `{revision, fetchedAt, etag}` |
| `feeds/featured.json`, `feeds/messages.json`, `feeds/companions.json`, `feeds/saves-backend.json` | cached owner feeds (refreshed every 6 h) |
| `enrich/<gameId>.json` | on-console Steam enrichment (30 days) |
| `homebrew.json` | pkg-zone listing cache (7 days) |
| `payloads/<id>.elf`, `payloads/installed.json` | companion payloads and `{<id>:{version, sha256, installedAt}}` |
| `icon.png` | the embedded icon written at start, used for notification icons |
| `log.txt`, `log.1.txt` | file log, rotated at 2 MiB |
| `xeno-store.elf`, `xeno-store.elf.new` | self-update staging |
| `xeno-store.elf.version` | the saved update's version (`N.N.N`, no newline), written after the ELF; the TV app's starter reads it (§10) |
| `tvapi.json` | `{"preferredAuto":true}`: "Choose automatically" for the TV app's default drive |
| `tv-app.json` | the TV app install record `{"version","checksum","size","modified","inode"}`, written after the store installed `/data/homebrew/PPSA99500.ffpkg` (version `""` for the web UI's install); a file that no longer matches it is a copy placed by hand |

---

## 5. `queue.json`

```json
{"v":1,"savedAt":1760313600,"jobs":[ {…job object as in docs/API.md §4…} ]}
```

Written atomically (`.tmp` + rename) on every state change and every 5 s during transfers. On load, jobs in `downloading` / `connecting` / `verifying` become `paused` with the message *Stopped when XENO STORE closed*; `installing` jobs re-check the install status once. Groups (`group`, `groupOrder`) survive restarts, so an install set continues in order.

---

## 6. `settings.json`

Defaults (SPEC §9 + Phase 2):

```json
{"v":1,
 "general":{"language":"auto","theme":"xeno-gold","cardSize":"m","reduceMotion":false,"sounds":true,"hints":true,"showPs4":true,"adultFilter":false,
            "largeText":false,"highContrast":false,"safeMode":false,"screensaver":true,"screensaverMinutes":3,"controllerSwapConfirm":false,"mdns":true},
 "downloads":{"defaultRoot":"/data","connections":8,"concurrentJobs":1,"maxBytesPerSec":0,"verifyHashes":true,"autoInstallPkg":true,"shadowMountScan":true,
              "keepAwake":true,"retryMax":8,"allowInsecureTls":false,"scheduleStart":"","scheduleEnd":"",
              "preferFormat":"auto","turboStart":true,"autoInstallSets":true,"installOrder":"game,backport,update,dlc","restModeAutoResume":true,"extractZip":true,"deleteArchiveAfterExtract":true},
 "debrid":{"realdebrid":{"apiKey":""},"alldebrid":{"apiKey":""},"torbox":{"apiKey":""},"premiumize":{"apiKey":""},"preferred":"auto"},
 "sources":{"enabled":{"archive":true,"vikingfile":true,"dlpsgame":true,"other":true},"acknowledged":false},
 "catalog":{"autoRefresh":true,
            "url":"https://raw.githubusercontent.com/XENOKING123/XENO-STORE/main/catalog/xeno-catalog.json",
            "revisionUrl":"https://raw.githubusercontent.com/XENOKING123/XENO-STORE/main/catalog/revision.json",
            "featuredUrl":"https://raw.githubusercontent.com/XENOKING123/XENO-STORE/main/updates/featured.json",
            "messagesUrl":"https://raw.githubusercontent.com/XENOKING123/XENO-STORE/main/updates/messages.json",
            "companionsUrl":"https://raw.githubusercontent.com/XENOKING123/XENO-STORE/main/updates/companions.json",
            "savesBackendUrl":"https://raw.githubusercontent.com/XENOKING123/XENO-STORE/main/updates/saves-backend.json","lastCheck":0},
 "saves":{"backupRoot":"","autoBackup":true,"indexUrls":[],"backend":{"kind":"github","url":"","anonKey":"","bucket":"ps5-saves","garlicFunction":"garlic-savedb"},
          "uploadHost":"pixeldrain","uploadEndpoint":"","submitEndpoint":"","nickname":""},
 "browse":{"presets":[{"name":"Small PS5 verified","filters":"{\"platform\":[\"PS5\"],\"verified\":true,\"size\":\"le20\"}","sort":"newest"}]},
 "notifications":{"toasts":true,"onComplete":true,"onFail":true,"onInstall":true},
 "updates":{"autoCheck":true,"feed":"https://raw.githubusercontent.com/XENOKING123/XENO-STORE/main/updates/payloads.json","lastCheck":0},
 "pairing":{"pin":""}}
```

`PUT /api/settings` deep-merges; unknown keys are ignored, ranges are clamped (connections 1–16, concurrentJobs 1–4, screensaverMinutes 1–30), URLs must be `http(s)`. Debrid keys are returned masked by the API but stored in clear in this file (it lives only on the console). `preset.filters` is opaque JSON text produced by the Browse screen.

---

## 7. Catalog schema

`catalog/xeno-catalog.json` (schema v1, compact JSON, sorted keys, deterministic):

```json
{"schemaVersion":1,"revision":2,"generatedAt":"2026-10-13T04:19:46Z","name":"XENO STORE Catalog",
 "sources":[{"id":"archive","name":"Archive.org","kind":"direct","homepage":"https://archive.org","notice":"…"},
            {"id":"dlpsgame","name":"DLPSGAME","kind":"page","homepage":"https://dlpsgame.com","password":"DLPSGAME.COM","notice":"…"}],
 "genres":["Action","Adventure","…"],
 "games":[{"id":"cyberpunk-2077-ps5","slug":"cyberpunk-2077","title":"Cyberpunk 2077","platform":"PS5","titleIds":["PPSA04029"],"region":"EUR",
   "genres":["Role-playing"],"tagline":"…","description":"…","about":"…","publisher":"CD PROJEKT RED","developer":"CD PROJEKT RED","releaseDate":"2022-02-15","year":2022,"addedAt":"2026-10-03",
   "cover":"https://cdn.prosperopatches.com/…/icon0.webp","coverFallback":"https://image.api.playstation.com/…","hero":"…","heroFallback":"…","artworkLayout":"wide","tint":"#1a2a5a",
   "screenshots":["https://…"],"metacritic":86,"features":["Single-player","DualSense"],"ageRating":18,"languages":["English"],"series":"cyberpunk","steamId":1091500,
   "psStoreUrl":"https://store.playstation.com/en-us/concept/10000128","cheats":12,
   "sizeMin":30061297664,"sizeMax":156011528192,"featured":0,"popularity":0.374,"verified":true,"compat":["9.00","13.60"],
   "updates":2,"dlc":1,"backports":1,
   "releases":[{"id":"ppsa04029-ffpfsc","titleId":"PPSA04029","region":"EUR","version":"01.003.002","format":"FFPFSC","kind":"game",
     "provider":"Archive.org","sourceId":"archive","filename":"…","url":"https://archive.org/download/…","browserUrl":"","mirrors":["https://…"],
     "sizeBytes":156011528192,"sha256":"","md5":"…","sha1":"…","verifiedAt":"2026-10-03","consoleVerified":false,"compat":"","password":"","notes":"","addedAt":"2026-10-03",
     "backportFw":"","requiresFw":"","health":"ok"}]}]}
```

Rules: `games[].id` = `<slug>-<ps4|ps5|homebrew>`; one game per (normalized title, platform); releases deduped by URL; `format` ∈ `FFPFSC, exFAT, PKG, FPKG, FFPKG, FFPFS, ZIP, RAR, 7z, PAGE`; `kind` ∈ `game, update, dlc, bundle, backport, app`; `sourceId` must exist in `sources`. Homebrew apps use `platform:"HOMEBREW"` with `category`, `author`, `supportsPs5` and `kind:"app"` releases. `health` comes from the audit (`ok` / `flaky` / `page`; `dead` releases are dropped).

`catalog/revision.json`: `{"revision":2,"generatedAt":"…","games":1965,"releases":2592,"verifiedReleases":881,"auditedAt":""}` — the console fetches this first and downloads the catalog only when `revision` grew.

`catalog/report.json`: per-source status and counts, dropped releases with reasons, histograms, enrichment coverage, audit summary (what the sync workflow prints).

---

## 8. Published feeds (`updates/`)

All fetched from `raw.githubusercontent.com` (allow-listed), cached under `/data/xeno-store/feeds/`, refreshed every 6 h with the catalog, overridable per URL in Settings → Catalog.

### `payloads.json` — Payload Manager source + self-update feed
```json
{"name":"XENO STORE","author":"XENOKING","description":"…",
 "payloads":[{"name":"XENO STORE","version":"1.0.0","filename":"xeno-store.elf",
   "url":"https://github.com/XENOKING123/XENO-STORE/releases/download/v1.0.0/xeno-store.elf","sha256":"<64 hex, written by CI>","minFw":"","notes":"…"}],
 "minimumVersion":"1.0.0"}
```
The updater uses the entry whose `filename` is `xeno-store.elf`; `sha256` is lower-cased and must match the staged file. The release job replaces `version`, `url` and `sha256` for every entry (`FILLED-BY-CI` is the committed placeholder before the first release).

### `tv-app.json` — the TV app feed

Written by the release job next to `payloads.json` (never by hand):

```json
{"version":"1.3.0","filename":"PPSA99500.ffpkg",
 "url":"https://github.com/XENOKING123/XENO-STORE/releases/download/v1.3.0/PPSA99500.ffpkg",
 "sha256":"<64 lowercase hex>","size":46137344}
```

The store's TV app check (`POST /api/v1/tv-app {"action":"check"}`, API.md §13.9) reads it first and falls back to
GitHub `releases/latest` plus its `SHA256SUMS` while it is missing. It is used only when `filename` is
`PPSA99500.ffpkg`, `version` is plain `N.N.N` (a leading `v` is dropped), `url` is a release download URL of this
repository ending in `/PPSA99500.ffpkg`, `sha256` is 64 lowercase hex characters (`checksum` is accepted as an alias)
and `size` lies between 65536 + 1376 bytes and 512 MiB; anything else is "The TV app release information is
invalid. Nothing was downloaded.". The release job applies the same rules, and checks the UFS2 magic, before it
writes the file. The install downloads exactly this URL and refuses a body larger than `size`, a different SHA-256
or a missing UFS2 magic `19 01 54 19` at offset 65536 + 1372.

### `messages.json` — announcement banner
```json
{"messages":[{"id":"welcome-1.0.0","ts":1760313600,"url":"https://…","texts":{"en":"…","de":"…"}}]}
```
Newest `ts` wins; each `id` is dismissible once per device; `texts` is a `{lang: text}` map with `en` as fallback (a plain `text` string also works).

### `featured.json` — owner-curated Discover + Homebrew essentials
```json
{"hero":["ghost-of-yotei-ps5"],"rows":[{"title":{"en":"Staff picks","es":"Selección del equipo"},"ids":["elden-ring-ps5"]}],
 "homebrew":["RetroArch_1_19"],"banner":{"text":{"en":"…"},"url":"https://…"}}
```
`hero` ids are spotlighted first; each `rows` entry becomes a rail under "New on XENO"; `homebrew` lists pkg-zone ids for the Essentials rail; `banner` joins the announcements. Unknown ids are ignored (CI checks they exist).

### `companions.json` — companion payload catalog
```json
{"payloads":[{"id":"shadowmount-plus","name":"ShadowMountPlus","kind":"shadowmount","version":"1.03","filename":"ShadowMountPlus.elf",
  "url":"https://github.com/…/ShadowMountPlus.elf","sha256":"<64 hex>","homepage":"https://…","notes":"…","size":0}]}
```
`id` is `[a-z0-9-]` (derived from the name when absent), `url` must be `http(s)`, `sha256` is checked after download — a placeholder such as `FILLED-BY-OWNER` makes the install fail on purpose. `kind` drives the checklist actions (`shadowmount`, `dpi`, `kernel`, `np-signin`, `np-signout`, `dumper`, `saves`). Top-level `_notes` is ignored by the console.

### `saves-backend.json` — Save Data Store backend
```json
{"kind":"github"}
```
or
```json
{"kind":"supabase","url":"https://<ref>.supabase.co","anonKey":"<public anon key>","bucket":"ps5-saves","garlicFunction":"garlic-savedb"}
```
`github` (default) reads `updates/saves.json` and submits through a prefilled issue; `supabase` lists `public.ps5_save_collection`, downloads through signed URLs or the garlic function and uploads to the bucket. Settings → Saves → Backend overrides this file; `kind:"none"` disables the store. Extra keys (`_example`, `note`, `issueUrl`, …) are ignored by the console and exist for humans.

### `saves.json` — community saves index (owned by the saves backend)
```json
{"schemaVersion":1,"updatedAt":"…","saves":[{"id":"sv_…","titleId":"PPSA04029","gameId":"cyberpunk-2077-ps5","title":"Cyberpunk 2077",
 "category":"starter","name":"…","description":"…","author":"nick","gameVersion":"01.004.000","region":"EUR","dirName":"SAVEDATA00",
 "size":1234567,"sha256":"…","url":"https://pixeldrain.com/api/file/…","mirror":"","format":"xeno-save-zip","createdAt":"…","downloads":0,"status":"published","tags":["ng+"],"screenshot":""}]}
```
Consoles show only `status:"published"`. `category` ∈ `starter, platinum, checkpoint, modded, chapter, other`.

---

## 9. xeno-save-zip

A save archive produced by *Upload my save* and accepted by *Install to my saves*:

```
<zip root>/
  SAVEDATA00            decrypted save file(s) exactly as mounted
  sce_sys/param.sfo     optional, ACCOUNT_ID zeroed
  sce_sys/icon0.png     optional, used as the store card cover
  xeno-save.json        manifest (below)
```
```json
{"titleId":"PPSA04029","dirName":"SAVEDATA00","gameVersion":"01.004.000","region":"EUR",
 "files":[{"path":"SAVEDATA00","size":1234567,"sha256":"…"}],"accountIdStripped":true,"sameConsoleOnly":false,"createdBy":"XENO STORE 1.0.0","createdAt":"…"}
```
Sealed keys (`sce_sys/keystone`, `sealedkey`) are never included. Garlic Save Manager's decrypted ZIPs (plain files + `sce_sys/`, no manifest) install as-is: the title id comes from the index row, version and region from the row when present, else "unknown — check before installing". Backups made by the Saves hub use the same layout plus the original `sce_sys/` folder and restore only on the same console.

---

## 10. Games-row app (`PPSA99500.ffpkg`)

A UFS2 image that ShadowMountPlus mounts from `/data/homebrew/` and shows as **XENO STORE** under Games. Since 1.3.0
it holds the native TV app (SPEC §19) with the store inside, in `xeno/`:

```
PPSA99500/
  eboot.bin               the TV app (fake-signed SELF, ET_SCE_DYNEXEC, so sce_module/ is required)
  sce_module/libc.prx     the kit's clean-room libc module (SHA-256 e6ff45d1…, checked when staged)
  assets/                 fonts/ (baked Inter), audio/ (interface sounds + index files), i18n/ (10 catalogues;
                          English is the key), brand/xeno-badge.png
  licenses/               app/tv/NOTICE and the licence texts of every linked component
  xeno/xeno-store.elf     the store; the app sends it to the ELF loader on :9021 when nothing answers on :9095
  xeno/version.txt        VERSION + "\n" (the starter's bundled version)
  sce_sys/icon0.png       512×512 XENO logo
  sce_sys/param.json      {"applicationCategoryType":0,"contentBadgeType":1,"titleId":"PPSA99500","conceptId":"99500","contentId":"IV0000-PPSA99500_00-XENOSTORE0000000","contentVersion":"01.003.000","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"XENO STORE"},…},"amm":{…},"kernel":{…},"pubtools":{…},…}
```

There is no root `xeno-store.elf` in this image (nothing reads `/app0/xeno-store.elf` any more). The starter's
copies are `/app0/xeno/xeno-store.elf` + `/app0/xeno/version.txt` (bundled) and `/data/xeno-store/xeno-store.elf` +
`/data/xeno-store/xeno-store.elf.version` (saved by the updater); the saved copy wins unless the bundled one is newer
(SPEC §19.2).

`tools/make_ffpkg.py --app-dir app/tv/dist/PPSA99500` stages the folder: the app folder from `make tv` (its
`eboot.bin`, `sce_module/` and `assets/`), `app/tv/NOTICE` and `app/tv/licenses/`, the ELF from `--elf` (default
`./xeno-store.elf`) and `version.txt` from `VERSION` (the build fails unless the ELF carries exactly one
`XENO-STORE/<version>` marker equal to it), and `sce_sys/` from the app folder's `param.json` with `titleId`,
`conceptId`, `contentId`, `contentVersion` and `localizedParameters` forced to ours (the GL memory keys `amm`,
`kernel` and `pubtools` are kept; `pubtools.creationDate` comes from `$SOURCE_DATE_EPOCH` or the build time) and our
`icon0.png`. `make ffpkg` uses this mode by itself when `app/tv/dist/PPSA99500/eboot.bin` exists
(`FFPKG_APP_DIR=` forces the launcher image). Without `--app-dir` the image holds the native launcher
(`eboot.bin` + `sce_module/libc.prx` from `make eboot`) and `xeno-store.elf` at its root, as in 1.2.1.

Every entry is mode `0777`; the image is built with `makefs` (UFS2, 64 KiB blocks and fragments, `minfree=0`), sized
to the files plus 8 MiB of slack. No `-S`: the console's UFS driver addresses blocks in 512-byte units, so
`fs_fsbtodb` must be log2(fsize / 512) = 7; makefs with `-S 4096` writes 4 and the image cannot be read on the PS5.
`tools/ufs2_check.py` reads the image back the way the console does (superblock search, `fs_fsbtodb` addressing),
rejects a wrong `fs_fsbtodb`, a truncated file or a missing icon, and compares every file with the staged folder; in
`--app-dir` mode the script also checks the UFS2 magic `19 01 54 19` at 65536 + 1372 (what the store's installer
checks), reads `xeno/version.txt` back from the image and keeps the image at most 512 MiB. contentVersion is
`NN.NNN.NNN` (1.3.0 → `01.003.000`), checked against `VERSION` by `tools/tv/check_param.py` (`make tv-check`).
Settings → Updates → *Install the XENO STORE app* (web UI) and Settings → Updates → *TV app* (the app) put it in
place on the console; copying it to `/data/homebrew/` by hand works too.

`make pkg` (`tools/make_homebrew.py`) can still build a `dist/XENO-STORE/` folder (+ zip) for etaHEN / itemzflow homebrew launchers (`eboot.bin` = `xeno-store.elf`, title id `XENO00002`); it is not part of a release.

---

## 11. Release assets

Tag `v<VERSION>`:

| asset | what |
|:--|:--|
| `PPSA99500.ffpkg` | the Games-row app: the TV app with the store inside (§10) |
| `xeno-store.elf` | the store; Payload Manager and the in-app updater download this file |
| `XENO-STORE-Desktop-Windows.zip`, `XENO-STORE-Desktop-Mac.zip` (universal: Apple silicon + Intel), `XENO-STORE-Desktop-Linux.tar.gz` | XENO STORE Desktop |
| `XENO-STORE-<VERSION>-tv-sources.tar.gz` | the TV app's complete corresponding source (GPL-3.0): `git archive` of the release commit plus every pinned upstream source archive (SDK, ps5-opengl, libwebp, PacBrew, LLVM runtimes, zlib), each checked against its SHA-256 (`tools/tv/make_source_bundle.py`) |
| `SHA256SUMS` | `sha256sum` lines for every file above; the in-app updater and app installer verify against it |
