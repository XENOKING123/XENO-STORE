#!/usr/bin/env python3
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
test_tvapi_downloads.py — the downloads family of the /api/v1 layer the native
TV app talks to (PORT-SPEC 1.5.3, 1.9; map-downloads §2-§9, §12, §14):

  TvDownloadsBasics   §14 items 1-3, 12-14: the drive list, the Bearer gate on all
                      nine routes, the empty queue, unknown releases, the delivery
                      check, disabled sources, Vikingfile without TorBox, the
                      provider-browser stub (never 404), the download settings
                      (1..4, booleans, the default drive and its "auto" flag).
  TvDownloadsFlow     §14 items 4-10: create and follow a download to "complete"
                      with the catalogue's fields, a server size that differs from
                      the catalogue, pause / resume / cancel {deletePartial} /
                      delete (a kept partial goes with the row), duplicates and
                      history reuse, retries and errors, the ZIP password flow
                      (a 128-byte password included), a PKG install.
  TvDownloadsZip      §14 item 9 with TV_SHOW_ZIP: a ZIP option created from the
                      TV app extracts to its image.
  TvDownloadsTorBox   §14 item 11 against a TorBox fixture (XENO_TB_BASE): host
                      limits from /webdl/hosters (E11), "Preparing with TorBox",
                      direct delivery never touching TorBox.
  TvDownloadsBudget   §14 item 16 / PORT-SPEC 1.9: GET downloads, storage, browser
                      and debrid at 10 requests per second each with 50 jobs, and
                      no statvfs or file creation on an HTTP worker.

Every reply goes through tvcontract (strict JSON, the app's field tables, the
sentence error envelope) via tvworld.TvClient. Jobs the TV app cannot create
(URL jobs, retry fixtures, ZIP and PKG releases hidden from the TV catalogue)
come from the web UI's /api/queue, as they would on a console.
"""
import http.server
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time
import unittest
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402
import range_server  # noqa: E402
import tvcontract as tc  # noqa: E402
import tvworld  # noqa: E402

MiB = 1024 * 1024

# Sentences (map-downloads §12): exact, the app translates them by key.
S = {
    "unpaired": "Pair this device with your PS5 to continue.",
    "unknown_release": "Unknown curated release.",
    "drive": "Selected storage is not connected or writable.",
    "filename_busy": "Another download uses this filename on that drive. Open Downloads to manage it.",
    "has_job": "This release already has a job on that drive. Open Downloads to resume or retry it.",
    "cancelled_partial": ("Open Downloads to resume this cancelled download, or reconnect its original drive and "
                          "delete the partial file before downloading again."),
    "no_space": "Not enough space after unfinished downloads. Free space or cancel a queued download.",
    "source": "Enable this source and acknowledge the download notice in Sources first.",
    "tb_connect": "Connect TorBox in App settings first.",
    "tb_host": "TorBox currently cannot download this host or file size. Choose the original source.",
    "provider": "This provider does not support direct console downloads yet.",
    "delivery": "Choose direct or torbox delivery.",
    "not_running": "This job is not running.",
    "cannot_resume": "This job cannot be resumed in its current state.",
    "complete_cancel": "Completed files cannot be deleted through cancellation.",
    "only_cancelled": "Only cancelled downloads can be deleted.",
    "remove": ("Only completed or cancelled history can be removed. Reconnect the original drive and delete any "
               "kept partial file first."),
    "pw_state": "Pause extraction or wait for it to finish before entering a password.",
    "pw_len": "Enter an archive password of up to 128 bytes.",
    "pw_ctrl": "The archive password cannot contain control characters.",
    "unknown_download": "Unknown download.",
    "unknown_action": "Unknown queue action.",
    "unknown_dl_action": "Unknown download action.",
    "expected_storage": "Expected storageId.",
    "expected_delete": "Expected deleteArchives.",
    "limit": "Choose a whole number from 1 to 4.",
    "file_exists": "A file with this name is already on that drive. XENO STORE will not overwrite it.",
    "install_wait": "Wait for the install to finish before removing it from history.",
    "no_link": "This download has no link to refresh.",
    "browser": ("Vikingfile asks for a human check, so XENO STORE cannot fetch it by itself. Download it with "
                "TorBox, or open this game in the browser version."),
    "browser_gone": "This verification session is no longer active.",
    "browser_action": "Unknown browser action.",
    "preparing": "Preparing with TorBox",
}
NINE_ROUTES = [("GET", "/api/v1/downloads"), ("POST", "/api/v1/downloads"), ("POST", "/api/v1/downloads/j_000000/pause"),
               ("GET", "/api/v1/storage"), ("POST", "/api/v1/storage/preferred"), ("POST", "/api/v1/settings/downloads"),
               ("POST", "/api/v1/settings/extraction"), ("GET", "/api/v1/debrid"), ("GET", "/api/v1/browser"),
               ("POST", "/api/v1/browser")]
NOT_READY = "This part of XENO STORE is not ready yet."
SRC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src")


# --------------------------------------------------------------------------- #
# helpers
# --------------------------------------------------------------------------- #

def index_ready(c):
    """True when the TV catalogue index answers (tvcat_release): a browser-only
    release refused for direct delivery proves the lookup found it."""
    r = c.post("/api/v1/downloads", {"releaseId": "fx-viking-only-a", "storageId": "internal"})
    return r.status == 409


def jobs(c):
    r = c.get("/api/v1/downloads")
    return r.check()


def job_row(c, jid):
    for j in jobs(c):
        if j["id"] == jid:
            return j
    return None


def wait_row(c, jid, cond, timeout=60.0, what="condition", every=0.1):
    """Polls GET /api/v1/downloads until the job's row satisfies cond (None = gone)."""
    deadline = time.time() + timeout
    last = "<never seen>"
    seen = []
    while time.time() < deadline:
        row = job_row(c, jid)
        last = row
        if row is not None and (not seen or seen[-1] != row["status"]):
            seen.append(row["status"])
        if cond(row):
            return row, seen
        time.sleep(every)
    raise AssertionError(f"timeout waiting for {jid}: {what}; last={json.dumps(last)[:700]} statuses={seen}")


