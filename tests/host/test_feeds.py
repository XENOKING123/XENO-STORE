#!/usr/bin/env python3
"""
test_feeds.py — the owner's feeds (featured, messages, companions,
saves-backend) cached under <data>/feeds/ when the store is updated:

  * copies left by a build without <data>/feeds/.build (1.2.1 cached a
    saves-backend.json that said "github" and companion entries with
    placeholder hashes) are fetched again whatever their age: the Save
    Store's own look-up follows the published site at once, the first
    refresh brings the others, and the marker then names this build — after
    a restart nothing is fetched again before the copies are 6 h old;
  * offline (the feeds host failing): the cached copies are served and
    nothing is marked; the boot refresh tries once, then leaves the host
    alone; once the host answers again the next refresh fetches them;
  * with catalog auto-refresh off, a catalog refresh keeps the 6-hour rule
    for featured and messages, while the Save Store still gets the published
    saves-backend.json and the payload list and a payload install get the
    published companions.json (no placeholder hashes left to refuse).

Everything is loopback: the feed URLs come from settings.json, the default
GitHub save index from XENO_SAVES_INDEX_BASE and the save site is
supabase_mock.SupabaseMock.
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
from supabase_mock import SupabaseMock  # noqa: E402

ANON = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJyb2xlIjoiYW5vbiIsInJlZiI6Im1vY2sifQ.bW9jay1hbm9uLWtleS1mb3ItdGVzdHM"
REAL_SHA = "124051ab3a762474720ae53187d2920bc96d6be1d69aa298e715667efc385a2f"
FEEDS = ("featured", "messages", "companions", "saves-backend")

# what a 1.2.1 console left in <data>/feeds/
OLD = {
    "featured": {"hero": ["old-hero"]},
    "messages": {"messages": [{"id": "old", "text": "1.2.1 era"}]},
    "companions": {"payloads": [{"id": "garlic-save-manager", "name": "Garlic Save Manager", "kind": "saves", "version": "1.0",
                                 "url": "https://github.com/XENOKING123/XENO-STORE/releases/download/companions/garlic-save-manager.elf",
                                 "sha256": "FILLED-BY-OWNER"}]},
    "saves-backend": {"kind": "github"},
}


def published(site):
    return {"kind": "supabase", "name": "XenoKing", "url": site.base, "anonKey": ANON, "bucket": "ps5-saves", "garlicFunction": "garlic-savedb"}


def current(site):
    return {
        "featured": {"hero": ["new-hero"]},
        "messages": {"messages": [{"id": "new", "text": "current"}]},
        "companions": {"payloads": [{"id": "garlic-save-manager", "name": "Garlic Save Manager", "kind": "saves", "version": "1.7",
                                     "url": "https://github.com/earthonion/garlic-savemgr/releases/download/v1.7/garlic-savemgr.elf",
                                     "sha256": REAL_SHA}]},
        "saves-backend": published(site),
    }


class Feeds(threading.Thread):
    """The feeds host: serves <name>.json (and the files in .files), logs every request, answers 503 while down."""

    def __init__(self):
        super().__init__(daemon=True)
        self.docs = {}
        self.files = {}
        self.log = []
        self.down = False
        host = self

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                path = self.path.split("?", 1)[0]
                host.log.append(path)
                name = path.strip("/")[:-len(".json")] if path.endswith(".json") else ""
                if host.down:
                    st, body = 503, b'{"error":"unavailable"}'
                elif path in host.files:
                    st, body = 200, host.files[path]
                elif name in host.docs:
                    st, body = 200, json.dumps(host.docs[name]).encode()
                else:
                    st, body = 404, b'{"error":"not found"}'
                self.send_response(st)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.srv.daemon_threads = True
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        self.start()

    def run(self):
        self.srv.serve_forever()

    def hits(self, name):
        return self.log.count("/%s.json" % name)

    def close(self):
        self.srv.shutdown()


class Base(unittest.TestCase):
    auto_refresh = True

    @classmethod
    def setUpClass(cls):
        cls.feeds = Feeds()
        cls.site = SupabaseMock(ANON).start()
        cls.feeds.docs.update(current(cls.site))
        cls.feeds.docs["saves"] = {"schemaVersion": 1, "saves": []}
        cls.tmp = tempfile.mkdtemp(prefix="xeno-feeds-")
        cls.data = os.path.join(cls.tmp, "data")
        os.makedirs(os.path.join(cls.data, "feeds"))
        for name, doc in OLD.items():                       # cached minutes ago, by a build that wrote no marker
            with open(cls.cached(name), "w", encoding="utf-8") as f:
                json.dump(doc, f)
        b = cls.feeds.base
        with open(os.path.join(cls.data, "settings.json"), "w", encoding="utf-8") as f:
            json.dump({"catalog": {"autoRefresh": cls.auto_refresh, "url": b + "/catalog.json", "revisionUrl": b + "/revision.json",
                                   "featuredUrl": b + "/featured.json", "messagesUrl": b + "/messages.json",
                                   "companionsUrl": b + "/companions.json", "savesBackendUrl": b + "/saves-backend.json"},
                       "updates": {"autoCheck": False}}, f)
        env = {"XENO_SAVES_INDEX_BASE": b, "XENO_PORT_GARLIC": "1", "XENO_PORT_LOADER": "1", "XENO_PORT_DPI": "1",
               "XENO_PORT_ETAHEN": "1", "XENO_PORT_SHADOWMOUNT": "1"}
        cls.srv = harness.XenoServer(cls.data, env=env).start()
        cls.started = time.time()
        st, b = cls.srv.api("GET", "/api/status")
        cls.version = b["version"]

    @classmethod
    def tearDownClass(cls):
        cls.srv.stop()
        cls.site.stop()
        cls.feeds.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    @classmethod
    def cached(cls, name):
        return os.path.join(cls.data, "feeds", name + ".json")

    def marker(self):
        try:
            with open(os.path.join(self.data, "feeds", ".build"), encoding="utf-8") as f:
                return f.read().strip()
        except FileNotFoundError:
            return None

    def served(self):
        st, b = self.srv.api("GET", "/api/feeds")
        self.assertEqual(st, 200, b)
        return {"featured": b["featured"], "messages": b["messages"], "companions": b["companions"], "saves-backend": b["savesBackend"]}

    def store_backend(self, refresh=False):
        st, b = self.srv.api("GET", "/api/saves/store" + ("?refresh=1" if refresh else ""))
        self.assertEqual(st, 200, b)
        return b["backend"], b["backendName"]

    def catalog_refresh(self):
        # the catalogue itself is not served: the refresh fails, the feeds still follow it
        self.srv.api("POST", "/api/catalog/refresh", {})


class UpgradeTests(Base):
    def test_1_save_store_follows_the_published_site_at_once(self):
        self.assertEqual(self.store_backend(), ("supabase", "XenoKing"))
        self.assertEqual(self.feeds.hits("saves-backend"), 1)
        with open(self.cached("saves-backend"), encoding="utf-8") as f:
            self.assertEqual(json.load(f)["kind"], "supabase")

    def test_2_first_refresh_fetches_the_others_and_marks_the_build(self):
        self.catalog_refresh()
        self.assertEqual(self.served(), current(self.site))
        self.assertEqual(self.marker(), self.version)
        for name in FEEDS:
            self.assertEqual(self.feeds.hits(name), 1, name)            # once each, saves-backend included

    def test_3_after_a_restart_nothing_is_fetched_again(self):
        self.srv.stop()
        self.feeds.docs["featured"] = {"hero": ["newer-hero"]}
        mark = len(self.feeds.log)
        self.srv.start()
        self.catalog_refresh()
        self.assertEqual(self.store_backend(), ("supabase", "XenoKing"))
        self.assertEqual(self.served()["featured"], {"hero": ["new-hero"]})   # this build's copy, still fresh
        self.assertEqual([p for p in self.feeds.log[mark:] if p[1:-5] in FEEDS], [])


class OfflineTests(Base):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.feeds.down = True

    def test_1_cache_served_then_fetched_when_the_host_is_back(self):
        # the Save Store asks once and keeps the cached copy
        self.assertEqual(self.store_backend(), ("github", ""))
        self.assertEqual(self.feeds.hits("saves-backend"), 1)
        self.catalog_refresh()
        self.assertEqual(self.served(), OLD)
        self.assertIsNone(self.marker())
        # the boot refresh (20 s after start) tries each copy again, then leaves the host alone
        tried = self.feeds.hits("featured")
        deadline = self.started + 40
        while self.feeds.hits("featured") == tried and time.time() < deadline:
            time.sleep(0.25)
        self.assertGreater(self.feeds.hits("featured"), tried, "the boot refresh did not try the copies of the other build")
        time.sleep(2)
        n = len(self.feeds.log)
        time.sleep(3)
        self.assertEqual([p for p in self.feeds.log[n:] if p[1:-5] in FEEDS], [])
        self.assertEqual(self.served(), OLD)
        self.assertIsNone(self.marker())
        # the host is back: the next refresh brings every feed and marks the build
        self.feeds.down = False
        self.catalog_refresh()
        self.assertEqual(self.served(), current(self.site))
        self.assertEqual(self.marker(), self.version)
        self.assertEqual(self.store_backend(refresh=True), ("supabase", "XenoKing"))


class AutoRefreshOffTests(Base):
    auto_refresh = False

    def test_1_save_store_still_follows_the_site(self):
        self.assertEqual(self.store_backend(), ("supabase", "XenoKing"))
        # a catalog refresh started by hand keeps the 6-hour rule for the featured feed
        self.catalog_refresh()
        self.assertEqual(self.served()["featured"], OLD["featured"])
        self.assertEqual(self.feeds.hits("featured"), 0)
        self.assertIsNone(self.marker())

    def test_2_payload_list_fetches_the_published_companions(self):
        # the other build's copy has placeholder hashes: the list fetches the published one at once
        st, b = self.srv.api("GET", "/api/payloads")
        self.assertEqual(st, 200, b)
        g = {p["id"]: p for p in b["payloads"]}["garlic-save-manager"]
        self.assertEqual((g["available"], g["version"], g["sha256"]), (True, "1.7", REAL_SHA), g)
        self.assertEqual(self.served()["companions"], current(self.site)["companions"])
        self.srv.api("GET", "/api/payloads")
        self.assertEqual(self.feeds.hits("companions"), 1)                       # once
        self.assertEqual(self.served()["featured"], OLD["featured"])             # the others are left alone
        self.assertEqual(self.feeds.hits("featured"), 0)


class AutoRefreshOffInstallTests(Base):
    auto_refresh = False

    def test_1_install_fetches_the_published_companions_first(self):
        elf = b"\x7fELF" + b"GARLIC-FROM-THE-PUBLISHED-LIST" + bytes(range(256))
        self.feeds.files["/garlic.elf"] = elf
        self.feeds.docs["companions"] = {"payloads": [{"id": "garlic-save-manager", "name": "Garlic Save Manager", "kind": "saves",
                                                       "version": "1.7", "url": self.feeds.base + "/garlic.elf",
                                                       "sha256": hashlib.sha256(elf).hexdigest()}]}
        st, b = self.srv.api("POST", "/api/payloads/garlic-save-manager/install", {})
        self.assertEqual(st, 200, b)
        jid = b["job"]["id"]
        deadline = time.time() + 30
        while time.time() < deadline:
            st, j = self.srv.api("GET", "/api/saves/jobs/%s" % jid)
            if j["job"]["state"] in ("done", "failed"):
                break
            time.sleep(0.1)
        self.assertEqual(j["job"]["state"], "done", j)
        self.assertTrue(j["job"]["result"]["verified"], j)
        with open(os.path.join(self.data, "payloads", "garlic-save-manager.elf"), "rb") as f:
            self.assertEqual(f.read(), elf)
        self.assertEqual(self.feeds.hits("companions"), 1)
        self.assertEqual(self.feeds.hits("featured"), 0)


if __name__ == "__main__":
    unittest.main()
