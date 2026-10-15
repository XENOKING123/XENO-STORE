#!/usr/bin/env python3
# XENO STORE - the TV app's complete corresponding source for a release.
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
make_source_bundle.py — builds XENO-STORE-<version>-tv-sources.tar.gz (PORT-SPEC §3.2).

The TV app in PPSA99500.ffpkg (app/tv: Orbit Store 1.2.0's TV app on BlackBearReloaded's
ps5-homebrew-ui kit, GPL-3.0-or-later) links GPL-3.0 components statically, so every release
that ships it also ships its complete corresponding source:

    XENO-STORE-<v>-tv-sources/
        README.txt                     what this is, the commit, how to verify and rebuild
        SHA256SUMS                     of every file below ('sha256sum -c SHA256SUMS')
        XENO-STORE-<v>-source.tar.gz   git archive of the released commit (prefix XENO-STORE-<v>/)
        upstream/<archive>             every pinned upstream source archive (PINS), hash-checked

Usage:
    python3 tools/tv/make_source_bundle.py --cache DIR [--ref REF] [--version X.Y.Z] [--out PATH]
                                           [--fetch] [--mirror URL] [--sources-json PATH] [--repo DIR]

The upstream archives are read from --cache DIR (the file name of each pin), so the script works
offline. A missing archive is an error that names every missing file and where it comes from;
with --fetch it is downloaded into the cache instead (from --mirror first when given, then the
upstream URL). Every archive is checked against its pinned SHA-256 before it is used. The pins
are the ones in Orbit Store 1.2.0's SOURCES.json (also listed in app/tv/NOTICE): --sources-json
cross-checks them against such a file, and app/tv/NOTICE at the archived commit must not pin a
different hash. The output is reproducible: fixed order, owner and modes, and the commit time as
every timestamp.

