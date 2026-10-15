#!/usr/bin/env python3
"""
test_saves_garlic.py — the save engine's Garlic Save Manager tier on the host
build, against garlic_mock.GarlicMock (a stand-in that follows
garlic-savemgr src/main.c route by route, single recv() per request included):

  * community install through Garlic: files uploaded with the right names and
    sizes (never xeno-save.json / sealedkey / keystone), uid sent as hex,
    import_check then import_finish, the save lands for the right profile;
  * a single wrapper folder in the zip is stripped; a zip without
    sce_sys/param.sfo at the save root is refused before Garlic is touched;
    a community zip holding a raw (sealed) image is refused before anything
    changes;
  * a zeroed param.sfo: the pinned 1.7 refuses it (param.sfo is corrupted!)
    and the job fails after an unmount; a newer Garlic answers sfo_zeroed and
    the import is repeated with force=1&title_id=&dir_name= — never with a
    guessed folder name;
  * a Garlic error unmounts its image and fails the job; a Garlic older than
    1.6 (no import API) is reported instead of used;
  * decrypted export through Garlic's mount route, which reads exactly the
    profile's own image (ACCOUNT_ID cleared), even when several profiles hold
    the same save or export_title_dec would skip the profile's copy;
  * restore of a Garlic-layout decrypted zip ("<TID>/sdimg_<dir>/…"); a raw
    backup whose top level has no save image is refused;
  * profile-to-profile transfer re-signed through Garlic (export for the
    source profile, import for the destination; a move removes the source),
    and refused while the title runs;
  * Garlic down: status/setup report it, POST /api/saves/garlic/start sends
    the pinned companion release (downloaded, sha256 checked) to a fake ELF
    loader and waits for the port, and falls back to an ELF on the console
    only when the release cannot be downloaded; never a second copy while one
    answers (start or payload Launch); a foreign program on the port is not
    mistaken for Garlic; a running title blocks its save install, transfer
    and export.

The native tier simulation is off (XENO_SIM_NATIVE=0) and every port is a test
port (XENO_PORT_GARLIC / XENO_PORT_LOADER), so nothing touches /data or the
network.
"""
import hashlib
import http.server
import io
import json
import os
import shutil
import struct
import sys
import tempfile
import threading
import time
import unittest
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402
from garlic_mock import FakeLoader, GarlicMock, parse_sfo  # noqa: E402

PLAYER = 0x179A0CD8
GUEST = 0x2B3C4D5E
TITLE = "PPSA01234"
OTHER = "PPSA09999"
XFER = "PPSA05555"
COMMUNITY_AID = 0x1122334455667788
GUEST_AID = 0x0A0B0C0D0E0F1011


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def build_sfo(entries):
    """PSF v1.1 writer: entries = [(key, fmt, data, max_len)]."""
    keys = b""
    key_offsets = []
    for k, _, _, _ in entries:
        key_offsets.append(len(keys))
        keys += k.encode() + b"\0"
    data = b""
    data_offsets = []
    for _, _, d, mx in entries:
        data_offsets.append(len(data))
        data += d.ljust(mx, b"\0")
    n = len(entries)
    key_start = 20 + 16 * n
    data_start = key_start + len(keys)
    pad = (-data_start) % 4
    data_start += pad
    hdr = b"\0PSF" + struct.pack("<IIII", 0x0101, key_start, data_start, n)
    idx = b"".join(struct.pack("<HHIII", key_offsets[i], entries[i][1], len(entries[i][2]), entries[i][3], data_offsets[i]) for i in range(n))
    return hdr + idx + keys + b"\0" * pad + data


def make_sfo(title=TITLE, dirname="SAVEDATA00", account=COMMUNITY_AID):
    return build_sfo([
        ("ACCOUNT_ID", 0x0404, struct.pack("<Q", account), 8),
        ("MAINTITLE", 0x0204, b"Test Game\0", 128),
        ("SAVEDATA_DIRECTORY", 0x0204, dirname.encode() + b"\0", 64),
        ("TITLE_ID", 0x0204, title.encode() + b"\0", 12),
    ])


def sfo_account(buf):
    key_start, data_start, n = struct.unpack("<III", buf[8:20])
    for i in range(n):
        koff, fmt, ln, mx, doff = struct.unpack("<HHIII", buf[20 + 16 * i:36 + 16 * i])
        if buf[key_start + koff:].split(b"\0", 1)[0] == b"ACCOUNT_ID":
            return struct.unpack("<Q", buf[data_start + doff:data_start + doff + 8])[0]
    return None


