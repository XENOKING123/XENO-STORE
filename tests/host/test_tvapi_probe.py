#!/usr/bin/env python3
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
test_tvapi_probe.py — the real-parser check of the /api/v1 layer (PORT-SPEC 1.10
class 11, TvContractProbe).

app/tv/build/host/tv-contract-probe (app/tv/tools/contract-probe.cpp, built by
app/tv/tools/build-contract-probe.sh) links the TV app's own src/orbit/http.cpp,
json.cpp, api.cpp and model.cpp, calls every orbit::api::Client method once and
prints one JSON line per call. These suites start the host backend
(build/host/xeno-store) with the tvworld fixture world, run the probe against it
and check:

  TvContractProbe              the full walk: no call takes a parse-failure path
                               (an "unreadable" sentence, a 2xx body the parser
                               rejects, an unreadable refusal envelope), every
                               status and refusal sentence matches the seeded
                               world, the catalogue the app builds equals
                               tv_golden.build() on the fixture (count, ids in
                               order, games, sizes), and the values the app's
                               model holds equal what the backend sent; with
                               the Save Data Store's calls (/api/saves: the
                               list from a local GitHub index, the profiles,
                               an install followed and cancelled, an unknown
                               save's refusal).
  TvContractProbeUpdates       POST /updates and /tv-app with the host updates
                               switch on (XENO_TVAPI_UPDATES=1): the 202 replies
                               the app parses without checking.
  TvContractProbeRealCatalogue the binary's embedded revision-10 catalogue
                               (2.36 MB) through the app's strict parser, inside
                               the client's 16 MiB limit, equal to tv_golden.

Skipped unless XENO_TV_PROBE points at the built probe (CI's tv-app job sets
XENO_TV_PROBE=app/tv/build/host/tv-contract-probe; a relative path is taken from
the repository root). The backend's mismatches are not fixed here: each one is
an assertion that names the route, the field, and the expected and actual value.
"""
import json
import os
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402
import tv_golden  # noqa: E402
import tvworld  # noqa: E402

PROJECT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_probe = os.environ.get("XENO_TV_PROBE", "")
PROBE = _probe if not _probe or os.path.isabs(_probe) else os.path.join(PROJECT, _probe)
HAVE_PROBE = bool(PROBE) and os.path.isfile(PROBE) and os.access(PROBE, os.X_OK)
SKIP = "XENO_TV_PROBE does not point at the built probe (bash app/tv/tools/build-contract-probe.sh)"

# A closed local port: the catalogue refresh and the update checks fail at once
# instead of reaching the network.
CLOSED = "http://127.0.0.1:9"

# The probe's choices in the fixture world.
SLOW_RELEASE = "fx-long-haul-ffpfsc"      # 64 MiB at 1 MiB/s: walks pause, resume, retry, cancel ...
DONE_RELEASE = "fx-tidewater-ffpfsc"      # 8 MiB at full speed: complete, then delete and remove
BROWSER_RELEASE = "fx-neon-courier-vf"    # browser-only (Vikingfile)
GAME = "fx-tidewater-ps5"                 # art and favourites
SLOW_SAVE = "xk-probe-slow"               # 64 MiB at 1 MiB/s: installed, followed, cancelled

# Wording of the client's parse-failure sentences (APPO/api.cpp) and of a refusal
# whose {"error"} envelope could not be read.
PARSE_WORDS = ("unreadable", "could not be read", "did not open a console session",
               "could not complete this request", "Could not save the setting.")

S_CANNOT_RESUME = "This job cannot be resumed in its current state."
S_PASSWORD_STATE = "Pause extraction or wait for it to finish before entering a password."
S_NO_LINK = "This download has no link to refresh."
S_ONLY_CANCELLED = "Only cancelled downloads can be deleted."
S_BROWSER = ("Vikingfile asks for a human check, so XENO STORE cannot fetch it by itself. "
             "Download it with TorBox, or open this game in the browser version.")
S_BROWSER_GONE = "This verification session is no longer active."
S_UPDATES_HOST = "Updates work with XENO STORE running on your PS5."
S_TVAPP_HOST = "The TV app is installed from XENO STORE running on your PS5."

# Every call of the full walk, in order: (call, status, refusal sentence or None).
EXPECTED = (
    ("session", 200, None),
    ("system", 200, None),
    ("sources", 200, None),
    ("save_sources", 200, None),
    ("catalog", 200, None),
    ("catalogue_status", 200, None),
    ("refresh_catalogue", 202, None),
    ("art:cover", 200, None),
    ("art:hero", 200, None),
    ("favourites", 200, None),
    ("set_favourite", 200, None),
    ("storage", 200, None),
    ("set_preferred_storage", 200, None),
    ("set_download_limit", 200, None),
    ("set_delete_archives", 200, None),
    ("debrid", 200, None),
    ("browser_session", 200, None),
    ("start_browser", 409, S_BROWSER),
    ("cancel_browser", 409, S_BROWSER_GONE),
    ("downloads", 200, None),
    ("create_download", 201, None),
    ("job_action:pause", 200, None),
    ("job_action:resume", 200, None),
    ("job_action:retry", 409, S_CANNOT_RESUME),
    ("job_action:cancel", 200, None),
    ("job_action:archive-password", 409, S_PASSWORD_STATE),
    ("job_action:refresh-link", 409, S_NO_LINK),
    ("job_action:forget", 200, None),
    ("create_download:complete", 201, None),
    ("job_action:delete", 409, S_ONLY_CANCELLED),
    ("job_action:remove", 200, None),
    ("library", 200, None),
    ("library:refresh", 200, None),
    ("library_storage", 200, None),
    ("library_storage:refresh", 200, None),
    ("library_action", 202, None),
    ("service_update", 200, None),
    ("service_action", 409, S_UPDATES_HOST),
    ("tv_app", 200, None),
    ("tv_app_action", 409, S_TVAPP_HOST),
    ("save_store", 200, None),
    ("save_users", 200, None),
    ("install_save", 200, None),
    ("save_job", 200, None),
    ("cancel_save_job", 200, None),
    ("install_save:unknown", 404, "unknown save id"),
)


def fnv1a(values):
    """The probe's FNV-1a 64 over "value\\n" for each value, as 16 hex digits."""
    h = 1469598103934665603
    for v in values:
        for b in v.encode("utf-8") + b"\n":
            h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"


