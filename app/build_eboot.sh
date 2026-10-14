#!/usr/bin/env bash
# build_eboot.sh — builds eboot.bin for the Games-row app (PPSA99500): the freestanding launcher in
# app/eboot/eboot_main.c, linked static with the SDK's app layout (eboot.x) and wrapped as a
# fake-signed SELF by make_fself.py, with the parameters the ps5-payload-sdk install_app sample uses.
#
#   PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk app/build_eboot.sh [OUTPUT]     (default app/dist/eboot.bin)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
out="${1:-$here/dist/eboot.bin}"
: "${PS5_PAYLOAD_SDK:?build_eboot: PS5_PAYLOAD_SDK is not set — export it to the ps5-payload-dev SDK}"
cc="$PS5_PAYLOAD_SDK/bin/prospero-clang"
ld="$PS5_PAYLOAD_SDK/bin/prospero-lld"
[ -x "$cc" ] && [ -x "$ld" ] || { echo "build_eboot: prospero-clang / prospero-lld not found in $PS5_PAYLOAD_SDK/bin" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
"$cc" -std=gnu11 -Wall -Wextra -Werror -O2 -ffreestanding -fno-builtin -fno-stack-protector -c \
  -o "$work/eboot.o" "$here/eboot/eboot_main.c"
"$ld" --static -T "$here/eboot/eboot.x" -o "$work/eboot.elf" "$work/eboot.o"

AUTHID=0x3800000000000022
AUTHINFO="00 00 00 00 00 00 00 00 00 00 00 00 00 1C 00 40 00 FF 00 00 00 00 \
00 80 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 80 \
00 40 00 40 00 00 00 00 00 00 00 80 00 00 00 00 00 00 00 08 00 40 \
FF FF 00 00 00 F0 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 \
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 \
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 \
00 00 00 00"
SDKVER=0x07590001

mkdir -p "$(dirname "$out")"
python3 "$here/eboot/make_fself.py" --ptype fake --paid "$AUTHID" --auth-info "$AUTHINFO" \
  --app-version "$SDKVER" --fw-version "$SDKVER" "$work/eboot.elf" "$out"
echo "eboot: $out ($(wc -c < "$out") bytes, magic $(head -c4 "$out" | od -An -tx1 | tr -d ' '))"
