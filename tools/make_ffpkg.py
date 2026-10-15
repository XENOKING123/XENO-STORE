#!/usr/bin/env python3
"""
make_ffpkg.py — builds an installable XENO STORE package (SPEC §12, the
"we have one too" .ffpkg/.pkg home-screen app).

It stages a PS5 application folder and turns it into the two Games-row forms:

    build/apppkg/src/PPSA99500/
        eboot.bin                 the launcher ('make eboot': app/native-launcher, a native app in a
                                  fake-signed SELF) — opens the running store, or hands the store
                                  below to the ELF loader
        sce_module/libc.prx       the native toolchain's clean-room libc module, which a native app
                                  folder carries; staged from app/dist/sce_module/ whenever eboot.bin
                                  is a native app
        xeno-store.elf            the store itself (serves the UI + opens the browser)
        sce_sys/param.json        app metadata (titleId PPSA99500, full application)
        sce_sys/icon0.png         512x512 XENO logo (from homebrew/sce_sys/icon0.png)
    -> makefs (UFS2)  ->  dist/PPSA99500.ffpkg  copy to /data/homebrew/, ShadowMountPlus mounts it
                                                (checked byte for byte by tools/ufs2_check.py)
    -> fpkg_build     ->  dist/XENO-STORE.pkg   optional (--fpkg / $XENO_FPKG_BUILD), not released

Usage:
    python3 tools/make_ffpkg.py [--elf PATH] [--eboot PATH] [--sce-module DIR] [--out PATH] [--image PATH]
                                [--version X.Y.Z] [--fpkg PATH] [--stage DIR] [--quiet]

The .pkg builder is found, in order: --fpkg, $XENO_FPKG_BUILD, `fpkg_build`
on PATH, then a few conventional locations; the image needs `makefs` (Debian /
Ubuntu package "makefs"). Each output is skipped with a message when its tool is
missing; with neither tool the script exits 3 (so `make ffpkg` can skip
gracefully). Exit 1 = staging/input error, 2 = a builder or the image check
failed, 0 = every output whose tool exists was produced and verified.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ELF_MAGIC = b"\x7fELF"
ET_SCE_DYNEXEC = 0xFE10   # e_type of a native PS5 app (app/native-launcher, ps5-native-tool link)

TITLE_ID = "PPSA99500"
CONTENT_ID = "IV0000-PPSA99500_00-XENOSTORE0000000"
TITLE_NAME = "XENO STORE"
EXIT_NO_TOOL = 3


def read_version(root):
    try:
        with open(os.path.join(root, "VERSION"), encoding="utf-8") as fh:
            return fh.read().strip() or "0.0.0"
    except OSError:
        return "0.0.0"


def content_version(version):
    """PS5 param.json contentVersion "NN.NNN.NNN": 1.2.0 -> 01.002.000, 1.2.1 -> 01.002.001."""
    parts = [p for p in version.split(".") if p.isdigit()]
    major = int(parts[0]) if parts else 1
    minor = int(parts[1]) if len(parts) > 1 else 0
    patch = int(parts[2]) if len(parts) > 2 else 0
    return "%02d.%03d.%03d" % (min(99, major), min(999, minor), min(999, patch))


def find_fpkg(explicit):
    """Locate the fpkg_build binary. Returns a path or None."""
    candidates = []
    if explicit:
        candidates.append(explicit)
    env = os.environ.get("XENO_FPKG_BUILD")
    if env:
        candidates.append(env)
    on_path = shutil.which("fpkg_build")
    if on_path:
        candidates.append(on_path)
    # Conventional checkout locations next to the repo.
    rel = os.path.join("ps5upload-up", "engine", "target", "release", "examples", "fpkg_build")
    for base in (ROOT, os.path.dirname(ROOT), os.path.join(ROOT, "tools"), os.path.join(ROOT, "ext")):
        candidates.append(os.path.join(base, rel))
        candidates.append(os.path.join(base, "ps5upload", "engine", "target", "release", "examples", "fpkg_build"))
    for c in candidates:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None


def self_elf_type(path):
    """e_type of the ELF inside a fake-signed SELF: 0xFE10 (ET_SCE_DYNEXEC) for a native app such as
    app/native-launcher, 2 (ET_EXEC) for the freestanding 'make eboot-raw' launcher, None if unreadable.
    The SELF header is 0x20 bytes, then one 0x20-byte entry per segment (u16 count at 0x18), then
    the ELF header."""
    try:
        with open(path, "rb") as fh:
            head = fh.read(0x20)
            if len(head) < 0x20:
                return None
            fh.seek(0x20 + 0x20 * int.from_bytes(head[0x18:0x1A], "little"))
            elf = fh.read(0x12)
    except OSError:
        return None
    if len(elf) < 0x12 or elf[:4] != ELF_MAGIC:
        return None
    return int.from_bytes(elf[0x10:0x12], "little")


def resolve_elf(elf_arg):
    if elf_arg:
        return elf_arg
    return os.path.join(ROOT, "xeno-store.elf")


def load_localized(root):
    """Carry the translated title names over from the homebrew param.json."""
    try:
        with open(os.path.join(root, "homebrew", "sce_sys", "param.json"), encoding="utf-8") as fh:
            lp = json.load(fh).get("localizedParameters")
            if isinstance(lp, dict) and lp.get("defaultLanguage"):
                return lp
    except (OSError, ValueError):
        pass
    return {"defaultLanguage": "en-US", "en-US": {"titleName": TITLE_NAME}}


def build_param(version, root):
    """The staged app's param.json, in the PS5 shape of a Games-row app that ShadowMountPlus registers
    (the layout of a known-working .ffpkg app: category 0 + badge 1 + launchActivity)."""
    return {
        "ageLevel": {"default": 0},
        "applicationCategoryType": 0,
        "applicationDrmType": "free",
        "attribute": 0,
        "attribute2": 0,
        "attribute3": 0,
        "conceptId": TITLE_ID[4:],
        "contentBadgeType": 1,
        "contentId": CONTENT_ID,
        "contentVersion": content_version(version),
        "downloadDataSize": 256,
        "gameIntent": {"permittedIntents": [{"intentType": "launchActivity"}]},
        "localizedParameters": load_localized(root),
        "masterVersion": "01.00",
        "requiredSystemSoftwareVersion": "0x0000000000000000",
        "sdkVersion": "0x0000000000000000",
        "titleId": TITLE_ID,
        "versionFileUri": "",
    }


def build_image(makefs, src_dir, out, say):
    """UFS2 image with the profile ShadowMountPlus documents (newfs -O 2 -b 65536 -f 65536 -m 0 -i 262144),
    sized to the allocated data + metadata + 8 MiB slack, rounded to 1 MiB: a read-only image needs no
    free space, and a small file copies over FTP and downloads quickly.

    No -S: makefs derives fs_fsbtodb from -S (log2(fsize / sector)), but the PS5's FreeBSD UFS driver
    always addresses blocks in 512-byte units (fsbtodb() in sys/ufs/ffs/fs.h; newfs forces DEV_BSIZE
    for the same reason). -S 4096 wrote fs_fsbtodb 4 instead of 7, so the console read every inode
    and directory 8x too early and ShadowMountPlus reported the image unreadable. The 4096-byte LVD
    sector ShadowMountPlus attaches with is unaffected: every UFS read is 4 KiB aligned (8 KiB
    superblock, 64 KiB blocks)."""
    blk = 65536
    data = dirs = 0
    for dp, dnames, fnames in os.walk(src_dir):
        dirs += 1
        for fn in fnames:
            data += (os.path.getsize(os.path.join(dp, fn)) + blk - 1) // blk * blk
    total = data + dirs * blk + 8 * 1024 * 1024
    mib = (total + (1 << 20) - 1) >> 20
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if os.path.exists(out):
        os.remove(out)
    cmd = [makefs, "-t", "ffs", "-B", "le", "-s", "%dm" % mib,
           "-o", "version=2,bsize=65536,fsize=65536,density=262144,minfree=0", out, src_dir]
    say("make_ffpkg: %s" % " ".join(cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0 or not os.path.isfile(out):
        sys.stderr.write(proc.stdout + proc.stderr)
        print("make_ffpkg: makefs failed (exit %d)" % proc.returncode, file=sys.stderr)
        return 2
    check = subprocess.run([sys.executable, os.path.join(HERE, "ufs2_check.py"), out, "--against", src_dir, "--quiet"],
                           capture_output=True, text=True)
    if check.returncode != 0:
        sys.stderr.write(check.stdout + check.stderr)
        print("make_ffpkg: the image does not match the staged app", file=sys.stderr)
        return 2
    say("image: %s (%d MiB, UFS2, checked against the staged app)" % (out, mib))
    return 0


def main():
    ap = argparse.ArgumentParser(description="build dist/PPSA99500.ffpkg (and dist/XENO-STORE.pkg) from the store ELF")
    ap.add_argument("--elf", default="", help="the store ELF shipped inside the app (default xeno-store.elf)")
    ap.add_argument("--eboot", default=os.path.join(ROOT, "app", "dist", "eboot.bin"), help="launcher SELF ('make eboot')")
    ap.add_argument("--sce-module", default=os.path.join(ROOT, "app", "dist", "sce_module"),
                    help="the native launcher's modules (libc.prx), staged as sce_module/")
    ap.add_argument("--out", default=os.path.join(ROOT, "dist", "XENO-STORE.pkg"), help="output .pkg path")
    ap.add_argument("--image", default=os.path.join(ROOT, "dist", TITLE_ID + ".ffpkg"), help="output UFS2 image path")
    ap.add_argument("--version", default="", help="override the VERSION file")
    ap.add_argument("--fpkg", default="", help="path to the fpkg_build binary")
    ap.add_argument("--stage", default=os.path.join(ROOT, "build", "apppkg"), help="staging directory")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    def say(msg):
        if not args.quiet:
            print(msg)

    # The .pkg is optional and not released: built only when asked for (--fpkg / $XENO_FPKG_BUILD).
    wants_pkg = bool(args.fpkg or os.environ.get("XENO_FPKG_BUILD"))
    fpkg = find_fpkg(args.fpkg) if wants_pkg else None
    makefs = shutil.which("makefs")
    if wants_pkg and not fpkg:
        print("make_ffpkg: fpkg_build not found at --fpkg / $XENO_FPKG_BUILD. Skipping the .pkg.", file=sys.stderr)
    if not makefs:
        print("make_ffpkg: makefs not found (apt install makefs). Skipping the .ffpkg image.", file=sys.stderr)
    if not fpkg and not makefs:
        return EXIT_NO_TOOL

    version = args.version.strip() or read_version(ROOT)
    elf = resolve_elf(args.elf)
    if not os.path.isfile(elf):
        print("make_ffpkg: ELF not found: %s (run 'make ps5' first)" % elf, file=sys.stderr)
        return 1
    with open(elf, "rb") as fh:
        if fh.read(4) != ELF_MAGIC:
            print("make_ffpkg: %s is not an ELF file" % elf, file=sys.stderr)
            return 1

    eboot = os.path.abspath(args.eboot)
    if not os.path.isfile(eboot):
        print("make_ffpkg: launcher not found: %s (run 'make eboot')" % eboot, file=sys.stderr)
        return 1
    with open(eboot, "rb") as fh:
        if fh.read(4) not in (b"\x4f\x15\x3d\x1d", b"\x54\x14\xf5\xee"):
            print("make_ffpkg: %s is not a SELF (build it with 'make eboot')" % eboot, file=sys.stderr)
            return 1
    # A native app folder carries sce_module/libc.prx (the libc module the app loader expects; the
    # toolchain and the known-working reference app always ship it): refuse to build without it.
    native = self_elf_type(eboot) == ET_SCE_DYNEXEC
    modules = []
    if native:
        sce_module = os.path.abspath(args.sce_module)
        if not os.path.isfile(os.path.join(sce_module, "libc.prx")):
            print("make_ffpkg: %s is a native app but %s/libc.prx is missing (run 'make eboot')"
                  % (eboot, sce_module), file=sys.stderr)
            return 1
        modules = sorted(os.path.join(sce_module, n) for n in os.listdir(sce_module)
                         if n.endswith(".prx") and os.path.isfile(os.path.join(sce_module, n)))

    icon = os.path.join(ROOT, "homebrew", "sce_sys", "icon0.png")
    if not os.path.isfile(icon):
        print("make_ffpkg: missing icon %s" % icon, file=sys.stderr)
        return 1

    # Stage the application folder.
    stage = os.path.abspath(args.stage)
    src_dir = os.path.join(stage, "src", TITLE_ID)
    out_dir = os.path.join(stage, "out")
    if os.path.isdir(src_dir):
        shutil.rmtree(src_dir)
    os.makedirs(os.path.join(src_dir, "sce_sys"), exist_ok=True)
    os.makedirs(out_dir, exist_ok=True)
    # fpkg_build refuses to overwrite; clear any package left by an earlier run.
    for stale in ("XENO-STORE.pkg", "XENO-STORE.pkg.partial"):
        try:
            os.remove(os.path.join(out_dir, stale))
        except OSError:
            pass

    shutil.copyfile(eboot, os.path.join(src_dir, "eboot.bin"))
    if modules:
        os.makedirs(os.path.join(src_dir, "sce_module"), exist_ok=True)
        for module in modules:
            shutil.copyfile(module, os.path.join(src_dir, "sce_module", os.path.basename(module)))
    shutil.copyfile(elf, os.path.join(src_dir, "xeno-store.elf"))
    shutil.copyfile(icon, os.path.join(src_dir, "sce_sys", "icon0.png"))
    param = build_param(version, ROOT)
    with open(os.path.join(src_dir, "sce_sys", "param.json"), "w", encoding="utf-8") as fh:
        json.dump(param, fh, indent=2, ensure_ascii=False)
        fh.write("\n")
    for dp, dirs, files in os.walk(src_dir):   # 0777 like ShadowMountPlus' mkufs2.sh
        for n in dirs + files:
            os.chmod(os.path.join(dp, n), 0o777)

    say("make_ffpkg: staged %s" % src_dir)
    say("  eboot.bin  launcher SELF (%d bytes, %s)" % (os.path.getsize(eboot), "native app" if native else "freestanding"))
    for module in modules:
        say("  sce_module/%s (%d bytes)" % (os.path.basename(module), os.path.getsize(module)))
    say("  store      %s (%d bytes)" % (os.path.basename(elf), os.path.getsize(elf)))
    say("  title      %s  id %s  contentVersion %s" % (TITLE_NAME, TITLE_ID, param["contentVersion"]))

    if makefs:
        rc = build_image(makefs, src_dir, os.path.abspath(args.image), say)
        if rc != 0:
            return rc
    if not fpkg:
        return 0

    # Build and verify with the Rust tool. It writes <name>.pkg into out_dir.
    cmd = [fpkg, src_dir, out_dir, "--content-id", CONTENT_ID, "--name", "XENO-STORE"]
    say("make_ffpkg: %s" % " ".join(cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if not args.quiet:
        tail = proc.stdout.strip().splitlines()[-3:] if proc.stdout else []
        for line in tail:
            print("  " + line)
    built = os.path.join(out_dir, "XENO-STORE.pkg")
    if proc.returncode != 0 or not os.path.isfile(built):
        sys.stderr.write(proc.stderr)
        print("make_ffpkg: fpkg_build failed (exit %d)" % proc.returncode, file=sys.stderr)
        return 2

    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    shutil.copyfile(built, out)
    say("package: %s (%d bytes, verified)" % (out, os.path.getsize(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
