#!/usr/bin/env python3
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
test_tvapi_infra.py — self-tests of the shared /api/v1 test infrastructure
(PORT-SPEC 1.10). Nothing here needs the tvapi subsystems: only the skeleton's
GET /api/v1/system and /session and its gate.

  StrictJsonTests        tvcontract.strict_json: the app's parser rules (APPO/json.cpp)
                         plus our limits, edge by edge.
  ContractTests          the field tables accept every reply of the reference app's own
                         preview backend (tv/reference-replies.json, "app" profile) and
                         reject mutated ones with a precise path; error envelope; routes,
                         gates; request bodies byte for byte.
  GoldenFixtureTests     tv_golden on tv/fixture-catalog.json: one test per coverage row.
  GoldenRealTests        tv_golden on the repository's catalogue: the counts PORT-SPEC quotes.
  ArtGenTests            the generated art set and the art server.
  WorldTests             tvworld against the host binary: the fixture catalogue loads,
                         settings/feed seeding, the app's request bytes, pairing, the
                         normaliser and golden files.
"""
import collections
import copy
import json
import os
import socket
import sys
import tempfile
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "tv"))
import artgen  # noqa: E402
import harness  # noqa: E402
import range_server  # noqa: E402
import tv_golden  # noqa: E402
import tvcontract as tc  # noqa: E402
import tvworld  # noqa: E402

MiB = 1024 * 1024
REFERENCE = os.path.join(HERE, "tv", "reference-replies.json")
HAVE_BINARY = os.path.exists(harness.BINARY)


def fixture():
    return tvworld.load_fixture_catalog("http://127.0.0.1:1", "http://127.0.0.1:2")


def featured():
    return tvworld.load_fixture_featured()


def ids(options):
    return [o["id"] for o in options]


def by_game(options, gid):
    return [o for o in options if o["gameId"] == gid]


# --------------------------------------------------------------------------- #
class StrictJsonTests(unittest.TestCase):
    def ok(self, raw, want=None):
        v = tc.strict_json(raw)
        if want is not None:
            self.assertEqual(v, want)
        return v

    def bad(self, raw, reason):
        with self.assertRaises(tc.StrictJSONError) as cm:
            tc.strict_json(raw)
        self.assertIn(reason, str(cm.exception))
        self.assertIsInstance(cm.exception, AssertionError)

    def test_accepts_valid_documents(self):
        self.ok(b'{"a":[1,2.5,-3,true,false,null,"x"]}', {"a": [1, 2.5, -3, True, False, None, "x"]})
        self.ok(' \t\r\n[] \n', [])
        self.ok(b'"caf\xc3\xa9 \xe2\x9c\xa8"', "café ✨")
        self.ok(b'"\\ud83d\\ude80 \\u00e9 \\" \\\\ \\/ \\b\\f\\n\\r\\t"', "\U0001F680 é \" \\ / \b\f\n\r\t")
        self.ok(str(2 ** 53).encode(), 2 ** 53)
        self.ok(str(-2 ** 53).encode(), -2 ** 53)
        self.ok(b"1e3", 1000.0)

    def test_control_bytes_inside_strings(self):
        for b in (b"\x00", b"\x01", b"\x1f", b"\n", b"\t", b"\r"):
            self.bad(b'{"a":"x' + b + b'y"}', "not strict JSON")
        self.ok(b'{"a":"x\x7fy"}')                         # DEL is not a control byte for the app

    def test_lone_surrogates(self):
        for esc in (b"\\ud800", b"\\udbff", b"\\udc00", b"\\udfff", b"\\ude00\\ud83d", b"\\ud800x"):
            self.bad(b'["' + esc + b'"]', "lone surrogate")
        self.bad(b'{"\\ud800":1}', "lone surrogate")       # keys too
        self.bad(b'"\xed\xa0\x80"', "not UTF-8")           # a UTF-8 encoded surrogate

    def test_trailing_data(self):
        for raw in (b"{} x", b"{}{}", b"[1] 2", b'"a""b"', b"1 2"):
            self.bad(raw, "after the document")

    def test_nesting(self):
        # APPO/json.cpp:166-169: the top value is depth 0 and depth > 64 fails, so 65
        # nested arrays are accepted and 66 are refused.
        self.ok(b"[" * 65 + b"]" * 65)
        self.bad(b"[" * 66 + b"]" * 66, "nesting deeper than 64")
        self.ok(b'{"a":' * 64 + b"1" + b"}" * 64)
        self.bad(b'{"a":' * 65 + b"1" + b"}" * 65, "nesting deeper than 64")
        self.bad(b"[" * 5000 + b"]" * 5000, "nesting deeper than 64")

    def test_numbers_above_2_53(self):
        self.bad(str(2 ** 53 + 1).encode(), "above 2^53")
        self.bad(str(-2 ** 53 - 1).encode(), "above 2^53")
        self.bad(b"1e300", "above 2^53")
        self.bad(b"1e400", "not finite")
        for lit in (b"NaN", b"Infinity", b"-Infinity"):
            self.bad(lit, "NaN/Infinity")

    def test_other_refusals(self):
        self.bad(b"", "empty document")
        self.bad(b"  \n", "empty document")
        self.bad(b"\xef\xbb\xbf{}", "byte order mark")
        self.bad(b"\xff{}", "not UTF-8")
        self.bad(b'{"a":1,"a":2}', "duplicate key")
        for raw in (b"{'a':1}", b"[1,]", b".5", b"+1", b"[1 2]", b"tru", b'{"a"}', b"-"):
            self.bad(raw, "not strict JSON")
        for raw in (b"01", b"1."):                         # APPO/json.cpp refuses both too
            self.bad(raw, "after the document")
        self.assertEqual(tc.strict_json(b'{"a":1,"a":2}', allow_duplicates=True), {"a": 2})

    def test_str_input_and_type_errors(self):
        self.assertEqual(tc.strict_json('{"a":"é"}'), {"a": "é"})
        with self.assertRaises(TypeError):
            tc.strict_json(123)


# --------------------------------------------------------------------------- #
class ContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(REFERENCE, encoding="utf-8") as f:
            cls.ref = json.load(f)["modes"]

    def reference(self, route, mode="normal"):
        for r in self.ref[mode]:
            key = None
            try:
                key = tc.route_key(r["method"], r["path"])
            except KeyError:
                pass
            if key == route and 200 <= r["status"] < 300:
                return tc.strict_json(r["body"])
        raise KeyError(route)

    def errs(self, route, value, profile="app"):
        return tc.reply_errors(route, value, profile)

    def assertFlags(self, route, value, path_fragment, profile="app"):
        e = self.errs(route, value, profile)
        self.assertTrue(any(path_fragment in x for x in e), f"{path_fragment!r} not reported: {e}")

    def test_reference_replies_pass_the_app_profile(self):
        seen, refusals = set(), 0
        for mode, recs in self.ref.items():
            for r in recs:
                if "body" not in r:
                    continue
                try:
                    key = tc.route_key(r["method"], r["path"])
                except KeyError:
                    key = None
                if 200 <= r["status"] < 300 and key in tc.ROUTES:
                    with self.subTest(mode=mode, route=key):
                        v = tc.strict_json(r["body"])
                        self.assertEqual(self.errs(key, v), [])
                        tc.check_reply(key, r["body"].encode("utf-8"), profile="app")
                    seen.add(key)
                elif r["status"] >= 400:
                    with self.subTest(mode=mode, path=r["path"], status=r["status"]):
                        tc.assert_error_envelope(r["status"], r["body"].encode("utf-8"))
                    refusals += 1
        self.assertGreaterEqual(len(seen), 22, sorted(seen))
        self.assertGreaterEqual(refusals, 5)

    def test_reference_differs_from_xeno_promises(self):
        # The reference is another product: the xeno profile must notice it.
        e = self.errs("GET /api/v1/system", self.reference("GET /api/v1/system"), "xeno")
        self.assertTrue(any("$.name" in x for x in e) and any("$.instanceId: missing" in x for x in e), e)

    def test_mutations_are_rejected_with_a_path(self):
        system = self.reference("GET /api/v1/system")
        for key, val, frag in (("paired", 1, "$.paired: expected bool, got int"),
                               ("catalogueRevision", "7", "$.catalogueRevision: expected int, got str"),
                               ("version", 1.2, "$.version: expected str, got float")):
            m = dict(system, **{key: val})
            self.assertFlags("GET /api/v1/system", m, frag)
        cat = self.reference("GET /api/v1/catalog")
        m = copy.deepcopy(cat)
        m[2]["sizeBytes"] = True
        self.assertFlags("GET /api/v1/catalog", m, "$[2].sizeBytes: expected int, got bool")
        m = copy.deepcopy(cat)
        del m[0]["id"]
        self.assertFlags("GET /api/v1/catalog", m, "$[0].id: missing")
        for rank, frag in ((1.5, "$[0].newOnOrbitRank"), (101, "$[0].newOnOrbitRank: 101 is above 100"),
                           (0, "$[0].newOnOrbitRank: 0 is below 1")):
            m = copy.deepcopy(cat)
            m[0]["newOnOrbitRank"] = rank
            self.assertFlags("GET /api/v1/catalog", m, frag)
        self.assertFlags("GET /api/v1/catalog", {"items": cat}, "$: expected array, got object")
        # XENO's additions are promises of the xeno profile (the reference app never reads them)
        mine = tv_golden.build(fixture(), featured())
        self.assertEqual(tc.reply_errors("GET /api/v1/catalog", mine), [])
        for key, val, frag in (("popularRank", 0, "$[0].popularRank: 0 is below 1"),
                               ("popularRank", "1", "$[0].popularRank: expected int, got str"),
                               ("cheats", 0, "$[0].cheats: 0 is below 1"),
                               ("screenshotCount", 11, "$[0].screenshotCount: 11 is above 10"),
                               ("kind", "app", "$[0].kind"),
                               ("kind", "dlc", "$[0].notes: a dlc option's notes start with")):
            m = copy.deepcopy(mine)
            m[0][key] = val
            self.assertFlags("GET /api/v1/catalog", m, frag, "xeno")
        m = copy.deepcopy(mine)
        del m[0]["kind"]
        self.assertFlags("GET /api/v1/catalog", m, "$[0].kind: missing", "xeno")
        m = copy.deepcopy(mine)
        other = next(i for i, o in enumerate(m) if o["gameId"] != m[0]["gameId"])
        m[other]["popularRank"] = m[0]["popularRank"]
        self.assertFlags("GET /api/v1/catalog", m, f"$[{other}].popularRank: {m[0]['popularRank']} is also", "xeno")
        self.assertEqual(self.errs("GET /api/v1/catalog", m), [], "the app profile does not read popularRank")
        session = self.reference("GET /api/v1/session")
        self.assertFlags("GET /api/v1/session", dict(session, token=session["token"][:63]), "$.token: length 63")
        jobs = self.reference("GET /api/v1/downloads")
        m = copy.deepcopy(jobs)
        m[0]["id"] = "zero-1"
        self.assertFlags("GET /api/v1/downloads", m, "$[0].id")
        m = copy.deepcopy(jobs)
        m[0]["received"] = "12"
        self.assertFlags("GET /api/v1/downloads", m, "$[0].received: expected int, got str")
        self.assertFlags("GET /api/v1/browser", {"available": True, "active": True, "id": "", "state": "waiting"},
                         "an active session needs an id")
        self.assertFlags("GET /api/v1/browser", {"state": "sleeping"}, "$.state")
        self.assertFlags("GET /api/v1/debrid", {"provider": "realdebrid"}, "$.provider")
        self.assertFlags("POST /api/v1/settings/downloads", {"concurrentDownloads": 9}, "9 is above 8")
        self.assertEqual(self.errs("POST /api/v1/settings/downloads", {"concurrentDownloads": 5}), [])
        self.assertFlags("POST /api/v1/settings/downloads", {"concurrentDownloads": 5}, "5 is above 4", "xeno")
        lib = self.reference("GET /api/v1/library")
        m = copy.deepcopy(lib)
        m["games"][0]["sizeBytes"] = False
        self.assertFlags("GET /api/v1/library", m, "$.games[0].sizeBytes: expected int|null, got bool")

    def test_check_reply_raises_with_every_problem(self):
        bad = {"name": "XENO STORE", "paired": "yes"}
        with self.assertRaises(AssertionError) as cm:
            tc.check_reply("GET /api/v1/system", bad)
        msg = str(cm.exception)
        self.assertIn("$.paired: expected bool, got str", msg)
        self.assertIn("$.instanceId: missing", msg)
        # bytes are parsed strictly first; a concrete path resolves to its pattern
        with self.assertRaises(tc.StrictJSONError):
            tc.check_reply("POST /api/v1/downloads/j_1a2b3c/pause", b'[{"id":"a\x01"}]')
        self.assertEqual(tc.check_reply("POST /api/v1/downloads/j_1a2b3c/pause", b"[]"), [])
        with self.assertRaises(KeyError):
            tc.check_reply("GET /api/v1/nope", {})

    def test_a_xeno_reply_passes_the_xeno_profile(self):
        system = {"name": "XENO STORE", "version": "1.2.1", "instanceId": "0" * 32, "stopping": False,
                  "platform": "ps5", "paired": True, "localSessionAvailable": True, "stateHealthy": True,
                  "catalogueRevision": 10485760123, "preferredStorage": "usb0", "deleteArchives": True,
                  "concurrentDownloads": 2}
        self.assertEqual(tc.reply_errors("GET /api/v1/system", system), [])
        drive = {"id": "usb0", "label": "USB storage 1", "path": "/mnt/usb0/homebrew", "freeBytes": 1000 * MiB,
                 "totalBytes": 2000 * MiB, "pendingBytes": 10 * MiB, "projectedFreeBytes": 750 * MiB, "external": True}
        self.assertEqual(tc.reply_errors("GET /api/v1/storage", [drive]), [])
        self.assertFlags("GET /api/v1/storage", [dict(drive, label="USB storage 0")], "R4 label", "xeno")
        self.assertFlags("GET /api/v1/storage", [dict(drive, projectedFreeBytes=1)], "240 MiB", "xeno")

    def test_error_envelope(self):
        ok = tc.assert_error_envelope(401, b'{"error":"Pair this device with your PS5 to continue."}',
                                      expect_status=401, sentence=tc.S["unpaired"])
        self.assertEqual(ok["error"], tc.S["unpaired"])
        for status, body, frag in ((401, {"error": "pin_required"}, "machine code"),
                                   (400, {"ok": False, "error": "Expected a JSON object."}, '"ok"'),
                                   (400, {"error": ""}, "no sentence"),
                                   (400, {"error": "   "}, "no sentence"),
                                   (400, {"message": "x"}, "no sentence"),
                                   (400, ["Expected a JSON object."], "not an object"),
                                   (400, {"error": " Padded."}, "stray whitespace"),
                                   (200, {"error": "Fine."}, "is a success"),
                                   (404, b'{"error":"a\x01"}', "not strict JSON")):
            with self.subTest(body=body):
                with self.assertRaises(AssertionError) as cm:
                    tc.assert_error_envelope(status, body)
                self.assertIn(frag, str(cm.exception))
        with self.assertRaises(AssertionError):
            tc.assert_error_envelope(400, {"error": "Expected a JSON object."}, expect_status=409)
        with self.assertRaises(AssertionError):
            tc.assert_error_envelope(400, {"error": "Expected a JSON object."}, sentence="Other.")

    def test_routes_and_gates(self):
        # PORT-SPEC 1.4's table has 31 method+pattern rows (its text says 32); XENO adds the screenshots.
        self.assertEqual(len(tc.ALL_ROUTES), 32)
        self.assertEqual(set(tc.ROUTES) | {"GET /api/v1/zero/companion", "POST /api/v1/zero/companion"},
                         set(tc.GATES))
        self.assertEqual(set(k for k, g in tc.GATES.items() if "B" not in g), set(tc.OPEN_ROUTES))
        for key, path in tc.SAMPLE_PATHS.items():
            self.assertEqual(tc.route_key(key.split()[0], path), key)
        self.assertEqual(tc.route_key("POST", "/api/v1/downloads/j_1a2b3c/cancel?x=1"),
                         "POST /api/v1/downloads/{id}/{action}")
        self.assertEqual(tc.route_key("get", "/api/v1/art/fx-a/hero"), "GET /api/v1/art/{id}/{kind}")
        self.assertEqual(tc.route_key("GET", "/api/v1/art/fx-a/shot/3"), "GET /api/v1/art/{id}/shot/{n}")
        self.assertEqual((tc.GATES["GET /api/v1/art/{id}/shot/{n}"], tc.SUCCESS_STATUS["GET /api/v1/art/{id}/shot/{n}"]),
                         ("SB", 202))
        for bad in (("GET", "/api/v1/nope"), ("DELETE", "/api/v1/system"), ("GET", "/api/v1/downloads/x")):
            with self.assertRaises(KeyError):
                tc.route_key(*bad)
        self.assertEqual(tc.SUCCESS_STATUS["POST /api/v1/downloads"], 201)
        self.assertEqual(tc.SUCCESS_STATUS["POST /api/v1/library/actions"], 202)
        self.assertEqual(tc.GATES["GET /api/v1/updates"], "B")

    def test_app_bodies_byte_for_byte(self):
        B = tc.AppBody
        self.assertEqual(B.save_sources(["archive", "vikingfile"]),
                         '{"enabled":["archive","vikingfile"],"acknowledged":true,"noticeVersion":1}')
        self.assertEqual(B.save_sources([]), '{"enabled":[],"acknowledged":true,"noticeVersion":1}')
        self.assertEqual(B.favourite("fx-a", False), '{"gameId":"fx-a","favorite":false}')
        self.assertEqual(B.create_download("r1", "usb0", torbox=True),
                         '{"releaseId":"r1","storageId":"usb0","delivery":"torbox"}')
        self.assertEqual(B.job_action("cancel", delete_partial=True), '{"deletePartial":true}')
        self.assertEqual(B.job_action("pause"), "{}")
        self.assertEqual(B.job_action("archive-password", password='p"w'), '{"password":"p\\"w"}')
        self.assertEqual(B.library_action("copy", "r-1", title_id="PPSA01234", source_key="k", destination_id="d",
                                          job_id=3, confirmed=True),
                         '{"action":"copy","titleId":"PPSA01234","sourceKey":"k","destinationId":"d","jobId":3,'
                         '"confirmed":true,"requestId":"r-1"}')
        self.assertEqual(B.update("install", version="1.2.2", checksum="ab", confirmed=True),
                         '{"action":"install","version":"1.2.2","checksum":"ab","confirmed":true}')
        self.assertEqual(tc.quote('a"\\\n\r\t\x01é'), '"a\\"\\\\\\n\\r\\t\\u0001é"')
        # the capture used these bodies; every one is strict JSON
        for r in self.ref["normal"]:
            if "request" in r:
                tc.strict_json(r["request"])


# --------------------------------------------------------------------------- #
class GoldenFixtureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat, cls.feat = fixture(), featured()
        cls.out = tv_golden.build(cls.cat, cls.feat)

    def game_ids(self):
        return list(dict.fromkeys(o["gameId"] for o in self.out))

    def test_fixture_documents_every_game(self):
        games = self.cat["games"]
        self.assertGreaterEqual(len(games), 40)
        for g in games:
            self.assertTrue(g.get("_case"), f"{g['id']} has no _case note")
        self.assertEqual(len({g["id"] for g in games}), len(games))
        rids = [r["id"] for g in games for r in g["releases"]]
        self.assertEqual(len(set(rids)), len(rids))
        raw = json.dumps(tv_golden.load_json(tvworld.FIXTURE_CATALOG))
        for key in ("url", "cover", "hero", "coverFallback", "heroFallback"):
            for g in tv_golden.load_json(tvworld.FIXTURE_CATALOG)["games"]:
                for v in [g.get(key)] + [r.get(key) for r in g["releases"]]:
                    if v:
                        self.assertTrue(v.startswith(("{{FILES}}/", "{{ART}}/", "https://")), v)
        self.assertNotIn("127.0.0.1", raw)

    def test_sparse_defaults_encoding(self):
        raw = tv_golden.load_json(tvworld.FIXTURE_CATALOG)
        self.assertIn("defaults", raw)
        self.assertTrue(all(not tv_golden._empty(v) for g in raw["games"] for k, v in g.items()
                            if k in raw["defaults"]["game"]), "a game carries a member equal to its empty default")
        sparse_rel = [r for g in raw["games"] for r in g["releases"] if "sizeBytes" not in r]
        self.assertTrue(sparse_rel, "no release leaves sizeBytes out")
        exp = tv_golden.expand(raw)
        for g in exp["games"]:
            self.assertTrue(set(raw["defaults"]["game"]) <= set(g))
            for r in g["releases"]:
                self.assertTrue(set(raw["defaults"]["release"]) <= set(r))
        self.assertEqual(tv_golden.build(exp, self.feat), tv_golden.build(raw, self.feat))
        bad = copy.deepcopy(raw)
        bad["defaults"]["release"]["provider"] = "Archive.org"
        with self.assertRaises(ValueError):
            tv_golden.build(bad, self.feat)

    def test_platforms_sources_and_catalogue_order(self):
        gids = self.game_ids()
        cat_order = [g["id"] for g in self.cat["games"] if g["id"] in gids]
        self.assertEqual(gids, cat_order)
        self.assertEqual({o["platform"] for o in self.out}, {"ps4", "ps5"})
        self.assertNotIn("fx-pixel-shell-homebrew", gids)                 # HOMEBREW
        self.assertNotIn("fx-orbital-tools-ps5", gids)                    # pkgzone only
        self.assertNotIn("fx-paper-moon-ps4", gids)                       # dlpsgame PAGE
        self.assertNotIn("fx-static-bloom-ps5", gids)                     # dlpsgame PKG + RAR
        self.assertEqual({o["sourceId"] for o in self.out}, {"archive", "vikingfile"})
        self.assertEqual({o["provider"] for o in self.out}, {"Archive.org", "Vikingfile"})

    def test_formats_and_sizes(self):
        self.assertTrue({o["format"] for o in self.out} <= set(tc.TV_FORMATS))
        self.assertEqual({o["format"] for o in self.out}, set(tc.TV_FORMATS))   # every tier-A format covered
        gids = self.game_ids()
        for hidden in ("fx-dust-runner-ps4", "fx-sunken-choir-ps4", "fx-null-signal-ps5", "fx-locked-vault-ps5"):
            self.assertNotIn(hidden, gids)
        self.assertEqual(ids(by_game(self.out, "fx-glass-tide-ps5")), ["fx-glass-tide-ffpfsc"])
        z = tv_golden.build(self.cat, self.feat, show_zip=True)
        tide = by_game(z, "fx-glass-tide-ps5")
        self.assertEqual(ids(tide), ["fx-glass-tide-zip", "fx-glass-tide-ffpfsc"])   # catalogue order, no 7z
        zo = tide[0]
        self.assertEqual((zo["format"], zo["compression"], zo["archiveContents"]), ("Unknown", "zip", "unverified"))
        self.assertEqual(zo["extractedBytes"], zo["sizeBytes"])
        self.assertIn("fx-dust-runner-ps4", {o["gameId"] for o in z})
        self.assertNotIn("fx-sunken-choir-ps4", {o["gameId"] for o in z})
        for o in z:
            self.assertEqual(o["format"] == "Unknown", "compression" in o)
            tc.check_reply("GET /api/v1/catalog", [o])

    def test_r2_option_order(self):
        want = {
            "fx-tidewater-ps5": ["fx-tidewater-ffpfsc", "fx-tidewater-backport", "fx-tidewater-update",
                                 "fx-tidewater-dlc"],
            "fx-neon-courier-ps5": ["fx-neon-courier-exfat", "fx-neon-courier-vf"],
            "fx-ember-hollow-ps5": ["fx-ember-hollow-unaudited", "fx-ember-hollow-ok", "fx-ember-hollow-flaky"],
            "fx-glass-orchard-ps5": ["fx-glass-orchard-verified", "fx-glass-orchard-unverified",
                                     "fx-glass-orchard-ffpkg"],
            "fx-starfall-rally-ps5": ["fx-starfall-rally-first", "fx-starfall-rally-second", "fx-starfall-rally-vf",
                                      "fx-starfall-rally-update"],
            "fx-viking-only-ps5": ["fx-viking-only-a", "fx-viking-only-b"],
        }
        for gid, order in want.items():
            self.assertEqual(ids(by_game(self.out, gid)), order, gid)

    def test_notes_prefixes(self):
        n = {o["id"]: o["notes"] for o in self.out}
        self.assertEqual(n["fx-tidewater-update"], "Update 01.020. Install the base game first.")
        self.assertEqual(n["fx-starfall-rally-update"], "Update. Install the base game first.")
        self.assertEqual(n["fx-tidewater-dlc"], "Add-on content. Needs the base game. Season pass.")
        self.assertEqual(n["fx-tidewater-backport"], "Backport for older firmware.")
        self.assertEqual(n["fx-tidewater-ffpfsc"], "Languages: English")
        self.assertEqual(n["fx-neon-courier-exfat"], "")

    def test_title_id_derivation_order(self):
        t = {o["id"]: o["titleId"] for o in self.out}
        self.assertEqual(t["fx-paper-lanterns-ffpfs"], "PPSA90006")    # release titleId, upper-cased
        self.assertEqual(t["fx-quiet-harbour-pkg"], "CUSA90007")       # game titleIds[0], upper-cased
        self.assertEqual(t["fx-frostline-pkg"], "CUSA12345")           # filename
        self.assertEqual(t["fx-signal-fire-exfat"], "PLJM12345")       # URL path
        self.assertEqual(t["fx-moss-lantern-part1"], "")               # none

    def test_dates(self):
        g = {o["gameId"]: o for o in self.out}
        self.assertEqual(g["fx-iron-meridian-ps5"]["releaseDate"], "")      # "TBA"
        self.assertEqual(g["fx-iron-meridian-ps5"]["addedAt"], "")          # missing (sparse)
        self.assertEqual(g["fx-copper-coast-ps4"]["releaseDate"], "")       # "2026-7-1"
        self.assertEqual(g["fx-copper-coast-ps4"]["addedAt"], "2026-10-01T00:00:00Z")
        self.assertEqual(g["fx-tidewater-ps5"]["releaseDate"], "2026-09-12")

    def test_ranks(self):
        r = {o["gameId"]: o.get("newOnOrbitRank") for o in self.out}
        self.assertEqual({k: v for k, v in r.items() if v}, {"fx-aurora-drift-ps5": 1, "fx-hollow-crown-ps5": 2,
                                                             "fx-crown-of-ash-ps5": 3, "fx-velvet-static-ps5": 4})
        self.assertIn("fx-paper-moon-ps4", self.feat["hero"])               # the hidden hero is skipped
        for o in self.out:
            self.assertEqual("newOnOrbitRank" in o, r[o["gameId"]] is not None)
        archive_only = tv_golden.build(self.cat, self.feat, enabled=["archive"])
        self.assertEqual({o["gameId"]: o.get("newOnOrbitRank") for o in archive_only if "newOnOrbitRank" in o},
                         {k: v for k, v in r.items() if v})                 # the filter never renumbers
        no_feed = tv_golden.ranks(tv_golden.expand(self.cat), None)
        self.assertEqual(no_feed, {"fx-hollow-crown-ps5": 1, "fx-crown-of-ash-ps5": 2, "fx-velvet-static-ps5": 3})
        feed = {"hero": ["fx-velvet-static-ps5", 7, "fx-velvet-static-ps5", "fx-paper-moon-ps4", "nope"]}
        self.assertEqual(tv_golden.ranks(tv_golden.expand(self.cat), feed),
                         {"fx-velvet-static-ps5": 1, "fx-hollow-crown-ps5": 2, "fx-crown-of-ash-ps5": 3})
        tie = copy.deepcopy(self.cat)
        for g in tie["games"]:
            if g["id"] in ("fx-crown-of-ash-ps5", "fx-velvet-static-ps5"):
                g["featured"] = 5
        self.assertEqual(list(tv_golden.ranks(tv_golden.expand(tie), None)),
                         ["fx-hollow-crown-ps5", "fx-crown-of-ash-ps5", "fx-velvet-static-ps5"])

    def test_popular_ranks(self):
        p = {o["gameId"]: o["popularRank"] for o in self.out}
        self.assertEqual(sorted(p.values()), list(range(1, len(p) + 1)))    # every TV game once, no gaps
        order = sorted(p, key=p.get)
        self.assertEqual(order[:7], ["fx-tidewater-ps5",                    # 0.42 (paper-moon's 0.99 is hidden)
                                     "fx-neon-courier-ps5", "fx-quiet-harbour-ps4",     # 0.3: PS5 first
                                     "fx-iron-meridian-ps5", "fx-frostline-ps4",       # 0.25: PS5 first, though later
                                     "fx-ember-hollow-ps5", "fx-aurora-drift-ps5"])    # 0.2: catalogue order
        self.assertNotIn("fx-paper-moon-ps4", p)
        zero = order[7:]                                                    # popularity 0 ("0.9" is a string)
        self.assertIn("fx-copper-coast-ps4", zero)
        plat = {o["gameId"]: o["platform"] for o in self.out}
        self.assertEqual(zero, [x for x in self.game_ids() if x in zero and plat[x] == "ps5"] +
                         [x for x in self.game_ids() if x in zero and plat[x] == "ps4"])   # PS5 first, catalogue order
        for o in self.out:
            self.assertEqual(o["popularRank"], p[o["gameId"]])
        vf = tv_golden.build(self.cat, self.feat, enabled=["vikingfile"])
        self.assertTrue(vf)
        self.assertEqual({o["gameId"]: o["popularRank"] for o in vf}, {o["gameId"]: p[o["gameId"]] for o in vf})
        # coercions as jv_dbl: a bool is 0/1, an integer counts, a huge integer is infinite
        cat = copy.deepcopy(self.cat)
        for g in cat["games"]:
            if g["id"] == "fx-cafe-lumiere-ps4":
                g["popularity"] = True
            elif g["id"] == "fx-bramble-path-ps4":
                g["popularity"] = 10 ** 400
        q = tv_golden.popular_ranks(tv_golden.expand(cat))
        self.assertEqual(sorted(q, key=q.get)[:2], ["fx-bramble-path-ps4", "fx-cafe-lumiere-ps4"])

    def test_cheats_screenshots_and_kind(self):
        g = {o["gameId"]: o for o in self.out}
        self.assertEqual({k: o["cheats"] for k, o in g.items() if "cheats" in o},
                         {"fx-tidewater-ps5": 12, "fx-copper-coast-ps4": 2})          # 2.7 -> 2; -3 -> none
        self.assertEqual({k: o["screenshotCount"] for k, o in g.items() if "screenshotCount" in o},
                         {"fx-tidewater-ps5": 4, "fx-vast-archive-ps5": 10})         # "", 7, an AVIF not counted; 11 -> 10
        self.assertNotIn("screenshotCount", g["fx-iron-meridian-ps5"])               # an object is not a list
        for o in self.out:
            self.assertNotIn("screenshots", o)
        kinds = {o["id"]: o["kind"] for o in self.out}
        self.assertEqual([kinds[i] for i in ids(by_game(self.out, "fx-tidewater-ps5"))],
                         ["game", "backport", "update", "dlc"])
        self.assertEqual(kinds["fx-starfall-rally-update"], "update")
        self.assertEqual(collections.Counter(kinds.values()), collections.Counter(
            {"game": len(kinds) - 4, "backport": 1, "update": 2, "dlc": 1}))
        odd = copy.deepcopy(self.cat)
        for gm in odd["games"]:
            for r in gm["releases"]:
                if r["id"] == "fx-tidewater-update":
                    r["kind"] = "patch"                                          # not a TV kind: reads as a game
        o = tv_golden.find_option(tv_golden.build(odd, self.feat), "fx-tidewater-update")
        self.assertEqual((o["kind"], o["notes"]), ("game", ""))

    def test_game_ids(self):
        gids = self.game_ids()
        long_id = [g for g in gids if len(g) == 78]
        self.assertEqual(len(long_id), 1)
        self.assertNotIn("fx space game-ps5", gids)
        self.assertIn("fx space game-ps5", {g["id"] for g in self.cat["games"]})

    def test_r6_browser_and_direct(self):
        o = {x["id"]: x for x in self.out}
        self.assertEqual((o["fx-neon-courier-vf"]["browserAvailable"], o["fx-neon-courier-vf"]["directAvailable"]),
                         (True, False))
        self.assertEqual((o["fx-viking-only-b"]["browserAvailable"], o["fx-viking-only-b"]["directAvailable"]),
                         (True, False))                                     # no browserUrl, still browser-only
        rels = {r["id"]: r for g in self.cat["games"] for r in g["releases"]}
        self.assertTrue(rels["fx-neon-courier-vf"].get("browserUrl"))
        self.assertFalse(rels["fx-viking-only-b"].get("browserUrl"))
        self.assertTrue(rels["fx-neon-courier-exfat"].get("browserUrl"))
        self.assertEqual((o["fx-neon-courier-exfat"]["browserAvailable"], o["fx-neon-courier-exfat"]["directAvailable"]),
                         (False, True))                                     # archive with a browserUrl stays direct

    def test_shape_and_contract(self):
        tc.check_reply("GET /api/v1/catalog", self.out)
        order = list(tv_golden.CATALOG_KEYS)
        for o in self.out:
            keys = list(o)
            self.assertEqual(keys, [k for k in order if k in o])
            for k in ("url", "browserUrl", "mirrors", "sha256", "password", "health", "cover", "hero", "screenshots"):
                self.assertNotIn(k, o)
        raw = json.dumps(self.out, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.assertEqual(tc.strict_json(raw), self.out)
        g = {x["gameId"]: x for x in self.out}
        self.assertIn("\t", g["fx-tidewater-ps5"]["description"])
        self.assertIn('"', g["fx-tidewater-ps5"]["description"])
        self.assertIn("\U0001F680", g["fx-aurora-drift-ps5"]["tagline"])
        self.assertTrue(g["fx-cafe-lumiere-ps4"]["title"].startswith("Café"))
        self.assertEqual(g["fx-cafe-lumiere-ps4"]["region"], "JPN")        # release over game
        self.assertEqual(g["fx-quiet-harbour-ps4"]["region"], "USA")       # game when the release has none

    def test_filter_and_acknowledgement(self):
        self.assertEqual(tv_golden.build(self.cat, self.feat, acknowledged=False), [])
        self.assertEqual(tv_golden.build(self.cat, self.feat, enabled=[]), [])
        self.assertEqual({o["sourceId"] for o in tv_golden.build(self.cat, self.feat, enabled=["archive"])},
                         {"archive"})
        self.assertEqual(tv_golden.build(self.cat, self.feat, enabled={"vikingfile": False}),
                         tv_golden.build(self.cat, self.feat, enabled=["archive"]))

    def test_sources(self):
        s = tv_golden.sources(self.cat, self.feat, enabled=None, acknowledged=False)
        self.assertEqual(list(s), list(tv_golden.SOURCES_KEYS))
        self.assertEqual([o["id"] for o in s["options"]], ["archive", "vikingfile"])
        self.assertEqual(s["enabled"], ["archive", "vikingfile"])           # sent before acknowledgement too
        counts = {o["id"]: o["releaseCount"] for o in s["options"]}
        self.assertEqual(counts, {"archive": sum(o["sourceId"] == "archive" for o in self.out),
                                  "vikingfile": sum(o["sourceId"] == "vikingfile" for o in self.out)})
        self.assertEqual([o["downloadReady"] for o in s["options"]], [True, False])
        s2 = tv_golden.sources(self.cat, self.feat, enabled={"archive": False}, acknowledged=True)
        self.assertEqual(s2["enabled"], ["vikingfile"])
        self.assertEqual(s2["options"], s["options"])                      # counts ignore the filter
        tc.check_reply("GET /api/v1/sources", s2)
        self.assertEqual(tv_golden.game_count(self.cat), len(self.game_ids()))

    def test_deterministic_and_pure(self):
        before = copy.deepcopy(self.cat)
        a = tv_golden.build(self.cat, self.feat)
        b = tv_golden.build(copy.deepcopy(self.cat), copy.deepcopy(self.feat))
        self.assertEqual(json.dumps(a), json.dumps(b))
        self.assertEqual(self.cat, before)

    def test_reference_golden_files(self):
        # The default fixture World (acknowledged, every exposed source on) must serve these.
        tvworld.assert_golden("catalog", self.out)
        tvworld.assert_golden("sources", tv_golden.sources(self.cat, self.feat, enabled=None, acknowledged=True))

    def test_c_reader_coercions(self):
        g = {"n": 3.9, "b": True, "s": " 42x", "t": "x", "z": None}
        self.assertEqual([tv_golden._i(g, k) for k in ("n", "b", "s", "t", "z", "missing")], [3, 1, 42, 0, 0, 0])
        self.assertEqual([tv_golden._d(g, k) for k in ("n", "b", "s", "t", "z", "missing")], [3.9, 1.0, 0, 0, 0, 0])
        self.assertEqual(tv_golden._s({"a": 5}, "a"), "")


# --------------------------------------------------------------------------- #
class GoldenRealTests(unittest.TestCase):
    """PORT-SPEC R1 and map-catalog-art §2 quote these numbers for revision 10."""

    @classmethod
    def setUpClass(cls):
        cls.cat = tv_golden.load_repo_catalog()
        cls.feat = tv_golden.load_repo_featured()

    def test_counts(self):
        st = tv_golden.stats(self.cat, self.feat)
        again = tv_golden.stats(self.cat, self.feat)
        self.assertEqual(st, again)
        if self.cat.get("revision") != 10:
            self.skipTest(f"catalogue revision {self.cat.get('revision')}: the quoted counts are for revision 10 "
                          f"(now {st['options']} options, {st['games']} games)")
        self.assertEqual((st["options"], st["games"], st["bytes"]), (4292, 2593, 2364377))
        self.assertEqual(st["optionsBySource"], {"archive": 3560, "vikingfile": 732})
        self.assertEqual(st["bytesBySource"], {"archive": 1892203, "vikingfile": 472175})
        self.assertEqual(st["optionsByPlatform"], {"ps4": 3296, "ps5": 996})
        self.assertEqual(st["formats"], {"PKG": 3296, "exFAT": 673, "FFPFSC": 322, "FFPKG": 1})
        self.assertEqual(st["kinds"], {"game": 4238, "dlc": 39, "update": 12, "backport": 3})
        self.assertEqual((st["ranked"], st["rank1"], st["heroNotVisible"]), (21, "ghost-of-yotei-ps5", []))
        self.assertEqual((st["titleIdEmpty"], st["browserAvailable"], st["multiOptionGames"]), (1424, 732, 509))
        self.assertEqual((st["maxGameIdLength"], st["maxReleaseIdLength"], st["maxSizeBytes"]),
                         (78, 37, 296579432448))
        self.assertEqual(st["datedGames"], 602)
        z = tv_golden.stats(self.cat, self.feat, show_zip=True)
        self.assertEqual((z["options"], z["games"], z["bytes"]), (4771, 3008, 2624561))
        nf = tv_golden.stats(self.cat, None)
        self.assertEqual(nf["rank1"], "dragon-ball-sparking-zero-ps5")

    def test_every_option_passes_the_contract(self):
        out = tv_golden.build(self.cat, self.feat)
        tc.check_reply("GET /api/v1/catalog", out)
        raw = json.dumps(out, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.assertEqual(len(tc.strict_json(raw)), len(out))
        self.assertLess(len(raw), 16 * MiB)                               # APPO/api.cpp:156 cap
        tc.check_reply("GET /api/v1/sources", tv_golden.sources(self.cat, self.feat, acknowledged=True))


# --------------------------------------------------------------------------- #
class ArtGenTests(unittest.TestCase):
    def test_art_set(self):
        want = {"cover.webp": ("webp", 512, 512), "hero.webp": ("webp", 1920, 1080), "cover.png": ("png", 512, 512),
                "cover.jpg": ("jpeg", 512, 512), "cover.avif": ("avif", 512, 512), "cover.gif": ("gif", 512, 512),
                "anim.webp": ("webp", 512, 512), "huge-9000.png": ("png", 9000, 100),
                "big-2000.png": ("png", 2000, 2000), "cmyk.jpg": ("jpeg", 512, 512),
                "hero-square.webp": ("webp", 1080, 1080), "max-1024.png": ("png", 1024, 1024),
                "hero-2560.png": ("png", 2560, 1440)}
        for name, (fmt, w, h) in want.items():
            d = artgen.describe(artgen.make(name, "s"))
            self.assertEqual((d["format"], d["width"], d["height"]), (fmt, w, h), name)
        self.assertTrue(artgen.describe(artgen.make("anim.webp"))["animated"])
        self.assertEqual(artgen.describe(artgen.make("anim.webp"))["webp_chunk"], "VP8X")
        alpha = artgen.describe(artgen.make("cover-alpha.webp"))
        self.assertEqual((alpha["webp_chunk"], alpha["animated"]), ("VP8X", False))
        self.assertEqual(artgen.describe(artgen.make("cover-lossless.webp"))["webp_chunk"], "VP8L")
        self.assertEqual(artgen.describe(artgen.make("cover.webp"))["webp_chunk"], "VP8 ")
        self.assertEqual(artgen.describe(artgen.make("cmyk.jpg"))["sof"][2], 4)
        self.assertEqual(artgen.describe(artgen.make("hero.jpg"))["sof"][0], 0xC2)
        self.assertEqual(artgen.describe(artgen.make("cover.jpg"))["sof"], (0xC0, 8, 3))
        self.assertEqual(artgen.describe(artgen.make("cover-gray.jpg"))["sof"][2], 1)
        self.assertEqual(len(artgen.make("too-big.webp")), artgen.MAX_FETCH_BYTES + 1)
        self.assertNotEqual(artgen.make("cover.webp", "a"), artgen.make("cover.webp", "b"))
        for name in artgen.ART_SET:
            tc_kind = tc.sniff_image(artgen.make(name))
            if artgen.ART_SET[name][1] == "served":
                self.assertIsNotNone(tc_kind, name)
        with tempfile.TemporaryDirectory() as d:
            paths = artgen.write_art_set(d)
            self.assertEqual(set(paths), set(artgen.ART_SET))

    def test_fixture_art_names_exist(self):
        raw = tv_golden.load_json(tvworld.FIXTURE_CATALOG)
        for g in raw["games"]:
            for k in ("cover", "hero", "coverFallback", "heroFallback"):
                u = g.get(k, "")
                if u.startswith("{{ART}}/"):
                    self.assertIn(u.rsplit("/", 1)[-1], artgen.ART_SET, f"{g['id']}.{k}")
            shots = g.get("screenshots", [])
            for u in shots if isinstance(shots, list) else shots.values():
                if isinstance(u, str) and u:
                    self.assertTrue(u.startswith("{{ART}}/"), f"{g['id']}.screenshots: {u}")
                    self.assertIn(u.rsplit("/", 1)[-1], artgen.ART_SET, f"{g['id']}.screenshots")

    def test_preview_art_server_knows_the_fixture_art(self):
        """tools/tv/art_server.py, the live preview's art host for the same fixture, has every {{ART}}
        name: a good picture (accepted for its kind) wherever the backend tests serve artgen's."""
        sys.path.insert(0, os.path.join(tvworld.PROJECT, "tools", "tv"))
        import art_server  # noqa: E402
        raw = tv_golden.load_json(tvworld.FIXTURE_CATALOG)
        names = set()
        for g in raw["games"]:
            shots = g.get("screenshots", [])
            urls = [g.get(k, "") for k in ("cover", "hero", "coverFallback", "heroFallback")]
            for u in urls + (shots if isinstance(shots, list) else list(shots.values())):
                if isinstance(u, str) and u.startswith("{{ART}}/"):
                    names.add(u.rsplit("/", 1)[-1])
        for name in sorted(names):
            with self.subTest(name=name):
                self.assertIn(name, set(art_server.GOOD) | set(art_server.BAD))
                if artgen.ART_SET[name][1] == "served":
                    self.assertIn(name, art_server.GOOD)
                    self.assertEqual(art_server.check(art_server.make(name, "T", seed="t"), art_server.GOOD[name][0]), [])

    def test_server(self):
        with artgen.ArtServer() as srv:
            c = tvworld.TvClient(srv.port)
            p = "/art/fx-a/cover.webp"
            r = c.get(p)
            self.assertEqual((r.status, r.headers["content-type"]), (200, "image/webp"))
            self.assertEqual(r.body, srv.bytes_for(p)[0])
            fb = c.get("/art/fx-a/fallback/cover.webp")
            self.assertNotEqual(fb.body, r.body)                     # primary and fallback are distinguishable
            self.assertEqual(c.get("/art/fx-a/cover.jpg").headers["content-type"], "image/jpeg")
            self.assertEqual(c.get("/art/fx-a/nope.webp").status, 404)
            self.assertEqual(c.get("/elsewhere").status, 404)
            srv.set_status(p, 500, times=2)
            self.assertEqual([c.get(p).status for _ in range(3)], [500, 500, 200])
            srv.set_status(srv.base + p, 503)
            self.assertEqual(c.get(p).status, 503)
            srv.clear(p)
            srv.set_bytes(p, b"xyz", "image/webp")
            self.assertEqual(c.get(p).body, b"xyz")
            srv.set_delay("/art/fx-a/hero.webp", 0.3)
            self.assertGreaterEqual(c.get("/art/fx-a/hero.webp").elapsed, 0.3)
            self.assertEqual(srv.hits(p), 6)
            srv.set_redirect("/art/fx-r/cover.webp", srv.url("fx-a", "cover.png"), 301)
            r = c.get("/art/fx-r/cover.webp")
            self.assertEqual((r.status, r.headers["location"]), (301, srv.base + "/art/fx-a/cover.png"))
            srv.clear("/art/fx-r/cover.webp")
            self.assertEqual(c.get("/art/fx-r/cover.webp").status, 200)
            self.assertEqual(srv.requests[0][0], p)
            self.assertEqual(srv.requests[0][1].get("User-Agent"), tvworld.USER_AGENT)
            srv.reset()
            self.assertEqual(srv.hits(), {})


