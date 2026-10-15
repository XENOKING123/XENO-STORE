#!/usr/bin/env python3
# XENO STORE - read-only structure dump of a PS5 FSELF and its embedded ELF.
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Container geometry follows the FSELF writer in tooling/native/self_container.cpp
# (ps5-native-app-boilerplate, Copyright (C) 2026 BlackBearReloaded,
# GPL-3.0-or-later). Never executes or modifies its inputs.
#
# usage: python3 -I eboot-structure.py FSELF [ELF-extracted-from-it] [--symbols N]

import struct
import sys

PT = {1: "LOAD", 2: "DYNAMIC", 3: "INTERP", 4: "NOTE", 6: "PHDR", 7: "TLS",
      0x6474E550: "GNU_EH_FRAME", 0x6474E551: "GNU_STACK", 0x6474E552: "GNU_RELRO",
      0x61000000: "SCE_DYNLIBDATA", 0x61000001: "SCE_PROCPARAM",
      0x61000002: "SCE_MODULE_PARAM", 0x61000010: "SCE_RELRO",
      0x6FFFFF00: "SCE_COMMENT", 0x6FFFFF01: "SCE_VERSION"}
ET = {2: "ET_EXEC", 3: "ET_DYN", 0xFE00: "ET_SCE_EXEC", 0xFE0C: "ET_SCE_STUBLIB",
      0xFE10: "ET_SCE_DYNEXEC", 0xFE18: "ET_SCE_DYNAMIC"}
DT = {0: "NULL", 1: "NEEDED", 2: "PLTRELSZ", 3: "PLTGOT", 4: "HASH", 5: "STRTAB", 6: "SYMTAB",
      7: "RELA", 8: "RELASZ", 9: "RELAENT", 10: "STRSZ", 11: "SYMENT", 12: "INIT", 13: "FINI",
      14: "SONAME", 15: "RPATH", 16: "SYMBOLIC", 17: "REL", 20: "PLTREL", 21: "DEBUG",
      22: "TEXTREL", 23: "JMPREL", 24: "BIND_NOW", 25: "INIT_ARRAY", 26: "FINI_ARRAY",
      27: "INIT_ARRAYSZ", 28: "FINI_ARRAYSZ", 30: "FLAGS", 32: "PREINIT_ARRAY",
      33: "PREINIT_ARRAYSZ", 0x6FFFFFF9: "RELACOUNT", 0x6FFFFEF5: "GNU_HASH",
      0x6FFFFFFB: "FLAGS_1",
      0x61000005: "SCE_IDTABENTSZ", 0x61000007: "SCE_FINGERPRINT",
      0x61000009: "SCE_ORIGINAL_FILENAME(ps4)", 0x6100000D: "SCE_MODULE_INFO(ps4)",
      0x6100000F: "SCE_NEEDED_MODULE(ps4)", 0x61000011: "SCE_MODULE_ATTR",
      0x61000013: "SCE_EXPORT_LIB(ps4)", 0x61000015: "SCE_IMPORT_LIB(ps4)",
      0x61000017: "SCE_EXPORT_LIB_ATTR", 0x61000019: "SCE_IMPORT_LIB_ATTR",
      0x61000025: "SCE_HASH", 0x61000027: "SCE_PLTGOT", 0x61000029: "SCE_JMPREL",
      0x6100002B: "SCE_PLTREL", 0x6100002D: "SCE_PLTRELSZ", 0x6100002F: "SCE_RELA",
      0x61000031: "SCE_RELASZ", 0x61000033: "SCE_RELAENT", 0x61000035: "SCE_STRTAB",
      0x61000037: "SCE_STRSZ", 0x61000039: "SCE_SYMTAB", 0x6100003B: "SCE_SYMENT",
      0x6100003D: "SCE_HASHSZ", 0x6100003F: "SCE_SYMTABSZ",
      0x61000041: "SCE_ORIGINAL_FILENAME", 0x61000043: "SCE_MODULE_INFO",
      0x61000045: "SCE_NEEDED_MODULE", 0x61000047: "SCE_EXPORT_LIB",
      0x61000049: "SCE_IMPORT_LIB"}
