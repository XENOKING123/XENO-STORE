#!/usr/bin/env python3
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
tvworld.py — the shared test world of the /api/v1 suites (PORT-SPEC 1.10, 4.1).

One World is one host backend (build/host/xeno-store, through harness.XenoServer)
in a temp directory, with the TV fixture catalogue, the featured feed, settings,
two extra roots and local file and art servers. TvClient talks to it exactly like
the TV app: one request per connection, `Host: 127.0.0.1:<port>`, no Origin, a
Bearer token from GET /api/v1/session, `Content-Type: application/json` on POSTs,
and every reply parsed with tvcontract.strict_json.

Typical test class (one backend per class):

    import os, sys, unittest
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import tvcontract as tc, tvworld

    class TvFavorites(unittest.TestCase):
        @classmethod
        def setUpClass(cls):
            cls.w = tvworld.World("favorites").start()      # fixture catalogue, acknowledged
            cls.c = cls.w.client()                          # paired TvClient (Bearer set)

        @classmethod
        def tearDownClass(cls):
            cls.w.close()

        def test_set_twice(self):
            r = self.c.post("/api/v1/favorites", {"gameId": "fx-tidewater-ps5", "favorite": True})
            body = r.check()                                # status 200, transport, strict JSON, contract
            self.assertEqual(body, ["fx-tidewater-ps5"])
            r = self.c.post("/api/v1/favorites", {"gameId": "nope", "favorite": True})
            r.error(404, "Game is not in this catalogue.")  # sentence envelope
            self.w.assert_golden("favorites", body)         # tests/host/tv/golden/favorites.json

World(name, **options)  (nothing runs until start(); close() undoes everything)
  catalog="fixture"     the fixture with {{FILES}}/{{ART}} filled; "real" = the binary's embedded
                        revision-10 catalogue (no <data>/catalog.json); or a dict (placeholders filled)
  featured="fixture"    <data>/feeds/featured.json: "fixture", "real" (updates/featured.json), None, or a dict
  acknowledged=True     settings sources.acknowledged
  enabled=None          exposed sources on (None = all); a list of ids, or a map {id: bool}
  settings=None         extra settings.json members, deep-merged (e.g. {"pairing": {"pin": "1234"}})
  roots=("R1", "R2")    extra --root dirs under the temp dir (R1 "USB", R2)
  library=False         True seeds LIBRARY_FILES into the roots and <data>/user/app (PORT-SPEC 4.1)
  files="shared"        range_server.py for {{FILES}}: "shared" (one child process per test run),
                        "own" (a child process of this World, safe to kill), or None
  art=True              an artgen.ArtServer for {{ART}}; also sets XENO_HOST_ART_FIXTURE. Without one,
                        {{ART}} is a closed port (NO_ART) that XENO_HOST_ART_FIXTURE still names
                        (except for "real"), so the art URLs pass the URL policy as with a server
                        (screenshotCount counts them) and only their fetches fail
  show_zip=False        XENO_TV_SHOW_ZIP=1 (and golden_catalog() follows it)
  tvapi_updates=False   XENO_TVAPI_UPDATES=1
  env=None              more environment for the backend (os.environ is passed through by harness)
  sync_zip_sizes=True   ZIP releases served from {{FILES}} get the real file size
  port=None             fixed port (default: a free one)
  saves=None            the Save Data Store's GitHub index, served by a local SavesIndex
                        (XENO_SAVES_INDEX_BASE): a list of index rows; a row's "zip" (bytes) is
                        served and fills its url, size and sha256. The published saves-backend.json
                        answers 404, so no site is listed and nothing reaches the network

World members
  w.port, w.base, w.tmp, w.data, w.roots (extra roots), w.all_roots ([default root] + roots)
  w.srv (harness.XenoServer: w.srv.api("GET", "/api/queue") for core routes), w.files, w.art,
  w.saves_index (the SavesIndex of saves=, or None)
  w.catalog, w.featured  the exact dicts written (w.catalog is the parsed real one for "real")
  w.client(paired=True) -> TvClient;  w.token (the first pair())
  w.golden_catalog(enabled=..., acknowledged=...) / w.golden_sources(...)   tv_golden on w.catalog
  w.set_stopping(True|False)   the XENO_TVAPI_STOPPING_FILE host hook (stopping without stopping)
  w.restart(), w.stop(), w.close(), w.log_text(), w.wait_log(regex, timeout)
  w.normalizer(**rules) -> Normalizer with this world's roots;  w.assert_golden(name, value)
  w.release(rid) / w.game(gid)  fixture lookups;  w.file_url(name, modes="ok")

Subclass hooks (kept stable): World._servers() starts the file and art servers (it reads
  self.opt_files / self.opt_art and sets self.files / self.art); World._build_catalog() and
  World._settings_doc() return what start() writes. _deep_merge(dst, src) merges settings dicts.

