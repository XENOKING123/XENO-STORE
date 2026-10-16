#!/usr/bin/env python3
"""
test_hardening.py — Phase 3 C hardening and download durability:

  * the DNS-rebind / Origin / Sec-Fetch request gate (a phone on the LAN passes,
    evil.com is refused) and the nosniff / no-referrer headers,
  * device pairing (console code, show cooldown, strike lockout, bearer token,
    loopback-only /api/session), the paired devices list (names from the
    User-Agent, last seen, unpair one / all others, full slots replace the
    device unused the longest, the console's own session is never listed),
  * the self-healing :9095 listener (forced rebind keeps the server reachable),
  * download durability: the persisted resume cursor never advances past the
    last fsync, even when a flush fails mid-transfer,
  * the exFAT/USB bounded-window download path produces correct bytes,
  * restart into a staged update hands the ELF to the loader after a clean shutdown,
  * the start-up hand-off to a downloaded update: the hand-off marker, an update
    that never came up or crashed right away is set aside, stale ELFs are not
    "staged", the crash report survives the hand-off, a second copy stays quiet,
  * classic ZipCrypto extraction with the community password list (a wrong
    password that passes the check byte moves on to the next), a user
    override password, and a zip-slip refusal.

Each test spins up its own host-binary instance on a temp data dir.
"""
import hashlib
import json
import os
import shutil
import signal
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402
from harness import XenoServer, RangeServerProcess, free_port, MiB  # noqa: E402
import range_server  # noqa: E402


def community_entry_sha():
    return hashlib.sha256(range_server.encrypted_entry_bytes()).hexdigest()


class GateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="xeno-gate.")
        cls.srv = XenoServer(os.path.join(cls.tmp, "data")).start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.stop()

    def host(self, value):
        return {"Host": value}

    def test_phone_without_origin_passes(self):
        # A normal phone browser: Host is the console IP:port, no Origin header.
        st, _, _ = self.srv.raw("GET", "/api/status", headers=self.host(f"192.168.1.20:{self.srv.port}"))
        self.assertEqual(st, 200)

    def test_evil_host_rejected(self):
        for h in ("evil.com", f"evil.com:{self.srv.port}", "attacker.example"):
            st, _, _ = self.srv.raw("GET", "/api/status", headers=self.host(h))
            self.assertEqual(st, 403, f"Host {h} must be refused")

    def test_local_names_pass(self):
        for h in (f"ps5.local:{self.srv.port}", f"console.home.arpa:{self.srv.port}", f"localhost:{self.srv.port}", f"127.0.0.1:{self.srv.port}"):
            st, _, _ = self.srv.raw("GET", "/api/status", headers=self.host(h))
            self.assertEqual(st, 200, f"Host {h} must pass")

    def test_wrong_port_rejected(self):
        st, _, _ = self.srv.raw("GET", "/api/status", headers=self.host("192.168.1.20:1"))
        self.assertEqual(st, 403)

    def test_origin_must_match(self):
        good = f"192.168.1.20:{self.srv.port}"
        st, _, _ = self.srv.raw("GET", "/api/status", headers={"Host": good, "Origin": f"http://{good}"})
        self.assertEqual(st, 200)
        st, _, _ = self.srv.raw("GET", "/api/status", headers={"Host": good, "Origin": "http://evil.com"})
        self.assertEqual(st, 403)

    def test_sec_fetch_site_on_api(self):
        good = f"192.168.1.20:{self.srv.port}"
        for site in ("same-origin", "none"):
            st, _, _ = self.srv.raw("GET", "/api/status", headers={"Host": good, "Sec-Fetch-Site": site})
            self.assertEqual(st, 200, site)
        for site in ("cross-site", "same-site"):
            st, _, _ = self.srv.raw("GET", "/api/status", headers={"Host": good, "Sec-Fetch-Site": site})
            self.assertEqual(st, 403, site)

    def test_security_headers(self):
        st, hdrs, _ = self.srv.raw("GET", "/api/status", headers=self.host(f"192.168.1.20:{self.srv.port}"))
        self.assertEqual(hdrs.get("X-Content-Type-Options"), "nosniff")
        self.assertEqual(hdrs.get("Referrer-Policy"), "no-referrer")

    def test_static_assets_still_served(self):
        st, _, body = self.srv.raw("GET", "/", headers=self.host(f"192.168.1.20:{self.srv.port}"))
        self.assertEqual(st, 200)
        self.assertIn(b"<!", body[:64].lower())


class PairingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="xeno-pair.")
        cls.srv = XenoServer(os.path.join(cls.tmp, "data")).start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.stop()

    def code(self):
        st, b = self.srv.api("GET", "/api/pair")
        self.assertEqual(st, 200)
        self.assertEqual(len(b["code"]), 6)       # loopback peer sees the literal code
        self.assertIn("cooldown", b)
        return b["code"]

    def test_01_session_is_loopback_and_issues_token(self):
        st, b = self.srv.api("GET", "/api/session")
        self.assertEqual(st, 200)
        self.assertEqual(len(b["token"]), 64)
        self.assertTrue(b["pairUrl"].startswith("http://"))
        self.assertEqual(b["port"], self.srv.port)

    def test_02_show_cooldown(self):
        st, b = self.srv.api("POST", "/api/pair/show")
        self.assertEqual(st, 200)
        self.assertTrue(b.get("shown"))
        st, b = self.srv.api("POST", "/api/pair/show")
        self.assertEqual(st, 429)
        self.assertEqual(b["error"], "cooldown")
        self.assertGreater(b["retryAfter"], 0)

    def test_03_redeem_wrong_then_right(self):
        code = self.code()
        st, b = self.srv.api("POST", "/api/pair", {"code": "000000" if code != "000000" else "111111"})
        self.assertEqual(st, 403)
        self.assertEqual(b["error"], "bad_code")
        st, b = self.srv.api("POST", "/api/pair", {"code": code})
        self.assertEqual(st, 200)
        self.assertEqual(len(b["token"]), 64)
        # the web UI's Link page: loopback is "local"; a valid token reads as "linked"
        st, info = self.srv.api("GET", "/api/pair")
        self.assertTrue(info["local"])
        self.assertFalse(info["linked"])
        st, info = self.srv.api("GET", "/api/pair", headers={"Authorization": "Bearer " + b["token"]})
        self.assertTrue(info["linked"])
        st, info = self.srv.api("GET", "/api/pair", headers={"Authorization": "Bearer " + "0" * 64})
        self.assertFalse(info["linked"])

    def test_04_strike_lockout(self):
        # Five wrong codes in a row lock pairing for a minute (429).
        bad = "999999"
        if self.code() == bad:
            bad = "888888"
        got_lock = False
        for _ in range(6):
            st, b = self.srv.api("POST", "/api/pair", {"code": bad})
            if st == 429:
                got_lock = True
                self.assertGreater(b["retryAfter"], 0)
                break
            self.assertEqual(st, 403)
        self.assertTrue(got_lock, "pairing must lock out after repeated wrong codes")

    def test_05_token_authorizes_when_pin_set(self):
        srv = XenoServer(os.path.join(self.tmp, "data2")).start()
        try:
            code = srv.api("GET", "/api/pair")[1]["code"]
            token = srv.api("POST", "/api/pair", {"code": code})[1]["token"]
            srv.api("PUT", "/api/settings", {"pairing": {"pin": "7788"}})
            # no auth → refused
            st, _ = srv.api("POST", "/api/recent", {"id": "a"})
            self.assertEqual(st, 401)
            # bearer token → accepted
            st, _ = srv.api("POST", "/api/recent", {"id": "a"}, headers={"Authorization": f"Bearer {token}"})
            self.assertEqual(st, 200)
            # the PIN still works too
            st, _ = srv.api("POST", "/api/recent", {"id": "b"}, headers={"X-XENO-PIN": "7788"})
            self.assertEqual(st, 200)
        finally:
            srv.stop()

    def test_06_tokens_persist_across_restart(self):
        srv = XenoServer(os.path.join(self.tmp, "data3"))
        srv.start()
        code = srv.api("GET", "/api/pair")[1]["code"]
        token = srv.api("POST", "/api/pair", {"code": code})[1]["token"]
        srv.api("PUT", "/api/settings", {"pairing": {"pin": "4455"}})
        srv.stop()
        srv2 = XenoServer(srv.data_dir, port=srv.port)
        srv2.start()
        try:
            st, _ = srv2.api("POST", "/api/recent", {"id": "c"}, headers={"Authorization": f"Bearer {token}"})
            self.assertEqual(st, 200, "a paired token must survive a restart")
        finally:
            srv2.stop()


IPHONE_UA = ("Mozilla/5.0 (iPhone; CPU iPhone OS 17_5 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) "
             "Version/17.5 Mobile/15E148 Safari/604.1")
WINDOWS_CHROME_UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                     "Chrome/126.0.0.0 Safari/537.36")
PS5_UA = ("Mozilla/5.0 (PlayStation; PlayStation 5/9.00) AppleWebKit/605.1.15 (KHTML, like Gecko) "
          "Version/9.00 Safari/605.1.15")
ANDROID_FIREFOX_UA = "Mozilla/5.0 (Android 14; Mobile; rv:128.0) Gecko/128.0 Firefox/128.0"
MAC_EDGE_UA = ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) "
               "Chrome/126.0.0.0 Safari/537.36 Edg/126.0.0.0")


def bearer(token):
    return {"Authorization": "Bearer " + token}


