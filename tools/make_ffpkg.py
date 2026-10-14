#!/usr/bin/env python3
"""
make_ffpkg.py — builds an installable XENO STORE package (SPEC §12, the
"we have one too" .ffpkg/.pkg home-screen app).

It stages a PS5 application folder and turns it into the two Games-row forms:

    build/apppkg/src/PPSA99500/
        eboot.bin                 the launcher (app/build_eboot.sh: a fake-signed SELF) — opens
                                  the running store, or hands the store below to the ELF loader
        xeno-store-browser.elf    the store itself (serves the UI + opens the browser)
        sce_sys/param.json        app metadata (titleId PPSA99500, full application)
        sce_sys/icon0.png         512x512 XENO logo (from homebrew/sce_sys/icon0.png)
    -> makefs (UFS2)  ->  dist/PPSA99500.ffpkg  copy to /data/homebrew/, ShadowMountPlus mounts it
                                                (checked byte for byte by tools/ufs2_check.py)
    -> fpkg_build     ->  dist/XENO-STORE.pkg   install like any fake package (verified)

Usage:
    python3 tools/make_ffpkg.py [--elf PATH] [--eboot PATH] [--out PATH] [--image PATH]
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
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ELF_MAGIC = b"\x7fELF"

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
    """param.json contentVersion "MM.mm": 1.0.0 -> 01.00, 1.2.0 -> 01.20."""
    parts = [p for p in version.split(".") if p.isdigit()]
    major = int(parts[0]) if parts else 1
    minor = int(parts[1]) if len(parts) > 1 else 0
    patch = int(parts[2]) if len(parts) > 2 else 0
    minor2 = min(99, minor * 10 + patch) if minor < 10 else min(99, minor)
    return "%02d.%02d" % (min(99, major), minor2)


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


def resolve_elf(elf_arg):
    if elf_arg:
        return elf_arg
    browser = os.path.join(ROOT, "xeno-store-browser.elf")
    if os.path.isfile(browser):
        return browser
    server = os.path.join(ROOT, "xeno-store.elf")
    if os.path.isfile(server):
        print("make_ffpkg: xeno-store-browser.elf missing, using xeno-store.elf", file=sys.stderr)
        return server
    return browser


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
    """The staged app's param.json — a full, free, DRM-less application."""
    return {
        "applicationCategoryType": 33554432,
        "titleId": TITLE_ID,
        "contentId": CONTENT_ID,
        "contentVersion": content_version(version),
        "masterVersion": "01.00",
        "localizedParameters": load_localized(root),
        "kernelDigest": "ASSET",
        "pubtoolInfo": {"creationDate": time.strftime("%Y-%m-%d", time.gmtime())},
        "applicationDrmType": "free",
        "attribute": 0,
        "downloadDataSize": 0,
        "parentalLevel": 0,
        "sdkVersion": "7.59.0.0",
    }


def build_image(makefs, src_dir, out, say):
    """UFS2 image with the profile ShadowMountPlus documents (newfs -O 2 -b 65536 -f 65536 -m 0 -S 4096
    -i 262144) and its sizing: allocated data + metadata + 64 MiB slack, rounded to 1 MiB."""
    blk = 65536
    data = dirs = 0
    for dp, dnames, fnames in os.walk(src_dir):
        dirs += 1
        for fn in fnames:
            data += (os.path.getsize(os.path.join(dp, fn)) + blk - 1) // blk * blk
    total = data + dirs * blk + 64 * 1024 * 1024 + 8 * 1024 * 1024
    mib = (total + (1 << 20) - 1) >> 20
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if os.path.exists(out):
        os.remove(out)
    cmd = [makefs, "-t", "ffs", "-B", "le", "-S", "4096", "-s", "%dm" % mib,
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
    ap = argparse.ArgumentParser(description="build dist/XENO-STORE.pkg from the browser ELF")
    ap.add_argument("--elf", default="", help="the store ELF shipped inside the app (default xeno-store-browser.elf)")
    ap.add_argument("--eboot", default=os.path.join(ROOT, "app", "dist", "eboot.bin"), help="launcher SELF (app/build_eboot.sh)")
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

    fpkg = find_fpkg(args.fpkg)
    makefs = shutil.which("makefs")
    if not fpkg:
        print("make_ffpkg: fpkg_build not found. Set $XENO_FPKG_BUILD or pass --fpkg; build it with\n"
              "  cargo build --release -p ps5upload-fpkg --example fpkg_build\n"
              "in the ps5upload engine checkout. Skipping the .pkg.", file=sys.stderr)
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
        print("make_ffpkg: launcher not found: %s (run app/build_eboot.sh)" % eboot, file=sys.stderr)
        return 1
    with open(eboot, "rb") as fh:
        if fh.read(4) not in (b"\x4f\x15\x3d\x1d", b"\x54\x14\xf5\xee"):
            print("make_ffpkg: %s is not a SELF (build it with app/build_eboot.sh)" % eboot, file=sys.stderr)
            return 1

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
    shutil.copyfile(elf, os.path.join(src_dir, "xeno-store-browser.elf"))
    shutil.copyfile(icon, os.path.join(src_dir, "sce_sys", "icon0.png"))
    param = build_param(version, ROOT)
    with open(os.path.join(src_dir, "sce_sys", "param.json"), "w", encoding="utf-8") as fh:
        json.dump(param, fh, indent=2, ensure_ascii=False)
        fh.write("\n")
    for dp, dirs, files in os.walk(src_dir):   # 0777 like ShadowMountPlus' mkufs2.sh
        for n in dirs + files:
            os.chmod(os.path.join(dp, n), 0o777)

    say("make_ffpkg: staged %s" % src_dir)
    say("  eboot.bin  launcher SELF (%d bytes)" % os.path.getsize(eboot))
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