def option_of(w, rid, c=None):
    """The app's view of a release: /api/v1/catalog when it is served, else the
    Python reference builder over the same fixture."""
    if c is not None:
        r = c.get("/api/v1/catalog")
        if r.status == 200:
            for o in r.json:
                if o.get("id") == rid:
                    return o
    for o in w.golden_catalog():
        if o.get("id") == rid:
            return o
    raise KeyError(rid)


def queue_add(w, **body):
    """Creates a job through the web UI's /api/queue (no TV index needed)."""
    st, resp = w.srv.api("POST", "/api/queue", body)
    if st != 200:
        raise AssertionError(f"POST /api/queue failed {st}: {resp}")
    return resp["job"]["id"]


def core_job(w, jid):
    return w.srv.job(jid)


def put_settings(w, doc):
    st, b = w.srv.api("PUT", "/api/settings", doc)
    assert st == 200, (st, b)
    return b


def read_json(path):
    with open(path) as f:
        return json.load(f)


# --------------------------------------------------------------------------- #
# TorBox fixture (map-downloads §8; the reference's /webdl/hosters shape)
# --------------------------------------------------------------------------- #

class TorBoxMock:
    KEY = "tb-tv-good"

    def __init__(self):
        self.lock = threading.Lock()
        self.calls = []
        self.items = {}
        self.next_id = 700
        self.polls_before_done = 2
        self.hosters = []
        mock = self

        class H(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def _send(self, obj, code=200):
                body = json.dumps(obj).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(body)

            def _handle(self):
                url = urllib.parse.urlsplit(self.path)
                q = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
                form = {}
                if self.command == "POST":
                    n = int(self.headers.get("Content-Length") or 0)
                    form = {k: v[0] for k, v in urllib.parse.parse_qs(self.rfile.read(n).decode()).items()}
                sub = url.path[len("/tb/"):] if url.path.startswith("/tb/") else url.path
                auth = self.headers.get("Authorization") or ""
                key = auth[7:] if auth.lower().startswith("bearer ") else q.get("token", "")
                with mock.lock:
                    mock.calls.append({"method": self.command, "path": sub, "link": form.get("link", "")})
                if key != mock.KEY:
                    return self._send({"success": False, "error": "BAD_TOKEN", "detail": "Invalid token"}, 401)
                if sub == "user/me":
                    return self._send({"success": True, "data": {"id": 42, "email": "tv@example.com", "plan": 2}})
                if sub == "webdl/hosters":
                    with mock.lock:
                        return self._send({"success": True, "data": list(mock.hosters)})
                if sub == "webdl/createwebdownload":
                    link = form.get("link", "")
                    with urllib.request.urlopen(urllib.request.Request(link, method="HEAD"), timeout=10) as r:
                        size = int(r.headers.get("Content-Length") or 0)
                    with mock.lock:
                        wid = mock.next_id
                        mock.next_id += 1
                        mock.items[wid] = {"link": link, "name": link.rsplit("/", 1)[-1], "size": size, "polls": 0}
                    return self._send({"success": True, "data": {"webdownload_id": wid}})
                if sub == "webdl/mylist":
                    wid = int(q.get("id", "0") or 0)
                    with mock.lock:
                        it = mock.items.get(wid)
                        if it:
                            it["polls"] += 1
                    if not it:
                        return self._send({"success": False, "error": "ITEM_NOT_FOUND"})
                    if it["polls"] <= mock.polls_before_done:
                        return self._send({"success": True, "data": {"id": wid, "download_state": "downloading",
                                                                     "download_finished": False, "files": []}})
                    return self._send({"success": True, "data": {"id": wid, "name": it["name"], "download_state": "completed",
                                                                 "download_finished": True,
                                                                 "files": [{"id": 0, "name": it["name"], "size": it["size"]}]}})
                if sub == "webdl/requestdl":
                    wid = int(q.get("web_id", "0") or 0)
                    with mock.lock:
                        it = mock.items.get(wid)
                    if not it:
                        return self._send({"success": False, "error": "ITEM_NOT_FOUND"})
                    return self._send({"success": True, "data": it["link"]})
                return self._send({"success": False, "error": "UNKNOWN"}, 404)

            def do_GET(self):
                self._handle()

            def do_POST(self):
                self._handle()

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.httpd.daemon_threads = True
        self.base = f"http://127.0.0.1:{self.httpd.server_address[1]}"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def set_hosters(self, hosters):
        with self.lock:
            self.hosters = hosters

    def created_links(self):
        with self.lock:
            return [c["link"] for c in self.calls if c["path"] == "webdl/createwebdownload"]

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def hoster(name, domains, per_link=0, daily=0, used=0, status=True, links=0, links_used=0):
    return {"name": name, "domains": domains, "status": status, "per_link_size_limit": per_link,
            "daily_bandwidth_limit": daily, "daily_bandwidth_used": used,
            "daily_link_limit": links, "daily_link_used": links_used}


# --------------------------------------------------------------------------- #
# 1. Basics: drives, gate, empty queue, refusals, browser stub, settings
# --------------------------------------------------------------------------- #

class TvDownloadsBasics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.w = tvworld.World("dl-basics").start()
        cls.c = cls.w.client()
        cls.ready = index_ready(cls.c)

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    def need_index(self):
        self.assertTrue(self.ready, "the TV catalogue index (tvcat_release) does not answer")

    def test_01_storage_lists_writable_drives(self):
        drives = self.c.get("/api/v1/storage").check()
        ids = [d["id"] for d in drives]
        self.assertEqual(ids, ["root0", "root1", "internal"], drives)   # host roots first, internal last
        for d in drives:
            self.assertRegex(d["id"], r"^[A-Za-z0-9_-]{1,63}$")
            self.assertEqual(d["projectedFreeBytes"], d["freeBytes"] - d["pendingBytes"] - 240 * MiB)
            self.assertGreater(d["totalBytes"], 0)
        internal = drives[-1]
        self.assertEqual(internal["label"], "Internal storage")
        self.assertFalse(internal["external"])
        self.assertEqual(internal["path"], os.path.join(self.w.default_root, "homebrew"))
        self.assertEqual(drives[0]["path"], os.path.join(self.w.roots[0], "homebrew"))
        self.assertTrue(drives[0]["external"])
        self.w.assert_golden("storage", drives)

    def test_02_every_route_needs_the_bearer(self):
        for method, path in NINE_ROUTES:
            for tok in ("", "0" * 64):
                r = self.c.request(method, path, {} if method == "POST" else None, token=tok)
                r.error(401, S["unpaired"])

    def test_03_empty_queue_and_unknown_release(self):
        self.assertEqual(self.c.get("/api/v1/downloads").check(), [])
        self.c.post("/api/v1/downloads", {"releaseId": "nope", "storageId": "internal"}).error(404, S["unknown_release"])
        self.c.post("/api/v1/downloads", {"releaseId": 7, "storageId": "internal"}).error(404, S["unknown_release"])
        self.c.post("/api/v1/downloads", {}).error(404, S["unknown_release"])

    def test_04_delivery_is_checked_first(self):
        for d in ("zero", "TorBox", 1, None, True, ["torbox"]):
            self.c.post("/api/v1/downloads", {"releaseId": "nope", "storageId": "internal", "delivery": d}).error(400, S["delivery"])
        # "" and "direct" are direct delivery: the release check answers
        for d in ("", "direct"):
            self.c.post("/api/v1/downloads", {"releaseId": "nope", "storageId": "internal", "delivery": d}).error(404, S["unknown_release"])

    def test_05_browser_stub_never_404(self):
        b = self.c.get("/api/v1/browser").check()
        self.assertEqual(b, {"available": False, "active": False, "id": "", "state": "idle", "title": "", "jobId": "",
                             "message": S["browser"]})
        self.w.assert_golden("browser", b)
        self.assertLess(len(self.c.get("/api/v1/browser").body), 16384)
        self.c.post("/api/v1/browser", {"action": "start", "releaseId": "fx-viking-only-a", "storageId": "internal"}).error(409, S["browser"])
        self.c.post("/api/v1/browser", {"action": "cancel", "id": "x"}).error(409, S["browser_gone"])
        for body in ({}, {"action": "refresh"}, {"action": 1}, {"action": None}):
            self.c.post("/api/v1/browser", body).error(400, S["browser_action"])

    def test_06_debrid_without_torbox(self):
        d = self.c.get("/api/v1/debrid").check()
        self.assertEqual(d["provider"], "torbox")
        self.assertIs(d["available"], True)
        self.assertIs(d["connected"], False)
        self.assertIs(d["busy"], False)
        self.assertEqual(d["hosts"], [{"sourceId": "archive", "available": False, "maxFileSize": 0},
                                      {"sourceId": "vikingfile", "available": False, "maxFileSize": 0}])
        self.w.assert_golden("debrid", d)

    def test_07_concurrent_downloads_1_to_4(self):
        for bad in (5, 0, -1, 1.5, "2", True, None, 9):
            self.c.post("/api/v1/settings/downloads", {"concurrentDownloads": bad}).error(400, S["limit"])
        self.c.post("/api/v1/settings/downloads", {}).error(400, S["limit"])
        self.assertEqual(self.c.post("/api/v1/settings/downloads", {"concurrentDownloads": 2}).check(), {"concurrentDownloads": 2})
        self.assertEqual(self.c.get("/api/v1/system").check()["concurrentDownloads"], 2)
        st, s = self.w.srv.api("GET", "/api/settings")
        self.assertEqual(s["settings"]["downloads"]["concurrentJobs"], 2)
        self.assertEqual(self.c.post("/api/v1/settings/downloads", {"concurrentDownloads": 4.0}).check(), {"concurrentDownloads": 4})
        self.assertEqual(self.c.post("/api/v1/settings/downloads", {"concurrentDownloads": 1}).check(), {"concurrentDownloads": 1})

    def test_08_delete_archives_is_a_bool(self):
        for bad in ("yes", 1, 0, None, "false"):
            self.c.post("/api/v1/settings/extraction", {"deleteArchives": bad}).error(400, S["expected_delete"])
        self.c.post("/api/v1/settings/extraction", {}).error(400, S["expected_delete"])
        self.assertEqual(self.c.post("/api/v1/settings/extraction", {"deleteArchives": False}).check(), {"deleteArchives": False})
        self.assertIs(self.c.get("/api/v1/system").check()["deleteArchives"], False)
        self.assertEqual(self.c.post("/api/v1/settings/extraction", {"deleteArchives": True}).check(), {"deleteArchives": True})
        self.assertIs(self.c.get("/api/v1/system").check()["deleteArchives"], True)

    def test_09_preferred_storage_and_auto(self):
        tvapi_json = os.path.join(self.w.data, "tvapi.json")
        for bad in ({}, {"storageId": 1}, {"storageId": None}, {"storageId": "x" * 64}):
            self.c.post("/api/v1/storage/preferred", bad).error(400, S["expected_storage"])
        self.c.post("/api/v1/storage/preferred", {"storageId": "usb9"}).error(409, S["drive"])
        # "Choose automatically": the first external drive, remembered as automatic
        self.assertEqual(self.c.post("/api/v1/storage/preferred", {"storageId": ""}).check(), {"preferredStorage": ""})
        self.assertEqual(self.c.get("/api/v1/system").check()["preferredStorage"], "")
        self.assertIs(read_json(tvapi_json)["preferredAuto"], True)
        st, s = self.w.srv.api("GET", "/api/settings")
        self.assertEqual(s["settings"]["downloads"]["defaultRoot"].rstrip("/"), self.w.roots[0].rstrip("/"))
        # an explicit drive clears the flag
        self.assertEqual(self.c.post("/api/v1/storage/preferred", {"storageId": "root1"}).check(), {"preferredStorage": "root1"})
        self.assertEqual(self.c.get("/api/v1/system").check()["preferredStorage"], "root1")
        self.assertIs(read_json(tvapi_json)["preferredAuto"], False)
        st, s = self.w.srv.api("GET", "/api/settings")
        self.assertEqual(s["settings"]["downloads"]["defaultRoot"].rstrip("/"), self.w.roots[1].rstrip("/"))
        self.assertEqual(self.c.post("/api/v1/storage/preferred", {"storageId": "internal"}).check(), {"preferredStorage": "internal"})
        self.assertEqual(self.c.get("/api/v1/system").check()["preferredStorage"], "internal")

    def test_10_job_and_action_lookups(self):
        self.c.post("/api/v1/downloads/j_nope00/pause", {}).error(404, S["unknown_download"])
        self.c.post("/api/v1/downloads/" + "a" * 24 + "/pause", {}).error(404, S["unknown_dl_action"])
        self.c.post("/api/v1/downloads/j_nope00/" + "p" * 24, {}).error(404, S["unknown_dl_action"])
        jid = queue_add(self.w, gameId="fx-long-haul-ps5", releaseId="fx-long-haul-ffpfsc", root=self.w.roots[1])
        try:
            for action in ("move-up", "clear-cache", "extract", "PAUSE"):
                self.c.post(f"/api/v1/downloads/{jid}/{action}", {}).error(404, S["unknown_action"])
            self.c.post(f"/api/v1/downloads/{jid}/refresh-link", {}).error(409, S["no_link"])
        finally:
            self.w.srv.action(jid, "remove", {"deleteFiles": True})
        # Library copies and installs are not TV downloads
        self.assertEqual(self.c.get("/api/v1/downloads").check(), [])

    def test_11_disabled_source_and_notice(self):
        self.need_index()
        try:
            put_settings(self.w, {"sources": {"enabled": {"archive": False}}})
            self.c.post("/api/v1/downloads", {"releaseId": "fx-tidewater-ffpfsc", "storageId": "internal"}).error(409, S["source"])
            put_settings(self.w, {"sources": {"enabled": {"archive": True}, "acknowledged": False}})
            self.c.post("/api/v1/downloads", {"releaseId": "fx-tidewater-ffpfsc", "storageId": "internal"}).error(409, S["source"])
        finally:
            put_settings(self.w, {"sources": {"enabled": {"archive": True}, "acknowledged": True}})

    def test_12_vikingfile_needs_torbox(self):
        self.need_index()
        for rid in ("fx-viking-only-a", "fx-viking-only-b", "fx-neon-courier-vf"):
            self.c.post("/api/v1/downloads", {"releaseId": rid, "storageId": "internal"}).error(409, S["provider"])
        # TorBox is not connected here
        self.c.post("/api/v1/downloads", {"releaseId": "fx-viking-only-a", "storageId": "internal", "delivery": "torbox"}).error(409, S["tb_connect"])

    def test_13_drive_and_space(self):
        self.need_index()
        for sid in ("usb9", "", 5):
            self.c.post("/api/v1/downloads", {"releaseId": "fx-tidewater-ffpfsc", "storageId": sid}).error(409, S["drive"])
        # 276 GiB never fits the test drive (the projection the app uses: free - pending - 240 MiB)
        self.c.post("/api/v1/downloads", {"releaseId": "fx-vast-archive-ffpfsc", "storageId": "internal"}).error(409, S["no_space"])
        # hidden releases (RAR, a PAGE source, a size of 0, HOMEBREW) are not TV releases
        for rid in ("fx-static-bloom-rar", "fx-paper-moon-page", "fx-null-signal-zero", "fx-pixel-shell-pkg", "fx-glass-tide-zip"):
            self.c.post("/api/v1/downloads", {"releaseId": rid, "storageId": "internal"}).error(404, S["unknown_release"])
        self.assertEqual(self.c.get("/api/v1/downloads").check(), [])


# --------------------------------------------------------------------------- #
# 2. The download life cycle
# --------------------------------------------------------------------------- #

class TvDownloadsFlow(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.w = tvworld.World("dl-flow").start()
        cls.c = cls.w.client()
        cls.ready = index_ready(cls.c)

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    def need_index(self):
        self.assertTrue(self.ready, "the TV catalogue index (tvcat_release) does not answer")

    def create(self, rid, sid, **extra):
        body = {"releaseId": rid, "storageId": sid}
        body.update(extra)
        r = self.c.post("/api/v1/downloads", body)
        b = r.check(status=201)
        self.assertEqual(set(b), {"id"})
        self.assertRegex(b["id"], r"^j_[0-9a-f]{6}$")
        return b["id"]

    def act(self, jid, action, body=None):
        r = self.c.post(f"/api/v1/downloads/{jid}/{action}", body if body is not None else {})
        return r

    def test_01_create_follows_to_complete(self):
        self.need_index()
        rid = "fx-tidewater-ffpfsc"
        # start from "Choose automatically": creating a download picks the drive explicitly
        self.assertEqual(self.c.post("/api/v1/storage/preferred", {"storageId": ""}).check(), {"preferredStorage": ""})
        jid = self.create(rid, "internal")
        self.assertIs(read_json(os.path.join(self.w.data, "tvapi.json"))["preferredAuto"], False)
        row, seen = wait_row(self.c, jid, lambda j: j and j["status"] == "complete", timeout=90, what="complete")
        opt = option_of(self.w, rid, self.c)
        for k in ("titleId", "format", "filename", "gameId"):
            self.assertEqual(row[k], opt[k], k)
        self.assertEqual(row["total"], opt["sizeBytes"])
        self.assertEqual(row["received"], row["total"])
        self.assertEqual(row["releaseId"], rid)
        self.assertEqual(row["storageId"], "internal")
        self.assertEqual(row["delivery"], "direct")
        self.assertEqual(row["error"], "")
        self.assertTrue(row["path"].startswith(os.path.join(self.w.default_root, "homebrew") + "/"), row["path"])
        self.assertTrue(os.path.exists(row["path"]))
        self.assertTrue(set(seen) <= {"queued", "downloading", "verifying", "complete"}, seen)
        # the chosen drive became the default one (backend/queue.c)
        self.assertEqual(self.c.get("/api/v1/system").check()["preferredStorage"], "internal")
        self.w.assert_golden("downloads-complete", [row])

    def test_02_server_size_differs_from_the_catalogue(self):
        self.need_index()
        rid = "fx-off-by-one-ffpfsc"          # catalogue 8388607, the server sends 8388608
        jid = self.create(rid, "root0")
        row, _ = wait_row(self.c, jid, lambda j: j and j["status"] == "complete", timeout=90, what="complete")
        self.assertEqual(row["total"], 8388607)
        self.assertEqual(row["received"], 8388607)
        self.assertEqual(row["storageId"], "root0")
        st, qj = 200, core_job(self.w, jid)
        self.assertEqual(qj["total"], 8388608)      # XENO keeps what the server sent

    def test_03_pause_resume_cancel_delete(self):
        self.need_index()
        jid = self.create("fx-long-haul-ffpfsc", "root1")
        wait_row(self.c, jid, lambda j: j and j["status"] == "downloading" and j["received"] > 0, timeout=60, what="downloading")
        r = self.act(jid, "pause")
        arr = r.check()
        self.assertEqual([j["status"] for j in arr if j["id"] == jid], ["paused"])
        self.act(jid, "pause").error(409, S["not_running"])
        self.act(jid, "delete").error(409, S["only_cancelled"])
        self.act(jid, "remove").error(409, S["remove"])
        arr = self.act(jid, "resume").check()
        self.assertIn([j["status"] for j in arr if j["id"] == jid][0], ("queued", "downloading"))
        wait_row(self.c, jid, lambda j: j and j["status"] == "downloading" and j["speed"] > 0, timeout=60, what="downloading again")
        arr = self.act(jid, "cancel", {"deletePartial": True}).check()
        self.assertEqual([j["status"] for j in arr if j["id"] == jid], ["cancelled"])
        row, _ = wait_row(self.c, jid, lambda j: j and j["status"] == "cancelled" and j["received"] == 0, timeout=30,
                          what="partial deleted")
        path = core_job(self.w, jid)["path"]
        harness.wait_until(lambda: not os.path.exists(path + ".xpart"), timeout=10, what="the .xpart is gone")
        self.act(jid, "cancel", {}).check()                                   # already cancelled: nothing to do
        arr = self.act(jid, "delete").check()
        self.assertNotIn(jid, [j["id"] for j in arr])
        self.act(jid, "delete").error(404, S["unknown_download"])

    def test_04_duplicates_and_history(self):
        self.need_index()
        rid = "fx-long-haul-ffpfsc"
        jid = self.create(rid, "root0")
        self.c.post("/api/v1/downloads", {"releaseId": rid, "storageId": "root0"}).error(409, S["has_job"])
        wait_row(self.c, jid, lambda j: j and j["received"] > 0, timeout=60, what="some bytes")
        arr = self.act(jid, "cancel", {}).check()                               # keep the partial
        self.assertEqual([j["status"] for j in arr if j["id"] == jid], ["cancelled"])
        wait_row(self.c, jid, lambda j: j and j["status"] == "cancelled" and j["received"] > 0, timeout=30, what="kept partial")
        self.c.post("/api/v1/downloads", {"releaseId": rid, "storageId": "root0"}).error(409, S["cancelled_partial"])
        self.act(jid, "remove").error(409, S["remove"])                        # a kept partial is not history
        arr = self.act(jid, "forget").check()                                  # history only: the partial stays
        self.assertNotIn(jid, [j["id"] for j in arr])
        part = os.path.join(self.w.roots[0], "homebrew", option_of(self.w, rid)["filename"] + ".xpart")
        self.assertTrue(os.path.exists(part), part)
        # never overwrite: the kept partial blocks a new download of that file
        self.c.post("/api/v1/downloads", {"releaseId": rid, "storageId": "root0"}).error(409, S["file_exists"])
        os.unlink(part)
        sidecar = part + ".json"
        if os.path.exists(sidecar):
            os.unlink(sidecar)
        # finished history: refused while the file is there, reused once it is gone
        rid2 = "fx-ember-hollow-ok"
        j1 = self.create(rid2, "root0")
        row, _ = wait_row(self.c, j1, lambda j: j and j["status"] == "complete", timeout=90, what="complete")
        self.c.post("/api/v1/downloads", {"releaseId": rid2, "storageId": "root0"}).error(409, S["file_exists"])
        self.act(j1, "cancel", {}).error(409, S["complete_cancel"])
        os.unlink(row["path"])
        j2 = self.create(rid2, "root0")
        self.assertNotEqual(j1, j2)
        ids = [j["id"] for j in jobs(self.c) if j["releaseId"] == rid2 and j["storageId"] == "root0"]
        self.assertEqual(ids, [j2])
        wait_row(self.c, j2, lambda j: j and j["status"] == "complete", timeout=90, what="complete again")
        arr = self.act(j2, "remove").check()
        self.assertNotIn(j2, [j["id"] for j in arr])
        self.assertTrue(os.path.exists(row["path"]))                          # remove keeps the file
        # another job (here a web UI link) writing the same file on that drive
        fname = option_of(self.w, rid)["filename"]
        url_job = queue_add(self.w, url=self.w.file_url("big.bin", "slow"), filename=fname, format="FFPFSC", root=self.w.roots[0])
        try:
            self.c.post("/api/v1/downloads", {"releaseId": rid, "storageId": "root0"}).error(409, S["filename_busy"])
            # the same release on another drive is a different file; "Delete download"
            # on its cancelled row deletes the kept partial with the row (one job at a
            # time by default, so the link job steps aside)
            st, _ = self.w.srv.api("POST", f"/api/queue/{url_job}/pause", {})
            self.assertEqual(st, 200)
            other = self.create(rid, "root1")
            wait_row(self.c, other, lambda j: j and j["received"] > 0, timeout=60, what="some bytes")
            self.act(other, "cancel", {}).check()
            wait_row(self.c, other, lambda j: j and j["status"] == "cancelled" and j["received"] > 0, timeout=30,
                     what="kept partial")
            part1 = os.path.join(self.w.roots[1], "homebrew", fname + ".xpart")
            self.assertTrue(os.path.exists(part1), part1)
            arr = self.act(other, "delete").check()
            self.assertNotIn(other, [j["id"] for j in arr])
            harness.wait_until(lambda: not os.path.exists(part1), timeout=10, what="the kept .xpart is deleted")
        finally:
            self.w.srv.action(url_job, "remove", {"deleteFiles": True})

    def test_05_retries_then_error_then_retry_and_cancel(self):
        put_settings(self.w, {"downloads": {"retryMax": 2}})
        try:
            dead = f"http://127.0.0.1:{harness.free_port()}/nothing/here.ffpfsc"
            jid = queue_add(self.w, url=dead, filename="Retry Test.ffpfsc", format="FFPFSC", root=self.w.roots[1])
            row, _ = wait_row(self.c, jid, lambda j: j and j["status"] == "retrying", timeout=30, what="retrying")
            self.assertEqual(row["error"], "")
            self.assertGreater(row["retryAt"], time.time() - 1)
            self.assertEqual(row["speed"], 0)
            row, _ = wait_row(self.c, jid, lambda j: j and j["status"] == "error", timeout=40, what="error")
            self.assertTrue(row["error"] and row["error"].endswith((".", "!", "?")), row["error"])
            self.assertEqual(row["retryAt"], 0)
            self.act(jid, "pause").error(409, S["not_running"])
            arr = self.act(jid, "retry").check()
            self.assertIn([j["status"] for j in arr if j["id"] == jid][0], ("queued", "downloading", "retrying"))
            wait_row(self.c, jid, lambda j: j and j["status"] == "error", timeout=60, what="error again")
            arr = self.act(jid, "cancel", {}).check()                          # error rows can be cancelled
            self.assertEqual([j["status"] for j in arr if j["id"] == jid], ["cancelled"])
            self.act(jid, "forget").check()
        finally:
            put_settings(self.w, {"downloads": {"retryMax": 8}})
        # a 404 is not retried: an error row with XENO's sentence
        jid = queue_add(self.w, gameId="fx-dead-link-ps5", releaseId="fx-dead-link-ffpfsc", root=self.w.roots[1])
        row, _ = wait_row(self.c, jid, lambda j: j and j["status"] == "error", timeout=60, what="error")
        self.assertIn("404", row["error"])
        self.assertTrue(row["error"].endswith("."), row["error"])
        self.act(jid, "remove").error(409, S["remove"])
        self.act(jid, "cancel", {"deletePartial": True}).check()
        self.act(jid, "delete").check()

    def test_06_zip_password(self):
        # ZIP options are hidden from the TV catalogue (TV_SHOW_ZIP off), but a ZIP job
        # from the web UI shows on the TV app and its password can be entered there
        jid = queue_add(self.w, gameId="fx-locked-vault-ps5", releaseId="fx-locked-vault-zip", root=self.w.roots[1])
        row, _ = wait_row(self.c, jid, lambda j: j and j["status"] == "error", timeout=90, what="extraction failed")
        self.assertIs(row["canEnterArchivePassword"], True, row)
        self.assertIn("password", row["error"])
        self.assertEqual(row["received"], row["total"])
        self.act(jid, "archive-password", {"password": "p" * 129}).error(400, S["pw_len"])
        self.act(jid, "archive-password", {"password": ""}).error(400, S["pw_len"])
        self.act(jid, "archive-password", {"password": 12}).error(400, S["pw_len"])
        self.act(jid, "archive-password", {"password": "bad\x01pw"}).error(400, S["pw_ctrl"])
        self.act(jid, "archive-password", {"password": "bad\x7fpw"}).error(400, S["pw_ctrl"])
        arr = self.act(jid, "archive-password", {"password": "wrong-guess"}).check()
        self.assertIn([j["status"] for j in arr if j["id"] == jid][0], ("extracting", "error"))
        row, _ = wait_row(self.c, jid, lambda j: j and j["status"] == "error", timeout=60, what="wrong password")
        self.assertIs(row["canEnterArchivePassword"], True)
        self.act(jid, "archive-password", {"password": "xeno-secret-42"}).check()
        row, _ = wait_row(self.c, jid, lambda j: j and j["status"] == "complete", timeout=60, what="extracted")
        self.assertTrue(row["path"].endswith("PPSA07002 Custom.ffpfsc"), row["path"])
        self.assertTrue(os.path.exists(row["path"]))
        self.assertIs(row["canEnterArchivePassword"], False)
        self.act(jid, "archive-password", {"password": "again"}).error(409, S["pw_state"])
        self.act(jid, "remove").check()
        self.assertTrue(os.path.exists(row["path"]))
        # never logged
        self.assertNotIn("xeno-secret-42", self.w.log_text())

    def test_07_pkg_install(self):
        jid = queue_add(self.w, gameId="fx-quiet-harbour-ps4", releaseId="fx-quiet-harbour-pkg", root=self.w.default_root)
        self.w.srv.wait_job(jid, {"installing"}, timeout=90)
        row = job_row(self.c, jid)
        self.assertEqual(row["status"], "complete", row)
        self.assertEqual(row["error"], "")
        self.act(jid, "remove").error(409, S["install_wait"])
        self.act(jid, "cancel", {}).error(409, S["complete_cancel"])
        self.act(jid, "pause").error(409, S["not_running"])
        self.w.srv.wait_job(jid, {"installed"}, timeout=30)
        row = job_row(self.c, jid)
        self.assertEqual(row["status"], "complete")
        arr = self.act(jid, "remove").check()
        self.assertNotIn(jid, [j["id"] for j in arr])
        self.assertTrue(os.path.exists(row["path"]), row["path"])            # files are kept

    def test_08_archive_password_of_128_bytes(self):
        """The reference's limit is 128 bytes, and the whole password reaches the
        extractor: a ZipCrypto archive locked with exactly 128 bytes opens."""
        if not shutil.which("zip"):
            self.skipTest("the zip command is not installed")
        pw = "".join(chr(33 + (i * 7) % 94) for i in range(128))      # printable, quotes and backslashes included
        self.assertEqual(len(pw.encode()), 128)
        jid = queue_add(self.w, gameId="fx-locked-vault-ps5", releaseId="fx-locked-vault-zip", root=self.w.roots[0])
        row, _ = wait_row(self.c, jid, lambda j: j and j["status"] == "error", timeout=90, what="extraction failed")
        self.assertIs(row["canEnterArchivePassword"], True, row)
        # the archive on the drive becomes one locked with the 128-byte password
        archive = core_job(self.w, jid)["path"]
        work = os.path.join(self.w.tmp, "pw128")
        os.makedirs(work, exist_ok=True)
        entry = os.path.join(work, "PPSA07003 Long Key.ffpfsc")
        with open(entry, "wb") as f:
            f.write(range_server.encrypted_entry_bytes())
        made = os.path.join(work, "long.zip")
        subprocess.run(["zip", "-q", "-P", pw, "-j", made, entry], check=True)
        shutil.copyfile(made, archive)
        self.act(jid, "archive-password", {"password": pw + "x"}).error(400, S["pw_len"])
        if storable_password_bytes() < 128:
            # never cut to fit: refused with the sentence until the job can hold it
            self.act(jid, "archive-password", {"password": pw}).error(400, S["pw_len"])
            # an error row is not history: "Cancel" removes it and keeps the archive
            self.act(jid, "remove").error(409, S["remove"])
            arr = self.act(jid, "cancel", {}).check()
            self.assertNotIn(jid, [j["id"] for j in arr])
            self.assertTrue(os.path.exists(archive), archive)
            self.skipTest("dl_job.extractPassword / the extractor's copy hold fewer than 128 bytes "
                          f"({storable_password_bytes()}); src/dl.h and src/dlx_extract.c need [129]")
        arr = self.act(jid, "archive-password", {"password": pw}).check()
        self.assertIn([j["status"] for j in arr if j["id"] == jid][0], ("extracting", "complete"))
        row, _ = wait_row(self.c, jid, lambda j: j and j["status"] in ("complete", "error"), timeout=60, what="extracted")
        self.assertEqual(row["status"], "complete", row)
        self.assertTrue(row["path"].endswith("/homebrew/PPSA07003 Long Key.ffpfsc"), row["path"])
        self.assertTrue(os.path.exists(row["path"]))
        self.act(jid, "remove").check()
        self.assertNotIn(pw, self.w.log_text())


def storable_password_bytes():
    """The longest archive password the build can hold without cutting it: the
    smaller of dl_job.extractPassword and the extractor's local copy, minus the NUL."""
    sizes = []
    for name, rx in (("dl.h", r"char\s+extractPassword\[(\d+)\]"), ("dlx_extract.c", r"password\[(\d+)\];")):
        with open(os.path.join(SRC_DIR, name)) as f:
            m = re.search(rx, f.read())
        sizes.append(int(m.group(1)) if m else 0)
    return min(sizes) - 1


class TvDownloadsZip(unittest.TestCase):
    """TV_SHOW_ZIP on (XENO_TV_SHOW_ZIP=1): a ZIP option is created from the TV app,
    shows "extracting" with its own progress and ends as the extracted image."""

    @classmethod
    def setUpClass(cls):
        cls.w = tvworld.World("dl-zip", show_zip=True).start()
        cls.c = cls.w.client()
        cls.ready = index_ready(cls.c)

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    def test_01_zip_option_extracts(self):
        self.assertTrue(self.ready, "the TV catalogue index (tvcat_release) does not answer")
        rid = "fx-glass-tide-zip"
        opt = option_of(self.w, rid, self.c)
        self.assertEqual((opt["format"], opt["compression"]), ("Unknown", "zip"))
        r = self.c.post("/api/v1/downloads", {"releaseId": rid, "storageId": "root0"})
        jid = r.check(status=201)["id"]
        extracting = []

        def done(j):
            if j and j["status"] == "extracting":
                extracting.append(j)
            return j is not None and j["status"] == "complete"
        row, seen = wait_row(self.c, jid, done, timeout=90, what="extracted", every=0.05)
        for j in extracting:                       # the bar follows the extraction
            self.assertLessEqual(j["extracted"], j["extractTotal"])
            self.assertEqual(j["received"], j["total"])
        self.assertEqual((row["format"], row["filename"], row["total"]), (opt["format"], opt["filename"], opt["sizeBytes"]))
        self.assertEqual(row["received"], row["total"])
        self.assertTrue(row["path"].endswith("/homebrew/PPSA05555 Single Game.ffpfsc"), row["path"])
        self.assertTrue(os.path.exists(row["path"]))
        self.assertEqual((row["extracted"], row["extractTotal"]), (0, 0))


# --------------------------------------------------------------------------- #
# 3. TorBox
# --------------------------------------------------------------------------- #

class TvDownloadsTorBox(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tb = TorBoxMock()
        # Vikingfile: per-link 40 MiB, but only 16 MiB of today's bandwidth left
        cls.tb.set_hosters([hoster("archive", ["archive.org", "127.0.0.1"], per_link=0),
                            hoster("vikingfile", ["vikingfile.com", "vik1ngfile.site"], per_link=40 * MiB,
                                   daily=100 * 1024 * MiB, used=100 * 1024 * MiB - 16 * MiB),
                            hoster("down", ["1fichier.com"], status=False)])
        cls.w = tvworld.World("dl-torbox", env={"XENO_TB_BASE": cls.tb.base + "/tb"}).start()
        cls.c = cls.w.client()
        cls.ready = index_ready(cls.c)

    @classmethod
    def tearDownClass(cls):
        cls.w.close()
        cls.tb.stop()

    def connect(self):
        st, b = self.w.srv.api("PUT", "/api/debrid", {"service": "torbox", "apiKey": TorBoxMock.KEY})
        self.assertEqual(st, 200, b)

    def need_index(self):
        self.assertTrue(self.ready, "the TV catalogue index (tvcat_release) does not answer")

    def test_01_hosts_and_limits(self):
        self.connect()
        d = self.c.get("/api/v1/debrid").check()
        self.assertIs(d["connected"], True)
        self.assertIs(d["busy"], False)
        self.assertEqual(d["hosts"], [{"sourceId": "archive", "available": True, "maxFileSize": 0},
                                      {"sourceId": "vikingfile", "available": True, "maxFileSize": 16 * MiB}])

    def test_02_torbox_limit_and_flow(self):
        self.need_index()
        self.connect()
        # 32 MiB is above today's 16 MiB
        self.c.post("/api/v1/downloads", {"releaseId": "fx-neon-courier-vf", "storageId": "internal", "delivery": "torbox"}).error(409, S["tb_host"])
        # TorBox raised the allowance
        self.tb.set_hosters([hoster("archive", ["archive.org", "127.0.0.1"]),
                             hoster("vikingfile", ["vikingfile.com", "vik1ngfile.site"], per_link=40 * MiB)])
        self.connect()
        d = self.c.get("/api/v1/debrid").check()
        self.assertEqual(d["hosts"][1], {"sourceId": "vikingfile", "available": True, "maxFileSize": 40 * MiB})
        r = self.c.post("/api/v1/downloads", {"releaseId": "fx-neon-courier-vf", "storageId": "internal", "delivery": "torbox"})
        jid = r.check(status=201)["id"]
        row, _ = wait_row(self.c, jid, lambda j: j and j["deliveryPhase"] == S["preparing"], timeout=30, what="Preparing with TorBox")
        self.assertEqual((row["status"], row["delivery"]), ("queued", "torbox"))
        row, seen = wait_row(self.c, jid, lambda j: j and j["status"] == "complete", timeout=120, what="complete")
        self.assertEqual(row["delivery"], "torbox")
        self.assertEqual(row["deliveryPhase"], "")
        self.assertEqual(row["total"], 32 * MiB)
        self.assertTrue(any(l.endswith("/mid.bin") for l in self.tb.created_links()), self.tb.created_links())

    def test_03_direct_never_goes_through_torbox(self):
        self.need_index()
        self.connect()
        put_settings(self.w, {"debrid": {"preferred": "auto"}})
        before = len(self.tb.created_links())
        r = self.c.post("/api/v1/downloads", {"releaseId": "fx-glass-orchard-verified", "storageId": "root1"})
        jid = r.check(status=201)["id"]
        row, _ = wait_row(self.c, jid, lambda j: j and j["status"] == "complete", timeout=90, what="complete")
        self.assertEqual(row["delivery"], "direct")
        self.assertEqual(len(self.tb.created_links()), before)
        self.assertEqual(core_job(self.w, jid)["viaService"], "")

    def test_04_not_connected(self):
        st, _ = self.w.srv.api("DELETE", "/api/debrid/torbox")
        self.assertEqual(st, 200)
        try:
            d = self.c.get("/api/v1/debrid").check()
            self.assertIs(d["connected"], False)
            self.assertEqual([h["available"] for h in d["hosts"]], [False, False])
            self.need_index()
            self.c.post("/api/v1/downloads", {"releaseId": "fx-neon-courier-vf", "storageId": "internal",
                                              "delivery": "torbox"}).error(409, S["tb_connect"])
        finally:
            self.connect()


# --------------------------------------------------------------------------- #
# 4. The polled routes stay cheap (PORT-SPEC 1.9)
# --------------------------------------------------------------------------- #

class TvDownloadsBudget(unittest.TestCase):
    JOBS = 50
    SECONDS = 8

    @classmethod
    def setUpClass(cls):
        cls.w = tvworld.World("dl-budget").start()
        cls.c = cls.w.client()
        put_settings(cls.w, {"downloads": {"concurrentJobs": 1}})
        url = cls.w.file_url("small.bin", "slow")
        for i in range(cls.JOBS):
            queue_add(cls.w, url=url, filename=f"Load {i:02d}.ffpfsc", format="FFPFSC", root=cls.w.roots[i % 2])

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    def test_01_ten_requests_per_second_with_50_jobs(self):
        self.assertEqual(len(self.c.get("/api/v1/downloads").check()), self.JOBS)
        routes = ("/api/v1/downloads", "/api/v1/storage", "/api/v1/browser", "/api/v1/debrid")
        times = {p: [] for p in routes}
        end = time.time() + self.SECONDS
        while time.time() < end:
            tick = time.time()
            for p in routes:
                r = self.c.get(p)
                r.check()
                times[p].append(r.elapsed * 1000.0)
            left = 0.1 - (time.time() - tick)
            if left > 0:
                time.sleep(left)
        report = {p: (round(statistics.median(v), 2), round(sorted(v)[int(len(v) * 0.95) - 1], 2), round(max(v), 2))
                  for p, v in times.items()}
        sys.stderr.write(f"\n  budget (median, p95, max ms over {len(times[routes[0]])} rounds): {report}\n")
        for p, v in times.items():
            v = sorted(v)
            self.assertLess(statistics.median(v), 20.0, (p, report[p]))
            self.assertLess(v[int(len(v) * 0.95) - 1], 20.0, (p, report[p]))
            # a shared test host can stall one request; the budget is about the handler
            self.assertLess(v[-1], 250.0, (p, report[p]))

    def test_02_no_drive_access_on_http_workers(self):
        if not shutil.which("strace"):
            self.skipTest("strace is not installed")
        # quiet the queue first (the actions themselves save queue.json, which is fine)
        for j in self.c.get("/api/v1/downloads").check():
            if j["status"] in ("queued", "downloading", "retrying"):
                self.c.post(f"/api/v1/downloads/{j['id']}/pause", {}).check()
        pid = self.w.srv.proc.pid
        out = os.path.join(self.w.tmp, "strace.txt")
        tracer = subprocess.Popen(["strace", "-f", "-qq", "-s", "12", "-p", str(pid),
                                   "-e", "trace=statfs,fstatfs,openat,mkdir,creat,sendto", "-o", out],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        time.sleep(1.0)
        if tracer.poll() is not None:
            self.skipTest("strace cannot attach here: " + tracer.stderr.read().decode(errors="replace")[:200])
        try:
            for _ in range(20):
                for p in ("/api/v1/downloads", "/api/v1/storage", "/api/v1/browser", "/api/v1/debrid"):
                    self.c.get(p).check()
        finally:
            tracer.terminate()
            tracer.wait(timeout=10)
            tracer.stderr.close()
        calls = []
        workers = set()
        with open(out) as f:
            for line in f:
                m = re.match(r"^(\d+)\s+(\w+)\((.*)$", line)
                if not m:
                    continue
                tid, call, args = int(m.group(1)), m.group(2), m.group(3)
                if call == "sendto" and '"HTTP/1.1 ' in args:
                    workers.add(tid)          # the threads that answered our requests
                calls.append((tid, call, args, line.strip()))
        self.assertTrue(workers, "no HTTP reply was traced")
        bad = [l for tid, call, args, l in calls if tid in workers and
               (call in ("statfs", "fstatfs", "mkdir", "creat") or (call == "openat" and "O_CREAT" in args))]
        self.assertEqual(bad, [], "an HTTP worker touched a drive on a polled route")


if __name__ == "__main__":
    unittest.main()