def make_zip(entries):
    bio = io.BytesIO()
    with zipfile.ZipFile(bio, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in entries.items():
            z.writestr(name, data)
    return bio.getvalue()


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


GAME_DAT = b"COMMUNITY-SAVE-" + b"x" * 40000
BIN_DAT = bytes(range(256)) * 300
ICON = b"\x89PNG\r\n\x1a\n" + b"\0" * 200

PLAIN = {
    "game.dat": GAME_DAT,
    "data/sub/file.bin": BIN_DAT,
    "sce_sys/param.sfo": make_sfo(),
    "sce_sys/icon0.png": ICON,
    "sce_sys/sealedkey": b"SEALED-KEY",
    "sce_sys/keystone": b"KEYSTONE",
    "xeno-save.json": json.dumps({"titleId": TITLE, "dirName": "SAVEDATA00"}),
}
EXPECTED_UPLOADS = {"game.dat": len(GAME_DAT), "data/sub/file.bin": len(BIN_DAT), "sce_sys/param.sfo": len(PLAIN["sce_sys/param.sfo"]),
                    "sce_sys/icon0.png": len(ICON)}


class Files(http.server.BaseHTTPRequestHandler):
    routes = {}

    def log_message(self, *a):
        pass

    def _send(self, head):
        r = self.routes.get(self.path.split("?", 1)[0])
        if r is None:
            r = (404, "application/json", b'{"error":"not found"}')
        st, ct, body = r
        if isinstance(body, str):
            body = body.encode()
        self.send_response(st)
        self.send_header("Content-Type", ct)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if not head:
            self.wfile.write(body)

    def do_GET(self):
        self._send(False)

    def do_HEAD(self):
        self._send(True)


class FilesServer:
    def __init__(self):
        self.routes = {}
        handler = type("FilesH", (Files,), {"routes": self.routes})
        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.srv.daemon_threads = True
        self.port = self.srv.server_address[1]
        self.base = "http://127.0.0.1:%d" % self.port
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def add(self, path, body, ctype="application/octet-stream"):
        self.routes[path] = (200, ctype, body)

    def close(self):
        self.srv.shutdown()


class Base(unittest.TestCase):
    """One host store with the native simulation off and test ports."""
    garlic_running_at_start = True
    extra_env = {}

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="xeno-garlic-")
        cls.data = os.path.join(cls.tmp, "data")
        cls.sim = os.path.join(cls.data, "sim")
        os.makedirs(cls.sim)
        with open(os.path.join(cls.sim, "users.json"), "w") as f:
            json.dump({"users": [{"id": PLAYER, "name": "Player", "foreground": True}, {"id": GUEST, "name": "Guest", "foreground": False}]}, f)
        for uid in (PLAYER, GUEST):
            os.makedirs(cls.home(uid), exist_ok=True)
        cls.garlic_port = harness.free_port()
        cls.garlic = GarlicMock(cls.garlic_port, home_root=os.path.join(cls.sim, "user", "home"), foreground_uid="%x" % PLAYER)
        cls.garlic.account_ids["%x" % PLAYER] = 0x0102030405060708
        if cls.garlic_running_at_start:
            cls.garlic.start()
        cls.loader = FakeLoader(harness.free_port(), on_payload=cls.on_payload)
        cls.files = FilesServer()
        cls.companion_elf = b"\x7fELF" + b"GARLIC-FROM-RELEASE" + bytes(range(256)) * 32
        cls.files.add("/garlic.elf", cls.companion_elf)
        cls.files.add("/companions.json", json.dumps({"payloads": [
            {"id": "garlic-save-manager", "name": "Garlic Save Manager", "kind": "saves", "version": "1.7",
             "url": cls.files.base + "/garlic.elf", "sha256": hashlib.sha256(cls.companion_elf).hexdigest()},
            {"id": "np-fake-signout", "name": "NP fake sign-out", "kind": "np-signout", "url": "", "unavailable": True,
             "unavailableReason": "no upstream asset"},
        ]}), "application/json")
        rows = []
        for sid, title, dirname, entries in cls.zips():
            z = make_zip(entries)
            cls.files.add("/%s.zip" % sid, z, "application/zip")
            rows.append({"id": sid, "titleId": title, "title": "Test Game", "name": sid, "dirName": dirname, "url": "%s/%s.zip" % (cls.files.base, sid),
                         "sha256": hashlib.sha256(z).hexdigest(), "size": len(z), "format": "xeno-save-zip", "status": "published"})
        cls.files.add("/saves.json", json.dumps({"schemaVersion": 1, "saves": rows}), "application/json")
        env = {"XENO_SIM_NATIVE": "0", "XENO_PORT_GARLIC": str(cls.garlic_port), "XENO_PORT_LOADER": str(cls.loader.port),
               "XENO_PORT_DPI": "1", "XENO_PORT_ETAHEN": "1", "XENO_PORT_SHADOWMOUNT": "1"}
        env.update(cls.extra_env)
        cls.srv = harness.XenoServer(cls.data, env=env).start()
        st, b = cls.srv.api("PUT", "/api/settings", {
            "catalog": {"companionsUrl": cls.files.base + "/companions.json", "featuredUrl": cls.files.base + "/none.json",
                        "messagesUrl": cls.files.base + "/none.json", "savesBackendUrl": cls.files.base + "/none.json"},
            "saves": {"indexUrls": [cls.files.base + "/saves.json"], "backend": {"kind": "none"}},
        })
        assert st == 200, b
        st, b = cls.srv.api("POST", "/api/feeds/refresh", {})
        assert st == 200, b

    @classmethod
    def tearDownClass(cls):
        cls.srv.stop()
        cls.garlic.stop()
        cls.loader.close()
        cls.files.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)
        shutil.rmtree(cls.garlic.tmp, ignore_errors=True)

    @classmethod
    def on_payload(cls, data):
        pass

    @classmethod
    def zips(cls):
        return [("sv_plain", TITLE, "SAVEDATA00", PLAIN)]

    @classmethod
    def home(cls, uid):
        return os.path.join(cls.sim, "user", "home", "%x" % uid)

    def job_of(self, body):
        j = body.get("job") or {}
        jid = j.get("id")
        self.assertTrue(jid, body)
        deadline = time.time() + 60
        while j.get("state") not in ("done", "failed", "cancelled") and time.time() < deadline:
            time.sleep(0.1)
            st, b = self.srv.api("GET", "/api/saves/jobs/%s" % jid)
            j = b.get("job", j)
        return j

    def install(self, sid, uid=PLAYER):
        st, b = self.srv.api("POST", "/api/saves/store/%s/install" % sid, {"user": uid, "backupFirst": False, "wait": True})
        self.assertEqual(st, 200, b)
        return self.job_of(b)

    def mark(self):
        return len(self.garlic.log)

    def since(self, mark, prefix=""):
        return [r for r in self.garlic.log[mark:] if r["url"].startswith(prefix)]


# ---------------------------------------------------------------------------
# Garlic running: import / export
# ---------------------------------------------------------------------------