# --------------------------------------------------------------------------- #
class _Capture:
    """A one-shot TCP listener that records the raw request bytes."""

    def __init__(self):
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.data = b""
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _run(self):
        conn, _ = self.sock.accept()
        conn.settimeout(5)
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += conn.recv(4096)
        head, _, body = buf.partition(b"\r\n\r\n")
        for ln in head.split(b"\r\n"):
            if ln.lower().startswith(b"content-length:"):
                while len(body) < int(ln.split(b":")[1]):
                    body += conn.recv(4096)
        self.data = head + b"\r\n\r\n" + body
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\n"
                     b"Connection: close\r\n\r\n[]")
        conn.close()
        self.sock.close()


class ClientWireTests(unittest.TestCase):
    def test_request_bytes_like_the_app(self):
        cap = _Capture()
        c = tvworld.TvClient(cap.port, token="ab" * 32)
        r = c.post("/api/v1/favorites", {"gameId": "fx-a", "favorite": True})
        cap.t.join(5)
        self.assertEqual(r.check(), [])
        body = '{"gameId":"fx-a","favorite":true}'
        want = ("POST /api/v1/favorites HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\nAccept: */*\r\n"
                "User-Agent: XenoStoreTV\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\n"
                "Content-Length: %d\r\n\r\n%s" % (cap.port, "ab" * 32, len(body), body))
        self.assertEqual(cap.data.decode(), want)
        cap = _Capture()
        tvworld.TvClient(cap.port).get("/api/v1/system")
        cap.t.join(5)
        self.assertEqual(cap.data.decode(), "GET /api/v1/system HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
                         "Connection: close\r\nAccept: */*\r\nUser-Agent: XenoStoreTV\r\n\r\n" % cap.port)
        self.assertNotIn(b"Origin", cap.data)


