# Changelog

All notable changes to XENO STORE. The version lives in `VERSION`; releases are tagged `v<VERSION>`.

## 1.4.0 — unreleased

### Saves
- **Saves is a main tab** in the web UI, right after Browse (and on the phone's bottom bar): the community saves, *Upload my save*, your backups (the Saves hub) and the Save Store options all start there.
- **The TV app has a Saves tab too.** Browse the community saves, each one crediting the player who shared it, filter by *All*, *For my games* or category, search, and see how each save fits the version of the game you have. Install one to the profile you pick: your current save of that game is backed up first, the progress shows live and you can cancel. A game page shows **Community saves (N)** when there are saves for it, which opens the tab on that game.
- **Refresh in the Save Store fetches the list again**: the console rebuilds its list from the XenoKing site and the GitHub index instead of showing its copy for up to an hour (at most once every 30 seconds).
- **Backups wait their turn**: a backup now holds the same per-game lock as restores, installs and transfers, so a restore can no longer swap the save while it is being zipped. **Back up all** now lists the saves it skipped and why, instead of saying "Nothing to back up".

### Downloads
- **Auto-install does what the download says.** Turning Auto-install off in a download's options now keeps that package from installing even while Settings → Downloads → *Auto-install PKG* is on, and turning it on still installs when the setting is off. The setting is the default for new downloads; turning it on later does not install packages already in the queue.
- **Delete the PKG after installing** (Settings → Downloads, off by default) frees a package's space once the console confirms its install is complete. A package whose install fails or can't be confirmed is kept; Downloads shows *Package removed*.
- **Downloads continue after a restart** (Settings → Downloads → *Resume after a restart*, on by default): what was downloading when XENO STORE stopped — an update, *Restart download service*, a crash, a power cut or a reboot — carries on by itself when the store starts again, one after another in queue order. Downloads you paused stay paused. When the store crashed right after its last start, nothing starts by itself until the next normal start. A Verify that was cut short no longer downloads the whole file again. The TV app's *Restart download service* now says your downloads continue on their own.

### Pairing
- **See and unpair your devices**: Settings → Pair a device lists every paired phone and PC with its name and when it was last used, with **Unpair** and **Unpair all other devices**. The XENO STORE app and the console's own browser are not on the list and keep working.
- **Pairing no longer stops at 16 devices**: instead of "All pairing slots are in use", pairing one more replaces the device unused the longest, and the new device is told which one made room.
- **Unpaired devices are cut off at once** and their Link page says why, also the next time they open the store. XENO STORE Desktop opens its own link page when its PC was unpaired, and shows up in the list as *XENO STORE Desktop*.
- After updating from 1.3.x the TV app moves to the console's own session by itself. Devices paired before the update show *Not used since XENO STORE was updated* until they next connect.

### Start-up and updates
- **The store opens the XENO STORE app** at start, not the browser, when the app is installed and the store was started from your ELF loader or Payload Manager. Without the app (or if it doesn't start) the browser opens as before. *Restart now* brings the browser back only when you pressed it in the console's own browser; from a phone or a PC the app opens.
- **Settings → Updates → XENO STORE app** says when a newer app is published (*Update available*) and offers **Update the app**; *Check for updates* checks for it too.

### Fixes
- **Uninstall really removes the app**: *Home-screen app* now deletes `PPSA99500.ffpkg` (and a `PPSA99500` folder copy), takes both the **XENO STORE** and **XENO STORE Web** tiles off the home screen, and cancels an app install that is still running. Before, the file and the tiles stayed, and the tile started the store again.
- The uninstall sheet no longer has a *Payload Manager copy* checkbox that did nothing: Payload Manager keeps its own copy, which you remove in Payload Manager.
- **Fresh feeds after an update**: the featured games, announcements, companions list and save site settings are fetched again right after an update instead of up to 6 hours later, so the XenoKing saves show and companions install at once.
- **Placeholder companion hashes are never used**: a companion without a real checksum is shown as not available and never downloads; Garlic Save Manager then uses the release pinned in the store.
- **Safer releases**: the release job refuses to publish a version again from an older commit, tags the commit it built, and retries its push cleanly after a stopped rebase.
- **Linux desktop download**: unpacking `XENO-STORE-Desktop-Linux.tar.gz` no longer makes the folder it is unpacked into private (0700).

## 1.3.0 — 2026-10-15

### The TV app
- **XENO STORE is a real PS5 app now.** The XENO STORE tile under **Games** opens a native app made for the TV instead of the browser: drawn by the console's GPU at 1080p and driven entirely with the controller (D-pad, ✕, ◯, L1 / R1 for the tabs). It wears XENO STORE's gold-on-dark look, with the XENO badge spinning a gold ring while it starts.
- **Everything you need on one screen:**
  - **Discover**: the featured games up top, *New on XENO STORE*, the latest releases and every game, with real cover and hero art.
  - **Browse**: the whole catalog with filters (PS5 / PS4, format, region, favourites) and a search keyboard.
  - **Game pages**: the description, every release (game, backport, update, DLC) with its format, size, region and version, and the drive it goes to.
  - **Downloads**: live progress, speed and retry countdowns; pause, resume, retry, cancel (keep or delete the partial file) and archive passwords.
  - **Library**: the games on every drive, what is installed, free space per drive, and copying or moving a game image to another drive.
  - **Settings**: download sources, the default drive (or *Choose automatically*), the pairing code for your phone, a catalog refresh, updates for the store and for the app, and *Restart download service*.
- **It starts the store for you.** When the store isn't running, the app sends the copy it carries (or the newer update you downloaded) to your ELF loader on port 9021 and opens as soon as the store answers. No browser opens over it. If a downloaded update doesn't come up, the next try uses the copy inside the app.
- **One store, everywhere.** The app uses the same store as your phone and PC: a download started on the TV shows up in the web UI and the other way round, and favourites, drives and settings are shared.
- **The app updates itself**: **App settings → Updates → TV app** in the app checks for a new version of the app and installs it; it opens the next time you start it. A copy you placed yourself is only replaced when you confirm.
- **Vikingfile games**: the app says why it can't fetch them by itself and offers TorBox or *Open browser version*.
- **Languages**: English, Deutsch, Español, Français, Magyar, Italiano, Nederlands, Polski, Português (Brasil), Русский and Türkçe. Arabic, Japanese, Korean and Chinese show English in the TV app for now; the web UI keeps all 14 languages.

### Save Store
- **The XenoKing community saves** are in the Save Data Store: ready-made saves shared by the XenoKing community (57 today, more as people share), each one crediting the player who shared it, installable in one tap with a backup of your own save first. New saves added on the XenoKing site show up in the store by themselves.
- **Share yours with one press**: a save you share from the store goes straight to the XenoKing collection and shows up there and in the store right away.
- **Nothing to set up**: the store talks to Garlic Save Manager's real API and starts it for you when a save needs re-signing (it downloads the official release and checks its hash first; with no internet it can use a Garlic copy already on the console's internal storage; it never starts a second copy over a running one).
- **Safer installs**: a save is refused while its game is running, when it belongs to another game, or when it isn't a real save; restoring a backup swaps folders so your current save is only removed once the restored one is in place; only one save operation runs per game and profile at a time.
- **Companions** (ShadowMountPlus, kstuff, DPI, Garlic, the app dumper, NP fake sign-in) now come from each author's own release with checked hashes.

### Install and update
- Update the store as usual (**Settings → Updates**), then **Settings → Updates → XENO STORE app → Reinstall / update** (*Install the app* when it is not there yet) puts the new app in your Games row. Copying the new `PPSA99500.ffpkg` to `/data/homebrew/` yourself works too. The app needs kstuff + ShadowMountPlus, and an ELF loader on port 9021 to start the store.
- `PPSA99500.ffpkg` now carries the TV app with the store inside (`xeno/xeno-store.elf`). Each release also attaches `XENO-STORE-<version>-tv-sources.tar.gz`, the TV app's complete source with every pinned part it is built from, as its GPL-3.0 licence asks.

### Fixes
- **Password-locked archives**: about one wrong password in 128 gets past a ZIP archive's quick password check. When one of the community passwords did that, extraction stopped with *could not decrypt* before trying the right one. XENO STORE now moves on to the next password.

### Credits
- The TV app is based on **Orbit Store's TV app** ([saawant12/orbit-store-ps5](https://github.com/saawant12/orbit-store-ps5)) and **BlackBearReloaded's ps5-homebrew-ui** (GPL-3.0), with parts from SvenGDK's SharpProspero, the ps5-opengl SDK, libwebp, the LLVM runtimes, stb and the Inter typeface. `app/tv/NOTICE` lists every component and its licence. Thank you!

## 1.2.1 — 2026-10-14

### Fixes
- **The app now installs.** The 1.2.0 `PPSA99500.ffpkg` could never be read by the PS5: the image was built with a 4096-byte sector setting that made the console look for every file in the wrong place, so ShadowMountPlus could not open it (however it was copied). The image is now built the way the console reads it, the app's `param.json` uses the PS5 layout (category 0, content version `01.002.001`), and the build checks the image the same way the console does before it is released. Delete the old `PPSA99500.ffpkg` from `/data/homebrew/` and restart the console (ShadowMountPlus stops retrying a file after three failed tries) before installing the new one.
- **The app's launcher is now a real PS5 app.** The Games-row tile's `eboot.bin` is built like a known-working PS5 app (a native, dynamically linked executable with its `sce_module/libc.prx`) instead of a bare payload-style binary, using the open-source ps5-native-app-boilerplate toolchain (credited in `app/native-launcher/NOTICE`). It never starts a second store when one is already running, and closes itself once the store is open.
- **Downloads on PS5 are sturdier**: a paused or removed download no longer leaves a connection thread behind that could crash the store; a connection stuck in the console's network library is cancelled within about a tenth of a second; the console's network memory is sized for every download the settings allow, and downloads queue fairly for it instead of failing.
- **Updates can't lock you out**: if a downloaded update doesn't come up after the hand-over, the next start sets it aside and runs the built-in version; an update that stopped right after starting is set aside too, and an old leftover download is no longer offered as an update.
- **ShadowMountPlus is detected properly** (the check no longer depends on an API that older versions don't have), and redirects are followed by the store itself so the real download host is always checked.
- **The store no longer stops a few seconds after "ready" on PS5** (seen on firmware 12.40). Every thread in the store now starts with a proper stack instead of the console's small default, which the setup check and busy requests could run past.
- **Crash reports and safe start**: if the store ever stops by itself, it writes a short report. The next start says so in a notification, shows the report in **Settings → Diagnostics** (what was running, the network step, stack use) and starts safe: the automatic catalog, update and feed checks stay off for that session, and everything you start yourself still works.

### Apps
- **One ELF**: `xeno-store.elf` is the only payload now (`xeno-store-browser.elf` is gone). It opens the store on the TV when it starts; turn that off with **Settings → General → Open XENO STORE when it starts**.
- **The store installs its own app**: **Settings → Updates → XENO STORE app → Install the app** puts the XENO STORE app in your Games row. Copying `PPSA99500.ffpkg` to `/data/homebrew/` yourself still works.
- **Smaller app**: the `.ffpkg` keeps only 8 MiB of spare room, so `PPSA99500.ffpkg` is about 17 MiB instead of 81 MiB.
- **The app picks up updates**: an update downloaded in **Settings → Updates** is the version the app starts from then on.
- A much simpler release: `PPSA99500.ffpkg` (the app), `xeno-store.elf` (the store), one XENO STORE Desktop download per OS (Windows, Mac — universal for Apple silicon and Intel — and Linux) and one `SHA256SUMS`. The second browser ELF, `XENO-STORE.pkg`, the homebrew zip, the per-file `.sha256` files, `BUILD-INFO.json` and the catalog copy are gone.

## 1.2.0 — 2026-10-14

### Apps
- **PS5 app (`PPSA99500.ffpkg`)**: XENO STORE in the Games row. Copy it to `/data/homebrew/`; ShadowMountPlus mounts it. The app's launcher opens the running store full screen, or hands the store inside the app to the ELF loader first. Also ships as `XENO-STORE.pkg` for consoles that install fake packages.
- **XENO STORE Web**: the browser shortcut the store adds on the PS5 now has its own name, next to the app.
- **XENO STORE Desktop** for Windows, macOS and Linux: finds your PS5, links with the code the PS5 shows in a notification, opens the whole store in your PC browser, and sends game files from the PC straight into the PS5's download queue.
- **Link to your PS5**: a phone or PC browser opening the store lands on a link page first — "Show code on my PS5", type the 6 digits, done once per browser.
- Releases carry a `.sha256` file for every PS5 file and a `BUILD-INFO.json`.

### Look and feel
- Clean, bigger art: six large square tiles per row on a TV with the next one peeking in, nothing painted on the art (no PS4/PS5 badges), the title and size underneath, a bright ring on focus, quicker transform-only motion. The 3D bubble cards live on as the Arcade design pack.
- Letter-spaced tabs with a sliding underline and quieter section labels.
- Home order: the showcase, Top PS5 games, Latest releases, staff picks, then New on XENO STORE — PS5 games and real art first, so placeholder PS4 entries no longer lead a row.
- PlayStation artwork is requested at TV resolution instead of 260 px thumbnails; the announcement bar steps aside once you scroll.

## 1.1.1 — 2026-10-14

### Catalog
- Room for the full online catalog (5,000+ games from every source): the file leaves empty fields out and the store fills them back in, which halves its size, and the console keeps each JSON value in 24 bytes instead of 40, so it can read 1.5 million values in the same memory as 900k before.
- The 6-hourly sync enriches games again with official titles, cheat counts and covers from the bundled seeds.
- The offline copy kept by the browser is replaced, not left stale, when a catalog no longer fits.
- "Get links" works on every page source (SuperPSX, pkg.games, PSPKG, ArabicPS4Games, RomsFun, RomsPure, EspacyPKG) and also lists file-host pages such as Mediafire, 1fichier, AkiraBox, DataNodes and Pixeldrain.

### Saves
- Save spotlight: the Saves screen opens on a showcase of the most downloaded community saves (compatible ones first) with Install front and centre, followed by Newest, Most downloaded and Works with your games rails. Discover shows a compact spotlight above the community saves rail.

### Downloads
- A host that times out or answers 5xx / 429 makes the next attempt use the next mirror, with a short wait instead of the full back-off.
- Pausing, resuming or rebooting no longer uses up the retry budget.
- The resume tail check follows the "Allow insecure TLS" setting like the transfer itself.
- The console stays awake while a big game is verified, extracted or installed, not only while it downloads.

### Privacy
- Debrid API keys never leave the console: settings and exports show `****` plus the last four characters, and sending a masked value back keeps the stored key.

## 1.1.0 — 2026-10-13

### Home
- Discover opens on a moving showcase of the ten most popular games: full-bleed art that crossfades with a slow drift, a ranked line-up strip with an on-air marker, a progress bar, and an ambient glow tinted by each game. It pauses while you browse inside it, on other screens and in the screensaver; Reduce motion swaps instantly.

### Look and feel
- Six new themes: Obsidian, Neon, Sunset, Ocean, Galaxy and Sakura (13 in total).
- Design packs change the layout language on top of any theme: Broadcast (default), Gallery, Minimal and Arcade (Settings → Appearance).

### Updates
- Update & restart: after an update is downloaded and checked, Settings → Updates offers *Restart now*. The store stops cleanly, hands the new version to the ELF loader on port 9021 and the page reconnects by itself.
- Fixed: installing an update always failed the "is this an ELF" check on real-size files.

## 1.0.0 — 2026-10-13

First release. A native PS5 game store + download manager by XENOKING.

### Console payload (C, `src/`)
- One ELF (`xeno-store.elf`, plus `xeno-store-browser.elf` that opens the PS5 browser), HTTP/1.1 server on :9095, embedded UI + catalog + locales (gzip variants), PWA manifest.
- Download engine: segmented transfer with work-stealing chunks (1–16 connections), per-host adaptive window, resume sidecars with validator re-probe and tail check, mirror failover, retry backoff honouring `Retry-After`, provider-page detection, free-space guards, token-bucket bandwidth limit, sha256 verification, queue persistence.
- Phase 2 engine: Turbo start (mirror race), auto best version, install sets with ordered auto-install, schedule window with "Start now anyway", lifetime statistics, rest-mode detection and auto-resume, ZIP extraction on the console (miniz), copy/move between drives as queue jobs, debrid resolution (Real-Debrid, AllDebrid, TorBox, Premiumize).
- PKG install: native `sceAppInstUtilInstallByPackage`, fallbacks to the DPI daemon on :9040 and etaHEN DPI on :12800 (`/pkgsrv/<token>`), install polling, Play and Uninstall for installed titles.
- Console features: readiness checklist, uninstall and ghost-icon cleaner, companion payloads (sha256 + local ELF loader on :9021), saves hub (backup / restore / transfer), Save Data Store with native / Garlic / raw engine tiers and GitHub or Supabase backends, offline account activation (registry slots) with reboot, mDNS for `xeno.local` / `xenostore.local`, disk analyzer, homebrew store (pkg-zone) with image proxy, on-console Steam enrichment, owner-curated feeds (featured, messages, companions, saves backend).
- Self-update from `updates/payloads.json` with sha256 verification and GitHub release fallback.
- Safety: bounded buffers, sanitized paths built only from known roots, job-owned deletes, allow-listed outbound hosts, optional pairing PIN, runtime `dlsym` for every Sony API outside the five proven libraries.

### Web UI (`web/`)
- Single-file ES2017 bundle (no frameworks) with DualSense spatial navigation, history-based back, gamepad polling, sounds, 7 themes, large-text / high-contrast, safe mode, cover-wall screensaver, command palette and shortcuts overlay.
- Screens: Discover (hero, curated rows, rails, surprise me, all games), Browse (faceted filters with live counts, quick chips, presets, ranked search with "Did you mean"), Game (gallery + full-screen viewer, info panel, backports, install sets, Download best, saves tab), Downloads (groups, stats, schedule, rest pill, extraction, copy jobs), Library (storage + disk analyzer, installed titles with Play / Uninstall, files, Updates center, saves hub, community saves, transfer), Homebrew, Settings (all Phase 2 groups), Setup (checklist, Payload Manager QR, companions, ghost cleaner), Search overlay, first-run wizard, phone layout.
- 14 locales wired in (English complete; other locales fall back to English until translated).

### Catalog pipeline (`tools/catalog/`)
- 1,965 games / 2,592 releases merged from a verified catalog feed, Archive.org, dlpsgame, SuperPSX, pkg.games, pspkg, ArabicPS4Games, romsfun, romspure, espacypkg and pkg-zone; generic WordPress adapter; offline seeds enrichment (official titles, cheats counts, covers); Steam / PlayStation Store enrichment with committed caches; backports and install-set fields; link audit with `ok` / `flaky` / `dead` / `page` verdicts; deterministic output; 125 unit tests.

### Packaging, CI, docs
- `make` targets `web catalog assets host test ps5 ps5-check pkg all clean distclean`; `tools/make_homebrew.py` builds `dist/XENO-STORE/` + `XENO-STORE.zip` (mode 0777 entries, titleId `XENO00002`).
- `build.yml`: web + catalog → host tests (77) → PS5 ELF (LLVM 18, SDK v0.40) → release `v<VERSION>` with `SHA256SUMS`, writes hashes into `updates/payloads.json`.
- `catalog-sync.yml`: every 6 hours, online build + link audit + validation, commits only real changes.
- Docs: README, GUIDE, API, FORMATS, SPEC; feeds `updates/payloads.json`, `messages.json` (10 languages), `featured.json`, `companions.json`, `saves-backend.json`.

### Known limits (documented, not faked)
- RAR / 7z archives are downloaded, not extracted on the console.
- Human checks on browser hosts are finished in the PS5 browser or on the phone; no CAPTCHA bypass.
- PS5 fake packages run only on firmware 11.60 and lower (October 2026) with kstuff / etaHEN; the checklist says what applies to your console.
- Companion payloads install only once the owner publishes their sha256 (placeholders refuse to install by design).
- Hardware calls (install, saves, accounts, mDNS, rest mode) are verified on a console before each release; CI compiles them and tests the host simulation.