# The app's typed reads (APPO/json.cpp Value::as_*): another JSON type reads as the fallback.
def txt(d, k):
    v = d.get(k)
    return v if isinstance(v, str) else ""


def num(d, k, fallback=0):
    v = d.get(k)
    return v if isinstance(v, (int, float)) and not isinstance(v, bool) else fallback


def flag(d, k, fallback=False):
    v = d.get(k)
    return v if isinstance(v, bool) else fallback


def num_text(v):
    """The probe's number text: an integer when it is one, else %.17g."""
    return str(int(v)) if float(v).is_integer() else "%.17g" % v


def release_line(o):
    """One option as the app's parse_catalog keeps it (APPO/model.cpp), tab-separated like the probe."""
    plat = txt(o, "platform") or ("ps4" if txt(o, "titleId").startswith("CUSA") else "ps5")
    rank = num(o, "newOnOrbitRank")
    rank = int(rank) if 1 <= rank <= 100 and rank == int(rank) else 0
    fields = [txt(o, "id"), txt(o, "gameId"), txt(o, "titleId"), plat] + \
             [txt(o, k) for k in ("title", "genre", "tagline", "description", "provider", "sourceId", "format", "region",
                                  "version", "filename", "notes", "artworkLayout", "publisher", "releaseDate",
                                  "addedAt")] + \
             [str(rank), num_text(num(o, "sizeBytes")), txt(o, "compression"), num_text(num(o, "extractedBytes")),
              "1" if txt(o, "archiveContents") == "unverified" else "0",
              "1" if flag(o, "browserAvailable") else "0", "1" if flag(o, "directAvailable", True) else "0"]
    return "\t".join(fields)


def app_view(item, spec):
    """A wire object as the app's model holds it: spec maps key -> "s" (text), "n" (number),
    "v" (a volatile byte count such as free space: only "it is a number" is compared),
    "b"/"B" (flag, default false/true) or "o" (optional bytes: -1 unless a number)."""
    out = {}
    for k, kind in spec.items():
        if kind == "s":
            out[k] = txt(item, k)
        elif kind == "n":
            out[k] = num(item, k)
        elif kind == "v":
            out[k] = num(item, k, None) is not None
        elif kind == "o":
            out[k] = num(item, k, -1)
        else:
            out[k] = flag(item, k, kind == "B")
    return out


