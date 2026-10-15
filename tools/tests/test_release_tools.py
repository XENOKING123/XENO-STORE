#!/usr/bin/env python3
# XENO STORE - tests for the release tools (tools/make_ffpkg.py, tools/tv/make_source_bundle.py).
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Offline unit tests of the release tools' input checks ('make tools-test', part of 'make test'):

  make_source_bundle.gather()   a cached archive whose bytes are not the pinned ones is fetched
                                again with --fetch (CI restores older caches through restore-keys),
                                and reported without it
  make_ffpkg.stage_tv_app()     an app folder built for another VERSION or another title, or the
                                unmodified Orbit Store eboot, is refused before anything is staged

Python 3 standard library only; run under python3 -I.
"""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import pathlib
import shutil
import sys
import tempfile
import types
import unittest

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def sha256(data):
    return hashlib.sha256(data).hexdigest()


class SourceBundleGatherTests(unittest.TestCase):
    GOOD = b"the pinned upstream archive\n" * 64
    STALE = b"the bytes an older pin had under the same name\n" * 64

    def setUp(self):
        self.msb = load("xeno_make_source_bundle_t", os.path.join(TOOLS, "tv", "make_source_bundle.py"))
        self.tmp = tempfile.mkdtemp(prefix="xeno-msb-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.cache = os.path.join(self.tmp, "cache")
        self.upstream = os.path.join(self.tmp, "upstream")
        os.makedirs(self.cache)
        os.makedirs(self.upstream)
        self.name = "thing-1.0.tar.gz"
        with open(os.path.join(self.upstream, self.name), "wb") as fh:
            fh.write(self.GOOD)
        url = pathlib.Path(self.upstream, self.name).as_uri()
        self.msb.PINS = ((self.name, sha256(self.GOOD), url, "a test archive"),)
        self.msb.say = lambda _msg: None
        self.cached = os.path.join(self.cache, self.name)

    def put_cached(self, data):
        with open(self.cached, "wb") as fh:
            fh.write(data)

    def read_cached(self):
        with open(self.cached, "rb") as fh:
            return fh.read()

    def test_stale_cached_copy_is_fetched_again(self):
        self.put_cached(self.STALE)
        found = self.msb.gather(self.cache, True, "")
        self.assertEqual(found, [(self.name, self.cached, sha256(self.GOOD))])
        self.assertEqual(self.read_cached(), self.GOOD)
        self.assertFalse(os.path.exists(self.cached + ".partial"))

    def test_stale_cached_copy_without_fetch_is_reported(self):
        self.put_cached(self.STALE)
        with self.assertRaises(self.msb.Fail) as cm:
            self.msb.gather(self.cache, False, "")
        self.assertIn("not the pinned bytes", str(cm.exception))
        self.assertIn("--fetch", str(cm.exception))
        self.assertEqual(self.read_cached(), self.STALE)

    def test_missing_archive_is_fetched(self):
        found = self.msb.gather(self.cache, True, "")
        self.assertEqual(len(found), 1)
        self.assertEqual(self.read_cached(), self.GOOD)

    def test_missing_archive_without_fetch_is_reported(self):
        with self.assertRaises(self.msb.Fail) as cm:
            self.msb.gather(self.cache, False, "")
        self.assertIn("missing from the cache", str(cm.exception))

    def test_wrong_upstream_bytes_keep_the_cached_copy(self):
        # Upstream now serves other bytes too: the fetch fails on the hash, nothing is replaced.
        self.put_cached(self.STALE)
        with open(os.path.join(self.upstream, self.name), "wb") as fh:
            fh.write(b"neither\n")
        with self.assertRaises(self.msb.Fail) as cm:
            self.msb.gather(self.cache, True, "")
        self.assertIn("could not fetch", str(cm.exception))
        self.assertEqual(self.read_cached(), self.STALE)
        self.assertFalse(os.path.exists(self.cached + ".partial"))

    def test_good_cached_copy_is_used_without_fetching(self):
        self.put_cached(self.GOOD)
        os.remove(os.path.join(self.upstream, self.name))   # a fetch would fail now
        found = self.msb.gather(self.cache, True, "")
        self.assertEqual(found, [(self.name, self.cached, sha256(self.GOOD))])


class MakeFfpkgAppDirTests(unittest.TestCase):
    VERSION = "1.2.1"

    def setUp(self):
        self.mf = load("xeno_make_ffpkg_t", os.path.join(TOOLS, "make_ffpkg.py"))
        self.tmp = tempfile.mkdtemp(prefix="xeno-ffpkg-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.app = os.path.join(self.tmp, "PPSA99500")
        self.tv_src = os.path.join(self.tmp, "tv")
        self.stage = os.path.join(self.tmp, "stage")
        # A native app SELF: magic, no segment entries, then an ELF header with e_type ET_SCE_DYNEXEC.
        self.eboot_bytes = (b"\x4f\x15\x3d\x1d" + bytes(0x1C) + b"\x7fELF" + bytes(12)
                            + self.mf.ET_SCE_DYNEXEC.to_bytes(2, "little") + bytes(46))
        libc = b"a stand-in for the kit's libc.prx\n"
        self.mf.LIBC_PRX_SHA256 = sha256(libc)
        self.write(os.path.join(self.app, "eboot.bin"), self.eboot_bytes)
        self.write(os.path.join(self.app, "sce_module", "libc.prx"), libc)
        self.write(os.path.join(self.app, "assets", "fonts", "inter-regular.huifont"), b"font\n")
        self.write_param(self.mf.TITLE_ID, self.mf.content_version(self.VERSION))
        self.write(os.path.join(self.tv_src, "NOTICE"), b"notice\n")
        self.write(os.path.join(self.tv_src, "licenses", "GPL-3.0.txt"), b"GPL-3.0\n")
        self.elf = os.path.join(self.tmp, "xeno-store.elf")
        self.write(self.elf, b"\x7fELF" + bytes(60) + b"XENO-STORE/" + self.VERSION.encode() + b"\0")

    @staticmethod
    def write(path, data):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(data)

    def write_param(self, title_id, content_version):
        param = {"titleId": title_id, "contentVersion": content_version,
                 "amm": {"flexibleMemorySize": 0}, "kernel": {"cpuPageTableSize": 0}}
        self.write(os.path.join(self.app, "sce_sys", "param.json"), json.dumps(param).encode())

    def stage_app(self):
        args = types.SimpleNamespace(app_dir=self.app, tv_src=self.tv_src, eboot=None, sce_module=None,
                                     stage=self.stage)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            rc, src_dir, _out = self.mf.stage_tv_app(args, self.VERSION, self.elf, lambda _msg: None)
        return rc, src_dir, err.getvalue()

    def test_app_folder_built_for_this_version_is_staged(self):
        rc, src_dir, err = self.stage_app()
        self.assertEqual(rc, 0, err)
        with open(os.path.join(src_dir, "sce_sys", "param.json"), encoding="utf-8") as fh:
            param = json.load(fh)
        self.assertEqual((param["titleId"], param["contentVersion"]), ("PPSA99500", "01.002.001"))
        with open(os.path.join(src_dir, "xeno", "version.txt"), encoding="ascii") as fh:
            self.assertEqual(fh.read(), "1.2.1\n")

    def test_app_folder_from_an_older_version_is_refused(self):
        self.write_param(self.mf.TITLE_ID, "01.002.000")
        rc, src_dir, err = self.stage_app()
        self.assertEqual(rc, 1)
        self.assertIsNone(src_dir)
        self.assertIn("01.002.000", err)
        self.assertIn("01.002.001", err)
        self.assertIn("make tv", err)
        self.assertFalse(os.path.exists(self.stage))

    def test_app_folder_of_another_title_is_refused(self):
        self.write_param("PPSA99177", "01.005.000")   # the unported Orbit Store folder
        rc, _src, err = self.stage_app()
        self.assertEqual(rc, 1)
        self.assertIn("PPSA99177", err)

    def test_app_folder_without_a_content_version_is_refused(self):
        self.write_param(self.mf.TITLE_ID, None)
        rc, _src, err = self.stage_app()
        self.assertEqual(rc, 1)
        self.assertIn("make tv", err)

    def test_unmodified_orbit_eboot_is_refused(self):
        self.mf.ORBIT_M0_EBOOT_SHA256 = sha256(self.eboot_bytes)
        rc, _src, err = self.stage_app()
        self.assertEqual(rc, 1)
        self.assertIn("unmodified Orbit Store", err)
        self.assertFalse(os.path.exists(self.stage))

    def test_orbit_hash_is_the_one_ci_checks(self):
        # build.yml checks the tv-app artifact against the same constant before it is uploaded.
        workflow = os.path.join(os.path.dirname(TOOLS), ".github", "workflows", "build.yml")
        if not os.path.isfile(workflow):
            self.skipTest("no .github/workflows/build.yml")
        with open(workflow, encoding="utf-8") as fh:
            text = fh.read()
        self.assertIn(self.mf.ORBIT_M0_EBOOT_SHA256, text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
