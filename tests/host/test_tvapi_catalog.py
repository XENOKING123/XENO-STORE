#!/usr/bin/env python3
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
test_tvapi_catalog.py — the catalogue and system part of the /api/v1 layer the
native TV app talks to (PORT-SPEC 1.5.1, 1.5.2, 1.5.5 "Stop", 1.6.1, 1.9, 1.10):

  TvGate             the gate on the catalogue routes beyond test_tvapi_smoke.py:
                     open GETs answer without a token and meet the contract, every
                     POST wants a Bearer even with no PIN, bodies must be objects,
                     everything but GET /system answers 503 while stopping.
  TvSystem           GET /system golden shape, paired, instanceId (stable, new after
                     a restart), catalogueRevision = (revision << 20) | fnv1a(hero
                     ids) & 0xFFFFF, < 50 ms also while /api/update/check is blocked,
                     /session golden, /zero/companion 404.
  TvStopDrain        POST /api/v1/updates stop (tvsys_stop_begin): a failed queue
                     save refuses and changes nothing; then the jobs are paused in
                     queue.json, /system says stopping:true, the process exits by
                     itself and nothing is sent to the loader port (no relaunch).
                     Skipped while tvapi_update.c is still the stub.
  TvCatalogFixture   GET /catalog == tv_golden.build() on the fixture catalogue and
                     == golden/catalog.json; the R1/R2/R6 rules spot-checked; [] until
                     acknowledged; POST /sources (filter, empty list, reference
                     validation order, only the exposed ids written, a failed save
                     changes nothing); /catalog/updates; the TV_SHOW_ZIP variant.
  TvCatalogRealData  the embedded revision-10 catalogue with updates/featured.json:
                     == tv_golden.build() (4292 options, 2593 games, 2,364,377 bytes),
                     strict JSON, every item meets the contract, /catalog < 500 ms
                     and the index walk (the only catalog_mu hold) < 100 ms.
  TvCatalogRefresh   POST /catalog/updates against a local revision.json + catalogue
                     server: 400, busy during a web UI refresh (catalog_refreshing),
                     202 then 409 while busy, the new revision everywhere (system,
                     catalog, sources, gameCount), 429 inside the cooldown, a failed
                     refresh reported as a sentence.
  TvFavorites        set semantics, 404 for an unknown game only when adding, the
                     400 refusals, 409 when full, 503 with nothing changed when the
                     file cannot be written, shared with /api/favorites, bare
                     arrays, < 20 ms per GET at 10 requests per second.

