#!/usr/bin/env python3
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
test_tvapi_library.py — the Library routes of the /api/v1 layer the native TV
app talks to (PORT-SPEC 1.5.4, 1.9; map-library §3-§6, §12 items 1-10):

  TvLibrary        one backend with two extra roots (R1, R2), the fixture
                   catalogue plus a few Library-only games, and the seeded
                   Library files of tvworld.LIBRARY_FILES:
                     - 401 without a Bearer, first reply "idle" + busy, then
                       "ready" with the expected rows (D9, D10, D11, D15 steps
                       3-4, sourceKey, location, managed per format) and nothing
                       for partials, untagged files or our own PPSA99500;
                     - title IDs from the catalogue filename (D15 step 2) and
                       from the finished download job (step 1);
                     - /library/storage drives and destinations (sha256 ids);
                     - the action validator of the reference (400/409 texts);
                     - scan, copy (202 + snapshot, exact replay, reused id),
                       the refusals of the worker, the D7 interlock, adoption
                       of a web-UI copy, cancel of a queued and of a running
                       copy, move, rows carried while a drive is away.
  TvLibraryQuiet   a quiet backend with a bigger Library: no walk while nothing
                   changes (GET re-checks every 30 s), never stale, every reply
                   inside the PORT-SPEC 1.9 budget (< 100 ms), a POST refresh
                   walks again.

