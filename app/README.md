# XENO STORE — the Games-row app

XENO STORE runs as a **payload** (`xeno-store.elf`, loaded by the console's
jailbreak), so its downloads keep running after you leave the store. This
directory holds the apps that put XENO STORE on the **Games** row:

| Folder | What | Built by |
|:--|:--|:--|
| `tv/` | **The TV app**: XENO STORE's native PS5 app, drawn on the GPU and driven with the controller. It is what `PPSA99500.ffpkg` opens since 1.3.0. | `make tv` → `app/tv/dist/PPSA99500` |
| `native-launcher/` | The previous launcher (opens the store in the browser), kept as the fallback build until the TV app is proven on every console. | `make eboot` → `app/dist/` |
| `eboot/`, `launcher_main.c` | The old freestanding launcher and the payload-style launcher; not released. | `make eboot-raw`, `make launcher` |

`make ffpkg` (`tools/make_ffpkg.py`) packs `dist/PPSA99500.ffpkg`: the TV app when
`app/tv/dist/PPSA99500` holds a built app folder, otherwise the native launcher.
An app folder built for another `VERSION` (its `sce_sys/param.json` `contentVersion`)
is refused: run `make tv` again after changing `VERSION`.

## The TV app (`app/tv`)

The TV app is XENO STORE's own screen on the TV: Discover, Browse with filters
and search, game pages with every release option, Downloads, Library (drives,
copy and move) and Settings (sources, default drive, pairing, catalogue,
updates for the store and the app, restart). It wears the store's gold-on-dark
theme and talks to the store on `127.0.0.1:9095` through the `/api/v1` dialect
(`docs/API.md` §13), so the queue, favourites, drives and settings are the same
as in the web UI and on phones.