STRING_TAGS = {1, 14, 0x61000041, 0x61000009}
PACKED_STRING_TAGS = {0x61000043, 0x61000045, 0x61000047, 0x61000049,
                      0x6100000D, 0x6100000F, 0x61000013, 0x61000015}


def u(fmt, data, at=0):
    return struct.unpack_from("<" + fmt, data, at)


def flags_text(f):
    return "".join(c if f & b else "-" for c, b in (("R", 4), ("W", 2), ("X", 1)))


def phdrs(elf, base=0):
    phoff, = u("Q", elf, base + 0x20)
    phnum, = u("H", elf, base + 0x38)
    out = []
    for i in range(phnum):
        at = base + phoff + i * 0x38
        p_type, p_flags, p_off, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = u("IIQQQQQQ", elf, at)
        out.append(dict(i=i, type=p_type, flags=p_flags, off=p_off, vaddr=p_vaddr,
                        paddr=p_paddr, filesz=p_filesz, memsz=p_memsz, align=p_align))
    return out


def print_elf_header(elf, base=0):
    ident = elf[base:base + 16]
    e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, e_ehsize, \
        e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = u("HHIQQQIHHHHHH", elf, base + 16)
    print("  e_ident: class=%d data=%d version=%d OSABI=%d ABIVERSION=%d" %
          (ident[4], ident[5], ident[6], ident[7], ident[8]))
    print("  e_type=0x%04x (%s) e_machine=0x%x e_version=%d e_entry=0x%x" %
          (e_type, ET.get(e_type, "?"), e_machine, e_version, e_entry))
    print("  e_phoff=0x%x e_shoff=0x%x e_flags=0x%x e_ehsize=0x%x e_phentsize=0x%x e_phnum=%d "
          "e_shentsize=0x%x e_shnum=%d e_shstrndx=%d" %
          (e_phoff, e_shoff, e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx))


def print_phdrs(headers):
    print("  %-3s %-15s %-3s %-10s %-12s %-10s %-10s %-7s" %
          ("#", "type", "flg", "offset", "vaddr", "filesz", "memsz", "align"))
    for p in headers:
        print("  %-3d %-15s %-3s 0x%08x 0x%010x 0x%08x 0x%08x 0x%x" %
              (p["i"], PT.get(p["type"], "0x%x" % p["type"]), flags_text(p["flags"]),
               p["off"], p["vaddr"], p["filesz"], p["memsz"], p["align"]))


