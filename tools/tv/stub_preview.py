#!/usr/bin/env python3
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
stub_preview.py - a stand-in for app/tv/build/preview/xeno_preview, for
`preview_live.py --dry-run`: it tests the harness (worlds, seeding, fake
loader, proxy, marks, checks, report, contact sheet) without building the app.

Same command line and environment as the real preview (PORT-SPEC 2.9):
    stub_preview.py <assets dir> <output dir> [scenario[,scenario...]|all] [w h]
    stub_preview.py --list     one JSON line per scenario of scenarios.json
    XENO_BACKEND, XENO_SCENARIOS, XENO_LANG, XENO_TODAY, XENO_APP_VERSION,
    XENO_LOADER_PORT, XENO_STARTER_{SAVED,BUNDLED}_{ELF,VERSION}

Per scenario it talks to the backend over HTTP the way the app does (system,
session, sources, catalogue, downloads, storage, library, updates; the art of
the Discover hero and first rail; the Save Data Store's list, profiles and
the scenario's save install, followed to its end), runs the starter for real against
XENO_LOADER_PORT (cold start, crash fallback, restart through POST
/api/v1/updates stop), prints TVMARK lines where the real scenarios do
(library: remove-drive, connection-lost: stop-backend) and waits for the
world to change, writes one synthetic PNG per capture named in
tools/tv/scenarios.json, and prints one TVSTATE line in the real format.

Test switches: XENO_STUB_UNIFORM=<capture> writes that capture as one flat
colour; XENO_STUB_SKIP=<capture> leaves it out; XENO_STUB_START_WAIT=<s> how
long a started copy has to answer (default 5; the app waits 30);
XENO_STUB_NO_LIST=1 makes --list fail (an older preview); XENO_STUB_LIST_DROP=
<a,b> leaves those scenarios out of --list; XENO_STUB_LIST_RENAME=<capture>
lists that capture with a "-renamed" suffix.
Python 3 standard library + PIL.
"""
import json
import os
import socket
import sys
import time
import urllib.parse

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.environ.get("XENO_STUB_TABLE") or os.path.join(HERE, "scenarios.json")
START_UP = ("offline", "no-loader", "cold-start", "crash-fallback")
# The Save Data Store scenarios that install a save, as the real ones do (the
# saves of tools/tv/world.py): (save id, profile index, cancel once bytes arrive).
SAVE_INSTALLS = {"saves-install": ("xk-ember-forge", 1, False), "saves-progress": ("xk-starfall-season", 0, True),
                 "saves-closed": ("xk-ember-forge", 0, False),
                 "saves-running": ("xk-orchard-bloom", 0, False), "saves-garlic": ("xk-ember-forge", 0, False)}
# The installs that end with the details closed: the app's toast names the save.
SAVE_TOASTS = ("saves-closed",)
SAVES = ("saves", "game-saves", "saves-long", "saves-building") + tuple(SAVE_INSTALLS)


class Refused(Exception):
    pass


def http(port, method, path, body=None, token=None, timeout=10.0):
    data = b"" if body is None else json.dumps(body).encode()
    head = [f"{method} {path} HTTP/1.1", f"Host: 127.0.0.1:{port}", "Connection: close", "User-Agent: XenoStoreTV"]
    if token:
        head.append(f"Authorization: Bearer {token}")
    if method != "GET":
        head += ["Content-Type: application/json", f"Content-Length: {len(data)}"]
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    except ConnectionRefusedError as e:
        raise Refused(str(e)) from e
    with s:
        s.sendall(("\r\n".join(head) + "\r\n\r\n").encode() + (data if method != "GET" else b""))
        raw = b""
        while True:
            b = s.recv(65536)
            if not b:
                break
            raw += b
    end = raw.find(b"\r\n\r\n")
    if end < 0:
        raise OSError("connection closed without a reply")
    return int(raw[9:12]), raw[end + 4:]


def jbody(b):
    try:
        return json.loads(b.decode("utf-8"))
    except ValueError:
        return None


def version_tuple(v):
    out = []
    for p in v.strip().split("."):
        out.append(int(p) if p.isdigit() else 0)
    return tuple(out + [0, 0, 0])[:3]


class Stub:
    def __init__(self, out, scenario, table):
        self.out, self.name, self.table = out, scenario, table
        backend = os.environ.get("XENO_BACKEND", "")
        self.port = int(backend.split(":")[1]) if ":" in backend else 9095
        self.lang = os.environ.get("XENO_LANG", "en")
        self.loader = int(os.environ["XENO_LOADER_PORT"]) if os.environ.get("XENO_LOADER_PORT") else 0
        self.token = None
        self.state = {"link": "connecting", "linkError": "", "libraryError": "", "downloadsError": "",
                      "catalogueCount": 0, "toasts": [], "settingError": "", "libraryActionError": "",
                      "favouriteError": "", "browserError": "", "stateHealthy": True, "paired": False, "version": "",
                      "instanceId": "", "setupNeeded": False, "starting": "idle", "startDetail": "",
                      "restart": "idle", "restartDetail": "", "jobs": {}, "saves": "idle", "savesCount": 0,
                      "savesBuilding": False, "savesError": "", "saveInstall": "", "libraryGames": 0, "storageJob": "",
                      "captures": [], "marks": [], "timeouts": [], "errorsSeen": [], "startingSeen": [],
                      "restartSeen": [], "opened": [], "starterLog": []}
        self.t0 = time.time()

    # ---- the app's traffic -------------------------------------------------
    def get(self, path, auth=True):
        return http(self.port, "GET", path, token=self.token if auth else None)

    def system(self):
        st, b = self.get("/api/v1/system", auth=bool(self.token))
        v = jbody(b) or {}
        if st == 200:
            self.state.update(link="online", linkError="", stateHealthy=v.get("stateHealthy", True),
                              version=v.get("version", ""), instanceId=v.get("instanceId", ""))
        return st, v

    def error_of(self, where, st, b):
        if st >= 400:
            msg = (jbody(b) or {}).get("error", f"HTTP {st}") if b else f"HTTP {st}"
            self.state["errorsSeen"].append({"from": where, "error": msg})
            return msg
        return ""

    def poll(self):
        """One poll cycle, as Store's worker runs it."""
        try:
            st, v = self.system()
        except (Refused, OSError) as e:
            self.state.update(link="offline", linkError=f"connection refused ({e.__class__.__name__})")
            return False
        if not v.get("paired") and v.get("localSessionAvailable") and not self.token:
            st, b = self.get("/api/v1/session", auth=False)
            if st == 200:
                self.token = (jbody(b) or {}).get("token")
                self.state["paired"] = bool(self.token)
        st, b = self.get("/api/v1/sources")
        src = jbody(b) or {}
        self.state["setupNeeded"] = st == 200 and not src.get("acknowledged", True)
        st, b = self.get("/api/v1/catalog")
        cat = jbody(b)
        self.catalog = cat if isinstance(cat, list) else []
        self.state["catalogueCount"] = len({o.get("gameId") for o in self.catalog})
        for path, key in (("/api/v1/downloads", "downloadsError"), ("/api/v1/storage", "downloadsError"),
                          ("/api/v1/favorites", ""), ("/api/v1/library", "libraryError"), ("/api/v1/updates", ""),
                          ("/api/v1/tv-app", ""), ("/api/v1/catalog/updates", ""), ("/api/v1/browser", ""),
                          ("/api/v1/debrid", "")):
            st, b = self.get(path)
            msg = self.error_of(path, st, b)
            if key and msg:
                self.state[key] = msg
            if path == "/api/v1/downloads" and st == 200:
                counts = {}
                for j in jbody(b) or []:
                    counts[j.get("status", "")] = counts.get(j.get("status", ""), 0) + 1
                self.state["jobs"] = counts
            if path == "/api/v1/library" and st == 200:
                self.state["libraryGames"] = len((jbody(b) or {}).get("games", []))
        return True

    def saves(self, settle=True, timeout=60.0):
        """GET /api/saves/store as the Saves tab asks it: again every 4 s while the
        store builds its index (the first answer can take it 15 s)."""
        deadline = time.time() + timeout
        while True:
            st, b = http(self.port, "GET", "/api/saves/store", token=self.token, timeout=30.0)
            v = jbody(b) or {}
            if st != 200:
                load = "unsupported" if st == 404 else "failed"
                self.state.update(saves=load, savesError=v.get("message") or f"HTTP {st}")
                self.state["errorsSeen"].append({"from": "saves", "error": self.state["savesError"]})
                return v
            self.state.update(saves="ready", savesCount=len(v.get("saves") or []),
                              savesBuilding=bool(v.get("building")), savesError="")
            if not settle or not v.get("building") or time.time() > deadline:
                return v
            time.sleep(4.0)

    def install_save(self, save, profile, cancel):
        """POST /api/saves/store/{id}/install for a profile, then the job every second."""
        st, b = http(self.port, "GET", "/api/saves/users", token=self.token)
        users = (jbody(b) or {}).get("users") or []
        uid = users[profile]["id"] if 0 <= profile < len(users) else 0
        st, b = http(self.port, "POST", f"/api/saves/store/{urllib.parse.quote(save, safe='')}/install",
                     {"user": uid, "backupFirst": True, "wait": False}, token=self.token, timeout=30.0)
        job = (jbody(b) or {}).get("job") or {}
        if st != 200 or not job.get("id"):
            self.state["saveInstall"] = "refused"
            self.state["errorsSeen"].append({"from": "saveInstall", "error": (jbody(b) or {}).get("message", f"HTTP {st}")})
            return
        deadline = time.time() + 60
        while job.get("state") not in ("done", "failed", "cancelled") and time.time() < deadline:
            if cancel and job.get("state") == "running" and job.get("done", 0) > 0:
                http(self.port, "POST", f"/api/saves/jobs/{job['id']}/cancel", {}, token=self.token)
                cancel = False
            time.sleep(1.0)
            st, b = http(self.port, "GET", f"/api/saves/jobs/{job['id']}", token=self.token)
            job = (jbody(b) or {}).get("job") or job
        state, code = job.get("state", ""), job.get("errorCode", "")
        if state not in ("done", "failed", "cancelled"):
            self.state["timeouts"].append("save install")
        if state == "failed" and job.get("error"):
            self.state["errorsSeen"].append({"from": "saveInstall", "error": job["error"]})
        self.state["saveInstall"] = state if state in ("done", "cancelled") or not code else f"{state}:{code}"

    def wait_ready(self, timeout=30.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.poll() and self.state["link"] == "online":
                return True
            time.sleep(0.5)
        self.state["timeouts"].append("ready")
        return False

    def art(self, gid, kind, timeout=10.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            st, _ = self.get(f"/api/v1/art/{gid}/{kind}")
            if st != 202:
                return st
            time.sleep(0.3)
        return 202

    def discover_art(self):
        ranked = sorted({(o.get("newOnOrbitRank") or 0, o.get("gameId")) for o in self.catalog
                         if (o.get("newOnOrbitRank") or 0) > 0})
        for rank, gid in ranked[:3]:
            self.art(gid, "hero")
        for rank, gid in ranked[:6]:
            self.art(gid, "cover")

    # ---- the starter -------------------------------------------------------
    def log(self, line):
        self.state["starterLog"].append(line)
        print("[XENO-TV] " + line, file=sys.stderr, flush=True)

    def starting(self, s):
        self.state["starting"] = s
        if not self.state["startingSeen"] or self.state["startingSeen"][-1] != s:
            self.state["startingSeen"].append(s)

    def choose(self, avoid_saved=False):
        saved, saved_v = os.environ.get("XENO_STARTER_SAVED_ELF", ""), os.environ.get("XENO_STARTER_SAVED_VERSION", "")
        bundled, bundled_v = os.environ.get("XENO_STARTER_BUNDLED_ELF", ""), os.environ.get("XENO_STARTER_BUNDLED_VERSION", "")

        def ver(p):
            try:
                with open(p, encoding="utf-8") as f:
                    return f.read().strip()
            except OSError:
                return ""
        sv, bv = ver(saved_v), ver(bundled_v)
        if not avoid_saved and os.path.isfile(saved) and sv and (not bv or version_tuple(sv) >= version_tuple(bv)):
            return "saved", saved
        return "bundled", bundled

    def send(self, avoid_saved=False):
        which, path = self.choose(avoid_saved)
        with open(path, "rb") as f:
            data = f.read()
        try:
            s = socket.create_connection(("127.0.0.1", self.loader), timeout=5)
        except OSError:
            self.starting("no_loader")
            self.log(f"start: no ELF loader on :{self.loader}")
            return None
        with s:
            s.sendall(data)
            s.shutdown(socket.SHUT_WR)
            try:
                s.recv(1)
            except OSError:
                pass
        self.log(f"start: sent copy={which} ({len(data)} bytes){' (saved copy did not answer)' if avoid_saved else ''}")
        return which

    def wait_up(self, timeout, other_than=""):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                st, v = self.system()
                if st == 200 and (not other_than or v.get("instanceId") != other_than) and not v.get("stopping"):
                    return True
            except (Refused, OSError):
                pass
            time.sleep(0.5)
        return False

    def wait_down(self, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                self.system()
            except (Refused, OSError):
                return True
            time.sleep(0.3)
        return False

    def mark(self, name):
        self.state["marks"].append(name)
        print(f'TVMARK {{"scenario":{json.dumps(self.name)},"mark":{json.dumps(name)}}}', flush=True)
        time.sleep(0.3)

    # ---- pictures ------------------------------------------------------------
    def capture(self, name):
        if name == os.environ.get("XENO_STUB_SKIP"):
            return
        w, h = 960, 540
        if name == os.environ.get("XENO_STUB_UNIFORM"):
            im = Image.new("RGB", (w, h), (16, 18, 24))
        else:
            im = Image.new("RGB", (w, h), (14, 15, 20))
            d = ImageDraw.Draw(im)
            try:
                font = ImageFont.load_default(size=40)
                small = ImageFont.load_default(size=22)
            except TypeError:
                font = small = ImageFont.load_default()
            d.rectangle([0, 0, w, 60], fill=(30, 30, 38))
            d.text((24, 14), "XENO STORE  (stub preview)", font=small, fill=(212, 175, 55))
            d.text((40, 180), name, font=font, fill=(240, 220, 160))
            d.text((40, 250), f"{self.name} - {self.lang} - link {self.state['link']}", font=small, fill=(200, 200, 210))
            n = self.state["catalogueCount"]
            for i in range(min(n, 8)):
                d.rectangle([40 + i * 110, 330, 140 + i * 110, 470], outline=(212, 175, 55), width=3)
        im.save(os.path.join(self.out, name + ".png"))
        self.state["captures"].append(name)
        print(f"wrote {os.path.join(self.out, name + '.png')}", file=sys.stderr, flush=True)

    # ---- scenarios -------------------------------------------------------------
    def run(self):
        caps = list(self.table["scenarios"].get(self.name, {}).get("captures", []))
        n = self.name
        if n not in START_UP:
            self.wait_ready()
        if n == "offline" or n == "no-loader":
            self.poll()
            if n == "no-loader":
                if self.loader:
                    self.send()
                else:
                    self.starting("no_loader")
        elif n == "cold-start":
            self.poll()
            self.capture(caps.pop(0))
            self.starting("starting")
            self.send()
            self.capture(caps.pop(0))
            if self.wait_up(30):
                self.starting("idle")
                self.poll()
            else:
                self.state["timeouts"].append("ready")
        elif n == "crash-fallback":
            self.poll()
            self.starting("starting")
            self.send()
            wait = float(os.environ.get("XENO_STUB_START_WAIT", "5"))
            if self.wait_up(wait):
                self.state["timeouts"].append("expected the saved copy to fail")
            self.starting("failed")
            self.state["linkError"] = "The saved copy did not answer."
            self.capture(caps.pop(0))
            self.starting("starting")
            self.send(avoid_saved=True)
            if self.wait_up(30):
                self.starting("idle")
                self.state["linkError"] = ""
                self.poll()
            else:
                self.state["timeouts"].append("ready")
        elif n == "setup":
            st, b = self.get("/api/v1/sources")
            src = jbody(b) or {}
            self.capture(caps.pop(0))
            ids = [o["id"] for o in src.get("options", [])]
            st, b = http(self.port, "POST", "/api/v1/sources", {"enabled": ids, "acknowledged": True, "noticeVersion": 1},
                         token=self.token)
            self.error_of("/api/v1/sources", st, b)
            self.poll()
        elif n == "discover":
            self.discover_art()
        elif n == "favourite":
            # Favourite on a game's page, twice: each change shows its toast.
            game = next((o.get("gameId") for o in self.catalog if o.get("gameId")), "")
            for _ in range(2):
                st, b = self.get("/api/v1/favorites")
                saved = game in (jbody(b) or [])
                st, b = http(self.port, "POST", "/api/v1/favorites", {"gameId": game, "favorite": not saved},
                             token=self.token)
                if not self.error_of("/api/v1/favorites", st, b):
                    self.state["toasts"].append("Removed from favourites" if saved else "Added to favourites")
        elif n == "saves-building":
            if not self.saves(settle=False).get("building"):
                self.state["timeouts"].append("saves-building")
            self.capture(caps.pop(0))
            self.saves()
        elif n in SAVES:
            listing = self.saves()
            if n in SAVE_INSTALLS:
                self.install_save(*SAVE_INSTALLS[n])
            if n in SAVE_TOASTS:
                save = SAVE_INSTALLS[n][0]
                name = next((r.get("name", "") for r in listing.get("saves") or [] if r.get("id") == save), save)
                self.state["toasts"].append(f"{name} | {self.state['saveInstall']}")
        elif n == "library":
            self.capture(caps.pop(0))
            self.mark("remove-drive")
            time.sleep(2.5)
            self.poll()
        elif n == "restart":
            self.capture(caps.pop(0))
            before = self.state["instanceId"]
            st, b = http(self.port, "POST", "/api/v1/updates", {"action": "stop", "confirmed": True}, token=self.token)
            self.error_of("/api/v1/updates", st, b)
            self.state["restart"] = "stopping"
            self.state["restartSeen"].append("stopping")
            self.capture(caps.pop(0))
            if not self.wait_down(30):
                self.state["timeouts"].append("restart-stopped")
            self.state["restart"] = "starting"
            self.state["restartSeen"].append("starting")
            self.send()
            if self.wait_up(30, other_than=before):
                self.state["restart"] = "idle"
                self.state["restartSeen"].append("done")
                self.state["toasts"].append("XENO STORE restarted.")
                self.poll()
            else:
                self.state["timeouts"].append("restart-done")
        elif n == "connection-lost":
            self.capture(caps.pop(0))
            self.mark("stop-backend")
            if self.wait_down(30):
                self.state.update(link="offline", linkError="connection refused")
            else:
                self.state["timeouts"].append("offline")
        elif n == "state-unhealthy":
            deadline = time.time() + 30
            while time.time() < deadline and self.state["stateHealthy"]:
                self.poll()
                time.sleep(0.5)
            if self.state["stateHealthy"]:
                self.state["timeouts"].append("state-unhealthy")
        for c in caps:
            self.capture(c)
        self.state["seconds"] = round(time.time() - self.t0, 1)
        line = {"scenario": n, "backend": "live"}
        line.update(self.state)
        print("TVSTATE " + json.dumps(line, ensure_ascii=False, separators=(",", ":")), flush=True)


def list_scenarios(table):
    """--list, in the real preview's format."""
    if os.environ.get("XENO_STUB_NO_LIST"):
        print("unknown option --list", file=sys.stderr)
        return 2
    drop = {x for x in os.environ.get("XENO_STUB_LIST_DROP", "").split(",") if x}
    rename = os.environ.get("XENO_STUB_LIST_RENAME", "")
    own = START_UP + ("setup", "restart", "connection-lost", "state-unhealthy") + SAVES
    for name, sc in table["scenarios"].items():
        if name in drop:
            continue
        caps = [c + "-renamed" if c == rename else c for c in sc.get("captures", [])]
        print(json.dumps({"name": name, "fixture": name not in START_UP, "live": True, "ownWorld": name in own,
                          "waitReady": name not in START_UP, "captures": caps,
                          "marks": sc.get("expect", {}).get("marks", [])}, separators=(",", ":")))
    return 0


def main(argv):
    if len(argv) == 2 and argv[1] == "--list":
        with open(TABLE, encoding="utf-8") as f:
            return list_scenarios(json.load(f))
    if len(argv) < 3:
        print("usage: stub_preview.py <assets dir> <output dir> [scenario[,scenario...]|all]", file=sys.stderr)
        return 2
    out = argv[2]
    os.makedirs(out, exist_ok=True)
    with open(TABLE, encoding="utf-8") as f:
        table = json.load(f)
    wanted = os.environ.get("XENO_SCENARIOS") or (argv[3] if len(argv) > 3 else "all")
    names = [n.strip() for n in wanted.split(",") if n.strip() and n.strip() != "all"]
    for n in names:
        if n not in table["scenarios"]:
            print(f"no scenario named '{n}'", file=sys.stderr)
            return 2
    for n in names:
        Stub(out, n, table).run()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