Exit 0 = written, 1 = a source is missing or does not match its pin, 2 = usage or git error.
"""
import argparse
import gzip
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# (file name, SHA-256, upstream URL, what it is). Orbit Store 1.2.0 SOURCES.json "upstream", the
# entries that cover the TV app (BUILD-NOTES "External dependencies"; app/tv/NOTICE).
PINS = (
    ("ps5-payload-sdk-4eb7012.tar.gz",
     "fc9f5974e540c4d4af912f0661cf44075651e67a4d0070a6e8ad44e01bee8ac2",
     "https://github.com/ps5-payload-dev/sdk/archive/4eb701204fc3f8d31e84cf8ca272974e2be9c867.tar.gz",
     "PS5 payload SDK v0.42 (commit 4eb7012): headers, import stubs, prospero-lld and the libcxx.sh "
     "recipe of the prebuilt C++ runtime"),
    ("ps5-opengl-sdk-1.0.0.tar.gz",
     "f93643c04c843d56143b00951df1f8042ea7706ae9f19e4e9abf158e1ead77c5",
     "https://github.com/blackbearreloaded/ps5-opengl/releases/download/v1.0.0/ps5-opengl-sdk-1.0.0.tar.gz",
     "ps5-opengl SDK 1.0.0 with its sources/ (ps5-opengl, Mesa, OpenGNM, PSBC) and licences: "
     "libPS5OpenGL.a and the libSceAgc stubs"),
    ("libwebp-1.4.0.tar.gz",
     "61f873ec69e3be1b99535634340d5bde750b2e4447caa1db9f61be3fd49ab1e5",
     "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.4.0.tar.gz",
     "libwebp and libsharpyuv 1.4.0 (artwork decoding)"),
    ("pacbrew-repo-1687e82.tar.gz",
     "0b05d8c04597acf3b8a056b22b83b19e017ceac59f3cb1af0c149229997ecbbe",
     "https://github.com/ps5-payload-dev/pacbrew-repo/archive/1687e827675c66e1b040751c7f0811222bd3aea0.tar.gz",
     "PacBrew v0.40.2 build recipes (commit 1687e82): libwebp/PKGBUILD"),
    ("libcxx-18.1.8.src.tar.xz",
     "bdecf90be0072bc720fd5c9c8ab061cdb197edd0c8ad3e170dc3e6bfaa49f388",
     "https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/libcxx-18.1.8.src.tar.xz",
     "LLVM 18.1.8 libc++"),
    ("libcxxabi-18.1.8.src.tar.xz",
     "256c30d724eeb72713bc08ae1692f53aaf4ebe8a1d662c92bf59e69d6c53dce9",
     "https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/libcxxabi-18.1.8.src.tar.xz",
     "LLVM 18.1.8 libc++abi"),
    ("libunwind-18.1.8.src.tar.xz",
     "c31577d16978b0da0e472ef751f74893a5b459a7ea4a383b75f7ab93cf1e6877",
     "https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/libunwind-18.1.8.src.tar.xz",
     "LLVM 18.1.8 libunwind"),
    ("cmake-18.1.8.src.tar.xz",
     "59badef592dd34893cd319d42b323aaa990b452d05c7180ff20f23ab1b41e837",
     "https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/cmake-18.1.8.src.tar.xz",
     "LLVM 18.1.8 CMake modules (the C++ runtime build)"),
    ("compiler-rt-18.1.3.src.tar.xz",
     "9a7df9300413696b0c4f7ff1e2729cb82aca375f35c05d698c44f26a4edf1c27",
     "https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.3/compiler-rt-18.1.3.src.tar.xz",
     "LLVM 18.1.3 compiler-rt (the x86-64 builtins in the OpenGL link group)"),
    ("zlib-1.3.2.tar.gz",
     "bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16",
     "https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz",
     "zlib 1.3.2 (the host build tool ps5-native-tool)"),
)
NOTICE_LINE = re.compile(r"^\s+([A-Za-z0-9._+-]+\.tar\.(?:gz|xz))\s+([0-9a-f]{64})\s*$")


class Fail(Exception):
    """A clear, final error: printed as is, exit 1."""


def say(msg):
    print("make_source_bundle: " + msg, flush=True)


def git(repo, *args):
    try:
        proc = subprocess.run(["git", "-C", repo] + list(args), capture_output=True)
    except OSError as e:
        raise RuntimeError("cannot run git: %s" % e)
    if proc.returncode != 0:
        raise RuntimeError("git %s: %s" % (" ".join(args), proc.stderr.decode("utf-8", "replace").strip()))
    return proc.stdout.decode("utf-8")


def git_archive_gz(repo, commit, prefix, dest):
    """git archive of the commit, gzip'ed without a name or time stamp (the same bytes every run)."""
    proc = subprocess.Popen(["git", "-C", repo, "archive", "--format=tar", "--prefix=" + prefix, commit],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    with open(dest, "wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0,
                                                 compresslevel=6) as gz:
        shutil.copyfileobj(proc.stdout, gz, 1 << 20)
    err = proc.stderr.read()
    if proc.wait() != 0:
        raise Fail("git archive %s failed: %s" % (commit, err.decode("utf-8", "replace").strip()))


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def check_pins_against_sources_json(path):
    with open(path, encoding="utf-8") as fh:
        doc = json.load(fh)
    known = {e.get("name"): e for e in doc.get("upstream", []) if isinstance(e, dict)}
    problems = []
    for name, digest, url, _what in PINS:
        e = known.get(name)
        if e is None:
            problems.append("%s is not in %s" % (name, path))
        elif e.get("sha256") != digest:
            problems.append("%s: %s pins %s, this script %s" % (name, path, e.get("sha256"), digest))
        elif e.get("url") != url:
            problems.append("%s: %s has the URL %s, this script %s" % (name, path, e.get("url"), url))
    if problems:
        raise Fail("the pins disagree with %s:\n  %s" % (path, "\n  ".join(problems)))
    say("pins agree with %s (%d archives)" % (path, len(PINS)))


def check_pins_against_notice(repo, commit):
    """app/tv/NOTICE ('Corresponding source') at the archived commit must not pin other hashes."""
    try:
        text = git(repo, "show", "%s:app/tv/NOTICE" % commit)
    except RuntimeError:
        say("note: no app/tv/NOTICE at %s, nothing to cross-check" % commit[:12])
        return
    pins = {name: digest for name, digest, _u, _w in PINS}
    listed = {}
    for line in text.splitlines():
        m = NOTICE_LINE.match(line)
        if m:
            listed[m.group(1)] = m.group(2)
    conflicts = ["%s: NOTICE %s, pinned %s" % (n, d, pins[n]) for n, d in sorted(listed.items())
                 if n in pins and pins[n] != d]
    unknown = sorted(n for n in listed if n not in pins)
    if conflicts or unknown:
        raise Fail("app/tv/NOTICE at %s and the pins disagree:\n  %s" % (
            commit[:12], "\n  ".join(conflicts + ["%s is in NOTICE but not pinned here" % n for n in unknown])))
    missing = sorted(n for n in pins if n not in listed)
    say("app/tv/NOTICE lists %d of the %d pinned archives with the same hashes%s"
        % (len(listed), len(pins), (" (not listed: %s)" % ", ".join(missing)) if missing else ""))


def download(urls, dest, digest):
    """Fetch the first URL that answers with the pinned bytes into dest (via dest.partial)."""
    errors = []
    for url in urls:
        for attempt in range(3):
            partial = dest + ".partial"
            try:
                req = urllib.request.Request(url, headers={"User-Agent": "xeno-store-source-bundle"})
                with urllib.request.urlopen(req, timeout=60) as resp, open(partial, "wb") as out:
                    shutil.copyfileobj(resp, out, 1 << 20)
                got = sha256_file(partial)
                if got != digest:
                    raise ValueError("SHA-256 %s, pinned %s" % (got, digest))
                os.replace(partial, dest)
                return
            except (OSError, ValueError) as e:
                errors.append("%s (try %d): %s" % (url, attempt + 1, e))
                try:
                    os.remove(partial)
                except OSError:
                    pass
                if isinstance(e, ValueError):
                    break   # the bytes are wrong, not the connection: do not retry this URL
                time.sleep(2 * (attempt + 1))
    raise Fail("could not fetch %s:\n  %s" % (os.path.basename(dest), "\n  ".join(errors)))


def gather(cache, fetch, mirror):
    """Every pinned archive, checked, from the cache (and fetched into it with --fetch)."""
    if fetch:
        os.makedirs(cache, exist_ok=True)
    missing, bad, found = [], [], []
    for name, digest, url, _what in PINS:
        path = os.path.join(cache, name)
        if not os.path.isfile(path) and fetch:
            urls = ([mirror.rstrip("/") + "/" + name] if mirror else []) + [url]
            say("fetching %s" % name)
            download(urls, path, digest)
        if not os.path.isfile(path):
            missing.append((name, url))
            continue
        got = sha256_file(path)
        if got != digest:
            bad.append("%s: SHA-256 %s, pinned %s" % (path, got, digest))
            continue
        found.append((name, path, digest))
    if missing or bad:
        lines = []
        if missing:
            lines.append("missing from the cache %s (run with --fetch, or copy them there; Orbit Store "
                         "1.2.0's source bundle carries all of them):" % cache)
            lines += ["  %s  <- %s" % (n, u) for n, u in missing]
        if bad:
            lines.append("not the pinned bytes (delete them and fetch again):")
            lines += ["  " + b for b in bad]
        raise Fail("\n".join(lines))
    say("all %d upstream archives present and matching their pins" % len(found))
    return found


def readme(version, commit, ref, commit_date, source_name):
    out = []
    w = out.append
    title = "XENO STORE %s TV app: complete corresponding source" % version
    w(title)
    w("=" * len(title))
    w("")
    w("The XENO STORE TV app (eboot.bin and sce_module/libc.prx in PPSA99500.ffpkg) is free")
    w("software under the GNU General Public License, version 3 or (at your option) any later")
    w("version. It is based on Orbit Store 1.2.0's TV app and on BlackBearReloaded's")
    w("ps5-homebrew-ui kit (GPL-3.0-or-later). app/tv/NOTICE in the source archive lists every")
    w("component, its licence and the files XENO STORE changed. This archive is the app's complete")
    w("corresponding source for XENO STORE %s:" % version)
    w("")
    w("  %s" % source_name)
    w("      git archive of commit %s (%s, %s)," % (commit, ref, commit_date))
    w("      the whole XENO STORE tree, app/tv included. XENO STORE itself (xeno-store.elf,")
    w("      also bundled in the image as xeno/xeno-store.elf) is GPL-3.0 too: LICENSE.")
    w("  upstream/")
    w("      the pinned source archives of what the app links or is built with:")
    w("")
    for name, digest, url, what in PINS:
        w("      %s" % name)
        w("          %s" % what)
        w("          SHA-256 %s" % digest)
        w("          from %s" % url)
    w("")
    w("Verify:  sha256sum -c SHA256SUMS")
    w("")
    w("Rebuild: unpack %s, then in its top folder (Ubuntu 24.04 with" % source_name)
    w("         clang-18, lld-18, llvm-18, libclang-rt-18-dev, ninja-build, python3, makefs):")
    w("           make tv-deps   fetches and hash-checks the pinned SDK v0.42 + zlib, PacBrew")
    w("                          libwebp, ps5-opengl and GoogleTest into app/tv/.deps")
    w("           make tv        builds the app folder app/tv/dist/PPSA99500")
    w("           make ps5       builds xeno-store.elf (needs the PS5 payload SDK)")
    w("           make ffpkg     builds dist/PPSA99500.ffpkg from both")
    w("         To change a linked library, rebuild it from its archive in upstream/ with its own")
    w("         build files (the C++ runtime with the SDK's libcxx.sh, libwebp with PacBrew's")
    w("         libwebp/PKGBUILD, the graphics stack with ps5-opengl's sources/ and docs/), put the")
    w("         result where app/tv/tools/setup-*.sh and prepare-opengl.sh place the prebuilt one,")
    w("         and run 'make tv' again.")
    w("")
    w("Game names and artwork shown by the app are not part of this source and are not covered by")
    w("these licences.")
    return "\n".join(out) + "\n"


def add_file(tar, arcname, path, mtime):
    ti = tarfile.TarInfo(arcname)
    ti.size = os.path.getsize(path)
    ti.mtime = mtime
    ti.mode = 0o644
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = "root"
    with open(path, "rb") as fh:
        tar.addfile(ti, fh)


def add_dir(tar, arcname, mtime):
    ti = tarfile.TarInfo(arcname)
    ti.type = tarfile.DIRTYPE
    ti.mtime = mtime
    ti.mode = 0o755
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = "root"
    tar.addfile(ti)


def build(args):
    repo = os.path.abspath(args.repo)
    try:
        commit = git(repo, "rev-parse", "--verify", args.ref + "^{commit}").strip()
        commit_time = int(git(repo, "log", "-1", "--format=%ct", commit).strip())
        # The released VERSION is the one in the archived commit, not in the working tree.
        version = args.version.strip() or git(repo, "show", commit + ":VERSION").strip()
    except (RuntimeError, ValueError) as e:
        print("make_source_bundle: %s is not a git checkout with %s and its VERSION: %s"
              % (repo, args.ref, e), file=sys.stderr)
        return 2
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise Fail("version %r is not plain N.N.N" % version)
    commit_date = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(commit_time))
    say("XENO STORE %s, %s = %s (%s)" % (version, args.ref, commit, commit_date))

    if args.sources_json:
        check_pins_against_sources_json(args.sources_json)
    check_pins_against_notice(repo, commit)
    archives = gather(os.path.abspath(args.cache), args.fetch, args.mirror)

    top = "XENO-STORE-%s-tv-sources" % version
    source_name = "XENO-STORE-%s-source.tar.gz" % version
    out = os.path.abspath(args.out or os.path.join(repo, "dist", top + ".tar.gz"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".tv-sources-", dir=os.path.dirname(out)) as tmp:
        source = os.path.join(tmp, source_name)
        git_archive_gz(repo, commit, "XENO-STORE-%s/" % version, source)
        say("%s: %d bytes (git archive of %s)" % (source_name, os.path.getsize(source), commit[:12]))

        sums = [(source_name, sha256_file(source))] + [("upstream/" + n, d) for n, _p, d in archives]
        sums_path = os.path.join(tmp, "SHA256SUMS")
        with open(sums_path, "w", encoding="ascii", newline="\n") as fh:
            fh.writelines("%s  %s\n" % (d, n) for n, d in sums)
        readme_path = os.path.join(tmp, "README.txt")
        with open(readme_path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(readme(version, commit, args.ref, commit_date, source_name))

        partial = os.path.join(tmp, os.path.basename(out))
        with open(partial, "wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0,
                                                        compresslevel=6) as gz, \
                tarfile.open(fileobj=gz, mode="w", format=tarfile.USTAR_FORMAT) as tar:
            add_dir(tar, top, commit_time)
            add_file(tar, top + "/README.txt", readme_path, commit_time)
            add_file(tar, top + "/SHA256SUMS", sums_path, commit_time)
            add_file(tar, top + "/" + source_name, source, commit_time)
            add_dir(tar, top + "/upstream", commit_time)
            for name, path, _digest in archives:
                add_file(tar, top + "/upstream/" + name, path, commit_time)
        # Read it back: every member, and the checksum file against the bytes inside.
        with tarfile.open(partial, "r:gz") as tar:
            names = tar.getnames()
            want = [top, top + "/README.txt", top + "/SHA256SUMS", top + "/" + source_name, top + "/upstream"] + \
                   [top + "/upstream/" + n for n, _p, _d in archives]
            if names != want:
                raise Fail("the written archive holds %s, expected %s" % (names, want))
            for name, digest in sums:
                h = hashlib.sha256()
                fh = tar.extractfile(top + "/" + name)
                for chunk in iter(lambda: fh.read(1 << 20), b""):
                    h.update(chunk)
                if h.hexdigest() != digest:
                    raise Fail("%s changed while it was written" % name)
        os.replace(partial, out)
    say("wrote %s (%d bytes, sha256 %s)" % (out, os.path.getsize(out), sha256_file(out)))
    return 0


def main():
    ap = argparse.ArgumentParser(description="build XENO-STORE-<version>-tv-sources.tar.gz (the TV app's "
                                 "complete corresponding source)")
    ap.add_argument("--cache", required=True, help="directory holding the pinned upstream archives by file name")
    ap.add_argument("--fetch", action="store_true", help="download missing archives into --cache")
    ap.add_argument("--mirror", default="", help="with --fetch: try <URL>/<file name> before the upstream URL")
    ap.add_argument("--ref", default="HEAD", help="the released commit or tag (default HEAD)")
    ap.add_argument("--version", default="", help="override VERSION (default: the file in the archived commit)")
    ap.add_argument("--out", default="", help="output path (default dist/XENO-STORE-<version>-tv-sources.tar.gz)")
    ap.add_argument("--sources-json", default="", help="cross-check the pins against a SOURCES.json "
                    "(Orbit Store 1.2.0's source bundle)")
    ap.add_argument("--repo", default=ROOT, help="the XENO STORE git checkout (default: this one)")
    args = ap.parse_args()
    try:
        return build(args)
    except Fail as e:
        print("make_source_bundle: %s" % e, file=sys.stderr)
        return 1
    except (OSError, ValueError) as e:
        print("make_source_bundle: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