def dump_container(data):
    magic, = u("I", data, 0)
    print("== FSELF container")
    print("  magic=0x%08x  bytes[4:8]=%s  program_type(u32@0x08)=0x%x" %
          (magic, data[4:8].hex(), u("I", data, 8)[0]))
    header_size, meta_size = u("HH", data, 0x0c)
    file_size, = u("Q", data, 0x10)
    count, flags = u("HH", data, 0x18)
    print("  header_size=0x%x meta_size=0x%x declared_file_size=0x%x (actual 0x%x) "
          "segments=%d flags=0x%x" % (header_size, meta_size, file_size, len(data), count, flags))
    for i in range(count):
        f, off, fs, ms = u("QQQQ", data, 0x20 + i * 0x20)
        bits = []
        for name, b in (("ordered", 1), ("encrypted", 2), ("signed", 4), ("deflated", 8),
                        ("blocked", 0x800), ("digests", 0x10000)):
            if f & b:
                bits.append(name)
        print("  seg[%2d] flags=0x%-8x id=%-3d %-34s off=0x%08x filesz=0x%08x memsz=0x%08x" %
              (i, f, (f >> 20) & 0xffff, ",".join(bits), off, fs, ms))
    elf_at = 0x20 + count * 0x20
    print("== embedded ELF header (at 0x%x)" % elf_at)
    print_elf_header(data, elf_at)
    headers = phdrs(data, elf_at)
    print("== program headers (embedded copy)")
    print_phdrs(headers)
    ext = (elf_at + 0x40 + len(headers) * 0x38 + 15) & ~15
    authid, ptype, appver, fwver = u("QQQQ", data, ext)
    digest = data[ext + 0x20:ext + 0x40]
    print("== extended info (at 0x%x)" % ext)
    print("  paid/authid=0x%016x program_type=0x%x app_version=0x%016x fw_version=0x%016x" %
          (authid, ptype, appver, fwver))
    print("  digest=%s" % digest.hex())
    control = data[ext + 0x40:ext + 0x70]
    print("  control region(0x30)=%s" % control.hex())
    meta = data[header_size:header_size + meta_size]
    nonzero = sum(1 for b in meta if b)
    footer = header_size + count * 0x50
    print("== metadata: %d bytes, %d non-zero; footer@0x%x u32@+0x30=0x%x; signature area %s" %
          (len(meta), nonzero, footer, u("I", data, footer + 0x30)[0],
           "all zero" if not any(data[footer + 0x50:header_size + meta_size]) else "non-zero"))
    return headers


class Elf:
    def __init__(self, data):
        self.d = data
        self.ph = phdrs(data)

    def v2o(self, va):
        for p in self.ph:
            if p["type"] == 1 and p["vaddr"] <= va < p["vaddr"] + max(p["filesz"], 1):
                return p["off"] + va - p["vaddr"]
        return None

    def cstr(self, off):
        end = self.d.index(b"\0", off)
        return self.d[off:end].decode("latin-1")