It is the TV app of [Orbit Store](https://github.com/saawant12/orbit-store-ps5)
1.2.0 (GPL-3.0-or-later), built on BlackBearReloaded's
[ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) kit and
ported to XENO STORE's backend, brand and paths. Every changed upstream file
keeps its header and carries the line
`Modified for XENO STORE in 2026 (port: backend dialect, branding, paths). See app/tv/NOTICE.`;
`app/tv/NOTICE` lists the provenance, every licence and every changed file.
Design and contracts: `docs/SPEC.md` §19.

**How it starts the store.** When nothing answers on `127.0.0.1:9095`, the
app's starter sends a copy of the store to the ELF loader on `127.0.0.1:9021`:

- the saved update, `/data/xeno-store/xeno-store.elf` with its version in
  `/data/xeno-store/xeno-store.elf.version` (written by the store's updater), or
- the bundled copy, `/app0/xeno/xeno-store.elf` with `/app0/xeno/version.txt`,

whichever is newer (the saved one on a tie). It waits up to 30 s for the store
to answer; if a saved update does not come up, the next attempt sends the
bundled copy. The store notices the app's first request and **does not open the
browser** or its start-up notifications over it. *Restart download service*
asks the store to pause and save its downloads and stop
(`POST /api/v1/updates {"action":"stop","confirmed":true}`); the store never
relaunches itself after that, the app starts the next copy the same way.

**How it updates.** Settings → Updates in the app has two cards: the store
(the same updater as the web UI; the downloaded ELF becomes the saved copy above)
and the TV app (`/api/v1/tv-app`: the release's `PPSA99500.ffpkg`, checked by
size, SHA-256 and UFS2 magic, renamed over `/data/homebrew/PPSA99500.ffpkg`;
it opens on the next launch). The release job publishes `updates/tv-app.json`
for it.

## How people install it

1. Send `xeno-store.elf` to the ELF loader (port 9021) or Payload Manager once.
   The store opens on the TV.
2. **Settings → Updates → XENO STORE app → Install the app.** The store downloads
   `PPSA99500.ffpkg` from the latest release into `/data/homebrew/`, checks it
   against the release's `SHA256SUMS` and asks ShadowMountPlus to rescan
   (`src/app_install.c`). Copying `PPSA99500.ffpkg` to `/data/homebrew/` by
   hand works too.
3. From then on, open **XENO STORE** from its tile under Games. The TV app
   starts the store when it is not running and keeps itself up to date.

**Needs:** kstuff (fake-signed apps) and ShadowMountPlus (mounts the image and
adds the tile), plus an ELF loader on `127.0.0.1:9021` so the app can start the
store.

## What is inside `PPSA99500.ffpkg`

`tools/make_ffpkg.py --app-dir app/tv/dist/PPSA99500` stages the TV app folder
and writes a UFS2 image with makefs (the profile ShadowMountPlus documents),
then checks every file in it with `tools/ufs2_check.py`, the UFS2 magic, the
bundled version and the size (`docs/FORMATS.md` §10):

```
build/apppkg/src/PPSA99500/
  eboot.bin            the TV app (a native app in a fake-signed SELF)
  sce_module/libc.prx  the kit's clean-room libc module (a native app folder carries it)
  assets/              fonts, interface sounds, the 10 translations, the XENO badge
  licenses/            app/tv/NOTICE and the licence texts
  xeno/xeno-store.elf  the store the app starts
  xeno/version.txt     its version (VERSION); the ELF must carry the same XENO-STORE/<version> marker
  sce_sys/param.json   titleId PPSA99500, contentId IV0000-PPSA99500_00-XENOSTORE0000000
  sce_sys/icon0.png    512x512 XENO logo
```

Without a built TV app folder (`make ffpkg FFPKG_APP_DIR=` forces it) the image
holds the native launcher instead: `eboot.bin` and `sce_module/libc.prx` from
`make eboot`, and `xeno-store.elf` at the root.

## The native launcher (fallback)

The launcher (`app/native-launcher/src/launcher.c`) is a native PS5 app, laid
out like a known-working app eboot (ET_SCE_DYNEXEC, PT_DYNAMIC, PROCPARAM, TLS,
EH frame). It is built with the ps5-native-app-boilerplate toolchain and
imports only from the system's libSceLibcInternal (C library), libkernel
(files, sockets, kernel log) and libSceSystemService (exit); the app folder
also carries the toolchain's clean-room `sce_module/libc.prx`. When the tile is
pressed it:

1. asks a running store to open itself full-screen (`POST /api/launcher/open`
   on `127.0.0.1:9095`; only an `HTTP/1.x 200` answer counts), or else
2. sends `/app0/xeno-store.elf` to the ELF loader on `127.0.0.1:9021` (six
   connect attempts, 500 ms apart), polls `127.0.0.1:9095` every 500 ms for up
   to 30 s and asks the store to open once it answers. A newer
   `xeno-store.elf` downloaded from Settings → Updates takes over by itself
   when the store starts (`update_handoff_newer()` in `src/update.c`).
3. Then it asks the system to close the app
   (`sceSystemServiceLoadExec("exit")`), so the console goes back to the home
   screen or the browser.

Every step is written to the kernel log as `[XENO STORE] launcher: ...`.
`make launcher-test` runs the same code on the build machine against a fake
store and a fake ELF loader.

## Deep-link icon (automatic)

When the payload starts, `src/launcher_icon.c` also writes a shortcut app to
`/user/app/XENO00002`: a `param.json` whose `deeplinkUri` is
`http://127.0.0.1:9095/`, plus the icon. It registers the title through
libSceAppInstUtil (resolved at runtime, with a rescan fallback) and opens the
store in the browser. It needs no kstuff or ShadowMountPlus, and it does not
start the payload. The outcome is recorded in `/data/xeno-store/launcher.json`.

## Building

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make ps5          # xeno-store.elf
make tv-deps      # once: the TV app's pinned SDK v0.42, libwebp, ps5-opengl, GoogleTest -> app/tv/.deps
make tv           # app/tv/dist/PPSA99500 (make tv-check runs first)
make ffpkg        # dist/PPSA99500.ffpkg: the TV app + xeno/xeno-store.elf (needs makefs)
make eboot        # app/dist/eboot.bin + app/dist/sce_module/libc.prx (the fallback native launcher)
```

The TV app needs clang-18, lld-18, llvm-18, `libclang-rt-18-dev`, ninja, wget,
unzip and python3 (`PS5_PAYLOAD_SDK` is not used: `make tv-deps` fetches the
SDK it pins). `make tv-test` runs its unit tests (ASan + UBSan), `make
tv-preview` renders every screen with Mesa llvmpipe against a built-in fixture,
and `make tv-preview-live` renders them against the host build of the store in
a test world with a fake ELF loader (`tools/tv/README.md`), writing
`build/tv-preview/index.html` and `report.json`. Those three also need
`libwebp-dev`, Mesa (`libegl-dev`, `libgl-dev`, `libegl-mesa0`,
`libgl1-mesa-dri`) and `python3-pil`.

`make eboot` needs clang-18 (with clang++-18), ninja, wget, unzip and
python3. On its first run `app/native-launcher/tools/setup-native-dependencies.sh`
fetches the PS5 payload SDK v0.42 that the toolchain pins (sha256-checked) and
zlib 1.3.2 into `app/native-launcher/.deps/`, builds the host tool
`ps5-native-tool`, and rebuilds `libc.prx` twice from source, checking both
copies against `app/native-launcher/runtime/libc.prx.sha256`. It does not use
`PS5_PAYLOAD_SDK`. `make eboot-raw` still builds the old freestanding launcher
(`app/eboot`), which needs no `sce_module/`.

`make launcher` still builds `app/dist/xeno-store-launcher.elf`, a launcher
you can send as a payload, and `make pkg` builds an optional homebrew folder.
Neither is part of a release.

## Credits and licences

The TV app is based on Orbit Store's TV app (saawant12/orbit-store-ps5) and
BlackBearReloaded's ps5-homebrew-ui (GPL-3.0). `app/tv/NOTICE` lists every
component it is built from and its licence: the Orbit Store 1.2.0 TV app and
its source revision, the ps5-homebrew-ui kit, SvenGDK's SharpProspero-derived
parts, the ps5-opengl SDK, the PS5 payload SDK, libwebp, the LLVM runtimes,
stb, Inter and the interface sounds. Each release attaches
`XENO-STORE-<version>-tv-sources.tar.gz`, the TV app's complete corresponding
source with every pinned upstream archive.

`app/native-launcher/` reuses the native app toolchain of
[ps5-native-app-boilerplate / ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui)
by **BlackBearReloaded** (GPL-3.0-or-later, commit
`4bd942579dd981b3df9c740438489ca6a614ddc0`), as carried by
[Orbit Store](https://github.com/saawant12/orbit-store-ps5) 1.2.0's TV app: the
compiler wrapper, the PIE linker script, `app_crt.cpp`, `ps5-native-tool` and
the clean-room `libc.prx` module. Its FSELF and module writers contain
portions derived from [SharpProspero](https://github.com/SvenGDK/SharpProspero)
by SvenGDK (GPL-3.0). These files keep their copyright and SPDX headers;
`app/native-launcher/NOTICE` lists every file and where it came from. The
pinned PS5 payload SDK (John Törnblom and contributors, GPL-3.0-or-later)
and zlib (zlib licence, host tool only) are fetched at build time and not
redistributed. `libc.prx` inside `PPSA99500.ffpkg` is GPL-3.0-or-later; its
source is `app/native-launcher/tooling/native/`.
