"""site_check.py (the read-only live check of the XenoKing site) against the host tests' Supabase stand-in.

Offline: tests/host/supabase_mock.py serves the 57 rows of tests/host/xenoking_rows.json and a Garlic zip.

  python3 -I -m unittest discover -s tools/saves/tests -v      (or: make saves-test)
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tests", "host"))

import site_check  # noqa: E402
from supabase_mock import SupabaseMock  # noqa: E402

KEY = "public-anon-key-for-tests"


def garlic_zip():
    sfo = b"\0PSF" + struct.pack("<IIII", 0x0101, 20, 20, 0)
    bio = io.BytesIO()
    with zipfile.ZipFile(bio, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("Split Fiction/sce_sys/param.sfo", sfo)
        z.writestr("Split Fiction/savedata.bin", b"x" * 2000)
    return bio.getvalue()


class SiteCheckTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="xeno-site-check-")
        cls.mock = SupabaseMock(KEY).start()
        with open(os.path.join(ROOT, "tests", "host", "xenoking_rows.json"), encoding="utf-8") as f:
            for r in json.load(f)["rows"]:
                cls.mock.add_row(**r)
        cls.mock.garlic[94] = garlic_zip()
        cls.backend = os.path.join(cls.tmp, "saves-backend.json")
        with open(cls.backend, "w", encoding="utf-8") as f:
            json.dump({"kind": "supabase", "name": "XenoKing", "url": cls.mock.base, "anonKey": KEY, "garlicFunction": "garlic-savedb"}, f)

    @classmethod
    def tearDownClass(cls):
        cls.mock.stop()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_check(self, *extra):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = site_check.main(["--backend", self.backend, "--allow-http", "--timeout", "10", *extra])
        return rc, out.getvalue()

    def test_passes_and_writes_nothing(self):
        mark = len(self.mock.log)
        rc, out = self.run_check()
        self.assertEqual(rc, 0, out)
        self.assertIn("list: 57 rows", out)
        self.assertIn("param.sfo at Split Fiction/sce_sys/param.sfo", out)
        calls = [(e["method"], e["path"]) for e in self.mock.log[mark:]]
        self.assertEqual(calls, [("GET", "/rest/v1/ps5_save_collection"), ("POST", "/functions/v1/garlic-savedb")])
        self.assertEqual(json.loads(self.mock.log[-1]["body"]), {"action": "download", "id": 94})

    def test_too_few_rows(self):
        rc, out = self.run_check("--min-rows", "100")
        self.assertEqual(rc, 1)
        self.assertIn("expected at least 100 rows", out)

    def test_garlic_failure(self):
        try:
            self.mock.fail["garlic"] = (502, {"error": "Garlic session expired"})
            rc, out = self.run_check()
        finally:
            self.mock.fail.clear()
        self.assertEqual(rc, 1)
        self.assertIn("HTTP 502", out)
        try:
            self.mock.fail["garlic200"] = "Garlic session expired"
            rc, out = self.run_check()
        finally:
            self.mock.fail.clear()
        self.assertEqual(rc, 1)
        self.assertIn("not a zip", out)

    def test_redirect_not_followed(self):
        try:
            self.mock.fail["list_redirect"] = "http://127.0.0.1:9/elsewhere"
            rc, out = self.run_check()
        finally:
            self.mock.fail.clear()
        self.assertEqual(rc, 1)
        self.assertIn("FAIL list: HTTP 302", out)

    def test_published_backend_is_the_site(self):
        b, problems = site_check.load_backend(os.path.join(ROOT, "updates", "saves-backend.json"), False)
        self.assertEqual(problems, [])
        self.assertEqual(b["name"], "XenoKing")
        self.assertEqual(b["url"], "https://cotniitpyocsfqzywscl.supabase.co")
        self.assertEqual(b["bucket"], "ps5-saves")
        self.assertEqual(b["garlicFunction"], "garlic-savedb")
        self.assertNotIn("downloadCounter", b)
        self.assertNotIn("garlicList", b)
        self.assertNotIn("moderated", b)
        _, problems = site_check.load_backend(self.backend, False)
        self.assertTrue(any("https" in p for p in problems))


if __name__ == "__main__":
    unittest.main()