class GarlicFlowTests(Base):
    @classmethod
    def zips(cls):
        wrapped = {"My Save Folder/" + k: v for k, v in PLAIN.items()}
        wrapped["__MACOSX/My Save Folder/._game.dat"] = b"junk"
        zeroed = {"game.dat": b"ZEROED-SFO-SAVE", "sce_sys/param.sfo": b"\0" * 512,
                  "xeno-save.json": json.dumps({"titleId": TITLE, "dirName": "SAVEDATA01"})}
        nosfo = {"Wrapper/game.dat": b"no sfo here", "Wrapper/readme.txt": b"hi"}
        mismatch = {"game.dat": b"other", "sce_sys/param.sfo": make_sfo(title=OTHER, dirname="SAVEDATA00")}
        xfer = {"game.dat": b"PLAYER-XFER", "sce_sys/param.sfo": make_sfo(title=XFER, dirname="SAVEDATA00"), "sce_sys/icon0.png": ICON}
        raw = {"sdimg_SAVEDATA00": b"SEALED-IMAGE" * 64, "sce_sys/SAVEDATA00/param.sfo": make_sfo()}
        zeroed_nodn = {"game.dat": b"COMMUNITY-SLOT-X", "sce_sys/param.sfo": b"\0" * 512}
        return [("sv_plain", TITLE, "SAVEDATA00", PLAIN), ("sv_wrapped", TITLE, "SAVEDATA00", wrapped),
                ("sv_zeroed", TITLE, "SAVEDATA01", zeroed), ("sv_nosfo", TITLE, "SAVEDATA00", nosfo),
                ("sv_mismatch", TITLE, "SAVEDATA00", mismatch), ("sv_xfer", XFER, "SAVEDATA00", xfer),
                ("sv_raw", TITLE, "SAVEDATA00", raw), ("sv_zeroed_nodn", TITLE, "", zeroed_nodn)]

    def test_01_engine_and_status(self):
        st, b = self.srv.api("GET", "/api/saves/engine")
        self.assertEqual(st, 200, b)
        self.assertEqual(b["tier"], "garlic")
        self.assertFalse(b["native"]["available"])
        g = b["garlic"]
        self.assertTrue(g["running"])
        self.assertTrue(g["available"])
        self.assertEqual(g["port"], self.garlic_port)
        st, b = self.srv.api("GET", "/api/saves/garlic")
        self.assertEqual(st, 200, b)
        self.assertTrue(b["garlic"]["running"])
        status = [r for r in self.garlic.log if r["url"] == "/api/status"]
        self.assertTrue(status)
        self.assertTrue(all(r["headersInFirstRecv"] for r in status))

    def test_02_install_happy_path(self):
        m = self.mark()
        j = self.install("sv_plain")
        self.assertEqual(j["state"], "done", j)
        self.assertEqual(j["result"].get("tier"), "garlic", j)
        uploads = self.since(m, "/api/upload_file")
        got = {r["name"]: r["received"] for r in uploads}
        self.assertEqual(got, EXPECTED_UPLOADS)
        for r in uploads:
            self.assertTrue(r["headersInFirstRecv"], r)          # one send() for request line + headers
            self.assertEqual(r["contentLength"], r["received"])
        create = self.since(m, "/api/create_pfs")
        self.assertEqual(len(create), 1)
        size = int(create[0]["url"].split("size=", 1)[1])
        self.assertGreaterEqual(size, sum(EXPECTED_UPLOADS.values()))
        self.assertEqual([r["url"] for r in self.since(m, "/api/import_check?")], ["/api/import_check?uid=%x" % PLAYER])
        self.assertEqual([r["url"] for r in self.since(m, "/api/import_finish")], ["/api/import_finish?uid=%x" % PLAYER])
        # /api/status and the bare /api/import_check (the version probe) come before the import
        order = [r["url"].split("?", 1)[0] for r in self.garlic.log[m:] if r["url"] not in ("/api/status", "/api/import_check")]
        self.assertEqual(order[0], "/api/create_pfs")
        self.assertEqual(order[-2:], ["/api/import_check", "/api/import_finish"])
        key = ("%x" % PLAYER, TITLE, "sdimg_SAVEDATA00")
        self.assertIn(key, self.garlic.saves)
        files = self.garlic.saves[key]["files"]
        self.assertEqual(files["game.dat"], GAME_DAT)
        self.assertNotIn("sce_sys/sealedkey", files)
        self.assertNotIn("xeno-save.json", files)
        self.assertNotEqual(sfo_account(files["sce_sys/param.sfo"]), COMMUNITY_AID)   # re-signed before upload
        self.assertIn({"uid": "%x" % PLAYER, "aid": 0x0102030405060708}, self.garlic.resigned)
        self.assertFalse(self.garlic.mounted)
        self.assertTrue(os.path.isfile(os.path.join(self.home(PLAYER), "savedata_prospero", TITLE, "sdimg_SAVEDATA00")))

    def test_03_wrapped_folder_zip(self):
        m = self.mark()
        j = self.install("sv_wrapped", uid=GUEST)
        self.assertEqual(j["state"], "done", j)
        got = {r["name"]: r["received"] for r in self.since(m, "/api/upload_file")}
        self.assertEqual(got, EXPECTED_UPLOADS)                 # no "My Save Folder/", no __MACOSX
        self.assertEqual([r["url"] for r in self.since(m, "/api/import_finish")], ["/api/import_finish?uid=%x" % GUEST])
        self.assertIn(("%x" % GUEST, TITLE, "sdimg_SAVEDATA00"), self.garlic.saves)

    def test_04_zeroed_sfo_refused_by_pinned_release(self):
        m = self.mark()
        j = self.install("sv_zeroed")
        self.assertEqual(j["state"], "failed", j)
        self.assertIn("param.sfo is corrupted!", j["error"])
        self.assertEqual([r["url"] for r in self.since(m, "/api/import_finish")], ["/api/import_finish?uid=%x" % PLAYER])
        self.assertIn("/api/unmount", [r["url"].split("?", 1)[0] for r in self.garlic.log[m:]])
        self.assertFalse(self.garlic.mounted)
        self.assertNotIn(("%x" % PLAYER, TITLE, "sdimg_SAVEDATA01"), self.garlic.saves)

    def test_04b_zeroed_sfo_force_retry_newer_garlic(self):
        self.garlic.head = True
        try:
            m = self.mark()
            j = self.install("sv_zeroed")
        finally:
            self.garlic.head = False
        self.assertEqual(j["state"], "done", j)
        finish = [r["url"] for r in self.since(m, "/api/import_finish")]
        self.assertEqual(finish, ["/api/import_finish?uid=%x" % PLAYER,
                                  "/api/import_finish?uid=%x&force=1&title_id=%s&dir_name=SAVEDATA01" % (PLAYER, TITLE)])
        self.assertIn(("%x" % PLAYER, TITLE, "sdimg_SAVEDATA01"), self.garlic.saves)
        self.assertIn("%s/SAVEDATA01" % TITLE, j["result"].get("note", ""), j)

    def test_04c_zeroed_sfo_without_a_folder_name_is_not_forced(self):
        key = ("%x" % PLAYER, TITLE, "sdimg_SAVEDATA00")
        before = self.garlic.saves[key]["files"]["game.dat"]
        self.garlic.head = True
        try:
            m = self.mark()
            j = self.install("sv_zeroed_nodn")
        finally:
            self.garlic.head = False
        self.assertEqual(j["state"], "failed", j)
        self.assertIn("folder name is unknown", j["error"])
        self.assertEqual([r["url"] for r in self.since(m, "/api/import_finish")], ["/api/import_finish?uid=%x" % PLAYER])
        self.assertEqual(self.garlic.saves[key]["files"]["game.dat"], before)   # SAVEDATA00 untouched
        self.assertFalse(self.garlic.mounted)

    def test_05_garlic_error_unmounts_and_fails(self):
        self.garlic.fail_upload = {"data/sub/file.bin": "Cannot create data/sub/file.bin"}
        try:
            m = self.mark()
            j = self.install("sv_plain")
        finally:
            self.garlic.fail_upload = {}
        self.assertEqual(j["state"], "failed", j)
        self.assertIn("Cannot create data/sub/file.bin", j["error"])
        urls = [r["url"].split("?", 1)[0] for r in self.garlic.log[m:]]
        self.assertIn("/api/unmount", urls)
        self.assertNotIn("/api/import_finish", urls)
        self.assertGreater(urls.index("/api/unmount"), urls.index("/api/upload_file"))
        self.assertFalse(self.garlic.mounted)

    def test_06_zip_without_param_sfo_refused(self):
        m = self.mark()
        j = self.install("sv_nosfo")
        self.assertEqual(j["state"], "failed", j)
        self.assertEqual(j["errorCode"], "invalid_save", j)
        self.assertIn("sce_sys/param.sfo", j["error"])
        self.assertEqual(self.since(m, "/api/create_pfs"), [])

    def test_07_param_sfo_for_another_title_refused(self):
        m = self.mark()
        j = self.install("sv_mismatch")
        self.assertEqual(j["state"], "failed", j)
        self.assertEqual(j["errorCode"], "title_mismatch", j)
        self.assertEqual(self.since(m, "/api/create_pfs"), [])

    def test_08_decrypted_export(self):
        m = self.mark()
        st, b = self.srv.api("POST", "/api/saves/export", {"user": PLAYER, "titleId": TITLE, "dirName": "SAVEDATA00"})
        self.assertEqual(st, 200, b)
        self.assertEqual(b.get("format"), "xeno-save-zip", b)
        # the profile's own image through the mount route, never the whole-title export
        self.assertEqual(self.since(m, "/api/export_title_dec"), [])
        urls = [r["url"].split("?", 1)[0] for r in self.garlic.log[m:]]
        for route in ("/api/saves", "/api/mount", "/api/download_file", "/api/unmount"):
            self.assertIn(route, urls)
        with zipfile.ZipFile(b["zipPath"]) as z:
            names = set(z.namelist())
            self.assertIn("sce_sys/param.sfo", names)
            self.assertEqual(z.read("game.dat"), GAME_DAT)
            self.assertNotIn("sce_sys/sealedkey", names)
            self.assertFalse(any(n.startswith(TITLE + "/") for n in names), names)
            self.assertEqual(sfo_account(z.read("sce_sys/param.sfo")), 0)

    def test_09_export_picks_the_profile_when_several_hold_the_save(self):
        # GUEST got SAVEDATA00 in test_03: Garlic lists the same save twice
        self.garlic.saves[("%x" % GUEST, TITLE, "sdimg_SAVEDATA00")]["files"]["game.dat"] = b"GUEST-COPY"
        m = self.mark()
        st, b = self.srv.api("POST", "/api/saves/export", {"user": PLAYER, "titleId": TITLE, "dirName": "SAVEDATA00"})
        self.assertEqual(st, 200, b)
        urls = [r["url"].split("?", 1)[0] for r in self.garlic.log[m:]]
        for route in ("/api/saves", "/api/mount", "/api/download_file", "/api/unmount"):
            self.assertIn(route, urls)
        self.assertNotIn("/api/export_title_dec", urls)
        with zipfile.ZipFile(b["zipPath"]) as z:
            self.assertEqual(z.read("game.dat"), GAME_DAT)       # the player's copy, not the guest's
        self.assertFalse(self.garlic.mounted)

    def test_09b_unmountable_own_copy_is_not_replaced_by_another_profile(self):
        # export_title_dec would skip the player's image it cannot mount and zip only the guest's copy
        self.garlic.saves[("%x" % GUEST, TITLE, "sdimg_SAVEDATA00")]["files"]["game.dat"] = b"GUEST-COPY"
        self.garlic.export_skip = {("%x" % PLAYER, TITLE, "sdimg_SAVEDATA00")}
        try:
            st, b = self.srv.api("POST", "/api/saves/export", {"user": PLAYER, "titleId": TITLE, "dirName": "SAVEDATA00"})
        finally:
            self.garlic.export_skip = set()
        self.assertEqual(st, 200, b)
        with zipfile.ZipFile(b["zipPath"]) as z:
            self.assertNotEqual(z.read("game.dat"), b"GUEST-COPY", "exported another profile's save as the player's")
            self.assertEqual(z.read("game.dat"), GAME_DAT)

    def test_10_restore_garlic_layout_zip(self):
        root = self.srv.default_root
        zp = os.path.join(root, "xeno-store", "saves", "manual", "%s_ps5_20261015-1200.zip" % TITLE)
        entries = {"%s/sdimg_SAVEDATA02/%s" % (TITLE, k): v for k, v in PLAIN.items() if k != "xeno-save.json"}
        entries["%s/sdimg_SAVEDATA02/sce_sys/param.sfo" % TITLE] = make_sfo(dirname="SAVEDATA02")
        write(zp, make_zip(entries))
        m = self.mark()
        st, b = self.srv.api("POST", "/api/saves/restore", {"user": PLAYER, "zipPath": zp, "overwrite": True})
        self.assertEqual(st, 200, b)
        self.assertEqual(b["tier"], "garlic")
        self.assertEqual(b["style"], "decrypted")
        got = {r["name"] for r in self.since(m, "/api/upload_file")}
        self.assertEqual(got, set(EXPECTED_UPLOADS))
        self.assertIn(("%x" % PLAYER, TITLE, "sdimg_SAVEDATA02"), self.garlic.saves)

    def test_10b_raw_backup_inside_a_wrapper_folder(self):
        root = self.srv.default_root
        zp = os.path.join(root, "xeno-store", "saves", "manual", "%s_ps5_20261015-1300.zip" % TITLE)
        image = b"RAW-IMAGE" * 100
        write(zp, make_zip({"Backup/sdimg_SAVEDATA05": image, "Backup/sce_sys/SAVEDATA05/param.sfo": make_sfo(dirname="SAVEDATA05"),
                            "Backup/xeno-backup.json": json.dumps({"v": 1, "titleId": TITLE, "kind": "ps5", "user": {"id": PLAYER}})}))
        m = self.mark()
        st, b = self.srv.api("POST", "/api/saves/restore", {"user": GUEST, "zipPath": zp, "overwrite": True})
        self.assertEqual(st, 200, b)
        self.assertEqual((b["tier"], b["style"]), ("raw", "raw"))
        tdir = os.path.join(self.home(GUEST), "savedata_prospero", TITLE)
        with open(os.path.join(tdir, "sdimg_SAVEDATA05"), "rb") as f:
            self.assertEqual(f.read(), image)
        self.assertFalse(os.path.exists(os.path.join(tdir, "Backup")))
        self.assertFalse(os.path.exists(os.path.join(tdir, "xeno-backup.json")))
        self.assertEqual(self.since(m, "/api/create_pfs"), [])

    def test_10c_raw_backup_without_an_image_at_its_root_is_refused(self):
        # two wrapper folders: only the first is stripped, so the images would land one level too deep
        root = self.srv.default_root
        zp = os.path.join(root, "xeno-store", "saves", "manual", "%s_ps5_20261015-1400.zip" % TITLE)
        write(zp, make_zip({"Outer/Inner/sdimg_SAVEDATA06": b"RAW-IMAGE" * 10,
                            "Outer/Inner/xeno-backup.json": json.dumps({"v": 1, "titleId": TITLE, "kind": "ps5", "user": {"id": GUEST}})}))
        tdir = os.path.join(self.home(GUEST), "savedata_prospero", TITLE)
        before = sorted(os.listdir(tdir))
        st, b = self.srv.api("POST", "/api/saves/restore", {"user": GUEST, "zipPath": zp, "overwrite": True})
        self.assertEqual(st, 422, b)
        self.assertEqual(b.get("error"), "invalid_save", b)
        self.assertEqual(sorted(os.listdir(tdir)), before)                 # nothing was changed
        self.assertFalse([n for n in os.listdir(os.path.dirname(tdir)) if n.startswith(TITLE + ".")])   # no tmp left behind

    def test_10d_community_raw_zip_is_refused(self):
        tdir = os.path.join(self.home(PLAYER), "savedata_prospero", TITLE)
        before = sorted(os.listdir(tdir))
        m = self.mark()
        j = self.install("sv_raw")
        self.assertEqual(j["state"], "failed", j)
        self.assertEqual(j["errorCode"], "invalid_save", j)
        self.assertIn("sealed raw save image", j["error"])
        self.assertEqual(sorted(os.listdir(tdir)), before)
        self.assertEqual(self.since(m, "/api/create_pfs"), [])

    def test_11_save_open_in_garlic_is_not_touched(self):
        with self.garlic.mtx:
            self.garlic.mounted = True
            self.garlic.mount_point = "/data/save_mnt/user-session"
        try:
            m = self.mark()
            j = self.install("sv_plain")
        finally:
            with self.garlic.mtx:
                self.garlic.mounted = False
                self.garlic.mount_point = ""
        self.assertEqual(j["state"], "failed", j)
        self.assertIn("save open", j["error"])
        self.assertEqual(self.since(m, "/api/create_pfs"), [])

    def test_12_unavailable_companion(self):
        st, b = self.srv.api("GET", "/api/payloads")
        self.assertEqual(st, 200, b)
        rows = {p["id"]: p for p in b["payloads"]}
        self.assertFalse(rows["np-fake-signout"]["available"])
        self.assertTrue(rows["garlic-save-manager"]["available"])
        st, b = self.srv.api("POST", "/api/payloads/np-fake-signout/install", {})
        self.assertEqual(st, 422, b)
        self.assertEqual(b.get("error"), "unavailable", b)
        self.assertIn("no upstream asset", b.get("message", ""))

    def test_13_transfer_is_resigned_through_garlic(self):
        j = self.install("sv_xfer")
        self.assertEqual(j["state"], "done", j)
        self.garlic.account_ids["%x" % GUEST] = GUEST_AID
        m = self.mark()
        st, b = self.srv.api("POST", "/api/saves/transfer", {"fromUser": PLAYER, "toUser": GUEST, "titleId": XFER})
        self.assertEqual(st, 200, b)
        self.assertTrue(b["resigned"], b)
        self.assertEqual((b["tier"], b["saves"]), ("garlic", 1), b)
        self.assertFalse(b["moved"])
        self.assertEqual(self.since(m, "/api/export_title_dec"), [])      # only the source's own image is mounted
        urls = [r["url"].split("?", 1)[0] for r in self.garlic.log[m:]]
        for route in ("/api/mount", "/api/download_file"):
            self.assertIn(route, urls)
        self.assertEqual([r["url"] for r in self.since(m, "/api/import_finish")], ["/api/import_finish?uid=%x" % GUEST])
        got = {r["name"] for r in self.since(m, "/api/upload_file")}
        self.assertEqual(got, {"game.dat", "sce_sys/param.sfo", "sce_sys/icon0.png"})
        files = self.garlic.saves[("%x" % GUEST, XFER, "sdimg_SAVEDATA00")]["files"]
        self.assertEqual(files["game.dat"], b"PLAYER-XFER")
        self.assertIn({"uid": "%x" % GUEST, "aid": GUEST_AID}, self.garlic.resigned)   # the destination's account
        self.assertTrue(os.path.isfile(os.path.join(self.home(GUEST), "savedata_prospero", XFER, "sdimg_SAVEDATA00")))
        self.assertTrue(os.path.isfile(os.path.join(self.home(PLAYER), "savedata_prospero", XFER, "sdimg_SAVEDATA00")))
        self.assertFalse(self.garlic.mounted)
        # move back: both profiles hold the save now; the player's copy is backed up first,
        # replaced by the guest's, and the guest's folder goes
        self.garlic.saves[("%x" % GUEST, XFER, "sdimg_SAVEDATA00")]["files"]["game.dat"] = b"GUEST-XFER"
        m = self.mark()
        st, b = self.srv.api("POST", "/api/saves/transfer", {"fromUser": GUEST, "toUser": PLAYER, "titleId": XFER, "move": True})
        self.assertEqual(st, 200, b)
        self.assertTrue(b["resigned"], b)
        self.assertTrue(b["moved"])
        self.assertTrue(b["preBackup"].endswith(".pre-restore.zip"), b)
        urls = [r["url"].split("?", 1)[0] for r in self.garlic.log[m:]]
        for route in ("/api/mount", "/api/download_file", "/api/import_finish"):
            self.assertIn(route, urls)
        self.assertEqual(self.garlic.saves[("%x" % PLAYER, XFER, "sdimg_SAVEDATA00")]["files"]["game.dat"], b"GUEST-XFER")
        self.assertFalse(os.path.exists(os.path.join(self.home(GUEST), "savedata_prospero", XFER)))
        self.assertFalse(self.garlic.mounted)

    def test_13b_move_carries_the_source_profiles_own_save(self):
        # both profiles are listed for XFER/SAVEDATA00 and export_title_dec cannot mount the source's image:
        # the move must carry the source's own bytes (or fail and keep the source)
        src_key = ("%x" % PLAYER, XFER, "sdimg_SAVEDATA00")
        self.garlic.saves[src_key]["files"]["game.dat"] = b"PLAYER-OWN"
        self.garlic.saves[("%x" % GUEST, XFER, "sdimg_SAVEDATA00")] = {"files": {"game.dat": b"GUEST-STALE", "sce_sys/param.sfo": make_sfo(title=XFER)}}
        self.garlic.export_skip = {src_key}
        src = os.path.join(self.home(PLAYER), "savedata_prospero", XFER)
        try:
            m = self.mark()
            st, b = self.srv.api("POST", "/api/saves/transfer", {"fromUser": PLAYER, "toUser": GUEST, "titleId": XFER, "move": True})
        finally:
            self.garlic.export_skip = set()
        self.assertEqual(st, 200, b)
        self.assertEqual(self.since(m, "/api/export_title_dec"), [])
        self.assertEqual(self.garlic.saves[("%x" % GUEST, XFER, "sdimg_SAVEDATA00")]["files"]["game.dat"], b"PLAYER-OWN")
        self.assertTrue(b["moved"])
        self.assertFalse(os.path.exists(src))

    def test_13c_one_operation_per_save_folder(self):
        # an install holds (PLAYER, TITLE) while Garlic builds the image: a restore of the same title is refused
        root = self.srv.default_root
        zp = os.path.join(root, "xeno-store", "saves", "manual", "%s_ps5_20261015-1500.zip" % TITLE)
        write(zp, make_zip({k: v for k, v in PLAIN.items() if k != "xeno-save.json"}))
        self.garlic.delay = {"/api/create_pfs": 2.0}
        try:
            m = self.mark()
            st, b = self.srv.api("POST", "/api/saves/store/sv_plain/install", {"user": PLAYER, "backupFirst": False})
            self.assertEqual(st, 200, b)
            deadline = time.time() + 20
            while not self.since(m, "/api/create_pfs") and time.time() < deadline:
                time.sleep(0.05)
            self.assertTrue(self.since(m, "/api/create_pfs"))
            st2, b2 = self.srv.api("POST", "/api/saves/restore", {"user": PLAYER, "zipPath": zp, "overwrite": True})
            # another profile's save of the same title is not held
            st3, b3 = self.srv.api("POST", "/api/saves/restore", {"user": GUEST, "zipPath": zp, "overwrite": True})
        finally:
            self.garlic.delay = {}
        self.assertEqual(st2, 429, b2)
        self.assertEqual(b2.get("error"), "busy", b2)
        self.assertEqual(st3, 200, b3)
        self.assertEqual(self.job_of(b)["state"], "done")
        self.assertEqual(len(self.since(m, "/api/create_pfs")), 2)          # the install's and the guest's restore

    def test_14_garlic_without_the_import_api_is_not_used(self):
        self.garlic.no_import_api = True
        try:
            m = self.mark()
            j = self.install("sv_plain")
        finally:
            self.garlic.no_import_api = False
        self.assertEqual(j["state"], "failed", j)
        self.assertEqual(j["errorCode"], "garlic_unavailable", j)
        self.assertIn("older than 1.6", j["error"])
        self.assertEqual(self.since(m, "/api/create_pfs"), [])


