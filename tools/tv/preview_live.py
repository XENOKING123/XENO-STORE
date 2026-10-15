#!/usr/bin/env python3
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
preview_live.py - every screen of the TV app rendered at 1920x1080 against our
real host backend, in a deterministic test world, with automatic checks
(PORT-SPEC 4.1). tools/tv/README.md has the details.

    python3 -I tools/tv/preview_live.py [--out build/tv-preview] [--langs en|all|de,fr]
                                        [--scenarios a,b*] [--groups g,h] [--compare DIR]
                                        [--dry-run] [--keep] [--direct]

For every scenario group of tools/tv/scenarios.json (and every language):
  1. world.py builds a temp world (catalogue, feeds, settings, Library files,
     the starter's copies) and starts what the group needs: the backend
     (build/host/xeno-store), the fake ELF loader, the sentinel loader on the
     backend's own XENO_PORT_LOADER and the recording proxy;
  2. the seeder (below) drives /api/v1 like the app to put the world in its state
     (pairing, sources, the six Downloads rows, a Library copy, ...);
  3. the preview binary (app/tv/build/preview/xeno_preview) runs once with
     XENO_BACKEND=127.0.0.1:<proxy>, XENO_TODAY, XENO_LANG and XENO_SCENARIOS
     = the group's scenarios; TVMARK lines change the world on cue (stop the
     backend, remove a drive), TVSTATE lines end each scenario;
  4. the pictures go to <out>/<lang>/<capture>.png, the logs to <out>/logs/.

Then the six checks run, <out>/report.json and <out>/index.html are written,
and the exit status is 0 when every check passed (1 on a failure, 2 when the
run could not start). The checks (README.md has the details):
  1 captures   every expected picture exists and is not blank
  2 state      TVSTATE has no unexpected linkError/libraryError/downloadsError,
               and nothing the app showed says "unreadable"
  3 backend    no 5xx on /api/v1 (but art 502, and 503 while stopping) and no
               request to /zero/companion
  4 loader     the fake loader received what each scenario sends (cold start
               1, crash fallback 2 with copy=saved then copy=bundled, restart 1
               with the drain in the store log); the store never relaunched itself
  5 art        after Discover, the hero and first rail art was fetched and the
               backend art cache holds no failed entry for it
  6 timing     each scenario ended within its limit (60 s by default) and none
               of its waits ran out

Scenario names: the groups and expectations live in tools/tv/scenarios.json.
When the preview answers `--list`, its own capture and mark names are used
(differences from the table are listed in report.json "scenarioList"), and a
planned scenario it does not have, or cannot run live, is a setup failure.

--langs all re-runs the sweep (scenarios.json "sweep") for the app's 10 other
locales into <out>/<lang>/. --compare DIR adds the mean absolute luma
difference of each picture against the same picture in DIR (a previous run).
--dry-run uses tools/tv/stub_preview.py instead of the app: it tests this
harness with no app build.

Python 3 standard library + PIL. Run under python3 -I.
"""
import argparse
import fnmatch
import json
import os
import queue
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from PIL import Image, ImageChops, ImageStat  # noqa: E402

import contact_sheet  # noqa: E402
import world as world_mod  # noqa: E402

TABLE = os.path.join(HERE, "scenarios.json")
DEFAULT_MAX_SECONDS = 60
UNIFORM_SD = 4.0          # check 1: luma standard deviation must be above this
DETAIL_FRACTION = 0.01    # check 1: more than this share of pixels differs from the background
STOPPING_TEXT = "is stopping"   # check 3: the 503 a stopping store answers (TV_MSG_STOPPING)
COMPARE_FLAG = 6.0        # --compare: mean absolute luma difference that flags a picture
CHECKS = {
    "captures": "1. every capture exists and is not blank",
    "state": "2. no unexpected error in TVSTATE, nothing unreadable",
    "backend": "3. no 5xx on /api/v1 (art 502 and stopping 503 aside), no /zero/companion",
    "loader": "4. loader sends match, drain on restart, no self-relaunch",
    "art": "5. Discover art fetched, no failed art cache entry",
    "timing": "6. scenarios within their time limits, no wait ran out",
}


def log(msg):
    print(f"[tv-live {time.strftime('%H:%M:%S')}] {msg}", file=sys.stderr, flush=True)


# --------------------------------------------------------------------------- #
# Seeder: puts a world into the state its scenarios show, by driving /api/v1
# exactly as the TV app does (PORT-SPEC 4.1): one request per connection,
# `Host: 127.0.0.1:<port>`, a Bearer token from GET /api/v1/session, JSON bodies.
#
# Steps (scenarios.json "seed" lists them per group, in order):
# pair               GET /api/v1/session (the token every other step uses)
# acknowledge        POST /api/v1/sources: every offered source on, notice accepted
# settings           POST /api/v1/settings/downloads {"concurrentDownloads": N}
# downloads          the six Downloads rows, on drive R1:
#                      complete     fx-paper-lanterns-ffpfs  (ok/small.bin)
#                      error        fx-dead-link-ffpfsc      (f404/small.bin: 404)
#                      paused       fx-crown-of-ash-ffpfsc   (slow/small.bin, paused once bytes arrive)
#                      cancelled    fx-velvet-static-ffpfsc  (slow/small.bin, cancel {} keeps the partial)
#                      downloading  fx-long-haul-ffpfsc      (slow/big.bin: 64 MiB at 1 MiB/s)
#                      retrying     fx-second-wind-ffpfsc    (r429/small.bin, kept answering 429 by
#                                                             world.RetryKeeper until the world closes)
# downloads-light    only the throttled download (the restart world drains it)
# finished-download  only the complete row
# library-copy       a Library copy through POST /api/v1/library/actions (a managed game
#                    on one drive to another destination), followed to its end
# block-queue        <data>/queue.json becomes a directory, then a download is created,
#                    so the store cannot save its queue and /system says stateHealthy:false
# --------------------------------------------------------------------------- #

MiB = 1024 * 1024

PLAN = (
    ("complete", "fx-paper-lanterns-ffpfs"),
    ("error", "fx-dead-link-ffpfsc"),
    ("paused", "fx-crown-of-ash-ffpfsc"),
    ("cancelled", "fx-velvet-static-ffpfsc"),
    ("downloading", "fx-long-haul-ffpfsc"),
    ("retrying", "fx-second-wind-ffpfsc"),
)
RETRY_ROUTE = "r429/small.bin"


class SeedError(RuntimeError):
    pass


class Reply:
    def __init__(self, status, body):
        self.status, self.body = status, body

    @property
    def json(self):
        return json.loads(self.body.decode("utf-8"))

    def __repr__(self):
        return f"<{self.status} {self.body[:200]!r}>"


class AppClient:
    """The TV app's transport (APPO/http.cpp): Connection: close, read to EOF."""

    def __init__(self, port, token=None, timeout=30.0):
        self.port, self.token, self.timeout = port, token, timeout

    def request(self, method, path, body=None, token=None):
        tok = self.token if token is None else token
        data = b"" if body is None else json.dumps(body, separators=(",", ":")).encode("utf-8")
        head = [f"{method} {path} HTTP/1.1", f"Host: 127.0.0.1:{self.port}", "Connection: close",
                "Accept: */*", "User-Agent: XenoStoreTV"]
        if tok:
            head.append(f"Authorization: Bearer {tok}")
        if method != "GET":
            head += ["Content-Type: application/json", f"Content-Length: {len(data)}"]
        wire = ("\r\n".join(head) + "\r\n\r\n").encode() + (data if method != "GET" else b"")
        with socket.create_connection(("127.0.0.1", self.port), timeout=self.timeout) as s:
            s.sendall(wire)
            chunks = []
            while True:
                b = s.recv(65536)
                if not b:
                    break
                chunks.append(b)
        raw = b"".join(chunks)
        end = raw.find(b"\r\n\r\n")
        if end < 0:
            raise SeedError(f"{method} {path}: no HTTP reply")
        status = int(raw[9:12])
        return Reply(status, raw[end + 4:])

    def get(self, path, **kw):
        return self.request("GET", path, None, **kw)

    def post(self, path, body=None, **kw):
        return self.request("POST", path, {} if body is None else body, **kw)