Every /api/v1 refusal is checked with tvcontract.assert_error_envelope and every
success with the contract field tables (tvcontract.check_reply, strict JSON).
"""
import copy
import hashlib
import json
import os
import re
import shutil
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tvcontract as tc  # noqa: E402
import tvworld  # noqa: E402

MiB = 1024 * 1024
LIB = "/api/v1/library"
LIB_STORAGE = "/api/v1/library/storage"
ACTIONS = "/api/v1/library/actions"

# map-library §6.7: the app's translation keys, verbatim
S_BAD = "Expected a supported action and its current Library identifiers."
S_CONFIRM = "Confirm this action first."
S_REUSED = "This request ID was already used for another action."
S_IN_PROGRESS = "Another Library operation is in progress. Wait for it to finish."
S_INCOMPATIBLE = "Refresh Library with a compatible ShadowMount version before using this action."
S_WAITING = "Waiting to start…"
S_UPDATED = "Library updated."
S_SCAN_REQUESTED = "Scan requested. ShadowMount may register or mount detected games. Refresh Library after it finishes."
S_STARTED = "Storage operation started. Progress is available in Library and Storage."
S_CANCEL_REQUESTED = "Cancellation requested. Waiting for the storage job to stop."
S_PAUSE_DOWNLOADS = "Pause active and queued downloads before copying or moving games."
S_SOURCE_CHANGED = "This game's source changed. Refresh Library and select it again."
S_SOURCE_UNSUPPORTED = "This source is missing or does not support that action."
S_DEST_CHANGED = "The selected destination changed or is no longer writable. Refresh storage."
S_DEST_INSIDE = "Choose a different destination outside the source folder."
S_EXISTS = "A file with this name is already on that drive. XENO STORE will not overwrite it."
S_JOB_FINISHED = "This job has finished or can no longer be cancelled."
S_LIBRARY_COPY = "Wait for the Library copy to finish before downloading."    # map-downloads §12
S_NOT_READY = "This part of XENO STORE is not ready yet."                     # a subsystem still stubbed

CAPS = ["rescan", "copy_game_source", "move_game_source", "storage_job_status", "storage_job_cancel", "storage_space"]

# Library-only catalogue games (added to the fixture for this suite):
#  - a release whose file name carries no ID and whose game title differs from
#    the cleaned file name: only the catalogue filename index resolves it (D15 2);
#  - two games whose releases share one file name with different title IDs: the
#    filename index gives the first, a finished download job gives its own (D15 1).
EXTRA_GAMES = [
    {"id": "fx-lib-deep-blue-ps4", "slug": "fx-lib-deep-blue", "title": "Deep Blue Voyage", "platform": "PS4",
     "titleIds": ["CUSA03349"], "genres": ["Adventure"], "addedAt": "2026-03-01",
     "releases": [{"id": "fx-lib-deep-blue-pkg", "sourceId": "archive", "format": "PKG", "kind": "game",
                   "provider": "Archive.org", "url": "{{FILES}}/ok/tiny.pkg", "sizeBytes": 2097152,
                   "filename": "ABZU - [EU] [EN] [1.01].pkg", "health": "ok", "verifiedAt": "2026-10-01"}]},
    {"id": "fx-lib-twin-a-ps5", "slug": "fx-lib-twin-a", "title": "Twin Edition Alpha", "platform": "PS5",
     "titleIds": ["PPSA07101"], "genres": ["Action"], "addedAt": "2026-03-02",
     "releases": [{"id": "fx-lib-twin-a-ffpfsc", "sourceId": "archive", "format": "FFPFSC", "kind": "game",
                   "provider": "Archive.org", "url": "{{FILES}}/ok/small.bin", "sizeBytes": 8388608,
                   "filename": "Twin Edition.ffpfsc", "titleId": "PPSA07101", "health": "ok",
                   "verifiedAt": "2026-10-01"}]},
    {"id": "fx-lib-twin-b-ps5", "slug": "fx-lib-twin-b", "title": "Twin Edition Beta", "platform": "PS5",
     "titleIds": ["PPSA07102"], "genres": ["Action"], "addedAt": "2026-03-03",
     "releases": [{"id": "fx-lib-twin-b-ffpfsc", "sourceId": "archive", "format": "FFPFSC", "kind": "game",
                   "provider": "Archive.org", "url": "{{FILES}}/ok/small.bin", "sizeBytes": 8388608,
                   "filename": "Twin Edition.ffpfsc", "titleId": "PPSA07102", "health": "ok",
                   "verifiedAt": "2026-10-01"}]},
]

# Extra files next to tvworld.LIBRARY_FILES: (root index, relative path, size)
EXTRA_FILES = (
    (0, "xeno-store/pkg/ABZU - [EU] [EN] [1.01].pkg", 2 * MiB),     # D15 step 2 only -> CUSA03349
    (0, "homebrew/PPSA77777 Paper Lanterns.ffpfs", 8 * MiB),       # step 2 (PPSA90006) beats step 3 (PPSA77777)
    (0, "xeno-store/pkg/Quiet Harbour.pkg", 2 * MiB),              # catalogue title, game ID cusa90007 -> CUSA90007
    (0, "homebrew/Signal Fire.exfat", 1 * MiB),                    # ID only in the catalogue URL -> PLJM12345 (step 2)
    (0, "xeno-store/pkg/Moss Lantern Collection_1.pkg", 2 * MiB),  # no title ID anywhere: not listed
)


def sha(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def source_key(tid, path, fmt):
    """REF/library.c:128-134 identity(titleId, path, format)."""
    return sha(f"{tid}\n{path}\n{fmt}")


def drive_id(root):
    return sha("drive\n" + root)


def dest_id(root):
    return sha("dest\n" + root + "/homebrew")


def sparse(path, size):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.truncate(size)


def library_catalog():
    with open(tvworld.FIXTURE_CATALOG, encoding="utf-8") as f:
        cat = json.load(f)
    cat["games"] = cat["games"] + copy.deepcopy(EXTRA_GAMES)
    return cat


class LibraryMixin:
    """Helpers shared by the Library classes (self.w: World, self.c: TvClient)."""

    def library(self, method="GET"):
        r = self.c.request(method, LIB, {} if method == "POST" else None)
        return r.check()

    def storage(self, method="GET"):
        r = self.c.request(method, LIB_STORAGE, {} if method == "POST" else None)
        return r.check()

    def wait_library(self, until, timeout=30.0, refresh=False, what="library state"):
        """Polls GET (or POST, which re-checks every 5 s) until until(snapshot)."""
        deadline = time.monotonic() + timeout
        last = None
        while True:
            last = self.library("POST" if refresh else "GET")
            if until(last):
                return last
            if time.monotonic() >= deadline:
                raise AssertionError(f"timeout waiting for {what}; last {json.dumps(last)[:1500]}")
            time.sleep(0.5 if refresh else 0.2)

    def rows(self, snap, tid=None):
        return [g for g in snap["games"] if tid is None or g["titleId"] == tid]

    def row_at(self, snap, path):
        hits = [g for g in snap["games"] if g["path"] == path]
        return hits[0] if hits else None

    def dest(self, root):
        st = self.storage()
        for d in st["destinations"]:
            if d["mountPoint"] == root:
                return d
        raise AssertionError(f"no destination for {root}: {st['destinations']}")

    def act(self, body):
        """POSTs an accepted library action: 202 with the full snapshot. Returns the snapshot."""
        return self.c.post(ACTIONS, body).check()

    def run_action(self, body, rid, timeout=60.0):
        """POSTs an action (202 + snapshot) and waits until it ends; returns the final snapshot."""
        v = self.act(body)
        self.assertEqual(v["action"]["id"], rid, v["action"])
        self.assertIn(v["action"]["state"], ("queued", "running", "complete", "error"))
        return self.wait_library(lambda s: s["action"] and s["action"]["id"] == rid and
                                 s["action"]["state"] in ("complete", "error"), timeout=timeout,
                                 what=f"action {rid} to end")

    def copy_jobs(self):
        st, b = self.w.srv.api("GET", "/api/queue?history=1")
        self.assertEqual(st, 200, b)
        return [j for j in b["jobs"] if j.get("type") == "copy"]

    def tvcat_live(self):
        """The TV catalogue index (tvapi_catalog.c) answers: D15 steps 1-2 can work."""
        return self.c.get("/api/v1/catalog").status == 200


class TvLibrary(unittest.TestCase, LibraryMixin):
    @classmethod
    def setUpClass(cls):
        cls.w = tvworld.World("library", catalog=library_catalog(), library=True).start()
        cls.r1, cls.r2 = cls.w.roots
        for where, rel, size in EXTRA_FILES:
            sparse(os.path.join(cls.w.roots[where], rel), size)
        cls.c = cls.w.client()            # pairs (GET /session): the lazy start, no Library scan yet
        cls.rid_n = 0

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    def rid(self):
        type(self).rid_n += 1
        return "tv-19a3f2c1b40-%x" % type(self).rid_n

    # ---- 1-3: auth, the first snapshot, the rows -------------------------------
    def test_01_unpaired_gets_401(self):
        u = self.w.client(paired=False)
        for method, path, body in (("GET", LIB, None), ("POST", LIB, {}), ("GET", LIB_STORAGE, None),
                                   ("POST", LIB_STORAGE, {}),
                                   ("POST", ACTIONS, tc.AppBody.library_action("scan", "tv-1-00000001", confirmed=True))):
            u.request(method, path, body).error(401, tc.S["unpaired"])
            u.request(method, path, body, token="0" * 64).error(401, tc.S["unpaired"])

    def test_02_first_reply_idle_then_ready_rows(self):
        first = self.library()
        self.assertEqual(first["status"], "idle")
        self.assertIs(first["busy"], True)          # the first GET scheduled the first walk
        self.assertIs(first["stale"], False)
        self.assertEqual((first["updatedAt"], first["games"]), (0, []))
        self.assertEqual(first["capabilities"], CAPS)
        self.assertIsNone(first["action"])
        self.assertIsNone(first["storageJob"])
        self.assertEqual((first["provider"], first["storageError"], first["storageBusy"]), ("XENO STORE", "", False))
        snap = self.wait_library(lambda s: s["status"] == "ready" and not s["busy"], timeout=15, what="ready")
        self.assertEqual(snap["message"], "")
        self.assertIs(snap["stale"], False)
        self.assertGreater(snap["updatedAt"], 0)
        self.assertGreaterEqual(snap["checkedAt"], snap["updatedAt"])
        self.assertEqual(snap["capabilities"], CAPS)
        r1, r2, data = self.r1, self.r2, self.w.data

        def expect(path, **want):
            g = self.row_at(snap, path)
            self.assertIsNotNone(g, f"no row for {path}: {[x['path'] for x in snap['games']]}")
            for k, v in want.items():
                self.assertEqual(g[k], v, f"{path}: {k}")
            self.assertEqual(g["sourceKey"], source_key(g["titleId"], g["path"], g["format"]))
            self.assertEqual((g["runtimePath"], g["mounted"], g["installedSizeBytes"]), ("", False, None))
            return g

        expect(os.path.join(r1, "homebrew/PPSA01234.exfat"), titleId="PPSA01234", title="Harbor Lights",
               platform="ps5", format="exFAT", location="R1", installed=False, onDrive=True, managed=True,
               canManageSource=True, sizeBytes=8 * MiB, sizeStatus="ready")
        expect(os.path.join(r1, "xeno-store/pkg/CUSA00001.pkg"), titleId="CUSA00001", title="Bramble Path",
               platform="ps4", format="PKG", location="R1", onDrive=True, managed=False, canManageSource=False,
               sizeBytes=2 * MiB, sizeStatus="ready")
        # a catalogue file name without an ID: the exact catalogue title (D15 step 4, or step 2)
        expect(os.path.join(r1, "xeno-store/pkg/Lantern Rite.pkg"), titleId="CUSA00002", title="Lantern Rite",
               platform="ps4", format="PKG", managed=False)
        expect(os.path.join(r1, "xeno-store/pkg/Quiet Harbour.pkg"), titleId="CUSA90007", title="Quiet Harbour",
               platform="ps4", format="PKG")
        expect(os.path.join(r2, "homebrew/PPSA90013.ffpfsc"), titleId="PPSA90013", title="Aurora Drift",
               platform="ps5", format="FFPFSC", location="R2", managed=True, canManageSource=True, onDrive=True)
        # D11: installed on the console with no source file
        expect(os.path.join(data, "user/app/PPSA05555"), titleId="PPSA05555", title="Glass Tide", platform="ps5",
               format="", location="Internal storage", installed=True, onDrive=True, managed=False,
               canManageSource=False, sizeBytes=None, sizeStatus="unavailable")
        paths = [g["path"] for g in snap["games"]]
        for absent in ("homebrew/PPSA02222.exfat.xpart", "homebrew/game.exfat", "homebrew/PPSA99500.ffpkg",
                       "xeno-store/pkg/Moss Lantern Collection_1.pkg"):
            self.assertNotIn(os.path.join(r1, absent), paths)
        self.assertNotIn("PPSA99500", [g["titleId"] for g in snap["games"]])   # D9
        self.assertNotIn("PPSA02222", [g["titleId"] for g in snap["games"]])   # D10
        for g in snap["games"]:
            self.assertRegex(g["sourceKey"], r"^[0-9a-f]{64}$")
        # stable across polls: the same rows, byte for byte
        again = self.library()
        self.assertEqual(again["games"], snap["games"])
        self.assertIs(again["stale"], False)
        golden = os.path.join(tvworld.GOLDEN_DIR, "library.json")
        if tvworld.golden_update_mode() or os.path.exists(golden):
            self.w.assert_golden("library", snap)

    def test_03_title_id_from_catalogue_filename(self):
        """D15 step 2: the TV catalogue filename index wins over an ID in the name."""
        if not self.tvcat_live():
            self.skipTest("the TV catalogue index (tvapi_catalog.c) is not implemented in this build")
        abzu = os.path.join(self.r1, "xeno-store/pkg/ABZU - [EU] [EN] [1.01].pkg")
        lanterns = os.path.join(self.r1, "homebrew/PPSA77777 Paper Lanterns.ffpfs")
        signal = os.path.join(self.r1, "homebrew/Signal Fire.exfat")
        snap = self.wait_library(lambda s: self.row_at(s, abzu) is not None and self.row_at(s, lanterns) and
                                 self.row_at(s, lanterns)["titleId"] == "PPSA90006",
                                 timeout=45, refresh=True, what="catalogue filename resolution")
        g = self.row_at(snap, abzu)
        self.assertEqual((g["titleId"], g["title"], g["platform"], g["format"]),
                         ("CUSA03349", "Deep Blue Voyage", "ps4", "PKG"))
        g = self.row_at(snap, lanterns)
        self.assertEqual((g["titleId"], g["title"], g["platform"], g["format"], g["managed"]),
                         ("PPSA90006", "Paper Lanterns", "ps5", "FFPFS", True))
        # the catalogue's title ID came from the release URL path; the file name has none
        g = self.row_at(snap, signal)
        self.assertIsNotNone(g)
        self.assertEqual((g["titleId"], g["title"], g["platform"], g["format"]), ("PLJM12345", "Signal Fire", "ps5", "exFAT"))
        self.assertIsNone(self.row_at(snap, os.path.join(self.r1, "xeno-store/pkg/Moss Lantern Collection_1.pkg")))

    # ---- 4: /library/storage ---------------------------------------------------
    def test_04_library_storage(self):
        r = self.c.poll(LIB_STORAGE, until=lambda r: r.status == 200 and r.json["updatedAt"] > 0 and
                        not r.json["stale"], timeout=10)
        st = r.check()
        self.assertEqual(st["error"], "")
        self.assertIsInstance(st["busy"], bool)
        roots = self.w.all_roots
        self.assertEqual(sorted(d["path"] for d in st["drives"]), sorted(roots))
        labels = {self.w.default_root: "Internal storage", self.r1: "R1", self.r2: "R2"}
        for d in st["drives"]:
            self.assertEqual(d["id"], drive_id(d["path"]))
            self.assertEqual((d["mountPoint"], d["label"], d["readOnly"], d["filesystem"]),
                             (d["path"], labels[d["path"]], False, ""))
            self.assertEqual(d["usedBytes"], max(0, d["totalBytes"] - d["freeBytes"]))
            self.assertGreater(d["totalBytes"], 0)
        self.assertEqual(sorted(d["path"] for d in st["destinations"]), sorted(p + "/homebrew" for p in roots))
        for d in st["destinations"]:
            self.assertEqual(d["id"], dest_id(d["mountPoint"]))
            self.assertEqual(d["path"], d["mountPoint"] + "/homebrew")
            self.assertEqual((d["readOnly"], d["label"]), (False, labels[d["mountPoint"]]))
        # POST: the same shape and ids (it only asks the tv-storage thread for a rescan)
        st2 = self.storage("POST")
        self.assertEqual([d["id"] for d in st2["drives"]], [d["id"] for d in st["drives"]])
        self.assertEqual([d["id"] for d in st2["destinations"]], [d["id"] for d in st["destinations"]])
        golden = os.path.join(tvworld.GOLDEN_DIR, "library-storage.json")
        if tvworld.golden_update_mode() or os.path.exists(golden):
            self.w.assert_golden("library-storage", st)

    # ---- 5: the validator --------------------------------------------------------
    def test_05_action_validation(self):
        snap = self.wait_library(lambda s: s["status"] == "ready", what="ready")
        g = self.row_at(snap, os.path.join(self.r1, "homebrew/PPSA01234.exfat"))
        tid, key, dst = g["titleId"], g["sourceKey"], dest_id(self.r2)
        ok_rid = "tv-19a3f2c1b40-ff"
        bad = [
            {},
            {"action": "scan", "confirmed": True},                                         # no requestId
            {"action": "erase", "confirmed": True, "requestId": ok_rid},                   # unknown action
            {"action": ["scan"], "confirmed": True, "requestId": ok_rid},
            {"action": "scan", "confirmed": True, "requestId": "tv-1-01"},                 # 7 bytes
            {"action": "scan", "confirmed": True, "requestId": "r" * 65},                  # 65 bytes
            {"action": "scan", "confirmed": True, "requestId": "tv-1-\u0001-0001"},        # control byte
            {"action": "scan", "confirmed": True, "requestId": 12345678},
            {"action": "copy", "titleId": tid.lower(), "sourceKey": key, "destinationId": dst, "confirmed": True,
             "requestId": ok_rid},
            {"action": "copy", "titleId": 1234, "sourceKey": key, "destinationId": dst, "requestId": ok_rid},
            {"action": "copy", "titleId": tid, "sourceKey": key[:63], "destinationId": dst, "requestId": ok_rid},
            {"action": "copy", "titleId": tid, "sourceKey": key + "0", "destinationId": dst, "requestId": ok_rid},
            {"action": "copy", "titleId": tid, "sourceKey": key, "requestId": ok_rid},       # no destination
            {"action": "move", "titleId": tid, "sourceKey": key, "destinationId": dst[:63], "confirmed": True,
             "requestId": ok_rid},
            {"action": "mount", "titleId": tid, "requestId": ok_rid},                       # no sourceKey
            {"action": "cancel", "requestId": ok_rid},                                      # no jobId
            {"action": "cancel", "jobId": 0, "requestId": ok_rid},
            {"action": "cancel", "jobId": -3, "requestId": ok_rid},
            {"action": "cancel", "jobId": "7", "requestId": ok_rid},
            {"action": "cancel", "jobId": 1.5, "requestId": ok_rid},
            {"action": "cancel", "jobId": True, "requestId": ok_rid},
            {"action": "cancel", "jobId": 2 ** 53, "requestId": ok_rid},
        ]
        for body in bad:
            self.c.post(ACTIONS, body).error(400, S_BAD)
        # scan, move and unmount need "confirmed": true (a JSON true; copy does not)
        for body in ({"action": "scan", "requestId": ok_rid},
                     {"action": "scan", "confirmed": "true", "requestId": ok_rid},
                     {"action": "scan", "confirmed": 1, "requestId": ok_rid},
                     {"action": "scan", "confirmed": False, "requestId": ok_rid},
                     {"action": "move", "titleId": tid, "sourceKey": key, "destinationId": dst, "requestId": ok_rid},
                     {"action": "unmount", "titleId": tid, "sourceKey": key, "requestId": ok_rid}):
            self.c.post(ACTIONS, body).error(400, S_CONFIRM)
        # mount, unmount and measure: no capability in option B (D3)
        for body in (tc.AppBody.library_action("mount", ok_rid, title_id=tid, source_key=key),
                     tc.AppBody.library_action("unmount", ok_rid, title_id=tid, source_key=key, confirmed=True),
                     tc.AppBody.library_action("measure", ok_rid)):
            self.c.post(ACTIONS, body).error(409, S_INCOMPATIBLE)
        # nothing was accepted
        self.assertIsNone(self.library()["action"])

    # ---- 6: scan ----------------------------------------------------------------
    def test_06_scan(self):
        rid = self.rid()
        body = tc.AppBody.library_action("scan", rid, confirmed=True)
        v = self.act(body)
        self.assertEqual(v["action"], {"id": rid, "action": "scan", "titleId": "", "state": "queued", "message": S_WAITING})
        snap = self.wait_library(lambda s: s["action"]["state"] in ("complete", "error"), timeout=30, what="scan")
        self.assertEqual(snap["action"]["state"], "complete", snap["action"])
        self.assertIn(snap["action"]["message"], (S_UPDATED, S_SCAN_REQUESTED))
        self.assertEqual(snap["status"], "ready")

    # ---- 7: copy, replay, completion ------------------------------------------------
    def test_07_copy_replay_and_complete(self):
        snap = self.wait_library(lambda s: s["status"] == "ready" and
                                 (s["action"] is None or s["action"]["state"] not in ("queued", "running")))
        src = os.path.join(self.r1, "homebrew/PPSA01234.exfat")
        g = self.row_at(snap, src)
        d = self.dest(self.r2)
        rid = self.rid()
        body = tc.AppBody.library_action("copy", rid, title_id=g["titleId"], source_key=g["sourceKey"],
                                         destination_id=d["id"], confirmed=True)
        v = self.act(body)
        self.assertEqual(v["action"], {"id": rid, "action": "copy", "titleId": "PPSA01234", "state": "queued",
                                       "message": S_WAITING})
        self.assertEqual(v["status"], "ready")
        self.assertTrue(v["games"])               # the full snapshot (any 2xx replaces the app's state)
        # the exact same request again: 202, nothing runs twice
        self.act(body)
        # the same requestId with another body: 409
        other = tc.AppBody.library_action("copy", rid, title_id=g["titleId"], source_key=g["sourceKey"],
                                          destination_id=dest_id(self.w.default_root), confirmed=True)
        self.c.post(ACTIONS, other).error(409, S_REUSED)
        snap = self.wait_library(lambda s: s["action"]["state"] in ("complete", "error") and s["storageJob"] and
                                 s["storageJob"]["state"] == "completed", timeout=60, what="copy to complete")
        self.assertEqual((snap["action"]["state"], snap["action"]["message"]), ("complete", S_STARTED))
        job = snap["storageJob"]
        self.assertIsInstance(job["id"], int)
        self.assertGreaterEqual(job["id"], 1)
        dst = os.path.join(self.r2, "homebrew/PPSA01234.exfat")
        self.assertEqual((job["operation"], job["titleId"], job["source"], job["destination"]),
                         ("copy", "PPSA01234", src, dst))
        self.assertEqual((job["totalBytes"], job["processedBytes"]), (8 * MiB, 8 * MiB))
        self.assertEqual((job["active"], job["cancellable"], job["cancelRequested"], job["error"]), (False, False, False, ""))
        self.assertIs(snap["storageBusy"], False)
        self.assertTrue(os.path.exists(dst))
        self.assertTrue(os.path.exists(src))
        self.assertEqual(len(self.copy_jobs()), 1, "the replay must not start a second copy")
        # the next check lists both copies
        snap = self.wait_library(lambda s: len(self.rows(s, "PPSA01234")) == 2, timeout=30, what="both copies")
        rows = self.rows(snap, "PPSA01234")
        self.assertEqual(sorted(r["location"] for r in rows), ["R1", "R2"])
        self.assertEqual(len({r["sourceKey"] for r in rows}), 2)
        type(self).first_alias = job["id"]

    # ---- 8: refusals of the worker ------------------------------------------------------
    def test_08_copy_refusals(self):
        snap = self.wait_library(lambda s: len(self.rows(s, "PPSA01234")) == 2)
        g = self.row_at(snap, os.path.join(self.r1, "homebrew/PPSA01234.exfat"))
        pkg = self.row_at(snap, os.path.join(self.r1, "xeno-store/pkg/CUSA00001.pkg"))
        inst = self.row_at(snap, os.path.join(self.w.data, "user/app/PPSA05555"))
        cases = [
            ("copy", g, dest_id(self.r2), S_EXISTS),                      # already on R2: never overwritten
            ("copy", g, dest_id(self.r1), S_DEST_INSIDE),                 # same drive
            ("copy", g, "0" * 64, S_DEST_CHANGED),                        # no such destination
            ("copy", dict(g, sourceKey="f" * 64), dest_id(self.r2), S_SOURCE_CHANGED),
            ("copy", pkg, dest_id(self.r2), S_SOURCE_UNSUPPORTED),        # PKG: managed false (D5)
            ("move", inst, dest_id(self.r2), S_SOURCE_UNSUPPORTED),       # installed-only (D11)
        ]
        before = len(self.copy_jobs())
        for action, row, dst, sentence in cases:
            rid = self.rid()
            body = tc.AppBody.library_action(action, rid, title_id=row["titleId"], source_key=row["sourceKey"],
                                             destination_id=dst, confirmed=True)
            final = self.run_action(body, rid)
            self.assertEqual((final["action"]["state"], final["action"]["message"]), ("error", sentence), (action, row["path"]))
            self.assertEqual(final["action"]["titleId"], row["titleId"])
        self.assertEqual(len(self.copy_jobs()), before)

    # ---- 9: D7, adoption of a web-UI copy, cancel of a queued copy -------------------------
    def test_09_downloads_interlock_adoption_and_cancel(self):
        st, b = self.w.srv.api("POST", "/api/queue", {"url": self.w.file_url("big.bin", "slow"), "connections": 1,
                                                      "filename": "Slow Download.bin", "root": self.w.default_root})
        self.assertEqual(st, 200, b)
        dl = b["job"]["id"]
        try:
            self.w.srv.wait_job(dl, {"downloading"}, timeout=30)
            snap = self.wait_library(lambda s: s["status"] == "ready")
            src = os.path.join(self.r2, "homebrew/PPSA90013.ffpfsc")
            g = self.row_at(snap, src)
            before = len(self.copy_jobs())
            rid = self.rid()
            body = tc.AppBody.library_action("move", rid, title_id=g["titleId"], source_key=g["sourceKey"],
                                             destination_id=dest_id(self.r1), confirmed=True)
            final = self.run_action(body, rid)
            self.assertEqual((final["action"]["state"], final["action"]["message"]), ("error", S_PAUSE_DOWNLOADS))
            self.assertEqual(len(self.copy_jobs()), before)
            self.assertTrue(os.path.exists(src))
            # a copy started from the web UI waits behind the download (one slot) and is adopted
            st, b = self.w.srv.api("POST", "/api/library/transfer", {"path": src, "toRoot": self.w.default_root})
            self.assertEqual(st, 200, b)
            xs_copy = b["job"]["id"]
            snap = self.wait_library(lambda s: s["storageJob"] and s["storageJob"]["source"] == src, timeout=20,
                                     what="adoption of the web-UI copy")
            job = snap["storageJob"]
            self.assertEqual((job["operation"], job["state"], job["active"], job["cancellable"], job["titleId"]),
                             ("copy", "queued", True, True, "PPSA90013"))
            self.assertGreater(job["id"], getattr(type(self), "first_alias", 0))
            self.assertIs(snap["storageBusy"], True)
            # working(): no other action while it is active
            rid = self.rid()
            busy = tc.AppBody.library_action("copy", rid, title_id=g["titleId"], source_key=g["sourceKey"],
                                             destination_id=dest_id(self.r1), confirmed=True)
            self.c.post(ACTIONS, busy).error(409, S_IN_PROGRESS)
            # D7 the other way round (map-downloads §4.1 step 7): no new download while a copy waits
            r = self.c.post("/api/v1/downloads", tc.AppBody.create_download("fx-harbor-lights-exfat", "internal"))
            if not (r.status == 503 and r.json.get("error") == S_NOT_READY):
                r.error(409, S_LIBRARY_COPY)
            # cancel it from the TV
            rid = self.rid()
            final = self.run_action(tc.AppBody.library_action("cancel", rid, job_id=job["id"]), rid)
            self.assertEqual((final["action"]["state"], final["action"]["message"]), ("complete", S_CANCEL_REQUESTED))
            snap = self.wait_library(lambda s: s["storageJob"]["state"] == "cancelled", timeout=15, what="cancelled")
            job2 = snap["storageJob"]
            self.assertEqual((job2["id"], job2["active"], job2["cancellable"], job2["cancelRequested"]),
                             (job["id"], False, False, False))
            self.assertEqual(self.w.srv.job(xs_copy)["state"], "cancelled")
            self.assertTrue(os.path.exists(src))
        finally:
            self.w.srv.action(dl, "cancel", {"deleteFiles": True})
            self.w.srv.wait_job(dl, {"cancelled"}, timeout=30)

    # ---- 10: move ------------------------------------------------------------------------
    def test_10_move(self):
        src = os.path.join(self.r2, "homebrew/PPSA90013.ffpfsc")
        dst = os.path.join(self.r1, "homebrew/PPSA90013.ffpfsc")
        snap = self.wait_library(lambda s: self.row_at(s, src) is not None and
                                 (not s["storageJob"] or not s["storageJob"]["active"]))
        g = self.row_at(snap, src)
        rid = self.rid()
        body = tc.AppBody.library_action("move", rid, title_id=g["titleId"], source_key=g["sourceKey"],
                                         destination_id=dest_id(self.r1), confirmed=True)
        final = self.run_action(body, rid)
        self.assertEqual((final["action"]["state"], final["action"]["message"]), ("complete", S_STARTED), final["action"])
        snap = self.wait_library(lambda s: s["storageJob"]["source"] == src and s["storageJob"]["state"] == "completed",
                                 timeout=60, what="move")
        self.assertEqual((snap["storageJob"]["operation"], snap["storageJob"]["destination"]), ("move", dst))
        self.assertTrue(os.path.exists(dst))
        self.assertFalse(os.path.exists(src))
        snap = self.wait_library(lambda s: self.row_at(s, dst) is not None and self.row_at(s, src) is None,
                                 timeout=30, what="the moved row")
        self.assertEqual(self.row_at(snap, dst)["location"], "R1")

    # ---- 11: cancel a running copy --------------------------------------------------------------
    def test_11_cancel_running_copy(self):
        src = os.path.join(self.r1, "homebrew/PPSA06001 Big Copy.exfat")
        sparse(src, 1024 * MiB)
        try:
            snap = self.wait_library(lambda s: self.row_at(s, src) is not None, timeout=40, refresh=True,
                                     what="the big file")
            g = self.row_at(snap, src)
            self.assertEqual((g["titleId"], g["title"], g["managed"]), ("PPSA06001", "Big Copy", True))
            rid = self.rid()
            body = tc.AppBody.library_action("copy", rid, title_id=g["titleId"], source_key=g["sourceKey"],
                                             destination_id=dest_id(self.r2), confirmed=True)
            self.act(body)
            seen_requested = []
            r = self.c.poll(LIB, until=lambda r: r.json["storageJob"] is not None and
                            r.json["storageJob"]["source"] == src and r.json["storageJob"]["state"] == "running" and
                            r.json["storageJob"]["processedBytes"] > 0, timeout=30, every=0.05)
            job = r.check()["storageJob"]
            self.assertLess(job["processedBytes"], job["totalBytes"], "the copy finished before the test could cancel it")
            self.assertEqual((job["active"], job["cancellable"]), (True, True))
            # (working() refusing a second action is checked on a queued copy in test_09:
            # here the cancel goes out at once, while the copy still runs)
            rid = self.rid()
            self.act(tc.AppBody.library_action("cancel", rid, job_id=job["id"]))
            deadline = time.monotonic() + 30
            while True:
                s = self.library()
                if s["storageJob"]["cancelRequested"]:
                    seen_requested.append(s["storageJob"]["state"])
                if s["action"]["id"] == rid and s["action"]["state"] in ("complete", "error") and \
                        s["storageJob"]["state"] == "cancelled":
                    break
                self.assertLess(time.monotonic(), deadline, f"cancel did not finish: {json.dumps(s)[:800]}")
                time.sleep(0.05)
            self.assertEqual((s["action"]["state"], s["action"]["message"]), ("complete", S_CANCEL_REQUESTED))
            self.assertEqual((s["storageJob"]["active"], s["storageJob"]["cancelRequested"]), (False, False))
            for st in seen_requested:
                self.assertEqual(st, "running")       # "Cancelling…" only while it winds down
            leftovers = [n for n in os.listdir(os.path.join(self.r2, "homebrew")) if n.startswith("PPSA06001")]
            self.assertEqual(leftovers, [], "the partial copy must be removed")
            # the alias is no longer cancellable
            rid = self.rid()
            final = self.run_action(tc.AppBody.library_action("cancel", rid, job_id=job["id"]), rid)
            self.assertEqual((final["action"]["state"], final["action"]["message"]), ("error", S_JOB_FINISHED))
        finally:
            if os.path.exists(src):
                os.unlink(src)

    # ---- 12: a drive that goes away ----------------------------------------------------------
    def test_12_rows_carried_while_a_drive_is_away(self):
        on_r2 = os.path.join(self.r2, "homebrew/PPSA01234.exfat")
        snap = self.wait_library(lambda s: self.row_at(s, on_r2) is not None, timeout=30, refresh=True)
        before = self.row_at(snap, on_r2)
        away = self.r2 + ".away"
        os.rename(self.r2, away)
        try:
            snap = self.wait_library(lambda s: self.row_at(s, on_r2) is not None and not self.row_at(s, on_r2)["onDrive"],
                                     timeout=40, what="the R2 row carried with onDrive false")
            g = self.row_at(snap, on_r2)
            self.assertEqual((g["sourceKey"], g["location"], g["titleId"], g["installed"]),
                             (before["sourceKey"], "R2", "PPSA01234", False))
            self.assertIs(snap["stale"], False)
            st = self.c.poll(LIB_STORAGE, until=lambda r: self.r2 not in [d["path"] for d in r.json["drives"]],
                             timeout=15).check()
            self.assertNotIn(self.r2 + "/homebrew", [d["path"] for d in st["destinations"]])
        finally:
            os.rename(away, self.r2)
        snap = self.wait_library(lambda s: self.row_at(s, on_r2) is not None and self.row_at(s, on_r2)["onDrive"],
                                 timeout=40, what="the R2 row back on its drive")
        self.assertEqual(len([g for g in snap["games"] if g["path"] == on_r2]), 1)

    # ---- 13: D15 step 1 -----------------------------------------------------------------------
    def test_13_title_id_from_the_download_job(self):
        """Two catalogue releases share one file name: the finished job's release decides."""
        if not self.tvcat_live():
            self.skipTest("the TV catalogue index (tvapi_catalog.c) is not implemented in this build")
        st, b = self.w.srv.api("POST", "/api/queue", {"gameId": "fx-lib-twin-b-ps5", "releaseId": "fx-lib-twin-b-ffpfsc",
                                                      "root": self.r1})
        self.assertEqual(st, 200, b)
        self.w.srv.wait_job(b["job"]["id"], {"done"}, timeout=60)
        path = os.path.join(self.r1, "homebrew/Twin Edition.ffpfsc")
        snap = self.wait_library(lambda s: self.row_at(s, path) is not None, timeout=40, refresh=True,
                                 what="the downloaded twin")
        g = self.row_at(snap, path)
        self.assertEqual((g["titleId"], g["title"], g["platform"]), ("PPSA07102", "Twin Edition Beta", "ps5"))


