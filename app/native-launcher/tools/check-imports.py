#!/usr/bin/env python3
# XENO STORE - checks what a native app's intermediate PIE imports.
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Resolves every undefined dynamic symbol of llvm-pie.elf the way
# ps5-native-tool link does (tooling/native/sce_module_writer.cpp from
# ps5-native-app-boilerplate, Copyright (C) 2026 BlackBearReloaded,
# GPL-3.0-or-later): to the first DT_NEEDED stub, in DT_NEEDED order, that
# exports it, and prints each import with its NID. It fails when a module
# outside --allow is needed: a system library a native title does not load
# turns each call into a jump to address 0 (the kit's runtime_shims.c notes
# this for libScePosixForWebKit), and when the app would export a symbol.
# Read-only; never executes its inputs.
#
# usage: python3 -I check-imports.py PIE STUB_DIR --allow mod[,mod...]

import base64
import hashlib
import os
import struct
import sys

NID_SUFFIX = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")


def nid(name):
    digest = hashlib.sha1(name.encode() + NID_SUFFIX).digest()
    value = struct.unpack("<Q", digest[:8])[0]
    return base64.b64encode(struct.pack(">Q", value)).decode()[:11].replace("/", "-")


class Elf:
    def __init__(self, path):
        with open(path, "rb") as handle:
            self.data = handle.read()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
            raise ValueError("%s: not a little-endian ELF64 file" % path)
        self.path = path
        (self.shoff,) = struct.unpack_from("<Q", d, 0x28)
        self.shentsize, self.shnum = struct.unpack_from("<HH", d, 0x3A)
        self.sections = []
        for i in range(self.shnum):
            at = self.shoff + i * self.shentsize
            name, kind, _flags, _addr, offset, size, link, _info, _align, entsize = \
                struct.unpack_from("<IIQQQQIIQQ", d, at)
            self.sections.append((kind, offset, size, link, entsize))

    def section(self, kind):
        for index, section in enumerate(self.sections):
            if section[0] == kind:
                return index, section
        raise ValueError("%s: no section of type %d" % (self.path, kind))

    def string(self, section_index, offset):
        _kind, start, size, _link, _entsize = self.sections[section_index]
        end = self.data.index(b"\0", start + offset)
        return self.data[start + offset:end].decode()

    def dynamic(self):
        _index, (_kind, offset, size, link, _entsize) = self.section(6)   # SHT_DYNAMIC
        entries = []
        for at in range(offset, offset + size, 16):
            tag, value = struct.unpack_from("<qQ", self.data, at)
            if tag == 0:
                break
            entries.append((tag, value))
        return link, entries

    def symbols(self):
        """(name, defined, binding) for each dynamic symbol after the null one."""
        _index, (_kind, offset, size, link, entsize) = self.section(11)   # SHT_DYNSYM
        out = []
        for at in range(offset + entsize, offset + size, entsize):
            name, info, _other, shndx, _value, _size = struct.unpack_from("<IBBHQQ", self.data, at)
            out.append((self.string(link, name), shndx != 0, info >> 4))
        return out


def stub_table(directory):
    stubs = {}
    for name in sorted(os.listdir(directory)):
        if not name.endswith(".so"):
            continue
        elf = Elf(os.path.join(directory, name))
        link, entries = elf.dynamic()
        soname = next((elf.string(link, v) for t, v in entries if t == 14), None)
        if soname is None:
            continue
        exports = {n for n, defined, binding in elf.symbols() if defined and binding in (1, 2) and n}
        stubs.setdefault(soname, exports)
    return stubs


def module_of(soname):
    return soname.split(".")[0]


def main(argv):
    if len(argv) != 5 or argv[3] != "--allow":
        sys.stderr.write("usage: check-imports.py PIE STUB_DIR --allow mod[,mod...]\n")
        return 2
    pie_path, stub_dir, _flag, allow_list = argv[1:]
    allowed = {m for m in allow_list.split(",") if m}

    pie = Elf(pie_path)
    stubs = stub_table(stub_dir)
    link, entries = pie.dynamic()
    needed = [pie.string(link, v) for t, v in entries if t == 1]
    failures = []
    for soname in needed:
        if module_of(soname) not in allowed:
            failures.append("needs %s, which is not in the allowed set %s" % (soname, sorted(allowed)))
        if soname not in stubs:
            failures.append("no stub in %s has SONAME %s" % (stub_dir, soname))

    by_module = {}
    for name, defined, _binding in pie.symbols():
        if defined:
            failures.append("exports %s (the converter accepts imports only)" % name)
            continue
        provider = next((s for s in needed if name in stubs.get(s, ())), None)
        if provider is None:
            failures.append("no needed stub exports %s" % name)
            continue
        by_module.setdefault(module_of(provider), []).append(name)

    for module in [module_of(s) for s in needed]:
        names = sorted(by_module.get(module, []))
        print("%-20s %2d: %s" % (module, len(names), ", ".join("%s (%s)" % (n, nid(n)) for n in names)))
    for failure in failures:
        print("check-imports: FAIL: " + failure, file=sys.stderr)
    if failures:
        return 1
    print("check-imports: ok (%d imports from %s)" % (sum(len(v) for v in by_module.values()),
                                                       ", ".join(module_of(s) for s in needed)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