def _client(port):
    """tvworld.TvClient when tests/host/tvworld.py is importable (strict JSON),
    else the local AppClient. Both expose .get/.post -> reply.status, reply.json."""
    try:
        return world_mod.deps()["tvworld"].TvClient(port)
    except Exception:  # noqa: BLE001
        return AppClient(port)


class Seeder:
    def __init__(self, world=None, port=None, log=None):
        self.world = world
        self.port = port if port is not None else world.port
        self.c = _client(self.port)
        self.log = log or (lambda msg: print("seed: " + msg, file=sys.stderr))
        self.jobs = {}
        self.storage_id = None

    # ---- helpers --------------------------------------------------------
    def _ok(self, r, *statuses, what=""):
        if r.status not in statuses:
            raise SeedError(f"{what}: HTTP {r.status} {r.body[:300]!r}")
        return r.json if r.body else None

    def _poll(self, what, fn, timeout=30.0, every=0.2):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = fn()
            if last and last[0]:
                return last[1]
            time.sleep(every)
        raise SeedError(f"timeout waiting for {what}; last: {last[1] if last else None!r}"[:600])

    def jobs_list(self):
        return self._ok(self.c.get("/api/v1/downloads"), 200, what="GET /api/v1/downloads")

    def job(self, jid):
        for j in self.jobs_list():
            if j.get("id") == jid:
                return j
        return None

    def wait_job(self, jid, until, what, timeout=30.0):
        def probe():
            j = self.job(jid)
            return (j is not None and until(j)), j
        return self._poll(what, probe, timeout)

    def drive(self):
        if self.storage_id:
            return self.storage_id
        drives = self._ok(self.c.get("/api/v1/storage"), 200, what="GET /api/v1/storage")
        if not drives:
            raise SeedError("GET /api/v1/storage lists no drive")
        pick = drives[0]
        if self.world is not None:
            r1 = os.path.realpath(self.world.roots[0])
            for d in drives:
                if os.path.realpath(str(d.get("path", ""))).startswith(r1):
                    pick = d
                    break
        self.storage_id = pick["id"]
        return self.storage_id

    def create(self, release_id):
        r = self.c.post("/api/v1/downloads", {"releaseId": release_id, "storageId": self.drive()})
        jid = self._ok(r, 201, what=f"POST /api/v1/downloads {release_id}")["id"]
        self.jobs[release_id] = jid
        return jid

    def action(self, jid, action, body=None):
        return self._ok(self.c.post(f"/api/v1/downloads/{jid}/{action}", body or {}), 200,
                        what=f"POST /api/v1/downloads/{jid}/{action}")

    # ---- steps ------------------------------------------------------------
    def step_pair(self):
        v = self._ok(self.c.get("/api/v1/session", token=""), 200, what="GET /api/v1/session")
        self.c.token = v["token"]
        return f"token {v['token'][:8]}..., pair code {v.get('pairCode', '')}"

    def step_acknowledge(self):
        src = self._ok(self.c.get("/api/v1/sources"), 200, what="GET /api/v1/sources")
        ids = [o["id"] for o in src.get("options", [])]
        v = self._ok(self.c.post("/api/v1/sources", {"enabled": ids, "acknowledged": True, "noticeVersion": 1}), 200,
                     what="POST /api/v1/sources")
        return f"enabled {v.get('enabled')}, acknowledged {v.get('acknowledged')}"

    def step_settings(self):
        n = int((self.world.spec if self.world else {}).get("concurrentDownloads", 2))
        v = self._ok(self.c.post("/api/v1/settings/downloads", {"concurrentDownloads": n}), 200,
                     what="POST /api/v1/settings/downloads")
        return f"concurrentDownloads {v.get('concurrentDownloads')}"

    def _seed_job(self, kind, rid):
        jid = self.create(rid)
        if kind == "complete":
            self.wait_job(jid, lambda j: j["status"] == "complete", f"{rid} to complete", 60)
        elif kind == "error":
            self.wait_job(jid, lambda j: j["status"] == "error", f"{rid} to fail", 60)
        elif kind == "paused":
            self.wait_job(jid, lambda j: j["status"] == "downloading" and j.get("received", 0) > 0,
                          f"{rid} to receive bytes")
            self.action(jid, "pause")
            self.wait_job(jid, lambda j: j["status"] == "paused", f"{rid} to pause")
        elif kind == "cancelled":
            self.wait_job(jid, lambda j: j["status"] == "downloading" and j.get("received", 0) > 0,
                          f"{rid} to receive bytes")
            self.action(jid, "cancel", {})
            j = self.wait_job(jid, lambda j: j["status"] == "cancelled", f"{rid} to cancel")
            if not j.get("received", 0) > 0:
                raise SeedError(f"{rid}: cancelled with no partial kept ({j})")
        elif kind == "downloading":
            self.wait_job(jid, lambda j: j["status"] == "downloading", f"{rid} to download")
        elif kind == "retrying":
            self.wait_job(jid, lambda j: j["status"] == "retrying", f"{rid} to wait for a retry", 30)
        return jid

    def _arm_retry(self):
        if self.world is None or self.world.retry_keeper is not None:
            return
        self.world.retry_keeper = world_mod.RetryKeeper(self.world.run.files.base, RETRY_ROUTE).start()

    def step_downloads(self, plan=PLAN):
        done = []
        for kind, rid in plan:
            if kind == "retrying":
                self._arm_retry()
            jid = self._seed_job(kind, rid)
            done.append(f"{kind} {jid}")
        return ", ".join(done)

    def step_downloads_light(self):
        return self.step_downloads((("downloading", "fx-long-haul-ffpfsc"),))

    def step_finished_download(self):
        return self.step_downloads((("complete", "fx-paper-lanterns-ffpfs"),))

    def step_library_copy(self, wait="end"):
        self.c.post("/api/v1/library", {})

        def ready():
            v = self._ok(self.c.get("/api/v1/library"), 200, what="GET /api/v1/library")
            games = [g for g in v.get("games", []) if g.get("managed") and g.get("canManageSource") and g.get("onDrive")]
            return (v.get("status") == "ready" and bool(games)), (v, games)
        lib, games = self._poll("a copyable Library game", ready, 30)
        game = games[0]
        st = self._poll("Library destinations",
                        lambda: (lambda v: (bool(v.get("destinations")), v))(
                            self._ok(self.c.get("/api/v1/library/storage"), 200, what="GET /api/v1/library/storage")),
                        20)
        src_dir = os.path.dirname(game.get("path", ""))
        dests = [d for d in st["destinations"] if not src_dir.startswith(str(d.get("path", "")).rstrip("/") or "\0")]
        dest = (dests or st["destinations"])[0]
        rid = f"tv-seed-{int(time.time() * 1000):x}-1"
        body = {"action": "copy", "titleId": game["titleId"], "sourceKey": game["sourceKey"],
                "destinationId": dest["id"], "confirmed": True, "requestId": rid}
        self._ok(self.c.post("/api/v1/library/actions", body), 202, what="POST /api/v1/library/actions copy")
        terminal = ("completed", "failed", "cancelled")

        def job():
            v = self._ok(self.c.get("/api/v1/library"), 200, what="GET /api/v1/library")
            sj = v.get("storageJob") or {}
            done = sj.get("state") in terminal if wait == "end" else bool(sj)
            return done, sj
        sj = self._poll("the storage job", job, 120, 0.5)
        if sj.get("state") == "failed":
            raise SeedError(f"the Library copy failed: {sj}")
        return f"{game['titleId']} -> {dest.get('label') or dest.get('path')}: {sj.get('state')}"

    def step_block_queue(self):
        if self.world is None:
            raise SeedError("block-queue needs a world (it changes <data>/queue.json)")
        q = os.path.join(self.world.data, "queue.json")
        if os.path.isdir(q):
            shutil.rmtree(q)
        elif os.path.exists(q):
            os.remove(q)
        os.makedirs(os.path.join(q, "blocker"))
        r = self.c.post("/api/v1/downloads", {"releaseId": "fx-paper-lanterns-ffpfs", "storageId": self.drive()})

        def unhealthy():
            v = self._ok(self.c.get("/api/v1/system"), 200, what="GET /api/v1/system")
            return (v.get("stateHealthy") is False), v
        self._poll("stateHealthy:false", unhealthy, 15)
        return f"queue.json blocked; a create answered {r.status}; stateHealthy false"

    # ---- driver ---------------------------------------------------------
    def run(self, steps):
        out = []
        for step in steps:
            t0 = time.time()
            fn = getattr(self, "step_" + step.replace("-", "_"), None)
            rec = {"step": step, "ok": False, "detail": "", "seconds": 0.0}
            try:
                if fn is None:
                    raise SeedError(f"unknown seed step {step!r}")
                rec["detail"] = fn() or ""
                rec["ok"] = True
            except Exception as e:  # noqa: BLE001
                rec["detail"] = f"{e.__class__.__name__}: {e}"
            rec["seconds"] = round(time.time() - t0, 2)
            self.log(f"{step}: {'ok' if rec['ok'] else 'FAILED'} ({rec['seconds']} s) {rec['detail']}")
            out.append(rec)
            if not rec["ok"]:
                break
        return out


