#!/usr/bin/env python3
"""
test_saves_site.py — the Save Data Store against its live backend, the
XenoKing site (a Supabase project), through supabase_mock.SupabaseMock:

  * the published updates/saves-backend.json (served locally) is followed by
    a console with no backend settings, even before its feeds were ever
    refreshed; a stale cached copy is fetched again; Settings → Saves →
    Backend overrides it and "none" turns it off;
  * listing: the 57 site rows (xenoking_rows.json) with a display name from
    the notes (never the site's file_name), the sharer as author, the game as
    title, covers through /api/saves/store/<id>/cover (one signature per
    cover, then cached), source label "XenoKing"; the GitHub index stays
    merged; a second row with the same Garlic id is shown once;
  * downloads through the real install path: a Garlic row through the site's
    garlic-savedb function and a bucket row through a signed URL, both
    installed by the (simulated) native engine; Garlic failures are explained;
  * sharing: export → upload (host "site", the default) into
    ps5-saves/xeno/<yyyymmdd>-<16 hex>.zip → one insert of exactly the site's
    columns → listed at once; a refused insert, or a zip uploaded elsewhere,
    falls back to the GitHub issue — never with a link to the site's private
    bucket (a public copy goes up first) — or to the user's own submit
    endpoint; share_check still runs before any upload; a console that never
    listed saves still uploads and submits to the site;
  * an override in Settings that cannot be used never falls back to the
    published site; a deployment of the backend/supabase template typed into
    Settings gets moderated inserts, uploads its policies (migrations 0004 +
    0005, replayed here) accept and its download counter;
  * the console never writes anything else to the site (no update / delete,
    no other table, no download counter, no Garlic "list"), and the anon key
    goes to the site's origin only (never to another host, never across a
    redirect).

Everything is loopback: the backend URL comes from the published file or
settings; XENO_SAVES_INDEX_BASE serves the default GitHub index locally and
XENO_PIXELDRAIN_BASE points pixeldrain at the local Files host.
"""
import hashlib
import http.client
import http.server
import io
import json
import os
import re
import shutil
import struct
import sys
import tempfile
import threading
import time
import unittest
import urllib.parse
import urllib.request
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402
from supabase_mock import COLUMNS, SupabaseMock  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
ANON = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJyb2xlIjoiYW5vbiIsInJlZiI6Im1vY2sifQ.bW9jay1hbm9uLWtleS1mb3ItdGVzdHM"
PLAYER = 0x179A0CD8
TITLE = "PPSA01234"
SITE_COLUMNS = {"title_id", "game", "shared_by", "notes", "file_path", "file_name", "file_size"}
STORAGE_RE = re.compile(r"^xeno/\d{8}-[0-9a-f]{16}\.zip$")


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def build_sfo(entries):
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


def make_sfo(title, account, dirname="SAVEDATA00"):
    return build_sfo([
        ("ACCOUNT_ID", 0x0404, struct.pack("<Q", account), 8),
        ("MAINTITLE", 0x0204, b"Test Game", 128),
        ("SAVEDATA_DIRECTORY", 0x0204, dirname.encode(), 64),
        ("TITLE_ID", 0x0204, title.encode(), 12),
    ])


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


def read(path):
    with open(path, "rb") as f:
        return f.read()


def site_rows():
    with open(os.path.join(HERE, "xenoking_rows.json"), encoding="utf-8") as f:
        return json.load(f)["rows"]


def issue_block(issue_url):
    """The JSON block of a prefilled GitHub issue (what tools/saves/intake.py parses)."""
    body = urllib.parse.parse_qs(urllib.parse.urlsplit(issue_url).query)["body"][0]
    return json.loads(body.split("```json\n", 1)[1].split("\n```", 1)[0])


def template_upload_patterns(upto=None):
    """Object names the template's bucket insert policies allow, replayed from
    backend/supabase/migrations in order (DROP / CREATE POLICY on
    storage.objects); a policy without a name check allows any name."""
    mig = os.path.join(REPO, "backend", "supabase", "migrations")
    policies = {}
    for fn in sorted(os.listdir(mig)):
        if not fn.endswith(".sql") or (upto and fn[:4] > upto):
            continue
        with open(os.path.join(mig, fn), encoding="utf-8") as f:
            sql = re.sub(r"--[^\n]*", "", f.read())
        for stmt in sql.split(";"):
            m = re.search(r'DROP POLICY IF EXISTS "([^"]+)" ON storage\.objects', stmt)
            if m:
                policies.pop(m.group(1), None)
                continue
            m = re.search(r'CREATE POLICY "([^"]+)" ON storage\.objects\s+FOR INSERT.*?WITH CHECK\s*\((.*)\)\s*$', stmt, re.S)
            if m:
                policies[m.group(1)] = re.findall(r"name ~ '([^']+)'", m.group(2)) or [".*"]
    return [p for pats in policies.values() for p in pats]


class Files(threading.Thread):
    """Another host (published feeds, the GitHub index, plain downloads); records every request."""

    def __init__(self):
        super().__init__(daemon=True)
        self.routes = {}
        self.log = []
        self.pixeldrain = {}         # id -> bytes uploaded with PUT /api/file/<name>
        files = self

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def _send(self, method):
                n = int(self.headers.get("Content-Length") or 0)
                data = self.rfile.read(n) if n else b""
                files.log.append({"method": method, "path": self.path, "headers": {k.lower(): v for k, v in self.headers.items()}, "body": data})
                path = self.path.split("?", 1)[0]
                r = files.routes.get(path)
                if method == "PUT" and path.startswith("/api/file/"):           # pixeldrain upload
                    fid = "pd%04d" % (len(files.pixeldrain) + 1)
                    files.pixeldrain[fid] = data
                    r = (201, "application/json", json.dumps({"success": True, "id": fid}))
                elif path.startswith("/api/file/") and path[len("/api/file/"):] in files.pixeldrain:
                    r = (200, "application/zip", files.pixeldrain[path[len("/api/file/"):]])
                st, ct, body = r if r else (404, "application/json", b'{"error":"not found"}')
                if callable(body):
                    body = body()
                if isinstance(body, str):
                    body = body.encode()
                self.send_response(st)
                self.send_header("Content-Type", ct)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                if method != "HEAD":
                    self.wfile.write(body)

            def do_GET(self):
                self._send("GET")

            def do_HEAD(self):
                self._send("HEAD")

            def do_POST(self):
                self._send("POST")

            def do_PUT(self):
                self._send("PUT")

        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.srv.daemon_threads = True
        self.port = self.srv.server_address[1]
        self.base = "http://127.0.0.1:%d" % self.port
        self.start()

    def run(self):
        self.srv.serve_forever()

    def add(self, path, body, ctype="application/json"):
        self.routes[path] = (200, ctype, body)

    def close(self):
        self.srv.shutdown()


