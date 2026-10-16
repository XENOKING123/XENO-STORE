# Orbit Store for TV (native app)

Modified for XENO STORE in 2026 (port: backend dialect, branding, paths). See app/tv/NOTICE.

## XENO STORE

This folder is the **XENO STORE TV app**: title `PPSA99500`, its
`contentVersion` taken from the repository's `VERSION`, and a client of
`xeno-store.elf` on `127.0.0.1:9095` through the `/api/v1` dialect
(`docs/API.md` §13 in the repository). It is Orbit Store 1.2.0's TV app,
ported to XENO STORE's backend, brand and paths; `NOTICE` lists the
provenance, every licence and every changed file.

- **Build** from the repository root, not in this folder:
  `make tv-deps && make tv && make ffpkg` packs `dist/PPSA99500.ffpkg`;
  `make tv-test` runs the unit tests and `make tv-preview` /
  `make tv-preview-live` draw previews on the PC. There is no `make ffpkg`
  in `app/tv`, and no payload is staged here: `tools/make_ffpkg.py` bundles
  the store's ELF as `/app0/xeno/xeno-store.elf`. See `app/README.md`.
- **Starting the store.** When nothing answers on port 9095, the app sends
  the saved update `/data/xeno-store/xeno-store.elf` or its bundled copy,
  whichever is newer, to the ELF loader on port 9021. An older XENO STORE
  without `/api/v1` (1.2.x) on the port is named on the opening screen,
  with **Replace it now** and **Open browser version**.
- **Install** as `app/README.md` ("How people install it") describes: the
  store's **Settings → Updates → XENO STORE app → Install the app**, or `PPSA99500.ffpkg` copied to
  `/data/homebrew/`.
- **Log:** `/download0/hui/dev/app.log` in the app's sandbox, that is
  `/mnt/sandbox/PPSA99500_000/download0/hui/dev/app.log` over FTP while it
  runs.

Everything below describes upstream Orbit Store 1.2.0 and is kept for
provenance. Its ports, paths, title ID and commands are Orbit's; sections
marked *Upstream only* do not apply to this app.

---

Native TV app **1.4.0**, bundled with the **Orbit Store 1.1.0 service**.

A full-screen PS5 app for Orbit's storefront, drawn by the GPU instead of
the console browser. It is a client of the Orbit backend payload. Downloads,
the queue, storage and the catalogue still live in the payload, which keeps
running when the app is closed. The app talks to it on `127.0.0.1:34177` and
opens the console's own session, so no pairing code is needed.

The app carries a copy of Orbit. When nothing answers on Orbit's port, it
starts Orbit through the ELF loader the jailbreak runs on `127.0.0.1:9021`,
so a separate payload launch is unnecessary when the loader is available.

It follows the browser storefront's design and rules (`src/`)
screen for screen:

- **Discover** (the opening page): the focused game's art behind the hero,
  then New on Orbit, Latest releases and an All games grid, largest downloads first:
  one grid of every game, no pages; L2 / R2 jump 16 rows.
- **Browse**: search, All games or Favourites, Source,
  Platform, Format, Region, Download size and Sort by (release date, newest first, by default),
  Reset filters, and a grid of covers with each game's state.
- **Quick favourites**: press Square on the selected game in Browse or Discover
  to save it or remove it from your favourites. A toast with a heart confirms
  it, wherever a favourite is saved in the app.
- **Library**: the games ShadowMount reports on the console and its drives,
  with the status views, search, Location and Format; a game's details with
  Mount/Unmount and Copy/Move to drive (the same confirmations as the web);
  Storage with each drive's games and Measure game sizes; Scan for games.
- **Downloads**: Active, Finished, Failed and Cancelled, with pause, resume,
  retry, cancel (keep or delete the partial file) and remove from history.
  A PKG that XENO STORE installs once it is downloaded reads Installing, then
  Installed (a badge); a failed install is under Failed with Retry install.
- **Game page**: full-width artwork, the tagline and description, a primary
  download button, Favourite and About. The primary opens Download options;
  Source, Download using and Save to each open a choice panel. Back restores
  the previous control, and the enabled inner download button starts selected.
  View download / View in Library / Download another copy, storage estimates,
  metadata and Vikingfile instructions remain available.
- **App settings** (the gear in the top bar): Download sources, Storage (the
  default drive), Pair a device, Game catalogue and Updates. See below.

Connect TorBox in the browser's **App settings → Debrid**, then select
**Download using → TorBox** on a supported game's native download page.
Without an account, **Set up TorBox** opens the browser with your game, source and destination preserved. The queue labels these jobs **via TorBox**. Account connection and host/file
limits still apply. See TorBox connection and recovery (upstream `docs/debrid.md`).