TvClient(port, token=None)
  c.get(path, **kw) / c.post(path, body=None, **kw) / c.request(method, path, body, ...)
      body: dict/list (sent compact, UTF-8), str or bytes (sent as is), None (empty POST body)
      kw: token= (override; "" = none), headers=[(name, value)], content_type= (None = omit),
          host= (Host header), timeout=
  c.pair() -> token (GET /api/v1/session; also stored in c.token)
  c.poll(path, until=lambda reply: ..., timeout=10, every=0.1) -> the first reply that satisfies until

Reply
  r.status, r.headers (lower-case names), r.body (bytes), r.elapsed (seconds), r.route (contract key)
  r.json            strict_json(body) (StrictJSONError is an AssertionError)
  r.check(route=None, status=None, profile="xeno")
                    expected success status (tvcontract.SUCCESS_STATUS), transport rules
                    (Content-Length == len(body), Connection: close, JSON content type),
                    strict JSON and the field table; returns the parsed value
  r.error(status=None, sentence=None)    assert_error_envelope plus transport rules
  r.image(kind=None)                     a 200 art reply: sniffed bytes, Content-Type, no-store
  r.transport_errors() -> list[str]

SavesIndex()   a local HTTP server: s.add(path, body, content_type), s.base, s.requests, s.stop()
save_sfo(title_id, title, account=0, dirname="SAVEDATA00")   a save's param.sfo
save_zip(title_id, title, data)   a decrypted community save (sce_sys/param.sfo + savedata.bin)

Normalizer(roots=(), data=None, tmp=None, version=None, keys=None, paths=None)   (PORT-SPEC 1.10)
  instanceId <hex32>; token <hex64>; pairCode <code6>; checkedAt, updatedAt, checkAfter, retryAt,
  automaticCheckAfter, refreshAfter <unix> (0 stays 0); freeBytes, totalBytes, usedBytes <bytes>;
  projectedFreeBytes <bytes> after checking freeBytes - pendingBytes - 240 MiB; absolute temp paths
  inside any string <root:N> (N = index in roots), then <data> and <tmp>; httpPort <port>; job ids j_xxxxxx
  <job:N> in first-seen order (stable for one Normalizer); requestId kept; address <ipv4> unless "";
  sourceKey and drives[]/destinations[] ids <hex64>; top-level version/runningVersion/savedVersion/
  installedVersion/latestVersion equal to the running version <version>.
  keys={"speed": "<speed>"} adds key rules (a string placeholder, or a function value -> value);
  paths={"games[].sizeBytes": "<bytes>"} adds rules on a key path (indices written as []).

Golden files (tests/host/tv/golden/<name>.json, pretty JSON, key order kept)
  assert_golden(name, value, normalizer=None)   compares the normalised value byte for byte;
  XENO_TV_GOLDEN_UPDATE=1 rewrites the file instead (review the diff like code).
  golden/catalog.json and golden/sources.json are written from tv_golden for the default fixture
  World (acknowledged, every source on); test_tvapi_infra.py keeps them equal to tv_golden.

Fixture helpers
  load_fixture_catalog(files_base, art_base) / load_fixture_featured()   placeholders filled
  FIXTURE_CATALOG, FIXTURE_FEATURED (paths), EXPOSED_SOURCES, LIBRARY_FILES

