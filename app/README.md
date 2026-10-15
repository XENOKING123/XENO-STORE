# XENO STORE — the Games-row app

XENO STORE runs as a **payload** (`xeno-store.elf`, loaded by the console's
jailbreak), so its downloads keep running after you leave the store. This
directory builds the app that puts XENO STORE on the **Games** row and opens it
with one press: `dist/PPSA99500.ffpkg`.

## How people install it

1. Send `xeno-store.elf` to the ELF loader (port 9021) or Payload Manager once.
   The store opens on the TV.
2. **Settings → Updates → Install the XENO STORE app.** The store downloads
   `PPSA99500.ffpkg` from the latest release into `/data/homebrew/`, checks it
   against the release's `SHA256SUMS` and asks ShadowMountPlus to rescan
   (`src/app_install.c`). Copying `PPSA99500.ffpkg` to `/data/homebrew/` by
   hand works too.
3. From then on, open **XENO STORE** from its tile under Games.

**Needs:** kstuff (fake-signed apps) and ShadowMountPlus (mounts the image and
adds the tile), plus an ELF loader on `127.0.0.1:9021` so the app can start the
store.

## What is inside `PPSA99500.ffpkg`

`tools/make_ffpkg.py` stages the app folder and writes a UFS2 image with
makefs (the profile ShadowMountPlus documents), then checks every file in it
with `tools/ufs2_check.py`:

```
build/apppkg/src/PPSA99500/
  eboot.bin            the launcher (app/native-launcher, a native app in a fake-signed SELF)
  sce_module/libc.prx  the toolchain's clean-room libc module (a native app folder carries it)
  xeno-store.elf       the store the launcher starts
  sce_sys/param.json   titleId PPSA99500, contentId IV0000-PPSA99500_00-XENOSTORE0000000
  sce_sys/icon0.png    512x512 XENO logo
```

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
make eboot        # app/dist/eboot.bin + app/dist/sce_module/libc.prx (the native launcher)
make ffpkg        # dist/PPSA99500.ffpkg (needs makefs)
```

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
