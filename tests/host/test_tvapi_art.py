#!/usr/bin/env python3
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
test_tvapi_art.py — GET /api/v1/art/{gameId}/{cover|hero}, the TV app's artwork
(PORT-SPEC 1.7, 1.9, 1.10 "TvArt"; R8, R9; map-catalog-art §8).

  TvArt              the fixture catalogue against a local art server
                     (XENO_HOST_ART_FIXTURE): 202 then 200 for WebP (VP8, VP8L,
                     VP8X), PNG and JPEG with the right Content-Type and the
                     original bytes; the candidate rule (_artOk) and its
                     fallbacks; AVIF/GIF never fetched; animated WebP, a 9000-px
                     side, a 2000x2000 PNG cover, a PNG hero over 2560x1440, CMYK,
                     12-bit and lossless JPEG, text, an empty and a cut-off file,
                     a 12 MiB+ body and square heroes refused (fallback or 404,
                     remembered); shared and collection covers, hero == cover,
                     unknown games, bad ids and kinds answered 404 at once without
                     a fetch; art for a game the TV catalogue hides (R9); 401
                     without a paired token; 503 while stopping; 502 after a 500
                     and no refetch inside the window; redirects followed, capped
                     at three and their end re-checked against the URL policy; the
                     Accept header; the first reply within budget (PORT-SPEC 1.9).
  TvArtLimits        a catalogue with 376 extra games: the 24 MiB budget evicts
                     the least recently served picture, the 256-entry limit
                     recycles the oldest, the queue holds 64 keys and answers 503
                     when full, a key the app stopped asking for makes way after
                     10 s, nothing blocks while both workers wait on slow fetches,
                     and the cache never holds more than 24 MiB.
  TvArtRevision      a newer catalogue (POST /api/catalog/refresh) clears the cache
                     and the refusal memory: a URL refused before is fetched again
                     and a game whose cover URL changed gets the new picture.
  TvArtRealCatalogue the candidate rule on the real revision-10 catalogue against a
                     Python mirror of it: the same games, covers and heroes, and for
                     a sample the exact URLs tried in order (image.api.playstation.com
                     sized w=512 / w=1920). https goes to a dead proxy, so nothing
                     leaves the machine.

