#!/usr/bin/env python3
# XENO STORE TV app - Makes assets/brand/xeno-badge.png from the home-screen icon.
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/make-brand.py [--source icon0.png] [--out xeno-badge.png] [--check]
#
# The badge is the 512 x 512 home-screen icon (homebrew/sce_sys/icon0.png in
# the repository root) cut to a circle, with a 1 px dark rim just inside its
# edge, as the web UI's .brand-badge draws it. The edge is anti-aliased by
# 8 x 8 supersampling; corners are fully transparent.
#
# The output is deterministic: plain Python (no imaging library), a fixed
# zlib level, and no timestamps or other metadata. --check regenerates the
# badge in memory and fails when the committed file's pixels differ.

import argparse
import struct
import sys
import zlib
from pathlib import Path

SIZE = 512
RADIUS = SIZE / 2.0
RIM = 1.0  # px, inside the edge
RIM_COLOUR = (6, 11, 28)  # --bg0
RIM_STRENGTH = 0.7
SAMPLES = 8

HERE = Path(__file__).resolve().parent
APP = HERE.parent
DEFAULT_SOURCE = APP.parent.parent / "homebrew" / "sce_sys" / "icon0.png"
DEFAULT_OUT = APP / "assets" / "brand" / "xeno-badge.png"
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def read_png(path):
    """Decodes an 8-bit, non-interlaced RGB or RGBA PNG to RGBA rows."""
    data = Path(path).read_bytes()
    if data[:8] != PNG_MAGIC:
        raise SystemExit(f"{path}: not a PNG file")
    pos = 8
    header = None
    idat = []
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    if header is None:
        raise SystemExit(f"{path}: no IHDR chunk")
    width, height, depth, colour, _, _, interlace = header
    if depth != 8 or colour not in (2, 6) or interlace != 0:
        raise SystemExit(f"{path}: only 8-bit non-interlaced RGB or RGBA PNGs are supported")
    channels = 3 if colour == 2 else 4
    raw = zlib.decompress(b"".join(idat))
    stride = width * channels
    rows = []
    previous = bytearray(stride)
    at = 0
    for _ in range(height):
        kind = raw[at]
        line = bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            corner = previous[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + left) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + up) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + ((left + up) >> 1)) & 0xFF
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                predictor = left if pa <= pb and pa <= pc else up if pb <= pc else corner
                line[i] = (line[i] + predictor) & 0xFF
            elif kind != 0:
                raise SystemExit(f"{path}: bad PNG filter {kind}")
        previous = line
        if channels == 3:
            rgba = bytearray(width * 4)
            for x in range(width):
                rgba[x * 4:x * 4 + 3] = line[x * 3:x * 3 + 3]
                rgba[x * 4 + 3] = 255
            rows.append(rgba)
        else:
            rows.append(line)
    return width, height, rows


def paeth(row, previous):
    """One RGBA row with PNG filter 4 (Paeth), its filter byte first."""
    out = bytearray(len(row) + 1)
    out[0] = 4
    for i in range(len(row)):
        left = row[i - 4] if i >= 4 else 0
        up = previous[i]
        corner = previous[i - 4] if i >= 4 else 0
        p = left + up - corner
        pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
        predictor = left if pa <= pb and pa <= pc else up if pb <= pc else corner
        out[i + 1] = (row[i] - predictor) & 0xFF
    return bytes(out)


def write_png(path, width, height, rows):
    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body +
                struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))

    raw = b"".join(paeth(row, rows[y - 1] if y > 0 else bytes(len(row)))
                   for y, row in enumerate(rows))
    png = (PNG_MAGIC + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(png)


def coverage(x, y, radius):
    """The share of pixel (x, y) inside the circle of this radius."""
    inside = 0
    for sy in range(SAMPLES):
        dy = y + (sy + 0.5) / SAMPLES - RADIUS
        for sx in range(SAMPLES):
            dx = x + (sx + 0.5) / SAMPLES - RADIUS
            if dx * dx + dy * dy <= radius * radius:
                inside += 1
    return inside / (SAMPLES * SAMPLES)


def badge(source):
    width, height, rows = read_png(source)
    if (width, height) != (SIZE, SIZE):
        raise SystemExit(f"{source}: expected {SIZE}x{SIZE}, got {width}x{height}")
    out = []
    edge = RADIUS - RIM - 1.5  # pixels fully inside this need no sampling
    for y in range(SIZE):
        row = bytearray(rows[y])
        for x in range(SIZE):
            cx = x + 0.5 - RADIUS
            cy = y + 0.5 - RADIUS
            if cx * cx + cy * cy <= edge * edge:
                continue
            outer = coverage(x, y, RADIUS)
            i = x * 4
            if outer <= 0.0:
                # Fully transparent; the rim's colour keeps filtered edges dark.
                row[i:i + 4] = bytes((*RIM_COLOUR, 0))
                continue
            inner = coverage(x, y, RADIUS - RIM)
            rim = (outer - inner) / outer * RIM_STRENGTH
            for c in range(3):
                row[i + c] = int(round(row[i + c] * (1.0 - rim) + RIM_COLOUR[c] * rim))
            row[i + 3] = int(round(row[i + 3] * outer))
        out.append(row)
    return out


def main():
    parser = argparse.ArgumentParser(
        description="Makes assets/brand/xeno-badge.png from the home-screen icon.")
    parser.add_argument("--source", default=str(DEFAULT_SOURCE),
                        help="512x512 icon (default: homebrew/sce_sys/icon0.png)")
    parser.add_argument("--out", default=str(DEFAULT_OUT), help="badge to write")
    parser.add_argument("--check", action="store_true",
                        help="fail when --out differs from a fresh badge, write nothing")
    args = parser.parse_args()
    rows = badge(args.source)
    if args.check:
        width, height, committed = read_png(args.out)
        if (width, height) != (SIZE, SIZE) or committed != rows:
            print(f"{args.out} is out of date: run tools/make-brand.py", file=sys.stderr)
            return 1
        print(f"{args.out}: up to date")
        return 0
    write_png(args.out, SIZE, SIZE, rows)
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