class TvLibraryQuiet(unittest.TestCase, LibraryMixin):
    """map-library §12 item 10 and PORT-SPEC 1.9: no walk while nothing changes."""
    FILES = 300

    @classmethod
    def setUpClass(cls):
        cls.w = tvworld.World("library-quiet", library=True).start()
        r1 = cls.w.roots[0]
        for i in range(cls.FILES):
            sparse(os.path.join(r1, "homebrew", "PPSA1%04d Quiet Game %d.exfat" % (i, i)), 1 * MiB)
        cls.c = cls.w.client()
        # every folder mtime is now older than the 2 s same-second guard of the signature
        time.sleep(3)

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    def walks(self):
        return len(re.findall(r"tvlib: walked the drives", self.w.log_text()))

    def quiet_checks(self):
        return len(re.findall(r"tvlib: nothing changed, no walk", self.w.log_text()))

    def timed(self, path):
        """One request; an outlier is measured again (CPU contention, not our code)."""
        best = None
        for _ in range(3):
            r = self.c.get(path)
            r.check()
            best = r if best is None or r.elapsed < best.elapsed else best
            if r.elapsed < 0.1:
                break
        return best

    def test_01_no_walk_when_nothing_changed(self):
        self.assertEqual(self.library()["status"], "idle")
        snap = self.wait_library(lambda s: s["status"] == "ready" and not s["busy"], timeout=20, what="ready")
        self.assertGreaterEqual(len(snap["games"]), self.FILES)
        self.assertEqual(self.walks(), 1)
        quiet0 = self.quiet_checks()
        times = []
        # The app polls GET /library every ~3 s; a GET re-checks every 30 s.
        end = time.monotonic() + 72
        while time.monotonic() < end:
            r = self.timed(LIB)
            v = r.json
            self.assertEqual(v["status"], "ready")
            self.assertIs(v["stale"], False)
            times.append(r.elapsed)
            times.append(self.timed(LIB_STORAGE).elapsed)
            time.sleep(3)
        self.assertEqual(self.walks(), 1, "nothing changed: the drives must not be walked again")
        self.assertGreaterEqual(self.quiet_checks() - quiet0, 2, "a GET re-checks (without walking) every 30 s")
        self.assertLess(max(times), 0.1, f"PORT-SPEC 1.9: GET /library and /library/storage < 100 ms; {sorted(times)[-5:]}")
        # a POST refresh is a forced check (at most every 5 s): it walks
        def forced_walk():
            self.library("POST")
            return self.walks() == 2
        harness_wait(forced_walk, 20, "the forced walk")
        snap2 = self.wait_library(lambda s: not s["busy"])
        self.assertEqual(snap2["games"], snap["games"])

    def test_02_changed_folder_walks_again(self):
        r1 = self.w.roots[0]
        before = self.walks()
        path = os.path.join(r1, "homebrew", "PPSA18888 Late Arrival.exfat")
        sparse(path, 1 * MiB)
        snap = self.wait_library(lambda s: self.row_at(s, path) is not None, timeout=40, refresh=True,
                                 what="the new file")
        self.assertGreater(self.walks(), before)
        self.assertEqual(self.row_at(snap, path)["title"], "Late Arrival")


def harness_wait(cond, timeout, what):
    deadline = time.monotonic() + timeout
    while not cond():
        if time.monotonic() >= deadline:
            raise AssertionError(f"timeout waiting for {what}")
        time.sleep(0.1)


if __name__ == "__main__":
    unittest.main()