Pictures come from tests/host/tv/artgen.py (PIL). Every refusal body must pass
tvcontract.assert_error_envelope.
"""
import json
import os
import re
import statistics
import sys
import time
import unittest
import urllib.parse
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tvcontract as tc  # noqa: E402
import tvworld  # noqa: E402  (puts tests/host/tv on sys.path)
import artgen  # noqa: E402
import harness  # noqa: E402

MiB = 1024 * 1024
ACCEPT = "image/webp,image/png,image/jpeg;q=0.9"
FIRST_REPLY_BUDGET = 0.020        # PORT-SPEC 1.9: the first reply (202) within 20 ms
NEVER_BLOCKS = 0.250              # any single reply: far below a network wait
CACHE_BUDGET = 24 * MiB
CACHE_ENTRIES = 256
QUEUE = 64
STALE_S = 10.0                    # a queued key nobody asked for in 10 s makes way
CACHE_LINE = re.compile(r"tvart: cached (\S+) .*; cache (\d+) image\(s\), (\d+) bytes;")
INDEX_LINE = r"tvart: art index for catalogue revision {rev}: (\d+) games with art \((\d+) covers, (\d+) heroes\)"


def art(gid, kind="cover"):
    return f"/api/v1/art/{gid}/{kind}"


def src(gid, name, fallback=False):
    return f"/art/{gid}/{'fallback/' if fallback else ''}{name}"


def padded_webp(size, seed):
    """A still WebP of exactly `size` bytes: a real VP8 picture plus a JUNK chunk."""
    base = artgen.make("cover.webp", seed=seed)
    chunks = base[12:]
    pad = size - len(base) - 8
    assert pad >= 0 and pad % 2 == 0, pad
    chunks += b"JUNK" + pad.to_bytes(4, "little") + b"\0" * pad
    return b"RIFF" + (4 + len(chunks)).to_bytes(4, "little") + b"WEBP" + chunks


def patch_sof(data, marker=None, precision=None):
    """A JPEG with its frame header changed: another SOFn coding or sample precision.
    Only the header matters to the backend (it never decodes)."""
    i = 2
    while i + 4 <= len(data):
        m = data[i + 1]
        if m in (0xD8, 0x01) or 0xD0 <= m <= 0xD7:
            i += 2
            continue
        if 0xC0 <= m <= 0xCF and m not in (0xC4, 0xC8, 0xCC):
            b = bytearray(data)
            if marker is not None:
                b[i + 1] = marker
            if precision is not None:
                b[i + 4] = precision
            return bytes(b)
        i += 2 + int.from_bytes(data[i + 2:i + 4], "big")
    raise ValueError("no frame header")


class ArtMixin:
    def wait_art(self, gid, kind="cover", timeout=25.0):
        """Asks again (like the app, but faster) until the answer is not 202."""
        return self.c.poll(art(gid, kind), until=lambda r: r.status != 202, timeout=timeout, every=0.05)

    def assert_image(self, r, source_path):
        """A 200 with the original bytes of `source_path` on the art server."""
        self.assertEqual(r.status, 200, r)
        kind = tc.check_art_image(r.status, r.headers, r.body)
        self.assertEqual(r.transport_errors(json_body=False), [], r)
        data, _ = self.w.art.bytes_for(source_path)
        self.assertEqual(r.body, data, f"{r.path} did not serve the bytes of {source_path} unchanged")
        return kind

    def assert_served(self, gid, kind, source_path):
        return self.assert_image(self.wait_art(gid, kind), source_path)

    def assert_unknown(self, r):
        r.error(404, tc.S["art_unknown"])

    def assert_pending(self, r):
        self.assertEqual(r.status, 202, r)
        self.assertEqual(r.check(), {"pending": True})

    def cache_lines(self):
        return [(m.group(1), int(m.group(2)), int(m.group(3))) for m in CACHE_LINE.finditer(self.w.log_text())]

    def assert_memory_budget(self):
        lines = self.cache_lines()
        self.assertTrue(lines, "no 'tvart: cached' line in the log")
        for key, images, held in lines:
            self.assertLessEqual(held, CACHE_BUDGET, f"cache held {held} bytes after {key}")
            self.assertLessEqual(images, CACHE_ENTRIES, f"cache held {images} pictures after {key}")


def start_world(cls, name, **kw):
    cls.w = tvworld.World(name, **kw).start()
    cls.c = cls.w.client()                # pairing is the first /api/v1 request: the art workers start
    rev = cls.w.catalog["revision"]
    cls.index = cls.w.wait_log(INDEX_LINE.format(rev=rev), timeout=20)


# --------------------------------------------------------------------------- #
class TvArt(unittest.TestCase, ArtMixin):
    @classmethod
    def setUpClass(cls):
        start_world(cls, "art")

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    # ---- the gate ------------------------------------------------------------
    def test_01_bearer_required(self):
        for tok in ("", "0" * 64, "f" * 63):
            r = self.c.get(art("fx-tidewater-ps5"), token=tok)
            r.error(401, tc.S["unpaired"])
        self.assertEqual(self.w.art.hits(src("fx-tidewater-ps5", "cover.webp")), 0, "an unpaired request fetched art")

    def test_02_stopping(self):
        self.w.set_stopping(True)
        try:
            self.c.get(art("fx-tidewater-ps5")).error(503, tc.S["stopping"])
        finally:
            self.w.set_stopping(False)

    # ---- 202 then 200 --------------------------------------------------------
    def test_03_formats_served_unchanged(self):
        cases = [
            ("fx-tidewater-ps5", "cover", src("fx-tidewater-ps5", "cover.webp"), "webp"),       # VP8
            ("fx-tidewater-ps5", "hero", src("fx-tidewater-ps5", "hero.webp"), "webp"),
            ("fx-neon-courier-ps5", "cover", src("fx-neon-courier-ps5", "cover.png"), "png"),
            ("fx-neon-courier-ps5", "hero", src("fx-neon-courier-ps5", "hero.png"), "png"),
            ("fx-ember-hollow-ps5", "cover", src("fx-ember-hollow-ps5", "cover.jpg"), "jpeg"),   # SOF0
            ("fx-ember-hollow-ps5", "hero", src("fx-ember-hollow-ps5", "hero.jpg"), "jpeg"),     # SOF2
        ]
        firsts = []
        for gid, kind, path, fmt in cases:
            r = self.c.get(art(gid, kind))
            self.assert_pending(r)
            firsts.append(r.elapsed)
        for gid, kind, path, fmt in cases:
            with self.subTest(gid=gid, kind=kind):
                self.assertEqual(self.assert_served(gid, kind, path), fmt)
                self.assertEqual(self.w.art.hits(path), 1)
        # every fetch asks for the three formats the app decodes, never image/* (AVIF). The PS5
        # transport overwrites its default "Accept: */*" with it; the host one sends it last.
        paths = {c[2] for c in cases}
        for p, hdrs in self.w.art.requests:
            if p in paths:
                self.assertEqual(hdrs.get("Accept"), ACCEPT, p)
        self.assertLess(statistics.median(firsts), FIRST_REPLY_BUDGET, firsts)
        self.assertLess(max(firsts), NEVER_BLOCKS, firsts)

    def test_04_cached_reply_needs_no_fetch(self):
        path = src("fx-iron-meridian-ps5", "cover.webp")
        self.assert_served("fx-iron-meridian-ps5", "cover", path)
        for _ in range(5):
            r = self.c.get(art("fx-iron-meridian-ps5"))
            self.assert_image(r, path)
            self.assertLess(r.elapsed, NEVER_BLOCKS)
        self.assertEqual(self.w.art.hits(path), 1, "a cached picture was fetched again")

    def test_05_more_kinds_and_exact_limits(self):
        """VP8L, VP8X with alpha, greyscale JPEG, the exact pixel caps and the 1.4 ratio: all served."""
        a = self.w.art
        for gid, name in (("fx-orbital-tools-ps5", "cover-lossless.webp"), ("fx-dust-runner-ps4", "cover-alpha.webp"),
                          ("fx-glass-tide-ps5", "cover-gray.jpg"), ("fx-sunken-choir-ps4", "max-1024.png")):
            a.set_bytes(src(gid, "cover.webp"), artgen.make(name, seed=gid), artgen.content_type(name))
        a.set_bytes(src("fx-crown-of-ash-ps5", "hero.webp"), artgen.make("hero-2560.png", seed="cap"), "image/png")
        a.set_bytes(src("fx-velvet-static-ps5", "hero.webp"), artgen.make("hero-ratio-140.webp", seed="r"), "image/webp")
        for gid in ("fx-orbital-tools-ps5", "fx-dust-runner-ps4", "fx-glass-tide-ps5", "fx-sunken-choir-ps4"):
            with self.subTest(gid=gid):
                self.assert_served(gid, "cover", src(gid, "cover.webp"))
        for gid in ("fx-crown-of-ash-ps5", "fx-velvet-static-ps5"):
            with self.subTest(gid=gid):
                self.assert_served(gid, "hero", src(gid, "hero.webp"))

    def test_06_any_catalogue_game(self):
        """R9: art for games the TV catalogue hides (homebrew), and the 78+ character id."""
        for gid in ("fx-pixel-shell-homebrew",
                    "fx-the-extraordinarily-long-collectors-edition-title-of-a-test-game-xyzzzz-ps5"):
            with self.subTest(gid=gid):
                self.assert_served(gid, "cover", src(gid, "cover.webp"))

    # ---- candidates and fallbacks ----------------------------------------------
    def test_07_fallbacks(self):
        a = self.w.art
        cases = [
            # GIF primary: skipped by its URL, never fetched; the WebP fallback
            ("fx-glass-orchard-ps5", "cover", src("fx-glass-orchard-ps5", "cover.webp", True),
             [src("fx-glass-orchard-ps5", "cover.gif")]),
            # square hero refused, the wide heroFallback served
            ("fx-tall-tales-ps5", "hero", src("fx-tall-tales-ps5", "hero.webp", True), []),
            # a side over 8192 refused, the JPEG fallback served
            ("fx-giant-steps-ps5", "cover", src("fx-giant-steps-ps5", "cover.jpg", True), []),
            # CMYK JPEG refused, the PNG fallback served
            ("fx-ink-well-ps5", "cover", src("fx-ink-well-ps5", "cover.png", True), []),
            # shared archive collection thumbnail: straight to the fallback
            ("fx-collection-c-ps4", "cover", src("fx-collection-c-ps4", "cover.webp", True), []),
        ]
        for gid, kind, path, never in cases:
            with self.subTest(gid=gid, kind=kind):
                self.assert_served(gid, kind, path)
                for p in never:
                    self.assertEqual(a.hits(p), 0, f"{p} must never be fetched")
        self.assertEqual(a.hits(src("fx-tall-tales-ps5", "hero-square.png")), 1)
        self.assertEqual(a.hits(src("fx-giant-steps-ps5", "huge-9000.png")), 1)
        self.assertEqual(a.hits(src("fx-ink-well-ps5", "cmyk.jpg")), 1)
        # tidewater's AVIF fallback is never requested
        self.assert_served("fx-tidewater-ps5", "cover", src("fx-tidewater-ps5", "cover.webp"))
        self.assertEqual(a.hits(src("fx-tidewater-ps5", "cover.avif", True)), 0)
        log = self.w.log_text()
        self.assertRegex(log, r"tvart: refused for fx-giant-steps-ps5#cover: image/png 9000x100: a side is over 8192 px")
        self.assertRegex(log, r"tvart: refused for fx-ink-well-ps5#cover: JPEG with 4 components")
        self.assertRegex(log, r"tvart: refused for fx-tall-tales-ps5#hero: hero 1080x1080 is not wide")

    def test_08_refused_then_404_and_remembered(self):
        a = self.w.art
        cases = [("fx-starfall-rally-ps5", "cover", src("fx-starfall-rally-ps5", "anim.webp"), "animated WebP"),
                 ("fx-wide-open-ps5", "cover", src("fx-wide-open-ps5", "big-2000.png"),
                  "image/png 2000x2000 is over the 1048576-pixel cover cap"),
                 ("fx-glass-orchard-ps5", "hero", src("fx-glass-orchard-ps5", "hero-square.webp"), "is not wide")]
        for gid, kind, path, why in cases:
            with self.subTest(gid=gid, kind=kind):
                self.assert_unknown(self.wait_art(gid, kind))
                self.assertEqual(a.hits(path), 1)
                self.assertIn(f"tvart: refused for {gid}#{kind}: ", self.w.log_text())
                self.assertIn(why, self.w.log_text())
                # remembered: 404 at once, no new fetch
                for _ in range(3):
                    r = self.c.get(art(gid, kind))
                    self.assert_unknown(r)
                    self.assertLess(r.elapsed, NEVER_BLOCKS)
                self.assertEqual(a.hits(path), 1, "a refused URL was fetched again in the same catalogue")

    def test_09_bad_bytes_refused(self):
        """Each picture below is refused; without another candidate the answer is 404."""
        a = self.w.art
        jpg = artgen.make("cover.jpg", seed="sof")
        cases = [  # (game, kind, bytes, what the log says)
            ("fx-copper-coast-ps4", "cover", artgen.make("not-image.webp", seed="t"), "not a WebP, PNG or JPEG image"),
            ("fx-null-signal-ps5", "cover", artgen.make("empty.png"), "not a WebP, PNG or JPEG image"),
            ("fx-lantern-rite-ps4", "cover", artgen.make("truncated.png", seed="t"), "PNG cut short"),
            ("fx-bramble-path-ps4", "cover", artgen.make("too-big.webp", seed="t"),
             "12582913 bytes is over the 12 MiB limit"),
            ("fx-second-wind-ps5", "cover", patch_sof(jpg, marker=0xC3),
             "JPEG coding SOF3 (arithmetic, lossless or hierarchical)"),
            ("fx-vast-archive-ps5", "cover", patch_sof(jpg, precision=12), "12-bit JPEG"),
            ("fx-harbor-lights-ps5", "hero", artgen.make("hero-ratio-139.webp", seed="t"), "hero 1390x1000 is not wide"),
            ("fx-ridge-runner-ps4", "hero", artgen.make("hero-2561.png", seed="t"),
             "image/png 2561x1441 is over the 3686400-pixel hero cap"),
        ]
        for gid, kind, data, _ in cases:
            a.set_bytes(src(gid, "cover.webp" if kind == "cover" else "hero.webp"), data)
        for gid, kind, _, why in cases:
            with self.subTest(gid=gid, kind=kind):
                self.assert_unknown(self.wait_art(gid, kind))
                self.assertIn(f"tvart: refused for {gid}#{kind}: {why}", self.w.log_text())

    def test_10_immediate_404_without_fetch(self):
        a = self.w.art
        before = sum(a.hits().values())
        for path in (art("fx-signal-fire-ps5"), art("fx-moss-lantern-ps4"),     # one cover shared by two games
                     art("fx-collection-a-ps4"),                                # collection thumbnail, no fallback
                     art("fx-collection-b-ps4"),                                # ... with an AVIF fallback
                     art("fx-mirror-lake-ps5", "hero"),                         # hero == cover
                     art("fx-quiet-harbour-ps4", "hero"),                       # no hero at all
                     art("no-such-game-ps5"), art("no-such-game-ps5", "hero"),  # not in the catalogue
                     art("fx-tidewater-ps5", "banner"), art("fx-tidewater-ps5", "Cover"),   # bad kind
                     art("fx%20space%20game-ps5"),                              # an id the app cannot send
                     art("fx-tidewater-ps5.")):                                 # a valid id that is not a game
            with self.subTest(path=path):
                r = self.c.get(path)
                self.assert_unknown(r)
                self.assertLess(r.elapsed, NEVER_BLOCKS)
        self.assertEqual(sum(a.hits().values()), before, "a 404 at once fetched something")
        # an id longer than a route capture never reaches the handler: the dispatcher's sentence
        self.c.get(art("x" * 96)).error(404, tc.S["not_available"])
        # the mirror-lake cover itself is fine
        self.assert_served("fx-mirror-lake-ps5", "cover", src("fx-mirror-lake-ps5", "cover.webp"))

    # ---- failures ----------------------------------------------------------------
    def test_11_502_after_a_500(self):
        a = self.w.art
        path = src("fx-paper-lanterns-ps5", "cover.webp")
        a.set_status(path, 500)
        try:
            self.wait_art("fx-paper-lanterns-ps5").error(502, tc.S["art_failed"])
            for _ in range(3):
                self.c.get(art("fx-paper-lanterns-ps5")).error(502, tc.S["art_failed"])
            self.assertEqual(a.hits(path), 1, "refetched inside the 120 s window")
            self.assertRegex(self.w.log_text(), r"tvart: could not fetch for fx-paper-lanterns-ps5#cover: HTTP 500")
        finally:
            a.clear(path)
        # the hero of the same game is independent
        self.assert_served("fx-paper-lanterns-ps5", "hero", src("fx-paper-lanterns-ps5", "hero.webp"))

    def test_12_redirects(self):
        a = self.w.art
        # followed on the same origin
        first = src("fx-aurora-drift-ps5", "cover.webp")
        a.set_redirect(first, src("fx-aurora-drift-ps5", "cover.webp", True))
        self.assert_served("fx-aurora-drift-ps5", "cover", src("fx-aurora-drift-ps5", "cover.webp", True))
        self.assertEqual(a.hits(first), 1)
        # where the hops end is checked again: a loopback name is outside the policy
        p = src("fx-hollow-crown-ps5", "cover.webp")
        a.set_redirect(p, f"http://localhost:{a.port}" + src("fx-hollow-crown-ps5", "cover.webp", True))
        self.assert_unknown(self.wait_art("fx-hollow-crown-ps5"))
        self.assertIn("tvart: refused for fx-hollow-crown-ps5#cover: redirected outside the art URL policy",
                      self.w.log_text())
        self.assertEqual(a.hits(src("fx-hollow-crown-ps5", "cover.webp", True)), 0,
                         "a hop outside the policy was contacted")
        # every hop is checked before it is contacted: a loopback hop that bounces back to
        # the fixture origin is refused without being requested
        p = src("fx-frostline-ps4", "cover.jpg")
        bounce = "/bounce/internal-action"
        a.set_redirect(p, f"http://localhost:{a.port}{bounce}")
        a.set_redirect(bounce, a.base + src("fx-frostline-ps4", "cover.jpg", True))
        self.assert_unknown(self.wait_art("fx-frostline-ps4"))
        self.assertEqual(a.hits(bounce), 0, "the loopback hop was requested")
        self.assertEqual(a.hits(src("fx-frostline-ps4", "cover.jpg", True)), 0)
        self.assertIn("tvart: refused for fx-frostline-ps4#cover: redirected outside the art URL policy",
                      self.w.log_text())
        # at most three hops
        chain = [src("fx-cafe-lumiere-ps4", f"hop{i}.webp") for i in range(5)]
        a.set_redirect(src("fx-cafe-lumiere-ps4", "cover.webp"), chain[0])
        for i in range(4):
            a.set_redirect(chain[i], chain[i + 1])
        self.wait_art("fx-cafe-lumiere-ps4").error(502, tc.S["art_failed"])
        self.assertEqual(a.hits(chain[2]), 1)
        self.assertEqual(a.hits(chain[3]), 0)

    def test_13_memory_budget(self):
        self.assert_memory_budget()


# --------------------------------------------------------------------------- #
def limits_catalog():
    """The fixture plus lim-NNN-ps5 (300 small covers), big-N-ps5 (6 big covers) and
    q-NNN-ps5 (70 covers for the queue test)."""
    cat = json.loads(open(tvworld.FIXTURE_CATALOG, encoding="utf-8").read())

    def game(gid):
        return {"id": gid, "slug": gid, "title": gid.replace("-", " ").title(), "platform": "PS5",
                "cover": "{{ART}}/art/" + gid + "/cover.webp", "releases": []}
    cat["games"] += [game(f"lim-{i:03d}-ps5") for i in range(300)]
    cat["games"] += [game(f"big-{i}-ps5") for i in range(6)]
    cat["games"] += [game(f"q-{i:03d}-ps5") for i in range(70)]
    return cat


class TvArtLimits(unittest.TestCase, ArtMixin):
    @classmethod
    def setUpClass(cls):
        start_world(cls, "art-limits", catalog=limits_catalog(), files=None)

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    def test_1_budget_evicts_least_recently_served(self):
        a = self.w.art
        big = 5 * MiB
        for i in range(6):
            a.set_bytes(src(f"big-{i}-ps5", "cover.webp"), padded_webp(big, f"big-{i}"), "image/webp")
        for i in range(4):                                        # 20 MiB
            self.assert_served(f"big-{i}-ps5", "cover", src(f"big-{i}-ps5", "cover.webp"))
        time.sleep(0.05)
        self.assert_image(self.c.get(art("big-0-ps5")), src("big-0-ps5", "cover.webp"))   # big-0 is fresh again
        self.assert_served("big-4-ps5", "cover", src("big-4-ps5", "cover.webp"))          # 25 MiB: one must go
        self.assertRegex(self.w.log_text(), r"tvart: evicted big-1-ps5#cover \(5242880 bytes, budget\)")
        self.assert_pending(self.c.get(art("big-1-ps5")))                                 # evicted: fetched again
        self.assert_served("big-1-ps5", "cover", src("big-1-ps5", "cover.webp"))
        self.assertEqual(a.hits(src("big-1-ps5", "cover.webp")), 2)
        self.assert_image(self.c.get(art("big-0-ps5")), src("big-0-ps5", "cover.webp"))
        self.assertEqual(a.hits(src("big-0-ps5", "cover.webp")), 1)
        self.assert_memory_budget()

    def test_2_entry_limit_recycles_the_oldest(self):
        a = self.w.art
        ids = [f"lim-{i:03d}-ps5" for i in range(300)]
        for start in range(0, len(ids), 50):
            batch = ids[start:start + 50]
            for gid in batch:
                self.assertIn(self.c.get(art(gid)).status, (200, 202))
            for gid in batch:
                self.assert_served(gid, "cover", src(gid, "cover.webp"))
        # 300 new keys through 256 slots: the first ones were recycled, the last are cached
        self.assert_image(self.c.get(art(ids[-1])), src(ids[-1], "cover.webp"))
        self.assert_pending(self.c.get(art(ids[0])))
        self.assert_served(ids[0], "cover", src(ids[0], "cover.webp"))
        self.assertEqual(a.hits(src(ids[0], "cover.webp")), 2)
        self.assertEqual(a.hits(src(ids[-1], "cover.webp")), 1)
        self.assert_memory_budget()

    def test_3_queue_full_then_stale_keys_make_way(self):
        a = self.w.art
        ids = [f"q-{i:03d}-ps5" for i in range(70)]
        for gid in ids:
            a.set_delay(src(gid, "cover.webp"), 30.0)   # beyond the 10 s receive timeout: each fetch fails at 10 s
        try:
            # both workers busy
            for gid in ids[:2]:
                self.assert_pending(self.c.get(art(gid)))
                harness.wait_until(lambda: a.hits(src(gid, "cover.webp")) == 1, timeout=10,
                                   what=f"a worker fetching {gid}")
            # 64 queued, each answered at once
            t_queued = time.monotonic()
            elapsed = []
            for gid in ids[2:2 + QUEUE]:
                r = self.c.get(art(gid))
                self.assert_pending(r)
                elapsed.append(r.elapsed)
            self.assertLess(statistics.median(elapsed), FIRST_REPLY_BUDGET, elapsed)
            self.assertLess(max(elapsed), NEVER_BLOCKS, elapsed)
            # full: 503 busy (the app asks again in 0.6 s)
            r = self.c.get(art(ids[66]))
            r.error(503, tc.S["art_busy"])
            self.assertLess(r.elapsed, NEVER_BLOCKS)
            # cached and refused keys still answer while the workers wait
            self.assert_unknown(self.c.get(art("no-such-game-ps5")))
            # after the receive timeout the workers take the newest queued keys
            for gid in (ids[65], ids[64]):
                harness.wait_until(lambda: a.hits(src(gid, "cover.webp")) == 1, timeout=20,
                                   what=f"a worker taking {gid}")
            self.c.get(art(ids[0])).error(502, tc.S["art_failed"])
            for gid in (ids[66], ids[67]):                       # the two free places
                self.assert_pending(self.c.get(art(gid)))
            # keys q-002.. have not been asked for since; once that is 10 s, the oldest makes way
            wait = t_queued + STALE_S + 0.5 - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            self.assert_pending(self.c.get(art(ids[68])))
            self.assertRegex(self.w.log_text(), rf"tvart: dropped {ids[2]}#cover from the queue")
            self.assert_pending(self.c.get(art(ids[2])))        # asked again: queued again
            self.assert_pending(self.c.get(art(ids[66])))       # asked recently: still waiting
            self.assertNotRegex(self.w.log_text(), rf"tvart: dropped {ids[66]}#cover")
        finally:
            for gid in ids:
                a.clear(src(gid, "cover.webp"))


# --------------------------------------------------------------------------- #
class TvArtRevision(unittest.TestCase, ArtMixin):
    @classmethod
    def setUpClass(cls):
        start_world(cls, "art-revision", files=None)

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    def test_new_catalogue_clears_cache_and_refusals(self):
        a, w = self.w.art, self.w
        anim = src("fx-starfall-rally-ps5", "anim.webp")
        self.assert_unknown(self.wait_art("fx-starfall-rally-ps5"))
        self.assertEqual(a.hits(anim), 1)
        # the server now has a good picture at the same URL, but the refusal holds for this catalogue
        a.set_bytes(anim, artgen.make("cover.webp", seed="fixed-later"), "image/webp")
        self.assert_unknown(self.c.get(art("fx-starfall-rally-ps5")))
        self.assertEqual(a.hits(anim), 1)
        self.assert_served("fx-tidewater-ps5", "cover", src("fx-tidewater-ps5", "cover.webp"))

        # a newer catalogue: tidewater's cover moves to another URL
        v2 = json.loads(json.dumps(w.catalog))
        v2["revision"] = w.catalog["revision"] + 1
        moved = src("fx-tidewater-ps5", "cover-lossless.webp", True)
        for g in v2["games"]:
            if g["id"] == "fx-tidewater-ps5":
                g["cover"] = a.base + moved
        a.set_bytes("/catalog-v2.json", json.dumps(v2).encode("utf-8"), "application/json")
        a.set_bytes("/revision.json", json.dumps({"revision": v2["revision"]}).encode(), "application/json")
        st, b = w.srv.api("PUT", "/api/settings", {"catalog": {"url": a.base + "/catalog-v2.json",
                                                               "revisionUrl": a.base + "/revision.json"}})
        self.assertEqual(st, 200, b)
        st, b = w.srv.api("POST", "/api/catalog/refresh", {})
        self.assertEqual((st, b.get("updated")), (200, True), b)
        w.wait_log(INDEX_LINE.format(rev=v2["revision"]) + r", .*; cache cleared", timeout=15)

        # refusal memory cleared: fetched again, now served
        self.assert_pending(self.c.get(art("fx-starfall-rally-ps5")))
        self.assert_served("fx-starfall-rally-ps5", "cover", anim)
        self.assertEqual(a.hits(anim), 2)
        # cache cleared: the new URL
        self.assert_pending(self.c.get(art("fx-tidewater-ps5")))
        self.assert_served("fx-tidewater-ps5", "cover", moved)
        self.assert_memory_budget()


# --------------------------------------------------------------------------- #
# The candidate rule, mirrored (PORT-SPEC 1.7; map-catalog-art §8.2; web/src/js
# 10-state.js _artOk and 00-util.js sharpArt).
PS_IMAGE_HOST = "image.api.playstation.com"


def sharp_art(url, width):
    base = re.sub(r"([?&])w=\d+&?", r"\1", url, count=1)
    base = re.sub(r"[?&]$", "", base)
    return base + ("&" if "?" in base else "?") + f"w={width}"


def py_url(url, hero):
    """The URL policy on a public catalogue URL: the URL fetched, or None."""
    if not url or len(url) >= 2048 or any(ord(ch) <= 0x20 or ord(ch) == 0x7F for ch in url):
        return None
    sp = urllib.parse.urlsplit(url)
    if sp.scheme.lower() != "https" or "@" in sp.netloc or not sp.hostname:
        return None
    if sp.path.lower().endswith((".avif", ".gif")):
        return None
    if sp.hostname == PS_IMAGE_HOST:
        return sharp_art(url, 1920 if hero else 512)
    return url


def py_candidates(catalog):
    """{gameId: [cover1, cover2, hero1, hero2]} (None = no candidate) for every game with one."""
    games = catalog.get("games", [])
    use = Counter(g.get("cover") for g in games if g.get("cover"))
    out = {}
    for g in games:
        gid = g.get("id") or ""
        if not re.fullmatch(r"[A-Za-z0-9._-]{1,95}", gid) or gid in (".", ".."):
            continue
        c, cf, h, hf = (g.get(k) or "" for k in ("cover", "coverFallback", "hero", "heroFallback"))
        shared = bool(c) and use[c] > 1
        bad = not c or shared or "archive.org/services/img" in c
        cand = ([cf, "" if shared else c] if bad else [c, cf]) + [h if h != c else "", hf if hf != cf else ""]
        urls = []
        for k, u in enumerate(cand):
            v = py_url(u, k >= 2)
            if v and k % 2 and urls[k - 1] == v:
                v = None
            urls.append(v)
        if any(urls):
            out[gid] = urls
    return out


class TvArtRealCatalogue(unittest.TestCase, ArtMixin):
    DEAD = "http://127.0.0.1:9"     # nothing listens: every https fetch fails at once

    @classmethod
    def setUpClass(cls):
        start_world(cls, "art-real", catalog="real", featured=None, art=False, files=None,
                    env={"HTTPS_PROXY": cls.DEAD, "HTTP_PROXY": cls.DEAD, "NO_PROXY": ""})
        cls.expect = py_candidates(cls.w.catalog)

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    def test_1_index_matches_the_rule(self):
        games, covers, heroes = (int(x) for x in self.index.groups())
        e = self.expect
        self.assertEqual(games, len(e))
        self.assertEqual(covers, sum(1 for u in e.values() if u[0] or u[1]))
        self.assertEqual(heroes, sum(1 for u in e.values() if u[2] or u[3]))

    def _sample(self):
        """Keys whose candidates show every rule: PlayStation rewrites, two candidates,
        item-specific archive thumbnails, heroes. Short enough for one log line each."""
        picks = {}

        def take(tag, n, pred):
            got = 0
            for gid in sorted(self.expect):
                if got >= n:
                    break
                urls = self.expect[gid]
                for kind, pair in (("cover", urls[:2]), ("hero", urls[2:])):
                    key = (gid, kind)
                    if key in picks or not any(pair) or len(gid) > 50 or any(u and len(u) > 150 for u in pair):
                        continue
                    if pred(kind, pair):
                        picks[key] = [u for u in pair if u]
                        got += 1
                        break
        take("ps cover", 6, lambda k, p: k == "cover" and any(u and PS_IMAGE_HOST in u for u in p))
        take("ps hero", 4, lambda k, p: k == "hero" and any(u and PS_IMAGE_HOST in u for u in p))
        take("two covers", 6, lambda k, p: k == "cover" and all(p))
        take("two heroes", 4, lambda k, p: k == "hero" and all(p))
        take("archive item", 3, lambda k, p: k == "cover" and any(u and "archive.org/services/img" in u for u in p))
        take("plain", 6, lambda k, p: True)
        return picks

    def test_2_urls_tried_in_order(self):
        picks = self._sample()
        self.assertGreaterEqual(len(picks), 20, picks)
        firsts = []
        for gid, kind in picks:
            r = self.c.get(art(gid, kind))
            self.assertIn(r.status, (202, 502), r)
            firsts.append(r.elapsed)
        self.assertLess(statistics.median(firsts), FIRST_REPLY_BUDGET, firsts)
        for (gid, kind), urls in picks.items():
            with self.subTest(gid=gid, kind=kind):
                self.wait_art(gid, kind).error(502, tc.S["art_failed"])     # every candidate failed (dead proxy)
                lines = re.findall(rf"^\S+ WARN  tvart: could not fetch for {re.escape(gid)}#{kind}: .*: (\S+)$",
                                   self.w.log_text(), re.M)
                self.assertEqual(lines, urls)
        for (gid, kind), urls in picks.items():
            for u in urls:
                if PS_IMAGE_HOST in u:
                    self.assertRegex(u, r"[?&]w=512$" if kind == "cover" else r"[?&]w=1920$")

    def test_3_no_candidate_is_404_at_once(self):
        ids = [g["id"] for g in self.w.catalog["games"]]
        none_cover = [g for g in ids if g not in self.expect and re.fullmatch(r"[A-Za-z0-9._-]{1,60}", g)][:10]
        no_hero = [g for g, u in sorted(self.expect.items()) if not (u[2] or u[3]) and len(g) <= 60][:10]
        self.assertTrue(none_cover and no_hero)
        for gid, kind in [(g, "cover") for g in none_cover] + [(g, "hero") for g in no_hero]:
            with self.subTest(gid=gid, kind=kind):
                r = self.c.get(art(gid, kind))
                self.assert_unknown(r)
                self.assertLess(r.elapsed, NEVER_BLOCKS)
        self.assertNotRegex(self.w.log_text(), rf"tvart: could not fetch for ({'|'.join(map(re.escape, none_cover))})#")


if __name__ == "__main__":
    unittest.main()