def no_key(test, entries, where):
    for e in entries:
        h = e["headers"]
        test.assertNotIn("apikey", h, "%s got the anon key: %s" % (where, e["path"]))
        test.assertNotIn("authorization", h, "%s got an Authorization header: %s" % (where, e["path"]))
        test.assertNotIn(ANON, json.dumps(h), "%s got the anon key: %s" % (where, e["path"]))


def published(base, name="XenoKing"):
    return {"kind": "supabase", "name": name, "url": base, "anonKey": ANON, "bucket": "ps5-saves", "garlicFunction": "garlic-savedb",
            "indexUrl": "https://raw.githubusercontent.com/XENOKING123/XENO-STORE/main/updates/saves.json", "submit": "site", "uploadHost": "site"}


class Base(unittest.TestCase):
    @classmethod
    def start_console(cls, files):
        cls.tmp = tempfile.mkdtemp(prefix="xeno-site-")
        cls.data = os.path.join(cls.tmp, "data")
        cls.sim = os.path.join(cls.data, "sim")
        os.makedirs(cls.sim, exist_ok=True)
        with open(os.path.join(cls.sim, "users.json"), "w") as f:
            json.dump({"users": [{"id": PLAYER, "name": "Player", "foreground": True}]}, f)
        harness.write_catalog(cls.data, [harness.make_game("test-game-ps5", "Test Game", TITLE, [
            harness.make_release("r1", files.base + "/game.bin", "PPSA01234 Test Game.ffpfsc", 1024)])])
        # Player's own PS5 save of TITLE (decrypted content simulated) + the installed title
        t = os.path.join(cls.home(PLAYER), "savedata_prospero", TITLE)
        write(os.path.join(t, "sdimg_SAVEDATA00"), b"IMG" + os.urandom(4096))
        write(os.path.join(t, "sce_sys", "SAVEDATA00", "param.sfo"), make_sfo(TITLE, 0xABCDEF))
        write(os.path.join(t, ".sim", "SAVEDATA00", "game.dat"), b"MY-OWN-SAVE-" + b"z" * 5000)
        write(os.path.join(t, ".sim", "SAVEDATA00", "sce_sys", "param.sfo"), make_sfo(TITLE, 0xABCDEF))
        os.makedirs(os.path.join(cls.data, "user", "app", TITLE), exist_ok=True)
        write(os.path.join(cls.sim, "system_data", "priv", "appmeta", TITLE, "param.json"), json.dumps({"contentVersion": "01.004.000"}).encode())
        env = {"XENO_SAVES_INDEX_BASE": files.base, "XENO_PORT_GARLIC": "1", "XENO_PORT_LOADER": "1", "XENO_PORT_DPI": "1",
               "XENO_PORT_ETAHEN": "1", "XENO_PORT_SHADOWMOUNT": "1", "XENO_PIXELDRAIN_BASE": files.base, "XENO_CATBOX_BASE": files.base}
        cls.srv = harness.XenoServer(cls.data, env=env).start()
        st, b = cls.srv.api("PUT", "/api/settings", {
            "catalog": {"companionsUrl": files.base + "/none.json", "featuredUrl": files.base + "/none.json",
                        "messagesUrl": files.base + "/none.json", "savesBackendUrl": files.base + "/saves-backend.json"},
            "saves": {"nickname": "tester"},
        })
        assert st == 200, b

    @classmethod
    def home(cls, uid):
        return os.path.join(cls.sim, "user", "home", "%x" % uid)

    def store(self, refresh=False, q=""):
        st, b = self.srv.api("GET", "/api/saves/store" + ("?refresh=1" if refresh else "") + q)
        self.assertEqual(st, 200, b)
        return b

    def job(self, body, timeout=60):
        j = body.get("job") or {}
        self.assertTrue(j.get("id"), body)
        deadline = time.time() + timeout
        while j.get("state") not in ("done", "failed", "cancelled") and time.time() < deadline:
            time.sleep(0.1)
            st, b = self.srv.api("GET", "/api/saves/jobs/%s" % j["id"])
            j = b.get("job", j)
        return j


# ---------------------------------------------------------------------------
# the site as the published backend
# ---------------------------------------------------------------------------