# --------------------------------------------------------------------------- #
class NormalizerTests(unittest.TestCase):
    def test_placeholders(self):
        n = tvworld.Normalizer(roots=["/tmp/w/data/downloads", "/tmp/w/R1"], data="/tmp/w/data", tmp="/tmp/w",
                               version="1.2.1")
        v = n({"instanceId": "a" * 32, "token": "b" * 64, "pairCode": "123456", "address": "192.168.1.9",
               "checkedAt": 1792000000, "checkAfter": 0, "version": "1.2.1", "httpPort": 9095,
               "jobs": [{"id": "j_0a1b2c", "path": "/tmp/w/R1/homebrew/x.exfat"}, {"id": "j_ffffff"},
                        {"id": "j_0a1b2c", "version": "1.2.1"}],
               "drives": [{"id": "c" * 64, "path": "/tmp/w/data/downloads/homebrew", "freeBytes": 5,
                           "totalBytes": 9, "usedBytes": 4}],
               "requestId": "j_123456", "log": "/tmp/w/data/x and /tmp/w/other"})
        self.assertEqual(v["instanceId"], "<hex32>")
        self.assertEqual((v["token"], v["pairCode"], v["address"]), ("<hex64>", "<code6>", "<ipv4>"))
        self.assertEqual((v["checkedAt"], v["checkAfter"], v["version"], v["httpPort"]),
                         ("<unix>", 0, "<version>", "<port>"))
        self.assertEqual([j["id"] for j in v["jobs"]], ["<job:1>", "<job:2>", "<job:1>"])
        self.assertEqual(v["jobs"][2]["version"], "1.2.1")                  # only top-level versions
        self.assertEqual(v["jobs"][0]["path"], "<root:1>/homebrew/x.exfat")
        self.assertEqual(v["drives"][0], {"id": "<hex64>", "path": "<root:0>/homebrew", "freeBytes": "<bytes>",
                                          "totalBytes": "<bytes>", "usedBytes": "<bytes>"})
        self.assertEqual(v["requestId"], "j_123456")                        # echoed exactly
        self.assertEqual(v["log"], "<data>/x and <tmp>/other")
        self.assertEqual(n({"id": "j_ffffff"}), {"id": "<job:2>"})          # stable across calls

    def test_projected_free_bytes_formula(self):
        n = tvworld.Normalizer()
        free = 1000 * MiB
        ok = {"freeBytes": free, "pendingBytes": 10, "projectedFreeBytes": free - 10 - 240 * MiB}
        self.assertEqual(n([ok])[0]["projectedFreeBytes"], "<bytes>")
        with self.assertRaises(AssertionError):
            n([dict(ok, projectedFreeBytes=1)])

    def test_extra_rules(self):
        n = tvworld.Normalizer(keys={"speed": "<speed>", "received": lambda v: v // 1000},
                               paths={"games[].sizeBytes": "<size>"})
        self.assertEqual(n({"speed": 5, "received": 12345, "games": [{"sizeBytes": 1}], "sizeBytes": 2}),
                         {"speed": "<speed>", "received": 12, "games": [{"sizeBytes": "<size>"}], "sizeBytes": 2})

    def test_golden_files(self):
        old_dir, old_env = tvworld.GOLDEN_DIR, os.environ.pop("XENO_TV_GOLDEN_UPDATE", None)
        with tempfile.TemporaryDirectory() as d:
            tvworld.GOLDEN_DIR = d
            try:
                with self.assertRaises(AssertionError) as cm:
                    tvworld.assert_golden("sample", {"a": 1})
                self.assertIn("XENO_TV_GOLDEN_UPDATE=1", str(cm.exception))
                os.environ["XENO_TV_GOLDEN_UPDATE"] = "1"
                tvworld.assert_golden("sample", {"token": "f" * 64, "b": [1, "é"], "a": 1})
                del os.environ["XENO_TV_GOLDEN_UPDATE"]
                with open(os.path.join(d, "sample.json"), encoding="utf-8") as f:
                    self.assertEqual(f.read(), '{\n "token": "<hex64>",\n "b": [\n  1,\n  "é"\n ],\n "a": 1\n}\n')
                tvworld.assert_golden("sample", {"token": "0" * 64, "b": [1, "é"], "a": 1})
                with self.assertRaises(AssertionError) as cm:                # key order matters
                    tvworld.assert_golden("sample", {"a": 1, "token": "0" * 64, "b": [1, "é"]})
                self.assertIn("differs from golden/sample.json", str(cm.exception))
                with self.assertRaises(ValueError):
                    tvworld.golden_path("../x")
            finally:
                tvworld.GOLDEN_DIR = old_dir
                os.environ.pop("XENO_TV_GOLDEN_UPDATE", None)
                if old_env is not None:
                    os.environ["XENO_TV_GOLDEN_UPDATE"] = old_env


# --------------------------------------------------------------------------- #
@unittest.skipUnless(HAVE_BINARY, f"host binary {harness.BINARY} is missing (make host)")
class WorldTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.w = tvworld.World("infra", library=True, acknowledged=False, enabled=["archive"]).start()
        cls.c = cls.w.client()

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    def test_fixture_catalogue_loads_in_the_backend(self):
        st, info = self.w.srv.api("GET", "/api/catalog/info")
        self.assertEqual(st, 200)
        self.assertEqual((info["revision"], info["source"]), (2000000000, "cached"))
        self.assertEqual(info["games"], len(self.w.catalog["games"]))
        self.assertEqual(info["releases"], sum(len(g["releases"]) for g in self.w.catalog["games"]))
        st, _, raw = self.w.srv.raw("GET", "/catalog.json")
        self.assertEqual(st, 200)
        doc = json.loads(raw)
        self.assertEqual([g["id"] for g in doc["games"]], [g["id"] for g in self.w.catalog["games"]])
        self.assertNotIn("{{", raw.decode("utf-8"))
        st, hit = self.w.srv.api("GET", "/api/catalog/search?q=Tidewater&limit=5")
        self.assertEqual(st, 200, hit)
        self.assertIn("fx-tidewater-ps5", json.dumps(hit))

    def test_seeded_state(self):
        st, s = self.w.srv.api("GET", "/api/settings")
        self.assertEqual(st, 200)
        src = s.get("settings", s)["sources"]
        self.assertIs(src["acknowledged"], False)
        self.assertEqual((src["enabled"]["archive"], src["enabled"]["vikingfile"]), (True, False))
        with open(os.path.join(self.w.data, "feeds", "featured.json"), encoding="utf-8") as f:
            self.assertEqual(json.load(f), self.w.featured)
        self.assertEqual(self.w.srv.env["XENO_HOST_ART_FIXTURE"], self.w.art.hostport)
        self.assertEqual(self.w.golden_catalog(), [])                       # not acknowledged
        self.assertEqual({o["sourceId"] for o in self.w.golden_catalog(acknowledged=True)}, {"archive"})
        self.assertEqual(self.w.golden_sources()["enabled"], ["archive"])
        zip_rel = self.w.release("fx-glass-tide-zip")
        self.assertEqual(zip_rel["sizeBytes"], os.path.getsize(os.path.join(self.w.files.files_dir, "single.zip")))
        self.assertTrue(zip_rel["url"].startswith(self.w.files.base + "/ok/"))
        self.assertTrue(self.w.game("fx-tidewater-ps5")["cover"].startswith(self.w.art.base + "/art/"))
        for where, rel, size, _ in tvworld.LIBRARY_FILES:
            if where == "user":
                self.assertTrue(os.path.isdir(os.path.join(self.w.data, "user", "app", rel)))
            else:
                self.assertEqual(os.path.getsize(os.path.join(self.w.roots[where], rel)), size)
        self.assertEqual(self.w.all_roots[0], os.path.join(self.w.data, "downloads"))

    def test_pairing_and_system(self):
        self.assertRegex(self.c.token, r"^[0-9a-f]{64}$")
        st, web = self.w.srv.api("GET", "/api/session")
        self.assertEqual(web["token"], self.c.token)
        r = self.c.get("/api/v1/system")
        v = r.check()
        self.assertIs(v["paired"], True)
        self.assertEqual(r.transport_errors(), [])
        self.assertIs(self.c.get("/api/v1/system", token="").json["paired"], False)
        n = self.w.normalizer()(v)
        self.assertEqual((n["instanceId"], n["version"]), ("<hex32>", "<version>"))

    def test_gate_envelopes_and_stopping(self):
        r = self.c.get("/api/v1/downloads", token="")
        r.error(401, tc.S["unpaired"])
        self.w.set_stopping(True)
        try:
            self.c.get("/api/v1/downloads").error(503, tc.S["stopping"])
            self.assertIs(self.c.get("/api/v1/system").json["stopping"], True)
        finally:
            self.w.set_stopping(False)
        self.assertIs(self.c.get("/api/v1/system").json["stopping"], False)
        self.c.post("/api/v1/favorites", "[]").error(400, tc.S["not_object"])

    def test_poll(self):
        r = self.c.poll("/api/v1/system", until=lambda x: x.status == 200, timeout=2)
        self.assertEqual(r.status, 200)
        with self.assertRaises(AssertionError):
            self.c.poll("/api/v1/system", until=lambda x: False, timeout=0.3)


@unittest.skipUnless(HAVE_BINARY, f"host binary {harness.BINARY} is missing (make host)")
class RealWorldTests(unittest.TestCase):
    def test_embedded_revision_10_and_feed(self):
        with tvworld.World("infra-real", catalog="real", featured="real", art=False, files=None) as w:
            st, info = w.srv.api("GET", "/api/catalog/info")
            self.assertEqual((info["source"], info["revision"]), ("embedded", w.catalog["revision"]))
            self.assertEqual(info["games"], len(w.catalog["games"]))
            self.assertFalse(os.path.exists(os.path.join(w.data, "catalog.json")))
            with open(os.path.join(w.data, "feeds", "featured.json"), encoding="utf-8") as f:
                self.assertEqual(json.load(f)["hero"], tv_golden.load_repo_featured()["hero"])
            self.assertNotIn("XENO_HOST_ART_FIXTURE", w.env())


if __name__ == "__main__":
    unittest.main()
