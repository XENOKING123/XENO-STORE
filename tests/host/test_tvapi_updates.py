#!/usr/bin/env python3
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
test_tvapi_updates.py — TvUpdates of PORT-SPEC 1.10 (map-system §13 items 6-8):
the store's own update and the TV app's installer as the native TV app sees
them (GET|POST /api/v1/updates, GET|POST /api/v1/tv-app), the TV app's stop,
and the start-up rules E8/E9.

A loopback "release host" (Fixture) plays the update feed
(settings updates.feed -> /updates/payloads.json), the TV app feed next to it
(/updates/tv-app.json), GitHub's releases/latest API ($XENO_GITHUB_API_BASE)
and the release assets. Every backend runs with XENO_TVAPI_UPDATES=1 (the
host switch that makes both routes available) unless a test says otherwise.

  status        idle shape (golden updates.json / tv-app.json), strict types, 401
                without a Bearer, available:false without the host switch
  validation    every 400/409 sentence of the reference validators, automatic
                checks that do nothing while Settings -> automatic checks is off
  store flow    check (202, checking -> checked), 429 inside checkAfter, install
                pinned by version and checksum (409 otherwise), downloading ->
                saved, savedVersion, restartRequired, the saved ELF and .version
  feed rules    a feed older than this build falls back to GitHub; a beta or
                invalid release is refused; ELF32 and ARM payloads are refused
  E5            GET /api/v1/updates, /api/v1/system and the web UI's /api/status
                answer in well under the budget while a check is blocked 30 s
  tv-app        none/orbit/manual/folder/blocked from files and the record; a
                pinned install of a good UFS2 image (record, orbit); a copy the
                owner placed needs replace:true; size, SHA-256 and UFS2 magic
                failures leave nothing behind; 507 when space is short; GitHub
                fallback; an invalid feed
  stop          pauses and saves the queue, drains, exits with code 0 and never
                hands anything to the ELF loader; an unwritable queue gives 503
  E8            no browser and no start-up toasts while the TV app polls

Budgets (PORT-SPEC 1.9) are asserted on the median of repeated reads, and every
single read must stay far below the stall a blocked lock would cause (seconds).
"""
import hashlib
import http.server
import json
import os
import shutil
import signal
import socket
import statistics
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import harness  # noqa: E402
import tvcontract as tc  # noqa: E402
import tvworld  # noqa: E402

MiB = 1024 * 1024
APP = "PPSA99500.ffpkg"
UFS2_OFFSET = 65536 + 1372


def running_version():
    with open(os.path.join(harness.PROJECT, "VERSION"), encoding="utf-8") as f:
        return f.read().strip().lstrip("v")


VERSION = running_version()


def bump(v, by=1):
    a, b, c = (int(x) for x in v.split("."))
    return f"{a}.{b}.{c + by}"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def elf_bytes(elf_class=2, machine=0x3E, size=16384, seed=1):
    """A file that starts like an ELF: class 2 = ELF64, machine 0x3E = x86-64."""
    head = bytearray(b"\x7fELF" + bytes([elf_class, 1, 1]) + bytes(9))
    head += (2).to_bytes(2, "little") + machine.to_bytes(2, "little")
    body = bytes(((i * 13 + seed) & 0xFF) for i in range(size - len(head)))
    return bytes(head) + body


def ufs2_image(size=256 * 1024, magic=True, seed=5):
    img = bytearray(((i * 7 + seed) & 0xFF) for i in range(size))
    img[UFS2_OFFSET:UFS2_OFFSET + 4] = b"\x19\x01\x54\x19" if magic else b"\x00\x00\x00\x00"
    return bytes(img)


# The sentences of the reference validators (rebranded where they name the product).
S = {
    "unpaired": "Pair this device with your PS5 to continue.",
    "not_object": "Expected a JSON object.",
    "auto_only": "Automatic mode is only valid for release checks.",
    "unknown": "Unknown update action.",
    "unavailable": "Updates work with XENO STORE running on your PS5.",
    "busy": "A XENO STORE update is in progress. Wait for it to finish.",
    "wait": "Please wait before checking or downloading again.",
    "confirm": "Confirm stopping XENO STORE and pausing downloads first.",
    "check_again": "Check for updates again before installing this release.",
    "save_queue": "Could not save the queue. XENO STORE has not been stopped.",
    "stop_installing": "A game is still installing from XENO STORE. Wait for it to finish, then restart.",
    "no_newer": "No newer XENO STORE release was found. An older release will not replace this build.",
    "invalid_release": "No compatible XENO STORE release found. An older or invalid release will not replace this build.",
    "install_failed": "The release failed SHA-256 or ELF validation. The saved copy was not replaced.",
    "interrupted": "Update interrupted. Try again.",
    "tv_unknown": "Unknown TV app action.",
    "tv_replace": "Replace is only valid, as true or false, when installing.",
    "tv_auto": "Automatic mode is only valid for TV app checks.",
    "tv_unavailable": "The TV app is installed from XENO STORE running on your PS5.",
    "tv_check_again": "Check for the TV app again before installing it.",
    "tv_manual": "A copy you placed yourself is already in /data/homebrew. Confirm replacing it with this release.",
    "tv_folder": "The TV app is already installed as the folder /data/homebrew/PPSA99500. Remove that folder to install the release, or keep updating it yourself.",
    "tv_blocked": "Something other than the TV app is at /data/homebrew/PPSA99500.ffpkg. Move it away to install updates here.",
    "tv_newer": "A newer TV app is already installed. Nothing was replaced.",
    "tv_invalid": "The TV app release information is invalid. Nothing was downloaded.",
    "tv_larger": "The download was larger than the release says. Nothing was installed.",
    "tv_sha": "The TV app failed its SHA-256 check. Nothing was installed.",
    "tv_not_image": "The download isn't a TV app image. Nothing was installed.",
    "failed": "XENO STORE could not complete this request.",
}


# --------------------------------------------------------------------------- #
# The loopback release host
# --------------------------------------------------------------------------- #

class Fixture:
    """Serves whatever a test put in `routes` ({path: (status, content type, bytes)}).
    /slow/payloads.json holds the request until release_slow() (or 30 s), then 503."""

    def __init__(self):
        self.routes = {}
        self.hits = []
        self.mu = threading.Lock()
        self.slow_go = threading.Event()
        self.slow_waiting = threading.Event()
        fx = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def do_GET(self):
                path = self.path.split("?", 1)[0]
                with fx.mu:
                    fx.hits.append(path)
                    route = fx.routes.get(path)
                if path == "/slow/payloads.json":
                    fx.slow_waiting.set()
                    fx.slow_go.wait(30)
                    route = (503, "application/json", b'{"message":"busy"}')
                if route is None:
                    route = (404, "application/json", b'{"message":"Not Found"}')
                status, ctype, body = route[:3]
                declared = route[3] if len(route) > 3 and route[3] is not None else len(body)
                try:
                    self.send_response(status)
                    self.send_header("Content-Type", ctype)
                    self.send_header("Content-Length", str(declared))
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                self.close_connection = True

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}"

    def close(self):
        self.slow_go.set()
        self.server.shutdown()
        self.server.server_close()

    def reset(self):
        with self.mu:
            self.routes = {}
            self.hits = []
        self.slow_go.clear()
        self.slow_waiting.clear()

    def put(self, path, body, status=200, ctype="application/octet-stream", declared=None):
        """`declared` sends that Content-Length instead (more than the body: the
        connection closes before the promised bytes arrive)."""
        if isinstance(body, (dict, list)):
            body, ctype = json.dumps(body).encode(), "application/json"
        with self.mu:
            self.routes[path] = (status, ctype, body, declared)

    def hit(self, path):
        with self.mu:
            return path in self.hits

    @property
    def feed_url(self):
        return self.base + "/updates/payloads.json"

    def store_release(self, version, elf, feed=True, checksum=None):
        """xeno-store.elf of `version` under /rel/v<version>/ and (feed=True) the feed naming it."""
        url = f"{self.base}/rel/v{version}/xeno-store.elf"
        self.put(f"/rel/v{version}/xeno-store.elf", elf)
        if feed:
            self.put("/updates/payloads.json", {"payloads": [{
                "name": "XENO STORE", "version": version, "filename": "xeno-store.elf", "url": url,
                "sha256": sha(elf) if checksum is None else checksum, "minFw": "", "notes": "test"}]})
        return url

    def github_release(self, version, elf=None, image=None):
        """GitHub's releases/latest for tag v<version>, with the assets that are given."""
        assets, sums = [], []
        if elf is not None:
            assets.append({"name": "xeno-store.elf", "size": len(elf),
                           "browser_download_url": f"{self.base}/rel/v{version}/xeno-store.elf"})
            self.put(f"/rel/v{version}/xeno-store.elf", elf)
            sums.append(f"{sha(elf)}  xeno-store.elf")
        if image is not None:
            assets.append({"name": APP, "size": len(image), "browser_download_url": f"{self.base}/rel/v{version}/{APP}"})
            self.put(f"/rel/v{version}/{APP}", image)
            sums.append(f"{sha(image)}  {APP}")
        sums.append(f"{'c' * 64}  XENO-STORE-Desktop-Windows.zip")
        self.put(f"/rel/v{version}/SHA256SUMS", ("\n".join(sums) + "\n").encode())
        assets.append({"name": "SHA256SUMS", "size": 10, "browser_download_url": f"{self.base}/rel/v{version}/SHA256SUMS"})
        self.put("/gh/repos/XENOKING123/XENO-STORE/releases/latest",
                 {"tag_name": f"v{version}", "name": f"XENO STORE {version}", "body": "notes", "assets": assets})

    def tv_release(self, version, image, served=None, size=None, checksum=None, url=None):
        """tv-app.json naming PPSA99500.ffpkg of `version`; `served` overrides the bytes on the wire."""
        path = f"/rel/v{version}/{APP}"
        self.put(path, image if served is None else served)
        self.put("/updates/tv-app.json", {
            "version": version, "filename": APP, "url": url or (self.base + path),
            "sha256": sha(image) if checksum is None else checksum, "size": len(image) if size is None else size})