class SiteTests(Base):
    @classmethod
    def setUpClass(cls):
        cls.files = Files()
        cls.site = SupabaseMock(ANON).start()
        cls.rows = site_rows()
        for r in cls.rows:
            cls.site.add_row(**r)
        # a second row of an existing Garlic save (the site lists it twice; the store once)
        dup = dict(next(r for r in cls.rows if r["garlic_id"] == 174))
        dup.update(id="00000000-0000-4000-8000-000000000174", created_at="2026-10-01T00:00:00.000000+00:00")
        cls.site.add_row(**dup)
        # Garlic zips: Split Fiction (garlic 94) as Garlic packs it (one wrapper folder, no manifest)
        cls.garlic_zip = make_zip({"Split Fiction/sce_sys/param.sfo": make_sfo("PPSA08560", 0x5566778899AABBCC),
                                   "Split Fiction/sce_sys/icon0.png": b"\x89PNG\r\n\x1a\n" + b"\0" * 64,
                                   "Split Fiction/savedata.bin": b"SPLIT-FICTION-COMPLETED" + b"s" * 3000})
        cls.site.garlic[94] = cls.garlic_zip
        # a bucket row (a save shared earlier through the site itself)
        cls.bucket_zip = make_zip({"sce_sys/param.sfo": make_sfo(TITLE, 0x1122334455667788), "game.dat": b"BUCKET-SAVE-" + b"b" * 2000})
        cls.site.put_object("PPSA01234/bucket-save.zip", cls.bucket_zip)
        cls.site.put_object("PPSA25286/garlic-81-cover.png", b"\x89PNG\r\n\x1a\n" + b"COVER81", "image/png")
        cls.bucket_row = cls.site.add_row(title_id=TITLE, game="Test Game", shared_by="bucketeer", notes="Bucket save. Stored in the site's own bucket.",
                                          file_path="PPSA01234/bucket-save.zip", file_name="PPSA01234_Test_Game.zip", file_size=len(cls.bucket_zip),
                                          created_at="2026-10-15T09:00:00.000000+00:00")
        # the GitHub index (default feed, served locally) stays merged
        gh_zip = make_zip({"sce_sys/param.sfo": make_sfo(TITLE, 0), "game.dat": b"GITHUB"})
        cls.files.add("/gh.zip", gh_zip, "application/zip")
        cls.files.add("/saves.json", json.dumps({"schemaVersion": 1, "saves": [
            {"id": "gh_1", "titleId": TITLE, "title": "Test Game", "name": "From the GitHub index", "url": cls.files.base + "/gh.zip",
             "sha256": hashlib.sha256(gh_zip).hexdigest(), "size": len(gh_zip), "status": "published"}]}))
        cls.files.add("/none.json", "{}")
        cls.files.add("/saves-backend.json", json.dumps(published(cls.site.base)))
        cls.files.add("/elsewhere.zip", cls.garlic_zip, "application/zip")
        cls.start_console(cls.files)

    @classmethod
    def tearDownClass(cls):
        cls.srv.stop()
        cls.site.stop()
        cls.files.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def site_row(self, listing, garlic_id):
        for s in listing["saves"]:
            if s.get("garlicId") == str(garlic_id):
                return s
        self.fail("garlic %s not listed" % garlic_id)

    # ---- listing -----------------------------------------------------------
    def test_01_published_backend_listing(self):
        # no feeds refresh happened: the first listing loads the published saves-backend.json itself
        b = self.store()
        self.assertEqual(b["backend"], "supabase")
        self.assertEqual(b["backendName"], "XenoKing")
        self.assertTrue(b["siteUpload"])
        site = [s for s in b["saves"] if s["id"].startswith("sb:")]
        self.assertEqual(len(site), 57 + 1, [s["id"] for s in site][:5])       # 57 Garlic rows (one listed twice on the site) + the bucket row
        self.assertEqual(len([s for s in site if s.get("garlicId") == "174"]), 1)
        self.assertTrue(any(s["id"] == "gh_1" and s["source"] == "github" for s in b["saves"]))
        kinds = {x["kind"]: x for x in b["sources"]}
        self.assertTrue(kinds["supabase"]["ok"])
        self.assertEqual(kinds["supabase"]["name"], "XenoKing")
        by_gid = {s["garlicId"]: s for s in site if s.get("garlicId")}
        for r in self.rows:
            s = by_gid[str(r["garlic_id"])]
            self.assertEqual(s["author"], r["shared_by"])
            self.assertEqual(s["title"], r["game"])
            self.assertEqual(s["titleId"], r["title_id"])
            self.assertEqual(s["description"], r["notes"] or "")
            self.assertEqual(s["size"], r["file_size"])
            self.assertEqual(s["sourceName"], "XenoKing")
            self.assertEqual(s["source"], "garlic")
            self.assertNotEqual(s["name"], r["file_name"])
            self.assertNotIn(".zip", s["name"])
            self.assertLessEqual(len(s["name"]), 81, s["name"])
            self.assertTrue(s["name"].strip())
            if r["cover_path"]:
                self.assertEqual(s["cover"], "/api/saves/store/%s/cover" % s["id"])
            else:
                self.assertEqual(s["cover"], "")
            self.assertEqual(s["createdAt"], r["created_at"])
        names = {g: by_gid[str(g)]["name"] for g in (174, 81, 129, 148, 79, 90, 178, 101)}
        self.assertEqual(names[174], "100% Platinum")
        self.assertEqual(names[81], "Crimson Desert PS5 Starter Save")
        self.assertEqual(names[129], "DEATH STRANDING DIRECTOR'S CUT save")       # no notes
        self.assertEqual(names[79], "Dragon's Dogma 2 version 1.000")
        self.assertEqual(names[90], "Yotei Six dead")
        self.assertEqual(names[178], "Main story completed")
        self.assertEqual(names[101], "Super Starter Save LVL 40, no missions done")
        self.assertTrue(names[148].endswith("…") and names[148].startswith("99K stats, maxed materials"), names[148])
        self.assertEqual(by_gid["174"]["category"], "platinum")
        self.assertEqual(by_gid["101"]["category"], "starter")
        bucket = next(s for s in site if s["id"] == "sb:" + self.bucket_row["id"])
        self.assertEqual(bucket["name"], "Bucket save")
        self.assertEqual(bucket["storagePath"], "PPSA01234/bucket-save.zip")
        self.assertEqual(bucket["source"], "supabase")
        self.assertTrue(bucket["installed"])
        # reads only: one GET on the table, nothing through the function
        self.assertTrue(self.site.requests("GET", "/rest/v1/ps5_save_collection"))
        self.assertEqual(self.site.requests("POST", "/functions/v1/"), [])

    def test_02_cover_signed_once(self):
        b = self.store()
        s = self.site_row(b, 81)
        before = len(self.site.requests("POST", "/storage/v1/object/sign/"))
        for _ in range(3):
            c = http.client.HTTPConnection("127.0.0.1", self.srv.port, timeout=20)
            c.request("GET", s["cover"])
            r = c.getresponse()
            r.read()
            self.assertEqual(r.status, 302)
            loc = r.getheader("Location")
            c.close()
            self.assertTrue(loc.startswith(self.site.base + "/storage/v1/object/sign/ps5-saves/PPSA25286/garlic-81-cover.png?token="), loc)
        self.assertEqual(len(self.site.requests("POST", "/storage/v1/object/sign/")) - before, 1)   # cached after the first
        sign = self.site.requests("POST", "/storage/v1/object/sign/")[-1]
        self.assertEqual(json.loads(sign["body"]), {"expiresIn": 3600})
        with urllib.request.urlopen(loc, timeout=10) as r:
            self.assertEqual(r.read(), b"\x89PNG\r\n\x1a\nCOVER81")
        st, _ = self.srv.api("GET", "/api/saves/store/%s/cover" % self.site_row(b, 94)["id"])   # no cover_path → 404
        self.assertEqual(st, 404)

    # ---- downloads through the install path ---------------------------------
    def test_03_install_garlic_row(self):
        s = self.site_row(self.store(), 94)
        mark = len(self.site.log)
        st, b = self.srv.api("POST", "/api/saves/store/%s/install" % urllib.parse.quote(s["id"]), {"user": PLAYER, "backupFirst": False, "wait": True})
        self.assertEqual(st, 200, b)
        j = self.job(b)
        self.assertEqual(j["state"], "done", j)
        self.assertEqual(j["result"]["tier"], "native")
        fn = [e for e in self.site.log[mark:] if e["path"] == "/functions/v1/garlic-savedb"]
        self.assertEqual(len(fn), 1)
        self.assertEqual(json.loads(fn[0]["body"]), {"action": "download", "id": 94})
        self.assertEqual(fn[0]["headers"]["apikey"], ANON)
        self.assertEqual(fn[0]["headers"]["authorization"], "Bearer " + ANON)
        self.assertEqual(read(j["result"]["zipPath"]), self.garlic_zip)
        dest = os.path.join(self.home(PLAYER), "savedata_prospero", "PPSA08560", ".sim", "SAVEDATA00", "savedata.bin")
        self.assertTrue(read(dest).startswith(b"SPLIT-FICTION-COMPLETED"))
        self.assertEqual(self.site.requests("POST", "/rest/v1/rpc/"), [])    # no download counter on the site

    def test_04_install_bucket_row(self):
        mark = len(self.site.log)
        sid = "sb:" + self.bucket_row["id"]
        st, b = self.srv.api("POST", "/api/saves/store/%s/install" % sid, {"user": PLAYER, "backupFirst": True, "wait": True})
        self.assertEqual(st, 200, b)
        j = self.job(b)
        self.assertEqual(j["state"], "done", j)
        sign = [e for e in self.site.log[mark:] if e["method"] == "POST" and e["path"].startswith("/storage/v1/object/sign/")]
        self.assertEqual([e["path"] for e in sign], ["/storage/v1/object/sign/ps5-saves/PPSA01234/bucket-save.zip"])
        self.assertEqual(json.loads(sign[0]["body"]), {"expiresIn": 300})
        got = [e for e in self.site.log[mark:] if e["method"] == "GET" and e["path"].startswith("/storage/v1/object/sign/")]
        self.assertEqual(len(got), 1)
        self.assertNotIn("apikey", got[0]["headers"])                        # the signed URL needs no key
        self.assertEqual(read(j["result"]["zipPath"]), self.bucket_zip)
        dest = os.path.join(self.home(PLAYER), "savedata_prospero", TITLE, ".sim", "SAVEDATA00", "game.dat")
        self.assertTrue(read(dest).startswith(b"BUCKET-SAVE-"))

    def test_05_garlic_failures_explained(self):
        s = self.site_row(self.store(), 94)
        url = "/api/saves/store/%s/download" % s["id"]
        try:
            self.site.fail["garlic"] = (502, {"error": "Garlic session expired"})
            j = self.job(self.srv.api("POST", url, {"wait": True})[1])
            self.assertEqual(j["state"], "failed", j)
            self.assertIn("XenoKing could not fetch this save from Garlic (HTTP 502)", j["error"])
            del self.site.fail["garlic"]
            self.site.fail["garlic200"] = "Garlic session expired"
            j = self.job(self.srv.api("POST", url, {"wait": True})[1])
            self.assertEqual(j["state"], "failed", j)
            self.assertIn("XenoKing could not fetch this save from Garlic: Garlic session expired", j["error"])
            del self.site.fail["garlic200"]
            # an older function answering with a link: fetched from that host, without the key
            self.site.fail["garlic_url"] = self.files.base + "/elsewhere.zip"
            j = self.job(self.srv.api("POST", url, {"wait": True})[1])
            self.assertEqual(j["state"], "done", j)
            self.assertEqual(read(j["result"]["zipPath"]), self.garlic_zip)
            self.assertTrue([e for e in self.files.log if e["path"] == "/elsewhere.zip"])
        finally:
            self.site.fail.clear()
        no_key(self, self.files.log, "the other host")

    # ---- sharing -------------------------------------------------------------
    def export(self):
        st, b = self.srv.api("POST", "/api/saves/export", {"user": PLAYER, "titleId": TITLE, "dirName": "SAVEDATA00"})
        self.assertEqual(st, 200, b)
        self.assertTrue(b["shareable"], b)
        return b

    def test_06_share_upload_insert_listed(self):
        exp = self.export()
        st, s = self.srv.api("GET", "/api/settings")
        self.assertEqual(s["settings"]["saves"]["uploadHost"], "site")          # the default
        st, up = self.srv.api("POST", "/api/saves/upload", {"zipPath": exp["zipPath"]})   # no host: the default
        self.assertEqual(st, 200, up)
        self.assertEqual(up["host"], "site")
        sp = up["storagePath"]
        self.assertRegex(sp, STORAGE_RE)
        self.assertEqual(sp[5:13], time.strftime("%Y%m%d", time.gmtime()))
        self.assertEqual(up["url"], self.site.base + "/storage/v1/object/ps5-saves/" + sp)
        self.assertEqual(up["size"], os.path.getsize(exp["zipPath"]))
        self.assertEqual(up["sha256"], exp["sha256"])
        put = self.site.requests("POST", "/storage/v1/object/ps5-saves/")[-1]
        self.assertEqual(put["path"], "/storage/v1/object/ps5-saves/" + sp)
        self.assertEqual(put["headers"]["content-type"], "application/zip")
        self.assertEqual(put["headers"]["x-upsert"], "false")
        self.assertEqual(put["headers"]["apikey"], ANON)
        self.assertEqual(self.site.objects[sp][0], read(exp["zipPath"]))
        # the wizard's submit (name, description, tags, nickname, storagePath)
        meta = {"titleId": TITLE, "gameId": "test-game-ps5", "title": "Test Game", "category": "chapter", "name": "My run",
                "description": "Chapter 4 with max money.\nLoad slot 1.", "tags": ["ng+", "max money"], "nickname": "tester",
                "gameVersion": "01.004.000", "region": "EUR", "dirName": "SAVEDATA00", "size": up["size"], "sha256": up["sha256"],
                "url": up["url"], "storagePath": sp, "host": "site", "zipPath": exp["zipPath"]}
        st, r = self.srv.api("POST", "/api/saves/submit", meta)
        self.assertEqual(st, 200, r)
        self.assertEqual(r["method"], "site")
        self.assertEqual(r["status"], "published")
        self.assertEqual(r["backendName"], "XenoKing")
        self.assertNotIn("issueUrl", r)
        self.assertNotIn("qr", r)
        self.assertIn("live on XenoKing", r["note"])
        ins = self.site.requests("POST", "/rest/v1/ps5_save_collection")
        self.assertEqual(len(ins), 1)
        sent = json.loads(ins[0]["body"])
        self.assertEqual(set(sent), SITE_COLUMNS)                                # exactly the site's columns
        self.assertTrue(set(sent) <= set(COLUMNS))
        self.assertIn("return=representation", ins[0]["headers"]["prefer"])
        self.assertEqual(sent["title_id"], TITLE)
        self.assertEqual(sent["game"], "Test Game")
        self.assertEqual(sent["shared_by"], "tester")
        self.assertEqual(sent["notes"], "My run — Chapter 4 with max money. Load slot 1. #ng+ #max-money")
        self.assertEqual(sent["file_path"], sp)
        self.assertEqual(sent["file_name"], "PPSA01234_Test_Game.zip")
        self.assertEqual(sent["file_size"], up["size"])
        row = self.site.rows[-1]
        self.assertEqual(r["id"], row["id"])
        self.assertEqual(r["storeId"], "sb:" + row["id"])
        # in the store right away (no refresh), credited to the sharer
        b = self.store()
        mine = [s for s in b["saves"] if s["id"] == r["storeId"]]
        self.assertEqual(len(mine), 1, [s["id"] for s in b["saves"]][:5])
        self.assertEqual(mine[0]["name"], "My run — Chapter 4 with max money")
        self.assertEqual(mine[0]["author"], "tester")
        self.assertEqual(mine[0]["storagePath"], sp)
        self.assertEqual(mine[0]["sourceName"], "XenoKing")
        # and it downloads back from the bucket, byte for byte
        j = self.job(self.srv.api("POST", "/api/saves/store/%s/download" % r["storeId"], {"wait": True})[1])
        self.assertEqual(j["state"], "done", j)
        self.assertEqual(read(j["result"]["zipPath"]), read(exp["zipPath"]))
        self.__class__.shared = (exp, up)

    def test_07_insert_refused_falls_back_to_github(self):
        exp, up = self.shared
        meta = {"titleId": TITLE, "title": "Test Game", "category": "starter", "name": "Second", "description": "max money",
                "nickname": "tester", "size": up["size"], "sha256": up["sha256"], "url": up["url"], "storagePath": up["storagePath"],
                "zipPath": exp["zipPath"]}
        before = len(self.site.rows)
        try:
            self.site.fail["insert"] = (500, {"code": "XX000", "message": "database is read-only"})
            st, r = self.srv.api("POST", "/api/saves/submit", meta)
        finally:
            self.site.fail.clear()
        self.assertEqual(st, 200, r)
        self.assertEqual(r["method"], "github")
        self.assertTrue(r["issueUrl"].startswith("https://github.com/XENOKING123/XENO-STORE/issues/new?"))
        self.assertIn("XenoKing refused the save (HTTP 500: database is read-only)", r["siteError"])
        self.assertEqual(len(self.site.rows), before)
        self.assertIn("site insert failed", self.srv.log_text())
        # the issue never carries the site's private object (it opens with the anon key only, so intake
        # could not fetch it): a public copy of the same export went to pixeldrain first
        blk = issue_block(r["issueUrl"])
        self.assertEqual(r["publicHost"], "pixeldrain")
        self.assertEqual(blk["url"], r["publicUrl"])
        self.assertTrue(blk["url"].startswith(self.files.base + "/api/file/"), blk["url"])
        self.assertNotIn(self.site.base, r["issueUrl"])
        self.assertNotIn("/storage/v1/", blk["url"])
        self.assertEqual(blk["sha256"], exp["sha256"])
        self.assertEqual(blk["size"], os.path.getsize(exp["zipPath"]))
        with urllib.request.urlopen(blk["url"], timeout=10) as resp:          # no key, like intake.py and consoles
            self.assertEqual(resp.read(), read(exp["zipPath"]))
        with self.assertRaises(urllib.error.HTTPError) as cm:                  # while the site object needs the key
            urllib.request.urlopen(up["url"], timeout=10)
        self.assertEqual(cm.exception.code, 401)
        self.site.log.pop()   # that request came from the test, not the console
        # without the zip at hand (an API client), nothing goes to GitHub with the private link
        try:
            self.site.fail["insert"] = (503, {"message": "temporarily unavailable"})
            st, r = self.srv.api("POST", "/api/saves/submit", dict(meta, zipPath=""))
        finally:
            self.site.fail.clear()
        self.assertEqual(st, 422, r)
        self.assertEqual(r["error"], "site_insert_failed")
        self.assertIn("XenoKing refused the save (HTTP 503", r["message"])
        self.assertIn("private bucket", r["message"])
        # a zip uploaded elsewhere is never inserted (file_path must be the site's object); its link is public already
        pd = self.files.base + "/api/file/pd0001"
        st, r = self.srv.api("POST", "/api/saves/submit", dict(meta, url=pd, storagePath=""))
        self.assertEqual(r["method"], "github")
        self.assertIn("not uploaded to XenoKing", r["siteError"])
        self.assertNotIn("publicHost", r)
        self.assertEqual(issue_block(r["issueUrl"])["url"], pd)
        st, r = self.srv.api("POST", "/api/saves/submit", dict(meta, storagePath="../../other-table.zip"))
        self.assertEqual(r["method"], "github")
        self.assertTrue(issue_block(r["issueUrl"])["url"].startswith(self.files.base + "/api/file/"))
        self.assertEqual(len(self.site.rows), before)
        no_key(self, self.files.log, "the public host")
        # the stand-in refuses unknown columns like PostgREST (the console never sends one)
        req = urllib.request.Request(self.site.base + "/rest/v1/ps5_save_collection", method="POST",
                                     data=json.dumps({"title_id": TITLE, "game": "x", "shared_by": "x", "file_path": "x", "file_name": "x", "category": "starter"}).encode(),
                                     headers={"apikey": ANON, "Authorization": "Bearer " + ANON, "Content-Type": "application/json"})
        try:
            urllib.request.urlopen(req, timeout=10)
            self.fail("unknown column accepted")
        except urllib.error.HTTPError as e:
            self.assertEqual(e.code, 400)
            self.assertEqual(json.loads(e.read())["code"], "PGRST204")
        self.site.log.pop()   # that request came from the test, not the console

    def test_08_upload_guards(self):
        exp, _ = self.shared
        mark = len(self.site.log)
        # share_check runs before any upload: a raw console backup is refused
        raw = os.path.join(os.path.dirname(exp["zipPath"]), "raw-backup.zip")
        write(raw, make_zip({"sdimg_SAVEDATA00": b"RAW", "xeno-backup.json": b"{}"}))
        st, b = self.srv.api("POST", "/api/saves/upload", {"zipPath": raw, "host": "site"})
        self.assertEqual(st, 422, b)
        self.assertEqual(b["error"], "not_shareable")
        st, b = self.srv.api("POST", "/api/saves/upload", {"zipPath": "/etc/hostname", "host": "site"})
        self.assertEqual(st, 403, b)
        self.assertEqual(self.site.log[mark:], [])
        # the site refusing the object is reported, nothing is listed
        try:
            self.site.fail["upload"] = (413, {"statusCode": "413", "error": "Payload too large", "message": "The object exceeded the maximum allowed size"})
            st, b = self.srv.api("POST", "/api/saves/upload", {"zipPath": exp["zipPath"], "host": "site"})
        finally:
            self.site.fail.clear()
        self.assertEqual(st, 422, b)
        self.assertEqual(b["error"], "upload_failed")
        self.assertIn("XenoKing refused the upload (HTTP 413: The object exceeded the maximum allowed size)", b["message"])

    def test_09_key_never_leaves_the_site(self):
        # a redirect from the site is not followed: the key cannot be carried to another host
        try:
            self.site.fail["list_redirect"] = self.files.base + "/steal"
            b = self.store(refresh=True)
        finally:
            self.site.fail.clear()
        self.assertFalse([e for e in self.files.log if e["path"].startswith("/steal")])
        self.assertTrue(any(s["id"].startswith("sb:") for s in b["saves"]))        # the cached copy is still served
        no_key(self, self.files.log, "the feeds / index host")

    def test_10_custom_submit_endpoint(self):
        # the user's own submit endpoint still gets what the site did not take
        self.files.add("/submit", json.dumps({"ok": True}))
        st, r = self.srv.api("PUT", "/api/settings", {"saves": {"submitEndpoint": self.files.base + "/submit"}})
        self.assertEqual(st, 200, r)
        try:
            meta = {"titleId": TITLE, "title": "Test Game", "category": "starter", "name": "Elsewhere", "description": "on pixeldrain",
                    "nickname": "tester", "size": 10, "sha256": "", "url": self.files.base + "/api/file/pd0001", "host": "pixeldrain"}
            st, r = self.srv.api("POST", "/api/saves/submit", meta)
            self.assertEqual(st, 200, r)
            self.assertEqual(r["method"], "custom")
            self.assertIn("not uploaded to XenoKing", r["siteError"])
            self.assertNotIn("issueUrl", r)
            hits = [e for e in self.files.log if e["method"] == "POST" and e["path"] == "/submit"]
            self.assertEqual(len(hits), 1)
            sent = json.loads(hits[0]["body"])
            self.assertEqual((sent["titleId"], sent["name"], sent["url"]), (TITLE, "Elsewhere", meta["url"]))
            # a share the site takes stays on the site
            exp, _ = self.shared
            st, up = self.srv.api("POST", "/api/saves/upload", {"zipPath": exp["zipPath"], "host": "site"})
            self.assertEqual(st, 200, up)
            st, r = self.srv.api("POST", "/api/saves/submit", dict(meta, name="On the site", url=up["url"], storagePath=up["storagePath"],
                                                                   size=up["size"], sha256=up["sha256"], host="site"))
            self.assertEqual(r["method"], "site", r)
            self.assertEqual(len([e for e in self.files.log if e["path"] == "/submit"]), 1)
        finally:
            self.srv.api("PUT", "/api/settings", {"saves": {"submitEndpoint": ""}})

    def test_11_long_share_name_one_ellipsis(self):
        exp, _ = self.shared
        st, up = self.srv.api("POST", "/api/saves/upload", {"zipPath": exp["zipPath"], "host": "site"})
        self.assertEqual(st, 200, up)
        name = "N" * 30 + " " + "\u00e9" * 70
        meta = {"titleId": TITLE, "title": "Test Game", "category": "starter", "name": name, "description": "word " * 50,
                "nickname": "tester", "size": up["size"], "sha256": up["sha256"], "url": up["url"], "storagePath": up["storagePath"], "host": "site"}
        st, r = self.srv.api("POST", "/api/saves/submit", meta)
        self.assertEqual(r["method"], "site", r)
        sent = json.loads(self.site.requests("POST", "/rest/v1/ps5_save_collection")[-1]["body"])
        self.assertTrue(sent["notes"].startswith("N" * 30 + " " + "\u00e9" * 48 + "\u2026 \u2014 word"), sent["notes"][:100])
        listed = [x for x in self.store()["saves"] if x["id"] == r["storeId"]]
        self.assertEqual(len(listed), 1)
        self.assertEqual(listed[0]["name"], "N" * 30 + " " + "\u00e9" * 48 + "\u2026")
        self.assertEqual(listed[0]["name"].count("\u2026"), 1)

    def test_99_only_reads_upload_and_insert(self):
        allowed = []
        for e in self.site.log:
            m, p = e["method"], e["path"]
            if m == "GET" and p == "/rest/v1/ps5_save_collection":
                q = urllib.parse.parse_qs(e["query"])
                self.assertEqual(q.get("select"), ["*"])
                continue
            if m == "GET" and p.startswith("/storage/v1/object/sign/ps5-saves/"):
                continue
            if m == "POST" and p.startswith("/storage/v1/object/sign/ps5-saves/"):
                continue
            if m == "POST" and p == "/functions/v1/garlic-savedb":
                self.assertEqual(json.loads(e["body"])["action"], "download")
                continue
            if m == "POST" and p.startswith("/storage/v1/object/ps5-saves/"):
                self.assertRegex(p[len("/storage/v1/object/ps5-saves/"):], STORAGE_RE)
                allowed.append(e)
                continue
            if m == "POST" and p == "/rest/v1/ps5_save_collection":
                self.assertTrue(set(json.loads(e["body"])) <= SITE_COLUMNS)
                allowed.append(e)
                continue
            self.fail("the console sent %s %s to the site" % (m, p))
        self.assertTrue(allowed)
        for e in self.site.log:
            if not (e["method"] == "GET" and e["path"].startswith("/storage/v1/object/sign/")):
                self.assertEqual(e["headers"].get("apikey"), ANON, e["path"])
        no_key(self, self.files.log, "another host")