# --------------------------------------------------------------------------- #
# Plan
# --------------------------------------------------------------------------- #

def load_table(path):
    with open(path, encoding="utf-8") as f:
        t = json.load(f)
    names = set()
    for g in t["groups"]:
        for s in g["scenarios"]:
            if s not in t["scenarios"]:
                raise SystemExit(f"{path}: group {g['name']} names an unknown scenario {s!r}")
            names.add(s)
    return t


def app_langs():
    d = os.path.join(REPO, "app", "tv", "i18n")
    try:
        return sorted(f[:-5] for f in os.listdir(d) if f.endswith(".json") and f != "en.json")
    except OSError:
        return []


def resolve_langs(arg):
    if arg in ("", "en"):
        return ["en"]
    if arg == "all":
        return ["en"] + app_langs()
    return [x.strip() for x in arg.split(",") if x.strip()]


def matches(name, patterns):
    return any(fnmatch.fnmatchcase(name, p) for p in patterns)


def build_plan(table, langs, scenario_filter=None, group_filter=None):
    """[(lang, group dict, [scenario names])] in run order: every language's groups."""
    plan = []
    for lang in langs:
        for g in table["groups"]:
            if group_filter and g["name"] not in group_filter:
                continue
            names = list(g["scenarios"])
            if scenario_filter:
                names = [n for n in names if matches(n, scenario_filter)]
            elif lang != "en":
                if g.get("sweep") is False:
                    continue
                names = [n for n in names if matches(n, table.get("sweep", []))]
            if names:
                plan.append((lang, g, names))
    return plan


