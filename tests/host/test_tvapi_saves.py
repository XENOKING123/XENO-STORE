#!/usr/bin/env python3
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
test_tvapi_saves.py — the TV app's Saves tab against the Save Data Store
(/api/saves, src/ext_console.c + src/cx_savestore.c), called the way
app/tv/src/orbit/api.cpp calls it: tvworld.TvClient (one request per
connection, Content-Type: application/json) with the token of
GET /api/v1/session and nothing else.

  TvSavesSessionTests  a console with a pairing PIN: the session token alone
                       lists the saves and the profiles, starts an install
                       ({"user", "backupFirst": true, "wait": false}), reads
                       its job and cancels it, through the gate of src/api.c;
                       without the token (or with another one) the writes are
                       refused with 401 pin_required and no job starts. A save
                       id with a colon travels percent-encoded, as the app
                       sends "sb:<uuid>", and is installed for the signed-in
                       profile once its own save was backed up.

The GitHub index is local (tvworld.World saves=); no site, nothing reaches the
network.
"""
import os
import sys
import unittest
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402
import tvworld  # noqa: E402

HAVE_BINARY = os.path.isfile(harness.BINARY) and os.access(harness.BINARY, os.X_OK)
PIN = "2468"
PLAYER = 0x179A0CD8                      # the host build's simulated profile
TITLE = "PPSA90003"                      # Ember Hollow in the fixture catalogue
COLON_ID = "xk:ember-forge"              # sent as xk%3Aember-forge
SLOW_ID = "xk-starfall-slow"
INSTALL = {"user": 0, "backupFirst": True, "wait": False}     # Client::install_save's body


def encoded(save_id):
    """api::save_path_segment: unreserved characters kept, the rest %XX."""
    return "".join(c if c.isalnum() or c in "-_.~" else "%%%02X" % ord(c) for c in save_id)


@unittest.skipUnless(HAVE_BINARY, f"host binary {harness.BINARY} is missing (make host)")
class TvSavesSessionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.marker = b"COMMUNITY-SAVE-"
        files = tvworld.shared_files()
        cls.w = tvworld.World("saves", library=False, settings={"pairing": {"pin": PIN}}, saves=[
            {"id": COLON_ID, "titleId": TITLE, "title": "Ember Hollow", "name": "Chapter 4", "author": "ashwright",
             "category": "chapter", "gameVersion": "01.050.000", "dirName": "SAVEDATA00",
             "zip": tvworld.save_zip(TITLE, "Ember Hollow", cls.marker * 512)},
            {"id": SLOW_ID, "titleId": "PPSA90005", "title": "Starfall Rally", "name": "Season two",
             "author": "gridline", "url": files.base + "/slow/big.bin", "size": 64 * tvworld.MiB},
        ]).start()
        try:
            cls.c = cls.w.client()
            # The profile's own save, which an install backs up first.
            cls.title_dir = os.path.join(cls.w.data, "sim", "user", "home", "%x" % PLAYER, "savedata_prospero", TITLE)
            os.makedirs(cls.title_dir, exist_ok=True)
            with open(os.path.join(cls.title_dir, "sdimg_SAVEDATA00"), "wb") as f:
                f.write(b"MY-OWN-SAVE" * 400)
        except BaseException:
            cls.w.close()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    def job(self, job_id, until, timeout=60.0):
        r = self.c.poll(f"/api/saves/jobs/{job_id}", until=lambda r: r.status == 200 and until(r.json["job"]),
                        timeout=timeout, every=0.2)
        return r.json["job"]

    def jobs(self):
        r = self.c.get("/api/saves/jobs")
        self.assertEqual(r.status, 200, r)
        return r.json.get("jobs", [])

    def test_1_reads_with_the_session_token(self):
        r = self.c.get("/api/saves/store")
        self.assertEqual(r.status, 200, r)
        v = r.json
        self.assertIs(v["building"], False)
        self.assertEqual(sorted(s["id"] for s in v["saves"]), sorted([COLON_ID, SLOW_ID]))
        self.assertEqual(v["backend"], "github", "the published saves-backend.json is a local 404: no site")
        self.assertEqual(v["backendName"], "")
        row = next(s for s in v["saves"] if s["id"] == COLON_ID)
        self.assertEqual((row["gameId"], row["author"], row["compat"]), ("fx-ember-hollow-ps5", "ashwright", "not_installed"))
        r = self.c.get("/api/saves/users")
        self.assertEqual(r.status, 200, r)
        users = r.json["users"]
        self.assertEqual([(u["id"], u["foreground"]) for u in users], [(PLAYER, True)])

    def test_2_writes_need_the_session_token(self):
        before = len(self.jobs())
        for token in ("", "0" * 64):
            r = self.c.post(f"/api/saves/store/{SLOW_ID}/install", INSTALL, token=token)
            self.assertEqual(r.status, 401, r)
            self.assertEqual(r.json["error"], "pin_required", r)
            r = self.c.post("/api/saves/jobs/s_nope/cancel", {}, token=token)
            self.assertEqual(r.status, 401, r)
        self.assertEqual(len(self.jobs()), before, "a refused install must not start a job")
        # The same writes with the session token reach the handlers.
        r = self.c.post("/api/saves/jobs/s_nope/cancel", {})
        self.assertEqual((r.status, r.json["error"], r.json["message"]), (404, "not_found", "No such running job"))
        r = self.c.post(f"/api/saves/store/{encoded('sb:5a1d0000-0000-4000-8000-0000000000ff')}/install", INSTALL)
        self.assertEqual((r.status, r.json["error"], r.json["message"]), (404, "not_found", "unknown save id"),
                         "a percent-encoded id reaches the store as the id, not as an unknown route")

    def test_3_install_followed_and_cancelled(self):
        r = self.c.post(f"/api/saves/store/{SLOW_ID}/install", INSTALL)
        self.assertEqual(r.status, 200, r)
        job = r.json["job"]
        self.assertEqual(job["type"], "save-install")
        self.assertIn(job["state"], ("queued", "running"))
        job = self.job(job["id"], lambda j: j["state"] == "running" and j["done"] > 0, timeout=30)
        self.assertEqual(job["total"], 64 * tvworld.MiB)
        r = self.c.post(f"/api/saves/jobs/{job['id']}/cancel", {})
        self.assertEqual(r.status, 200, r)
        job = self.job(job["id"], lambda j: j["state"] in ("done", "failed", "cancelled"), timeout=30)
        self.assertEqual((job["state"], job["errorCode"]), ("cancelled", "cancelled"), job)

    def test_4_install_backs_up_first_then_replaces_the_save(self):
        r = self.c.post(f"/api/saves/store/{encoded(COLON_ID)}/install", INSTALL)
        self.assertEqual(r.status, 200, r)
        job = self.job(r.json["job"]["id"], lambda j: j["state"] in ("done", "failed", "cancelled"))
        self.assertEqual(job["state"], "done", job)
        result = job["result"]
        self.assertEqual((result["titleId"], result["tier"]), (TITLE, "native"))
        self.assertTrue(result["preBackup"] and os.path.isfile(result["preBackup"]),
                        f"backupFirst: the profile's own save is zipped first ({result})")
        with zipfile.ZipFile(result["preBackup"]) as z:
            self.assertIn("sdimg_SAVEDATA00", z.namelist())
        installed = []
        for root, _, names in os.walk(self.title_dir):
            for n in names:
                with open(os.path.join(root, n), "rb") as f:
                    if f.read().startswith(self.marker):
                        installed.append(n)
        self.assertEqual(installed, ["savedata.bin"], "the community save's data is in the profile's save")


if __name__ == "__main__":
    unittest.main()
