#!/usr/bin/env bash
# XENO STORE - builds the Games-row app's eboot.bin as a native PS5 app.
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the four stages of tools/build.sh from ps5-native-app-boilerplate
# (Copyright (C) 2026 BlackBearReloaded, GPL-3.0-or-later), with its loader
# constants, for src/launcher.c:
#   1. tooling/prospero-clang18 compiles src/*.c and tooling/native/app_crt.cpp
#   2. prospero-lld links an intermediate PIE (tooling/native/ps5-pie.ld)
#   3. ps5-native-tool link converts it to the PS5 app layout
#      (ET_SCE_DYNEXEC, PT_DYNAMIC, PROCPARAM, TLS, EH frame, SCE_VERSION)
#   4. ps5-native-tool self --sign wraps it in a development FSELF
# then writes OUT/eboot.bin and OUT/sce_module/libc.prx: the kit's clean-room
# libc module, the loader companion an app folder has to carry (the app's own
# C library calls go to the system's libSceLibcInternal), rebuilt from
# tooling/native and checked against runtime/libc.prx.sha256.
# tools/check-imports.py fails the build when the app would import from a
# module outside the three below.
#
# The pinned PS5 payload SDK v0.42 and zlib (host tool only) are fetched into
# .deps/native by tools/setup-native-dependencies.sh; PS5_PAYLOAD_SDK from the
# environment is not used.
#
# usage: build.sh [OUT]          default OUT: ../dist (app/dist in the repo)
# needs: clang-18, clang++-18, ninja, wget, unzip, sha256sum, make, python3
# env:   PS5_CLANG (clang-18), CXX (clang++-18), BUILD_JOBS, USE_CCACHE (0)

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
out=${1:-$root/../dist}
mkdir -p "$out"
out=$(cd -- "$out" && pwd)
export USE_CCACHE=${USE_CCACHE:-0}

# Loader/container constants, unchanged from the kit's tools/build.sh.
module_sdk=0x02000009
companion_sdk=0x08050001
fself_magic=0x1D3D154F
# The only modules the launcher may import from. The known-working reference app imports all
# three; libSceSystemService gives sceSystemServiceLoadExec, the clean exit.
allowed_modules=libSceLibcInternal,libkernel,libSceSystemService

echo "==> [deps] pinned PS5 payload SDK v0.42 + zlib"
bash "$root/tools/setup-native-dependencies.sh" >/dev/null
echo "==> [host] ps5-native-tool, libc-builder"
bash "$root/tools/build-host-tools.sh" >/dev/null
if [[ ! -f $root/runtime/libc.prx ]] ||
    ! (cd "$root/runtime" && sha256sum --check --strict --quiet libc.prx.sha256 2>/dev/null); then
    echo "==> [runtime] libc.prx (two byte-identical builds, checked against the release digest)"
    bash "$root/tools/rebuild-libc.sh" >/dev/null
fi
(cd "$root/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)

sdk_root="$root/.deps/native/ps5-payload-sdk"
native="$root/tooling/native"
tool="$root/build/host/ps5-native-tool"
compiler=$(command -v "${PS5_CLANG:-clang-18}")
cc() { PS5_PAYLOAD_SDK="$sdk_root" PS5_CLANG="$compiler" USE_CCACHE=0 \
    sh "$root/tooling/prospero-clang18" "$@"; }

work="$root/build/launcher"
rm -rf -- "$work"
mkdir -p "$work/obj"
objects=()
echo "==> [cc] src/*.c, app_crt.cpp"
for source in "$root"/src/*.c; do
    object="$work/obj/$(basename "$source").o"
    cc -std=gnu11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
        -c "$source" -o "$object"
    objects+=("$object")
done
cc -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti \
    -ffunction-sections -fdata-sections -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"

echo "==> [ld] intermediate PIE"
"$sdk_root/bin/prospero-lld" -T "$native/ps5-pie.ld" --eh-frame-hdr \
    --version-script "$native/app-symbols.map" \
    -e _start -o "$work/llvm-pie.elf" "$work/obj/app_crt.o" "${objects[@]}" \
    --as-needed "$sdk_root"/target/lib/*.so

echo "==> [check] imports"
python3 -I "$root/tools/check-imports.py" "$work/llvm-pie.elf" "$sdk_root/target/lib" \
    --allow "$allowed_modules"

echo "==> [link] PS5 app ELF"
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
    --stub-dir "$sdk_root/target/lib" --module-sdk "$module_sdk" \
    --companion-sdk "$companion_sdk" --file-name eboot.elf

echo "==> [self] eboot.bin"
mkdir -p "$out/sce_module"
"$tool" self --sign --in "$work/eboot.elf" --out "$out/eboot.bin" --magic "$fself_magic"
cp "$root/runtime/libc.prx" "$out/sce_module/libc.prx"
"$tool" self --inspect --file "$out/eboot.bin"
printf 'eboot: %s (%s bytes)\nruntime: %s (%s bytes)\n' \
    "$out/eboot.bin" "$(stat -c %s "$out/eboot.bin")" \
    "$out/sce_module/libc.prx" "$(stat -c %s "$out/sce_module/libc.prx")"