def world_spec(table, group):
    spec = dict(table.get("worldDefaults", {}))
    spec.update(group.get("world", {}))
    return spec


def preview_prefix(args):
    """The preview command without its arguments: the app, or the stub."""
    if args.dry_run:
        return [sys.executable, "-I", os.path.join(HERE, "stub_preview.py")]
    return [args.preview]


def preview_list(prefix):
    """{name: row} from `<preview> --list`: one JSON object per line with
    name, fixture, live, ownWorld, waitReady, captures and marks. None when
    the binary has no --list (it then exits non-zero or prints nothing)."""
    try:
        p = subprocess.run(prefix + ["--list"], stdin=subprocess.DEVNULL, capture_output=True, timeout=60,
                           cwd=REPO)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if p.returncode != 0:
        return None
    rows = {}
    for line in p.stdout.decode("utf-8", "replace").splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            row = json.loads(line)
        except ValueError:
            continue
        if isinstance(row, dict) and isinstance(row.get("name"), str):
            rows[row["name"]] = row
    return rows or None


def reconcile(table, plan, listed):
    """Holds the plan against the preview's own scenario list. Scenarios the
    binary lacks, or cannot run live, are dropped (a setup failure each); the
    capture and mark names come from the binary, and where they differ from
    scenarios.json the difference is noted. Returns (plan, info) with info
    for report.json "scenarioList"."""
    info = {"source": "scenarios.json", "drift": [], "notCovered": [], "failures": []}
    if not listed:
        info["note"] = "the preview has no --list: capture and mark names come from scenarios.json"
        return plan, info
    info["source"] = "preview --list"
    kept = []
    for lang, g, names in plan:
        ok = []
        for n in names:
            row = listed.get(n)
            if row is None:
                info["failures"].append(f"{lang}/{g['name']}: the preview has no scenario {n!r}; not run")
            elif not row.get("live", False):
                info["failures"].append(f"{lang}/{g['name']}: scenario {n!r} does not run against a live backend; "
                                        f"not run")
            else:
                ok.append(n)
        if ok:
            kept.append((lang, g, ok))
    for n in sorted({n for _, _, names in kept for n in names}):
        row, sc = listed[n], table["scenarios"][n]
        caps = [c for c in row.get("captures") or [] if isinstance(c, str)]
        if caps != sc.get("captures", []):
            info["drift"].append({"scenario": n, "what": "captures", "table": sc.get("captures", []), "preview": caps})
            sc["captures"] = caps
        marks = [m for m in row.get("marks") or [] if isinstance(m, str)]
        exp = sc.setdefault("expect", {})
        if marks != exp.get("marks", []):
            info["drift"].append({"scenario": n, "what": "marks", "table": exp.get("marks", []), "preview": marks})
            exp["marks"] = marks
        for m in marks:
            if m not in world_mod.MARKS:
                info["failures"].append(f"{n}: the preview prints TVMARK {m!r}, which world.py does not handle")
    covered = {n for g in table["groups"] for n in g["scenarios"]}
    info["notCovered"] = sorted(n for n, row in listed.items() if row.get("live") and n not in covered)
    return kept, info


# --------------------------------------------------------------------------- #
# Pictures
# --------------------------------------------------------------------------- #

def picture_stats(path):
    """(luma standard deviation, share of pixels whose luma differs from the
    most common value by more than 2)."""
    with Image.open(path) as im:
        lum = im.convert("L")
        sd = ImageStat.Stat(lum).stddev[0]
        hist = lum.histogram()
        total = sum(hist)
        bg = max(range(256), key=lambda i: hist[i])
        near = sum(hist[max(0, bg - 2):min(256, bg + 3)])
        return sd, (total - near) / total if total else 0.0


def luma_diff(a, b):
    """Mean absolute luma difference (0-255) of two pictures; the second is
    resized to the first when the sizes differ."""
    with Image.open(a) as ia, Image.open(b) as ib:
        la, lb = ia.convert("L"), ib.convert("L")
        if lb.size != la.size:
            lb = lb.resize(la.size)
        d = ImageChops.difference(la, lb)
        return ImageStat.Stat(d).mean[0]


# --------------------------------------------------------------------------- #
# One group
# --------------------------------------------------------------------------- #

