# Native launcher (eboot.bin of PPSA99500)

The Games-row app's `eboot.bin`, built as a native PS5 app with the
ps5-native-app-boilerplate toolchain by BlackBearReloaded (GPL-3.0-or-later;
see `NOTICE`). It replaces the freestanding launcher in `../eboot/`.

```
src/launcher.c          the launcher: open the running store, or start it through the ELF loader
build.sh                compile -> intermediate PIE -> PS5 app ELF -> FSELF; writes OUT/eboot.bin
                        and OUT/sce_module/libc.prx (default OUT: ../dist)
tooling/, tools/*.sh,   the toolchain, unmodified (compiler wrapper, app_crt.cpp, ps5-pie.ld,
runtime/                ps5-native-tool, the clean-room libc.prx builder and its digest)
tools/check-imports.py  fails the build if the app would import from a module other than
                        libSceLibcInternal, libkernel and libSceSystemService
tools/eboot-structure.py  read-only dump of an FSELF and its ELF (for comparing with a known app)
tests/run-host-test.py  builds src/launcher.c for Linux and runs it against a fake store and a
                        fake ELF loader
```

`make eboot` from the repository root runs `build.sh app/dist`. The first run
fetches the pinned PS5 payload SDK v0.42 and zlib into `.deps/` (sha256-checked),
builds `ps5-native-tool` into `build/host/` and rebuilds `runtime/libc.prx`
twice, byte for byte, checking it against `runtime/libc.prx.sha256`.

Needs clang-18 and clang++-18, ninja, wget, unzip, make and python3.

`eboot.bin` imports its C library calls from the system's libSceLibcInternal,
files and sockets from libkernel and `sceSystemServiceLoadExec` from
libSceSystemService. `sce_module/libc.prx` is the kit's clean-room `libc`
module (no Sony code), which the app loader expects in every app folder.

The app logs each step to the kernel log as `[XENO STORE] launcher: ...` and
ends with `sceSystemServiceLoadExec("exit")`; returning to `exit()` would end a
native title in the crash reporter.

The copied `tooling/README.md` and `runtime/README.md` link to `NATIVE_TOOLING.md` and `RUNTIME_SHIM.md`, which are not part of the published source this toolchain comes from. In this repository `make eboot` (`build.sh`) regenerates `libc.prx` and checks it against `runtime/libc.prx.sha256`.