# ---------------------------------------------------------------------------
# published file vs settings, refresh of a stale copy, backend off
# ---------------------------------------------------------------------------

class BackendChoiceTests(Base):
    @classmethod
    def setUpClass(cls):
        cls.files = Files()
        cls.a = SupabaseMock(ANON).start()
        cls.b = SupabaseMock(ANON).start()
        for i in range(3):
            cls.a.add_row(title_id="PPSA0100%d" % i, game="Game A%d" % i, shared_by="alice", notes="From site A. Row %d" % i,
                          file_path="garlic:%d" % (500 + i), file_name="x.zip", garlic_id=500 + i)
        # odd rows: a 1000-character first sentence, a first line, rows a console must skip
        cls.a.add_row(title_id="PPSA01009", game="Odd Game", shared_by="alice", notes="\u03a9" * 1000, file_path="garlic:600", file_name="z.zip", garlic_id=600)
        cls.a.add_row(title_id="PPSA01010", game="Odd Game 2", shared_by="alice", notes="First line here\nsecond line. More", file_path="garlic:601",
                      file_name="z.zip", garlic_id=601)
        cls.a.add_row(title_id="CUSA00001", game="PS4 game", shared_by="mallory", notes="ps4", file_path="garlic:602", file_name="z.zip", garlic_id=602)
        cls.a.add_row(id="../../etc", title_id="PPSA01011", game="Bad id", shared_by="mallory", notes="bad id", file_path="garlic:603", file_name="z.zip", garlic_id=603)
        cls.a.add_row(title_id="PPSA01012", game="No file", shared_by="mallory", notes="nothing to download", file_path="../secret.zip", file_name="z.zip")
        # notes as a share writes them: "<name cut with …> — <description>", and a cut that lands right after the " — "
        cls.a.add_row(title_id="PPSA01013", game="Shared", shared_by="alice", notes="\u03a9" * 30 + " " + "\u00e9" * 48 + "\u2026 \u2014 " + "word " * 40,
                      file_path="garlic:604", file_name="z.zip", garlic_id=604)
        cls.a.add_row(title_id="PPSA01014", game="Shared 2", shared_by="alice", notes="A" * 70 + " \u2014 " + "b" * 40,
                      file_path="garlic:605", file_name="z.zip", garlic_id=605)
        for i in range(2):
            cls.b.add_row(title_id="PPSA0200%d" % i, game="Game B%d" % i, shared_by="bob", notes="From site B",
                          file_path="PPSA0200%d/b.zip" % i, file_name="y.zip")
        cls.files.add("/saves.json", json.dumps({"schemaVersion": 1, "saves": []}))
        cls.files.add("/none.json", "{}")
        cls.files.add("/saves-backend.json", json.dumps(published(cls.a.base)))
        cls.start_console(cls.files)

    @classmethod
    def tearDownClass(cls):
        cls.srv.stop()
        cls.a.stop()
        cls.b.stop()
        cls.files.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def authors(self, b):
        return sorted({s["author"] for s in b["saves"] if s["id"].startswith("sb:")})

    def test_01_settings_override_and_off(self):
        b = self.store()
        self.assertEqual(self.authors(b), ["alice"])
        self.assertEqual(b["backendName"], "XenoKing")
        by_title = {s["titleId"]: s for s in b["saves"] if s["id"].startswith("sb:")}
        self.assertEqual(len(by_title), 7)                                        # the 3 rows + 4 odd ones; mallory's are skipped
        long_name = by_title["PPSA01009"]["name"]
        self.assertEqual(long_name, "\u03a9" * 80 + "\u2026")
        self.assertEqual(by_title["PPSA01013"]["name"], "\u03a9" * 30 + " " + "\u00e9" * 48 + "\u2026")   # one ellipsis, not two
        self.assertEqual(by_title["PPSA01014"]["name"], "A" * 70 + "\u2026")                             # no dangling " —"
        self.assertEqual(len(by_title["PPSA01009"]["description"]), 1000)
        self.assertEqual(by_title["PPSA01010"]["name"], "First line here")
        self.assertEqual(by_title["PPSA01000"]["name"], "From site A")
        # Settings → Saves → Backend wins over the published file
        st, r = self.srv.api("PUT", "/api/settings", {"saves": {"backend": {"kind": "supabase", "url": self.b.base + "/", "anonKey": ANON}}})
        self.assertEqual(st, 200, r)
        b = self.store()
        self.assertEqual(self.authors(b), ["bob"])
        self.assertEqual(b["backendName"], "127.0.0.1")
        # a new backend URL must be https (a loopback mock only on the host build)
        st, r = self.srv.api("PUT", "/api/settings", {"saves": {"backend": {"kind": "supabase", "url": "http://192.168.1.20:54321", "anonKey": ANON}}})
        self.assertEqual(st, 400, r)
        self.assertEqual(r["error"], "invalid_settings")
        self.assertIn("saves.backend.url must be https://", r["message"])
        self.assertEqual(self.authors(self.store()), ["bob"])                      # unchanged
        # an override that cannot be used never falls back to the published site: GitHub index only, and it says why
        st, r = self.srv.api("PUT", "/api/settings", {"saves": {"backend": {"kind": "supabase", "url": self.b.base, "anonKey": ""}}})
        self.assertEqual(st, 200, r)
        mark = len(self.a.log)
        b = self.store(refresh=True)
        self.assertEqual(self.authors(b), [])
        self.assertFalse(b["siteUpload"])
        self.assertEqual(b["backendName"], "")
        self.assertIn("anon key", b["backendError"])
        self.assertTrue(any(x["kind"] == "github" for x in b["sources"]))
        self.assertEqual(self.a.log[mark:], [])
        # an older settings.json with a plain-http URL (earlier versions took it) is the same
        self.srv.stop()
        path = os.path.join(self.data, "settings.json")
        with open(path, encoding="utf-8") as f:
            disk = json.load(f)
        disk["saves"]["backend"].update(kind="supabase", url="http://example.com", anonKey=ANON)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(disk, f)
        self.srv.start()
        st, r = self.srv.api("GET", "/api/settings")
        self.assertEqual(r["settings"]["saves"]["backend"]["url"], "http://example.com")   # the file was not rejected
        b = self.store(refresh=True)
        self.assertEqual(self.authors(b), [])
        self.assertIn("must be https://", b["backendError"])
        self.assertEqual(self.a.log[mark:], [])
        st, e = self.srv.api("POST", "/api/saves/export", {"user": PLAYER, "titleId": TITLE, "dirName": "SAVEDATA00"})
        self.assertEqual(st, 200, e)
        st, r = self.srv.api("POST", "/api/saves/upload", {"zipPath": e["zipPath"], "host": "site"})
        self.assertEqual(st, 422, r)
        self.assertEqual(r["error"], "site_unavailable")
        st, r = self.srv.api("PUT", "/api/settings", {"saves": {"nickname": "tester2"}})   # the stored value does not block other settings
        self.assertEqual(st, 200, r)
        self.assertEqual(self.a.log[mark:], [])
        # off: no site rows, no site upload
        st, r = self.srv.api("PUT", "/api/settings", {"saves": {"backend": {"kind": "none", "url": "", "anonKey": ""}}})
        self.assertEqual(st, 200, r)
        b = self.store()
        self.assertEqual(self.authors(b), [])
        self.assertFalse(b["siteUpload"])
        self.assertEqual(b["backendName"], "")
        st, e = self.srv.api("POST", "/api/saves/export", {"user": PLAYER, "titleId": TITLE, "dirName": "SAVEDATA00"})
        self.assertEqual(st, 200, e)
        st, r = self.srv.api("POST", "/api/saves/upload", {"zipPath": e["zipPath"], "host": "site"})
        self.assertEqual(st, 422, r)
        self.assertEqual(r["error"], "site_unavailable")
        st, r = self.srv.api("PUT", "/api/settings", {"saves": {"backend": {"kind": "github"}}})
        self.assertEqual(st, 200, r)
        self.assertEqual(self.authors(self.store()), ["alice"])

    def test_02_stale_published_copy_refreshes(self):
        # the owner points the published file elsewhere; a console whose cached copy is older than 6 h follows it
        self.srv.stop()
        self.files.add("/saves-backend.json", json.dumps(published(self.b.base, name="XenoKing B")))
        cached = os.path.join(self.data, "feeds", "saves-backend.json")
        self.assertTrue(os.path.exists(cached))
        old = time.time() - 7 * 3600
        os.utime(cached, (old, old))
        self.srv.start()
        b = self.store()
        self.assertEqual(b["backendName"], "XenoKing B")
        self.assertEqual(self.authors(b), ["bob"])
        self.assertIn("XenoKing B", read(cached).decode())
        no_key(self, self.files.log, "the feeds host")