class GroupRun:
    def __init__(self, args, table, run, lang, group, names, out):
        self.args, self.table, self.run = args, table, run
        self.lang, self.group, self.names = lang, group, names
        self.out = out
        self.variant = group.get("variant", "")
        self.label = f"{lang}/{group['name']}"
        self.logs = os.path.join(out, "logs", lang, group["name"])
        self.lang_dir = os.path.join(out, lang)
        self.run_dir = os.path.join(out, ".runs", lang, group["name"])
        self.spec = world_spec(table, group)
        self.world = None
        self.states = {}          # scenario -> TVSTATE dict
        self.windows = {}         # scenario -> (t0, t1)
        self.pictures = []        # picture records
        self.mark_notes = []
        self.seed = []
        self.setup_failures = []
        self.art_results = {}     # scenario -> list of (gid, kind, status, served path)
        self.preview_rc = None
        self.seconds = 0.0

    # ---- pictures -----------------------------------------------------------
    def _place(self, scenario, capture):
        src = os.path.join(self.run_dir, capture + ".png")
        if not os.path.exists(src):
            return None
        name = capture + (f".{self.variant}" if self.variant else "") + ".png"
        dst = os.path.join(self.lang_dir, name)
        if os.path.exists(dst):            # another scenario of this run took the name
            dst = os.path.join(self.lang_dir, f"{scenario}--{name}")
        shutil.move(src, dst)
        return dst

    def _take(self, scenario, captures):
        for cap in captures:
            dst = self._place(scenario, cap)
            rec = {"lang": self.lang, "group": self.group["name"], "scenario": scenario, "capture": cap,
                   "file": os.path.relpath(dst, self.out).replace(os.sep, "/") if dst else "", "ok": True,
                   "failures": []}
            self.pictures.append(rec)
            self.run.all_pictures.append(rec)

    # ---- the preview process -------------------------------------------------
    def preview_cmd(self):
        return preview_prefix(self.args) + [self.args.assets, self.run_dir, ",".join(self.names)]

    def run_preview(self):
        env = self.world.preview_env(self.lang, self.names, direct=self.args.direct)
        env["XENO_TODAY"] = self.table.get("today", "2026-10-15")
        if self.args.llvmpipe_threads:
            env["LP_NUM_THREADS"] = str(self.args.llvmpipe_threads)
        os.makedirs(self.run_dir, exist_ok=True)
        limit = sum(self.table["scenarios"][n].get("maxSeconds", DEFAULT_MAX_SECONDS) for n in self.names) + 90
        stdout_log = open(os.path.join(self.logs, "preview.out"), "w", encoding="utf-8")
        stderr_log = open(os.path.join(self.logs, "preview.err"), "wb")
        cmd = self.preview_cmd()
        log(f"{self.label}: preview {', '.join(self.names)}")
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=stderr_log, stdin=subprocess.DEVNULL, env=env,
                             cwd=REPO, start_new_session=True)
        lines = queue.Queue()

        def reader():
            for raw in p.stdout:
                lines.put(raw.decode("utf-8", "replace").rstrip("\n"))
            lines.put(None)

        threading.Thread(target=reader, daemon=True).start()
        t_start = time.time()
        current = 0
        prev_end = t_start
        deadline = t_start + limit
        timed_out = False
        try:
            while True:
                try:
                    line = lines.get(timeout=max(0.1, min(1.0, deadline - time.time())))
                except queue.Empty:
                    if time.time() > deadline:
                        timed_out = True
                        break
                    continue
                if line is None:
                    break
                stdout_log.write(line + "\n")
                stdout_log.flush()
                if line.startswith("TVMARK "):
                    self._on_mark(line[7:])
                elif line.startswith("TVSTATE "):
                    try:
                        st = json.loads(line[8:])
                    except ValueError as e:
                        self.setup_failures.append(f"{self.label}: unreadable TVSTATE line ({e})")
                        continue
                    name = st.get("scenario", self.names[min(current, len(self.names) - 1)])
                    now = time.time()
                    self.states[name] = st
                    self.windows[name] = (prev_end, now)
                    prev_end = now
                    current += 1
                    self._take(name, st.get("captures", []))
                    if self.table["scenarios"].get(name, {}).get("expect", {}).get("art"):
                        self.art_results[name] = self.art_check(name)
        finally:
            if timed_out or p.poll() is None:
                if timed_out:
                    self.setup_failures.append(f"{self.label}: the preview ran past {limit} s and was stopped")
                try:
                    os.killpg(p.pid, signal.SIGKILL)
                except OSError:
                    pass
            self.preview_rc = p.wait()
            stdout_log.close()
            stderr_log.close()
        # Pictures of a scenario that never printed TVSTATE (a crash): keep them.
        left = sorted(f[:-4] for f in os.listdir(self.run_dir) if f.endswith(".png"))
        pending = [n for n in self.names if n not in self.states]
        if left:
            owner = pending[0] if pending else "(unknown)"
            self._take(owner, left)
        for n in pending:
            self.windows.setdefault(n, (prev_end, time.time()))
        if self.preview_rc not in (0, None) and not timed_out:
            self.setup_failures.append(f"{self.label}: the preview exited with {self.preview_rc} "
                                       f"(see logs/{self.lang}/{self.group['name']}/preview.err)")

    def _on_mark(self, payload):
        try:
            m = json.loads(payload)
        except ValueError:
            self.setup_failures.append(f"{self.label}: unreadable TVMARK line {payload!r}")
            return
        mark = m.get("mark", "")
        try:
            note = self.world.on_mark(mark)
            self.mark_notes.append({"scenario": m.get("scenario"), "mark": mark, "note": note, "t": time.time()})
            log(f"{self.label}: mark {mark}: {note}")
        except KeyError:
            self.setup_failures.append(f"{self.label}: scenario {m.get('scenario')} asked for an unknown mark {mark!r}")

    # ---- check 5 support (runs right after Discover, while the world is up) --
    def art_expectations(self):
        """(game id, kind, art server path) of the Discover hero and first rail:
        the featured feed's visible hero ids and the first four games by
        newOnOrbitRank, with the URL the backend should end up serving."""
        c = AppClient(self.world.port)
        try:
            c.token = c.get("/api/v1/session", token="").json["token"]
            cat = c.get("/api/v1/catalog").json
        except Exception as e:  # noqa: BLE001
            return c, [], f"cannot read the catalogue: {e}"
        visible = {o.get("gameId") for o in cat if isinstance(o, dict)}
        ranks = sorted({(o.get("newOnOrbitRank") or 0, o.get("gameId")) for o in cat
                        if isinstance(o, dict) and (o.get("newOnOrbitRank") or 0) > 0})
        games = {g["id"]: g for g in self.world.catalog.get("games", [])}
        want = []
        art_base = self.run.art.base
        shared = {}
        for g in games.values():
            if g.get("cover"):
                shared[g["cover"]] = shared.get(g["cover"], 0) + 1

        def served_path(url):
            if not url or not url.startswith(art_base):
                return None
            path = url[len(art_base):]
            name = path.rsplit("/", 1)[-1]
            return path if name in world_mod.art_server.GOOD else None

        hero_ids = [gid for gid in (self.world.featured or {}).get("hero", []) if gid in visible]
        for gid in hero_ids:
            g = games.get(gid, {})
            cands = [g.get("hero") if g.get("hero") != g.get("cover") else None,
                     g.get("heroFallback") if g.get("heroFallback") != g.get("coverFallback") else None]
            path = next((served_path(u) for u in cands if served_path(u)), None)
            if path:
                want.append((gid, "hero", path))
        for _, gid in ranks[:4]:
            g = games.get(gid, {})
            cover = g.get("cover") if shared.get(g.get("cover"), 0) <= 1 else None
            path = next((served_path(u) for u in (cover, g.get("coverFallback")) if served_path(u)), None)
            if path:
                want.append((gid, "cover", path))
        return c, want, ""

    def art_check(self, scenario):
        c, want, err = self.art_expectations()
        out = {"expected": [], "failures": [], "note": err}
        if err:
            out["failures"].append(err)
            return out
        served = {p for p, s in self.run.art.served() if s == 200}
        for gid, kind, path in want:
            rec = {"gameId": gid, "kind": kind, "path": path, "served": path in served, "status": None}
            # The backend's answer now: 200 from the cache (202 = still queued, 502 = a FAILED entry).
            deadline = time.time() + 15
            st = None
            while time.time() < deadline:
                try:
                    st = c.get(f"/api/v1/art/{gid}/{kind}").status
                except Exception as e:  # noqa: BLE001
                    st = f"{e.__class__.__name__}"
                if st != 202:
                    break
                time.sleep(0.3)
            rec["status"] = st
            out["expected"].append(rec)
            if not rec["served"]:
                out["failures"].append(f"{scenario}: the art server never served {path} ({gid} {kind})")
            if st != 200:
                out["failures"].append(f"{scenario}: GET /api/v1/art/{gid}/{kind} answers {st} after Discover"
                                       + (" (a FAILED cache entry)" if st == 502 else ""))
        if not want:
            out["note"] = "no hero or rail art expected from the fixture"
        return out

    # ---- the whole group ------------------------------------------------------
    def execute(self):
        t0 = time.time()
        os.makedirs(self.logs, exist_ok=True)
        os.makedirs(self.lang_dir, exist_ok=True)
        shutil.rmtree(self.run_dir, ignore_errors=True)
        self.run.reset_counters()
        self.world = world_mod.World(self.run, f"{self.lang}-{self.group['name']}", self.spec, self.logs,
                                     keep=self.args.keep)
        try:
            self.world.start()
            steps = list(self.spec.get("seed") or [])
            if steps and self.spec.get("backend", "up") == "up":
                self.seed = Seeder(self.world, log=lambda m: log(f"{self.label}: seed {m}")).run(steps)
                for r in self.seed:
                    if not r["ok"]:
                        self.setup_failures.append(f"{self.label}: seed step {r['step']} failed: {r['detail']}")
            self.run_preview()
        except Exception as e:  # noqa: BLE001
            self.setup_failures.append(f"{self.label}: {e.__class__.__name__}: {e}")
        finally:
            self.collect()
            try:
                self.world.close()
            except Exception as e:  # noqa: BLE001
                self.setup_failures.append(f"{self.label}: closing the world: {e}")
            shutil.rmtree(self.run_dir, ignore_errors=True)
            self.seconds = round(time.time() - t0, 1)
        return self

    def collect(self):
        w = self.world
        self.requests = list(w.proxy.records) if w and w.proxy else []
        self.loader_events = w.loader_events("loader") if w else []
        self.sentinel_events = w.loader_events("sentinel") if w else []
        self.store_log = w.backend_log() if w else ""
        with open(os.path.join(self.logs, "seed.json"), "w", encoding="utf-8") as f:
            json.dump({"seed": self.seed, "marks": self.mark_notes, "states": self.states}, f, indent=1)