For Vikingfile, choose the download option and save location, then select
**Open Vikingfile on PS5**. This opens Vikingfile directly in the PS5 browser.
Complete any verification and select **Download**, then return to the native
Orbit Store app. Orbit checks the file and adds it to your chosen drive’s queue.
The native app shows verification progress, cancellation and **View download**
when the file is ready. Going back from the progress panel keeps verification
running; only **Cancel verification** stops it. Your game, source and drive stay
selected. Opening a page alone does not add a download.

Orbit’s download service runs in the background; you do not need to open its
browser interface first. If a source or drive is no longer available, choose an
available option explicitly. An older service without provider-browser support
shows an update message.
Auto-start, payload managers and diagnostics stay in the browser version;
App settings → More settings opens it.

## App settings

A page of its own: sections on the left (Up/Down), the chosen one on the
right (Right or Cross to enter it, Circle or Left to leave).

- **Download sources**: the browser's source choice and download notice.
  On first run, Browse and Discover show **Choose sources**, which opens
  this section; saving it goes on to Discover.
- **Storage**: the drive new downloads start with under Save to
  (`POST /api/v1/storage/preferred`), or Choose automatically. Starting a
  download on another drive still makes that drive the default, as before.
  An Orbit older than this endpoint says it needs updating.
- **Pair a device**: the console's address (`http://<PS5 IP>:34177` in Orbit,
  `:9095` in XENO STORE, from
  the console session) and the six-digit code, read again whenever the
  section opens and after Orbit restarts.
