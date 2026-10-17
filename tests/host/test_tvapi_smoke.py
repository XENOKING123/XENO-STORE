#!/usr/bin/env python3
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
test_tvapi_smoke.py — the foundation of the /api/v1 layer the native TV app
talks to (PORT-SPEC 1.2-1.4, 1.6.2, 1.8 E1-E13):

  TvGateTests       lazy start (no TV thread before the first /api/v1 request),
                    GET /api/v1/system shape and strict types, GET /api/v1/session
                    (loopback token == /api/session token, 403 from the LAN), the
                    gate order (stopping, Bearer, JSON object body) with every
                    sentence of PORT-SPEC 1.3, dispatcher 404/405 sentences, the
                    32 routes of the table, the PIN gate with a Bearer token.
  TvStorageTests    the tv-storage snapshot built from --root dirs, drive ids and
                    labels, a removed root dropping out, preferredStorage.
  DlHelperTests     the dl.c helpers through the existing /api routes: cancel a
                    failed job, cancel {deleteFiles} resets written, extract with a
                    password, the stop pause (dl_pause_all_for_stop) with a failing
                    and a working queue.json, stateHealthy.
  CatalogRefreshTests  catalog_refreshing() during a slow refresh, a second
                    refresh refused with 409 while one runs.

Every /api/v1 error body must be {"error":"<sentence>"}: an object, a non-empty
sentence that is not a machine code, and no "ok" key.
"""
import http.server
import json
import os
import re
import shutil
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402
import range_server  # noqa: E402

MiB = 1024 * 1024

S_STOPPING = "XENO STORE is stopping. It will be back in a moment."
S_UNPAIRED = "Pair this device with your PS5 to continue."
S_NOT_OBJECT = "Expected a JSON object."
S_USE_JSON = "Use application/json."
S_NOT_AVAILABLE = "This request is not available in this version of XENO STORE."
S_PAIR_AGAIN = "Pair this device again from Settings."
S_SESSION_REMOTE = "Pair this device using the code shown on your PS5."
S_ZERO = "Orbit Zero is not part of XENO STORE."

# The route table of PORT-SPEC 1.4: (method, path, gate flags).
ROUTES = [
    ("GET", "/api/v1/system", ""),
    ("GET", "/api/v1/session", "S"),
    ("GET", "/api/v1/zero/companion", ""),
    ("POST", "/api/v1/zero/companion", ""),
    ("GET", "/api/v1/sources", "S"),
    ("POST", "/api/v1/sources", "SBO"),
    ("GET", "/api/v1/catalog", "S"),
    ("GET", "/api/v1/catalog/updates", "S"),
    ("POST", "/api/v1/catalog/updates", "SBO"),
    ("GET", "/api/v1/art/no-such-game/cover", "SB"),
    ("GET", "/api/v1/art/no-such-game/shot/0", "SB"),
    ("GET", "/api/v1/favorites", "SB"),
    ("POST", "/api/v1/favorites", "SBO"),
    ("GET", "/api/v1/downloads", "SB"),
    ("POST", "/api/v1/downloads", "SBO"),
    ("POST", "/api/v1/downloads/j_000000/pause", "SBO"),
    ("GET", "/api/v1/storage", "SB"),
    ("POST", "/api/v1/storage/preferred", "SBO"),
    ("POST", "/api/v1/settings/downloads", "SBO"),
    ("POST", "/api/v1/settings/extraction", "SBO"),
    ("GET", "/api/v1/debrid", "SB"),
    ("GET", "/api/v1/browser", "SB"),
    ("POST", "/api/v1/browser", "SBO"),
    ("GET", "/api/v1/library", "SB"),
    ("POST", "/api/v1/library", "SBO"),
    ("GET", "/api/v1/library/storage", "SB"),
    ("POST", "/api/v1/library/storage", "SBO"),
    ("POST", "/api/v1/library/actions", "SBO"),
    ("GET", "/api/v1/updates", "B"),
    ("POST", "/api/v1/updates", "SBO"),
    ("GET", "/api/v1/tv-app", "SB"),
    ("POST", "/api/v1/tv-app", "SBO"),
]

RS = {}


def setUpModule():
    RS["tmp"] = tempfile.mkdtemp(prefix="xeno-tvsmoke-rs.")
    RS["httpd"], port = range_server.serve_in_thread(os.path.join(RS["tmp"], "files"))
    RS["base"] = f"http://127.0.0.1:{port}"


def tearDownModule():
    RS["httpd"].shutdown()
    shutil.rmtree(RS["tmp"], ignore_errors=True)


def bearer(token):
    return {"Authorization": "Bearer " + token}


class EnvelopeMixin:
    def assert_envelope(self, status, body, sentence=None, code=None):
        """A /api/v1 refusal: a JSON object with a human sentence in "error"."""
        if code is not None:
            self.assertEqual(status, code, body)
        self.assertIsInstance(body, dict, body)
        self.assertNotIn("ok", body, body)
        err = body.get("error")
        self.assertIsInstance(err, str, body)
        self.assertTrue(err.strip(), body)
        self.assertIsNone(re.match(r"^[a-z_]+$", err), f"a machine code, not a sentence: {err!r}")
        if sentence is not None:
            self.assertEqual(err, sentence)


class TvGateTests(unittest.TestCase, EnvelopeMixin):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="xeno-tvgate.")
        cls.stop_file = os.path.join(cls.tmp, "stopping")
        cls.root = os.path.join(cls.tmp, "usb0")
        os.makedirs(cls.root)
        cls.srv = harness.XenoServer(os.path.join(cls.tmp, "data"), roots=[cls.root],
                                     env={"XENO_TVAPI_STOPPING_FILE": cls.stop_file}).start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.stop()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def token(self):
        st, b = self.srv.api("GET", "/api/v1/session")
        self.assertEqual(st, 200, b)
        return b["token"]

    def diag(self):
        st, b = self.srv.api("GET", "/api/diagnostics")
        self.assertEqual(st, 200, b)
        return b

    # ---- lazy start (R13) ---------------------------------------------------
    def test_01_lazy_start(self):
        # The web UI's own traffic never starts the TV services.
        for path in ("/api/status", "/api/settings", "/api/queue", "/api/storage"):
            self.assertEqual(self.srv.api("GET", path)[0], 200)
        d = self.diag()
        self.assertIn("tvapi", d)
        self.assertIs(d["tvapi"]["started"], False)
        self.assertEqual(d["tvapi"]["threads"], 0)
        self.assertEqual(d["tvapi"]["routes"], len(ROUTES))
        self.assertNotIn("tvapi: thread", self.srv.log_text())
        self.assertIn("tvapi: 32 /api/v1 route(s) registered", self.srv.log_text())
        # The first /api/v1 request starts them (the tv-storage thread at least).
        st, _ = self.srv.api("GET", "/api/v1/system")
        self.assertEqual(st, 200)
        d = self.diag()
        self.assertIs(d["tvapi"]["started"], True)
        self.assertGreaterEqual(d["tvapi"]["threads"], 1)
        self.assertIn("tvapi: thread tv-storage started", self.srv.log_text())
        self.assertGreaterEqual(d["tvapi"]["clientSeenAgoMs"], 0)   # loopback GET /system (E8 hook)
        # ...and only once.
        self.srv.api("GET", "/api/v1/system")
        self.assertEqual(self.srv.log_text().count("starting the TV app services"), 1)

    # ---- GET /api/v1/system --------------------------------------------------
    def test_02_system_shape_and_types(self):
        st, hdrs, raw = self.srv.raw("GET", "/api/v1/system")
        self.assertEqual(st, 200)
        self.assertTrue(hdrs.get("Content-Type", "").startswith("application/json"))
        self.assertEqual(hdrs.get("Cache-Control"), "no-store")
        self.assertEqual(hdrs.get("Connection"), "close")
        b = json.loads(raw)
        self.assertIsInstance(b, dict)
        types = {"name": str, "version": str, "instanceId": str, "stopping": bool, "platform": str,
                 "paired": bool, "localSessionAvailable": bool, "stateHealthy": bool,
                 "catalogueRevision": int, "preferredStorage": str, "deleteArchives": bool,
                 "concurrentDownloads": int}
        for k, t in types.items():
            self.assertIn(k, b)
            self.assertIs(type(b[k]), t, f"{k} must be a JSON {t.__name__}, got {b[k]!r}")
        self.assertEqual(b["name"], "XENO STORE")
        self.assertEqual(b["version"], self.srv.api("GET", "/api/status")[1]["version"])
        self.assertRegex(b["version"], r"^\d+\.\d+\.\d+$")
        self.assertRegex(b["instanceId"], r"^[0-9a-f]{32}$")
        self.assertEqual(b["platform"], "host")                 # R12: sys_platform() as is
        self.assertIs(b["stopping"], False)
        self.assertIs(b["paired"], False)                        # no Bearer
        self.assertIs(b["localSessionAvailable"], True)          # loopback caller
        self.assertIs(b["stateHealthy"], True)
        self.assertGreater(b["catalogueRevision"], 0)
        self.assertLess(b["catalogueRevision"], 2 ** 53)
        self.assertEqual(b["preferredStorage"], "internal")
        self.assertIs(b["deleteArchives"], True)
        self.assertTrue(1 <= b["concurrentDownloads"] <= 4)
        # instanceId is stable within the process; paired follows the Bearer
        st, b2 = self.srv.api("GET", "/api/v1/system", headers=bearer(self.token()))
        self.assertEqual(b2["instanceId"], b["instanceId"])
        self.assertIs(b2["paired"], True)
        st, b3 = self.srv.api("GET", "/api/v1/system", headers=bearer("0" * 64))
        self.assertIs(b3["paired"], False)
        # the settings getters feed it
        self.srv.api("PUT", "/api/settings", {"downloads": {"concurrentJobs": 3, "deleteArchiveAfterExtract": False}})
        try:
            b4 = self.srv.api("GET", "/api/v1/system")[1]
            self.assertEqual(b4["concurrentDownloads"], 3)
            self.assertIs(b4["deleteArchives"], False)
        finally:
            self.srv.api("PUT", "/api/settings", {"downloads": {"concurrentJobs": 1, "deleteArchiveAfterExtract": True}})

    # ---- GET /api/v1/session ---------------------------------------------------
    def test_03_session_loopback(self):
        st, b = self.srv.api("GET", "/api/v1/session")
        self.assertEqual(st, 200, b)
        self.assertEqual(set(b), {"token", "pairCode", "address"})
        self.assertRegex(b["token"], r"^[0-9a-f]{64}$")
        self.assertRegex(b["pairCode"], r"^\d{6}$")
        self.assertIsInstance(b["address"], str)
        self.assertFalse(b["address"].startswith("127."), "never the loopback fallback")
        if b["address"]:
            self.assertRegex(b["address"], r"^\d+\.\d+\.\d+\.\d+$")
        st, web = self.srv.api("GET", "/api/session")
        self.assertEqual(st, 200)
        self.assertEqual(b["token"], web["token"], "the TV app and the console browser share the console's session")
        self.assertEqual(b["pairCode"], web["code"])
        st, again = self.srv.api("GET", "/api/v1/session", headers=bearer("f" * 64))
        self.assertEqual(again["token"], b["token"], "an old Bearer is ignored")

    def test_04_session_refused_from_the_lan(self):
        ip = self.srv.api("GET", "/api/status")[1].get("ip", "127.0.0.1")
        if not ip or ip.startswith("127."):
            self.skipTest("no non-loopback IPv4 on this machine")
        url = f"http://{ip}:{self.srv.port}"
        try:
            with urllib.request.urlopen(url + "/api/v1/session", timeout=10) as r:
                st, raw = r.status, r.read()
        except urllib.error.HTTPError as e:
            st, raw = e.code, e.read()
        except OSError as e:
            self.skipTest(f"cannot reach {ip}: {e}")
        self.assert_envelope(st, json.loads(raw), S_SESSION_REMOTE, 403)
        with urllib.request.urlopen(url + "/api/v1/system", timeout=10) as r:
            b = json.loads(r.read())
        self.assertIs(b["localSessionAvailable"], False)

    # ---- the gate ----------------------------------------------------------------
    def test_05_bearer_required(self):
        bad = "0" * 64
        for method, path, flags in ROUTES:
            if "B" not in flags:
                continue
            body = {} if method == "POST" else None
            for hdrs in (None, bearer(bad), {"Authorization": "Basic abc"}):
                st, b = self.srv.api(method, path, body, headers=hdrs)
                self.assert_envelope(st, b, S_UNPAIRED, 401)

    def test_06_open_routes_need_no_bearer(self):
        for method, path, flags in ROUTES:
            if "B" in flags or path.endswith("/session"):
                continue
            st, b = self.srv.api(method, path, {} if method == "POST" else None)
            self.assertNotEqual(st, 401, f"{method} {path} must not ask for a Bearer: {b}")

    def test_07_post_body_must_be_a_json_object(self):
        h = bearer(self.token())
        for method, path, flags in ROUTES:
            if "O" not in flags:
                continue
            for raw in (b"[]", b'"x"', b"1", b"null", b"true", b""):
                st, b = self.srv.api(method, path, raw, headers=h)
                self.assert_envelope(st, b, S_NOT_OBJECT, 400)
            # broken JSON: the dispatcher answers, with the same sentence
            st, b = self.srv.api(method, path, b"{not json", headers=h)
            self.assert_envelope(st, b, S_NOT_OBJECT, 400)
            # a body that is not sent as JSON
            st, b = self.srv.api(method, path, b"{}", headers=dict(h, **{"Content-Type": "text/plain"}))
            self.assert_envelope(st, b, S_USE_JSON, 415)
        # GET routes ignore the body check
        st, b = self.srv.api("GET", "/api/v1/system", None)
        self.assertEqual(st, 200)

    def test_08_dispatcher_sentences(self):
        for path in ("/api/v1/nope", "/api/v1", "/api/v1/downloads/a/b/c", "/api/v1/art/x"):
            st, b = self.srv.api("GET", path)
            self.assert_envelope(st, b, S_NOT_AVAILABLE, 404)
        for method, path in (("DELETE", "/api/v1/system"), ("POST", "/api/v1/system"), ("PUT", "/api/v1/downloads"),
                             ("GET", "/api/v1/storage/preferred"), ("GET", "/api/v1/library/actions")):
            st, b = self.srv.api(method, path, {} if method != "GET" else None)
            self.assert_envelope(st, b, S_NOT_AVAILABLE, 405)
        # the web UI's /api keeps its code envelope
        st, b = self.srv.api("GET", "/api/nope")
        self.assertEqual((st, b.get("ok"), b.get("error")), (404, False, "not_found"))
        st, b = self.srv.api("PUT", "/api/settings", b"{not json")
        self.assertEqual((st, b.get("error")), (400, "bad_json"))

    def test_09_every_route_is_registered(self):
        h = bearer(self.token())
        for method, path, _ in ROUTES:
            st, b = self.srv.api(method, path, {} if method == "POST" else None, headers=h)
            if st >= 400:
                self.assert_envelope(st, b)
                self.assertNotEqual(b["error"], S_NOT_AVAILABLE, f"{method} {path} is not routed")
        for method in ("GET", "POST"):
            st, b = self.srv.api(method, "/api/v1/zero/companion", {} if method == "POST" else None, headers=h)
            self.assert_envelope(st, b, S_ZERO, 404)

    def test_10_pin_set_bearer_still_passes(self):
        token = self.token()
        st, b = self.srv.api("PUT", "/api/settings", {"pairing": {"pin": "2468"}})
        self.assertEqual(st, 200, b)
        try:
            # no Bearer: the core PIN gate refuses, with the /api/v1 sentence
            st, b = self.srv.api("POST", "/api/v1/zero/companion", {})
            self.assert_envelope(st, b, S_PAIR_AGAIN, 401)
            st, b = self.srv.api("POST", "/api/v1/favorites", {"gameId": "x", "favorite": True})
            self.assert_envelope(st, b, S_PAIR_AGAIN, 401)
            # Bearer: passes the PIN gate and reaches the /api/v1 handlers
            st, b = self.srv.api("POST", "/api/v1/zero/companion", {}, headers=bearer(token))
            self.assert_envelope(st, b, S_ZERO, 404)
            st, b = self.srv.api("POST", "/api/v1/favorites", b"[]", headers=bearer(token))
            self.assert_envelope(st, b, S_NOT_OBJECT, 400)
            # GETs never needed the PIN
            self.assertEqual(self.srv.api("GET", "/api/v1/system")[0], 200)
        finally:
            st, _ = self.srv.api("PUT", "/api/settings", {"pairing": {"pin": ""}}, headers={"X-XENO-PIN": "2468"})
            self.assertEqual(st, 200)

    def test_11_gate_order_while_stopping(self):
        token = self.token()
        with open(self.stop_file, "w") as f:
            f.write("1")
        try:
            st, b = self.srv.api("GET", "/api/v1/system")
            self.assertEqual(st, 200, "GET /system answers while stopping")
            self.assertIs(b["stopping"], True)
            # stopping comes first: before the Bearer and before the body
            st, b = self.srv.api("GET", "/api/v1/downloads")
            self.assert_envelope(st, b, S_STOPPING, 503)
            st, b = self.srv.api("POST", "/api/v1/favorites", b"[]")
            self.assert_envelope(st, b, S_STOPPING, 503)
            st, b = self.srv.api("GET", "/api/v1/session")
            self.assert_envelope(st, b, S_STOPPING, 503)
            for method, path, flags in ROUTES:
                if "S" in flags:
                    st, b = self.srv.api(method, path, {} if method == "POST" else None, headers=bearer(token))
                    self.assert_envelope(st, b, S_STOPPING, 503)
            # GET /updates is readable while stopping, but still needs the Bearer
            st, b = self.srv.api("GET", "/api/v1/updates")
            self.assert_envelope(st, b, S_UNPAIRED, 401)
            st, b = self.srv.api("GET", "/api/v1/updates", headers=bearer(token))
            if st >= 400:
                self.assertNotEqual(b.get("error"), S_STOPPING)
            # /zero/companion has no gate at all
            st, b = self.srv.api("GET", "/api/v1/zero/companion")
            self.assert_envelope(st, b, S_ZERO, 404)
        finally:
            os.remove(self.stop_file)
        st, b = self.srv.api("GET", "/api/v1/system")
        self.assertIs(b["stopping"], False)
        # then the Bearer before the body
        st, b = self.srv.api("POST", "/api/v1/favorites", b"[]")
        self.assert_envelope(st, b, S_UNPAIRED, 401)
        st, b = self.srv.api("POST", "/api/v1/favorites", b"[]", headers=bearer(token))
        self.assert_envelope(st, b, S_NOT_OBJECT, 400)


class TvStorageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="xeno-tvstorage.")
        cls.r0 = os.path.join(cls.tmp, "usbstick")
        cls.r1 = os.path.join(cls.tmp, "second drive")
        os.makedirs(cls.r0)
        os.makedirs(cls.r1)
        cls.data = os.path.join(cls.tmp, "data")
        cls.srv = harness.XenoServer(cls.data, roots=[cls.r0, cls.r1]).start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.stop()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def tv(self):
        st, b = self.srv.api("GET", "/api/diagnostics")
        self.assertEqual(st, 200)
        return b["tvapi"]

    def wait_tv(self, cond, what, timeout=12):
        last = {}

        def ok():
            last["tv"] = self.tv()
            return cond(last["tv"])
        try:
            harness.wait_until(ok, timeout=timeout, every=0.2, what=what)
        except AssertionError:
            raise AssertionError(f"timeout waiting for {what}; last={json.dumps(last.get('tv'))[:1500]}")
        return last["tv"]

    def test_1_snapshot_from_roots(self):
        self.srv.api("GET", "/api/v1/system")   # lazy start
        tv = self.wait_tv(lambda t: t.get("storage", {}).get("updatedAt", 0) > 0, "the first tv-storage refresh")
        drives = tv["drives"]
        self.assertEqual([d["id"] for d in drives], ["root0", "root1", "internal"], drives)   # USB/roots first, internal last
        by = {d["id"]: d for d in drives}
        self.assertEqual(by["root0"]["label"], "usbstick")
        self.assertEqual(by["root1"]["label"], "second drive")
        self.assertEqual(by["internal"]["label"], "Internal storage")
        self.assertEqual(by["root0"]["path"], self.r0)
        self.assertEqual(by["internal"]["path"], os.path.join(self.data, "downloads"))
        for d in drives:
            self.assertIs(d["writable"], True)
            self.assertIs(type(d["free"]), int)
            self.assertIs(type(d["total"]), int)
            self.assertGreater(d["total"], 0)
            self.assertGreaterEqual(d["total"], d["free"])
        self.assertIs(by["root0"]["external"], True)
        self.assertIs(by["internal"]["external"], False)
        self.assertEqual(tv["storage"]["count"], 3)
        self.assertIs(tv["storage"]["thread"], True)

    def test_2_removed_root_drops_out_and_returns(self):
        self.srv.api("GET", "/api/v1/system")
        tv = self.wait_tv(lambda t: t.get("storage", {}).get("count") == 3, "three drives")
        gen = tv["storage"]["gen"]
        shutil.rmtree(self.r1)   # holds xeno-store/ from the writability probe
        try:
            tv = self.wait_tv(lambda t: [d["id"] for d in t["drives"]] == ["root0", "internal"], "root1 to drop out")
            self.assertGreater(tv["storage"]["gen"], gen)
            self.assertIn("tv-storage: the set of drives changed", self.srv.log_text())
        finally:
            os.makedirs(self.r1, exist_ok=True)
        self.wait_tv(lambda t: [d["id"] for d in t["drives"]] == ["root0", "root1", "internal"], "root1 to come back")

    def test_3_preferred_storage(self):
        st, b = self.srv.api("GET", "/api/v1/system")
        self.assertEqual(b["preferredStorage"], "internal")
        st, _ = self.srv.api("PUT", "/api/settings", {"downloads": {"defaultRoot": self.r1}})
        self.assertEqual(st, 200)
        try:
            self.assertEqual(self.srv.api("GET", "/api/v1/system")[1]["preferredStorage"], "root1")
            tv = self.wait_tv(lambda t: any(d["id"] == "root1" and d["default"] for d in t["drives"]), "root1 as default")
            self.assertEqual(tv["preferredStorage"], "root1")
        finally:
            self.srv.api("PUT", "/api/settings", {"downloads": {"defaultRoot": os.path.join(self.data, "downloads")}})

    def test_4_preferred_auto_from_tvapi_json(self):
        data = os.path.join(self.tmp, "data-auto")
        os.makedirs(data)
        with open(os.path.join(data, "tvapi.json"), "w") as f:
            json.dump({"preferredAuto": True}, f)
        srv = harness.XenoServer(data).start()
        try:
            st, b = srv.api("GET", "/api/v1/system")
            self.assertEqual(st, 200)
            self.assertEqual(b["preferredStorage"], "", "auto: the app chooses the drive")
            st, d = srv.api("GET", "/api/diagnostics")
            self.assertIs(d["tvapi"]["preferredAuto"], True)
        finally:
            srv.stop()


class DlHelperTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="xeno-tvdl.")
        cls.srv = harness.XenoServer(os.path.join(cls.tmp, "data")).start()
        cls.srv.api("PUT", "/api/settings", {"downloads": {"extractZip": True, "deleteArchiveAfterExtract": False,
                                                            "retryMax": 1, "concurrentJobs": 2}})

    @classmethod
    def tearDownClass(cls):
        cls.srv.stop()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_1_cancel_a_failed_job(self):
        job = self.srv.add(url=f"{RS['base']}/f404/small.bin", filename="gone.bin", size=8 * MiB)
        j = self.srv.wait_job(job["id"], "failed", timeout=60)
        self.assertEqual(j["state"], "failed")
        st, b = self.srv.action(job["id"], "cancel")
        self.assertEqual(st, 200, b)
        self.assertEqual(b["job"]["state"], "cancelled")
        self.srv.action(job["id"], "remove")

    def test_2_cancel_with_delete_resets_written(self):
        job = self.srv.add(url=f"{RS['base']}/slow/big.bin", filename="slow-big.bin", size=64 * MiB)
        self.srv.wait_job(job["id"], None, timeout=60, until=lambda x: x["state"] == "downloading" and x["written"] > MiB)
        st, b = self.srv.action(job["id"], "cancel", {"deleteFiles": True})
        self.assertEqual(st, 200, b)
        j = self.srv.wait_job(job["id"], None, timeout=30, until=lambda x: x["state"] == "cancelled" and x["written"] == 0)
        self.assertFalse(os.path.exists(j["path"] + ".xpart"), "the partial is deleted")
        self.srv.action(job["id"], "remove")

    def test_3_extract_with_a_password(self):
        job = self.srv.add(url=f"{RS['base']}/ok/secretpw.zip", filename="tv-secretpw.zip", format="ZIP")
        j = self.srv.wait_job(job["id"], None, timeout=90, until=lambda x: x["extract"]["state"] in ("failed", "extracted"))
        self.assertEqual(j["extract"]["state"], "failed", "no password and not a community one")
        self.assertIn("password", j["extract"]["message"])
        st, b = self.srv.action(job["id"], "extract", {"password": "xeno-secret-42"})
        self.assertEqual(st, 200, b)
        j = self.srv.wait_job(job["id"], "extracted", timeout=90)
        out = os.path.join(self.srv.data_dir, "downloads", "homebrew", "PPSA07002 Custom.ffpfsc")
        self.assertTrue(os.path.exists(out))
        self.assertNotIn("xeno-secret-42", self.srv.log_text(), "the password is never logged")

    def test_4_pause_all_for_stop(self):
        # A dedicated instance: once the stop pause succeeded nothing new starts in it.
        srv = harness.XenoServer(os.path.join(self.tmp, "data-stop")).start()
        try:
            srv.api("PUT", "/api/settings", {"downloads": {"concurrentJobs": 1}})
            a = srv.add(url=f"{RS['base']}/slow/big.bin", filename="stop-a.bin", size=64 * MiB)
            b = srv.add(url=f"{RS['base']}/slow/mid.bin", filename="stop-b.bin", size=32 * MiB)
            srv.wait_job(a["id"], None, timeout=60, until=lambda x: x["state"] == "downloading" and x["written"] > 0)
            self.assertEqual(srv.job(b["id"])["state"], "queued")
            # 1. queue.json cannot be written: refused, nothing stays paused, stateHealthy goes false
            qpath = os.path.join(srv.data_dir, "queue.json")
            os.remove(qpath)
            os.makedirs(os.path.join(qpath, "blocker"))
            try:
                st, r = srv.api("POST", "/api/debug/queue/pause-all")
                self.assertEqual(st, 503, r)
                self.assertNotEqual(srv.job(b["id"])["state"], "paused")
                self.assertNotEqual(srv.job(a["id"])["state"], "paused")
                d = srv.api("GET", "/api/diagnostics")[1]
                self.assertIs(d["queue"]["stateHealthy"], False)
                self.assertIs(srv.api("GET", "/api/v1/system")[1]["stateHealthy"], False)
            finally:
                shutil.rmtree(qpath)
            # 2. writable again: both jobs paused with the restart message, saved, workers drain to 0
            st, r = srv.api("POST", "/api/debug/queue/pause-all")
            self.assertEqual(st, 200, r)
            self.assertEqual(r["paused"], 2)
            self.assertIs(r["stateHealthy"], True)
            for jid in (a["id"], b["id"]):
                j = srv.job(jid)
                self.assertEqual(j["state"], "paused")
                self.assertEqual(j["message"], "Paused for the XENO STORE restart")
            harness.wait_until(lambda: srv.api("GET", "/api/diagnostics")[1]["queue"]["workersRunning"] == 0,
                               timeout=10, what="the job workers to exit")
            with open(qpath) as f:
                saved = {j["id"]: j["state"] for j in json.load(f)["jobs"]}
            self.assertEqual(saved.get(a["id"]), "paused")
            self.assertEqual(saved.get(b["id"]), "paused")
            self.assertIs(srv.api("GET", "/api/v1/system")[1]["stateHealthy"], True)
            # 3. the scheduler is held: a resumed job stays queued
            st, r = srv.action(b["id"], "resume")
            self.assertEqual(st, 200, r)
            time.sleep(2.5)
            self.assertEqual(srv.job(b["id"])["state"], "queued")
            self.assertEqual(srv.api("GET", "/api/diagnostics")[1]["queue"]["workersRunning"], 0)
        finally:
            srv.stop()


class _SlowRevision(http.server.BaseHTTPRequestHandler):
    delay = 3.0

    def do_GET(self):  # noqa: N802
        time.sleep(self.delay)
        body = json.dumps({"revision": 1}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class CatalogRefreshTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="xeno-tvrefresh.")
        cls.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _SlowRevision)
        cls.httpd.daemon_threads = True
        threading.Thread(target=cls.httpd.serve_forever, daemon=True).start()
        data = os.path.join(cls.tmp, "data")
        os.makedirs(data)
        with open(os.path.join(data, "settings.json"), "w") as f:
            json.dump({"catalog": {"autoRefresh": False,
                                   "revisionUrl": f"http://127.0.0.1:{cls.httpd.server_address[1]}/revision.json"}}, f)
        cls.srv = harness.XenoServer(data).start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.stop()
        cls.httpd.shutdown()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def refreshing(self):
        st, b = self.srv.api("GET", "/api/status")
        self.assertEqual(st, 200)
        self.assertIs(type(b["catalog"]["refreshing"]), bool)
        return b["catalog"]["refreshing"]

    def test_refreshing_flag_and_busy(self):
        self.assertIs(self.refreshing(), False)
        result = {}
        t = threading.Thread(target=lambda: result.update(r=self.srv.api("POST", "/api/catalog/refresh", {})))
        t.start()
        harness.wait_until(self.refreshing, timeout=5, every=0.05, what="catalog_refreshing() during the refresh")
        st, b = self.srv.api("POST", "/api/catalog/refresh", {})
        self.assertEqual(st, 409, b)
        self.assertEqual(b["error"], "refresh_busy")
        t.join(30)
        st, b = result["r"]
        self.assertEqual(st, 200, b)
        self.assertIs(b["updated"], False)   # revision 1 is not newer
        self.assertIs(self.refreshing(), False)
        # the next one runs again
        st, b = self.srv.api("POST", "/api/catalog/refresh", {})
        self.assertEqual(st, 200, b)


if __name__ == "__main__":
    unittest.main()