# --------------------------------------------------------------------------- #
# The checks
# --------------------------------------------------------------------------- #

def _in(window, t):
    return window[0] <= t < window[1] + 0.5


def check_group(gr, table, results):
    """Appends every failure of this group to results[check]["failures"]."""
    def fail(check, msg):
        results[check]["failures"].append(msg)

    label = gr.label
    for name in gr.names:
        sc = table["scenarios"][name]
        exp = sc.get("expect", {})
        st = gr.states.get(name)
        win = gr.windows.get(name, (0, 0))
        tag = f"{label}/{name}"
        # 1. captures
        expected = sc.get("captures", [])
        mine = [p for p in gr.pictures if p["scenario"] == name]
        have = {p["capture"]: p for p in mine}
        for cap in expected:
            p = have.get(cap)
            if p is None or not p["file"]:
                fail("captures", f"{tag}: capture {cap} is missing")
                gr.pictures.append({"lang": gr.lang, "group": gr.group["name"], "scenario": name, "capture": cap,
                                    "file": "", "ok": False, "failures": ["missing"]})
                continue
        for p in mine:
            if not p["file"]:
                continue
            try:
                sd, detail = picture_stats(os.path.join(gr.out, p["file"]))
            except Exception as e:  # noqa: BLE001
                p["ok"] = False
                p["failures"].append(f"unreadable PNG: {e}")
                fail("captures", f"{tag}: {p['capture']}: unreadable PNG ({e})")
                continue
            p["stddev"], p["detail"] = round(sd, 2), round(detail, 4)
            if sd <= UNIFORM_SD or detail <= DETAIL_FRACTION:
                p["ok"] = False
                p["failures"].append(f"blank: luma sd {sd:.1f}, {detail:.2%} of pixels differ from the background")
                fail("captures", f"{tag}: {p['capture']} looks blank (luma sd {sd:.1f}, detail {detail:.2%})")
            if p["capture"] not in expected:
                p.setdefault("notes", []).append("not in scenarios.json")
        # 2. state
        if st is None:
            fail("state", f"{tag}: no TVSTATE line (the scenario did not finish)")
        else:
            for key, allow in (("linkError", "linkError"), ("libraryError", "libraryError"),
                               ("downloadsError", "downloadsError")):
                if st.get(key) and not exp.get(allow):
                    fail("state", f"{tag}: {key} {st.get(key)!r}")
            texts = list(st.get("toasts", [])) + [e.get("error", "") for e in st.get("errorsSeen", [])] + \
                [str(v) for k, v in st.items() if k.endswith("Error") and v]
            for t in texts:
                if "unreadable" in t.lower():
                    fail("state", f"{tag}: the app showed {t!r}")
            for m in exp.get("marks", []):
                if m not in st.get("marks", []):
                    fail("state", f"{tag}: the scenario never reached its {m} mark")
        # 3. backend
        reqs = [r for r in gr.requests if _in(win, r["t"])]
        bad = {}                                   # (method, path, status) -> [count, first body]
        for r in reqs:
            path = r.get("path", "")
            if "/zero/companion" in path:
                fail("backend", f"{tag}: {r['method']} {path} (the Zero code is removed)")
            if path.startswith("/api/v1/") and r.get("status", 0) >= 500:
                if path.startswith("/api/v1/art/") and r["status"] == 502:
                    continue
                if r["status"] == 503 and exp.get("allow503") and STOPPING_TEXT in r.get("body", ""):
                    continue
                key = (r["method"], path, r["status"])
                bad.setdefault(key, [0, r.get("body", "")[:160]])[0] += 1
        for (method, path, status), (count, body) in bad.items():
            fail("backend", f"{tag}: {method} {path} -> {status} {body!r}" + (f" ({count} times)" if count > 1 else ""))
        # 4. loader
        if "loaderSends" in exp:
            sends = [e for e in gr.loader_events if e.get("event") == "received" and _in(win, e.get("t", 0))]
            if len(sends) != exp["loaderSends"]:
                fail("loader", f"{tag}: the fake loader received {len(sends)} send(s), expected {exp['loaderSends']} "
                               f"({', '.join(str(e.get('bytes')) + ' bytes' for e in sends) or 'none'})")
        if exp.get("starterOrder") and st is not None:
            text = "\n".join(st.get("starterLog", []))
            try:
                with open(os.path.join(gr.logs, "preview.err"), encoding="utf-8", errors="replace") as f:
                    text += "\n" + f.read()
            except OSError:
                pass
            pos = 0
            for needle in exp["starterOrder"]:
                i = text.find(needle, pos)
                if i < 0:
                    fail("loader", f"{tag}: the app log has no {needle!r}" + (" after the earlier start" if pos else ""))
                    break
                pos = i + len(needle)
        if exp.get("drain"):
            lg = gr.store_log
            if "stop drain done" not in lg:
                fail("loader", f"{tag}: the store log shows no finished stop drain")
            if re.search(r"stop drain: \d+ job worker\(s\) still running", lg):
                fail("loader", f"{tag}: the drain stopped with job workers still running")
            if "update: restart requested" in lg or "relaunch failed" in lg:
                fail("loader", f"{tag}: the store tried to relaunch itself")
            starts = len(re.findall(r"XENO STORE \S+ starting", lg))
            if starts < 2:
                fail("loader", f"{tag}: no second instance in the store log ({starts} start(s))")
        # 5. art
        if exp.get("art"):
            ar = gr.art_results.get(name)
            if ar is None:
                fail("art", f"{tag}: the art check did not run (no TVSTATE)")
            else:
                for f_ in ar["failures"]:
                    fail("art", f"{label}/{f_}")
        # 6. timing
        if st is not None:
            limit = sc.get("maxSeconds", DEFAULT_MAX_SECONDS)
            secs = st.get("seconds")
            if isinstance(secs, (int, float)) and secs > limit:
                fail("timing", f"{tag}: took {secs:.1f} s (limit {limit} s)")
            for t in st.get("timeouts", []):
                # Name what the store was doing, so a wait that only runs out on
                # another machine can be read from the CI log alone.
                diag = [ln for ln in gr.store_log.splitlines()
                        if re.search(r"tvlib|tv-storage|storage:|tvapi: thread|stall|deadlock", ln)][-25:]
                extra = "".join("\n        | " + ln[:220] for ln in diag)
                state = f" (libraryGames {st.get('libraryGames')}, libraryError {st.get('libraryError')!r})"
                fail("timing", f"{tag}: a wait ran out ({t}){state}{extra}")
    # Group-wide: the sentinel on the store's own loader port saw nothing.
    got = [e for e in gr.sentinel_events if e.get("event") == "received"]
    if got:
        fail("loader", f"{label}: the store sent {len(got)} ELF(s) to its own loader port (a self-relaunch)")


