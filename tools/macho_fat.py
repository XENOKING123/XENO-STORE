#!/usr/bin/env python3
"""
macho_fat.py — joins thin 64-bit Mach-O executables (e.g. the darwin/arm64 and
darwin/amd64 builds of XENO STORE Desktop) into one universal binary, the
format `lipo -create` writes, so macOS users get a single download that runs
on Apple silicon and Intel Macs.

    python3 tools/macho_fat.py OUT IN1 IN2 [...]

Each slice is copied unchanged (its code signature stays valid) at a 16 KiB
aligned offset; the fat header is big-endian.
"""
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
ALIGN = 14  # 2^14 = 16 KiB, what lipo uses for arm64/x86_64


def main(argv):
    if len(argv) < 4:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    out, inputs = argv[1], argv[2:]
    slices = []
    for path in inputs:
        with open(path, "rb") as fh:
            data = fh.read()
        if len(data) < 32:
            print("macho_fat: %s is too small" % path, file=sys.stderr)
            return 1
        magic, cputype, cpusub = struct.unpack("<III", data[:12])
        if magic != MH_MAGIC_64:
            print("macho_fat: %s is not a 64-bit little-endian Mach-O" % path, file=sys.stderr)
            return 1
        if any(s[0] == cputype for s in slices):
            print("macho_fat: two slices for CPU type 0x%x" % cputype, file=sys.stderr)
            return 1
        slices.append((cputype, cpusub & 0x00FFFFFF, data))
    step = 1 << ALIGN
    entries, layout = [], []
    offset = 8 + 20 * len(slices)
    for cputype, cpusub, data in slices:
        offset = (offset + step - 1) // step * step
        entries.append(struct.pack(">IIIII", cputype, cpusub, offset, len(data), ALIGN))
        layout.append((offset, data))
        offset += len(data)
    blob = bytearray(struct.pack(">II", FAT_MAGIC, len(slices)) + b"".join(entries))
    for off, data in layout:
        blob += b"\0" * (off - len(blob))
        blob += data
    with open(out, "wb") as fh:
        fh.write(blob)
    print("macho_fat: %s (%d slices, %d bytes)" % (out, len(slices), len(blob)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
