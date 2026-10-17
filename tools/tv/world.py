#!/usr/bin/env python3
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
world.py - the temporary test world of the live TV preview (PORT-SPEC 4.1).

Two levels:

RunServers (one per preview_live.py run, shared by every world of the run)
  files   tests/host/range_server.py as a child process: the release files
          (ok/, slow/, f404/, r429/ routes); generated once per run
  art     art_server.ArtServer: PIL covers and heroes for the fixture's
          {{ART}} URLs, plus the refused images
  feed    FeedServer: the store update feed (updates/payloads.json and the
          update ELF, a copy of the host binary served as the next version),
          the TV app feed (updates/tv-app.json and a PPSA99500.ffpkg image
          with the UFS2 magic), catalog/revision.json (answered after 2 s, so
          Settings shows the refresh as busy), a GitHub API stand-in (404)
          and each world's Save Data Store files under /w/<world>/: the
          GitHub saves index (saves.json), its zips and the published
          saves-backend.json

World (one per scenario group of scenarios.json)
  <tmp>/data            the backend's --data: catalog.json (the TV fixture
                        catalogue, revision 2000000000, {{FILES}} and {{ART}}
                        filled with the run's servers, the job plan's routes),
                        feeds/featured.json, settings.json (sources
                        acknowledged or not, 1 connection per job, the
                        local feeds), the TV app image and install record of
                        the tvApp state, and the saved update the updater writes;
                        for the Save Data Store (spec "saves", SAVES_MODES):
                        sim/users.json (two profiles), the installed games
                        (user/app/<titleId> and their appmeta versions) and a
                        profile's save to back up
  site                  the XenoKing site's stand-in (tests/host/supabase_mock.py)
                        for the saves worlds, named by the published
                        saves-backend.json
  <tmp>/R1, <tmp>/R2    extra --root dirs with the Library files
                        (tvworld.LIBRARY_FILES; <data>/user/app/PPSA05555)
  <tmp>/app0/xeno       the starter's bundled copy (the host binary) and version.txt
  backend               build/host/xeno-store --port B --data D --root R1 --root R2
                        --no-browser, with XENO_HOST_ART_FIXTURE,
                        XENO_HOST_RESOLVE_HOST, XENO_TVAPI_UPDATES=1,
                        XENO_APP_BASE, XENO_GITHUB_API_BASE,
                        XENO_SAVES_INDEX_BASE (this world's saves index) and
                        XENO_PORT_LOADER (the sentinel below); the saves worlds
                        add XENO_HOST_RUNNING_TITLE, and "garlic" turns the
                        native save engine off with Garlic and the loader closed
  fake loader           fake_loader.py on L, starting the host binary on B
                        (loader "fake"); "closed" is a port nobody listens on
  sentinel              a record-only fake loader on the backend's own
                        XENO_PORT_LOADER: it must receive nothing, because the
                        store never relaunches itself after a TV stop
  proxy                 GatedProxy on A, the address the app is given: it
                        listens only while the backend listens on B (so a
                        stopped backend is a refused connection, as on the
                        console), rewrites Host to B and records every
                        request's method, path and status (check 3)

Dependencies: tests/host/tvworld.py (fixture loading, LIBRARY_FILES, the app's
HTTP client), tests/host/harness.py (RangeServerProcess, free_port),
tests/host/range_server.py and tests/host/tv/fixture-catalog.json /
fixture-featured.json, and tests/host/supabase_mock.py for the saves worlds.
They are imported lazily, on the first World or RunServers; when they are
missing, WorldDependencyError says which file.

Python 3 standard library + PIL (through art_server). Run under python3 -I.
"""
import hashlib
import importlib
import json
import os
import random
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
TESTS_HOST = os.path.join(REPO, "tests", "host")
TESTS_TV = os.path.join(TESTS_HOST, "tv")
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import art_server  # noqa: E402
import fake_loader  # noqa: E402

MiB = 1024 * 1024
FIXTURE_REVISION = 2000000000
UFS2_MAGIC_OFFSET = 65536 + 1372
UFS2_MAGIC = b"\x19\x01\x54\x19"
TV_APP_FILE = "PPSA99500.ffpkg"
# TVMARK names World.on_mark handles (the preview prints them where a scenario
# needs the world changed).
MARKS = ("stop-backend", "remove-drive", "restore-drive")
# Seconds the fake loader waits before it runs a received store copy, so the
# app's "starting" screen lasts long enough to be captured.
LOADER_EXEC_DELAY = 1.0

# Release routes of the job plan (the seeder's downloads): the fixture's URL
# path is replaced by a range_server route, same file and size.
JOB_ROUTES = {
    "fx-crown-of-ash-ffpfsc": "slow/small.bin",      # paused after it starts
    "fx-velvet-static-ffpfsc": "slow/small.bin",     # cancelled with the partial kept
}

# The Save Data Store of a world (spec "saves"). The IDs, games and sharers
# are those of the preview's fixture (app/tv/host/fixture.cpp), so its
# scenarios open the same saves against either backend.
#   none       every other world: an empty GitHub index and no site, so the
#              store never reaches the network for saves
#   community  seven saves: four in the GitHub index, three on the XenoKing
#              site; two profiles; Ember Hollow, Glass Orchard, Starfall Rally
#              and Echo Valley installed (Echo Valley without a version), Glass
#              Orchard running; the native save engine (the host simulation)
#   garlic     as community, but no native engine, no Garlic Save Manager and
#              no ELF loader: an install fails with garlic_unavailable
#   building   an empty site that answers after SAVES_BUILD_DELAY seconds, so
#              the store's first list (it waits 15 s) says it is still building
SAVES_MODES = ("none", "community", "garlic", "building")
SAVES_ANON = "xeno-tv-preview-anon-key"
SAVES_BUILD_DELAY = 18.0     # past the store's 15 s wait, inside its 20 s request timeout
SAVES_RUNNING = "PPSA90004"
SAVES_PROFILES = ({"id": 0x10000000, "name": "XENO", "foreground": True},
                  {"id": 0x10000001, "name": "Guest", "foreground": False})
SAVES_INSTALLED = {"PPSA90003": "01.050.000", "PPSA90004": "01.000.000", "PPSA90005": "01.020.000",
                   "PPSA90010": ""}
# GitHub index rows; "slow" downloads from range_server's slow/big.bin (64 MiB at
# 1 MiB/s, long enough to show the progress and cancel), the others are real
# decrypted saves the host simulation installs, with "data" kB of save data
# (as real saves: a few dozen kilobytes to a few megabytes).
SAVES_INDEX = (
    {"id": "xk-ember-forge", "titleId": "PPSA90003", "title": "Ember Hollow",
     "name": "Chapter 4 \u2014 every forge unlocked",
     "description": "Saved at the mountain pass with every forge in the village relit and the smith's quests done.",
     "author": "ashwright", "category": "chapter", "gameVersion": "01.050.000", "region": "EUR",
     "createdAt": "2026-10-12T18:20:00Z", "data": 2400},
    {"id": "xk-ember-ngplus", "titleId": "PPSA90003", "title": "Ember Hollow",
     "name": "New game+ with the maxed starter kit",
     "description": "A fresh new game+ start with every tool maxed and 50,000 coins.",
     "author": "lumen", "category": "starter", "gameVersion": "01.060.000", "region": "EUR",
     "createdAt": "2026-10-08T09:00:00Z", "data": 1100},
    {"id": "xk-orchard-bloom", "titleId": "PPSA90004", "title": "Glass Orchard", "name": "Every garden in bloom",
     "description": "100% complete: all 60 gardens grown and every beam path solved.",
     "author": "petalfall", "category": "platinum", "gameVersion": "01.000.000", "region": "USA",
     "createdAt": "2026-10-10T12:00:00Z", "data": 600},
    {"id": "xk-starfall-season", "titleId": "PPSA90005", "title": "Starfall Rally",
     "name": "Season two \u2014 every car unlocked",
     "description": "All 40 cars unlocked and maxed, season two ready to start.",
     "author": "gridline", "category": "starter", "gameVersion": "01.010.000", "region": "USA",
     "createdAt": "2026-10-05T20:00:00Z", "slow": True},
)
# The site's rows (its own columns, and "data" as above); the store names them from
# the notes and lists them as "sb:<id>" (the fixture's IDs). Echo Valley's notes
# are long, with a long first sentence: a long name and long notes in the details.
SAVES_SITE = (
    {"id": "5a1d0000-0000-4000-8000-000000000001", "title_id": "PPSA90001", "game": "Tidewater", "shared_by": "tidesinger",
     "notes": "100% platinum route. Every chart and wreck found, ready for the last trophy.",
     "file_path": "PPSA90001/tidewater-platinum.zip", "created_at": "2026-10-14T08:30:00.000000+00:00",
     "data": 3100},
    {"id": "5a1d0000-0000-4000-8000-000000000002", "title_id": "PPSA90002", "game": "Neon Courier", "shared_by": "nightshift",
     "notes": "Boss rush checkpoint. Saved before the final delivery.",
     "file_path": "PPSA90002/neon-courier-boss-rush.zip", "created_at": "2026-10-13T22:10:00.000000+00:00",
     "data": 800},
    {"id": "5a1d0000-0000-4000-8000-000000000003", "title_id": "PPSA90010", "game": "Echo Valley", "shared_by": "undertow",
     "notes": "Glitched endless night on the lighthouse coast with every lantern in town lit. The moon stays "
              "overhead for good, so the night creatures never leave and the lantern quests can be repeated "
              "for coins. Saved on the pier before the last ferry with the side quests done. Use at your own risk.",
     "file_path": "PPSA90010/echo-valley-endless-night.zip", "created_at": "2026-10-01T10:00:00.000000+00:00",
     "data": 34},
)

_DEPS = {}


class WorldDependencyError(RuntimeError):
    pass


def saves_site_module():
    """tests/host/supabase_mock.py, for the worlds with the XenoKing site."""
    deps()
    try:
        return importlib.import_module("supabase_mock")
    except Exception as e:  # noqa: BLE001
        raise WorldDependencyError(f"the saves worlds need tests/host/supabase_mock.py: "
                                   f"{e.__class__.__name__}: {e}") from e


def deps():
    """tvworld, harness and range_server from tests/host (imported once)."""
    if _DEPS:
        return _DEPS
    for p in (TESTS_HOST, TESTS_TV):
        if p not in sys.path:
            sys.path.insert(0, p)
    for name, why in (("harness", "tests/host/harness.py"), ("range_server", "tests/host/range_server.py"),
                      ("tvworld", "tests/host/tvworld.py (with tests/host/tv/fixture-catalog.json)")):
        try:
            _DEPS[name] = importlib.import_module(name)
        except Exception as e:  # noqa: BLE001
            _DEPS.clear()
            raise WorldDependencyError(f"the preview world needs {why}: {e.__class__.__name__}: {e}") from e
    return _DEPS


def save_data(key, kb):
    """kb kilobytes of save data that does not compress (a zip about that size),
    the same for the same key on every run."""
    return random.Random(key).randbytes(kb * 1000)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def read_version():
    try:
        with open(os.path.join(REPO, "VERSION"), encoding="utf-8") as f:
            return f.read().strip().lstrip("v")
    except OSError:
        return "0.0.0"


def next_version(v):
    parts = [int(x) for x in re.findall(r"\d+", v)[:3]] + [0, 0, 0]
    return f"{parts[0]}.{parts[1]}.{parts[2] + 1}"


def content_version(v):
    """1.2.1 -> 01.002.001 (sce_sys/param.json contentVersion)."""
    parts = [int(x) for x in re.findall(r"\d+", v)[:3]] + [0, 0, 0]
    return f"{parts[0]:02d}.{parts[1]:03d}.{parts[2]:03d}"


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_json(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(value, f, ensure_ascii=False, indent=1)
        f.write("\n")


def sparse_file(path, size, header=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        if header is not None:
            f.write(b"\x7fCNT")
            f.seek(0x40)
            f.write(header)
        f.truncate(size)


def make_tv_app_image(path, size=1 * MiB):
    """A PPSA99500.ffpkg the backend accepts as an image: the UFS2 magic at 65536+1372."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.truncate(size)
        f.seek(UFS2_MAGIC_OFFSET)
        f.write(UFS2_MAGIC)


def make_exit_elf(path, code=3):
    """A minimal static x86-64 ELF that exits at once with `code`: the saved copy
    of the crash-fallback world (an update that dies right after its start)."""
    base = 0x400000
    code_bytes = b"\xb8\x3c\x00\x00\x00" + b"\xbf" + struct.pack("<I", code) + b"\x0f\x05"   # exit(code)
    size = 64 + 56 + len(code_bytes)
    ident = b"\x7fELF\x02\x01\x01" + b"\x00" * 9
    ehdr = struct.pack("<16sHHIQQQIHHHHHH", ident, 2, 62, 1, base + 120, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    phdr = struct.pack("<IIQQQQQQ", 1, 5, 0, base, base, size, size, 0x1000)
    with open(path, "wb") as f:
        f.write(ehdr + phdr + code_bytes)
    os.chmod(path, 0o755)
    return path


def listening(port):
    """True when something listens on TCP `port` (read from /proc/net/tcp{,6}:
    no connection is made, so the backend never sees a probe)."""
    hexport = f"{port:04X}"
    found_table = False
    for table in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            with open(table, encoding="ascii") as f:
                found_table = True
                next(f, None)
                for line in f:
                    parts = line.split()
                    if len(parts) > 3 and parts[3] == "0A" and parts[1].rsplit(":", 1)[-1] == hexport:
                        return True
        except OSError:
            pass
    if found_table:
        return False
    try:   # no procfs: a connect probe
        with socket.create_connection(("127.0.0.1", port), timeout=0.3):
            return True
    except OSError:
        return False


# --------------------------------------------------------------------------- #
# The recording proxy in front of the backend
# --------------------------------------------------------------------------- #

class GatedProxy:
    """127.0.0.1:<port> -> 127.0.0.1:<upstream>, open only while the upstream
    listens. One request per connection (the app's transport). Records
    {t, method, path, status, bytes, ms, error, body (first 300 bytes of a 4xx/5xx)}."""

    def __init__(self, upstream, port=None, log_path=None, poll=0.05):
        self.upstream, self.port = upstream, port or free_port()
        self.log_path, self.poll = log_path, poll
        self.records = []
        self.mu = threading.Lock()
        self.sock = None
        self.stop_evt = threading.Event()
        self.opened = 0

    def start(self):
        threading.Thread(target=self._watch, name="proxy-watch", daemon=True).start()
        return self

    def _watch(self):
        while not self.stop_evt.is_set():
            up = listening(self.upstream)
            with self.mu:
                is_open = self.sock is not None
            if up and not is_open:
                self._open()
            elif not up and is_open:
                self._close_listener()
            time.sleep(self.poll)
        self._close_listener()

    def _open(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind(("127.0.0.1", self.port))
            s.listen(64)
        except OSError:
            s.close()
            return
        with self.mu:
            self.sock = s
            self.opened += 1
        threading.Thread(target=self._accept, args=(s,), name="proxy-accept", daemon=True).start()

    def _close_listener(self):
        with self.mu:
            s, self.sock = self.sock, None
        if s is not None:
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            s.close()

    def wait_open(self, timeout=10.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.mu:
                if self.sock is not None:
                    return True
            time.sleep(0.02)
        return False

    def _accept(self, s):
        while True:
            try:
                conn, _ = s.accept()
            except OSError:
                return
            threading.Thread(target=self._serve, args=(conn,), daemon=True).start()

    def _record(self, rec):
        with self.mu:
            self.records.append(rec)
            if self.log_path:
                with open(self.log_path, "a", encoding="utf-8") as f:
                    f.write(json.dumps(rec, ensure_ascii=False) + "\n")

    def _serve(self, conn):
        t0 = time.time()
        rec = {"t": round(t0, 3), "method": "", "path": "", "status": 0, "bytes": 0, "ms": 0, "error": ""}
        up = None
        try:
            conn.settimeout(60)
            buf = b""
            while b"\r\n\r\n" not in buf and len(buf) < 65536:
                b = conn.recv(65536)
                if not b:
                    break
                buf += b
            end = buf.find(b"\r\n\r\n")
            if end < 0:
                rec["error"] = "incomplete request"
                return
            head, rest = buf[:end].decode("latin-1"), buf[end + 4:]
            lines = head.split("\r\n")
            parts = lines[0].split(" ")
            rec["method"], rec["path"] = parts[0], parts[1] if len(parts) > 1 else ""
            clen = 0
            out = [lines[0]]
            for ln in lines[1:]:
                k = ln.split(":", 1)[0].strip().lower()
                if k == "host":
                    ln = f"Host: 127.0.0.1:{self.upstream}"
                elif k == "content-length":
                    try:
                        clen = int(ln.split(":", 1)[1].strip())
                    except ValueError:
                        clen = 0
                out.append(ln)
            while len(rest) < clen:
                b = conn.recv(65536)
                if not b:
                    break
                rest += b
            try:
                up = socket.create_connection(("127.0.0.1", self.upstream), timeout=5)
            except OSError as e:
                rec["error"] = f"upstream: {e.__class__.__name__}"
                self._close_listener()     # the backend went away: refuse from now on
                return
            up.settimeout(120)
            up.sendall(("\r\n".join(out) + "\r\n\r\n").encode("latin-1") + rest)
            first, body_keep, total = b"", b"", 0
            while True:
                b = up.recv(65536)
                if not b:
                    break
                if len(first) < 4096:
                    first += b[:4096 - len(first)]
                total += len(b)
                conn.sendall(b)
            m = re.match(rb"HTTP/1\.[01] (\d{3})", first)
            rec["status"] = int(m.group(1)) if m else 0
            rec["bytes"] = total
            if rec["status"] >= 400:
                i = first.find(b"\r\n\r\n")
                body_keep = first[i + 4:i + 304] if i >= 0 else b""
                rec["body"] = body_keep.decode("utf-8", "replace")
        except OSError as e:
            rec["error"] = rec["error"] or f"{e.__class__.__name__}: {e}"
        finally:
            rec["ms"] = int((time.time() - t0) * 1000)
            for s in (up, conn):
                if s is not None:
                    try:
                        s.close()
                    except OSError:
                        pass
            self._record(rec)

    def since(self, t0, t1=None):
        with self.mu:
            return [r for r in self.records if r["t"] >= t0 and (t1 is None or r["t"] < t1)]

    def stop(self):
        self.stop_evt.set()
        self._close_listener()


# --------------------------------------------------------------------------- #
# Feed server (store and TV app updates, catalogue revision, GitHub stand-in)
# --------------------------------------------------------------------------- #

class FeedServer:
    def __init__(self, workdir, host_binary, version):
        self.dir = workdir
        self.host_binary = host_binary
        self.version = version
        self.next = next_version(version)
        self.revision_delay = 2.0
        self.revision = FIXTURE_REVISION
        self.requests = []
        self.mu = threading.Lock()
        self.httpd = None
        self.files = {}

    def start(self):
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, fmt, *args):
                pass

            def do_GET(self):  # noqa: N802
                owner._serve(self)

            def do_HEAD(self):  # noqa: N802
                owner._serve(self, head=True)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        threading.Thread(target=self.httpd.serve_forever, name="feed-server", daemon=True).start()
        self._build()
        return self

    @property
    def base(self):
        return f"http://127.0.0.1:{self.httpd.server_address[1]}"

    @property
    def payloads_url(self):
        return self.base + "/updates/payloads.json"

    def _build(self):
        os.makedirs(self.dir, exist_ok=True)
        img = os.path.join(self.dir, TV_APP_FILE)
        make_tv_app_image(img, 2 * MiB)
        self.tv_app_image = img
        elf_sha = sha256_file(self.host_binary)
        self.update_sha = elf_sha
        tv_url = f"{self.base}/releases/download/v{self.next}/{TV_APP_FILE}"
        self.files = {
            "/updates/payloads.json": ("application/json", json.dumps({"payloads": [{
                "filename": "xeno-store.elf", "version": self.next, "url": self.base + "/files/xeno-store.elf",
                "sha256": elf_sha, "size": os.path.getsize(self.host_binary),
                "notes": "Live preview fixture: the host binary served as the next version."}]}).encode()),
            "/updates/tv-app.json": ("application/json", json.dumps({
                "version": self.next, "filename": TV_APP_FILE, "url": tv_url, "sha256": sha256_file(img),
                "size": os.path.getsize(img)}).encode()),
            "/files/xeno-store.elf": ("application/octet-stream", self.host_binary),
            f"/releases/download/v{self.next}/{TV_APP_FILE}": ("application/octet-stream", img),
        }

    def hits(self, prefix=""):
        with self.mu:
            return [p for p in self.requests if p.startswith(prefix)]

    def add_world(self, name, files):
        """Serves a world's files ({relative path: (content type, bytes)}) under /w/<name>/."""
        prefix = f"/w/{name}/"
        with self.mu:
            for rel, entry in files.items():
                self.files[prefix + rel] = entry

    def drop_world(self, name):
        prefix = f"/w/{name}/"
        with self.mu:
            for path in [p for p in self.files if p.startswith(prefix)]:
                del self.files[path]

    def _serve(self, h, head=False):
        path = h.path.split("?", 1)[0]
        with self.mu:
            self.requests.append(path)
            entry = self.files.get(path)
        if path == "/catalog/revision.json":
            time.sleep(self.revision_delay)
            body, ctype, code = json.dumps({"revision": self.revision}).encode(), "application/json", 200
        elif entry is not None:
            ctype, src = entry
            if isinstance(src, bytes):
                body, code = src, 200
            else:
                with open(src, "rb") as f:
                    body, code = f.read(), 200
        else:
            body, ctype, code = b'{"message":"Not Found"}', "application/json", 404
        try:
            h.send_response(code)
            h.send_header("Content-Type", ctype)
            h.send_header("Content-Length", str(len(body)))
            h.send_header("Connection", "close")
            h.end_headers()
            if not head:
                h.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass
        h.close_connection = True

    def stop(self):
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()
            self.httpd = None


# --------------------------------------------------------------------------- #
# Servers shared by the run
# --------------------------------------------------------------------------- #

class RunServers:
    def __init__(self, host_binary, workdir=None, keep=False):
        self.host_binary = os.path.abspath(host_binary)
        self.keep = keep
        self.workdir = workdir or tempfile.mkdtemp(prefix="xeno-tv-live.")
        self.version = read_version()
        self.files = self.art = self.feed = None

    def start(self):
        d = deps()
        fixture = d["tvworld"].load_fixture_catalog()
        self.titles = art_server.titles_from_catalog(fixture)
        self.files = d["harness"].RangeServerProcess(os.path.join(self.workdir, "files")).start(timeout=120)
        self.art = art_server.ArtServer(self.titles).start()
        self.feed = FeedServer(os.path.join(self.workdir, "feed"), self.host_binary, self.version).start()
        return self

    def reset_counters(self):
        if self.art:
            self.art.reset()
        try:
            urllib.request.urlopen(self.files.base + "/ctl/reset", timeout=5).read()
        except OSError:
            pass

    def close(self):
        for s in (self.art, self.feed):
            if s:
                s.stop()
        if self.files:
            self.files.kill()
        if not self.keep:
            shutil.rmtree(self.workdir, ignore_errors=True)


# --------------------------------------------------------------------------- #
# The backend process
# --------------------------------------------------------------------------- #

class Backend:
    def __init__(self, binary, port, data, roots, env, log_path):
        self.binary, self.port, self.data, self.roots = binary, port, data, list(roots)
        self.env, self.log_path = env, log_path
        self.proc = None
        self.logf = None

    def argv(self):
        a = [self.binary, "--port", str(self.port), "--data", self.data, "--no-browser", "--log-level", "debug"]
        for r in self.roots:
            a += ["--root", r]
        return a

    def start(self, timeout=20.0):
        self.logf = open(self.log_path, "ab")
        self.proc = subprocess.Popen(self.argv(), stdout=self.logf, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, env=self.env, start_new_session=True)
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"xeno-store exited at start with {self.proc.returncode}; see {self.log_path}")
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{self.port}/api/status", timeout=2) as r:
                    if r.status == 200:
                        return self
            except OSError:
                pass
            time.sleep(0.1)
        raise RuntimeError(f"xeno-store did not answer on :{self.port}; see {self.log_path}")

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def stop(self, kill=False, timeout=15.0):
        if self.proc is None:
            return None
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL if kill else signal.SIGTERM)
            try:
                self.proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        rc = self.proc.returncode
        if self.logf:
            self.logf.close()
            self.logf = None
        return rc


# --------------------------------------------------------------------------- #
# One world
# --------------------------------------------------------------------------- #

class World:
    """One scenario group's world. spec: the group's "world" member merged over
    scenarios.json "worldDefaults". Nothing runs before start()."""

    def __init__(self, run, name, spec, logs_dir, keep=False):
        self.run, self.name, self.spec = run, name, dict(spec)
        self.logs = logs_dir
        self.keep = keep
        self.tmp = tempfile.mkdtemp(prefix=f"xeno-tv-{re.sub(r'[^A-Za-z0-9_-]', '_', name)}.")
        self.data = os.path.join(self.tmp, "data")
        self.roots = [os.path.join(self.tmp, "R1"), os.path.join(self.tmp, "R2")]
        self.app0 = os.path.join(self.tmp, "app0", "xeno")
        self.port = free_port()                 # the backend's own port (B)
        self.backend = None
        self.loader = self.sentinel = None
        self.loader_port = None
        self.proxy = None
        self.catalog = self.featured = None
        self.marks = []
        self.retry_keeper = None
        self.instances = 0
        self.tag = re.sub(r"[^A-Za-z0-9_-]", "_", name)
        self.site = None                        # the XenoKing site's stand-in (saves worlds)
        self.saves_base = None                  # this world's files on the feed server
        self.closed_port = None                 # nobody listens: Garlic and the loader of "garlic"
        os.makedirs(self.logs, exist_ok=True)

    # ---- paths and environment -------------------------------------------
    @property
    def bundled_elf(self):
        return os.path.join(self.app0, "xeno-store.elf")

    @property
    def bundled_version(self):
        return os.path.join(self.app0, "version.txt")

    @property
    def saved_elf(self):
        if self.spec.get("saved") == "crash":
            return os.path.join(self.tmp, "saved", "xeno-store.elf")
        return os.path.join(self.data, "xeno-store.elf")       # where the store's updater saves

    @property
    def saved_version(self):
        return self.saved_elf + ".version"

    @property
    def saves_mode(self):
        mode = self.spec.get("saves") or "none"
        if mode not in SAVES_MODES:
            raise ValueError(f"world {self.name}: saves must be one of {', '.join(SAVES_MODES)}, not {mode!r}")
        return mode

    def backend_env(self):
        env = dict(os.environ)
        for k in list(env):
            if k.lower() in ("https_proxy", "http_proxy", "all_proxy", "no_proxy") or k.startswith("XENO_"):
                env.pop(k)
        feed = self.run.feed.base
        env.update({
            "XENO_HOST_ART_FIXTURE": self.run.art.hostport,
            "XENO_HOST_RESOLVE_HOST": "127.0.0.1",
            "XENO_APP_BASE": feed + "/releases/download/",
            "XENO_GITHUB_API_BASE": feed + "/github",
            "XENO_PORT_LOADER": str(self.sentinel_port),
            "XENO_SAVES_INDEX_BASE": self.saves_base,
        })
        if self.spec.get("updates", True):
            env["XENO_TVAPI_UPDATES"] = "1"
        mode = self.saves_mode
        if mode in ("community", "garlic"):
            env["XENO_HOST_RUNNING_TITLE"] = SAVES_RUNNING
        if mode == "garlic":
            # No native engine, so an install needs Garlic Save Manager: it is not
            # running and cannot be started, as nothing answers on the loader's
            # port either (the sentinel is not asked, so it still sees nothing).
            env.update({"XENO_SIM_NATIVE": "0", "XENO_PORT_GARLIC": str(self.closed_port),
                        "XENO_PORT_LOADER": str(self.closed_port)})
        return env

    def preview_env(self, lang, scenarios, direct=False):
        env = dict(os.environ)
        for k in list(env):
            if k.startswith(("XENO_", "ORBIT_")) and not k.startswith("XENO_STUB_"):   # stub_preview test switches stay
                env.pop(k)
        env.update({
            "XENO_BACKEND": f"127.0.0.1:{self.port if direct else self.proxy.port}",
            "XENO_TODAY": self.run_today,
            "XENO_LANG": lang,
            "XENO_APP_VERSION": content_version(self.run.version),
            "XENO_SCENARIOS": ",".join(scenarios),
            "XENO_STARTER_BUNDLED_ELF": self.bundled_elf,
            "XENO_STARTER_BUNDLED_VERSION": self.bundled_version,
            "XENO_STARTER_SAVED_ELF": self.saved_elf,
            "XENO_STARTER_SAVED_VERSION": self.saved_version,
            "EGL_PLATFORM": "surfaceless", "LIBGL_ALWAYS_SOFTWARE": "1", "GALLIUM_DRIVER": "llvmpipe",
        })
        if self.loader_port:
            env["XENO_LOADER_PORT"] = str(self.loader_port)
        return env

    run_today = "2026-10-15"

    # ---- building -------------------------------------------------------
    def _catalog(self):
        tvw = deps()["tvworld"]
        cat = tvw.load_fixture_catalog(self.run.files.base, self.run.art.base)
        routes = dict(JOB_ROUTES)
        routes.update(self.spec.get("releaseRoutes") or {})
        prefix = self.run.files.base + "/"
        for g in cat.get("games", []):
            for r in g.get("releases", []):
                rid, url = r.get("id"), r.get("url", "")
                if rid in routes and url.startswith(prefix):
                    r["url"] = prefix + routes[rid]
        return cat

    def _settings(self):
        tvw = deps()["tvworld"]
        feed = self.run.feed
        return {
            "sources": {"enabled": {k: True for k in tvw.EXPOSED_SOURCES},
                        "acknowledged": bool(self.spec.get("acknowledged", True))},
            "catalog": {"autoRefresh": False, "revisionUrl": feed.base + "/catalog/revision.json",
                        "url": feed.base + "/catalog/catalog.json",
                        "savesBackendUrl": self.saves_base + "/saves-backend.json"},
            "updates": {"autoCheck": False, "feed": feed.payloads_url},
            "downloads": {"connections": 1, "concurrentJobs": int(self.spec.get("concurrentDownloads", 2))},
        }

    def _library(self):
        tvw = deps()["tvworld"]
        for where, rel, size, header in tvw.LIBRARY_FILES:
            if where == "user":
                # the host build reads /user/app from <data>/user/app (storage.c)
                os.makedirs(os.path.join(self.data, "user", "app", rel), exist_ok=True)
            elif isinstance(where, int) and where < len(self.roots):
                sparse_file(os.path.join(self.roots[where], rel), size, header)

    def _tv_app(self):
        state = self.spec.get("tvApp", "none")
        if state not in ("orbit", "manual"):
            return
        img = os.path.join(self.data, "sim", "data", "homebrew", TV_APP_FILE)
        make_tv_app_image(img, 2 * MiB)
        if state == "orbit":
            st = os.stat(img)
            write_json(os.path.join(self.data, "tv-app.json"), {
                "version": self.run.version, "checksum": sha256_file(img), "size": st.st_size,
                "modified": int(st.st_mtime), "inode": st.st_ino})

    def _starter_files(self):
        os.makedirs(self.app0, exist_ok=True)
        try:
            os.symlink(self.run.host_binary, self.bundled_elf)
        except OSError:
            shutil.copy2(self.run.host_binary, self.bundled_elf)
        with open(self.bundled_version, "w", encoding="ascii") as f:
            f.write(self.run.version + "\n")
        if self.spec.get("saved") == "crash":
            os.makedirs(os.path.dirname(self.saved_elf), exist_ok=True)
            make_exit_elf(self.saved_elf, 3)
            with open(self.saved_version, "w", encoding="ascii") as f:
                f.write("9.9.9")          # newer than the bundled copy: the starter tries it first

    def _saves(self):
        """The Save Data Store of the world's "saves" mode (SAVES_MODES): its
        GitHub index and published saves-backend.json on the feed server, the
        site, and the console's profiles, installed games and a save to back up."""
        mode = self.saves_mode
        tvw = deps()["tvworld"]
        self.saves_base = f"{self.run.feed.base}/w/{self.tag}"
        files, index = {}, []
        if mode in ("community", "garlic"):
            for row in SAVES_INDEX:
                r = {k: v for k, v in row.items() if k not in ("slow", "data")}
                r.update(dirName="SAVEDATA00", status="published", format="xeno-save-zip")
                if row.get("slow"):
                    r.update(url=self.run.files.base + "/slow/big.bin", size=64 * MiB)
                else:
                    z = tvw.save_zip(row["titleId"], row["title"], save_data(row["id"], row["data"]))
                    rel = f"saves/{row['id']}.zip"
                    files[rel] = ("application/zip", z)
                    r.update(url=f"{self.saves_base}/{rel}", size=len(z), sha256=hashlib.sha256(z).hexdigest())
                index.append(r)
        files["saves.json"] = ("application/json", json.dumps({"schemaVersion": 1, "saves": index}).encode())
        if mode != "none":
            self.site = saves_site_module().SupabaseMock(SAVES_ANON).start()
            if mode == "building":
                self.site.list_delay = SAVES_BUILD_DELAY
            else:
                for row in SAVES_SITE:
                    z = tvw.save_zip(row["title_id"], row["game"], save_data(row["file_path"], row["data"]))
                    self.site.put_object(row["file_path"], z)
                    self.site.add_row(file_name=f"{row['title_id']}_{row['game'].replace(' ', '_')}.zip",
                                      file_size=len(z), **{k: v for k, v in row.items() if k != "data"})
            files["saves-backend.json"] = ("application/json", json.dumps({
                "kind": "supabase", "name": "XenoKing", "url": self.site.base, "anonKey": SAVES_ANON,
                "bucket": "ps5-saves", "garlicFunction": "garlic-savedb"}).encode())
        self.run.feed.add_world(self.tag, files)
        if mode in ("community", "garlic"):
            sim = os.path.join(self.data, "sim")
            write_json(os.path.join(sim, "users.json"), {"users": list(SAVES_PROFILES)})
            for tid, version in SAVES_INSTALLED.items():
                os.makedirs(os.path.join(self.data, "user", "app", tid), exist_ok=True)
                if version:
                    write_json(os.path.join(sim, "system_data", "priv", "appmeta", tid, "param.json"),
                               {"contentVersion": version})
            # The second profile's own Ember Hollow save: an install for it backs this up first.
            home = os.path.join(sim, "user", "home", "%x" % SAVES_PROFILES[1]["id"], "savedata_prospero", "PPSA90003")
            os.makedirs(os.path.join(home, "sce_sys", "SAVEDATA00"), exist_ok=True)
            with open(os.path.join(home, "sdimg_SAVEDATA00"), "wb") as f:
                f.write(b"IMG" + b"Z" * 4096)
            with open(os.path.join(home, "sce_sys", "SAVEDATA00", "param.sfo"), "wb") as f:
                f.write(tvw.save_sfo("PPSA90003", "Ember Hollow", 0x1122334455667788))

    def build(self):
        os.makedirs(self.data, exist_ok=True)
        for r in self.roots:
            os.makedirs(r, exist_ok=True)
        self._saves()
        self.catalog = self._catalog()
        write_json(os.path.join(self.data, "catalog.json"), self.catalog)
        self.featured = deps()["tvworld"].load_fixture_featured()
        write_json(os.path.join(self.data, "feeds", "featured.json"), self.featured)
        write_json(os.path.join(self.data, "settings.json"), self._settings())
        if self.spec.get("library", True):
            self._library()
        self._tv_app()
        self._starter_files()
        return self

    # ---- running ---------------------------------------------------------
    def start(self):
        """Builds, then starts the sentinel, the proxy, the fake loader and the
        backend as the spec says. Returns self."""
        self.closed_port = free_port()
        self.build()
        self.sentinel_port = free_port()
        self.sentinel = self._spawn_loader("sentinel", self.sentinel_port, record_only=True)
        self.proxy = GatedProxy(self.port, log_path=os.path.join(self.logs, "requests.jsonl")).start()
        loader = self.spec.get("loader", "none")
        if loader == "fake":
            self.loader_port = free_port()
            self.loader = self._spawn_loader("loader", self.loader_port)
        elif loader == "closed":
            self.loader_port = free_port()      # nobody listens there: the starter's connect is refused
        if self.spec.get("backend", "up") == "up":
            self.start_backend()
            if not self.proxy.wait_open(10):
                raise RuntimeError("the recording proxy did not open")
        return self

    def _spawn_loader(self, label, port, record_only=False):
        script = os.path.join(HERE, "fake_loader.py")
        argv = [sys.executable, "-I", script, "--port", str(port), "--log", os.path.join(self.logs, f"{label}.jsonl"),
                "--child-log-dir", os.path.join(self.logs, f"{label}-children")]
        if record_only:
            argv.append("--record-only")
        else:
            argv += ["--host-binary", self.run.host_binary, "--backend-port", str(self.port), "--data", self.data,
                     "--delay", str(self.spec.get("loaderDelay", LOADER_EXEC_DELAY))]
            if self.spec.get("saved") == "crash":
                argv += ["--plain", self.saved_elf]       # runs with no arguments and exits at once
            for r in self.roots:
                argv += ["--root", r]
        out = open(os.path.join(self.logs, f"{label}.out"), "ab")
        p = subprocess.Popen(argv, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                             env=self.backend_env() if not record_only else dict(os.environ))
        p._xeno_out = out
        deadline = time.time() + 15
        while time.time() < deadline:
            if listening(port):
                return p
            if p.poll() is not None:
                raise RuntimeError(f"fake_loader ({label}) exited with {p.returncode}")
            time.sleep(0.05)
        raise RuntimeError(f"fake_loader ({label}) did not listen on {port}")

    def start_backend(self):
        self.instances += 1
        self.backend = Backend(self.run.host_binary, self.port, self.data, self.roots, self.backend_env(),
                               os.path.join(self.logs, f"backend-{self.instances}.out"))
        self.backend.start()
        return self.backend

    def stop_backend(self, kill=False):
        """Stops whichever backend serves B: ours, or the one a fake loader started."""
        rc = None
        if self.backend and self.backend.alive():
            rc = self.backend.stop(kill=kill)
        for pid in self.loader_child_pids():
            try:
                os.kill(pid, signal.SIGKILL if kill else signal.SIGTERM)
            except OSError:
                pass
        deadline = time.time() + 15
        while listening(self.port) and time.time() < deadline:
            time.sleep(0.05)
        return rc

    def loader_events(self, label="loader"):
        return fake_loader.read_events(os.path.join(self.logs, f"{label}.jsonl"))

    def loader_child_pids(self):
        started, exited = {}, set()
        for ev in self.loader_events():
            if ev.get("event") == "started" and ev.get("kind") == "host":
                started[ev["pid"]] = ev
            elif ev.get("event") == "exited":
                exited.add(ev.get("pid"))
        return [pid for pid in started if pid not in exited]

    # ---- marks (TVMARK lines of the preview) ----------------------------
    def on_mark(self, mark):
        """Changes the world where a scenario asks for it. Returns a short note."""
        self.marks.append((time.time(), mark))
        if mark == "stop-backend":
            self.stop_backend(kill=True)
            return "backend killed"
        if mark == "remove-drive":
            r2 = self.roots[1]
            if os.path.isdir(r2):
                os.rename(r2, r2 + ".removed")
            return "R2 removed"
        if mark == "restore-drive":
            r2 = self.roots[1]
            if os.path.isdir(r2 + ".removed"):
                os.rename(r2 + ".removed", r2)
            return "R2 back"
        raise KeyError(mark)

    # ---- logs ------------------------------------------------------------
    def backend_log(self):
        """Every instance's log: <data>/log.1.txt + log.txt (the store's own log)."""
        out = []
        for n in ("log.1.txt", "log.txt"):
            try:
                with open(os.path.join(self.data, n), "rb") as f:
                    out.append(f.read().decode("utf-8", "replace"))
            except OSError:
                pass
        return "".join(out)

    def close(self):
        if self.retry_keeper:
            self.retry_keeper.stop()
        q = os.path.join(self.data, "queue.json")
        if os.path.isdir(q):                    # the blocked-queue world: give the file back
            shutil.rmtree(q, ignore_errors=True)
        try:
            self.stop_backend()
        except Exception:  # noqa: BLE001
            pass
        for p in (self.loader, self.sentinel):
            if p is not None and p.poll() is None:
                p.send_signal(signal.SIGTERM)
                try:
                    p.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait()
            if p is not None:
                p._xeno_out.close()
        if self.proxy:
            self.proxy.stop()
        if self.site:
            self.site.stop()
            self.site = None
        self.run.feed.drop_world(self.tag)
        for n in ("log.1.txt", "log.txt"):
            src = os.path.join(self.data, n)
            if os.path.exists(src):
                shutil.copy2(src, os.path.join(self.logs, "store-" + n))
        if self.keep:
            print(f"world {self.name}: kept {self.tmp}", file=sys.stderr)
        else:
            shutil.rmtree(self.tmp, ignore_errors=True)


class RetryKeeper:
    """Keeps a range_server r429 route answering 429: whenever the route was hit,
    /ctl/reset re-arms its first-request flag, so the job waits again (5, 15,
    45 s ... of backoff) and its row stays "retrying" until the keeper stops;
    then the next attempt succeeds (PORT-SPEC 4.1: "503 then OK")."""

    def __init__(self, files_base, route):
        self.base, self.route = files_base, "/" + route.lstrip("/")
        self.stop_evt = threading.Event()
        self.resets = 0

    def start(self):
        threading.Thread(target=self._loop, name="retry-keeper", daemon=True).start()
        return self

    def _loop(self):
        while not self.stop_evt.wait(0.15):
            try:
                with urllib.request.urlopen(self.base + "/ctl/stats", timeout=3) as r:
                    st = json.loads(r.read())
                if st.get("requests", {}).get(self.route, 0) > 0:
                    urllib.request.urlopen(self.base + "/ctl/reset", timeout=3).read()
                    self.resets += 1
            except OSError:
                pass

    def stop(self):
        self.stop_evt.set()