# --------------------------------------------------------------------------- #
# Compare
# --------------------------------------------------------------------------- #

def compare(out, other, pictures, threshold):
    """Mean absolute luma difference of every picture of this run against the
    same file in `other` (a previous --out). "gone" lists the pictures of
    `other` that this run should have made again but did not: those of the
    languages and scenarios that ran (from other/report.json; without one,
    every picture of the languages that ran)."""
    res = {"against": os.path.abspath(other), "threshold": threshold, "flagged": [], "new": [], "gone": [],
           "pictures": 0}
    seen = set()
    ran = {(p["lang"], p["scenario"]) for p in pictures}
    langs = {p["lang"] for p in pictures}
    for p in pictures:
        if not p.get("file"):
            continue
        seen.add(p["file"])
        theirs = os.path.join(other, p["file"])
        if not os.path.exists(theirs):
            res["new"].append(p["file"])
            continue
        d = luma_diff(os.path.join(out, p["file"]), theirs)
        p["compare"] = round(d, 3)
        res["pictures"] += 1
        if d > threshold:
            res["flagged"].append({"file": p["file"], "diff": round(d, 3)})
    try:
        with open(os.path.join(other, "report.json"), encoding="utf-8") as f:
            before = [p for p in json.load(f).get("pictures", []) if p.get("file")]
        candidates = [p["file"] for p in before if (p.get("lang"), p.get("scenario")) in ran]
    except (OSError, ValueError, AttributeError):
        candidates = []
        for lang in sorted(langs):
            d = os.path.join(other, lang)
            if os.path.isdir(d):
                candidates += [f"{lang}/{f}" for f in sorted(os.listdir(d)) if f.endswith(".png")]
    res["gone"] = sorted({rel for rel in candidates if rel not in seen and os.path.exists(os.path.join(other, rel))})
    res["flagged"].sort(key=lambda x: -x["diff"])
    return res


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #

class Run(world_mod.RunServers):
    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.all_pictures = []