# ---------------------------------------------------------------------------
# a console that never listed saves (first start, an API client)
# ---------------------------------------------------------------------------

class FirstUseTests(Base):
    """Upload and submit follow the published saves-backend.json on their own."""

    def setUp(self):
        self.files = Files()
        self.site = SupabaseMock(ANON).start()
        self.files.add("/saves.json", json.dumps({"schemaVersion": 1, "saves": []}))
        self.files.add("/none.json", "{}")
        self.files.add("/saves-backend.json", json.dumps(published(self.site.base)))
        type(self).start_console(self.files)

    def tearDown(self):
        self.srv.stop()
        self.site.stop()
        self.files.close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_01_upload_before_any_listing(self):
        st, exp = self.srv.api("POST", "/api/saves/export", {"user": PLAYER, "titleId": TITLE, "dirName": "SAVEDATA00"})
        self.assertEqual(st, 200, exp)
        st, up = self.srv.api("POST", "/api/saves/upload", {"zipPath": exp["zipPath"], "host": "site"})
        self.assertEqual(st, 200, up)
        self.assertEqual(up["host"], "site")
        self.assertRegex(up["storagePath"], STORAGE_RE)
        self.assertEqual(self.site.requests("GET", "/rest/v1/"), [])               # nothing was listed first
        st, up = self.srv.api("POST", "/api/saves/upload", {"zipPath": exp["zipPath"]})   # the default host is the site too
        self.assertEqual((st, up.get("host")), (200, "site"), up)

    def test_02_submit_before_any_listing(self):
        sp = "xeno/20261015-0123456789abcdef.zip"
        data = make_zip({"sce_sys/param.sfo": make_sfo(TITLE, 0), "game.dat": b"FIRST"})
        self.site.put_object(sp, data)
        meta = {"titleId": TITLE, "title": "Test Game", "category": "starter", "name": "First", "description": "first share",
                "nickname": "tester", "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                "url": self.site.base + "/storage/v1/object/ps5-saves/" + sp, "storagePath": sp, "host": "site"}
        st, r = self.srv.api("POST", "/api/saves/submit", meta)
        self.assertEqual(st, 200, r)
        self.assertEqual(r["method"], "site", r)
        self.assertEqual(len(self.site.requests("POST", "/rest/v1/ps5_save_collection")), 1)


# ---------------------------------------------------------------------------
# a deployment of backend/supabase typed into Settings
# ---------------------------------------------------------------------------

class TemplateTests(Base):
    @classmethod
    def setUpClass(cls):
        cls.files = Files()
        cls.tpl = SupabaseMock(ANON, template=True, upload_patterns=template_upload_patterns()).start()
        cls.zip = make_zip({"sce_sys/param.sfo": make_sfo(TITLE, 0), "game.dat": b"TEMPLATE-SAVE-" + b"t" * 500})
        cls.tpl.put_object("PPSA01234/0000-save.zip", cls.zip)
        cls.pub = cls.tpl.add_row(title_id=TITLE, game="Test Game", shared_by="owner", notes="Published on the template.", file_path="PPSA01234/0000-save.zip",
                                  file_name="PPSA01234_Test_Game.zip", file_size=len(cls.zip), category="platinum", status="published")
        cls.tpl.add_row(title_id=TITLE, game="Test Game", shared_by="someone", notes="Waiting for review.", file_path="PPSA01234/0001-save.zip",
                        file_name="PPSA01234_Test_Game.zip", status="pending")
        cls.files.add("/saves.json", json.dumps({"schemaVersion": 1, "saves": []}))
        cls.files.add("/none.json", "{}")
        cls.files.add("/saves-backend.json", json.dumps({"kind": "github"}))
        cls.start_console(cls.files)
        st, r = cls.srv.api("PUT", "/api/settings", {"saves": {"backend": {"kind": "supabase", "url": cls.tpl.base, "anonKey": ANON}}})
        assert st == 200, r

    @classmethod
    def tearDownClass(cls):
        cls.srv.stop()
        cls.tpl.stop()
        cls.files.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_00_migrations_take_console_uploads(self):
        sp = "xeno/20261015-0123456789abcdef.zip"
        self.assertFalse(any(re.fullmatch(p, sp) for p in template_upload_patterns(upto="0004")))   # what broke console shares
        self.assertTrue(any(re.fullmatch(p, sp) for p in template_upload_patterns()))
        for bad in ("xeno/../x.zip", "xeno/2026-0123456789abcdef.zip", "other/20261015-0123456789abcdef.zip"):
            self.assertFalse(any(re.fullmatch(p, bad) for p in template_upload_patterns()), bad)

    def test_01_listing_published_only(self):
        b = self.store()
        self.assertTrue(b["siteUpload"])
        mine = [s for s in b["saves"] if s["id"].startswith("sb:")]
        self.assertEqual([s["author"] for s in mine], ["owner"])
        self.assertEqual(mine[0]["category"], "platinum")

    def test_02_share_waits_for_review(self):
        st, exp = self.srv.api("POST", "/api/saves/export", {"user": PLAYER, "titleId": TITLE, "dirName": "SAVEDATA00"})
        self.assertEqual(st, 200, exp)
        st, up = self.srv.api("POST", "/api/saves/upload", {"zipPath": exp["zipPath"]})
        self.assertEqual(st, 200, up)
        self.assertEqual(up["host"], "site")
        self.assertIn(up["storagePath"], self.tpl.objects)                           # the bucket policy took it
        meta = {"titleId": TITLE, "title": "Test Game", "category": "starter", "name": "Template share", "description": "max money",
                "nickname": "tester", "size": up["size"], "sha256": up["sha256"], "url": up["url"], "storagePath": up["storagePath"],
                "zipPath": exp["zipPath"], "host": "site"}
        st, r = self.srv.api("POST", "/api/saves/submit", meta)
        self.assertEqual(st, 200, r)
        self.assertEqual((r["method"], r["status"]), ("site", "pending"), r)
        self.assertNotIn("issueUrl", r)
        self.assertIn("waits for review", r["note"])
        sent = json.loads(self.tpl.requests("POST", "/rest/v1/ps5_save_collection")[-1]["body"])
        self.assertEqual(set(sent), SITE_COLUMNS | {"status", "source"})
        self.assertEqual((sent["status"], sent["source"]), ("pending", "xeno"))
        self.assertEqual(self.tpl.rows[-1]["status"], "pending")
        probes = [e for e in self.tpl.requests("GET", "/rest/v1/ps5_save_collection") if "select=status" in e["query"]]
        self.assertEqual(len(probes), 1)                                              # asked once, remembered
        self.assertFalse(any(s["id"] == r.get("storeId") for s in self.store()["saves"]))   # not listed until published

    def test_03_download_counted(self):
        sid = "sb:" + self.pub["id"]
        j = self.job(self.srv.api("POST", "/api/saves/store/%s/download" % sid, {"wait": True})[1])
        self.assertEqual(j["state"], "done", j)
        self.assertEqual(read(j["result"]["zipPath"]), self.zip)
        calls = self.tpl.requests("POST", "/rest/v1/rpc/")
        deadline = time.time() + 10
        while not calls and time.time() < deadline:
            time.sleep(0.1)
            calls = self.tpl.requests("POST", "/rest/v1/rpc/")
        self.assertEqual([e["path"] for e in calls], ["/rest/v1/rpc/increment_downloads"])
        self.assertEqual(json.loads(calls[0]["body"]), {"id": self.pub["id"]})
        self.assertEqual(self.pub["downloads"], 1)


if __name__ == "__main__":
    unittest.main()