DRIVE = {"id": "s", "label": "s", "path": "s", "freeBytes": "v", "totalBytes": "v", "pendingBytes": "n",
         "projectedFreeBytes": "v", "external": "b"}
LIBRARY_GAME = {"titleId": "s", "title": "s", "platform": "s", "format": "s", "path": "s", "runtimePath": "s",
                "sourceKey": "s", "location": "s", "installed": "b", "mounted": "b", "onDrive": "b", "managed": "b",
                "canManageSource": "b", "installedSizeBytes": "o", "sizeBytes": "o", "sizeStatus": "s"}
LIBRARY_DRIVE = {"id": "s", "path": "s", "label": "s", "filesystem": "s", "readOnly": "b", "totalBytes": "v",
                 "freeBytes": "v", "usedBytes": "v"}
SAVE_ROW = {"id": "s", "titleId": "s", "gameId": "s", "title": "s", "name": "s", "description": "s", "author": "s",
            "category": "s", "gameVersion": "s", "region": "s", "createdAt": "s", "source": "s", "size": "n",
            "installed": "b", "installedVersion": "s", "versionCompare": "n", "compat": "s", "compatText": "s"}
SAVE_USER = {"id": "n", "name": "s", "foreground": "b"}


def first_seen(values):
    seen, out = set(), []
    for v in values:
        if v not in seen:
            seen.add(v)
            out.append(v)
    return out


def run_probe(world, *args, timeout=300):
    """Runs the probe against the world; returns (exit code, call lines, wait lines, summary, stderr)."""
    p = subprocess.run([PROBE, f"127.0.0.1:{world.port}", *args], capture_output=True, text=True,
                       timeout=timeout, env={k: v for k, v in os.environ.items()
                                             if k.lower() not in ("http_proxy", "https_proxy")})
    calls, waits, summary = [], [], None
    for raw in p.stdout.splitlines():
        line = json.loads(raw)
        if line["call"] == "wait":
            waits.append(line)
        elif line["call"] == "summary":
            summary = line
        else:
            calls.append(line)
    return p.returncode, calls, waits, summary, p.stderr