def dump_elf(data, symbol_limit):
    e = Elf(data)
    print("== extracted ELF (%d bytes)" % len(data))
    print_elf_header(data)
    dyn = None
    for p in e.ph:
        if p["type"] == 2:
            dyn = p
    entries = []
    if dyn:
        for at in range(dyn["off"], dyn["off"] + dyn["filesz"], 16):
            tag, val = u("qQ", data, at)
            entries.append((tag, val))
    tags = {}
    for tag, val in entries:
        tags.setdefault(tag, []).append(val)
    strtab = tags.get(5, [None])[0]
    stroff = e.v2o(strtab) if strtab is not None else None

    def s(off):
        return e.cstr(stroff + off) if stroff is not None else "?"

    print("== DYNAMIC (%d entries)" % len(entries))
    for tag, val in entries:
        name = DT.get(tag & 0xffffffffffffffff if tag >= 0 else tag, "0x%x" % tag)
        extra = ""
        if tag in STRING_TAGS:
            extra = '  "%s"' % s(val & 0xffffffff)
        elif tag in PACKED_STRING_TAGS:
            extra = '  name="%s" id=%d ver=0x%x' % (s(val & 0xffffffff), (val >> 48) & 0xffff,
                                                    (val >> 32) & 0xffff)
        print("  %-26s 0x%x%s" % (name, val, extra))

    relas = {}
    rela, relasz = tags.get(7, [None])[0], tags.get(8, [0])[0]
    if rela is not None:
        ro = e.v2o(rela)
        for at in range(ro, ro + relasz, 24):
            r_off, r_info, r_add = u("QQq", data, at)
            relas[r_off] = (r_info & 0xffffffff, r_info >> 32, r_add)
    jmprel, pltsz = tags.get(23, [None])[0], tags.get(2, [0])[0]
    print("  relocations: RELA %d entries (RELATIVE %d), JMPREL %d entries" %
          (len(relas), sum(1 for r in relas.values() if r[0] == 8), pltsz // 24))

    symtab = tags.get(6, [None])[0]
    symsz = tags.get(0x6100003F, [0])[0]
    if symtab is not None and symsz:
        so = e.v2o(symtab)
        names = []
        for at in range(so, so + symsz, 24):
            st_name, st_info, st_other, st_shndx, st_value, st_size = u("IBBHQQ", data, at)
            names.append(s(st_name))
        names = [n for n in names if n]
        print("  dynamic symbols: %d named; first %d: %s" %
              (len(names), symbol_limit, ", ".join(names[:symbol_limit])))

    for p in e.ph:
        body = data[p["off"]:p["off"] + p["filesz"]]
        if p["type"] == 0x61000001:
            print("== PROCPARAM (vaddr 0x%x, %d bytes)" % (p["vaddr"], len(body)))
            size, = u("Q", body, 0)
            magic = body[8:12]
            version, w10, w14 = u("III", body, 0x0c)
            print("  size=0x%x magic=%r version=%d u32@0x10=0x%08x u32@0x14=0x%08x" %
                  (size, magic, version, w10, w14))
            for off in range(0x18, min(len(body), 0x58), 8):
                val, = u("Q", body, off)
                r = relas.get(p["vaddr"] + off)
                ptr = ""
                if r and r[0] == 8:
                    target = r[2]
                    to = e.v2o(target)
                    bsize = u("Q", data, to)[0] if to is not None else 0
                    ptr = " -> RELATIVE 0x%x (block size field 0x%x)" % (target, bsize)
                if val or ptr:
                    print("  +0x%02x = 0x%x%s" % (off, val, ptr))
            if len(body) >= 0x60:
                print("  u32@0x58=0x%x u32@0x5c=0x%x" % u("II", body, 0x58))
            print("  raw: %s" % body.hex())
        elif p["type"] == 0x6FFFFF00:
            print("== SCE_COMMENT: %s" % body.hex())
            print("   text: %r" % body)
        elif p["type"] == 0x6FFFFF01:
            print("== SCE_VERSION (%d bytes)" % len(body))
            at = 0
            while at + 4 <= len(body):
                ln, = u("H", body, at + 2)
                rec = body[at + 4:at + 4 + ln]
                if not rec:
                    break
                name_end = rec.find(b":")
                print("   [%r] %s" % (rec[1:name_end + 1], rec[name_end + 1:].hex()))
                at += 4 + ln
        elif p["type"] == 4:
            namesz, descsz, ntype = u("III", body, 0)
            name = body[12:12 + namesz]
            desc = body[12 + ((namesz + 3) & ~3):][:descsz]
            print("== NOTE @0x%x: name=%r type=%d desc=%s" % (p["off"], name, ntype, desc.hex()))
        elif p["type"] == 7:
            print("== TLS: vaddr=0x%x filesz=0x%x memsz=0x%x align=0x%x" %
                  (p["vaddr"], p["filesz"], p["memsz"], p["align"]))
        elif p["type"] == 0x6474E550 and body:
            ver, enc_ptr, enc_cnt, enc_tab = body[0], body[1], body[2], body[3]
            print("== GNU_EH_FRAME: version=%d enc=0x%x/0x%x/0x%x fde_count=%d" %
                  (ver, enc_ptr, enc_cnt, enc_tab, u("I", body, 8)[0] if len(body) >= 12 else -1))


def main():
    args = [a for a in sys.argv[1:]]
    symbol_limit = 12
    if "--symbols" in args:
        i = args.index("--symbols")
        symbol_limit = int(args[i + 1])
        del args[i:i + 2]
    if not args:
        raise SystemExit(__doc__ or "usage: eboot-structure.py FSELF [ELF]")
    with open(args[0], "rb") as stream:
        data = stream.read()
    print("#### %s (%d bytes)" % (args[0], len(data)))
    dump_container(data)
    if len(args) > 1:
        with open(args[1], "rb") as stream:
            dump_elf(stream.read(), symbol_limit)


if __name__ == "__main__":
    main()
