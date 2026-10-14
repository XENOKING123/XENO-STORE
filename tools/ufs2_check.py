#!/usr/bin/env python3
"""
ufs2_check.py — reads a UFS2 image (a ShadowMountPlus .ffpkg) without mounting it and checks it.

    python3 tools/ufs2_check.py IMAGE [--against DIR]

Prints the superblock profile and every file in the image. With --against, every regular file
under DIR must be in the image at the same path with identical bytes, and the image must hold
sce_sys/param.json and eboot.bin at its root (the layout ShadowMountPlus requires). Exit 0 = OK.
Only what an app image needs is supported: direct, single and double indirect blocks.
"""
import argparse
import os
import struct
import sys

SBLOCK = 65536
UFS2_MAGIC = 0x19540119
DINODE2 = 256
IFMT, IFDIR, IFREG = 0o170000, 0o040000, 0o100000


class Ufs2:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.f.seek(SBLOCK)
        sb = self.f.read(1376 + 8)
        magic = struct.unpack_from("<I", sb, 1372)[0]
        if magic != UFS2_MAGIC:
            raise ValueError("not a UFS2 image (magic 0x%08x)" % magic)
        g = lambda off: struct.unpack_from("<i", sb, off)[0]
        self.iblkno, self.bsize, self.fsize, self.frag = g(16), g(48), g(52), g(56)
        self.ncg = struct.unpack_from("<I", sb, 44)[0]
        self.inopb = struct.unpack_from("<I", sb, 120)[0]
        self.ipg = struct.unpack_from("<I", sb, 184)[0]
        self.fpg = g(188)
        self.minfree = g(60)
        self.nindir = self.bsize // 8

    def read_at(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def inode(self, ino):
        cg, idx = divmod(ino, self.ipg)
        fsba = cg * self.fpg + self.iblkno + (idx // self.inopb) * self.frag
        raw = self.read_at(fsba * self.fsize + (idx % self.inopb) * DINODE2, DINODE2)
        mode = struct.unpack_from("<H", raw, 0)[0]
        size = struct.unpack_from("<Q", raw, 16)[0]
        db = list(struct.unpack_from("<12q", raw, 112))
        ib = list(struct.unpack_from("<3q", raw, 208))
        return mode, size, db, ib

    def _ptrs(self, blk):
        return struct.unpack_from("<%dq" % self.nindir, self.read_at(blk * self.fsize, self.bsize))

    def blocks(self, db, ib, size):
        need = (size + self.bsize - 1) // self.bsize
        out = [b for b in db][:need]
        if len(out) < need and ib[0]:
            out += list(self._ptrs(ib[0]))[:need - len(out)]
        if len(out) < need and ib[1]:
            for l1 in self._ptrs(ib[1]):
                if len(out) >= need or not l1:
                    break
                out += list(self._ptrs(l1))[:need - len(out)]
        if len(out) < need:
            raise ValueError("file needs triple indirect blocks (unsupported)")
        return out

    def content(self, ino):
        mode, size, db, ib = self.inode(ino)
        data = bytearray()
        for b in self.blocks(db, ib, size):
            n = min(self.bsize, size - len(data))
            data += self.read_at(b * self.fsize, n) if b else bytes(n)
        return mode, bytes(data)

    def listdir(self, ino):
        mode, data = self.content(ino)
        if mode & IFMT != IFDIR:
            raise ValueError("inode %d is not a directory" % ino)
        out, off = [], 0
        while off + 8 <= len(data):
            d_ino, reclen, _typ, namlen = struct.unpack_from("<IHBB", data, off)
            if reclen == 0:
                break
            if d_ino:
                name = data[off + 8:off + 8 + namlen].decode("utf-8", "replace")
                if name not in (".", ".."):
                    out.append((name, d_ino))
            off += reclen
        return out

    def walk(self, ino=2, prefix=""):
        for name, child in self.listdir(ino):
            mode = self.inode(child)[0]
            path = prefix + name
            if mode & IFMT == IFDIR:
                yield path + "/", child, mode
                yield from self.walk(child, path + "/")
            else:
                yield path, child, mode


def main():
    ap = argparse.ArgumentParser(description="check a UFS2 (.ffpkg) image")
    ap.add_argument("image")
    ap.add_argument("--against", default="", help="staging directory the image must match")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    try:
        fs = Ufs2(a.image)
        files = {p: (ino, mode) for p, ino, mode in fs.walk()}
    except (OSError, ValueError, struct.error) as e:
        print("ufs2_check: %s: %s" % (a.image, e), file=sys.stderr)
        return 1
    if not a.quiet:
        print("ufs2_check: %s — UFS2, bsize %d, fsize %d, %d cylinder groups, minfree %d%%"
              % (a.image, fs.bsize, fs.fsize, fs.ncg, fs.minfree))
        for p in sorted(files):
            print("  %s %s" % (oct(files[p][1] & 0o7777), p))
    errors = []
    for need in ("eboot.bin", "sce_sys/param.json"):
        if need not in files:
            errors.append("missing %s at the image root" % need)
    if a.against:
        for dp, _dirs, fnames in os.walk(a.against):
            for fn in fnames:
                full = os.path.join(dp, fn)
                rel = os.path.relpath(full, a.against).replace(os.sep, "/")
                if rel not in files:
                    errors.append("missing %s" % rel)
                    continue
                with open(full, "rb") as fh:
                    want = fh.read()
                if fs.content(files[rel][0])[1] != want:
                    errors.append("content differs: %s" % rel)
    for e in errors:
        print("ufs2_check: " + e, file=sys.stderr)
    if not errors and not a.quiet:
        print("ufs2_check: OK (%d entries%s)" % (len(files), ", identical to " + a.against if a.against else ""))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