class ProbeAsserts:
    """Checks shared by the suites (mixed into unittest.TestCase)."""

    def by_call(self):
        return {c["call"]: c for c in self.calls}

    def assert_no_parse_failure(self):
        bad = []
        for c in self.calls:
            where = f'{c["method"]} {c["path"]} ({c["call"]})'
            if c["parseFailure"]:
                bad.append(f"{where}: the client took a parse-failure path: status {c['status']}, "
                           f"parse {c['parse']!r}, error {c['error']!r}")
            if c["status"] and c["parse"] not in ("ok", "image") and not (c["parse"] == "empty" and c["status"] == 202):
                bad.append(f"{where}: the app's parser rejects the body: {c['parse']!r}")
            for word in PARSE_WORDS:
                if word in c["error"]:
                    bad.append(f"{where}: error {c['error']!r} is the client's parse-failure wording ({word!r})")
            if c["status"] == 0:
                bad.append(f"{where}: transport failure {c['error']!r}")
            if c["bytes"] > c["limit"]:
                bad.append(f"{where}: {c['bytes']} bytes is over the client's limit of {c['limit']}")
        self.assertEqual(bad, [], "\n".join(bad))
        self.assertIsNotNone(self.summary, "the probe printed no summary line:\n" + self.stderr[-2000:])
        self.assertEqual((self.summary["parseFailures"], self.summary["transportFailures"]), (0, 0))
        self.assertEqual(self.rc, 0, "the probe exits 1 on a parse or transport failure")

    def assert_items(self, route, got, sent, spec, key="id"):
        """Each item the app's model holds equals the wire item read with the app's rules."""
        want = [app_view(item, spec) for item in sent]
        bad = []
        if len(got) != len(want):
            bad.append(f"{route}: the app keeps {len(got)} items, the backend sent {len(want)}")
        for g, w in zip(got, want):
            for k, kind in spec.items():
                value = g.get(k)
                if kind == "v":
                    value = isinstance(value, (int, float)) and not isinstance(value, bool)
                if value != w[k]:
                    bad.append(f"{route} item {w.get(key)!r} field {k}: expected {w[k]!r}, the app read {g.get(k)!r}")
        self.assertEqual(bad, [], "\n".join(bad))

    def assert_catalogue(self, c, golden, raw_bytes=None):
        ids = [o["id"] for o in golden]
        games = first_seen(o["gameId"] for o in golden)
        got = (c["counts"]["releases"], c["counts"]["raw"], c["counts"]["dropped"], c["counts"]["games"])
        self.assertEqual(got, (len(golden), len(golden), 0, len(games)),
                         "GET /api/v1/catalog: (releases the app keeps, items, items the app drops, game cards) "
                         f"expected {(len(golden), len(golden), 0, len(games))}, got {got}")
        self.assertEqual(c["values"]["reparsed"], len(golden))
        self.assertEqual(c["values"]["idsHash"], fnv1a(ids), "GET /api/v1/catalog: release ids or their order differ")
        self.assertEqual(c["values"]["fieldsHash"], fnv1a(release_line(o) for o in golden),
                         "GET /api/v1/catalog: a field the app reads differs from the reference builder's value")
        self.assertEqual(c["values"]["gamesHash"], fnv1a(games), "GET /api/v1/catalog: game cards differ")
        self.assertEqual((c["values"]["first"], c["values"]["last"]), (ids[0], ids[-1]))
        self.assertEqual(c["values"]["sizeSum"], sum(o["sizeBytes"] for o in golden), "sizeBytes as the app reads them")
        self.assertEqual(c["counts"]["browserOnly"],
                         sum(1 for o in golden if o["browserAvailable"] and not o["directAvailable"]),
                         "browserAvailable/directAvailable as the app reads them")
        self.assertEqual(c["counts"]["ranked"], sum(1 for o in golden if 1 <= o.get("newOnOrbitRank", 0) <= 100),
                         "newOnOrbitRank as the app reads it")
        self.assertEqual(c["counts"]["ps4"], sum(1 for o in golden if o["platform"] == "ps4"), "platform")
        self.assertEqual(c["limit"], 16 << 20, "Client::catalog reads up to 16 MiB")
        self.assertLess(c["bytes"], c["limit"])
        if raw_bytes is not None:
            self.assertEqual(c["bytes"], raw_bytes)


# --------------------------------------------------------------------------- #
# 1. The full walk in the fixture world
# --------------------------------------------------------------------------- #