- **Game catalogue**: games, catalogue revision, when it was last checked,
  and Refresh catalogue (with Orbit's cooldown).
- **Updates**: two cards, because the two parts update separately.
  - **TV app**: this app's version (from `contentVersion`, `01.000.000` is
    1.0.0), the latest release and **Update TV app**. A copy placed by hand
    in `/data/homebrew` is replaced only after **Replace with this
    release** is confirmed; a folder copy, or a copy outside
    `/data/homebrew`, is left alone with instructions. Once installed, the
    card says to close the app and open it again, and to restart the
    console if the old version still opens.
  - **Download service** (Orbit): running, saved for next start and latest
    versions; **Install update**; and **Restart download service**, after a
    confirmation, because it pauses downloads. The app asks Orbit to stop,
    waits for its port to close and four seconds more, then sends the saved
    copy to the ELF loader, as when it starts Orbit (it tries once more if
    Orbit doesn't answer within 15 seconds). Without a loader it says to run
    `/data/orbit-store/orbit_store.elf` from a payload manager, and
    reconnects by itself. A toast names the version that came back.
- **More settings**: Open browser version, and both versions side by side.

When the app opens it asks Orbit to look for updates of both parts (the
same automatic check the browser makes, with a shared six-hour cooldown).
A newer release puts a dot on the gear and on Updates and shows a toast
once. Nothing is downloaded or installed until it is chosen.

## What the console needs

- A jailbreak that loads **kstuff** (fake-signed apps) and **ShadowMountPlus**
  (mounts the app and adds its tile). The usual Relapse loader chain loads
  both. Without them the app does not start; the browser icon still works.
- An **ELF loader on port 9021**, as the jailbreak's loader chain provides,
  for the app to start Orbit. Without one, start Orbit from a payload
  manager; the app then waits for it and offers **Try again**.
- Orbit with the artwork endpoint (`/api/v1/art`, 0.6.0 and later), which the app
  brings with it. A running Orbit without it (0.5.1 and older releases) works
  too, but every cover shows its title's initial. To use the app's copy,
  stop the running Orbit (or restart the console) before opening the app.

### Starting Orbit

*Upstream only: XENO STORE's starter is described in the preface.*

When the first status request is refused (nothing listens on port 34177),
the app sends Orbit to the loader once, shows **Starting Orbit…** and waits up
to 30 seconds for it to answer. It sends the copy Orbit saved in
`/data/orbit-store/orbit_store.elf`, which payload managers start and Orbit's
updater keeps current, unless the copy in the package (`/app0/orbit/`) is a
newer release or the saved one can't be read. A slow answer is not a refusal
and never triggers a start, and losing Orbit later is not answered with
another payload: the offline screen's **Try again** starts it on request.
Orbit never replaces a newer saved copy with an older one, so an older
package can't downgrade an Orbit you updated. The log records each
attempt (`[ORBIT] start: ...`).

## Install on the PS5

*Upstream only: XENO STORE's app is `PPSA99500`; see the preface and
`app/README.md`.*

1. Stop a running Orbit first (App settings, Stop Orbit, in the browser
   version, or restart the console and run the jailbreak). A second copy of
   the payload does not replace a running one.
2. Optional: send `build/orbit_store.elf` yourself. The app starts its own
   copy of Orbit when none is running.
3. Copy **one** of these to the console:
   - `dist/PPSA99177.ffpkg` into `/data/homebrew/`, or
   - the folder `dist/PPSA99177/` to `/data/homebrew/PPSA99177/`.

   ShadowMountPlus registers it about ten seconds after the copy finishes.
4. Open **Orbit Store** from the Games row. The icon is the Solid Planet; the
   browser shortcut keeps its own icon in Media.
5. Discover opens first. If Orbit is not running you see
   **Starting Orbit…**, then Discover. If the app can't start it you see
   "Orbit isn't running" with the reason; start Orbit from a payload manager
   and the app reconnects by itself, or press **Try again**.

Title ID `PPSA99177`, content version `01.004.000`. To update, replace the
image or folder and, as ShadowMountPlus stages `param.json` and the icon
once, also replace its staged copies if the tile text or icon changed.

Keep `downloadDataSize` in `sce_sys/param.json` at 256, the kit's package value.

The app writes only its own log (`/download0/hui/dev/app.log` inside its
sandbox; `/mnt/sandbox/PPSA99177_000/download0/hui/dev/app.log` over FTP
while it runs). Close it with the PS button and Close Application.

## Interface language

Orbit follows the PS5 system language when the app starts. English, German, Spanish, French, Hungarian, Italian, Dutch, Polish, Brazilian Portuguese, Russian and Turkish are supported; other languages use English. Close and reopen the app after changing the console language. The language preference in the browser version applies only to that browser.

The interface translations share `i18n/` with the browser. The build packages them under `assets/i18n/`. Game names and descriptions keep the catalogue’s original text. For translation checks, run `make tv-check` from the repository root (`tools/i18n-coverage.py`).

## Controls

| Button | Does |
| --- | --- |
| D-pad / left stick | Move the focus. Up from a screen reaches the top bar. |
| Cross | Select |
| Circle | Back: leaves a game page, closes the keyboard or a dialog |
| L1 / R1 | Previous / next tab |
| Square | On a selected Browse or Discover game: toggle Favourite. In search: delete. In Browse's field: clear the search. In Library: cancel a running copy or move. On a game page: About. |
| Triangle | In search: space. In the Browse grid: search again. On a game page: toggle Favourite. |
| L2 / R2 | In the Browse and Discover game grids: jump 16 rows (96 games) up / down, keeping the column |

Dropdowns (filters, Save to, a copy's destination) open with Cross; move with
the D-pad, choose with Cross, close with Circle.

## Build

*Upstream only.* Orbit built the app in its own build image with `make ffpkg`
in its `app/` folder, which also staged Orbit's payload. XENO STORE builds it
from the repository root (see the preface); the same image is `Dockerfile`
here. The first build fetches pinned dependencies into `app/tv/.deps/`
(ignored): the PS5 payload SDK, the ps5-opengl SDK, PacBrew (for libwebp),
zlib and GoogleTest. Each is checked against a SHA-256 or commit in `tools/`.

### Previews without a console

`make tv-preview` runs the real app code on the PC (Mesa, surfaceless EGL)
with scripted controller input and writes a picture per step. It uses a
built-in stand-in backend with invented games and generated art
(`host/fixture.cpp`). `make tv-preview-live` previews against the desktop
build of XENO STORE instead. Pictures are the PC's rendering; frame rate,
sound and controller feel are console checks.

## How it is built

```
src/orbit/       the app: API client, state sync, artwork, screens
src/main.cpp     console entry point
host/            PC preview and its stand-in backend
src/gfx, ui, core, audio, platform, runtime, third_party
                 the ps5-homebrew-ui kit (see Provenance)
tooling/, tools/ the native PS5 build: compile, link, FSELF signing, packaging
```

- **Starting Orbit.** `orbit/starter` picks the copy to send and streams it
  to the loader; `orbit/store` decides when (see Starting Orbit above).
- **Network.** `orbit/http` is a small HTTP/1.1 client for the loopback
  backend. `orbit/store` polls status, sources, the catalogue (only when its
  revision changes), the queue, storage, favourites and the Library (Orbit
  answers that from its cache and limits ShadowMount reads itself) on one
  worker thread (every second while Downloads is open, else every three)
  and carries out actions in order. Library drives are read only while the
  Library needs them. Screens read a snapshot; nothing on the frame waits
  for the network.
- **Rules.** `orbit/model` mirrors the browser's: grouping, Latest releases,
  Browse's filters and order (`src/browse.ts`), and where an option stands
  (`src/collection.ts`: an unfinished download, an exact Library copy, a
  finished download, or a related copy).
- **Artwork.** `GET /api/v1/art/<gameId>/<cover|hero>` on the backend fetches
  the catalogue's own HTTPS URL on its workers and caches it (see
  the upstream `docs/api.md`). The app decodes WebP with libwebp (scaled while
  decoding) and PNG/JPEG with stb_image, at drawing size, on its own worker,
  then uploads at most two textures per frame. The heap is fixed at 128 MiB,
  so at most 64 covers and 4 banners stay resident.
- **Look.** The browser storefront's tokens and measurements (`src/styles.css`
  at 1920 x 1080, 1 rem = 20 px) in Inter, with the kit's SDF renderer: the
  same header, pill tabs, 3 px white focus ring with its glow behind the
  element, covers with names and dates, dropdowns, dialogs and veils over
  the art. The mark is rasterised on the CPU at start-up so its cut-outs
  work over art.

