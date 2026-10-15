#!/usr/bin/env bash
# XENO STORE TV app - Builds the host contract probe (build/host/tv-contract-probe).
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/build-contract-probe.sh
#
# Compiles tools/contract-probe.cpp with the app's own client and parser
# (src/orbit/http.cpp, json.cpp, api.cpp, model.cpp, and i18n.cpp and
# src/core/save_file.cpp, which they need) using the host toolchain of the PC
# preview (tools/preview.sh): clang++ -std=c++20 -O2 -Wall -Wextra -Werror.
# HOST_CXX overrides the compiler; BUILD_JOBS and USE_CCACHE as in
# tools/ninja-build.sh. The repository's tests/host/test_tvapi_probe.py runs it
# against the host backend (XENO_TV_PROBE=app/tv/build/host/tv-contract-probe).

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export USE_CCACHE=${USE_CCACHE:-0}
source "$root/tools/ninja-build.sh"
cxx=$(command -v "${HOST_CXX:-clang++}")
build="$root/build/host"
ninja_begin "$build/probe.ninja"

sources=(
    "$root/tools/contract-probe.cpp"
    "$root/src/orbit/http.cpp"
    "$root/src/orbit/json.cpp"
    "$root/src/orbit/api.cpp"
    "$root/src/orbit/model.cpp"
    "$root/src/orbit/i18n.cpp"
    "$root/src/core/save_file.cpp"
)

objects=()
for source in "${sources[@]}"; do
    relative=${source#"$root/"}
    object="$build/probe-obj/${relative//\//_}.o"
    ninja_inputs=("$source" "$cxx")
    ninja_edge CXX "$object" "${compiler_cache[@]}" "$cxx" -std=c++20 -O2 -Wall -Wextra -Werror \
        -I"$root/src" -MD -MF "$object.d" -c "$source" -o "$object"
    objects+=("$object")
done
ninja_inputs=("${objects[@]}" "$cxx")
ninja_edge LINK "$build/tv-contract-probe" "$cxx" "${objects[@]}" -lpthread -o "$build/tv-contract-probe"
if ! ninja_run >"$build/probe-build.log" 2>&1; then
    grep -E 'error|FAILED' -A6 "$build/probe-build.log" | head -120 >&2
    exit 1
fi
printf '%s\n' "contract probe: $build/tv-contract-probe"