@unittest.skipUnless(HAVE_PROBE, SKIP)
class TvContractProbe(ProbeAsserts, unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        files = tvworld.shared_files()
        cls.w = tvworld.World("probe", library=True, settings={"catalog": {
            "url": CLOSED + "/catalog.json", "revisionUrl": CLOSED + "/revision.json"}}, saves=[
            {"id": "xk-probe-chapter", "titleId": "PPSA90003", "title": "Ember Hollow", "name": "Chapter 4",
             "description": "Every forge unlocked.", "author": "ashwright", "category": "chapter",
             "gameVersion": "01.050.000", "region": "EUR", "createdAt": "2026-10-12T18:20:00Z",
             "zip": tvworld.save_zip("PPSA90003", "Ember Hollow", b"PROBE" * 400)},
            {"id": SLOW_SAVE, "titleId": "PPSA90005", "title": "Starfall Rally", "name": "Season two",
             "author": "gridline", "url": files.base + "/slow/big.bin", "size": 64 * tvworld.MiB},
        ]).start()
        try:
            c = cls.w.client()
            # The Library's first read runs in the background: let it settle, so the
            # probe sees the seeded titles rather than an empty "idle" snapshot.
            c.poll("/api/v1/library", until=lambda r: r.status == 200 and r.json["status"] == "ready"
                   and not r.json["busy"], timeout=30)
            cls.wire = {
                "system": c.get("/api/v1/system").check(),
                "sources": c.get("/api/v1/sources").check(),
                "updates": c.get("/api/v1/catalog/updates").check(),
                "storage": c.get("/api/v1/storage").check(),
                "debrid": c.get("/api/v1/debrid").check(),
                "library": c.get("/api/v1/library").check(),
                "library_storage": c.get("/api/v1/library/storage").check(),
                "saves": c.get("/api/saves/store").json,
                "save_users": c.get("/api/saves/users").json,
            }
            cls.golden = cls.w.golden_catalog()
            cls.rc, cls.calls, cls.waits, cls.summary, cls.stderr = run_probe(
                cls.w, "--release", SLOW_RELEASE, "--complete", DONE_RELEASE, "--browser", BROWSER_RELEASE,
                "--game", GAME, "--save", SLOW_SAVE, "--wait", "60")
            cls.after = {"favorites": c.get("/api/v1/favorites").check(),
                         "system": c.get("/api/v1/system").check(),
                         "downloads": c.get("/api/v1/downloads").check()}
        except BaseException:
            cls.w.close()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    def test_01_every_client_method_ran_in_order(self):
        self.assertEqual([c["call"] for c in self.calls], [e[0] for e in EXPECTED], self.stderr[-2000:])
        self.assertEqual(self.summary["calls"], len(EXPECTED))

    def test_02_no_parse_failure(self):
        self.assert_no_parse_failure()

    def test_03_statuses_match_the_seeded_world(self):
        got = self.by_call()
        bad = []
        for call, status, sentence in EXPECTED:
            c = got.get(call)
            if c is None:
                bad.append(f"{call}: not run")
                continue
            where = f'{c["method"]} {c["path"]} ({call})'
            if c["status"] != status:
                bad.append(f"{where}: status expected {status}, got {c['status']} (error {c['error']!r})")
            if sentence is None and c["error"]:
                bad.append(f"{where}: error expected none, got {c['error']!r}")
            if sentence is not None and c["error"] != sentence:
                bad.append(f"{where}: error expected {sentence!r}, got {c['error']!r}")
            if not c["contentType"].startswith("image/") and c["status"] and \
                    not c["contentType"].startswith("application/json"):
                bad.append(f"{where}: Content-Type expected application/json, got {c['contentType']!r}")
        self.assertEqual(bad, [], "\n".join(bad))

    def test_04_catalogue_equals_the_golden_builder(self):
        self.assert_catalogue(self.by_call()["catalog"], self.golden)

    def test_05_session_system_sources(self):
        got = self.by_call()
        v = got["session"]["values"]
        self.assertEqual((v["tokenLength"], v["pairCodeLength"]), (64, 6), "GET /api/v1/session: token, pairCode")
        s, want = got["system"]["values"], self.wire["system"]
        for key in ("name", "version", "instanceId", "platform", "stopping", "stateHealthy", "catalogueRevision",
                    "localSessionAvailable", "preferredStorage", "concurrentDownloads", "deleteArchives"):
            self.assertEqual(s[key], want[key], f"GET /api/v1/system field {key}: the app read {s[key]!r}, "
                                                f"the backend sent {want[key]!r}")
        self.assertIs(s["paired"], True, "GET /api/v1/system field paired: with the session's Bearer")
        src, want = got["sources"]["values"], self.wire["sources"]
        self.assertEqual(src["enabled"], want["enabled"])
        self.assertEqual((src["acknowledged"], src["noticeVersion"]), (want["acknowledged"], want["noticeVersion"]))
        self.assertEqual(src["options"], [o["id"] for o in want["options"]])
        self.assertEqual(src["releaseCounts"], [f'{o["id"]}={o["releaseCount"]}' + ("/ready" if o["downloadReady"] else "")
                                                for o in want["options"]])
        self.assertEqual(got["sources"]["counts"]["raw"], len(want["options"]))
        self.assertEqual(got["save_sources"]["values"]["enabled"], want["enabled"])

    def test_06_catalogue_status_and_refresh(self):
        got = self.by_call()
        u, want = got["catalogue_status"]["values"], self.wire["updates"]
        for key in ("available", "busy", "cached", "revision", "gameCount"):
            self.assertEqual(u[key], want[key], f"GET /api/v1/catalog/updates field {key}")
        self.assertEqual(u["gameCount"], len(first_seen(o["gameId"] for o in self.golden)))
        self.assertIs(got["refresh_catalogue"]["values"]["busy"], True, "POST /api/v1/catalog/updates: busy")

    def test_07_art_favourites_settings(self):
        got = self.by_call()
        for call in ("art:cover", "art:hero"):
            c = got[call]
            self.assertIn(c["values"]["firstStatus"], (200, 202), f"{c['path']}: first reply")
            self.assertIs(c["values"]["pending"], False, f"{c['path']}: still pending after {c['counts']['polls']} polls")
            self.assertIn(c["values"]["contentType"], ("image/webp", "image/png", "image/jpeg"))
            self.assertGreater(c["counts"]["bytes"], 0)
        self.assertEqual(got["favourites"]["values"]["ids"], [])
        self.assertEqual(got["set_favourite"]["values"]["ids"], [GAME])
        self.assertEqual(self.after["favorites"], [GAME])
        drives = [d["id"] for d in self.wire["storage"]]
        self.assertEqual(got["storage"]["values"]["ids"], drives, "GET /api/v1/storage: drive ids")
        self.assertEqual(got["storage"]["values"]["labels"], [d["label"] for d in self.wire["storage"]])
        self.assertEqual(got["storage"]["counts"]["external"], sum(1 for d in self.wire["storage"] if d["external"]))
        self.assertEqual(got["storage"]["counts"]["withSpace"], len(drives), "freeBytes/totalBytes as the app reads them")
        self.assert_items("GET /api/v1/storage", got["storage"]["values"]["drives"], self.wire["storage"], DRIVE)
        self.assertEqual(got["set_preferred_storage"]["values"]["preferredStorage"], drives[0])
        self.assertEqual(got["set_download_limit"]["values"]["concurrentDownloads"], 2)
        self.assertIs(got["set_delete_archives"]["values"]["deleteArchives"], True)
        self.assertEqual((self.after["system"]["preferredStorage"], self.after["system"]["concurrentDownloads"]),
                         (drives[0], 2))

    def test_08_debrid_and_browser(self):
        got = self.by_call()
        d, want = got["debrid"]["values"], self.wire["debrid"]
        self.assertEqual((d["available"], d["connected"], d["busy"]),
                         tuple(flag(want, k) for k in ("available", "connected", "busy")) if want.get("provider") == "torbox"
                         else (False, False, False), "GET /api/v1/debrid: provider must be \"torbox\" for the app to read it")
        self.assertIs(d["connected"], False)
        hosts = [{"sourceId": txt(h, "sourceId"), "maxFileSize": num(h, "maxFileSize"),
                  "available": flag(h, "available") and isinstance(h.get("maxFileSize"), (int, float))
                  and not isinstance(h.get("maxFileSize"), bool) and h["maxFileSize"] >= 0}
                 for h in want.get("hosts", [])]
        self.assertEqual(d["hosts"], hosts, "GET /api/v1/debrid hosts[] as the app reads them")
        b = got["browser_session"]["values"]
        self.assertEqual((b["available"], b["active"], b["state"], b["message"]), (False, False, "idle", S_BROWSER))

    def test_09_download_walk(self):
        got = self.by_call()
        for wline in self.waits:
            self.assertIn(wline["got"], wline["want"].split("|"),
                          f"job {wline['job']}: waited {wline['ms']} ms for {wline['want']}, saw {wline['got']}")
        job = got["create_download"]["values"]["id"]
        self.assertRegex(job, r"^j_[0-9a-f]{6}$", "POST /api/v1/downloads: id as the app reads it")
        opt = next(o for o in self.golden if o["id"] == SLOW_RELEASE)
        row = got["job_action:pause"]["values"]["job"]
        want = {"id": job, "releaseId": SLOW_RELEASE, "gameId": opt["gameId"], "titleId": opt["titleId"],
                "storageId": got["set_preferred_storage"]["values"]["preferredStorage"], "filename": opt["filename"],
                "format": opt["format"], "status": "paused", "error": "", "delivery": "direct",
                "total": opt["sizeBytes"]}
        for key, value in want.items():
            self.assertEqual(row[key], value, f"POST /api/v1/downloads/{job}/pause job field {key}: "
                                              f"expected {value!r}, the app read {row[key]!r}")
        self.assertIn(got["job_action:resume"]["values"]["job"]["status"], ("queued", "downloading"))
        self.assertEqual(got["job_action:cancel"]["values"]["job"]["status"], "cancelled")
        self.assertEqual(got["job_action:forget"]["values"]["ids"], [])
        done = got["create_download:complete"]["values"]["id"]
        self.assertRegex(done, r"^j_[0-9a-f]{6}$")
        self.assertEqual(got["job_action:remove"]["values"]["ids"], [])
        self.assertEqual(self.after["downloads"], [])
        for call in ("job_action:pause", "job_action:resume", "job_action:cancel"):
            self.assertEqual(got[call]["counts"]["jobs"], got[call]["counts"]["raw"], f"{call}: rows the app drops")

    def test_10_library(self):
        got = self.by_call()
        want = self.wire["library"]
        lib = got["library"]
        self.assertEqual(lib["counts"]["games"], len(want["games"]),
                         f"GET /api/v1/library: games the app keeps {lib['counts']['games']}, sent {len(want['games'])}")
        self.assertEqual(lib["counts"]["raw"], len(want["games"]))
        self.assertGreater(lib["counts"]["games"], 0, "the seeded Library titles")
        self.assertEqual(lib["values"]["titleIds"], [g["titleId"] for g in want["games"]])
        self.assert_items("GET /api/v1/library games[]", lib["values"]["games"], want["games"], LIBRARY_GAME, "titleId")
        self.assertEqual(lib["values"]["status"], want["status"])
        self.assertEqual(lib["values"]["capabilities"], want["capabilities"])
        st = got["library_storage"]
        self.assertEqual(st["values"]["drives"], [d["id"] for d in self.wire["library_storage"]["drives"]])
        self.assertEqual(st["values"]["destinations"], [d["id"] for d in self.wire["library_storage"]["destinations"]])
        for member in ("drives", "destinations"):
            self.assert_items(f"GET /api/v1/library/storage {member}[]", st["values"][member + "Detail"],
                              self.wire["library_storage"][member], LIBRARY_DRIVE)
        act = got["library_action"]["values"]
        self.assertIs(act["hasAction"], True, "POST /api/v1/library/actions: the reply carries the action")
        self.assertEqual(act["actionId"], act["requestId"], "POST /api/v1/library/actions field action.id")
        self.assertIn(act["actionState"], ("queued", "running", "complete"))

    def test_11_updates(self):
        got = self.by_call()
        u = got["service_update"]["values"]
        self.assertEqual((u["available"], u["runningVersion"], u["instanceId"]),
                         (False, self.wire["system"]["version"], self.wire["system"]["instanceId"]))
        t = got["tv_app"]["values"]
        self.assertEqual((t["available"], t["installed"], t["phase"]), (False, "none", "idle"))

    def test_12_save_data_store(self):
        got = self.by_call()
        lst, want = got["save_store"], self.wire["saves"]
        self.assertEqual((lst["counts"]["saves"], lst["counts"]["raw"]), (len(want["saves"]), len(want["saves"])),
                         "GET /api/saves/store: saves the app keeps, saves sent")
        self.assertEqual([r["id"] for r in want["saves"]], ["xk-probe-chapter", SLOW_SAVE])
        self.assert_items("GET /api/saves/store saves[]", lst["values"]["saves"], want["saves"], SAVE_ROW)
        self.assertEqual((lst["values"]["backend"], lst["values"]["backendName"], lst["values"]["building"]),
                         (want["backend"], want["backendName"], want["building"]))
        users = got["save_users"]
        self.assert_items("GET /api/saves/users users[]", users["values"]["users"], self.wire["save_users"]["users"],
                          SAVE_USER)
        self.assertEqual(sum(1 for u in users["values"]["users"] if u["foreground"]), 1, "the signed-in profile")
        inst = got["install_save"]["values"]
        self.assertEqual((inst["save"], inst["type"]), (SLOW_SAVE, "save-install"))
        self.assertIn(inst["state"], ("queued", "running"))
        job = got["save_job"]["values"]
        self.assertEqual((job["id"], job["state"], job["total"]), (inst["id"], "running", 64 * tvworld.MiB))
        self.assertGreater(job["done"], 0, "the job's progress as the app reads it")
        self.assertEqual(got["cancel_save_job"]["values"]["id"], inst["id"])
        st, b = self.w.srv.api("GET", f"/api/saves/jobs/{inst['id']}")
        self.assertEqual((st, b["job"]["state"], b["job"]["errorCode"]), (200, "cancelled", "cancelled"))
        self.assertEqual(got["install_save:unknown"]["values"]["code"], "not_found",
                         "a refusal outside /api/v1: the code of {\"error\": code, \"message\": sentence}")


# --------------------------------------------------------------------------- #
# 2. The update routes with the host switch on
# --------------------------------------------------------------------------- #

@unittest.skipUnless(HAVE_PROBE, SKIP)
class TvContractProbeUpdates(ProbeAsserts, unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.w = tvworld.World("probe-upd", tvapi_updates=True, art=False, files=None,
                              settings={"updates": {"feed": CLOSED + "/updates.json", "autoCheck": False}},
                              env={"XENO_GITHUB_API_BASE": CLOSED + "/gh", "XENO_APP_BASE": CLOSED + "/latest"}).start()
        try:
            cls.rc, cls.calls, cls.waits, cls.summary, cls.stderr = run_probe(
                cls.w, "--only", "session,service_update,service_action,tv_app,tv_app_action")
        except BaseException:
            cls.w.close()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    def test_1_no_parse_failure(self):
        self.assertEqual([c["call"] for c in self.calls],
                         ["session", "service_update", "service_action", "tv_app", "tv_app_action"])
        self.assert_no_parse_failure()

    def test_2_statuses(self):
        got = self.by_call()
        want = {"session": 200, "service_update": 200, "service_action": 202, "tv_app": 200, "tv_app_action": 202}
        self.assertEqual({k: got[k]["status"] for k in want}, want, {k: got[k]["error"] for k in want})
        self.assertIs(got["service_update"]["values"]["available"], True)
        self.assertIs(got["tv_app"]["values"]["available"], True)
        for call in ("service_action", "tv_app_action"):
            v = got[call]["values"]
            self.assertTrue(v["busy"] or v["phase"] in ("checking", "checked", "error"),
                            f"POST {got[call]['path']}: the 202 reply as the app reads it: {v}")


# --------------------------------------------------------------------------- #
# 3. The real revision-10 catalogue through the app's parser
# --------------------------------------------------------------------------- #

@unittest.skipUnless(HAVE_PROBE, SKIP)
class TvContractProbeRealCatalogue(ProbeAsserts, unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.w = tvworld.World("probe-real", catalog="real", featured="real", art=False, files=None).start()
        try:
            cls.golden = cls.w.golden_catalog()
            cls.stats = tv_golden.stats(cls.w.catalog, cls.w.featured)
            cls.rc, cls.calls, cls.waits, cls.summary, cls.stderr = run_probe(
                cls.w, "--only", "session,system,sources,catalog,catalogue_status")
        except BaseException:
            cls.w.close()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    def test_1_no_parse_failure(self):
        self.assertEqual([c["call"] for c in self.calls], ["session", "system", "sources", "catalog", "catalogue_status"])
        self.assert_no_parse_failure()
        self.assertEqual([c["status"] for c in self.calls], [200] * 5)

    def test_2_the_app_builds_the_golden_catalogue(self):
        c = self.by_call()["catalog"]
        self.assert_catalogue(c, self.golden, raw_bytes=self.stats["bytes"])
        self.assertEqual((c["counts"]["releases"], c["counts"]["games"], c["bytes"]),
                         (self.stats["options"], self.stats["games"], self.stats["bytes"]))
        # inside the app's limits: the transfer under its 8 s timeout, the size under 16 MiB
        self.assertLess(c["ms"], 8000)
        sys.stderr.write(f"\n  real catalogue in the app's parser: {c['bytes']} bytes, transfer {c['ms']} ms, "
                         f"parse {c['values']['parseMs']} ms, cards {c['values']['groupMs']} ms\n")

    def test_3_status_and_sources(self):
        got = self.by_call()
        u = got["catalogue_status"]["values"]
        self.assertEqual((u["revision"], u["gameCount"]), (self.w.catalog["revision"], self.stats["games"]))
        by_src = self.stats["optionsBySource"]
        self.assertEqual(got["sources"]["values"]["releaseCounts"],
                         [f"archive={by_src['archive']}/ready", f"vikingfile={by_src['vikingfile']}"])


if __name__ == "__main__":
    if not os.path.exists(harness.BINARY):
        sys.stderr.write("test_tvapi_probe: the host binary is missing (make host)\n")
    unittest.main()