Environment: XENO_BIN (the binary, as harness), XENO_TV_KEEP=1 keeps temp dirs after close().
Python 3 standard library (+ PIL through artgen); run under python3 -I.
"""
import atexit
import copy
import difflib
import hashlib
import http.server
import json
import io
import os
import re
import shutil
import socket
import struct
import sys
import tempfile
import threading
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
TV_DIR = os.path.join(HERE, "tv")
for _p in (HERE, TV_DIR):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import artgen  # noqa: E402
import harness  # noqa: E402
import range_server  # noqa: E402
import tv_golden  # noqa: E402
import tvcontract  # noqa: E402

__all__ = ["World", "TvClient", "Reply", "Normalizer", "SavesIndex", "save_sfo", "save_zip", "assert_golden", "golden_path", "golden_update_mode",
           "load_fixture_catalog", "load_fixture_featured", "fill_placeholders", "shared_files",
           "FIXTURE_CATALOG", "FIXTURE_FEATURED", "GOLDEN_DIR", "EXPOSED_SOURCES", "LIBRARY_FILES",
           "USER_AGENT", "MiB", "RESERVE_GAP"]

PROJECT = os.path.dirname(os.path.dirname(HERE))
FIXTURE_CATALOG = os.path.join(TV_DIR, "fixture-catalog.json")
FIXTURE_FEATURED = os.path.join(TV_DIR, "fixture-featured.json")
GOLDEN_DIR = os.path.join(TV_DIR, "golden")
USER_AGENT = "XenoStoreTV"                        # PORT-SPEC 2.1 (APPO/http.cpp:148 renamed)
MiB = 1024 * 1024
RESERVE_GAP = 240 * MiB
EXPOSED_SOURCES = ("archive", "vikingfile")       # R1: direct/browser kinds, not pkgzone
FILES_TOKEN, ART_TOKEN = "{{FILES}}", "{{ART}}"
NO_ART = "127.0.0.1:9"                            # {{ART}} of a world without an art server (closed)

# PORT-SPEC 4.1 / map-library §12: (root index in World.roots or "user", relative path, size, header)
# Sizes are sparse. Root index 0 is R1, 1 is R2. "user" is <data>/user/app (installed titles).
LIBRARY_FILES = (
    (0, "homebrew/PPSA01234.exfat", 8 * MiB, None),                         # = fx-harbor-lights-exfat
    (0, "xeno-store/pkg/CUSA00001.pkg", 2 * MiB, b"UP0001-CUSA00001_00-XENOTESTLIB00001"),
    (0, "xeno-store/pkg/Lantern Rite.pkg", 2 * MiB, None),                  # catalogue filename, no id
    (0, "homebrew/PPSA02222.exfat.xpart", 1 * MiB, None),                   # a partial: never listed
    (0, "homebrew/game.exfat", 1 * MiB, None),                              # no title id: skipped
    (0, "homebrew/PPSA99500.ffpkg", 1 * MiB, None),                         # our own app: hidden
    (1, "homebrew/PPSA90013.ffpfsc", 8 * MiB, None),                        # = Aurora Drift, on R2
    ("user", "PPSA05555", None, None),                                      # installed-only title
)


# --------------------------------------------------------------------------- #
# Fixture loading
# --------------------------------------------------------------------------- #

def _load(path):
    with open(path, "rb") as f:
        return json.loads(f.read().decode("utf-8"))


def fill_placeholders(value, files_base, art_base):
    """Deep copy of `value` with {{FILES}} and {{ART}} replaced in every string."""
    if isinstance(value, str):
        return value.replace(FILES_TOKEN, files_base.rstrip("/")).replace(ART_TOKEN, art_base.rstrip("/"))
    if isinstance(value, list):
        return [fill_placeholders(v, files_base, art_base) for v in value]
    if isinstance(value, dict):
        return {k: fill_placeholders(v, files_base, art_base) for k, v in value.items()}
    return value


def load_fixture_catalog(files_base="http://127.0.0.1:9", art_base="http://127.0.0.1:9"):
    return fill_placeholders(_load(FIXTURE_CATALOG), files_base, art_base)


def load_fixture_featured():
    return _load(FIXTURE_FEATURED)


# --------------------------------------------------------------------------- #
# The app's HTTP client
# --------------------------------------------------------------------------- #

class Reply:
    """One HTTP reply, read the way APPO/http.cpp reads it (until the server closes)."""

    def __init__(self, method, path, status, headers, body, elapsed, raw_head=""):
        self.method, self.path = method, path
        self.status, self.headers, self.body, self.elapsed = status, headers, body, elapsed
        self.raw_head = raw_head
        self._json = None
        self._parsed = False
        try:
            self.route = tvcontract.route_key(method, path) if path.startswith("/api/v1/") else None
        except KeyError:
            self.route = None

    def __repr__(self):
        return f"<Reply {self.method} {self.path} {self.status} {len(self.body)} bytes {self.body[:160]!r}>"

    @property
    def json(self):
        if not self._parsed:
            self._json = tvcontract.strict_json(self.body)
            self._parsed = True
        return self._json

    def transport_errors(self, json_body=True):
        """PORT-SPEC 1.3: Content-Length on every reply, Connection: close, no gzip; JSON
        replies say application/json."""
        err = []
        cl = self.headers.get("content-length")
        if cl is None:
            err.append("no Content-Length")
        elif not cl.isdigit() or int(cl) != len(self.body):
            err.append(f"Content-Length {cl!r} but {len(self.body)} body bytes")
        if self.headers.get("connection", "").lower() != "close":
            err.append(f"Connection is {self.headers.get('connection')!r}, not close")
        if "content-encoding" in self.headers:
            err.append(f"Content-Encoding {self.headers['content-encoding']!r} (the app cannot decode it)")
        if self.headers.get("transfer-encoding"):
            err.append(f"Transfer-Encoding {self.headers['transfer-encoding']!r}")
        if json_body:
            ct = self.headers.get("content-type", "")
            if not ct.split(";")[0].strip().lower() == "application/json":
                err.append(f"Content-Type {ct!r} is not application/json")
        return err

    def _assert_transport(self, json_body=True):
        err = self.transport_errors(json_body)
        if err:
            raise AssertionError(f"{self.method} {self.path} -> {self.status}: " + "; ".join(err))

    def check(self, route=None, status=None, profile="xeno", transport=True):
        """A success reply: status, transport, strict JSON, contract. Returns the value."""
        key = route or self.route
        if key is None:
            raise AssertionError(f"{self.method} {self.path}: not a contract route; pass route=")
        if " " not in key:
            key = tvcontract.route_key(self.method, key)
        want = status if status is not None else tvcontract.SUCCESS_STATUS.get(key, 200)
        if self.status != want:
            raise AssertionError(f"{self.method} {self.path}: status {self.status}, expected {want}; "
                                 f"body {self.body[:400]!r}")
        if transport:
            self._assert_transport()
        return tvcontract.check_reply(key, self.json, profile=profile)

    def error(self, status=None, sentence=None, transport=True):
        """A refusal: tvcontract.assert_error_envelope plus the transport rules."""
        if transport:
            self._assert_transport()
        return tvcontract.assert_error_envelope(self.status, self.body, expect_status=status, sentence=sentence)

    def image(self, kind=None):
        """A 200 art reply (PORT-SPEC 1.7): WebP/PNG/JPEG bytes, matching Content-Type,
        Cache-Control: no-store, transport rules. Returns "webp" | "png" | "jpeg"."""
        self._assert_transport(json_body=False)
        got = tvcontract.check_art_image(self.status, self.headers, self.body)
        if kind is not None and got != kind:
            raise AssertionError(f"{self.method} {self.path}: {got} bytes, expected {kind}")
        return got


class TvClient:
    """The app's transport (APPO/http.cpp:137-167 format_request, 254-330 parse_response)."""

    def __init__(self, port, token=None, host="127.0.0.1", timeout=30.0, user_agent=USER_AGENT, max_bytes=64 * MiB):
        self.port, self.token, self.host = port, token, host
        self.timeout, self.user_agent, self.max_bytes = timeout, user_agent, max_bytes

    def _encode(self, body):
        if body is None:
            return b""
        if isinstance(body, (bytes, bytearray)):
            return bytes(body)
        if isinstance(body, str):
            return body.encode("utf-8")
        return json.dumps(body, separators=(",", ":"), ensure_ascii=False).encode("utf-8")

    def request(self, method, path, body=None, token=None, headers=(), content_type="application/json",
                host=None, timeout=None):
        method = method.upper()
        tok = self.token if token is None else token
        data = self._encode(body)
        head = [f"{method} {path} HTTP/1.1",
                f"Host: {host if host is not None else f'127.0.0.1:{self.port}'}",
                "Connection: close", "Accept: */*", f"User-Agent: {self.user_agent}"]
        if tok:
            head.append(f"Authorization: Bearer {tok}")
        if method != "GET":
            if content_type is not None:
                head.append(f"Content-Type: {content_type}")
            head.append(f"Content-Length: {len(data)}")
        for k, v in headers:
            head.append(f"{k}: {v}")
        wire = ("\r\n".join(head) + "\r\n\r\n").encode("utf-8") + (data if method != "GET" else b"")
        t0 = time.monotonic()
        with socket.create_connection((self.host, self.port), timeout=timeout or self.timeout) as s:
            s.sendall(wire)
            chunks, n = [], 0
            while True:
                b = s.recv(65536)
                if not b:
                    break
                chunks.append(b)
                n += len(b)
                if n > self.max_bytes:
                    raise AssertionError(f"{method} {path}: reply larger than {self.max_bytes} bytes")
        elapsed = time.monotonic() - t0
        raw = b"".join(chunks)
        end = raw.find(b"\r\n\r\n")
        if end < 0 or not raw.startswith(b"HTTP/1."):
            raise AssertionError(f"{method} {path}: invalid reply {raw[:200]!r}")
        head_text = raw[:end].decode("latin-1")
        lines = head_text.split("\r\n")
        parts = lines[0].split(" ", 2)
        if len(parts) < 2 or not parts[1].isdigit():
            raise AssertionError(f"{method} {path}: invalid status line {lines[0]!r}")
        hdrs = {}
        for ln in lines[1:]:
            if ":" in ln:
                k, v = ln.split(":", 1)
                k = k.strip().lower()
                hdrs[k] = (hdrs[k] + ", " + v.strip()) if k in hdrs else v.strip()
        payload = raw[end + 4:]
        cl = hdrs.get("content-length", "")
        if cl.isdigit():
            if len(payload) < int(cl):
                raise AssertionError(f"{method} {path}: truncated reply ({len(payload)} of {cl} bytes)")
            payload = payload[:int(cl)]
        return Reply(method, path, int(parts[1]), hdrs, payload, elapsed, head_text)

    def get(self, path, **kw):
        return self.request("GET", path, None, **kw)

    def post(self, path, body=None, **kw):
        return self.request("POST", path, body, **kw)

    def pair(self):
        """GET /api/v1/session (no Bearer, loopback): stores and returns the 64-hex token."""
        r = self.get("/api/v1/session", token="")
        v = r.check("GET /api/v1/session")
        self.token = v["token"]
        return self.token

    def poll(self, path, until, timeout=10.0, every=0.1, method="GET", body=None, **kw):
        deadline = time.monotonic() + timeout
        last = None
        while True:
            last = self.request(method, path, body, **kw)
            if until(last):
                return last
            if time.monotonic() >= deadline:
                raise AssertionError(f"timeout polling {method} {path}; last {last!r}")
            time.sleep(every)