# ---------------------------------------------------------------------------
# Garlic down: discovery, auto-start, single copy
# ---------------------------------------------------------------------------

class GarlicStartTests(Base):
    garlic_running_at_start = False
    extra_env = {"XENO_HOST_RUNNING_TITLE": OTHER}
    starts_mock = True

    @classmethod
    def setUpClass(cls):
        cls.console_elf = b"\x7fELF" + b"GARLIC-ON-THE-CONSOLE" + bytes(range(256)) * 32
        super().setUpClass()
        cls.console_path = os.path.join(cls.sim, "data", "homebrew", "Garlic SaveMgr", "Garlic-SaveMgr.ELF")
        write(cls.console_path, cls.console_elf)
        write(os.path.join(cls.sim, "data", "homebrew", "Other", "garlic-notes.txt"), b"not an elf")

    @classmethod
    def on_payload(cls, data):
        if cls.starts_mock and data[:4] == b"\x7fELF" and not cls.garlic.running():
            cls.garlic.start()

    @classmethod
    def zips(cls):
        other = {"game.dat": b"other", "sce_sys/param.sfo": make_sfo(title=OTHER)}
        return [("sv_plain", TITLE, "SAVEDATA00", PLAIN), ("sv_running", OTHER, "SAVEDATA00", other)]

    def start_garlic(self):
        st, b = self.srv.api("POST", "/api/saves/garlic/start", {"wait": True})
        self.assertEqual(st, 200, b)
        return self.job_of(b)

    def test_01_status_and_setup_when_down(self):
        st, b = self.srv.api("GET", "/api/saves/garlic")
        self.assertEqual(st, 200, b)
        g = b["garlic"]
        self.assertFalse(g["running"])
        self.assertTrue(g["canStart"])
        self.assertTrue(g["available"])
        # the pinned release goes before the copy found on the console (whose version is unknown)
        self.assertEqual((g.get("source"), g.get("version")), ("download", "1.7"), g)
        self.assertNotIn("elfPath", g)
        st, b = self.srv.api("GET", "/api/saves/engine")
        self.assertEqual(b["tier"], "raw")
        st, b = self.srv.api("GET", "/api/setup/check")
        chk = {c["id"]: c for c in b["checks"]}["garlic"]
        self.assertFalse(chk["ok"])
        self.assertTrue(chk["optional"])
        self.assertEqual((chk.get("action"), chk.get("actionId")), ("install", "garlic-save-manager"))
        self.assertEqual(self.loader.received, [])

    def test_01b_console_copy_only_when_the_release_cannot_be_downloaded(self):
        route = self.files.routes.pop("/garlic.elf")
        try:
            j = self.start_garlic()
        finally:
            self.files.routes["/garlic.elf"] = route
        self.assertEqual(j["state"], "done", j)
        self.assertEqual(self.loader.received, [self.console_elf])
        self.assertFalse(os.path.exists(os.path.join(self.data, "payloads", "garlic-save-manager.elf")))
        st, b = self.srv.api("GET", "/api/saves/garlic")
        g = b["garlic"]
        self.assertTrue(g["running"])
        self.assertEqual((g.get("source"), g.get("elfPath")), ("console", self.console_path), g)
        self.assertNotIn("version", g)                                   # not the pinned file: version unknown
        self.garlic.stop()

    def test_02_start_sends_the_pinned_release(self):
        n = len(self.loader.received)
        j = self.start_garlic()
        self.assertEqual(j["state"], "done", j)
        self.assertEqual(self.loader.received[n:], [self.companion_elf])
        self.assertTrue(self.garlic.running())
        st, b = self.srv.api("GET", "/api/saves/garlic")
        self.assertTrue(b["garlic"]["running"])
        self.assertEqual((b["garlic"].get("source"), b["garlic"].get("version")), ("download", "1.7"), b)
        st, b = self.srv.api("GET", "/api/setup/check")
        self.assertTrue({c["id"]: c for c in b["checks"]}["garlic"]["ok"])

    def test_03_never_a_second_copy(self):
        n = len(self.loader.received)
        j = self.start_garlic()
        self.assertEqual(j["state"], "done", j)
        self.assertEqual(len(self.loader.received), n)

    def test_04_foreign_program_on_the_port(self):
        self.garlic.stop()

        class NotGarlic(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                body = b"<html>something else</html>"
                self.send_response(404)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        http.server.ThreadingHTTPServer.allow_reuse_address = True
        other = http.server.ThreadingHTTPServer(("127.0.0.1", self.garlic_port), NotGarlic)
        threading.Thread(target=other.serve_forever, daemon=True).start()
        try:
            n = len(self.loader.received)
            j = self.start_garlic()
            self.assertEqual(j["state"], "failed", j)
            self.assertIn("not Garlic", j["error"])
            self.assertEqual(len(self.loader.received), n)
        finally:
            other.shutdown()
            other.server_close()

    def test_05_store_copy_without_a_console_copy(self):
        os.unlink(self.console_path)
        n = len(self.loader.received)
        j = self.start_garlic()
        self.assertEqual(j["state"], "done", j)
        self.assertEqual(self.loader.received[n:], [self.companion_elf])
        stored = os.path.join(self.data, "payloads", "garlic-save-manager.elf")
        self.assertEqual(harness.sha256_file(stored), hashlib.sha256(self.companion_elf).hexdigest())
        st, b = self.srv.api("GET", "/api/payloads")
        self.assertTrue({p["id"]: p for p in b["payloads"]}["garlic-save-manager"]["installed"])

    def test_06_install_starts_garlic_on_demand(self):
        self.garlic.stop()
        n = len(self.loader.received)
        m = self.mark()
        j = self.install("sv_plain")
        self.assertEqual(j["state"], "done", j)
        self.assertEqual(self.loader.received[n:], [self.companion_elf])   # the store's own pinned copy
        self.assertTrue(self.since(m, "/api/import_finish"))

    def test_07_running_title_is_not_replaced(self):
        m = self.mark()
        j = self.install("sv_running")
        self.assertEqual(j["state"], "failed", j)
        self.assertEqual(j["errorCode"], "title_running", j)
        self.assertEqual(self.since(m, "/api/create_pfs"), [])

    def test_08_running_title_is_not_transferred(self):
        write(os.path.join(self.home(PLAYER), "savedata_prospero", OTHER, "sdimg_SAVEDATA00"), b"IMAGE")
        m = self.mark()
        st, b = self.srv.api("POST", "/api/saves/transfer", {"fromUser": PLAYER, "toUser": GUEST, "titleId": OTHER, "move": True})
        self.assertEqual(st, 422, b)
        self.assertEqual(b.get("error"), "title_running", b)
        self.assertTrue(os.path.isfile(os.path.join(self.home(PLAYER), "savedata_prospero", OTHER, "sdimg_SAVEDATA00")))
        self.assertFalse(os.path.exists(os.path.join(self.home(GUEST), "savedata_prospero", OTHER)))
        self.assertEqual(self.since(m, "/api/export_title_dec"), [])

    def test_09_running_title_is_not_exported(self):
        if not self.garlic.running():
            self.garlic.start()
        write(os.path.join(self.home(PLAYER), "savedata_prospero", OTHER, "sdimg_SAVEDATA00"), b"IMAGE")
        self.garlic.saves[("%x" % PLAYER, OTHER, "sdimg_SAVEDATA00")] = {"files": {"game.dat": b"x", "sce_sys/param.sfo": make_sfo(title=OTHER)}}
        m = self.mark()
        st, b = self.srv.api("POST", "/api/saves/export", {"user": PLAYER, "titleId": OTHER, "dirName": "SAVEDATA00"})
        self.assertEqual(st, 422, b)
        self.assertEqual(b.get("error"), "title_running", b)
        self.assertEqual([r for r in self.garlic.log[m:] if r["url"] != "/api/status"], [])   # Garlic is not even asked

    def test_10_payload_launch_never_starts_a_second_garlic(self):
        if not self.garlic.running():
            self.garlic.start()
        n = len(self.loader.received)
        st, b = self.srv.api("POST", "/api/payloads/garlic-save-manager/launch", {})
        self.assertEqual(st, 409, b)
        self.assertEqual(b.get("error"), "already_running", b)
        self.assertEqual(len(self.loader.received), n)
        # stopped: Launch starts it the way the save engine does (pinned copy, port awaited)
        self.garlic.stop()
        st, b = self.srv.api("POST", "/api/payloads/garlic-save-manager/launch", {})
        self.assertEqual(st, 200, b)
        self.assertEqual(self.loader.received[n:], [self.companion_elf])
        self.assertTrue(self.garlic.running())

    def test_11_move_without_garlic_is_refused(self):
        # nothing to start: no download, no store copy, no console copy (removed in test_05)
        self.garlic.stop()
        stored = os.path.join(self.data, "payloads", "garlic-save-manager.elf")
        route = self.files.routes.pop("/garlic.elf")
        os.rename(stored, stored + ".off")
        img = os.path.join(self.home(PLAYER), "savedata_prospero", TITLE, "sdimg_SAVEDATA07")
        write(img, b"PLAYER-IMAGE")
        try:
            n = len(self.loader.received)
            st, b = self.srv.api("POST", "/api/saves/transfer", {"fromUser": PLAYER, "toUser": GUEST, "titleId": TITLE, "move": True})
        finally:
            os.rename(stored + ".off", stored)
            self.files.routes["/garlic.elf"] = route
        self.assertEqual(st, 422, b)
        self.assertEqual(b.get("error"), "resign_unavailable", b)
        self.assertIn("Nothing was changed", b.get("message", ""))
        self.assertEqual(len(self.loader.received), n)
        with open(img, "rb") as f:
            self.assertEqual(f.read(), b"PLAYER-IMAGE")                 # the source keeps its loadable save
        self.assertFalse(os.path.exists(os.path.join(self.home(GUEST), "savedata_prospero", TITLE)))


if __name__ == "__main__":
    unittest.main()