## Installing from Orbit

*Upstream only: XENO STORE installs and updates the app through
`/api/v1/tv-app` and the release's `updates/tv-app.json` (see `app/README.md`).*

Once a release carries the app, people install it from the browser version:
**App settings → TV app → Install on this PS5**, and update it there or in
the app itself (**App settings → Updates**). Orbit downloads the image,
checks it and places it in `/data/homebrew/` (see the TV app installer in
the upstream `docs/api.md`). The same panel links to the latest GitHub
release for installing by hand.

Orbit found the app through a `tv-app.json` feed that its own release
tooling generated with the image's exact size and checksum.

## Provenance and licences

The app is GPL-3.0-or-later, like Orbit.

- `src/gfx`, `src/ui` (with `components`), `src/core`, `src/audio`,
  `src/platform`, `src/runtime`, `src/third_party/stb/{vorbis.c,stb_vorbis.inc}`,
  `tooling/`, `runtime/`, `host/platform_host.cpp`, `third_party/stb/` and
  most of `tools/` are from
  [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) at
  commit `4bd942579dd981b3df9c740438489ca6a614ddc0` (GPL-3.0-or-later;
  BlackBearReloaded; its native tooling derives from SharpProspero, GPL-3.0).
  They are unmodified except `tools/bake-fonts.sh` (Orbit's font table),
  `tools/build-tests.sh` (links libwebp, adds `host/`), `tools/build.sh`
  (copies the staged Orbit payload into the package) and the new
  `tools/preview.sh` and `tools/stage-payload.sh` (deleted in XENO STORE; see
  `NOTICE`).
- `assets/audio/sfx/glass` are the kit's interface sounds (BlackBearReloaded,
  distributed under the kit's licence).
- Inter 4.1 (`third_party/fonts`, SIL Open Font License 1.1); the baked atlases
  in `assets/fonts` carry the licence beside them.
- `src/third_party/stb/stb_image.inc` is stb_image (public domain or MIT) at
  the kit's pinned stb commit.
- Linked at build time, not stored here: the ps5-opengl SDK
  (GPL-3.0-or-later, with Mesa and OpenGNM PSBC under their own licences),
  libwebp 1.4.0 from PacBrew (BSD-3-Clause, `../licenses/libwebp-*`), and the
  kit's clean-room `libc.prx` shim generated from `tooling/`.
- Carried in the package: Orbit's own `orbit_store.elf`, with the components
  listed in Orbit's `THIRD-PARTY-NOTICES.md`. XENO STORE's package carries
  `xeno-store.elf` instead; `NOTICE` and the release's corresponding-source
  bundle (`tools/tv/make_source_bundle.py`) list what XENO STORE ships.

### Orbit Zero

*Upstream only: Orbit Zero is not part of XENO STORE, and this app has no
Orbit Zero controls.*

These controls require the matching Orbit Store 1.0.0 FFPKG, receiver ELF and Orbit Zero desktop app. Earlier packages do not support Zero. Orbit Zero 1.1.0 and the matching native app support the same 11 interface languages. PS4 browsing needs the 1.1.0 receiver and native app 1.4.0.

Pair Orbit Zero on the computer with the running, matching Orbit ELF. In the TV app, open **App settings → Orbit Zero → Enable Orbit Zero**. The native storefront then uses the computer's catalogue and artwork; Download follows the computer's saved preference. Downloads shows the internet rate and the transfer rate to the PS5 separately. Keep the computer and Orbit Zero running while it transfers. Vikingfile verification opens on the computer, with a prompt to continue there. Turning this mode off affects new downloads; existing computer transfers retain their original route.

**Computer** is the default preference. **Prefer PS5** probes the selected source before starting a new direct console download and falls back to the computer when the console cannot fetch it. The computer and PS5 may use Wi-Fi or Ethernet on the same reachable local network; only direct console downloads require the PS5 to have internet access. Orbit's companion API documented registration, routing and token boundaries.