# --------------------------------------------------------------------------- #
# Normaliser (PORT-SPEC 1.10)
# --------------------------------------------------------------------------- #

_RX = {k: re.compile(v) for k, v in {
    "hex32": r"[0-9a-f]{32}", "hex64": r"[0-9a-f]{64}", "code6": r"[0-9]{6}",
    "job": r"j_[0-9a-f]{6}", "ipv4": r"(?:[0-9]{1,3}\.){3}[0-9]{1,3}"}.items()}
_UNIX_KEYS = ("checkedAt", "updatedAt", "checkAfter", "retryAt", "automaticCheckAfter", "refreshAfter")
_BYTES_KEYS = ("freeBytes", "totalBytes", "usedBytes")
_VERSION_KEYS = ("version", "runningVersion", "savedVersion", "installedVersion", "latestVersion")


def _running_version():
    try:
        with open(os.path.join(PROJECT, "VERSION"), encoding="utf-8") as f:
            return f.read().strip().lstrip("v")
    except OSError:
        return ""


def _is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


class Normalizer:
    """Replaces run-dependent values by placeholders (see the module docstring)."""

    def __init__(self, roots=(), data=None, tmp=None, version=None, keys=None, paths=None):
        self.version = _running_version() if version is None else version
        subs = []
        for i, r in enumerate(roots):
            subs.append((r, f"<root:{i}>"))
        if data:
            subs.append((data, "<data>"))
        if tmp:
            subs.append((tmp, "<tmp>"))
        expanded = []
        for p, ph in subs:
            p = p.rstrip("/")
            expanded.append((p, ph))
            rp = os.path.realpath(p)
            if rp != p:
                expanded.append((rp, ph))
        self.subs = sorted(expanded, key=lambda t: -len(t[0]))
        self.jobs = {}
        self.keys = dict(keys or {})
        self.paths = dict(paths or {})

    def job(self, jid):
        if jid not in self.jobs:
            self.jobs[jid] = f"<job:{len(self.jobs) + 1}>"
        return self.jobs[jid]

    def _str(self, s):
        for p, ph in self.subs:
            if p and p in s:
                s = s.replace(p, ph)
        if _RX["job"].fullmatch(s):
            return self.job(s)
        return s

    @staticmethod
    def _apply(rule, v):
        return rule(v) if callable(rule) else rule

    def _value(self, key, v, path, parent, depth):
        cpath = re.sub(r"\[\d+\]", "[]", path)
        for pat, rule in self.paths.items():
            if cpath == "$." + pat or cpath.endswith("." + pat) or cpath == pat:
                return self._apply(rule, v)
        if key is not None:
            if key in self.keys:
                return self._apply(self.keys[key], v)
            if key == "requestId":
                return v
            if key == "instanceId" and isinstance(v, str) and _RX["hex32"].fullmatch(v):
                return "<hex32>"
            if key in ("token", "sourceKey") and isinstance(v, str) and _RX["hex64"].fullmatch(v):
                return "<hex64>"
            if key == "pairCode" and isinstance(v, str) and _RX["code6"].fullmatch(v):
                return "<code6>"
            if key == "address" and isinstance(v, str) and _RX["ipv4"].fullmatch(v):
                return "<ipv4>"
            if key == "httpPort" and _is_int(v):
                return "<port>"
            if key in _UNIX_KEYS and _is_int(v):
                return v if v == 0 else "<unix>"
            if key in _BYTES_KEYS and _is_int(v):
                return "<bytes>"
            if key == "projectedFreeBytes" and _is_int(v):
                f, p = parent.get("freeBytes"), parent.get("pendingBytes")
                if _is_int(f) and _is_int(p) and v != f - p - RESERVE_GAP:
                    raise AssertionError(f"{path}: projectedFreeBytes {v} != freeBytes - pendingBytes - 240 MiB "
                                         f"= {f - p - RESERVE_GAP}")
                return "<bytes>"
            if key == "id" and isinstance(v, str) and _RX["hex64"].fullmatch(v) and \
                    re.search(r"\.(drives|destinations)\[\d+\]$", path.rsplit(".", 1)[0] if "." in path else ""):
                return "<hex64>"
            if depth == 1 and key in _VERSION_KEYS and self.version and v == self.version:
                return "<version>"
        return self._walk(v, path, depth)

    def _walk(self, v, path="$", depth=0):
        if isinstance(v, str):
            return self._str(v)
        if isinstance(v, list):
            return [self._value(None, it, f"{path}[{i}]", None, depth + 1) for i, it in enumerate(v)]
        if isinstance(v, dict):
            return {k: self._value(k, it, f"{path}.{k}", v, depth + 1) for k, it in v.items()}
        return v

    def __call__(self, value):
        return self._walk(copy.deepcopy(value))


