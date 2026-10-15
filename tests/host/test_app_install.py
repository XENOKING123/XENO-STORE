#!/usr/bin/env python3
"""
test_app_install.py — the store installs its own Games-row app
(GET /api/app/status, POST /api/app/install): a loopback release server
publishes PPSA99500.ffpkg + SHA256SUMS behind a "latest" redirect
(XENO_APP_BASE), and every test runs a fresh host instance whose
/data/homebrew maps to <data>/sim/data/homebrew.

Covered: download + verify + swap-in (bytes, no .part, job done, status),
a wrong published hash (job fails, no file, no .part, an older package stays
untouched), a missing SHA256SUMS, a web page instead of the package, a second
install while one runs (409), cancel (no .part), and the free-space check.
Also: ShadowMountPlus without its HTTP API (before 1.7) — the install and the
setup check rely on its process, and "launch" is offered only when it surely
is not running.
"""
import hashlib
import http.server
import json
import os
import shutil
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402

APP = "PPSA99500.ffpkg"
PKG = bytes((i * 7 + 3) & 0xFF for i in range(256)) * (12 * 1024)   # 3 MiB
PKG_SHA = hashlib.sha256(PKG).hexdigest()
OLD = b"old package bytes" * 100


class Release(http.server.BaseHTTPRequestHandler):
    """/<mode>/(PPSA99500.ffpkg|SHA256SUMS) → 302 → /assets/<mode>/... (like
    releases/latest/download). Modes: ok, badhash, nosha, html, slow."""

    def log_message(self, *a):
        pass

    def _send(self, status, ctype, body, extra=None):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parts = self.path.split("?", 1)[0].strip("/").split("/")
        if len(parts) == 2:
            mode, name = parts
            self._send(302, "text/plain", b"", {"Location": f"/assets/{mode}/{name}"})
            return
        if len(parts) != 3 or parts[0] != "assets":
            self._send(404, "text/plain", b"not found")
            return
        _, mode, name = parts
        if name == "SHA256SUMS":
            if mode == "nosha":
                self._send(404, "text/plain", b"not found")
                return
            sha = "0" * 64 if mode == "badhash" else PKG_SHA
            sums = f"{'a' * 64}  xeno-store.elf\n{sha}  {APP}\n{'b' * 64}  XENO-STORE-Desktop-Windows.zip\n"
            self._send(200, "application/octet-stream", sums.encode())
            return
        if name != APP:
            self._send(404, "text/plain", b"not found")
            return
        if mode == "html":
            self._send(200, "text/html; charset=utf-8", b"<html><body>Sign in</body></html>")
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(PKG)))
        self.end_headers()
        step = 64 * 1024
        try:
            for off in range(0, len(PKG), step):
                self.wfile.write(PKG[off:off + step])
                if mode == "slow":
                    self.wfile.flush()
                    time.sleep(0.1)   # ~5 s for the whole package
        except (BrokenPipeError, ConnectionResetError):
            pass


class AppInstallTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.release = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Release)
        cls.release.daemon_threads = True
        threading.Thread(target=cls.release.serve_forever, daemon=True).start()
        cls.base = f"http://127.0.0.1:{cls.release.server_address[1]}"
        cls.tmp = tempfile.mkdtemp(prefix="xeno-app-")

    @classmethod
    def tearDownClass(cls):
        cls.release.shutdown()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        self.srv = None

    def tearDown(self):
        if self.srv:
            self.srv.stop()

    # -- helpers ------------------------------------------------------------

    def start(self, mode, env=None):
        data = tempfile.mkdtemp(prefix=f"{mode}-", dir=self.tmp)
        e = {"XENO_APP_BASE": f"{self.base}/{mode}"}
        e.update(env or {})
        self.srv = harness.XenoServer(data, env=e).start()
        self.homebrew = os.path.join(data, "sim", "data", "homebrew")
        self.pkg = os.path.join(self.homebrew, APP)
        return self.srv

    def install(self):
        st, b = self.srv.api("POST", "/api/app/install", {})
        self.assertEqual(st, 200, b)
        job = b["job"]
        self.assertEqual(job["type"], "app-install")
        self.assertIn(job["state"], ("queued", "running"))
        return job["id"]

    def wait(self, jid, timeout=30):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            st, b = self.srv.api("GET", f"/api/saves/jobs/{jid}")
            self.assertEqual(st, 200, b)
            last = b["job"]
            if last["state"] in ("done", "failed", "cancelled"):
                return last
            time.sleep(0.1)
        raise AssertionError(f"job {jid} did not settle: {json.dumps(last)[:500]}")

    def status(self):
        st, b = self.srv.api("GET", "/api/app/status")
        self.assertEqual(st, 200, b)
        self.assertTrue(b["ok"])
        return b

    def assert_no_part(self):
        self.assertFalse(os.path.exists(self.pkg + ".part"), "the .part file was left behind")

    # -- tests --------------------------------------------------------------

    def test_status_before_install(self):
        self.start("ok")
        s = self.status()
        self.assertFalse(s["installed"])
        self.assertFalse(s["file"])
        self.assertFalse(s["registered"])
        self.assertEqual(s["size"], 0)
        self.assertEqual(s["job"], "")
        self.assertEqual(s["version"], "")
        self.assertEqual(s["titleId"], "PPSA99500")
        self.assertTrue(s["path"].endswith("/sim/data/homebrew/" + APP), s["path"])

    def test_install_downloads_verifies_and_swaps_in(self):
        self.start("ok")
        jid = self.install()
        job = self.wait(jid)
        self.assertEqual(job["state"], "done", job)
        self.assertEqual(job["percent"], 100)
        res = job["result"]
        self.assertTrue(res["verified"])
        self.assertEqual(res["sha256"], PKG_SHA)
        self.assertEqual(res["size"], len(PKG))
        self.assertIn("shadowMount", res)
        with open(self.pkg, "rb") as f:
            self.assertEqual(f.read(), PKG)
        self.assert_no_part()
        s = self.status()
        self.assertTrue(s["installed"])
        self.assertTrue(s["file"])
        self.assertEqual(s["size"], len(PKG))
        self.assertEqual(s["job"], "")
        self.assertEqual(s["lastJob"], jid)
        # a reinstall (update) replaces the package in place
        jid2 = self.install()
        self.assertEqual(self.wait(jid2)["state"], "done")
        with open(self.pkg, "rb") as f:
            self.assertEqual(f.read(), PKG)
        self.assert_no_part()

    def test_version_from_installed_app(self):
        srv = self.start("ok")
        param = os.path.join(srv.data_dir, "user", "app", "PPSA99500", "sce_sys", "param.json")
        os.makedirs(os.path.dirname(param))
        with open(param, "w") as f:
            json.dump({"titleId": "PPSA99500", "contentVersion": "01.020.000"}, f)
        s = self.status()
        self.assertTrue(s["installed"])
        self.assertTrue(s["registered"])
        self.assertFalse(s["file"])
        self.assertEqual(s["version"], "01.020.000")

    def test_wrong_hash_fails_and_leaves_no_file(self):
        self.start("badhash")
        jid = self.install()
        job = self.wait(jid)
        self.assertEqual(job["state"], "failed", job)
        self.assertEqual(job["errorCode"], "sha256_mismatch", job)
        self.assertFalse(os.path.exists(self.pkg))
        self.assert_no_part()
        self.assertFalse(self.status()["installed"])

    def test_wrong_hash_keeps_the_older_package(self):
        self.start("badhash")
        os.makedirs(self.homebrew, exist_ok=True)
        with open(self.pkg, "wb") as f:
            f.write(OLD)
        job = self.wait(self.install())
        self.assertEqual(job["state"], "failed", job)
        with open(self.pkg, "rb") as f:
            self.assertEqual(f.read(), OLD)
        self.assert_no_part()

    def test_missing_sha256_fails(self):
        self.start("nosha")
        job = self.wait(self.install())
        self.assertEqual(job["state"], "failed", job)
        self.assertEqual(job["errorCode"], "sha256_unavailable", job)
        self.assertFalse(os.path.exists(self.pkg))
        self.assert_no_part()

    def test_web_page_instead_of_package_fails(self):
        self.start("html")
        job = self.wait(self.install())
        self.assertEqual(job["state"], "failed", job)
        self.assertEqual(job["errorCode"], "not_package", job)
        self.assertFalse(os.path.exists(self.pkg))
        self.assert_no_part()

    def test_second_install_refused_while_running_then_cancel(self):
        self.start("slow")
        jid = self.install()
        st, b = self.srv.api("POST", "/api/app/install", {})
        self.assertEqual(st, 409, b)
        self.assertFalse(b["ok"])
        self.assertEqual(b["error"], "in_progress")
        self.assertEqual(self.status()["job"], jid)
        # wait until bytes flow, then cancel: the .part goes away
        deadline = time.time() + 10
        while time.time() < deadline:
            st, b = self.srv.api("GET", f"/api/saves/jobs/{jid}")
            if b["job"]["done"] > 0:
                break
            time.sleep(0.05)
        st, b = self.srv.api("POST", f"/api/saves/jobs/{jid}/cancel", {})
        self.assertEqual(st, 200, b)
        job = self.wait(jid)
        self.assertEqual(job["state"], "cancelled", job)
        self.assertFalse(os.path.exists(self.pkg))
        self.assert_no_part()
        self.assertEqual(self.status()["job"], "")
        # once nothing runs, a new install is accepted again
        st, b = self.srv.api("POST", "/api/app/install", {})
        self.assertEqual(st, 200, b)
        self.srv.api("POST", f"/api/saves/jobs/{b['job']['id']}/cancel", {})
        self.wait(b["job"]["id"])

    def test_not_enough_space(self):
        self.start("ok", env={"XENO_FAKE_FREE": "1048576"})
        job = self.wait(self.install())
        self.assertEqual(job["state"], "failed", job)
        self.assertEqual(job["errorCode"], "no_space", job)
        self.assertFalse(os.path.exists(self.pkg))
        self.assert_no_part()

    # -- ShadowMountPlus without its HTTP API (versions before 1.7) ----------
    # The host build reads the process list from XENO_HOST_SHADOWMOUNT
    # ("running" / "stopped"; unset means it cannot be read).

    def test_install_done_when_shadowmount_has_no_api(self):
        self.start("ok", env={"XENO_HOST_SHADOWMOUNT": "running"})
        job = self.wait(self.install())
        self.assertEqual(job["state"], "done", job)
        self.assertTrue(job["result"]["shadowMount"], job)
        self.assertIn("on its own", job["result"]["shadowMountStatus"])
        st, b = self.srv.api("POST", "/api/shadowmount/scan")
        self.assertEqual(st, 200, b)
        self.assertTrue(b["available"], b)

    def shadowmount_check(self, process):
        if self.srv:
            self.srv.stop()
        env = {"XENO_PORT_SHADOWMOUNT": "1"}   # nothing answers: no API
        if process:
            env["XENO_HOST_SHADOWMOUNT"] = process
        srv = self.start("ok", env=env)
        st, b = srv.api("PUT", "/api/settings", {"catalog": {"revisionUrl": f"{self.base}/ok/SHA256SUMS"}})
        self.assertEqual(st, 200, b)
        payloads = os.path.join(srv.data_dir, "payloads")
        os.makedirs(payloads, exist_ok=True)
        with open(os.path.join(payloads, "shadowmountplus.elf"), "wb") as f:
            f.write(b"\x7fELF" + bytes(60))
        with open(os.path.join(payloads, "installed.json"), "w") as f:
            json.dump({"shadowmountplus": {"version": "1.6", "name": "ShadowMountPlus", "kind": "loader"}}, f)
        st, b = srv.api("GET", "/api/setup/check")
        self.assertEqual(st, 200, b)
        return {c["id"]: c for c in b["checks"]}["shadowmount"]

    def test_setup_check_finds_shadowmount_without_api(self):
        # running: ok, and no "launch" (a second copy would replace it)
        c = self.shadowmount_check("running")
        self.assertTrue(c["ok"], c)
        self.assertNotIn("action", c)
        self.assertIn("running", c["detail"])
        # surely not running: a failure that offers the copy on the console
        c = self.shadowmount_check("stopped")
        self.assertFalse(c["ok"], c)
        self.assertFalse(c["optional"], c)
        self.assertEqual(c.get("action"), "launch", c)
        self.assertEqual(c.get("actionId"), "shadowmountplus", c)
        # cannot tell: optional, nothing launched on a guess
        c = self.shadowmount_check(None)
        self.assertFalse(c["ok"], c)
        self.assertTrue(c["optional"], c)
        self.assertNotIn("action", c)


if __name__ == "__main__":
    unittest.main()