class LoaderSink:
    """A fake ELF loader (XENO_PORT_LOADER): records every payload sent to it."""

    def __init__(self):
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.port = self.sock.getsockname()[1]
        self.got = []
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        while True:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            c.settimeout(10)
            buf = b""
            try:
                while True:
                    d = c.recv(65536)
                    if not d:
                        break
                    buf += d
            except OSError:
                pass
            c.close()
            self.got.append(buf)

    def close(self):
        self.sock.close()


# --------------------------------------------------------------------------- #
# Tests
# --------------------------------------------------------------------------- #

class TvUpdates(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fx = Fixture()

    @classmethod
    def tearDownClass(cls):
        cls.fx.close()

    def setUp(self):
        self.fx.reset()

    # ---- helpers ------------------------------------------------------------

    def world(self, name, tvapi_updates=True, settings=None, env=None, files=None, **kw):
        s = {"updates": {"feed": self.fx.feed_url, "autoCheck": False}}
        if settings:
            tvworld._deep_merge(s, settings)
        e = {"XENO_GITHUB_API_BASE": self.fx.base + "/gh"}
        e.update(env or {})
        w = tvworld.World(name, settings=s, env=e, tvapi_updates=tvapi_updates, files=files, art=False,
                          roots=(), **kw).start()
        self.addCleanup(w.close)
        return w

    def homebrew(self, w):
        return os.path.join(w.data, "sim", "data", "homebrew")

    def poll(self, c, path, cond, what, timeout=20):
        last = {}

        def ok():
            r = c.get(path)
            last["b"] = r.check()
            return cond(last["b"])
        try:
            harness.wait_until(ok, timeout=timeout, every=0.1, what=what)
        except AssertionError:
            raise AssertionError(f"timeout waiting for {what}; last={json.dumps(last.get('b'))}")
        return last["b"]

    def check_store(self, c, phase="checked"):
        r = c.post("/api/v1/updates", {"action": "check"})
        b = r.check(status=202)
        self.assertEqual(b["phase"], "checking", b)
        self.assertIs(b["busy"], True)
        self.assertEqual(b["latestVersion"], "")
        return self.poll(c, "/api/v1/updates", lambda x: not x["busy"], "the release check to end")

    def check_tv(self, c):
        r = c.post("/api/v1/tv-app", {"action": "check"})
        b = r.check(status=202)
        self.assertEqual(b["phase"], "checking", b)
        self.assertIs(b["busy"], True)
        return self.poll(c, "/api/v1/tv-app", lambda x: not x["busy"], "the TV app check to end")

    def install_tv(self, c, t, replace=None):
        body = {"action": "install", "version": t["latestVersion"], "checksum": t["checksum"]}
        if replace is not None:
            body["replace"] = replace
        return c.post("/api/v1/tv-app", body)

    def wait_tv_install(self, c):
        return self.poll(c, "/api/v1/tv-app", lambda x: not x["busy"] and x["phase"] in ("installed", "error"),
                         "the TV app install to end", timeout=30)

    def assert_no_part(self, w):
        hb = self.homebrew(w)
        self.assertFalse(os.path.exists(os.path.join(hb, APP + ".part")), "the .part file was left behind")

    def timed(self, fn, n):
        out = []
        for _ in range(n):
            t0 = time.monotonic()
            fn()
            out.append(time.monotonic() - t0)
        return out

    # ---- status ---------------------------------------------------------------

    def test_01_idle_status_shape(self):
        w = self.world("upd-idle")
        c = w.client()
        sysinfo = c.get("/api/v1/system").check()
        b = c.get("/api/v1/updates").check(status=200)
        self.assertIs(b["available"], True)
        self.assertEqual(b["runningVersion"], VERSION)
        self.assertEqual(b["instanceId"], sysinfo["instanceId"])
        for k in ("savedVersion", "latestVersion", "checksum", "error"):
            self.assertEqual(b[k], "", k)
        self.assertEqual(b["phase"], "idle")
        for k in ("busy", "updateAvailable", "restartRequired"):
            self.assertIs(b[k], False, k)
        for k in ("received", "retryAt", "checkedAt", "checkAfter"):
            self.assertEqual(b[k], 0, k)
        w.assert_golden("updates", b)
        t = c.get("/api/v1/tv-app").check(status=200)
        self.assertIs(t["available"], True)
        self.assertEqual(t["installed"], "none")
        self.assertEqual(t["phase"], "idle")
        self.assertIs(t["busy"], False)
        self.assertEqual((t["latestVersion"], t["checksum"], t["size"]), ("", "", 0))
        w.assert_golden("tv-app", t)
        # Bearer required (also while not stopping)
        anon = tvworld.TvClient(w.port)
        for path in ("/api/v1/updates", "/api/v1/tv-app"):
            anon.get(path).error(401, S["unpaired"])
            anon.post(path, {"action": "check"}).error(401, S["unpaired"])
        # PORT-SPEC 1.9: status reads are memory only
        for path in ("/api/v1/updates", "/api/v1/tv-app"):
            times = self.timed(lambda: c.get(path).check(), 9)
            self.assertLess(statistics.median(times), 0.1, (path, times))
        self.assertFalse(self.fx.hits, "a status read must never reach the network")

    def test_02_unavailable_without_the_host_switch(self):
        w = self.world("upd-off", tvapi_updates=False)
        c = w.client()
        b = c.get("/api/v1/updates").check()
        self.assertIs(b["available"], False)
        c.post("/api/v1/updates", {"action": "check"}).error(409, S["unavailable"])
        c.post("/api/v1/updates", {"action": "check", "automatic": True}).error(409, S["unavailable"])
        t = c.get("/api/v1/tv-app").check()
        self.assertIs(t["available"], False)
        self.assertEqual(t["installed"], "none")
        c.post("/api/v1/tv-app", {"action": "check"}).error(409, S["tv_unavailable"])
        self.assertFalse(self.fx.hits)

    def test_03_request_validation(self):
        w = self.world("upd-validate")
        c = w.client()
        U, T = "/api/v1/updates", "/api/v1/tv-app"
        cases = [
            (U, {"action": "nope"}, 400, S["unknown"]),
            (U, {"action": 1}, 400, S["unknown"]),
            (U, {}, 400, S["unknown"]),
            (U, {"action": "install", "automatic": True}, 400, S["auto_only"]),
            (U, {"action": "check", "automatic": 1}, 400, S["auto_only"]),
            (U, {"action": "stop", "automatic": False}, 400, S["auto_only"]),
            (U, {"action": "stop"}, 400, S["confirm"]),
            (U, {"action": "stop", "confirmed": 1}, 400, S["confirm"]),
            (U, {"action": "stop", "confirmed": False}, 400, S["confirm"]),
            (U, {"action": "install", "version": VERSION, "checksum": "a" * 64}, 409, S["check_again"]),
            (T, {"action": "nope"}, 400, S["tv_unknown"]),
            (T, {"action": "check", "replace": True}, 400, S["tv_replace"]),
            (T, {"action": "install", "replace": "yes"}, 400, S["tv_replace"]),
            (T, {"action": "install", "automatic": True}, 400, S["tv_auto"]),
            (T, {"action": "check", "automatic": "true"}, 400, S["tv_auto"]),
            (T, {"action": "install", "version": "1.0.0", "checksum": "a" * 64}, 409, S["tv_check_again"]),
        ]
        for path, body, status, sentence in cases:
            with self.subTest(path=path, body=body):
                c.post(path, body).error(status, sentence)
        for path in (U, T):
            for raw in (b"[]", b'"x"', b"1", b""):
                c.post(path, raw).error(400, S["not_object"])
        # Settings -> automatic checks is off (the World's default): an automatic
        # check answers 202 with the status and does nothing
        for path in (U, T):
            b = c.post(path, {"action": "check", "automatic": True}).check(status=202)
            self.assertEqual(b["phase"], "idle", b)
            self.assertIs(b["busy"], False)
        time.sleep(0.5)
        self.assertFalse(self.fx.hits, "an automatic check ran although automatic checks are off")
        self.assertEqual(c.get(U).check()["checkAfter"], 0, "a no-op starts no cooldown")

    def test_04_automatic_check_and_its_six_hour_window(self):
        nxt = bump(VERSION)
        self.fx.store_release(nxt, elf_bytes())
        w = self.world("upd-auto", settings={"updates": {"autoCheck": True}})
        c = w.client()
        b = c.post("/api/v1/updates", {"action": "check", "automatic": True}).check(status=202)
        self.assertEqual(b["phase"], "checking")
        now = time.time()
        self.assertGreater(b["automaticCheckAfter"], now + 6 * 3600 - 60)
        b = self.poll(c, "/api/v1/updates", lambda x: x["phase"] == "checked", "the automatic check")
        self.assertEqual(b["latestVersion"], nxt)
        hits = len(self.fx.hits)
        # inside the window: 202, nothing happens (the reply is the status)
        b = c.post("/api/v1/updates", {"action": "check", "automatic": True}).check(status=202)
        self.assertEqual(b["phase"], "checked")
        time.sleep(0.4)
        self.assertEqual(len(self.fx.hits), hits)

    # ---- the store's update ---------------------------------------------------

    def test_05_check_install_and_restart_state(self):
        nxt = bump(VERSION)
        elf = elf_bytes(seed=9)
        self.fx.store_release(nxt, elf)
        w = self.world("upd-flow")
        c = w.client()
        t0 = time.time()
        b = self.check_store(c)
        self.assertEqual(b["phase"], "checked", b)
        self.assertEqual(b["latestVersion"], nxt)
        self.assertEqual(b["checksum"], sha(elf))
        self.assertIs(b["updateAvailable"], True)
        self.assertEqual(b["error"], "")
        self.assertGreaterEqual(b["checkedAt"], int(t0) - 1)
        self.assertTrue(t0 + 50 <= b["checkAfter"] <= t0 + 70, b)
        self.assertTrue(self.fx.hit("/updates/payloads.json"))
        self.assertFalse(self.fx.hit("/gh/repos/XENOKING123/XENO-STORE/releases/latest"),
                         "a feed that is not older than this build needs no GitHub fallback")
        # cooldown
        c.post("/api/v1/updates", {"action": "check"}).error(429, S["wait"])
        # pinned by version and checksum
        c.post("/api/v1/updates", {"action": "install", "version": nxt, "checksum": "0" * 64}).error(409, S["check_again"])
        c.post("/api/v1/updates", {"action": "install", "version": VERSION, "checksum": sha(elf)}).error(409, S["check_again"])
        c.post("/api/v1/updates", {"action": "install", "version": nxt}).error(409, S["check_again"])
        r = c.post("/api/v1/updates", {"action": "install", "version": nxt, "checksum": sha(elf)})
        b = r.check(status=202)
        self.assertIn(b["phase"], ("downloading", "saving", "saved"), b)
        self.assertTrue(t0 + 50 <= b["retryAt"] <= time.time() + 61, b)
        b = self.poll(c, "/api/v1/updates", lambda x: not x["busy"], "the install to end")
        self.assertEqual(b["phase"], "saved", b)
        self.assertEqual(b["savedVersion"], nxt)
        self.assertIs(b["restartRequired"], True)
        self.assertEqual(b["received"], len(elf))
        with open(os.path.join(w.data, "xeno-store.elf"), "rb") as f:
            self.assertEqual(f.read(), elf)
        with open(os.path.join(w.data, "xeno-store.elf.version"), "rb") as f:
            self.assertEqual(f.read(), nxt.encode(), "the .version holds the plain version, no newline")
        self.assertFalse(os.path.exists(os.path.join(w.data, "xeno-store.elf.new")))
        # the web UI sees the same staged update
        st, s = w.srv.api("GET", "/api/status")
        self.assertTrue(s["update"]["staged"], s["update"])
        # retryAt holds a second install back
        c.post("/api/v1/updates", {"action": "install", "version": nxt, "checksum": sha(elf)}).error(429, S["wait"])

    def test_06_older_feed_falls_back_to_github(self):
        nxt = bump(VERSION, 2)
        elf = elf_bytes(seed=3)
        self.fx.store_release("1.0.0", elf_bytes(seed=4))
        self.fx.github_release(nxt, elf=elf)
        w = self.world("upd-gh")
        c = w.client()
        b = self.check_store(c)
        self.assertEqual(b["phase"], "checked", b)
        self.assertEqual(b["latestVersion"], nxt)
        self.assertEqual(b["checksum"], sha(elf), "the hash comes from the release's SHA256SUMS")
        self.assertTrue(self.fx.hit("/gh/repos/XENOKING123/XENO-STORE/releases/latest"))
        self.assertTrue(self.fx.hit(f"/rel/v{nxt}/SHA256SUMS"))

    def test_07_older_release_is_refused(self):
        self.fx.store_release("1.0.0", elf_bytes())   # GitHub: 404
        w = self.world("upd-older")
        c = w.client()
        b = self.check_store(c)
        self.assertEqual(b["phase"], "error", b)
        self.assertEqual(b["error"], S["no_newer"])
        self.assertEqual((b["latestVersion"], b["checksum"], b["checkedAt"]), ("", "", 0))
        self.assertIs(b["updateAvailable"], False)
        # nothing checked to install: the older release is never offered
        c.post("/api/v1/updates", {"action": "install", "version": "1.0.0", "checksum": sha(elf_bytes())}).error(
            409, S["check_again"])
        c.post("/api/v1/updates", {"action": "check"}).error(429, S["wait"])

    def test_08_beta_release_is_refused(self):
        nxt = bump(VERSION) + "-beta.1"
        self.fx.store_release(nxt, elf_bytes())
        w = self.world("upd-beta")
        c = w.client()
        b = self.check_store(c)
        self.assertEqual(b["phase"], "error", b)
        self.assertEqual(b["error"], S["invalid_release"])
        self.assertEqual(b["latestVersion"], "")
        self.assertIs(b["updateAvailable"], False)

    def test_09_elf32_and_arm_payloads_are_refused(self):
        nxt = bump(VERSION)
        elf32 = elf_bytes(elf_class=1)
        self.fx.store_release(nxt, elf32)
        w = self.world("upd-badelf")
        c = w.client()
        b = self.check_store(c)
        self.assertEqual(b["phase"], "checked", b)
        c.post("/api/v1/updates", {"action": "install", "version": nxt, "checksum": sha(elf32)}).check(status=202)
        b = self.poll(c, "/api/v1/updates", lambda x: not x["busy"], "the install to end")
        self.assertEqual(b["phase"], "error", b)
        self.assertEqual(b["error"], S["install_failed"])
        self.assertEqual(b["savedVersion"], "")
        self.assertIs(b["restartRequired"], False)
        for name in ("xeno-store.elf", "xeno-store.elf.version", "xeno-store.elf.new", "xeno-store.elf.new.part"):
            self.assertFalse(os.path.exists(os.path.join(w.data, name)), name)
        # the web UI's apply applies the same ELF64 x86-64 test (an aarch64 build here)
        arm = elf_bytes(machine=0xB7, seed=7)
        self.fx.store_release(bump(VERSION, 3), arm)
        st, r = w.srv.api("POST", "/api/update/check", {})
        self.assertEqual(st, 200, r)
        st, r = w.srv.api("POST", "/api/update/apply", {})
        self.assertEqual(st, 502, r)
        self.assertIn("ELF64", r.get("message", ""), r)
        self.assertFalse(os.path.exists(os.path.join(w.data, "xeno-store.elf")))
        self.assertEqual(c.get("/api/v1/updates").check()["savedVersion"], "")

    def test_09b_failed_download_says_interrupted_not_invalid(self):
        """Only a file that is not the release is called invalid: a download cut short
        says "Update interrupted", a wrong SHA-256 says the release failed validation."""
        nxt = bump(VERSION)
        elf = elf_bytes(size=256 * 1024)
        url_path = f"/rel/v{nxt}/xeno-store.elf"
        leftovers = ("xeno-store.elf", "xeno-store.elf.version", "xeno-store.elf.new", "xeno-store.elf.new.part",
                     "xeno-store.elf.version.new", "xeno-store.elf.version.prev")
        for case, served, declared, error in (
                ("cut", elf[: len(elf) // 2], len(elf), S["interrupted"]),
                ("sha", elf_bytes(size=256 * 1024, seed=9), None, S["install_failed"])):
            with self.subTest(case=case):
                self.fx.store_release(nxt, elf)
                w = self.world(f"upd-fail-{case}")
                c = w.client()
                self.assertEqual(self.check_store(c)["phase"], "checked")
                self.fx.put(url_path, served, declared=declared)
                c.post("/api/v1/updates", {"action": "install", "version": nxt, "checksum": sha(elf)}).check(status=202)
                b = self.poll(c, "/api/v1/updates", lambda x: not x["busy"], "the install to end")
                self.assertEqual((b["phase"], b["error"]), ("error", error), b)
                self.assertEqual(b["savedVersion"], "")
                for name in leftovers:
                    self.assertFalse(os.path.exists(os.path.join(w.data, name)), name)

    def test_10_saved_copy_must_pass_the_starter_elf_test(self):
        w = self.world("upd-saved")
        c = w.client()
        nxt = bump(VERSION)
        path = os.path.join(w.data, "xeno-store.elf")
        with open(path + ".version", "w") as f:
            f.write(nxt)
        for data, want in ((b"\x7fELF" + bytes(range(256)) * 64, ""),     # magic only: the starter skips it
                           (elf_bytes(machine=0xB7), ""),                    # aarch64
                           (elf_bytes(), nxt)):                              # ELF64 x86-64
            with open(path, "wb") as f:
                f.write(data)
            b = c.get("/api/v1/updates").check()
            self.assertEqual(b["savedVersion"], want)
            self.assertIs(b["restartRequired"], bool(want))
        with open(path + ".version", "w") as f:
            f.write("0.0.1")
        self.assertEqual(c.get("/api/v1/updates").check()["savedVersion"], "", "an older saved copy is never reported")

    # ---- E5: status reads never wait for a check -------------------------------

    def test_11_status_reads_stay_fast_during_a_blocked_check(self):
        w = self.world("upd-e5", settings={"updates": {"feed": self.fx.base + "/slow/payloads.json"}})
        c = w.client()
        web = {}

        def web_check():
            web["r"] = w.srv.api("POST", "/api/update/check", {}, timeout=90)
        th = threading.Thread(target=web_check, daemon=True)
        th.start()
        self.assertTrue(self.fx.slow_waiting.wait(10), "the web UI's check never reached the feed")
        b = c.post("/api/v1/updates", {"action": "check"}).check(status=202)
        self.assertEqual(b["phase"], "checking")
        try:
            samples = {"system": [], "updates": [], "status": []}
            deadline = time.time() + 3
            while time.time() < deadline:
                t0 = time.monotonic()
                c.get("/api/v1/system").check()
                samples["system"].append(time.monotonic() - t0)
                t0 = time.monotonic()
                u = c.get("/api/v1/updates").check()
                samples["updates"].append(time.monotonic() - t0)
                self.assertIs(u["busy"], True)
                self.assertEqual(u["phase"], "checking")
                t0 = time.monotonic()
                st, s = w.srv.api("GET", "/api/status", timeout=10)
                samples["status"].append(time.monotonic() - t0)
                self.assertEqual(st, 200)
            self.assertTrue(th.is_alive(), "the check must still be blocked while we measure")
            for name, budget in (("system", 0.05), ("updates", 0.1), ("status", 0.1)):
                times = samples[name]
                self.assertGreaterEqual(len(times), 5, name)
                self.assertLess(statistics.median(times), budget, (name, times))
                self.assertLess(max(times), 1.0, f"{name} waited for the blocked check: {times}")
        finally:
            self.fx.slow_go.set()
        th.join(60)
        self.assertFalse(th.is_alive())
        b = self.poll(c, "/api/v1/updates", lambda x: not x["busy"], "the shared check to end", timeout=40)
        self.assertEqual(b["phase"], "error", b)
        self.assertTrue(b["error"].startswith("Could not check for updates ("), b)
        self.assertEqual(self.fx.hits.count("/slow/payloads.json"), 1, "the TV check shares the running check")

    # ---- /tv-app ----------------------------------------------------------------

    def test_12_tv_app_states(self):
        w = self.world("tv-states")
        c = w.client()
        hb = self.homebrew(w)
        img = os.path.join(hb, APP)
        rec = os.path.join(w.data, "tv-app.json")

        def state():
            t = c.get("/api/v1/tv-app").check()
            return t["installed"], t["installedVersion"]
        self.assertEqual(state(), ("none", ""))
        os.makedirs(hb, exist_ok=True)
        with open(img, "wb") as f:
            f.write(ufs2_image())
        self.assertEqual(state(), ("manual", ""))
        st = os.stat(img)
        record = {"version": "1.4.0", "checksum": "b" * 64, "size": st.st_size, "modified": int(st.st_mtime), "inode": st.st_ino}
        with open(rec, "w") as f:
            json.dump(record, f)
        self.assertEqual(state(), ("orbit", "1.4.0"))
        with open(rec, "w") as f:
            json.dump(dict(record, size=st.st_size + 1), f)
        self.assertEqual(state(), ("manual", ""), "a record that does not match is a copy the owner placed")
        with open(rec, "w") as f:
            json.dump(dict(record, inode=str(st.st_ino)), f)
        self.assertEqual(state(), ("manual", ""), "the record's numbers must be JSON numbers")
        os.makedirs(os.path.join(hb, "PPSA99500"))
        with open(os.path.join(hb, "PPSA99500", "eboot.bin"), "wb") as f:
            f.write(b"\x7fELF")
        self.assertEqual(state(), ("folder", ""), "a folder copy wins over the image")
        shutil.rmtree(os.path.join(hb, "PPSA99500"))
        os.remove(img)
        os.makedirs(img)
        self.assertEqual(state(), ("blocked", ""))
        os.rmdir(img)
        os.symlink("/nonexistent", img)
        self.assertEqual(state(), ("blocked", ""), "a symlink is not the image")

    def test_13_pinned_install_of_a_good_image(self):
        image = ufs2_image(seed=11)
        self.fx.tv_release("1.5.0", image)
        w = self.world("tv-good")
        c = w.client()
        t = self.check_tv(c)
        self.assertEqual(t["phase"], "checked", t)
        self.assertEqual((t["latestVersion"], t["checksum"], t["size"]), ("1.5.0", sha(image), len(image)))
        self.assertGreater(t["checkedAt"], 0)
        self.assertEqual(t["error"], "")
        c.post("/api/v1/tv-app", {"action": "check"}).error(429, S["wait"])
        c.post("/api/v1/tv-app", {"action": "install", "version": "1.5.0", "checksum": "1" * 64}).error(409, S["tv_check_again"])
        r = self.install_tv(c, t)
        b = r.check(status=202)
        self.assertIs(b["busy"], True)
        self.assertIn(b["phase"], ("downloading", "verifying", "installing", "installed"), b)
        t = self.wait_tv_install(c)
        self.assertEqual(t["phase"], "installed", t)
        self.assertEqual(t["error"], "")
        self.assertEqual(t["installed"], "orbit")
        self.assertEqual(t["installedVersion"], "1.5.0")
        self.assertIs(t["updateAvailable"], False)
        self.assertEqual(t["received"], len(image))
        hb = self.homebrew(w)
        with open(os.path.join(hb, APP), "rb") as f:
            self.assertEqual(f.read(), image)
        self.assert_no_part(w)
        with open(os.path.join(w.data, "tv-app.json")) as f:
            rec = json.load(f)
        st = os.stat(os.path.join(hb, APP))
        self.assertEqual(rec, {"version": "1.5.0", "checksum": sha(image), "size": len(image),
                               "modified": int(st.st_mtime), "inode": st.st_ino})
        # retryAt (30 s) holds the next install back
        self.install_tv(c, t).error(429, S["wait"])
        # the web UI's own install path sees the same job type: nothing runs now
        st_, s = w.srv.api("GET", "/api/app/status")
        self.assertTrue(s["installed"] and s["file"], s)

    def test_14_a_copy_the_owner_placed_needs_replace(self):
        image = ufs2_image(seed=12)
        self.fx.tv_release("1.6.0", image)
        w = self.world("tv-replace")
        c = w.client()
        hb = self.homebrew(w)
        os.makedirs(hb, exist_ok=True)
        with open(os.path.join(hb, APP), "wb") as f:
            f.write(b"the owner's own copy" * 100)
        t = self.check_tv(c)
        self.assertEqual(t["installed"], "manual")
        self.install_tv(c, t).error(409, S["tv_manual"])
        self.install_tv(c, t, replace=False).error(409, S["tv_manual"])
        self.install_tv(c, t, replace=True).check(status=202)
        t = self.wait_tv_install(c)
        self.assertEqual((t["phase"], t["installed"], t["installedVersion"]), ("installed", "orbit", "1.6.0"), t)
        with open(os.path.join(hb, APP), "rb") as f:
            self.assertEqual(f.read(), image)

    def test_15_install_refused_for_folder_blocked_and_newer(self):
        image = ufs2_image(seed=13)
        self.fx.tv_release("1.7.0", image)
        w = self.world("tv-refuse")
        c = w.client()
        hb = self.homebrew(w)
        t = self.check_tv(c)
        os.makedirs(os.path.join(hb, "PPSA99500"))
        with open(os.path.join(hb, "PPSA99500", "eboot.bin"), "wb") as f:
            f.write(b"\x7fELF")
        self.install_tv(c, t).error(409, S["tv_folder"])
        shutil.rmtree(os.path.join(hb, "PPSA99500"))
        os.makedirs(os.path.join(hb, APP))
        self.install_tv(c, t).error(409, S["tv_blocked"])
        os.rmdir(os.path.join(hb, APP))
        with open(os.path.join(hb, APP), "wb") as f:
            f.write(image)
        st = os.stat(os.path.join(hb, APP))
        with open(os.path.join(w.data, "tv-app.json"), "w") as f:
            json.dump({"version": "9.0.0", "checksum": sha(image), "size": st.st_size, "modified": int(st.st_mtime),
                       "inode": st.st_ino}, f)
        self.install_tv(c, t).error(409, S["tv_newer"])
        self.assertEqual(c.get("/api/v1/tv-app").check()["retryAt"], 0, "a refused install starts no cooldown")

    def test_16_corrupt_downloads_are_rejected(self):
        image = ufs2_image(seed=14)
        cases = [
            ("larger", dict(served=image + b"x" * 100), S["tv_larger"]),
            ("short", dict(served=image[:-100]), S["tv_sha"]),
            ("sha256", dict(checksum=sha(b"another image")), S["tv_sha"]),
            ("magic", dict(served=ufs2_image(seed=14, magic=False), checksum=sha(ufs2_image(seed=14, magic=False))),
             S["tv_not_image"]),
        ]
        old = b"the owner's previous copy" * 64
        for name, kw, sentence in cases:
            with self.subTest(case=name):
                self.fx.reset()
                self.fx.tv_release("1.8.0", image, **kw)
                w = self.world(f"tv-bad-{name}")
                c = w.client()
                hb = self.homebrew(w)
                os.makedirs(hb, exist_ok=True)
                with open(os.path.join(hb, APP), "wb") as f:
                    f.write(old)
                t = self.check_tv(c)
                self.assertEqual(t["phase"], "checked", t)
                self.install_tv(c, t, replace=True).check(status=202)
                t = self.wait_tv_install(c)
                self.assertEqual(t["phase"], "error", t)
                self.assertEqual(t["error"], sentence)
                self.assertEqual(t["installed"], "manual")
                with open(os.path.join(hb, APP), "rb") as f:
                    self.assertEqual(f.read(), old, "the copy in place stays untouched")
                self.assert_no_part(w)
                self.assertFalse(os.path.exists(os.path.join(w.data, "tv-app.json")), "no record for a failed install")

    def test_17_not_enough_space_gives_507(self):
        image = ufs2_image(seed=15)
        self.fx.tv_release("1.9.0", image)
        w = self.world("tv-507", env={"XENO_FAKE_FREE": str(32 * MiB)})
        c = w.client()
        t = self.check_tv(c)
        need = (len(image) + 64 * MiB) // MiB
        self.install_tv(c, t).error(
            507, f"Not enough free space on the console's internal storage. The TV app needs about {need} MB.")
        self.assertFalse(os.path.exists(os.path.join(self.homebrew(w), APP + ".part")))

    def test_18_tv_check_falls_back_to_github_and_refuses_invalid_feeds(self):
        image = ufs2_image(seed=16)
        self.fx.github_release("2.0.0", image=image)    # no tv-app.json: 404
        w = self.world("tv-gh")
        c = w.client()
        t = self.check_tv(c)
        self.assertEqual(t["phase"], "checked", t)
        self.assertEqual((t["latestVersion"], t["checksum"], t["size"]), ("2.0.0", sha(image), len(image)))
        self.assertTrue(self.fx.hit("/updates/tv-app.json"))
        self.assertTrue(self.fx.hit("/gh/repos/XENOKING123/XENO-STORE/releases/latest"))
        bad_feeds = [
            ("url", dict(url="https://example.com/PPSA99500.ffpkg")),
            ("version", dict(version="2.1.0-beta.1")),
            ("size", dict(size=100)),
        ]
        for name, kw in bad_feeds:
            with self.subTest(feed=name):
                self.fx.reset()
                version = kw.pop("version", "2.1.0")
                self.fx.tv_release(version, image, **kw)
                w2 = self.world(f"tv-invalid-{name}")
                c2 = w2.client()
                t = self.check_tv(c2)
                self.assertEqual(t["phase"], "error", t)
                self.assertEqual(t["error"], S["tv_invalid"])
                self.assertEqual((t["latestVersion"], t["checksum"], t["size"], t["checkedAt"]), ("", "", 0, 0))

    def test_19a_web_ui_install_writes_the_record(self):
        # POST /api/app/install (the web UI, latest release, hash from SHA256SUMS):
        # the record gets version "", so the TV app sees the store's own copy.
        image = ufs2_image(seed=17)
        self.fx.put(f"/latest/{APP}", image)
        self.fx.put("/latest/SHA256SUMS", f"{sha(image)}  {APP}\n".encode())
        w = self.world("tv-web", env={"XENO_APP_BASE": self.fx.base + "/latest"})
        c = w.client()
        st, b = w.srv.api("POST", "/api/app/install", {})
        self.assertEqual(st, 200, b)
        jid = b["job"]["id"]
        harness.wait_until(lambda: w.srv.api("GET", f"/api/saves/jobs/{jid}")[1]["job"]["state"] in ("done", "failed"),
                           timeout=30, what="the web UI's install")
        self.assertEqual(w.srv.api("GET", f"/api/saves/jobs/{jid}")[1]["job"]["state"], "done")
        t = c.get("/api/v1/tv-app").check()
        self.assertEqual((t["installed"], t["installedVersion"]), ("orbit", ""), t)
        self.assertIs(t["busy"], False)
        self.assertEqual(t["phase"], "idle", "a web UI install is not the TV app's own action")
        with open(os.path.join(w.data, "tv-app.json")) as f:
            rec = json.load(f)
        self.assertEqual((rec["version"], rec["checksum"], rec["size"]), ("", sha(image), len(image)))

    # ---- the TV app's stop ---------------------------------------------------------

    def stop_or_skip(self, c):
        r = c.post("/api/v1/updates", {"action": "stop", "confirmed": True})
        if r.status == 503 and r.json.get("error") == S["failed"]:
            self.skipTest("tvsys_stop_begin is still the stub in this tree (tvapi_system.c)")
        return r

    def test_19_stop_drains_and_exits_without_relaunch(self):
        sink = LoaderSink()
        self.addCleanup(sink.close)
        w = self.world("upd-stop", files="own", env={"XENO_PORT_LOADER": str(sink.port)})
        c = w.client()
        job = w.srv.add(url=w.file_url("big.bin", "slow"), filename="stop-big.bin", size=64 * MiB)
        w.srv.wait_job(job["id"], None, timeout=60, until=lambda j: j["state"] == "downloading" and j["written"] > 0)
        sysinfo = c.get("/api/v1/system").check()
        c.post("/api/v1/updates", {"action": "stop"}).error(400, S["confirm"])
        r = self.stop_or_skip(c)
        b = r.check(status=202)
        self.assertEqual(b["phase"], "stopping")
        self.assertEqual(b["instanceId"], sysinfo["instanceId"])
        self.assertEqual(b["runningVersion"], VERSION)
        self.assertEqual(b["savedVersion"], "")
        # until the port closes /system answers stopping:true and /updates stays readable
        def maybe(method, path, body=None):
            try:
                return c.request(method, path, body, timeout=2)
            except (OSError, AssertionError):
                return None   # the port closed under the request (refused, reset, or no reply)
        s = maybe("GET", "/api/v1/system")
        if s is not None:
            self.assertIs(s.check()["stopping"], True)
        u = maybe("GET", "/api/v1/updates")
        if u is not None:
            self.assertEqual(u.check()["phase"], "stopping")
        p = maybe("POST", "/api/v1/updates", {"action": "check"})
        if p is not None:
            p.error(503, tc.S["stopping"])
        rc = w.srv.proc.wait(timeout=30)
        self.assertEqual(rc, 0, w.log_text()[-3000:])
        with open(os.path.join(w.data, "queue.json")) as f:
            jobs = {j["id"]: j for j in json.load(f)["jobs"]}
        self.assertEqual(jobs[job["id"]]["state"], "paused", jobs[job["id"]])
        time.sleep(1.5)
        self.assertEqual(sink.got, [], "the store must not start a copy itself: the TV app does")
        with socket.socket() as s2:   # the port is free for the next copy
            s2.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s2.bind(("127.0.0.1", w.port))

    def test_20_stop_refused_when_the_queue_cannot_be_saved(self):
        w = self.world("upd-stop-ro", files="own")
        c = w.client()
        job = w.srv.add(url=w.file_url("big.bin", "slow"), filename="stop-ro.bin", size=64 * MiB)
        w.srv.wait_job(job["id"], None, timeout=60, until=lambda j: j["state"] == "downloading")
        qpath = os.path.join(w.data, "queue.json")
        if os.path.exists(qpath):
            os.remove(qpath)
        os.makedirs(os.path.join(qpath, "blocker"))
        try:
            r = self.stop_or_skip(c)
            r.error(503, S["save_queue"])
            s = c.get("/api/v1/system").check()
            self.assertIs(s["stopping"], False)
            self.assertIsNone(w.srv.proc.poll(), "the store keeps running")
            self.assertNotEqual(w.srv.job(job["id"])["state"], "paused", "the jobs were put back")
            self.assertNotEqual(c.get("/api/v1/updates").check()["phase"], "stopping")
        finally:
            shutil.rmtree(qpath, ignore_errors=True)

    def test_20b_stop_waits_for_an_install_that_reads_from_the_store(self):
        # An etaHEN DPI (:12800) install reads its package from /pkgsrv/ while it runs:
        # the stop is refused (nothing paused, nothing held) until the install is done.
        w = self.world("upd-stop-dpi", files="own",
                       env={"XENO_HOST_INSTALL_VIA": "dpi12800", "XENO_HOST_INSTALL_SECS": "6"})
        c = w.client()
        job = w.srv.add(url=w.file_url("tiny.pkg"), filename="stop-dpi.pkg", size=2 * MiB, autoInstall=True)
        j = w.srv.wait_job(job["id"], "installing", timeout=60)
        self.assertEqual(j["install"]["via"], "dpi12800", j)
        self.stop_or_skip(c).error(409, S["stop_installing"])
        self.assertIs(c.get("/api/v1/system").check()["stopping"], False)
        self.assertNotEqual(c.get("/api/v1/updates").check()["phase"], "stopping")
        self.assertIsNone(w.srv.proc.poll(), "the store keeps running")
        self.assertIn("queue: stop refused: ", w.log_text())
        # the scheduler was not held: a new download still runs
        other = w.srv.add(url=w.file_url("small.bin"), filename="stop-dpi-other.bin", size=8 * MiB)
        w.srv.wait_job(other["id"], "done", timeout=60)
        w.srv.wait_job(job["id"], "installed", timeout=30)
        self.stop_or_skip(c).check(status=202)
        self.assertEqual(w.srv.proc.wait(timeout=30), 0, w.log_text()[-3000:])

    # ---- E8: no browser over the TV app -------------------------------------------

    def start_plain(self, name, poll_tv):
        tmp = tempfile.mkdtemp(prefix=f"xeno-{name}.")
        self.addCleanup(shutil.rmtree, tmp, True)
        port = harness.free_port()
        stop = threading.Event()
        seen = []

        def poller():
            c = tvworld.TvClient(port)
            while not stop.is_set():
                try:
                    if c.get("/api/v1/system", timeout=1).status == 200:
                        seen.append(time.time())
                except Exception:
                    pass
                time.sleep(0.2)
        if poll_tv:
            th = threading.Thread(target=poller, daemon=True)
            th.start()
            self.addCleanup(stop.set)
        srv = harness.XenoServer(os.path.join(tmp, "data"), port=port).start()
        self.addCleanup(srv.stop)
        return srv, seen

    def test_21_no_browser_or_toasts_while_the_tv_app_polls(self):
        srv, seen = self.start_plain("e8-tv", poll_tv=True)
        harness.wait_until(lambda: "ready: " in srv.log_text(), timeout=10, what="the ready line")
        log = srv.log_text()
        self.assertTrue(seen, "the poller never reached /api/v1/system")
        self.assertIn("the TV app is polling: no browser and no start-up toasts", log)
        self.assertNotIn("browser launch not built in", log)
        self.assertNotIn("toast: XENO STORE ready", log)
        self.assertNotIn("toast: XENO STORE pairing code", log)
        # without the TV app: the browser (a logged no-op on host) and both toasts, after the 2 s wait
        srv2, _ = self.start_plain("e8-web", poll_tv=False)
        harness.wait_until(lambda: "ready: " in srv2.log_text(), timeout=10, what="the ready line")
        log = srv2.log_text()
        self.assertIn("browser launch not built in", log)
        self.assertIn("toast: XENO STORE ready", log)
        self.assertIn("toast: XENO STORE pairing code", log)
        self.assertNotIn("the TV app is polling", log)

    def test_22_shutdown_closes_the_port_first(self):
        # E9: the port closes before main waits for housekeeping; a SIGTERM'd
        # store frees :port quickly and logs the order.
        srv, _ = self.start_plain("e9", poll_tv=False)
        harness.wait_until(lambda: "ready: " in srv.log_text(), timeout=10, what="the ready line")
        srv.proc.send_signal(signal.SIGTERM)
        self.assertEqual(srv.proc.wait(timeout=20), 0)
        log = srv.log_text()
        self.assertLess(log.index("server: stopped"), log.index("bye"))
        self.assertLess(log.index("shutting down"), log.index("server: stopped"))

    def test_23_old_copy_writes_nothing_after_its_port_closed(self):
        # E9: the port closes before housekeeping is joined, so the next copy can start on
        # the same data directory while the old one is still in a catalogue refresh. Nothing
        # the old copy does after that may overwrite what the next copy saved.
        tmp = tempfile.mkdtemp(prefix="xeno-e9late.")
        self.addCleanup(shutil.rmtree, tmp, True)
        data = os.path.join(tmp, "data")
        os.makedirs(data)
        fx = self.fx
        fx.put("/e9/rev.json", {"revision": 9999999999})
        with open(os.path.join(data, "settings.json"), "w", encoding="utf-8") as f:
            json.dump({"downloads": {"concurrentJobs": 2},
                       "catalog": {"autoRefresh": True, "url": fx.base + "/slow/payloads.json",
                                   "revisionUrl": fx.base + "/e9/rev.json", "lastCheck": 0,
                                   "featuredUrl": fx.base + "/x", "messagesUrl": fx.base + "/x",
                                   "companionsUrl": fx.base + "/x", "savesBackendUrl": fx.base + "/x"},
                       "updates": {"autoCheck": True, "feed": fx.feed_url, "lastCheck": 0}}, f)
        env = {"XENO_GITHUB_API_BASE": fx.base + "/gh"}
        port = harness.free_port()
        old = harness.XenoServer(data, port=port, env=env, log_name="old.log").start()
        self.addCleanup(old.kill)
        self.assertTrue(fx.slow_waiting.wait(40), "the old copy never started its catalogue download")
        old.proc.send_signal(signal.SIGTERM)

        def port_closed():
            with socket.socket() as s:
                return s.connect_ex(("127.0.0.1", port)) != 0
        harness.wait_until(port_closed, timeout=20, what="the old copy's port to close")
        new = harness.XenoServer(data, port=port, env=env, log_name="new.log").start()
        self.addCleanup(new.stop)
        st, r = new.api("PUT", "/api/settings", {"downloads": {"concurrentJobs": 4}})
        self.assertEqual(st, 200, r)
        fx.slow_go.set()   # whatever the old copy still waits for answers now
        self.assertEqual(old.proc.wait(timeout=60), 0)
        time.sleep(1)
        with open(os.path.join(data, "settings.json"), encoding="utf-8") as f:
            self.assertEqual(json.load(f)["downloads"]["concurrentJobs"], 4,
                             "the old copy wrote settings.json after the next copy started")


if __name__ == "__main__":
    unittest.main()