# --------------------------------------------------------------------------- #
# Golden files
# --------------------------------------------------------------------------- #

def golden_update_mode():
    return os.environ.get("XENO_TV_GOLDEN_UPDATE", "") == "1"


def golden_path(name):
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,80}", name):
        raise ValueError(f"bad golden name {name!r}")
    return os.path.join(GOLDEN_DIR, name + ".json")


def dumps_golden(value):
    return json.dumps(value, indent=1, ensure_ascii=False, sort_keys=False) + "\n"


def assert_golden(name, value, normalizer=None):
    """Normalises `value` and compares it byte for byte (key order included) with
    tests/host/tv/golden/<name>.json; XENO_TV_GOLDEN_UPDATE=1 writes the file instead.
    Returns the normalised value."""
    norm = (normalizer or Normalizer())(value)
    text = dumps_golden(norm)
    path = golden_path(name)
    if golden_update_mode():
        os.makedirs(GOLDEN_DIR, exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(tmp, path)
        return norm
    if not os.path.exists(path):
        raise AssertionError(f"golden file {os.path.relpath(path, PROJECT)} is missing: run the suite with "
                             f"XENO_TV_GOLDEN_UPDATE=1 and review the new file")
    with open(path, encoding="utf-8") as f:
        want = f.read()
    if want != text:
        diff = list(difflib.unified_diff(want.splitlines(), text.splitlines(), "golden/" + name + ".json",
                                         "actual", lineterm="", n=2))
        shown = "\n".join(diff[:120]) + (f"\n... {len(diff) - 120} more diff lines" if len(diff) > 120 else "")
        raise AssertionError(f"reply differs from golden/{name}.json (XENO_TV_GOLDEN_UPDATE=1 rewrites it):\n{shown}")
    return norm


# --------------------------------------------------------------------------- #
# File server shared by the worlds of one test run
# --------------------------------------------------------------------------- #

_SHARED = {}
_SHARED_MU = threading.Lock()


def shared_files():
    """One range_server.py child process for the whole run (files generated once)."""
    with _SHARED_MU:
        rs = _SHARED.get("rs")
        if rs is None or rs.proc is None or rs.proc.poll() is not None:
            tmp = tempfile.mkdtemp(prefix="xeno-tv-files.")
            rs = harness.RangeServerProcess(os.path.join(tmp, "files")).start()
            _SHARED["rs"], _SHARED["tmp"] = rs, tmp
            atexit.register(_close_shared)
        return rs


def _close_shared():
    rs = _SHARED.pop("rs", None)
    if rs:
        rs.kill()
    tmp = _SHARED.pop("tmp", None)
    if tmp and os.environ.get("XENO_TV_KEEP") != "1":
        shutil.rmtree(tmp, ignore_errors=True)


# --------------------------------------------------------------------------- #
# The world
# --------------------------------------------------------------------------- #

def _deep_merge(dst, src):
    for k, v in src.items():
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            _deep_merge(dst[k], v)
        else:
            dst[k] = copy.deepcopy(v)
    return dst


def _write_json(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(value, f, ensure_ascii=False, indent=1)
        f.write("\n")


def _sparse_file(path, size, header=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        if header is not None:
            f.write(b"\x7fCNT")
            f.seek(0x40)
            f.write(header)
        f.truncate(size)


def save_sfo(title_id, title, account=0, dirname="SAVEDATA00"):
    """A save's param.sfo (PSF 1.1): ACCOUNT_ID, MAINTITLE, SAVEDATA_DIRECTORY, TITLE_ID."""
    entries = [("ACCOUNT_ID", 0x0404, struct.pack("<Q", account), 8),
               ("MAINTITLE", 0x0204, title.encode("utf-8"), 128),
               ("SAVEDATA_DIRECTORY", 0x0204, dirname.encode(), 64),
               ("TITLE_ID", 0x0204, title_id.encode(), 12)]
    keys, key_offsets, data, data_offsets = b"", [], b"", []
    for k, _, d, mx in entries:
        key_offsets.append(len(keys))
        keys += k.encode() + b"\0"
        data_offsets.append(len(data))
        data += d.ljust(mx, b"\0")
    key_start = 20 + 16 * len(entries)
    pad = (-(key_start + len(keys))) % 4
    hdr = b"\0PSF" + struct.pack("<IIII", 0x0101, key_start, key_start + len(keys) + pad, len(entries))
    idx = b"".join(struct.pack("<HHIII", key_offsets[i], e[1], len(e[2]), e[3], data_offsets[i])
                   for i, e in enumerate(entries))
    return hdr + idx + keys + b"\0" * pad + data


def save_zip(title_id, title, data):
    """A decrypted community save as the Save Data Store installs it: sce_sys/param.sfo
    at the root and the save's data (savedata.bin)."""
    bio = io.BytesIO()
    with zipfile.ZipFile(bio, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("sce_sys/param.sfo", save_sfo(title_id, title))
        z.writestr("savedata.bin", data)
    return bio.getvalue()


class SavesIndex:
    """The Save Data Store's GitHub index on a local port (World saves=): GET of
    a path added with add(), else 404. Every request path lands in requests."""

    def __init__(self):
        self.routes, self.requests = {}, []
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):  # noqa: N802
                path = self.path.split("?", 1)[0]
                owner.requests.append(path)
                ctype, body = owner.routes.get(path, ("application/json", None))
                self.send_response(200 if body is not None else 404)
                self.send_header("Content-Type", ctype)
                body = body if body is not None else b'{"message":"Not Found"}'
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.srv.daemon_threads = True
        self.base = f"http://127.0.0.1:{self.srv.server_address[1]}"
        threading.Thread(target=self.srv.serve_forever, name="saves-index", daemon=True).start()

    def add(self, path, body, content_type="application/json"):
        self.routes[path] = (content_type, body if isinstance(body, bytes) else body.encode("utf-8"))

    def stop(self):
        self.srv.shutdown()
        self.srv.server_close()


class World:
    def __init__(self, name="tv", catalog="fixture", featured="fixture", acknowledged=True, enabled=None,
                 settings=None, roots=("R1", "R2"), library=False, files="shared", art=True, show_zip=False,
                 tvapi_updates=False, env=None, sync_zip_sizes=True, port=None, saves=None):
        self.name = name
        self.opt_catalog, self.opt_featured = catalog, featured
        self.acknowledged, self.enabled = acknowledged, enabled
        self.extra_settings = settings or {}
        self.root_names, self.library = tuple(roots), library
        self.opt_files, self.opt_art = files, art
        self.show_zip, self.tvapi_updates = show_zip, tvapi_updates
        self.extra_env = dict(env or {})
        self.sync_zip_sizes = sync_zip_sizes
        self.fixed_port = port
        self.opt_saves = saves
        self.tmp = None
        self.srv = None
        self.files = None
        self.art = None
        self.saves_index = None
        self._own_files = None
        self._catalog = None
        self.featured = None
        self.token = None
        self._started = False

    # ---- set-up -----------------------------------------------------------
    @property
    def catalog(self):
        if self._catalog is None and self.opt_catalog == "real":
            self._catalog = tv_golden.load_repo_catalog()
        return self._catalog

    def _servers(self):
        if self.opt_files == "shared":
            self.files = shared_files()
        elif self.opt_files == "own":
            self._own_files = harness.RangeServerProcess(os.path.join(self.tmp, "files")).start()
            self.files = self._own_files
        if self.opt_art:
            self.art = artgen.ArtServer().start()
        if self.opt_saves is not None:
            self.saves_index = SavesIndex()
            rows = []
            for row in self.opt_saves:
                r = {k: v for k, v in row.items() if k != "zip"}
                if "zip" in row:
                    path = f"/saves/{row['id']}.zip"
                    self.saves_index.add(path, row["zip"], "application/zip")
                    r.update(url=self.saves_index.base + path, size=len(row["zip"]),
                             sha256=hashlib.sha256(row["zip"]).hexdigest())
                rows.append(r)
            self.saves_index.add("/saves.json", json.dumps({"schemaVersion": 1, "saves": rows}))

    def _build_catalog(self):
        files_base = self.files.base if self.files else "http://127.0.0.1:9"
        art_base = self.art.base if self.art else "http://" + NO_ART
        if self.opt_catalog == "fixture":
            cat = load_fixture_catalog(files_base, art_base)
        elif isinstance(self.opt_catalog, dict):
            cat = fill_placeholders(self.opt_catalog, files_base, art_base)
        else:
            return None
        if self.sync_zip_sizes and self.files:
            prefix = self.files.base + "/"
            for g in cat.get("games", []):
                for r in g.get("releases", []):
                    url = r.get("url", "")
                    name = url.rsplit("/", 1)[-1]
                    if url.startswith(prefix) and name in range_server.ZIPS:
                        p = os.path.join(self.files.files_dir, name)
                        if os.path.exists(p):
                            r["sizeBytes"] = os.path.getsize(p)
        return cat

    def _build_featured(self):
        f = self.opt_featured
        if f == "fixture":
            return load_fixture_featured()
        if f == "real":
            return tv_golden.load_repo_featured()
        if isinstance(f, dict):
            return copy.deepcopy(f)
        return None

    def _settings_doc(self):
        if isinstance(self.enabled, dict):
            en = {k: bool(v) for k, v in self.enabled.items()}
        elif self.enabled is None:
            en = {k: True for k in EXPOSED_SOURCES}
        else:
            en = {k: (k in self.enabled) for k in EXPOSED_SOURCES}
        doc = {"sources": {"enabled": en, "acknowledged": bool(self.acknowledged)},
               "catalog": {"autoRefresh": False},
               "updates": {"autoCheck": False}}
        if self.saves_index:
            doc["catalog"]["savesBackendUrl"] = self.saves_index.base + "/saves-backend.json"
        return _deep_merge(doc, self.extra_settings)

    def _seed_library(self):
        for where, rel, size, header in LIBRARY_FILES:
            if where == "user":
                os.makedirs(os.path.join(self.data, "user", "app", rel), exist_ok=True)
            elif where < len(self.roots):
                _sparse_file(os.path.join(self.roots[where], rel), size, header)

    def env(self):
        e = {"XENO_TVAPI_STOPPING_FILE": self.stopping_file}
        if self.art:
            e["XENO_HOST_ART_FIXTURE"] = self.art.hostport
        elif self.opt_catalog != "real":
            e["XENO_HOST_ART_FIXTURE"] = NO_ART
        if self.show_zip:
            e["XENO_TV_SHOW_ZIP"] = "1"
        if self.tvapi_updates:
            e["XENO_TVAPI_UPDATES"] = "1"
        if self.saves_index:
            e["XENO_SAVES_INDEX_BASE"] = self.saves_index.base
        e.update(self.extra_env)
        return e

    def start(self, timeout=15.0):
        """Builds the temp world and starts the backend. Returns self."""
        if self._started:
            return self
        self.tmp = tempfile.mkdtemp(prefix=f"xeno-tv-{re.sub(r'[^A-Za-z0-9_-]', '_', self.name)}.")
        self.data = os.path.join(self.tmp, "data")
        self.roots = [os.path.join(self.tmp, n) for n in self.root_names]
        for r in self.roots:
            os.makedirs(r, exist_ok=True)
        os.makedirs(self.data, exist_ok=True)
        self.stopping_file = os.path.join(self.tmp, "stopping")
        try:
            self._servers()
            cat = self._build_catalog()
            if cat is not None:
                self._catalog = cat
                _write_json(os.path.join(self.data, "catalog.json"), cat)
            self.featured = self._build_featured()
            if self.featured is not None:
                _write_json(os.path.join(self.data, "feeds", "featured.json"), self.featured)
            _write_json(os.path.join(self.data, "settings.json"), self._settings_doc())
            if self.library:
                self._seed_library()
            self.srv = harness.XenoServer(self.data, roots=self.roots, env=self.env(), port=self.fixed_port)
            self.srv.start(timeout=timeout)
        except BaseException:
            self.close()
            raise
        self._started = True
        return self

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.close()

    # ---- members ----------------------------------------------------------
    @property
    def port(self):
        return self.srv.port

    @property
    def base(self):
        return self.srv.base

    @property
    def default_root(self):
        return os.path.join(self.data, "downloads")

    @property
    def all_roots(self):
        return [self.default_root] + list(self.roots)

    def client(self, paired=True, **kw):
        c = TvClient(self.port, **kw)
        if paired:
            tok = c.pair()
            if self.token is None:
                self.token = tok
        return c

    def golden_catalog(self, enabled="settings", acknowledged=None, show_zip=None):
        """tv_golden.build over this world's catalogue and feed; by default with the
        world's own enabled/acknowledged settings and show_zip."""
        en = (self.enabled if enabled == "settings" else enabled)
        ack = self.acknowledged if acknowledged is None else acknowledged
        sz = self.show_zip if show_zip is None else show_zip
        return tv_golden.build(self.catalog, self.featured, show_zip=sz, enabled=en, acknowledged=ack)

    def golden_sources(self, enabled="settings", acknowledged=None):
        en = (self.enabled if enabled == "settings" else enabled)
        ack = self.acknowledged if acknowledged is None else acknowledged
        return tv_golden.sources(self.catalog, self.featured, enabled=en, acknowledged=ack, show_zip=self.show_zip)

    def release(self, rid):
        for g in (self.catalog or {}).get("games", []):
            for r in g.get("releases", []):
                if r.get("id") == rid:
                    return r
        raise KeyError(rid)

    def game(self, gid):
        for g in (self.catalog or {}).get("games", []):
            if g.get("id") == gid:
                return g
        raise KeyError(gid)

    def file_url(self, name, modes="ok"):
        return f"{self.files.base}/{modes}/{name}"

    def set_stopping(self, on=True):
        if on:
            open(self.stopping_file, "w").close()
        elif os.path.exists(self.stopping_file):
            os.unlink(self.stopping_file)

    def log_text(self):
        return self.srv.log_text() if self.srv else ""

    def wait_log(self, pattern, timeout=10.0):
        rx = re.compile(pattern)
        harness.wait_until(lambda: rx.search(self.log_text()) is not None, timeout=timeout,
                           what=f"log line /{pattern}/")
        return rx.search(self.log_text())

    def normalizer(self, **kw):
        return Normalizer(roots=self.all_roots, data=self.data, tmp=self.tmp, **kw)

    def assert_golden(self, name, value, normalizer=None):
        return assert_golden(name, value, normalizer or self.normalizer())

    # ---- life cycle -------------------------------------------------------
    def stop(self):
        if self.srv:
            self.srv.stop()

    def restart(self, timeout=15.0):
        self.srv.stop()
        self.srv.start(timeout=timeout)
        return self

    def close(self):
        if self.srv:
            try:
                self.srv.stop()
            except Exception:
                self.srv.kill()
        if self.art:
            self.art.stop()
            self.art = None
        if self._own_files:
            self._own_files.kill()
            self._own_files = None
        if self.saves_index:
            self.saves_index.stop()
            self.saves_index = None
        if self.tmp and os.environ.get("XENO_TV_KEEP") != "1":
            shutil.rmtree(self.tmp, ignore_errors=True)
        self._started = False


def main(argv):
    """python3 -I tests/host/tvworld.py [--real] [--unacknowledged] [--library]: starts a world,
    prints its coordinates as JSON and keeps it up until Ctrl-C."""
    w = World("manual", catalog="real" if "--real" in argv else "fixture",
              featured="real" if "--real" in argv else "fixture",
              acknowledged="--unacknowledged" not in argv, library="--library" in argv).start()
    try:
        c = w.client()
        print(json.dumps({"port": w.port, "data": w.data, "roots": w.roots, "token": c.token,
                          "files": w.files.base if w.files else "", "art": w.art.base if w.art else ""}, indent=1))
        sys.stdout.flush()
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
    finally:
        w.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