Golden files: tests/host/tv/golden/{system,session,sources,sources-post,catalog,
catalog-updates,favorites}.json (XENO_TV_GOLDEN_UPDATE=1 rewrites them).
"""
import copy
import http.server
import json
import os
import re
import shutil
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402
import tv_golden  # noqa: E402
import tvcontract as tc  # noqa: E402
import tvworld  # noqa: E402

MiB = 1024 * 1024
NOT_READY = "This part of XENO STORE is not ready yet."
STOP_PAUSE = "Paused for the XENO STORE restart"
FIXTURE_REVISION = 2000000000

S_SOURCES_SAVE = "Could not save source settings. Check console storage."
S_FAV_SAVE = "Could not save favourites."
REFRESH_FAILED = re.compile(r"^Catalogue refresh failed \(.+\)\. Using your current catalogue\.$")


def catalogue_revision(revision, featured):
    """GET /system catalogueRevision (PORT-SPEC 1.5.1): (revision << 20) | (FNV-1a 32 of
    the featured feed's hero ids, each followed by one 0 byte, & 0xFFFFF). The revision
    is taken modulo 2^33 so the value stays below 2^53."""
    h = 0x811C9DC5
    hero = (featured or {}).get("hero") if isinstance(featured, dict) else None
    for s in hero if isinstance(hero, list) else []:
        if isinstance(s, str):
            for b in s.encode("utf-8") + b"\0":
                h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return ((revision & ((1 << 33) - 1)) << 20) | (h & 0xFFFFF)


def settings_doc(w):
    st, b = w.srv.api("GET", "/api/settings")
    assert st == 200, b
    return b["settings"]


def put_settings(w, doc):
    st, b = w.srv.api("PUT", "/api/settings", doc)
    assert st == 200, b
    return b["settings"]


def assert_budget(test, times, budget, what):
    """A PORT-SPEC 1.9 budget over repeated requests. Something blocking on the request
    path (a lock held across the network, a drive scan, a catalogue walk) makes every
    sample slow; one sample delayed by the scheduler of a busy test machine does not
    count, so every sample but the slowest must be within the budget, and the slowest
    within twice the budget."""
    ordered = sorted(times)
    test.assertLess(ordered[-2] if len(ordered) > 1 else ordered[-1], budget,
                    f"{what}: {len([t for t in times if t >= budget])} of {len(times)} over {budget * 1000:.0f} ms "
                    f"(slowest {ordered[-1] * 1000:.1f} ms)")
    test.assertLess(ordered[-1], 2 * budget, f"{what}: slowest {ordered[-1] * 1000:.1f} ms")


def timed(fn, n, every=0.0):
    out = []
    for _ in range(n):
        t0 = time.monotonic()
        fn()
        out.append(time.monotonic() - t0)
        if every:
            time.sleep(max(0.0, every - out[-1]))
    return out


class _Slow(http.server.BaseHTTPRequestHandler):
    """A feed that answers after `delay` seconds (a blocked update check)."""
    delay = 5.0
    hits = 0

    def do_GET(self):  # noqa: N802
        type(self).hits += 1
        time.sleep(self.delay)
        body = b'{"version":"0.0.1","url":"","sha256":""}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


class CatalogueServer:
    """Serves /revision.json and /catalog.json for the refresh tests. `catalog` is
    the document served (its "revision" is what revision.json says, unless
    `revision` overrides it); `delay` holds every answer; `fail` answers 500."""

    def __init__(self):
        srv = self

        class H(http.server.BaseHTTPRequestHandler):
            def do_GET(self):  # noqa: N802
                time.sleep(srv.delay)
                srv.hits.append(self.path)
                if srv.fail:
                    self.send_response(500)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                if self.path.startswith("/revision.json"):
                    rev = srv.revision if srv.revision is not None else (srv.catalog or {}).get("revision", 0)
                    body = json.dumps({"revision": rev}).encode()
                elif self.path.startswith("/catalog.json") and srv.catalog is not None:
                    body = json.dumps(srv.catalog).encode()
                else:
                    self.send_response(404)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *a):
                pass

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.httpd.daemon_threads = True
        self.base = f"http://127.0.0.1:{self.httpd.server_address[1]}"
        self.catalog, self.revision, self.delay, self.fail, self.hits = None, None, 0.0, False, []
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


# --------------------------------------------------------------------------- #
# The gate on the catalogue routes
# --------------------------------------------------------------------------- #

class TvGate(unittest.TestCase):
    POSTS = (("/api/v1/sources", tc.AppBody.save_sources(["archive", "vikingfile"])),
             ("/api/v1/catalog/updates", tc.AppBody.refresh_catalogue()),
             ("/api/v1/favorites", tc.AppBody.favourite("fx-tidewater-ps5", True)))

    @classmethod
    def setUpClass(cls):
        cls.w = tvworld.World("cat-gate", art=False).start()
        cls.c = cls.w.client()
        cls.anon = cls.w.client(paired=False)

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    def test_open_routes_answer_without_a_token(self):
        for path in ("/api/v1/sources", "/api/v1/catalog", "/api/v1/catalog/updates"):
            r = self.anon.get(path)
            r.check()
        # ...and with a stale token too (the app keeps sending the one it had)
        r = self.anon.get("/api/v1/sources", token="0" * 64)
        r.check()

    def test_posts_and_favorites_want_a_bearer_even_without_a_pin(self):
        for path, body in self.POSTS:
            for tok in ("", "0" * 64, "zz"):
                self.anon.post(path, body, token=tok).error(401, tc.S["unpaired"])
        self.anon.get("/api/v1/favorites").error(401, tc.S["unpaired"])
        # nothing happened behind the refusals
        self.assertEqual(self.c.get("/api/v1/favorites").check(), [])
        self.assertIs(self.c.get("/api/v1/catalog/updates").check()["busy"], False)

    def test_bodies_must_be_json_objects(self):
        for path, _ in self.POSTS:
            for raw in (b"[]", b'"x"', b"1", b"null", b"true", b""):
                self.c.post(path, raw).error(400, tc.S["not_object"])
            self.c.post(path, b"{broken").error(400, tc.S["not_object"])
            self.c.post(path, b"{}", content_type="text/plain").error(415, tc.S["use_json"])

    def test_stopping_answers_503_first(self):
        self.w.set_stopping(True)
        try:
            for path in ("/api/v1/sources", "/api/v1/catalog", "/api/v1/catalog/updates", "/api/v1/favorites"):
                self.anon.get(path).error(503, tc.S["stopping"])
                self.c.get(path).error(503, tc.S["stopping"])
            for path, body in self.POSTS:
                self.anon.post(path, b"[]").error(503, tc.S["stopping"])   # before the Bearer and the body
                self.c.post(path, body).error(503, tc.S["stopping"])
            body = self.c.get("/api/v1/system").check()
            self.assertIs(body["stopping"], True)
        finally:
            self.w.set_stopping(False)
        self.c.get("/api/v1/sources").check()


# --------------------------------------------------------------------------- #
# GET /system, /session, /zero/companion
# --------------------------------------------------------------------------- #

class TvSystem(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        _Slow.hits = 0
        cls.slow = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Slow)
        cls.slow.daemon_threads = True
        threading.Thread(target=cls.slow.serve_forever, daemon=True).start()
        feed = f"http://127.0.0.1:{cls.slow.server_address[1]}/payloads.json"
        cls.w = tvworld.World("cat-system", art=False, files=None, settings={"updates": {"feed": feed}}).start()
        cls.c = cls.w.client()
        cls.anon = cls.w.client(paired=False)

    @classmethod
    def tearDownClass(cls):
        cls.w.close()
        cls.slow.shutdown()
        cls.slow.server_close()

    def test_1_shape_and_golden(self):
        b = self.anon.get("/api/v1/system").check()
        self.assertIs(b["paired"], False)
        self.assertIs(b["localSessionAvailable"], True)
        self.assertEqual(b["platform"], "host")
        self.assertEqual(b["catalogueRevision"], catalogue_revision(FIXTURE_REVISION, self.w.featured))
        self.w.assert_golden("system", b)

    def test_2_paired_follows_the_bearer(self):
        self.assertIs(self.c.get("/api/v1/system").check()["paired"], True)
        self.assertIs(self.anon.get("/api/v1/system", token="1" * 64).check()["paired"], False)
        self.assertIs(self.anon.get("/api/v1/system", token="short").check()["paired"], False)

    def test_3_session_golden_and_zero(self):
        s = self.anon.get("/api/v1/session").check()
        self.assertEqual(s["token"], self.c.token)
        self.assertRegex(s["address"], r"^((?!127\.)\d{1,3}(\.\d{1,3}){3})?$")   # "" without a LAN address
        self.w.assert_golden("session", s, self.w.normalizer(keys={"address": "<ipv4 or empty>"}))
        for method in ("GET", "POST"):
            for client in (self.anon, self.c):
                client.request(method, "/api/v1/zero/companion", {} if method == "POST" else None).error(404, tc.S["zero"])

    def test_4_catalogue_revision_is_the_formula(self):
        rev = self.c.get("/api/v1/catalog/updates").check()["revision"]
        self.assertEqual(rev, FIXTURE_REVISION)
        sysrev = self.c.get("/api/v1/system").check()["catalogueRevision"]
        self.assertEqual(sysrev, catalogue_revision(rev, self.w.featured))
        self.assertLess(sysrev, 2 ** 53)
        self.assertEqual(sysrev >> 20, FIXTURE_REVISION)

    def test_5_fast_while_an_update_check_is_blocked(self):
        # the core update check blocks an HTTP worker on the slow feed for 5 s
        t = threading.Thread(target=lambda: self.w.srv.api("POST", "/api/update/check", {}, timeout=60))
        t.start()
        try:
            harness.wait_until(lambda: _Slow.hits > 0, timeout=10, every=0.05, what="the update check to reach the feed")
            times = timed(lambda: self.anon.get("/api/v1/system").check(), 20, every=0.05)
            assert_budget(self, times, 0.050, "GET /system during a blocked update check")
            self.assertGreater(_Slow.hits, 0)
            self.assertTrue(t.is_alive(), "the update check was still blocked while /system was measured")
        finally:
            t.join(30)

    def test_6_budget(self):
        times = timed(lambda: self.c.get("/api/v1/system").check(), 50, every=0.02)
        assert_budget(self, times, 0.050, "GET /system")

    def test_9_instance_id_is_stable_and_new_after_a_restart(self):
        a = self.anon.get("/api/v1/system").check()["instanceId"]
        self.assertEqual(self.anon.get("/api/v1/system").check()["instanceId"], a)
        self.w.restart()
        c = tvworld.TvClient(self.w.port)
        b = c.get("/api/v1/system").check()
        self.assertNotEqual(b["instanceId"], a)
        self.assertEqual(b["catalogueRevision"], catalogue_revision(FIXTURE_REVISION, self.w.featured))


# --------------------------------------------------------------------------- #
# The stop drain (tvsys_stop_begin, reached through POST /api/v1/updates)
# --------------------------------------------------------------------------- #

class TvStopDrain(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.loader = socket.socket()
        cls.loader.bind(("127.0.0.1", 0))
        cls.loader.listen(4)
        cls.loader.setblocking(False)
        cls.w = tvworld.World("cat-stop", art=False, tvapi_updates=True,
                              env={"XENO_PORT_LOADER": str(cls.loader.getsockname()[1])},
                              settings={"downloads": {"concurrentJobs": 1}}).start()
        cls.c = cls.w.client()

    @classmethod
    def tearDownClass(cls):
        cls.w.close()
        cls.loader.close()

    def stop(self):
        return self.c.post("/api/v1/updates", {"action": "stop", "confirmed": True})

    def test_drain(self):
        w = self.w
        a = w.srv.add(url=w.file_url("big.bin", "slow"), filename="drain-a.bin", size=64 * MiB)
        b = w.srv.add(url=w.file_url("mid.bin", "slow"), filename="drain-b.bin", size=32 * MiB)
        w.srv.wait_job(a["id"], None, timeout=60, until=lambda j: j["state"] == "downloading" and j["written"] > 0)
        self.assertEqual(w.srv.job(b["id"])["state"], "queued")

        # 1. queue.json cannot be written: refused, nothing paused, still running
        qpath = os.path.join(w.data, "queue.json")
        if os.path.exists(qpath):
            os.remove(qpath)
        os.makedirs(os.path.join(qpath, "blocker"))
        try:
            r = self.stop()
            if r.status == 503 and r.json.get("error") == NOT_READY:
                self.skipTest("POST /api/v1/updates is still the stub (tvapi_update.c)")
            r.error(503)
            self.assertNotEqual(w.srv.job(a["id"])["state"], "paused")
            self.assertNotEqual(w.srv.job(b["id"])["state"], "paused")
            sysb = self.c.get("/api/v1/system").check()
            self.assertIs(sysb["stopping"], False)
            self.c.get("/api/v1/sources").check()
        finally:
            shutil.rmtree(qpath)

        # 2. the stop: 202, stopping:true at once, paused and saved, then the process leaves
        before = self.c.get("/api/v1/system").check()
        r = self.stop()
        self.assertEqual(r.status, 202, r)
        body = r.json
        if "instanceId" in body:
            self.assertEqual(body["instanceId"], before["instanceId"])
        sysb = self.c.get("/api/v1/system").check()
        self.assertIs(sysb["stopping"], True)
        self.c.get("/api/v1/sources").error(503, tc.S["stopping"])
        self.c.post("/api/v1/updates", {"action": "stop", "confirmed": True}).error(503, tc.S["stopping"])
        rc = w.srv.proc.wait(timeout=30)
        self.assertEqual(rc, 0, w.log_text()[-3000:])
        with open(qpath) as f:
            saved = {j["id"]: j for j in json.load(f)["jobs"]}
        for jid in (a["id"], b["id"]):
            self.assertEqual(saved[jid]["state"], "paused", saved[jid])
            self.assertEqual(saved[jid].get("message"), STOP_PAUSE)
        log = w.log_text()
        self.assertIn("tvapi: stop accepted from the TV app: 2 job(s) paused", log)
        self.assertRegex(log, r"tvapi: stop drain done after \d+ ms")
        self.assertNotIn("relaunch", log.lower())
        try:
            conn, _ = self.loader.accept()
            conn.close()
            self.fail("something connected to the loader port: XENO STORE must not relaunch itself")
        except BlockingIOError:
            pass


# --------------------------------------------------------------------------- #
# The fixture catalogue
# --------------------------------------------------------------------------- #

class TvCatalogFixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.w = tvworld.World("cat-fixture", art=False).start()
        cls.c = cls.w.client()
        cls.anon = cls.w.client(paired=False)

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    def catalog(self):
        r = self.anon.get("/api/v1/catalog")
        return r, r.check()

    def set_sources(self, enabled):
        return self.c.post("/api/v1/sources", tc.AppBody.save_sources(enabled)).check()

    def test_01_catalog_equals_the_reference_builder(self):
        r, body = self.catalog()
        want = self.w.golden_catalog()
        self.assertEqual(body, want)
        self.assertEqual(json.dumps(body, sort_keys=False), json.dumps(want, sort_keys=False))   # key order too
        self.assertEqual(len(r.body), tv_golden.compact_bytes(want))
        self.w.assert_golden("catalog", body)

    def test_02_rules(self):
        _, body = self.catalog()
        games = {}
        for o in body:
            games.setdefault(o["gameId"], []).append(o)
        ids = [o["id"] for o in body]
        # R1: tier A only
        self.assertIn("fx-the-extraordinarily-long-collectors-edition-title-of-a-test-game-xyzzzz-ps5", games)
        for hidden in ("fx space game-ps5", "fx-pixel-shell-homebrew", "fx-orbital-tools-ps5", "fx-paper-moon-ps4",
                       "fx-sunken-choir-ps4", "fx-dust-runner-ps4", "fx-null-signal-ps5", "fx-locked-vault-ps5"):
            self.assertNotIn(hidden, games)
        for rid in ("fx-static-bloom-rar", "fx-glass-tide-zip", "fx-glass-tide-7z", "fx-static-bloom-dlps-pkg"):
            self.assertNotIn(rid, ids)
        self.assertEqual({o["format"] for o in body} - set(tv_golden.TV_FORMATS), set())
        self.assertTrue(all(o["sizeBytes"] > 0 for o in body))
        # R2: kind, then direct before browser, then ok before flaky, then verified first
        self.assertEqual([o["id"] for o in games["fx-tidewater-ps5"]],
                         ["fx-tidewater-ffpfsc", "fx-tidewater-backport", "fx-tidewater-update", "fx-tidewater-dlc"])
        self.assertEqual([o["sourceId"] for o in games["fx-neon-courier-ps5"]], ["archive", "vikingfile"])
        self.assertEqual(games["fx-ember-hollow-ps5"][-1]["id"], "fx-ember-hollow-flaky")
        # R6: from the source kind, never from browserUrl
        for o in body:
            self.assertEqual(o["browserAvailable"], o["sourceId"] == "vikingfile", o["id"])
            self.assertEqual(o["directAvailable"], o["sourceId"] == "archive", o["id"])
        # notes, dates, ranks
        notes = {o["id"]: o["notes"] for o in games["fx-tidewater-ps5"]}
        self.assertTrue(notes["fx-tidewater-dlc"].startswith("Add-on content. Needs the base game."))
        self.assertTrue(notes["fx-tidewater-backport"].startswith("Backport for older firmware."))
        self.assertTrue(notes["fx-tidewater-update"].startswith("Update "))
        for o in body:
            self.assertRegex(o["addedAt"], r"^(\d{4}-\d{2}-\d{2}T00:00:00Z)?$")
            self.assertRegex(o["releaseDate"], r"^(\d{4}-\d{2}-\d{2})?$")
            self.assertEqual(o["platform"], o["platform"].lower())
        ranks = {g: v[0].get("newOnOrbitRank") for g, v in games.items() if "newOnOrbitRank" in v[0]}
        hero = [h for h in self.w.featured["hero"] if h in games]
        self.assertEqual([g for g, _ in sorted(ranks.items(), key=lambda t: t[1])][:len(hero)], hero)
        self.assertEqual(sorted(ranks.values()), list(range(1, len(ranks) + 1)))

    def test_03_sources_and_updates(self):
        s = self.anon.get("/api/v1/sources").check()
        self.assertEqual(s, self.w.golden_sources())
        self.assertEqual([o["id"] for o in s["options"]], ["archive", "vikingfile"])
        self.w.assert_golden("sources", s)
        u = self.anon.get("/api/v1/catalog/updates").check()
        self.assertEqual(u["revision"], FIXTURE_REVISION)
        self.assertEqual(u["gameCount"], tv_golden.game_count(self.w.catalog))
        self.assertIs(u["cached"], True)
        self.assertIs(u["available"], True)
        self.assertIs(u["busy"], False)
        self.assertEqual((u["checkAfter"], u["error"]), (0, ""))
        self.w.assert_golden("catalog-updates", u)

    def test_04_not_acknowledged(self):
        put_settings(self.w, {"sources": {"acknowledged": False}})
        try:
            self.assertEqual(self.catalog()[1], [])
            s = self.anon.get("/api/v1/sources").check()
            self.assertIs(s["acknowledged"], False)
            self.assertEqual(s["enabled"], ["archive", "vikingfile"])   # the first-run draft starts with all on
            self.assertEqual(s, self.w.golden_sources(acknowledged=False))
        finally:
            r = self.set_sources(["archive", "vikingfile"])
        self.assertIs(r["acknowledged"], True)
        self.assertIs(settings_doc(self.w)["sources"]["acknowledged"], True, "shared with the web UI wizard")
        self.assertEqual(self.catalog()[1], self.w.golden_catalog())

    def test_05_post_sources_filters(self):
        try:
            s = self.set_sources(["archive"])
            self.assertEqual(s, self.w.golden_sources(enabled=["archive"], acknowledged=True))
            self.w.assert_golden("sources-post", s)
            self.assertEqual(self.anon.get("/api/v1/sources").check(), s)
            self.assertEqual(self.catalog()[1], self.w.golden_catalog(enabled=["archive"]))
            s = self.set_sources(["vikingfile"])
            self.assertEqual(s["enabled"], ["vikingfile"])
            body = self.catalog()[1]
            self.assertTrue(body and all(o["sourceId"] == "vikingfile" for o in body))
            self.assertEqual(body, self.w.golden_catalog(enabled=["vikingfile"]))
            s = self.set_sources([])                                    # empty is allowed
            self.assertEqual((s["enabled"], s["acknowledged"]), ([], True))
            self.assertEqual(self.catalog()[1], [])
            # the release counts never follow the filter
            self.assertEqual([o["releaseCount"] for o in s["options"]],
                             [o["releaseCount"] for o in self.w.golden_sources()["options"]])
        finally:
            self.set_sources(["archive", "vikingfile"])
        self.assertEqual(self.catalog()[1], self.w.golden_catalog())

    def test_06_post_sources_validation_order(self):
        before = self.anon.get("/api/v1/sources").check()
        select, unknown, notice = tc.S["sources_select"], tc.S["sources_unknown"], tc.S["sources_notice"]
        ok = {"acknowledged": True, "noticeVersion": 1}
        cases = [
            ({}, select),
            ({"enabled": "archive", **ok}, select),
            ({"enabled": {"archive": True}, **ok}, select),
            ({"enabled": ["archive", "vikingfile", "dlpsgame"], **ok}, select),    # more than the 2 options
            ({"enabled": "x", "acknowledged": False}, select),                    # the list is checked first
            ({"enabled": ["dlpsgame"], **ok}, unknown),                           # not exposed
            ({"enabled": ["other"], **ok}, unknown),
            ({"enabled": ["archive", "archive"], **ok}, unknown),
            ({"enabled": [1], **ok}, unknown),
            ({"enabled": [None], **ok}, unknown),
            ({"enabled": ["Archive"], **ok}, unknown),
            ({"enabled": ["dlpsgame"], "acknowledged": False}, unknown),          # ids before the notice
            ({"enabled": ["archive"]}, notice),
            ({"enabled": ["archive"], "acknowledged": False, "noticeVersion": 1}, notice),
            ({"enabled": ["archive"], "acknowledged": 1, "noticeVersion": 1}, notice),
            ({"enabled": ["archive"], "acknowledged": "true", "noticeVersion": 1}, notice),
            ({"enabled": ["archive"], "acknowledged": True, "noticeVersion": 2}, notice),
            ({"enabled": ["archive"], "acknowledged": True, "noticeVersion": "1"}, notice),
            ({"enabled": ["archive"], "acknowledged": True, "noticeVersion": 1.5}, notice),
            ({"enabled": ["archive"], "acknowledged": True}, notice),
        ]
        for body, sentence in cases:
            with self.subTest(body=body):
                self.c.post("/api/v1/sources", body).error(400, sentence)
        self.assertEqual(self.anon.get("/api/v1/sources").check(), before, "a refusal changes nothing")
        # 1.0 is the number 1 (the reference compares the double)
        self.c.post("/api/v1/sources", {"enabled": ["archive", "vikingfile"], "acknowledged": True, "noticeVersion": 1.0}).check()

    def test_07_only_the_exposed_ids_are_written(self):
        put_settings(self.w, {"sources": {"enabled": {"dlpsgame": False, "other": True, "pkgzone": False}}})
        try:
            self.set_sources(["vikingfile"])
            en = settings_doc(self.w)["sources"]["enabled"]
            self.assertEqual((en.get("archive"), en.get("vikingfile")), (False, True))
            self.assertEqual((en.get("dlpsgame"), en.get("other"), en.get("pkgzone")), (False, True, False))
        finally:
            self.set_sources(["archive", "vikingfile"])
        en = settings_doc(self.w)["sources"]["enabled"]
        self.assertEqual((en.get("archive"), en.get("vikingfile"), en.get("dlpsgame")), (True, True, False))

    def test_08_a_failed_save_changes_nothing(self):
        path = os.path.join(self.w.data, "settings.json")
        keep = path + ".keep"
        os.replace(path, keep)
        os.makedirs(os.path.join(path, "blocker"))
        try:
            self.c.post("/api/v1/sources", tc.AppBody.save_sources(["archive"])).error(503, S_SOURCES_SAVE)
            s = self.anon.get("/api/v1/sources").check()
            self.assertEqual(s["enabled"], ["archive", "vikingfile"])
            self.assertIs(s["acknowledged"], True)
        finally:
            shutil.rmtree(path)
            os.replace(keep, path)
        self.assertEqual(self.catalog()[1], self.w.golden_catalog())

    def test_09_budget(self):
        times = timed(lambda: self.catalog(), 5)
        assert_budget(self, times, 0.5, "GET /catalog (fixture)")
        times = timed(lambda: self.anon.get("/api/v1/sources").check(), 20, every=0.05)
        assert_budget(self, times, 0.050, "GET /sources")

    def test_10_show_zip_variant(self):
        with tvworld.World("cat-zip", art=False, files=None, show_zip=True) as w:
            body = tvworld.TvClient(w.port).get("/api/v1/catalog").check()
            want = w.golden_catalog()
            self.assertEqual(body, want)
            zips = [o for o in body if o["format"] == "Unknown"]
            self.assertTrue(zips, "the fixture has ZIP options")
            for o in zips:
                self.assertEqual((o["compression"], o["archiveContents"], o["extractedBytes"]), ("zip", "unverified", o["sizeBytes"]))
            self.assertFalse([o for o in body if o["id"].endswith(("-rar", "-7z"))], "RAR and 7z stay hidden")
            s = tvworld.TvClient(w.port).get("/api/v1/sources").check()
            self.assertEqual(s, w.golden_sources())


# --------------------------------------------------------------------------- #
# The real revision-10 catalogue
# --------------------------------------------------------------------------- #

class TvCatalogRealData(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.w = tvworld.World("cat-real", catalog="real", featured="real", art=False, files=None).start()
        cls.c = tvworld.TvClient(cls.w.port)
        cls.stats = tv_golden.stats(cls.w.catalog, cls.w.featured)

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    def test_1_catalog_equals_the_reference_builder(self):
        t0 = time.monotonic()
        r = self.c.get("/api/v1/catalog")
        cold = time.monotonic() - t0
        body = tc.strict_json(r.body)                        # the app's parser rules on 2.36 MB
        r.check()                                            # transport + every item against the contract
        want = self.w.golden_catalog()
        self.assertEqual(len(body), 4292)
        self.assertEqual(len({o["gameId"] for o in body}), 2593)
        self.assertEqual(len(r.body), 2364377)
        self.assertEqual((self.stats["options"], self.stats["games"], self.stats["bytes"]), (4292, 2593, 2364377))
        self.assertEqual(body, want)
        self.assertEqual(json.dumps(body, sort_keys=False), json.dumps(want, sort_keys=False))
        self.assertLess(cold, 1.0, f"first GET /catalog (with the index build) took {cold * 1000:.0f} ms")
        ranked = sorted({o["gameId"]: o["newOnOrbitRank"] for o in body if "newOnOrbitRank" in o}.items(), key=lambda t: t[1])
        self.assertEqual(len(ranked), self.stats["ranked"])
        self.assertEqual(ranked[0][0], "ghost-of-yotei-ps5")

    def test_2_budget_and_lock_hold(self):
        self.c.get("/api/v1/catalog").check(transport=False, profile="app")
        times = timed(lambda: self.c.get("/api/v1/catalog"), 4)
        assert_budget(self, times, 0.5, "GET /catalog (2.36 MB)")
        walks = [int(m) for m in re.findall(r"TV catalogue index: .* \(walk (\d+) ms", self.w.log_text())]
        self.assertTrue(walks, "the index build is logged")
        self.assertLess(max(walks), 100, f"catalog_mu held {max(walks)} ms by the index walk")
        self.assertEqual(len(walks), 1, "the index is built once and reused")

    def test_3_sources_updates_system(self):
        s = self.c.get("/api/v1/sources").check()
        self.assertEqual(s, self.w.golden_sources())
        self.assertEqual({o["id"]: o["releaseCount"] for o in s["options"]}, {"archive": 3560, "vikingfile": 732})
        self.assertEqual([o["downloadReady"] for o in s["options"]], [True, False])
        u = self.c.get("/api/v1/catalog/updates").check()
        self.assertEqual((u["revision"], u["gameCount"], u["cached"]), (10, 2593, False))
        b = self.c.get("/api/v1/system").check()
        self.assertEqual(b["catalogueRevision"], catalogue_revision(10, self.w.featured))


# --------------------------------------------------------------------------- #
# POST /catalog/updates
# --------------------------------------------------------------------------- #

class TvCatalogRefresh(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.feed = CatalogueServer()
        cls.w = tvworld.World("cat-refresh", art=False, files=None, settings={"catalog": {
            "url": cls.feed.base + "/catalog.json", "revisionUrl": cls.feed.base + "/revision.json"}}).start()
        cls.c = cls.w.client()
        nxt = copy.deepcopy(cls.w.catalog)
        nxt["revision"] = FIXTURE_REVISION + 1
        fresh = copy.deepcopy(next(g for g in nxt["games"] if g["id"] == "fx-signal-fire-ps5"))
        fresh["id"] = fresh["slug"] = "fx-fresh-arrival-ps5"
        fresh["title"] = "Fresh Arrival"
        for r in fresh["releases"]:
            r["id"] = "fx-fresh-arrival-exfat"
        nxt["games"].append(fresh)
        cls.next_catalog = nxt
        cls.feed.catalog = cls.w.catalog                      # revision.json says "not newer" at first

    @classmethod
    def tearDownClass(cls):
        cls.w.close()
        cls.feed.close()

    def status(self):
        return self.c.get("/api/v1/catalog/updates").check()

    def test_1_refusals_without_side_effects(self):
        for body in ({}, {"action": "check"}, {"action": 1}, {"action": "Refresh"}, {"action": ["refresh"]}):
            self.c.post("/api/v1/catalog/updates", body).error(400, tc.S["refresh_action"])
        u = self.status()
        self.assertEqual((u["busy"], u["checkAfter"]), (False, 0))
        self.assertEqual(self.feed.hits, [])

    def test_2_busy_follows_a_web_ui_refresh(self):
        self.feed.delay = 2.0
        t = threading.Thread(target=lambda: self.w.srv.api("POST", "/api/catalog/refresh", {}, timeout=60))
        t.start()
        try:
            r = self.c.poll("/api/v1/catalog/updates", until=lambda r: r.status == 200 and r.json["busy"], timeout=10, every=0.05)
            r.check()
            self.c.post("/api/v1/catalog/updates", tc.AppBody.refresh_catalogue()).error(409, tc.S["refresh_busy"])
        finally:
            t.join(60)
            self.feed.delay = 0.0
        u = self.status()
        self.assertIs(u["busy"], False)
        self.assertEqual(u["checkAfter"], 0, "a refused POST starts no cooldown")
        self.assertGreater(u["checkedAt"], 0)

    def test_3_refresh_installs_the_new_revision(self):
        old_sys = self.c.get("/api/v1/system").check()["catalogueRevision"]
        self.feed.catalog = self.next_catalog
        self.feed.delay = 1.0
        now = int(time.time())
        r = self.c.post("/api/v1/catalog/updates", tc.AppBody.refresh_catalogue())
        u = r.check()                                           # 202 + the status object
        self.assertEqual(r.status, 202)
        self.assertIs(u["busy"], True)
        self.assertTrue(now + 55 <= u["checkAfter"] <= now + 65, u)
        self.assertEqual(u["error"], "")
        self.c.post("/api/v1/catalog/updates", tc.AppBody.refresh_catalogue()).error(409, tc.S["refresh_busy"])
        r = self.c.poll("/api/v1/catalog/updates", until=lambda r: r.status == 200 and not r.json["busy"], timeout=60, every=0.1)
        u = r.check()
        self.feed.delay = 0.0
        self.assertEqual(u["error"], "")
        self.assertEqual(u["revision"], FIXTURE_REVISION + 1)
        self.assertEqual(u["gameCount"], tv_golden.game_count(self.next_catalog))
        self.assertGreater(u["checkedAt"], 0)
        new_sys = self.c.get("/api/v1/system").check()["catalogueRevision"]
        self.assertNotEqual(new_sys, old_sys)
        self.assertEqual(new_sys, catalogue_revision(FIXTURE_REVISION + 1, self.w.featured))
        body = self.c.get("/api/v1/catalog").check()
        self.assertEqual(body, tv_golden.build(self.next_catalog, self.w.featured))
        self.assertIn("fx-fresh-arrival-ps5", {o["gameId"] for o in body})
        s = self.c.get("/api/v1/sources").check()
        self.assertEqual(s, tv_golden.sources(self.next_catalog, self.w.featured, acknowledged=True))
        # inside the 60 s cooldown
        self.c.post("/api/v1/catalog/updates", tc.AppBody.refresh_catalogue()).error(429, tc.S["refresh_wait"])

    def test_4_a_failed_refresh_is_a_sentence(self):
        dead = socket.socket()
        dead.bind(("127.0.0.1", 0))
        port = dead.getsockname()[1]
        dead.close()
        url = f"http://127.0.0.1:{port}"
        with tvworld.World("cat-refresh-fail", art=False, files=None, settings={"catalog": {
                "url": url + "/catalog.json", "revisionUrl": url + "/revision.json"}}) as w:
            c = w.client()
            c.post("/api/v1/catalog/updates", tc.AppBody.refresh_catalogue()).check()
            r = c.poll("/api/v1/catalog/updates", until=lambda r: r.status == 200 and not r.json["busy"], timeout=60)
            u = r.check()
            self.assertRegex(u["error"], REFRESH_FAILED)
            self.assertEqual(u["revision"], FIXTURE_REVISION)
            self.assertEqual(c.get("/api/v1/system").check()["catalogueRevision"],
                             catalogue_revision(FIXTURE_REVISION, w.featured))


# --------------------------------------------------------------------------- #
# Favourites
# --------------------------------------------------------------------------- #

class TvFavorites(unittest.TestCase):
    GAME = "fx-tidewater-ps5"

    @classmethod
    def setUpClass(cls):
        cls.w = tvworld.World("cat-favorites", art=False, files=None).start()
        cls.c = cls.w.client()

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    def setUp(self):
        st, _ = self.w.srv.api("PUT", "/api/favorites", {"ids": []})
        self.assertEqual(st, 200)

    def fav(self, gid, on):
        return self.c.post("/api/v1/favorites", tc.AppBody.favourite(gid, on))

    def test_set_semantics(self):
        self.assertEqual(self.c.get("/api/v1/favorites").check(), [])
        self.assertEqual(self.fav(self.GAME, True).check(), [self.GAME])
        self.assertEqual(self.fav(self.GAME, True).check(), [self.GAME])          # set twice: one entry
        self.assertEqual(self.fav("fx-neon-courier-ps5", True).check(), [self.GAME, "fx-neon-courier-ps5"])
        body = self.c.get("/api/v1/favorites").check()
        self.w.assert_golden("favorites", body)
        self.assertEqual(self.fav(self.GAME, False).check(), ["fx-neon-courier-ps5"])
        self.assertEqual(self.fav(self.GAME, False).check(), ["fx-neon-courier-ps5"])   # removing twice is fine
        self.assertEqual(self.fav("never-heard-of-it", False).check(), ["fx-neon-courier-ps5"])

    def test_unknown_game_only_refused_when_adding(self):
        self.fav("fx-no-such-game", True).error(404, tc.S["fav_unknown"])
        self.fav("x" * 95, True).error(404, tc.S["fav_unknown"])
        # any catalogue game counts, not only the TV-visible ones (PAGE-only, HOMEBREW)
        self.assertEqual(self.fav("fx-paper-moon-ps4", True).check(), ["fx-paper-moon-ps4"])
        self.assertEqual(self.fav("fx-pixel-shell-homebrew", True).check(), ["fx-paper-moon-ps4", "fx-pixel-shell-homebrew"])

    def test_bad_bodies(self):
        bad = tc.S["fav_bad"]
        for body in ({}, {"gameId": self.GAME}, {"favorite": True}, {"gameId": self.GAME, "favorite": "true"},
                     {"gameId": self.GAME, "favorite": 1}, {"gameId": self.GAME, "favorite": None},
                     {"gameId": 5, "favorite": True}, {"gameId": "", "favorite": True},
                     {"gameId": "x" * 96, "favorite": True}, {"gameId": "a\nb", "favorite": True},
                     {"gameId": "a\tb", "favorite": False}, {"gameId": [self.GAME], "favorite": True}):
            with self.subTest(body=body):
                self.c.post("/api/v1/favorites", body).error(400, bad)
        # the JSON reader turns \u0000 into U+FFFD (src/json.c), so this is a plain unknown id
        self.assertEqual(self.c.post("/api/v1/favorites", {"gameId": "a\u0000b", "favorite": False}).check(), [])
        self.c.post("/api/v1/favorites", {"gameId": "a\u0000b", "favorite": True}).error(404, tc.S["fav_unknown"])
        self.assertEqual(self.c.get("/api/v1/favorites").check(), [])

    def test_shared_with_the_web_ui(self):
        self.fav(self.GAME, True).check()
        st, b = self.w.srv.api("GET", "/api/favorites")
        self.assertEqual(b["ids"], [self.GAME])
        with open(os.path.join(self.w.data, "favorites.json")) as f:
            self.assertEqual(json.load(f)["ids"], [self.GAME])
        st, b = self.w.srv.api("POST", "/api/favorites/toggle", {"id": "fx-neon-courier-ps5"})
        self.assertEqual(st, 200, b)
        self.assertEqual(self.c.get("/api/v1/favorites").check(), [self.GAME, "fx-neon-courier-ps5"])

    def test_full(self):
        ids = [f"web-only-{i:04d}" for i in range(4000)]
        st, b = self.w.srv.api("PUT", "/api/favorites", {"ids": ids})
        self.assertEqual((st, len(b["ids"])), (200, 4000))
        self.fav(self.GAME, True).error(409, tc.S["fav_full"])
        self.assertEqual(len(self.c.get("/api/v1/favorites").check()), 4000)
        self.assertEqual(len(self.fav("web-only-0000", False).check()), 3999)   # removing still works
        self.assertEqual(len(self.fav(self.GAME, True).check()), 4000)

    def test_failed_save_changes_nothing(self):
        self.fav(self.GAME, True).check()
        path = os.path.join(self.w.data, "favorites.json")
        os.remove(path)
        os.makedirs(os.path.join(path, "blocker"))
        try:
            self.fav("fx-neon-courier-ps5", True).error(503, S_FAV_SAVE)
            self.fav(self.GAME, False).error(503, S_FAV_SAVE)
            self.assertEqual(self.c.get("/api/v1/favorites").check(), [self.GAME])
        finally:
            shutil.rmtree(path)
        self.assertEqual(self.fav("fx-neon-courier-ps5", True).check(), [self.GAME, "fx-neon-courier-ps5"])

    def test_budget(self):
        self.fav(self.GAME, True).check()
        times = timed(lambda: self.c.get("/api/v1/favorites").check(), 30, every=0.1)
        assert_budget(self, times, 0.020, "GET /favorites at 10 requests per second")


if __name__ == "__main__":
    unittest.main()
