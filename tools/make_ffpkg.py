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

With --app-dir (the TV app folder from 'make tv', app/tv/dist/PPSA99500) the image holds the
native TV app instead of the launcher (docs: PORT-SPEC §3.5; the app is GPL-3.0-or-later, see
app/tv/NOTICE):

    build/apppkg/src/PPSA99500/
        eboot.bin                 the TV app (SELF, ET_SCE_DYNEXEC, so sce_module/ is required)
        sce_module/libc.prx       from the app folder; its SHA-256 must be LIBC_PRX_SHA256
        assets/                   fonts/, audio/, i18n/, brand/ from the app folder, as they are
        licenses/                 app/tv/NOTICE and app/tv/licenses/ (GPL-3.0 and the linked parts)
        xeno/xeno-store.elf       --elf: the copy the TV app hands to the ELF loader
        xeno/version.txt          VERSION + "\\n"; the ELF must carry exactly one
                                  XENO-STORE/<version> marker, equal to it
        sce_sys/param.json        the app folder's param.json (amm, kernel, pubtools kept) with
                                  titleId, conceptId, contentId, contentVersion and
                                  localizedParameters forced to ours and pubtools.creationDate
                                  from $SOURCE_DATE_EPOCH (or now)
        sce_sys/icon0.png         homebrew/sce_sys/icon0.png
    There is no root xeno-store.elf in this mode. After makefs the image is checked again: the
    UFS2 magic at 65536+1372 (what the store's TV app installer checks), xeno/version.txt read
    back from the image, the image's entries equal to the staged folder's, and its size at most
    APP_MAX_BYTES.

Usage:
    python3 tools/make_ffpkg.py [--elf PATH] [--eboot PATH] [--sce-module DIR] [--out PATH] [--image PATH]
                                [--version X.Y.Z] [--fpkg PATH] [--stage DIR] [--quiet]
    python3 tools/make_ffpkg.py --app-dir app/tv/dist/PPSA99500 [--tv-src app/tv] [--elf PATH] ...

The .pkg builder is found, in order: --fpkg, $XENO_FPKG_BUILD, `fpkg_build`
on PATH, then a few conventional locations; the image needs `makefs` (Debian /
Ubuntu package "makefs"). Each output is skipped with a message when its tool is
missing; with neither tool the script exits 3 (so `make ffpkg` can skip
gracefully). Exit 1 = staging/input error, 2 = a builder or the image check
failed, 0 = every output whose tool exists was produced and verified.
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ELF_MAGIC = b"\x7fELF"
ET_SCE_DYNEXEC = 0xFE10   # e_type of a native PS5 app (app/native-launcher, ps5-native-tool link)

TITLE_ID = "PPSA99500"
CONTENT_ID = "IV0000-PPSA99500_00-XENOSTORE0000000"
TITLE_NAME = "XENO STORE"
EXIT_NO_TOOL = 3

# --app-dir (the TV app). The clean-room libc module every app folder of the kit carries
# (app/tv/runtime/libc.prx.sha256, app/native-launcher/runtime/libc.prx.sha256).
LIBC_PRX_SHA256 = "e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036"
# eboot.bin of the unmodified Orbit Store 1.2.0 import (app/tv before the port, M0): it talks to
# port 34177 and starts /app0/orbit/orbit_store.elf, so it must never ship as PPSA99500.
ORBIT_M0_EBOOT_SHA256 = "65807c313631ce997d2e70ecc85b48afbcafa20ed3e52e7d798e29cc4b067951"
APP_MAX_BYTES = 512 * 1024 * 1024        # src/app_install.c: the console refuses bigger images
UFS2_MAGIC_OFFSET = 65536 + 1372         # superblock at 64 KiB, fs_magic at 1372 within it
UFS2_MAGIC = b"\x19\x01\x54\x19"         # 0x19540119 little-endian
UFS2_DENSITY = 262144                    # bytes per inode in the makefs profile below
VERSION_MARKER = re.compile(rb"XENO-STORE/[0-9][0-9A-Za-z.+-]*")   # the ELF's user agent
APP_DIR_ENTRIES = ("eboot.bin", "sce_module", "sce_sys", "assets")
SCE_SYS_EXTRAS = ("pic0.dds", "pic1.dds", "snd0.at9")   # copied as the kit's build.sh does
SELF_MAGICS = (b"\x4f\x15\x3d\x1d", b"\x54\x14\xf5\xee")


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


def build_image(makefs, src_dir, out, say, min_mib=0):
    """UFS2 image with the profile ShadowMountPlus documents (newfs -O 2 -b 65536 -f 65536 -m 0 -i 262144),
    sized to the allocated data + metadata + 8 MiB slack, rounded to 1 MiB: a read-only image needs no
    free space, and a small file copies over FTP and downloads quickly. min_mib (--app-dir) raises the
    size when the folder has more entries than the inode density leaves room for.

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
    mib = max(mib, min_mib)
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


# ---- --app-dir: the TV app ------------------------------------------------------------------


def fail(msg):
    print("make_ffpkg: " + msg, file=sys.stderr)
    return 1


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def version_markers(elf):
    """Every XENO-STORE/<version> string in the ELF (the HTTP user agent carries exactly one)."""
    with open(elf, "rb") as fh:
        return [m.decode("ascii") for m in VERSION_MARKER.findall(fh.read())]


def creation_date():
    """pubtools.creationDate: $SOURCE_DATE_EPOCH (reproducible builds) or now, UTC."""
    raw = os.environ.get("SOURCE_DATE_EPOCH", "").strip()
    if raw:
        if not raw.isdigit():
            raise ValueError("SOURCE_DATE_EPOCH must be a whole number of seconds, not %r" % raw)
        stamp = int(raw)
    else:
        stamp = int(time.time())
    return datetime.datetime.fromtimestamp(stamp, datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S")


def merge_param(base, version, root):
    """The TV app's own param.json (amm, kernel and pubtools are what its GL heap needs) with our ids
    and version forced over it. Key order is kept; keys the base lacks are added at the end."""
    param = dict(base)
    param["titleId"] = TITLE_ID
    param["conceptId"] = TITLE_ID[4:]
    param["contentId"] = CONTENT_ID
    param["contentVersion"] = content_version(version)
    param["localizedParameters"] = load_localized(root)
    pubtools = dict(param.get("pubtools") or {})
    pubtools["creationDate"] = creation_date()
    param["pubtools"] = pubtools
    return param


def symlinks_under(top):
    found = []
    for dp, dnames, fnames in os.walk(top):
        for n in dnames + fnames:
            if os.path.islink(os.path.join(dp, n)):
                found.append(os.path.relpath(os.path.join(dp, n), top))
    return found


def stage_tv_app(args, version, elf, say):
    """Checks the TV app folder and the extra inputs, then stages the image folder.
    Returns (exit code, src_dir, out_dir)."""
    app = os.path.abspath(args.app_dir)
    tv_src = os.path.abspath(args.tv_src)
    if args.eboot is not None or args.sce_module is not None:
        return fail("--eboot and --sce-module build the launcher image; with --app-dir the app folder "
                    "provides eboot.bin and sce_module/"), None, None
    if not os.path.isdir(app):
        return fail("app folder not found: %s (run 'make tv')" % app), None, None
    entries = sorted(os.listdir(app))
    unexpected = [n for n in entries if n not in APP_DIR_ENTRIES]
    if unexpected:
        return fail("unexpected entries in the app folder %s: %s (it must hold only %s; build it with "
                    "'make tv', which leaves the payload out: the image carries xeno/ itself)"
                    % (app, ", ".join(unexpected), ", ".join(APP_DIR_ENTRIES))), None, None
    links = symlinks_under(app)
    if links:
        return fail("symbolic links in the app folder %s: %s" % (app, ", ".join(sorted(links)))), None, None

    eboot = os.path.join(app, "eboot.bin")
    if not os.path.isfile(eboot):
        return fail("%s is missing (run 'make tv')" % eboot), None, None
    with open(eboot, "rb") as fh:
        if fh.read(4) not in SELF_MAGICS:
            return fail("%s is not a SELF" % eboot), None, None
    if self_elf_type(eboot) != ET_SCE_DYNEXEC:
        return fail("%s is not a native app (ET_SCE_DYNEXEC)" % eboot), None, None
    if sha256_file(eboot) == ORBIT_M0_EBOOT_SHA256:
        return fail("%s is the unmodified Orbit Store import, which talks to port 34177 and starts "
                    "/app0/orbit/orbit_store.elf: build the ported app with 'make tv'" % eboot), None, None

    sce_module = os.path.join(app, "sce_module")
    libc = os.path.join(sce_module, "libc.prx")
    if not os.path.isfile(libc):
        return fail("%s is missing: a native app folder carries sce_module/libc.prx" % libc), None, None
    modules = []
    for n in sorted(os.listdir(sce_module)):
        full = os.path.join(sce_module, n)
        if not (n.endswith(".prx") and os.path.isfile(full)):
            return fail("unexpected entry in %s: %s (only .prx modules)" % (sce_module, n)), None, None
        modules.append(full)
    pinned = os.path.join(tv_src, "runtime", "libc.prx.sha256")
    if os.path.isfile(pinned):
        with open(pinned, encoding="utf-8") as fh:
            words = fh.read().split()
        if not words or words[0].lower() != LIBC_PRX_SHA256:
            return fail("%s pins %s, this script pins %s: update both together"
                        % (pinned, words[0] if words else "nothing", LIBC_PRX_SHA256)), None, None
    digest = sha256_file(libc)
    if digest != LIBC_PRX_SHA256:
        return fail("%s has SHA-256 %s, expected %s (the kit's clean-room libc module)"
                    % (libc, digest, LIBC_PRX_SHA256)), None, None

    assets = os.path.join(app, "assets")
    if not os.path.isdir(os.path.join(assets, "fonts")):
        return fail("%s/fonts is missing: the app cannot draw text without its font atlases" % assets), None, None

    base_path = os.path.join(app, "sce_sys", "param.json")
    try:
        with open(base_path, encoding="utf-8") as fh:
            base = json.load(fh)
    except (OSError, ValueError) as e:
        return fail("cannot read the app's param.json %s: %s" % (base_path, e)), None, None
    if not isinstance(base, dict) or not isinstance(base.get("amm"), dict) or not isinstance(base.get("kernel"), dict):
        return fail("%s must be an object with the app's amm and kernel memory settings" % base_path), None, None
    # 'make tv' copies app/tv/sce_sys/param.json, which 'make tv-check' holds to VERSION, so the folder
    # says which store it was built against. merge_param() below overwrites these fields: checking
    # them first keeps an app folder left from another version (or another app) out of this image.
    want_cv = content_version(version)
    if base.get("titleId") != TITLE_ID or base.get("contentVersion") != want_cv:
        return fail("%s is the app folder of %s %s, but VERSION %s needs %s %s: rebuild it with 'make tv' "
                    "(or use the tv-app CI artifact built for this version)"
                    % (app, base.get("titleId"), base.get("contentVersion"), version, TITLE_ID, want_cv)), None, None
    extras = []
    for n in sorted(os.listdir(os.path.join(app, "sce_sys"))):
        if n in SCE_SYS_EXTRAS:
            extras.append(os.path.join(app, "sce_sys", n))
        elif n not in ("param.json", "icon0.png"):
            return fail("unexpected entry in %s/sce_sys: %s" % (app, n)), None, None

    notice = os.path.join(tv_src, "NOTICE")
    licenses = os.path.join(tv_src, "licenses")
    if not os.path.isfile(notice) or not os.path.isfile(os.path.join(licenses, "GPL-3.0.txt")):
        return fail("%s and %s/GPL-3.0.txt are required: the image carries the app's licences" % (notice, licenses)), None, None
    if os.path.exists(os.path.join(licenses, "NOTICE")):
        return fail("%s/NOTICE would collide with %s in the image's licenses/" % (licenses, notice)), None, None
    links = symlinks_under(licenses)
    if links:
        return fail("symbolic links in %s: %s" % (licenses, ", ".join(sorted(links)))), None, None

    icon = os.path.join(ROOT, "homebrew", "sce_sys", "icon0.png")
    if not os.path.isfile(icon):
        return fail("missing icon %s" % icon), None, None

    markers = version_markers(elf)
    want = "XENO-STORE/" + version
    if markers != [want]:
        return fail("%s must carry exactly one version marker %s; found %s (rebuild it with 'make ps5' "
                    "after changing VERSION)" % (elf, want, ", ".join(markers) or "none")), None, None
    try:
        param = merge_param(base, version, ROOT)
    except ValueError as e:
        return fail(str(e)), None, None

    src_dir, out_dir = prepare_stage(args.stage)
    shutil.copyfile(eboot, os.path.join(src_dir, "eboot.bin"))
    os.makedirs(os.path.join(src_dir, "sce_module"), exist_ok=True)
    for module in modules:
        shutil.copyfile(module, os.path.join(src_dir, "sce_module", os.path.basename(module)))
    shutil.copytree(assets, os.path.join(src_dir, "assets"), copy_function=shutil.copyfile)
    shutil.copytree(licenses, os.path.join(src_dir, "licenses"), copy_function=shutil.copyfile)
    shutil.copyfile(notice, os.path.join(src_dir, "licenses", "NOTICE"))
    os.makedirs(os.path.join(src_dir, "xeno"), exist_ok=True)
    shutil.copyfile(elf, os.path.join(src_dir, "xeno", "xeno-store.elf"))
    with open(os.path.join(src_dir, "xeno", "version.txt"), "w", encoding="ascii", newline="\n") as fh:
        fh.write(version + "\n")
    shutil.copyfile(icon, os.path.join(src_dir, "sce_sys", "icon0.png"))
    for extra in extras:
        shutil.copyfile(extra, os.path.join(src_dir, "sce_sys", os.path.basename(extra)))
    with open(os.path.join(src_dir, "sce_sys", "param.json"), "w", encoding="utf-8") as fh:
        json.dump(param, fh, indent=2, ensure_ascii=False)
        fh.write("\n")
    chmod_all(src_dir)

    say("make_ffpkg: staged %s (TV app from %s)" % (src_dir, app))
    say("  eboot.bin  TV app SELF (%d bytes, native app)" % os.path.getsize(eboot))
    for module in modules:
        say("  sce_module/%s (%d bytes, sha256 %s)" % (os.path.basename(module), os.path.getsize(module),
                                                       sha256_file(module)[:16] + "..."))
    say("  assets/    %d files" % sum(len(f) for _d, _s, f in os.walk(os.path.join(src_dir, "assets"))))
    say("  licenses/  NOTICE + %d licence files" % (sum(len(f) for _d, _s, f in os.walk(os.path.join(src_dir, "licenses"))) - 1))
    say("  xeno/      xeno-store.elf (%d bytes, %s) + version.txt" % (os.path.getsize(elf), want))
    say("  title      %s  id %s  contentVersion %s  created %s"
        % (TITLE_NAME, TITLE_ID, param["contentVersion"], param["pubtools"]["creationDate"]))
    return 0, src_dir, out_dir


def load_ufs2_check():
    """tools/ufs2_check.py as a module, by path (works under python3 -I too)."""
    spec = importlib.util.spec_from_file_location("xeno_ufs2_check", os.path.join(HERE, "ufs2_check.py"))
    mod = importlib.util.module_from_spec(spec)
    sys.dont_write_bytecode = True   # leave no tools/__pycache__ behind
    spec.loader.exec_module(mod)
    return mod


def staged_entries(src_dir):
    out = {}
    for dp, dnames, fnames in os.walk(src_dir):
        for n in dnames:
            out[os.path.relpath(os.path.join(dp, n), src_dir).replace(os.sep, "/") + "/"] = None
        for n in fnames:
            out[os.path.relpath(os.path.join(dp, n), src_dir).replace(os.sep, "/")] = os.path.join(dp, n)
    return out


def check_tv_image(image, src_dir, version, say):
    """The self-checks of --app-dir mode, after build_image's ufs2_check --against passed."""
    errors = []
    size = os.path.getsize(image)
    if size > APP_MAX_BYTES:
        errors.append("the image is %d bytes, more than the %d the console installs" % (size, APP_MAX_BYTES))
    with open(image, "rb") as fh:
        fh.seek(UFS2_MAGIC_OFFSET)
        magic = fh.read(4)
    if magic != UFS2_MAGIC:
        errors.append("no UFS2 magic 19 01 54 19 at offset %d (found %s)" % (UFS2_MAGIC_OFFSET, magic.hex(" ")))
    try:
        ufs2 = load_ufs2_check()
        fs = ufs2.Ufs2(image)
        files = {p: (ino, mode) for p, ino, mode in fs.walk()}
        stored = fs.content(files["xeno/version.txt"][0])[1] if "xeno/version.txt" in files else None
    except (OSError, ValueError, KeyError) as e:
        errors.append("cannot read the image back: %s" % e)
        files, stored = {}, None
    if files:
        if stored != (version + "\n").encode("ascii"):
            errors.append("xeno/version.txt in the image is %r, expected %r" % (stored, version + "\n"))
        staged = staged_entries(src_dir)
        missing = sorted(set(staged) - set(files))
        extra = sorted(set(files) - set(staged))
        if missing:
            errors.append("not in the image: %s" % ", ".join(missing))
        if extra:
            errors.append("in the image but not staged: %s" % ", ".join(extra))
        modes = sorted(p for p, (_ino, mode) in files.items() if mode & 0o7777 != 0o777)
        if modes:
            errors.append("not mode 0777 in the image: %s" % ", ".join(modes))
        if "xeno-store.elf" in files:
            errors.append("the image has a root xeno-store.elf (the TV app reads xeno/xeno-store.elf)")
    for e in errors:
        print("make_ffpkg: image check: " + e, file=sys.stderr)
    if errors:
        return 2
    say("image check: UFS2 magic at %d, xeno/version.txt = %s, %d entries identical to the staged folder, "
        "%d bytes (limit %d)" % (UFS2_MAGIC_OFFSET, version, len(files), size, APP_MAX_BYTES))
    return 0


# ---- shared ---------------------------------------------------------------------------------


def prepare_stage(stage_arg):
    """A fresh build/apppkg/src/PPSA99500 (with sce_sys/) and build/apppkg/out."""
    stage = os.path.abspath(stage_arg)
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
    return src_dir, out_dir


def chmod_all(src_dir):
    for dp, dirs, files in os.walk(src_dir):   # 0777 like ShadowMountPlus' mkufs2.sh
        for n in dirs + files:
            os.chmod(os.path.join(dp, n), 0o777)


def stage_launcher(args, version, elf, say):
    """The native launcher image (no --app-dir). Returns (exit code, src_dir, out_dir)."""
    eboot = os.path.abspath(args.eboot if args.eboot is not None else os.path.join(ROOT, "app", "dist", "eboot.bin"))
    if not os.path.isfile(eboot):
        print("make_ffpkg: launcher not found: %s (run 'make eboot')" % eboot, file=sys.stderr)
        return 1, None, None
    with open(eboot, "rb") as fh:
        if fh.read(4) not in SELF_MAGICS:
            print("make_ffpkg: %s is not a SELF (build it with 'make eboot')" % eboot, file=sys.stderr)
            return 1, None, None
    # A native app folder carries sce_module/libc.prx (the libc module the app loader expects; the
    # toolchain and the known-working reference app always ship it): refuse to build without it.
    native = self_elf_type(eboot) == ET_SCE_DYNEXEC
    modules = []
    if native:
        sce_module = os.path.abspath(args.sce_module if args.sce_module is not None
                                     else os.path.join(ROOT, "app", "dist", "sce_module"))
        if not os.path.isfile(os.path.join(sce_module, "libc.prx")):
            print("make_ffpkg: %s is a native app but %s/libc.prx is missing (run 'make eboot')"
                  % (eboot, sce_module), file=sys.stderr)
            return 1, None, None
        modules = sorted(os.path.join(sce_module, n) for n in os.listdir(sce_module)
                         if n.endswith(".prx") and os.path.isfile(os.path.join(sce_module, n)))

    icon = os.path.join(ROOT, "homebrew", "sce_sys", "icon0.png")
    if not os.path.isfile(icon):
        print("make_ffpkg: missing icon %s" % icon, file=sys.stderr)
        return 1, None, None

    # Stage the application folder.
    src_dir, out_dir = prepare_stage(args.stage)
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
    chmod_all(src_dir)

    say("make_ffpkg: staged %s" % src_dir)
    say("  eboot.bin  launcher SELF (%d bytes, %s)" % (os.path.getsize(eboot), "native app" if native else "freestanding"))
    for module in modules:
        say("  sce_module/%s (%d bytes)" % (os.path.basename(module), os.path.getsize(module)))
    say("  store      %s (%d bytes)" % (os.path.basename(elf), os.path.getsize(elf)))
    say("  title      %s  id %s  contentVersion %s" % (TITLE_NAME, TITLE_ID, param["contentVersion"]))
    return 0, src_dir, out_dir


def main():
    ap = argparse.ArgumentParser(description="build dist/PPSA99500.ffpkg (and dist/XENO-STORE.pkg) from the store ELF")
    ap.add_argument("--elf", default="", help="the store ELF shipped inside the app (default xeno-store.elf)")
    ap.add_argument("--app-dir", default="", help="the TV app folder ('make tv': app/tv/dist/PPSA99500); "
                    "the image then holds the TV app with the ELF in xeno/ instead of the launcher")
    ap.add_argument("--tv-src", default=os.path.join(ROOT, "app", "tv"),
                    help="with --app-dir: where NOTICE, licenses/ and runtime/libc.prx.sha256 are (default app/tv)")
    ap.add_argument("--eboot", default=None, help="launcher SELF ('make eboot'; default app/dist/eboot.bin)")
    ap.add_argument("--sce-module", default=None,
                    help="the native launcher's modules (libc.prx), staged as sce_module/ (default app/dist/sce_module)")
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

    if args.app_dir:
        if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
            return fail("version %r must be plain N.N.N: the TV app's starter compares xeno/version.txt "
                        "that way" % version)
        rc, src_dir, out_dir = stage_tv_app(args, version, elf, say)
    else:
        rc, src_dir, out_dir = stage_launcher(args, version, elf, say)
    if rc != 0:
        return rc

    if makefs:
        min_mib = 0
        if args.app_dir:   # at least one inode per entry (+ root, lost+found and slack) at -o density
            min_mib = ((len(staged_entries(src_dir)) + 16) * UFS2_DENSITY + (1 << 20) - 1) >> 20
        rc = build_image(makefs, src_dir, os.path.abspath(args.image), say, min_mib)
        if rc != 0:
            return rc
        if args.app_dir:
            rc = check_tv_image(os.path.abspath(args.image), src_dir, version, say)
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