def prepare_out(out, langs):
    os.makedirs(out, exist_ok=True)
    for name in ["logs", "thumbs", ".runs", "report.json", "index.html"] + langs:
        p = os.path.join(out, name)
        if os.path.isdir(p):
            shutil.rmtree(p)
        elif os.path.exists(p):
            os.remove(p)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(REPO, "build", "tv-preview"))
    ap.add_argument("--langs", default="en", help="en (default), all, or a list such as de,fr")
    ap.add_argument("--scenarios", help="only these scenarios (comma list, * allowed)")
    ap.add_argument("--groups", help="only these groups of scenarios.json (comma list)")
    ap.add_argument("--compare", metavar="DIR", help="a previous --out to compare each picture with")
    ap.add_argument("--compare-threshold", type=float, default=COMPARE_FLAG)
    ap.add_argument("--compare-fail", action="store_true", help="a flagged difference fails the run")
    ap.add_argument("--backend-bin", default=os.environ.get("XENO_BIN") or os.path.join(REPO, "build", "host", "xeno-store"))
    ap.add_argument("--preview", default=os.path.join(REPO, "app", "tv", "build", "preview", "xeno_preview"))
    ap.add_argument("--assets", default=os.path.join(REPO, "app", "tv", "assets"))
    ap.add_argument("--table", default=TABLE)
    ap.add_argument("--dry-run", action="store_true", help="tools/tv/stub_preview.py instead of the app")
    ap.add_argument("--direct", action="store_true", help="no recording proxy (check 3 then sees nothing)")
    ap.add_argument("--keep", action="store_true", help="keep the temp worlds")
    ap.add_argument("--llvmpipe-threads", type=int, default=0, help="LP_NUM_THREADS for the preview")
    ap.add_argument("--no-sheet", action="store_true")
    a = ap.parse_args(argv)
    a.out = os.path.abspath(a.out)

    table = load_table(a.table)
    langs = resolve_langs(a.langs)
    scen = [x.strip() for x in a.scenarios.split(",")] if a.scenarios else None
    groups = [x.strip() for x in a.groups.split(",")] if a.groups else None
    plan = build_plan(table, langs, scen, groups)
    if not plan:
        print("preview_live: nothing to run (check --scenarios/--groups/--langs)", file=sys.stderr)
        return 2
    if not os.access(a.backend_bin, os.X_OK):
        print(f"preview_live: no host backend at {a.backend_bin} (make host)", file=sys.stderr)
        return 2
    if not a.dry_run and not os.access(a.preview, os.X_OK):
        print(f"preview_live: no preview binary at {a.preview} (make tv-preview builds it)", file=sys.stderr)
        return 2
    if not a.dry_run and not os.path.isdir(a.assets):
        print(f"preview_live: no app assets at {a.assets}", file=sys.stderr)
        return 2
    try:
        world_mod.deps()
    except world_mod.WorldDependencyError as e:
        print(f"preview_live: {e}", file=sys.stderr)
        return 2
    plan, listing = reconcile(table, plan, preview_list(preview_prefix(a)))
    log(f"scenario names from {listing['source']}"
        + (f"; {len(listing['drift'])} difference(s) from scenarios.json" if listing["drift"] else "")
        + (f"; live scenarios in no group: {', '.join(listing['notCovered'])}" if listing["notCovered"] else ""))
    for f_ in listing["failures"]:
        log(f"setup: {f_}")

    prepare_out(a.out, sorted({lang for lang, _, _ in plan}))
    t0 = time.time()
    run = Run(a.backend_bin, keep=a.keep)
    groups_done = []
    try:
        log(f"servers: files, art and feeds (work dir {run.workdir})")
        run.start()
        for lang, group, names in plan:
            gr = GroupRun(a, table, run, lang, group, names, a.out).execute()
            groups_done.append(gr)
            log(f"{gr.label}: {len(gr.states)}/{len(names)} scenario(s) finished in {gr.seconds} s"
                + (f"; {len(gr.setup_failures)} setup failure(s)" if gr.setup_failures else ""))
    finally:
        run.close()
        shutil.rmtree(os.path.join(a.out, ".runs"), ignore_errors=True)

    results = {k: {"title": v, "ok": True, "failures": []} for k, v in CHECKS.items()}
    for gr in groups_done:
        check_group(gr, table, results)
    setup_failures = list(listing["failures"]) + [f for gr in groups_done for f in gr.setup_failures]
    for k in results:
        results[k]["ok"] = not results[k]["failures"]
    pictures = [p for gr in groups_done for p in gr.pictures]
    report = {
        "ok": all(r["ok"] for r in results.values()) and not setup_failures,
        "mode": "dry-run (stub preview)" if a.dry_run else "live",
        "startedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t0)),
        "seconds": round(time.time() - t0, 1),
        "langs": langs,
        "today": table.get("today"),
        "backend": {"binary": os.path.relpath(a.backend_bin, REPO), "sha256": world_mod.sha256_file(a.backend_bin),
                    "version": world_mod.read_version()},
        "preview": "tools/tv/stub_preview.py" if a.dry_run else os.path.relpath(a.preview, REPO),
        "checks": results,
        "setupFailures": setup_failures,
        "scenarioList": {k: v for k, v in listing.items() if k != "failures"},
        "groups": [{
            "lang": gr.lang, "group": gr.group["name"], "variant": gr.variant, "scenarios": gr.names,
            "seconds": gr.seconds, "previewExit": gr.preview_rc, "seed": gr.seed, "marks": gr.mark_notes,
            "requests": len(gr.requests), "loaderSends": sum(1 for e in gr.loader_events if e.get("event") == "received"),
            "art": gr.art_results, "logs": os.path.relpath(gr.logs, a.out).replace(os.sep, "/"),
            "states": gr.states,
        } for gr in groups_done],
        "pictures": pictures,
    }
    if a.compare:
        cmp_res = compare(a.out, a.compare, pictures, a.compare_threshold)
        report["compare"] = cmp_res
        if a.compare_fail and cmp_res["flagged"]:
            report["ok"] = False
    with open(os.path.join(a.out, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1, ensure_ascii=False)
        f.write("\n")
    if not a.no_sheet:
        path, n = contact_sheet.build(a.out, report)
        log(f"contact sheet: {path} ({n} pictures)")
    for k, r in results.items():
        log(f"check {r['title']}: {'PASS' if r['ok'] else 'FAIL'}"
            + (f" ({len(r['failures'])})" if r["failures"] else ""))
        for f_ in r["failures"][:25]:
            log(f"    {f_}")
    for f_ in setup_failures:
        log(f"setup: {f_}")
    if a.compare:
        log(f"compare: {report['compare']['pictures']} picture(s), {len(report['compare']['flagged'])} over "
            f"{a.compare_threshold}, {len(report['compare']['new'])} new, {len(report['compare']['gone'])} gone")
    log(f"report: {os.path.join(a.out, 'report.json')} -> {'OK' if report['ok'] else 'FAILED'}")
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