class PairedDevicesTests(unittest.TestCase):
    """Settings → Pair a device: the paired list (a friendly name from the
    User-Agent, first and last seen, "this device"), unpair one, unpair all
    others, full slots replace the device unused the longest, and the
    console's own session (GET /api/session and /api/v1/session) is never
    listed or removed. Each test runs its own instance."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="xeno-devices.")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.srv = self.start()

    def start(self, port=None):
        srv = XenoServer(os.path.join(self.tmp, "data"), port=port).start()
        self.addCleanup(srv.stop)
        return srv

    def restart(self):
        port = self.srv.port
        self.srv.stop()
        self.srv = self.start(port)

    def pair(self, ua):
        code = self.srv.api("GET", "/api/pair")[1]["code"]
        st, b = self.srv.api("POST", "/api/pair", {"code": code}, headers={"User-Agent": ua})
        self.assertEqual(st, 200, b)
        return b["token"]

    def listing(self, token=None):
        st, b = self.srv.api("GET", "/api/pair/devices", headers=bearer(token) if token else None)
        self.assertEqual(st, 200, b)
        return b

    def devices(self, token=None):
        return self.listing(token)["devices"]

    def linked(self, token):
        return self.srv.api("GET", "/api/pair", headers=bearer(token))[1]["linked"]

    def session(self):
        st, b = self.srv.api("GET", "/api/session")
        self.assertEqual(st, 200, b)
        return b["token"]

    def test_01_list_names_times_and_this_device(self):
        local = self.session()
        before = int(time.time())
        phone = self.pair(IPHONE_UA)
        pc = self.pair(WINDOWS_CHROME_UA)
        desktop = self.pair("XENO-STORE-Desktop/1.3.1")
        self.pair("Go-http-client/1.1")                     # a Desktop release from before it named itself
        self.pair(PS5_UA)
        self.pair(ANDROID_FIREFOX_UA)
        self.pair(MAC_EDGE_UA)
        self.pair("curl/8.5.0")                             # nothing recognisable: no name
        st, _, raw = self.srv.raw("GET", "/api/pair/devices", headers=bearer(phone))
        self.assertEqual(st, 200, raw)
        for tok in (local, phone, pc, desktop):
            self.assertNotIn(tok.encode(), raw, "a token never leaves the console")
        b = json.loads(raw)
        self.assertEqual(b["max"], 16)
        devs = b["devices"]
        self.assertEqual([d["name"] for d in devs], ["iPhone Safari", "Windows Chrome", "XENO STORE Desktop", "XENO STORE Desktop",
                                                     "PS5 browser", "Android Firefox", "Mac Edge", ""])
        self.assertEqual([d["current"] for d in devs], [True] + [False] * 7)
        self.assertEqual(len({d["id"] for d in devs}), 8)
        for d in devs:
            self.assertRegex(d["id"], r"^[0-9a-f]{16}$")
            self.assertGreaterEqual(d["firstSeen"], before)
            self.assertGreaterEqual(d["lastSeen"], d["firstSeen"])
        # the console's own session (the TV app shares it) works and is never listed
        self.assertEqual(self.srv.api("GET", "/api/v1/session")[1]["token"], local)
        self.assertTrue(self.linked(local))
        self.assertEqual(len(self.devices(local)), 8)
        self.assertFalse(any(d["current"] for d in self.devices(local)))
        self.assertFalse(any(d["current"] for d in self.devices()))   # the console's browser has no token
        # a request with a token moves its last seen, and only its own
        time.sleep(1.2)
        seen = {d["name"]: d["lastSeen"] for d in devs[:3]}
        self.srv.api("GET", "/api/status", headers=bearer(pc))
        now = {d["name"]: d for d in self.devices(local)[:3]}
        self.assertGreater(now["Windows Chrome"]["lastSeen"], seen["Windows Chrome"])
        self.assertEqual(now["iPhone Safari"]["lastSeen"], seen["iPhone Safari"])
        # names and times survive a restart, and so does the console's session
        self.restart()
        again = self.devices(pc)
        self.assertEqual([d["name"] for d in again], [d["name"] for d in devs])
        self.assertEqual([d["firstSeen"] for d in again], [d["firstSeen"] for d in devs])
        self.assertEqual([d["current"] for d in again], [False, True] + [False] * 6)
        self.assertEqual(self.session(), local)

    def test_02_unpair_one_and_all_others(self):
        local = self.session()
        phone = self.pair(IPHONE_UA)
        pc = self.pair(WINDOWS_CHROME_UA)
        desktop = self.pair("XENO-STORE-Desktop/1.3.1")
        pc_id = {d["name"]: d["id"] for d in self.devices(phone)}["Windows Chrome"]
        st, b = self.srv.api("POST", "/api/pair/unpair", {"id": pc_id}, headers=bearer(phone))
        self.assertEqual(st, 200, b)
        self.assertEqual([d["name"] for d in b["devices"]], ["iPhone Safari", "XENO STORE Desktop"])
        self.assertFalse(self.linked(pc), "an unpaired token stops working")
        self.assertTrue(self.linked(phone))
        st, b = self.srv.api("POST", "/api/pair/unpair", {"id": pc_id}, headers=bearer(phone))
        self.assertEqual((st, b["error"]), (404, "not_found"))
        st, b = self.srv.api("POST", "/api/pair/unpair", {}, headers=bearer(phone))
        self.assertEqual((st, b["error"]), (400, "bad_request"))
        # the console's own session has no row, and its id unpairs nothing
        local_id = hashlib.sha256(local.encode()).hexdigest()[:16]
        st, b = self.srv.api("POST", "/api/pair/unpair", {"id": local_id}, headers=bearer(phone))
        self.assertEqual(st, 404, b)
        self.assertTrue(self.linked(local))
        # all others: every device but the caller's, never the console's session
        st, b = self.srv.api("POST", "/api/pair/unpair-others", {}, headers=bearer(desktop))
        self.assertEqual(st, 200, b)
        self.assertEqual(b["unpaired"], 1)
        self.assertEqual([(d["name"], d["current"]) for d in b["devices"]], [("XENO STORE Desktop", True)])
        self.assertFalse(self.linked(phone))
        self.assertTrue(self.linked(desktop))
        self.assertTrue(self.linked(local))
        # from the console itself (no token): every listed device goes
        st, b = self.srv.api("POST", "/api/pair/unpair-others", {})
        self.assertEqual(st, 200, b)
        self.assertEqual((b["unpaired"], b["devices"]), (1, []))
        self.assertFalse(self.linked(desktop))
        self.assertTrue(self.linked(local))
        self.assertEqual(self.srv.api("GET", "/api/v1/session")[1]["token"], local)
        st, b = self.srv.api("GET", "/api/v1/system", headers=bearer(local))
        self.assertIs(b["paired"], True)
        # what was unpaired stays unpaired after a restart
        self.restart()
        self.assertEqual(self.devices(), [])
        self.assertFalse(self.linked(phone))
        self.assertEqual(self.session(), local)

    def test_03_unpaired_client_cannot_list_or_unpair(self):
        ip = self.srv.api("GET", "/api/status")[1].get("ip", "127.0.0.1")
        if not ip or ip.startswith("127."):
            self.skipTest("no non-loopback IPv4 on this machine")
        phone = self.pair(IPHONE_UA)
        pc = self.pair(WINDOWS_CHROME_UA)
        pc_id = {d["name"]: d["id"] for d in self.devices(phone)}["Windows Chrome"]

        def lan(method, path, body=None, token=None):
            hdrs = {"Content-Type": "application/json"} if body is not None else {}
            if token:
                hdrs.update(bearer(token))
            req = urllib.request.Request(f"http://{ip}:{self.srv.port}{path}", method=method, headers=hdrs,
                                         data=json.dumps(body).encode() if body is not None else None)
            try:
                with urllib.request.urlopen(req, timeout=10) as r:
                    return r.status, json.loads(r.read())
            except urllib.error.HTTPError as e:
                return e.code, json.loads(e.read())
            except OSError as e:
                self.skipTest(f"cannot reach {ip}: {e}")

        # /api/pair/* skips the PIN gate, so the routes check the caller themselves
        self.srv.api("PUT", "/api/settings", {"pairing": {"pin": "2468"}})
        try:
            for tok in (None, "0" * 64):
                for method, path, body in (("GET", "/api/pair/devices", None), ("POST", "/api/pair/unpair", {"id": pc_id}),
                                           ("POST", "/api/pair/unpair-others", {})):
                    st, b = lan(method, path, body, tok)
                    self.assertEqual((st, b.get("error")), (403, "not_paired"), (method, path, tok, b))
            self.assertTrue(self.linked(pc))
            self.assertTrue(self.linked(phone))
            # a paired phone on the LAN may
            st, b = lan("GET", "/api/pair/devices", token=phone)
            self.assertEqual(st, 200, b)
            self.assertEqual([d["current"] for d in b["devices"]], [True, False])
            st, b = lan("POST", "/api/pair/unpair", {"id": pc_id}, token=phone)
            self.assertEqual(st, 200, b)
            self.assertFalse(self.linked(pc))
        finally:
            self.srv.api("PUT", "/api/settings", {"pairing": {"pin": ""}}, headers={"X-XENO-PIN": "2468"})

    def test_04_full_slots_replace_the_device_unused_longest(self):
        local = self.session()
        toks = [self.pair(WINDOWS_CHROME_UA) for _ in range(16)]
        time.sleep(1.2)
        self.srv.api("GET", "/api/status", headers=bearer(toks[0]))   # the first one paired is in use
        newest = self.pair(IPHONE_UA)                                    # no "All pairing slots are in use"
        self.assertTrue(self.linked(newest))
        self.assertFalse(self.linked(toks[1]), "the device unused the longest makes room")
        self.assertTrue(self.linked(toks[0]), "a device in use stays paired")
        for t in toks[2:]:
            self.assertTrue(self.linked(t))
        self.assertTrue(self.linked(local), "the console's own session never makes room")
        b = self.listing(newest)
        self.assertEqual(len(b["devices"]), 16)
        self.assertEqual(b["devices"][-1]["name"], "iPhone Safari")
        self.assertTrue(b["devices"][-1]["current"])
        self.assertIn("replaced", self.srv.log_text())

    def test_05_an_older_pairing_file_is_kept(self):
        # pairing.json before names and times: tokens[0] is what /api/session and
        # /api/v1/session handed out, so it stays the console's own session
        self.srv.stop()
        a, b = "a1" * 32, "b2" * 32
        with open(os.path.join(self.srv.data_dir, "pairing.json"), "w") as f:
            json.dump({"v": 1, "tokens": [a, b]}, f)
        self.srv = self.start(self.srv.port)
        self.assertEqual(self.session(), a)
        self.assertTrue(self.linked(a))
        devs = self.devices()
        self.assertEqual([(d["name"], d["firstSeen"], d["lastSeen"]) for d in devs], [("", 0, 0)])
        # its first request names it and starts its times
        self.srv.api("GET", "/api/status", headers={**bearer(b), "User-Agent": IPHONE_UA})
        d = self.devices()[0]
        self.assertEqual(d["name"], "iPhone Safari")
        self.assertGreater(d["firstSeen"], 0)
        self.assertGreaterEqual(d["lastSeen"], d["firstSeen"])


PAIRED_DEVICES_SCRIPT = r"""
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const { chromium } = require(process.env.XENO_PLAYWRIGHT);
const base = process.argv[2];
const token = process.argv[3];
const browser = await chromium.launch();
try {
  const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
  // this browser holds the phone's token, as a linked phone would
  await page.addInitScript((tok) => { localStorage.setItem('xs.wizardDone', 'true'); localStorage.setItem('xs.token', JSON.stringify(tok)); }, token);
  await page.goto(base + '/');
  await page.waitForFunction(() => window.XS && XS.router && document.querySelector('#scr-discover'), null, { timeout: 30000 });
  await page.evaluate(() => XS.router.go('settings'));
  // the group repaints when settings and status arrive: read it once it lists `n` devices
  const read = async (n) => (await page.waitForFunction((n) => {
    const sec = document.querySelector('#scr-settings section.set-group[data-group="pair"]');
    const rows = sec ? Array.from(sec.querySelectorAll('.pd-row')) : [];
    if (rows.length !== n) return null;
    return {
      rows: rows.map((r) => ({
        nav: r.getAttribute('data-nav-id'), name: r.querySelector('.name').firstChild.textContent,
        chip: r.querySelector('.name .chip') ? r.querySelector('.name .chip').innerText : '',
        desc: r.querySelector('.desc').innerText, unpair: !!r.querySelector('button[data-nav-id$="-unpair"]') })),
      others: !!sec.querySelector('[data-nav-id="pd-others"]'),
      text: sec.innerText,
    };
  }, n, { timeout: 30000 })).jsonValue();
  const out = { before: await read(4) };
  const pc = out.before.rows.find((r) => r.name === 'Windows Chrome');
  if (!pc) { console.log(JSON.stringify(out)); process.exit(0); }   // the test reports what was listed
  await page.click(`#scr-settings [data-nav-id="${pc.nav}-unpair"]`);
  out.dialog = await page.evaluate(() => document.querySelector('.dialog-wrap .dialog').innerText);
  const answered = page.waitForResponse((r) => new URL(r.url()).pathname === '/api/pair/unpair');
  await page.click('.dialog-wrap [data-nav-id="dlg-1"]');
  out.status = (await answered).status();
  out.after = await read(3);
  console.log(JSON.stringify(out));
} finally {
  await browser.close();
}
"""


class PairedDevicesUiTests(unittest.TestCase):
    """Settings → Pair a device in the UI the console serves: every paired
    device with its name, last seen, "This device" and an Unpair button, the
    16-slot note, and Unpair removing the device."""

    @classmethod
    def setUpClass(cls):
        cls.pw = harness.playwright_module()
        if not cls.pw:
            raise unittest.SkipTest("node with Playwright is not installed")

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="xeno-devices-ui.")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.srv = XenoServer(os.path.join(self.tmp, "data")).start()
        self.addCleanup(self.srv.stop)

    def pair(self, ua):
        code = self.srv.api("GET", "/api/pair")[1]["code"]
        return self.srv.api("POST", "/api/pair", {"code": code}, headers={"User-Agent": ua})[1]["token"]

    def test_list_and_unpair(self):
        phone = self.pair(IPHONE_UA)
        pc = self.pair(WINDOWS_CHROME_UA)
        self.pair(PS5_UA)
        self.pair("curl/8.5.0")
        out = harness.run_ui_script(self, self.pw, self.tmp, PAIRED_DEVICES_SCRIPT, self.srv.base, phone)
        rows = out["before"]["rows"]
        self.assertEqual([r["name"] for r in rows], ["iPhone Safari", "Windows Chrome", "PS5 browser", "Unknown device"], out)
        self.assertEqual([r["chip"] for r in rows], ["This device", "", "", ""])
        for r in rows:
            self.assertTrue(r["unpair"], r)
            self.assertTrue(r["desc"].startswith("Last seen "), r)
        self.assertTrue(out["before"]["others"])
        self.assertIn("Up to 16 phones and PCs stay paired. Pairing one more replaces the device unused the longest.", out["before"]["text"])
        self.assertIn("Unpair Windows Chrome?", out["dialog"])
        self.assertEqual(out["status"], 200)
        self.assertEqual([r["name"] for r in out["after"]["rows"]], ["iPhone Safari", "PS5 browser", "Unknown device"])
        self.assertFalse(self.srv.api("GET", "/api/pair", headers=bearer(pc))[1]["linked"])
        self.assertTrue(self.srv.api("GET", "/api/pair", headers=bearer(phone))[1]["linked"])


class ListenerTests(unittest.TestCase):
    def test_self_healing_rebind(self):
        tmp = tempfile.mkdtemp(prefix="xeno-listener.")
        srv = XenoServer(os.path.join(tmp, "data")).start()
        try:
            self.assertEqual(srv.api("GET", "/api/status")[0], 200)
            srv.proc.send_signal(signal.SIGUSR1)   # host hook: force the watchdog to rebind
            time.sleep(1.5)
            # the server must still answer on the same port after the rebind
            ok = False
            for _ in range(50):
                try:
                    if srv.api("GET", "/api/status")[0] == 200:
                        ok = True
                        break
                except Exception:
                    pass
                time.sleep(0.1)
            self.assertTrue(ok, "server must stay reachable after a listener rebind")
            self.assertIn("rebound", srv.log_text(), "a rebind must be logged")
        finally:
            srv.stop()
            srv.stop()


class DurabilityTests(unittest.TestCase):
    def _sidecar_done_sum(self, path):
        with open(path) as f:
            sc = json.load(f)
        return sum(e[1] - e[0] for e in sc.get("done", [])), sc

    def test_cursor_never_past_fsync(self):
        tmp = tempfile.mkdtemp(prefix="xeno-durable.")
        rs = RangeServerProcess(os.path.join(tmp, "files")).start()
        threshold = 12 * MiB
        srv = XenoServer(os.path.join(tmp, "data"), env={"XENO_HOST_FSYNC_FAIL_AFTER": str(threshold)}).start()
        try:
            # one slow stream so periodic 5 s checkpoints run while bytes arrive
            job = srv.add(url=f"{rs.base}/slow/mid.bin", filename="durable.bin", format="BIN", connections=1)
            j = srv.wait_job(job["id"], None, timeout=60, until=lambda x: x["state"] == "failed")
            self.assertEqual(j["errorCode"], "io_error")
            sidecar = j["path"] + ".xpart.json"
            xpart = j["path"] + ".xpart"
            self.assertTrue(os.path.exists(sidecar), "a sidecar must have been written")
            done_sum, _ = self._sidecar_done_sum(sidecar)
            # The persisted (durable) cursor must never exceed what was fsynced.
            self.assertLessEqual(done_sum, threshold,
                                 "resume cursor advanced past the last successful fsync")
            if os.path.exists(xpart):
                # live bytes on disk ran ahead of the durable cursor we persisted.
                self.assertGreaterEqual(os.path.getsize(xpart), done_sum)
        finally:
            srv.stop()
            rs.kill()


class UsbWindowTests(unittest.TestCase):
    def test_usb_download_is_correct(self):
        tmp = tempfile.mkdtemp(prefix="xeno-usb.")
        rs = RangeServerProcess(os.path.join(tmp, "files")).start()
        srv = XenoServer(os.path.join(tmp, "data"), env={"XENO_HOST_USB": "1"}).start()
        try:
            want = rs.sha256("mid.bin")
            job = srv.add(url=f"{rs.base}/ok/mid.bin", filename="usb.bin", format="BIN", sha256=want, connections=8)
            j = srv.wait_job(job["id"], "done", timeout=90)
            self.assertEqual(j["state"], "done")
            self.assertEqual(harness.sha256_file(j["path"]), want, "USB bounded-window bytes must be correct")
        finally:
            srv.stop()
            rs.kill()

    def _small_chunk_server(self, tmp):
        # 32 KiB USB chunks: a 32 MiB file needs 1,024 of them, twice the
        # 512-slot chunk table, so these runs only pass if slots are recycled.
        return XenoServer(os.path.join(tmp, "data"), env={"XENO_HOST_USB": "1", "XENO_HOST_USB_CHUNK": str(32 * 1024)}).start()

    def test_usb_more_chunks_than_the_table(self):
        tmp = tempfile.mkdtemp(prefix="xeno-usb-many.")
        rs = RangeServerProcess(os.path.join(tmp, "files")).start()
        srv = self._small_chunk_server(tmp)
        try:
            want = rs.sha256("mid.bin")
            job = srv.add(url=f"{rs.base}/ok/mid.bin", filename="many.bin", format="BIN", sha256=want, connections=8)
            j = srv.wait_job(job["id"], ("done", "failed"), timeout=120)
            self.assertEqual(j["state"], "done", j)
            self.assertEqual(os.path.getsize(j["path"]), 32 * MiB)
            self.assertEqual(harness.sha256_file(j["path"]), want)
        finally:
            srv.stop()
            rs.kill()

    def test_usb_resume_past_the_table_is_complete(self):
        # Regression: a resumed USB plan once emitted one chunk per MiB of every
        # gap and silently dropped everything past slot 512, so the job ended
        # "done" with a short file.
        tmp = tempfile.mkdtemp(prefix="xeno-usb-resume.")
        rs = RangeServerProcess(os.path.join(tmp, "files")).start()
        srv = self._small_chunk_server(tmp)
        try:
            want = rs.sha256("mid.bin")
            job = srv.add(url=f"{rs.base}/slow/mid.bin", filename="resume.bin", format="BIN", sha256=want, connections=8)
            jid = job["id"]
            deadline = time.time() + 60
            while time.time() < deadline and srv.job(jid)["written"] < 2 * MiB:
                time.sleep(0.1)
            st, b = srv.action(jid, "pause")
            self.assertEqual(st, 200, b)
            srv.wait_job(jid, "paused", timeout=30)
            st, b = srv.action(jid, "resume")
            self.assertEqual(st, 200, b)
            j = srv.wait_job(jid, ("done", "failed"), timeout=180)
            self.assertEqual(j["state"], "done", j)
            self.assertEqual(os.path.getsize(j["path"]), 32 * MiB, "a resumed USB download must not end short")
            self.assertEqual(harness.sha256_file(j["path"]), want)
        finally:
            srv.stop()
            rs.kill()


class PageResolveTests(unittest.TestCase):
    """"Get links" on a game page from a catalog page source: direct files and file-host
    pages are listed, navigation links are not."""

    def test_generic_page_lists_files_and_file_hosts(self):
        import http.server, threading
        page = (b'<html><body><a href="/">Home</a><a href="/category/ps5/">PS5</a>'
                b'<a href="/files/PPSA01234-game.pkg">Download PKG</a>'
                b'<a href="https://www.mediafire.com/file/abc123/PPSA01234.rar/file">Mediafire</a>'
                b'<a href="https://www.mediafire.com/">Mediafire home</a>'
                b'<a href="https://akirabox.com/x9Yz/file">AkiraBox</a>'
                b'<a href="https://twitter.com/share">Share</a></body></html>')

        class H(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", str(len(page)))
                self.end_headers()
                self.wfile.write(page)

            def log_message(self, *a):
                pass
        hs = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=hs.serve_forever, daemon=True).start()
        tmp = tempfile.mkdtemp(prefix="xeno-resolve.")
        srv = XenoServer(os.path.join(tmp, "data"), env={"XENO_HOST_RESOLVE_HOST": "127.0.0.1"}).start()
        try:
            url = "http://127.0.0.1:%d/some-game-ps5/" % hs.server_address[1]
            st, b = srv.api("POST", "/api/links/resolve", {"url": url})
            self.assertEqual(st, 200, b)
            self.assertTrue(b["allowed"])
            got = {(l["kind"], l["url"]) for l in b["links"]}
            self.assertEqual(got, {
                ("direct", "http://127.0.0.1:%d/files/PPSA01234-game.pkg" % hs.server_address[1]),
                ("host", "https://www.mediafire.com/file/abc123/PPSA01234.rar/file"),
                ("host", "https://akirabox.com/x9Yz/file"),
            })
        finally:
            srv.stop()
            hs.shutdown()
            shutil.rmtree(tmp, ignore_errors=True)


class UpdateRestartTests(unittest.TestCase):
    """Restart into a downloaded update: the store shuts down cleanly, frees its
    port and hands the staged ELF to the local ELF loader."""

    def test_restart_hands_staged_elf_to_loader(self):
        import socket, threading
        sink = socket.socket()
        sink.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sink.bind(("127.0.0.1", 0))
        sink.listen(4)
        got = []

        def serve():
            while True:
                try:
                    c, _ = sink.accept()
                except OSError:
                    return
                c.settimeout(10)
                buf = b""
                try:
                    while True:
                        d = c.recv(65536)
                        if not d:
                            break
                        buf += d
                except OSError:
                    pass
                c.close()
                got.append(buf)
        threading.Thread(target=serve, daemon=True).start()
        tmp = tempfile.mkdtemp(prefix="xeno-restart.")
        srv = XenoServer(os.path.join(tmp, "data"), env={"XENO_PORT_LOADER": str(sink.getsockname()[1])}).start()
        try:
            st, b = srv.api("POST", "/api/update/restart", {})
            self.assertEqual(st, 409, b)
            self.assertEqual(b["error"], "nothing_staged")
            elf = b"\x7fELF" + bytes(range(256)) * 64
            with open(os.path.join(srv.data_dir, "xeno-store.elf"), "wb") as f:
                f.write(elf)
            # without a recorded newer version it is a leftover, not an update
            st, b = srv.api("GET", "/api/status")
            self.assertFalse(b["update"]["staged"], b["update"])
            st, b = srv.api("POST", "/api/update/restart", {})
            self.assertEqual(st, 409, b)
            with open(os.path.join(srv.data_dir, "xeno-store.elf.version"), "w") as f:
                f.write("99.0.0\n")
            st, b = srv.api("GET", "/api/status")
            self.assertTrue(b["update"]["staged"], b["update"])
            st, b = srv.api("POST", "/api/update/restart", {})
            self.assertEqual(st, 200, b)
            self.assertTrue(b["restarting"])
            srv.proc.wait(timeout=30)          # clean shutdown, then the hand-off
            self.assertEqual(srv.proc.returncode, 0)
            deadline = time.time() + 10
            while time.time() < deadline and elf not in got:
                time.sleep(0.05)
            self.assertIn(elf, got, "the loader must receive exactly the staged ELF")
            self.assertTrue(os.path.exists(os.path.join(srv.data_dir, "handoff.pending")))
            # the port is free again for the new version
            s2 = socket.socket()
            s2.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s2.bind(("127.0.0.1", srv.port))
            s2.close()
        finally:
            srv.stop()
            sink.close()


class LoaderSink:
    """A fake ELF loader: records every payload sent to it."""

    def __init__(self):
        import socket, threading
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.port = self.sock.getsockname()[1]
        self.got = []
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        while True:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            c.settimeout(10)
            buf = b""
            try:
                while True:
                    d = c.recv(65536)
                    if not d:
                        break
                    buf += d
            except OSError:
                pass
            c.close()
            self.got.append(buf)

    def close(self):
        self.sock.close()


class UpdateHandoffTests(unittest.TestCase):
    """At start, a newer downloaded xeno-store.elf takes over (XENO_HANDOFF=1 on
    host). handoff.pending ("<time> <version that handed off>") is cleared by a
    copy once its server is up, so an update that never comes up, or crashes
    right after its start, is set aside as *.failed instead of retried forever."""

    ELF = b"\x7fELF" + bytes(range(256)) * 64

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="xeno-handoff.")
        srv = XenoServer(os.path.join(cls.tmp, "probe")).start()
        try:
            cls.version = srv.api("GET", "/api/diagnostics")[1]["version"]
        finally:
            srv.stop()

    def setUp(self):
        self.sink = LoaderSink()
        self.data = tempfile.mkdtemp(prefix="data.", dir=self.tmp)
        self.env = {"XENO_HANDOFF": "1", "XENO_PORT_LOADER": str(self.sink.port)}

    def tearDown(self):
        self.sink.close()

    def path(self, name):
        return os.path.join(self.data, name)

    def write(self, name, data):
        with open(self.path(name), "wb" if isinstance(data, bytes) else "w") as f:
            f.write(data)

    def stage(self, version="99.0.0"):
        self.write("xeno-store.elf", self.ELF)
        if version is not None:
            self.write("xeno-store.elf.version", version + "\n")

    def run_to_exit(self, timeout=30):
        import subprocess
        env = dict(os.environ)
        env.update(self.env)
        p = subprocess.run([harness.BINARY, "--port", str(free_port()), "--data", self.data, "--log-level", "debug"],
                           env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", "replace")

    def received(self, timeout=10):
        self.wait_for(lambda: self.sink.got, timeout)
        return self.sink.got

    @staticmethod
    def wait_for(cond, timeout=5):
        # main() finishes its start-up steps a moment after the server first answers
        deadline = time.time() + timeout
        while time.time() < deadline and not cond():
            time.sleep(0.05)
        return cond()

    def assert_set_aside(self):
        self.assertFalse(os.path.exists(self.path("xeno-store.elf")))
        with open(self.path("xeno-store.elf.failed"), "rb") as f:
            self.assertEqual(f.read(), self.ELF)
        self.assertTrue(os.path.exists(self.path("xeno-store.elf.version.failed")))
        self.assertFalse(os.path.exists(self.path("handoff.pending")))

    def test_01_hands_off_and_leaves_crash_report(self):
        self.stage()
        report = f"XENO STORE {self.version} stopped: SIGSEGV (bad memory access)\nthread: http-worker\nuptime: 40 s\n"
        self.write("crash.txt", report)
        rc, out = self.run_to_exit()
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.received(), [self.ELF], "the loader must receive exactly the staged ELF")
        with open(self.path("handoff.pending")) as f:
            ts, frm = f.read().split()
        self.assertLess(abs(time.time() - int(ts)), 60)
        self.assertEqual(frm, self.version)
        # the handed-off copy must still see the report as fresh
        with open(self.path("crash.txt")) as f:
            self.assertEqual(f.read(), report)
        self.assertFalse(os.path.exists(self.path("crash.prev.txt")))

    def test_02_young_marker_from_another_build_runs_this_copy(self):
        # this copy was just handed the ELF (or the .version overstates it): no hand-off loop
        self.stage()
        self.write("handoff.pending", "%d 0.0.1\n" % (int(time.time()) - 5))
        srv = XenoServer(self.data, env=self.env).start()
        try:
            self.assertTrue(self.wait_for(lambda: not os.path.exists(self.path("handoff.pending"))),
                            "a copy that came up clears the marker")
            self.assertTrue(os.path.exists(self.path("xeno-store.elf")))
            self.assertTrue(os.path.exists(self.path("xeno-store.elf.version")))
            st, b = srv.api("GET", "/api/status")
            self.assertTrue(b["update"]["staged"], b["update"])
        finally:
            srv.stop()
        self.assertEqual(self.sink.got, [])

    def test_03_old_marker_sets_update_aside(self):
        self.stage()
        self.write("handoff.pending", "%d 0.0.1\n" % (int(time.time()) - 600))
        srv = XenoServer(self.data, env=self.env).start()
        try:
            self.assert_set_aside()
            st, b = srv.api("GET", "/api/status")
            self.assertFalse(b["update"]["staged"], b["update"])
            self.assertIn("never came up", srv.log_text())
            self.assertIn("toast: XENO STORE 99.0.0 did not start", srv.log_text())
        finally:
            srv.stop()
        self.assertEqual(self.sink.got, [])

    def test_04_same_build_started_again_sets_update_aside(self):
        # Two copies of this build start at once: the second runs this build and
        # leaves the update and the marker to the copy the first one handed off.
        # That copy died before its server came up: the next start retires the update.
        self.stage()
        rc, out = self.run_to_exit()
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.received(), [self.ELF])
        srv = XenoServer(self.data, env=self.env).start()
        try:
            self.assertTrue(os.path.exists(self.path("xeno-store.elf")))
            self.assertTrue(os.path.exists(self.path("handoff.pending")))
            self.assertIn("may still be starting", srv.log_text())
        finally:
            srv.stop()
        self.assertEqual(len(self.sink.got), 1, "no second hand-off")
        with open(self.path("handoff.pending")) as f:
            ts, frm = f.read().split()
        self.write("handoff.pending", "%d %s\n" % (int(ts) - 600, frm))
        srv = XenoServer(self.data, env=self.env).start()
        try:
            self.assert_set_aside()
            self.assertIn("never came up", srv.log_text())
        finally:
            srv.stop()
        self.assertEqual(len(self.sink.got), 1, "no second hand-off")

    def test_05_crash_guard_sets_update_aside(self):
        # 200 s is past the old 120 s window and inside the 300 s safe-start one
        self.stage()
        self.write("crash.txt", "XENO STORE 99.0.0 stopped: SIGSEGV (bad memory access)\nthread: main\nuptime: 200 s\n")
        srv = XenoServer(self.data, env=self.env).start()
        try:
            self.assert_set_aside()
            st, d = srv.api("GET", "/api/diagnostics")
            self.assertTrue(d["safeMode"])
            self.assertTrue(d["crash"]["lastRun"])
            self.assertEqual(d["crash"]["uptime"], 200)
            self.assertFalse(os.path.exists(self.path("crash.txt")))
            self.assertTrue(os.path.exists(self.path("crash.prev.txt")))
        finally:
            srv.stop()
        self.assertEqual(self.sink.got, [])

    def test_06_not_newer_is_not_staged(self):
        cases = ((None, ".old"), ("0.0.1", ".old"), (self.version, None))
        for version, aside in cases:
            with self.subTest(version=version):
                self.data = tempfile.mkdtemp(prefix="data.", dir=self.tmp)
                self.stage(version)
                srv = XenoServer(self.data, env=self.env).start()
                try:
                    st, b = srv.api("GET", "/api/status")
                    self.assertFalse(b["update"]["staged"], b["update"])
                    st, b = srv.api("POST", "/api/update/restart", {})
                    self.assertEqual(st, 409, b)
                    if aside:
                        self.assertFalse(os.path.exists(self.path("xeno-store.elf")))
                        self.assertTrue(os.path.exists(self.path("xeno-store.elf" + aside)))
                    else:   # this copy is that update: the built-in app still needs it next start
                        self.assertTrue(os.path.exists(self.path("xeno-store.elf")))
                finally:
                    srv.stop()
                self.assertEqual(self.sink.got, [])

    def test_07_crash_report_written_whole_and_kept_for_next_start(self):
        srv = XenoServer(self.data).start()
        srv.proc.send_signal(signal.SIGSEGV)
        srv.proc.wait(timeout=10)
        srv.stop()
        with open(self.path("crash.txt")) as f:
            rep = f.read()
        self.assertTrue(rep.startswith(f"XENO STORE {self.version} stopped: SIGSEGV"), rep)
        self.assertTrue(rep.endswith(" s\n"), rep)
        self.assertFalse(os.path.exists(self.path("crash.txt.tmp")))
        srv = XenoServer(self.data).start()
        try:
            st, d = srv.api("GET", "/api/diagnostics")
            self.assertTrue(d["safeMode"])
            self.assertTrue(d["crash"]["lastRun"])
            self.assertFalse(os.path.exists(self.path("crash.txt")))
            with open(self.path("crash.prev.txt")) as f:
                self.assertEqual(f.read(), rep)
        finally:
            srv.stop()

    def test_08_second_copy_stays_quiet(self):
        srv = XenoServer(self.data).start()
        try:
            self.assertTrue(self.wait_for(lambda: "toast: XENO STORE pairing code:" in srv.log_text()))
            log = srv.log_text()
            self.assertLess(log.index("server: listening"), log.index("toast: XENO STORE pairing code:"))
            import subprocess
            p = subprocess.run([harness.BINARY, "--port", str(srv.port), "--data", tempfile.mkdtemp(dir=self.tmp)],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
            out = p.stdout.decode("utf-8", "replace")
            self.assertEqual(p.returncode, 0, out)
            self.assertIn("already running", out)
            self.assertNotIn("pairing code", out, "a second copy must not show a code that pairs nothing")
        finally:
            srv.stop()


class ArchivePasswordTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="xeno-zip.")
        cls.rs = RangeServerProcess(os.path.join(cls.tmp, "files")).start()

    @classmethod
    def tearDownClass(cls):
        cls.rs.kill()

    def _server(self, name):
        srv = XenoServer(os.path.join(self.tmp, name)).start()
        srv.api("PUT", "/api/settings", {"downloads": {"extractZip": True, "deleteArchiveAfterExtract": False}})
        return srv

    def test_community_password_auto(self):
        srv = self._server("data-com")
        try:
            job = srv.add(url=f"{self.rs.base}/ok/secretcom.zip", filename="secretcom.zip", format="ZIP")
            j = srv.wait_job(job["id"], "extracted", timeout=90)
            out = os.path.join(srv.data_dir, "downloads", "homebrew", "PPSA07001 Community.ffpfsc")
            self.assertEqual(j["extract"]["dir"], out)
            self.assertTrue(os.path.exists(out))
            self.assertEqual(harness.sha256_file(out), community_entry_sha())
        finally:
            srv.stop()

    def test_password_list_survives_a_check_byte_clash(self):
        """A wrong community password can pass ZipCrypto's one-byte check (about
        1 time in 128) and then not inflate. The extractor tries the next
        password instead of giving up: in secretclash.zip the first community
        password does exactly that, and the right one is the last."""
        srv = self._server("data-clash")
        try:
            job = srv.add(url=f"{self.rs.base}/ok/secretclash.zip", filename="secretclash.zip", format="ZIP")
            j = srv.wait_job(job["id"], None, timeout=90, until=lambda x: x["extract"]["state"] in ("failed", "extracted"))
            self.assertEqual(j["extract"]["state"], "extracted", j["extract"])
            out = os.path.join(srv.data_dir, "downloads", "homebrew", "PPSA07004 Clash.ffpfsc")
            self.assertEqual(j["extract"]["dir"], out)
            self.assertEqual(harness.sha256_file(out), community_entry_sha())
        finally:
            srv.stop()

    def test_custom_password_required(self):
        srv = self._server("data-pw")
        try:
            # no password + not in the community list → extraction fails cleanly
            job = srv.add(url=f"{self.rs.base}/ok/secretpw.zip", filename="secretpw.zip", format="ZIP")
            j = srv.wait_job(job["id"], None, timeout=90, until=lambda x: x["extract"]["state"] in ("failed", "extracted"))
            self.assertEqual(j["extract"]["state"], "failed")
            self.assertEqual(j["state"], "done")
            # with the right override password → extracts
            job = srv.add(url=f"{self.rs.base}/ok/secretpw.zip", filename="secretpw2.zip", format="ZIP", password="xeno-secret-42")
            j = srv.wait_job(job["id"], "extracted", timeout=90)
            out = os.path.join(srv.data_dir, "downloads", "homebrew", "PPSA07002 Custom.ffpfsc")
            self.assertTrue(os.path.exists(out))
            self.assertEqual(harness.sha256_file(out), community_entry_sha())
        finally:
            srv.stop()

    def test_zip_slip_refused(self):
        srv = self._server("data-slip")
        try:
            job = srv.add(url=f"{self.rs.base}/ok/slip.zip", filename="slip.zip", format="ZIP")
            j = srv.wait_job(job["id"], "extracted", timeout=90)
            root = os.path.join(srv.data_dir, "downloads")
            dump = j["extract"]["dir"]
            # only the safe entries survive; every slip entry is skipped
            self.assertTrue(os.path.exists(os.path.join(dump, "safe", "keep.bin")))
            self.assertTrue(os.path.exists(os.path.join(dump, "safe", "readme.txt")))
            # the evil entries are named slip-*.bin; the legit archive is slip.zip
            for base in (self.tmp, root, os.path.dirname(root), os.path.dirname(self.tmp)):
                for dirpath, _, files in os.walk(base):
                    for f in files:
                        self.assertFalse(f.startswith("slip-"), os.path.join(dirpath, f))
            # the __MACOSX junk entry is dropped, not written
            self.assertFalse(os.path.exists(os.path.join(dump, "__MACOSX")))
        finally:
            srv.stop()


if __name__ == "__main__":
    unittest.main()
