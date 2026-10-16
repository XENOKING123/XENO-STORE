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
install while one runs (409), cancel (no .part), uninstall during an install
(the job is cancelled first) and the free-space check.
Also: ShadowMountPlus without its HTTP API (before 1.7) — the install and the
setup check rely on its process, and "launch" is offered only when it surely
is not running. And "Update available" on the Settings → Updates card: the
release named by updates/tv-app.json (served next to the updates feed) against
the installed app's version, never for a folder copy. And what a store that
starts on its own opens on the TV: the app when it is installed and registered,
else (or when it does not start) the browser, nothing when the TV app started it
or the setting is off, and the browser again when Settings → Updates → Restart
now started it.
"""
import hashlib
import http.server
import itertools
import json
import os
import shutil
import sys
import tempfile
import threading
import time
import unittest
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402

APP = "PPSA99500.ffpkg"
PKG = bytes((i * 7 + 3) & 0xFF for i in range(256)) * (12 * 1024)   # 3 MiB
PKG_SHA = hashlib.sha256(PKG).hexdigest()
OLD = b"old package bytes" * 100


class Release(http.server.BaseHTTPRequestHandler):
    """/<mode>/(PPSA99500.ffpkg|SHA256SUMS) → 302 → /assets/<mode>/... (like
    releases/latest/download). Modes: ok, badhash, nosha, html, slow.
    /updates/tv-app.json answers `feed` and counts the reads in `feed_hits`."""
    feed = b""
    feed_hits = 0

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
        if self.path.split("?", 1)[0] == "/updates/tv-app.json":
            Release.feed_hits += 1
            self._send(200, "application/json", Release.feed)
            return
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

    def start(self, mode, env=None, settings=None):
        data = tempfile.mkdtemp(prefix=f"{mode}-", dir=self.tmp)
        if settings:
            with open(os.path.join(data, "settings.json"), "w") as f:
                json.dump(settings, f)
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
        self.assertIs(s["updateAvailable"], False)

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

    # -- "Update available" on the card: updates/tv-app.json ----------------

    def publish(self, version, filename=APP):
        Release.feed = json.dumps({"version": version, "filename": filename, "size": len(PKG), "sha256": PKG_SHA,
                                   "url": f"https://github.com/XENOKING123/XENO-STORE/releases/download/v{version}/{APP}"}).encode()

    def installed_app(self, content_version):
        param = os.path.join(self.srv.data_dir, "user", "app", "PPSA99500", "sce_sys", "param.json")
        os.makedirs(os.path.dirname(param), exist_ok=True)
        with open(param, "w") as f:
            json.dump({"titleId": "PPSA99500", "contentVersion": content_version}, f)

    def settled(self, path="/api/app/status"):
        deadline = time.time() + 15
        while True:
            st, b = self.srv.api("GET", path)
            self.assertEqual(st, 200, b)
            if not b["checking"] or time.time() > deadline:
                return b
            path = "/api/app/status"
            time.sleep(0.1)

    def test_update_available_from_the_tv_app_feed(self):
        self.publish("1.4.0")
        self.start("ok", settings={"updates": {"feed": f"{self.base}/updates/payloads.json", "autoCheck": True}})
        self.installed_app("01.003.001")
        s = self.settled()
        self.assertEqual((s["version"], s["latestVersion"]), ("01.003.001", "1.4.0"), s)
        self.assertIs(s["updateAvailable"], True)
        self.assertEqual(s["copy"], "none")
        # the same version installed: nothing to update (the feed is not read again within 6 h)
        hits = Release.feed_hits
        self.installed_app("01.004.000")
        s = self.settled()
        self.assertEqual(s["latestVersion"], "1.4.0")
        self.assertIs(s["updateAvailable"], False)
        self.assertEqual(Release.feed_hits, hits)
        # Check for updates (?check=1) reads it again at once
        self.publish("1.4.1")
        s = self.settled("/api/app/status?check=1")
        self.assertEqual(s["latestVersion"], "1.4.1", s)
        self.assertIs(s["updateAvailable"], True)
        # a feed for another file is not this app's release: the last one stays
        self.publish("1.5.0", filename="other.ffpkg")
        s = self.settled("/api/app/status?check=1")
        self.assertEqual(s["latestVersion"], "1.4.1", s)

    def test_no_update_offered_for_a_folder_copy(self):
        # The app as the folder /data/homebrew/PPSA99500 (the TV app refuses to update it too):
        # the card's install would add PPSA99500.ffpkg beside it, a second copy.
        self.publish("1.4.0")
        self.start("ok", settings={"updates": {"feed": f"{self.base}/updates/payloads.json", "autoCheck": True}})
        self.installed_app("01.003.001")
        s = self.settled()
        self.assertIs(s["updateAvailable"], True, s)
        folder = os.path.join(os.path.dirname(s["path"]), "PPSA99500")
        os.makedirs(folder)
        with open(os.path.join(folder, "eboot.bin"), "wb") as f:
            f.write(b"\x7fELF")
        s = self.settled()
        self.assertIs(s["updateAvailable"], False, s)
        self.assertEqual((s["copy"], s["latestVersion"]), ("folder", "1.4.0"))

    def test_tv_app_feed_waits_for_check_while_automatic_checks_are_off(self):
        self.publish("1.4.0")
        self.start("ok", settings={"updates": {"feed": f"{self.base}/updates/payloads.json", "autoCheck": False}})
        self.installed_app("01.003.001")
        hits = Release.feed_hits
        s = self.settled()
        time.sleep(0.5)
        self.assertEqual(Release.feed_hits, hits, "no automatic read while automatic update checks are off")
        self.assertEqual((s["latestVersion"], s["updateAvailable"]), ("", False))
        s = self.settled("/api/app/status?check=1")
        self.assertEqual(s["latestVersion"], "1.4.0", s)
        self.assertIs(s["updateAvailable"], True)
        self.assertEqual(Release.feed_hits, hits + 1)

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

    def test_uninstall_cancels_a_running_install(self):
        # "Home-screen app" while an install runs (web UI, or the TV app's
        # update): the job is cancelled before the files go, so it cannot put
        # the app back or leave its .part behind
        self.start("slow")
        jid = self.install()
        deadline = time.time() + 10
        while time.time() < deadline and not os.path.exists(self.pkg + ".part"):
            time.sleep(0.05)
        self.assertTrue(os.path.exists(self.pkg + ".part"))
        st, b = self.srv.api("POST", "/api/uninstall", {"homebrew": True})
        self.assertEqual(st, 200, b)
        self.assertEqual(b["failed"], [])
        self.assertFalse(os.path.exists(self.pkg))
        self.assert_no_part()
        deadline = time.time() + 10
        while time.time() < deadline and self.srv.proc.poll() is None:
            time.sleep(0.1)
        self.assertIsNotNone(self.srv.proc.poll(), "server should stop after uninstall")
        self.assertFalse(os.path.exists(self.pkg))
        self.assert_no_part()
        with open(self.srv.log_path, encoding="utf-8", errors="replace") as f:
            self.assertIn(f"job {jid} (app-install) cancelled", f.read())

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



class StartupOpenTests(unittest.TestCase):
    """Settings → "Open XENO STORE when it starts" when the store starts on
    its own (ELF loader, Payload Manager): the XENO STORE app when its image
    (or folder) is in /data/homebrew and its title is registered (host:
    <data>/user/app/PPSA99500; the simulated launch writes <data>/sim/launched.json),
    the browser otherwise or when the app does not start (XENO_HOST_LAUNCH_FAIL),
    and neither when the TV app started the store or the setting is off. A copy
    that Settings → Updates → Restart now started (its hand-off marker, written
    by another build, does not say "start") opens the browser it was restarted
    from again, never the app over it. The app shows the pairing code itself, so
    the start-up toasts stay away when it opens."""

    def start(self, image=False, folder=False, registered=False, poll_tv=False, open_on_start=True, env=None, marker=None):
        tmp = tempfile.mkdtemp(prefix="xeno-startopen.")
        self.addCleanup(shutil.rmtree, tmp, True)
        data = os.path.join(tmp, "data")
        homebrew = os.path.join(data, "sim", "data", "homebrew")
        os.makedirs(homebrew)
        if marker is not None:   # <data>/handoff.pending: "<time> <version that handed off> [start|restart]"
            with open(os.path.join(data, "handoff.pending"), "w") as f:
                f.write(marker)
        if image:
            with open(os.path.join(homebrew, APP), "wb") as f:
                f.write(PKG)
        if folder:
            os.makedirs(os.path.join(homebrew, "PPSA99500"))
            with open(os.path.join(homebrew, "PPSA99500", "eboot.bin"), "wb") as f:
                f.write(b"\x7fELF")
        if registered:
            os.makedirs(os.path.join(data, "user", "app", "PPSA99500", "sce_sys"))
        port = harness.free_port()
        if not open_on_start:
            srv = harness.XenoServer(data, port=port).start()
            srv.api("PUT", "/api/settings", {"general": {"openOnStart": False}})
            srv.stop()
            os.unlink(srv.log_path)
        stop = threading.Event()
        seen = []

        def poller():   # the TV app waits for the store it started by polling GET /api/v1/system
            while not stop.is_set():
                try:
                    with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/v1/system", timeout=1) as r:
                        if r.status == 200:
                            seen.append(time.time())
                except Exception:
                    pass
                time.sleep(0.2)
        if poll_tv:
            threading.Thread(target=poller, daemon=True).start()
            self.addCleanup(stop.set)
        srv = harness.XenoServer(data, port=port, env=env).start()
        self.addCleanup(srv.stop)
        harness.wait_until(lambda: "ready: " in srv.log_text(), timeout=15, what="the ready line")
        if poll_tv:
            self.assertTrue(seen, "the poller never reached /api/v1/system")
        self.launched = os.path.join(data, "sim", "launched.json")
        return srv

    def assert_app_opened(self, srv):
        log = srv.log_text()
        with open(self.launched) as f:
            self.assertEqual(json.load(f)["titleId"], "PPSA99500")
        self.assertIn("start-up: opened the XENO STORE app", log)
        self.assertNotIn("browser launch not built in", log)
        self.assertNotIn("toast: XENO STORE pairing code", log, "the app shows the code itself")
        self.assertNotIn("toast: XENO STORE ready", log)

    def assert_browser_opened(self, srv):
        log = srv.log_text()
        self.assertFalse(os.path.exists(self.launched), "the app was launched")
        self.assertIn("browser launch not built in (url http://127.0.0.1:", log)
        self.assertIn("toast: XENO STORE pairing code", log)
        self.assertIn("toast: XENO STORE ready", log)

    def assert_nothing_opened(self, srv):
        self.assertFalse(os.path.exists(self.launched), "the app was launched")
        self.assertNotIn("browser launch not built in", srv.log_text())

    def test_the_app_when_installed_and_registered(self):
        self.assert_app_opened(self.start(image=True, registered=True))

    def test_the_app_as_a_folder(self):
        self.assert_app_opened(self.start(folder=True, registered=True))

    def test_the_browser_without_the_app(self):
        self.assert_browser_opened(self.start())
        self.assert_browser_opened(self.start(image=True))           # not registered (yet)
        self.assert_browser_opened(self.start(registered=True))      # a tile left behind without its image

    def test_the_browser_when_the_app_does_not_start(self):
        srv = self.start(image=True, registered=True, env={"XENO_HOST_LAUNCH_FAIL": "1"})
        self.assert_browser_opened(srv)
        self.assertIn("start-up: the XENO STORE app did not open", srv.log_text())

    def test_nothing_when_the_tv_app_started_the_store(self):
        srv = self.start(image=True, registered=True, poll_tv=True)
        self.assert_nothing_opened(srv)
        self.assertIn("the TV app is polling", srv.log_text())

    def test_nothing_when_the_setting_is_off(self):
        self.assert_nothing_opened(self.start(image=True, registered=True, open_on_start=False))

    def test_the_browser_again_after_restart_now(self):
        now = int(time.time())
        self.assert_browser_opened(self.start(image=True, registered=True, marker=f"{now - 3} 0.0.1 restart\n"))
        # an older build's marker does not say why: as before, the browser
        self.assert_browser_opened(self.start(image=True, registered=True, marker=f"{now - 3} 0.0.1\n"))
        # a store that started on its own and handed over to a downloaded update: the app
        self.assert_app_opened(self.start(image=True, registered=True, marker=f"{now - 3} 0.0.1 start\n"))
        # a marker from long ago is no restart of this copy
        self.assert_app_opened(self.start(image=True, registered=True, marker=f"{now - 600} 0.0.1 restart\n"))

    def test_the_decision_table(self):
        # app_install_open_choice over every input (host-only route)
        srv = harness.XenoServer(tempfile.mkdtemp(prefix="xeno-choice.")).start()
        self.addCleanup(shutil.rmtree, srv.data_dir, True)
        self.addCleanup(srv.stop)
        seen = {}
        for tv, wanted, recent, restarted, launchable in itertools.product((0, 1), repeat=5):
            st, b = srv.api("GET", f"/api/debug/open-choice?tvApp={tv}&wanted={wanted}&recent={recent}"
                                   f"&restarted={restarted}&launchable={launchable}")
            self.assertEqual(st, 200, b)
            seen[(tv, wanted, recent, restarted, launchable)] = b["open"]
        for (tv, wanted, recent, restarted, launchable), got in seen.items():
            if tv or not wanted or recent:      # the TV app is open, the setting is off, or the Games-row app just opened the browser
                want = "none"
            elif launchable and not restarted:  # started on its own with the app installed
                want = "app"
            else:
                want = "browser"
            self.assertEqual(got, want, dict(tvApp=tv, wanted=wanted, recent=recent, restarted=restarted, launchable=launchable))
        self.assertEqual(seen[(0, 1, 1, 0, 1)], "none", "a browser opened moments ago keeps even a launchable app shut")
        self.assertEqual(seen[(0, 1, 0, 1, 1)], "browser", "Restart now reopens the browser, never the app over it")


if __name__ == "__main__":
    unittest.main()
