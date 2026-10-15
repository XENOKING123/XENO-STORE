"""dlpsgame completeness (SPEC §17.10): category-tree expansion, the site's own X-WP-Total / X-WP-TotalPages,
the PS5 category-listing cross-check with its top-up chain (REST by slug → post page → listing card), the
snapshot written only after a complete online run, and snapshot rows with or without date / title_ids."""
from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import threading
import time
import unittest
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

from fakes import FakeHttp, Resp, fixture, fixture_json, make_ctx  # noqa: E402
from netutil import BudgetExceeded, Http, HttpError  # noqa: E402
from schema import SourceInfo, validate_catalog  # noqa: E402
from sources import dlpsgame  # noqa: E402
import merge  # noqa: E402

PS5_LIST = "https://dlpsgame.com/category/ps5/"
SNAP = "xeno-dlpsgame/catalog.json"


def categories_route(tree: dict | None = None):
    """``categories?parent=N`` → the children of N from the fixture tree (or ``tree``)."""
    tree = tree if tree is not None else fixture_json("dlpsgame_categories.json")

    def route(url: str):
        q = urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)
        return tree.get(q["parent"][0], [])
    return route


def slug_route(url: str):
    wanted = set(urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)["slug"][0].split(","))
    return [p for p in fixture_json("dlpsgame_rest_by_slug.json") if p["link"].rstrip("/").rsplit("/", 1)[-1] in wanted]


def site_routes(*, ps4_posts=None, ps4_headers=None, extra=()) -> list:
    """The canned site: tree, PS5/PS4 REST with totals, slug lookups, the PS5 listing (2 pages), one post page."""
    ps5 = fixture_json("dlpsgame_rest_ps5.json")
    ps4 = fixture_json("dlpsgame_rest_ps4.json") if ps4_posts is None else ps4_posts
    return list(extra) + [
        (r"/wp-json/wp/v2/categories\?parent=", categories_route()),
        (r"/wp-json/wp/v2/posts\?slug=", slug_route),
        (r"/wp-json/wp/v2/posts\?categories=63019[,&]", Resp(ps5, {"X-WP-Total": len(ps5), "X-WP-TotalPages": 1})),
        (r"/wp-json/wp/v2/posts\?categories=4370[,&]",
         Resp(ps4, ps4_headers or {"X-WP-Total": len(ps4), "X-WP-TotalPages": 1})),
        (r"/category/ps5/$", fixture("dlpsgame_category_ps5_p1.html")),
        (r"/category/ps5/page/2/$", fixture("dlpsgame_category_ps5_p2.html")),
        (r"/stellar-blade-ps5/$", fixture("dlpsgame_post_stellar.html")),
    ]


class TempRoot:
    """A throwaway project root so a test can never touch the committed snapshot."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="xeno-dlps-"))
        self.snap = self.root / "catalog" / "snapshots" / SNAP
        self.snap.parent.mkdir(parents=True)

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def write_old_snapshot(self, rows: list) -> bytes:
        self.snap.write_text(json.dumps(rows), encoding="utf-8")
        return self.snap.read_bytes()

    def ctx(self, http, **kw):
        kw.setdefault("budget", 60.0)
        return make_ctx(http, source_id="dlpsgame", root=self.root, **kw)


class CategoryTreeTests(TempRoot, unittest.TestCase):
    def test_tree_is_expanded_recursively_and_every_id_is_queried(self):
        http = FakeHttp(site_routes())
        ctx = self.ctx(http)
        run = dlpsgame.PlatformRun("PS5")
        dlpsgame.expand_categories(ctx, run)
        self.assertEqual(run.category_ids, [63019, 70001, 70002, 70003])   # grandchild 70003 included
        self.assertTrue(run.tree_ok)
        self.assertTrue({"ps5", "ps5-games", "ps5 games", "ps5-vr2"} <= run.category_names)
        dlpsgame.fetch_rest(ctx)
        posts = [u for u in http.seen if "/posts?categories=" in u]
        self.assertTrue(any("categories=63019,70001,70002,70003&" in u for u in posts))
        self.assertTrue(any("categories=4370,4371&" in u for u in posts))
        self.assertTrue(any("category 63019 + 3 subcategories" in line for line in ctx.logs))

    def test_category_failure_falls_back_to_the_root_id(self):
        http = FakeHttp([(r"/wp-json/wp/v2/categories", 403)] + site_routes())
        ctx = self.ctx(http)
        drafts = dlpsgame.fetch_rest(ctx)
        self.assertTrue(any("/posts?categories=63019&" in u for u in http.seen))
        self.assertTrue(any("using the root category only" in line for line in ctx.logs))
        # The REST half is not trusted as complete (tree unknown), but the PS5 listing ran to its end.
        self.assertIn("https://dlpsgame.com/stellar-blade-ps5/", {d.pageUrl for d in drafts})

    def test_children_are_paged_and_depth_is_bounded(self):
        many = [{"id": 80000 + i, "slug": f"c{i}", "name": f"C{i}"} for i in range(100)]
        chain = {"63019": None, "1": [{"id": 2, "slug": "d2"}], "2": [{"id": 3, "slug": "d3"}],
                 "3": [{"id": 4, "slug": "d4"}], "4": [{"id": 5, "slug": "d5"}]}

        def route(url: str):
            q = urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)
            parent, page = q["parent"][0], int(q.get("page", ["1"])[0])
            if parent == "63019":
                return Resp(many if page == 1 else [{"id": 1, "slug": "d1"}], {"X-WP-TotalPages": 2})
            return chain.get(parent) or []
        old = dlpsgame.MAX_CATEGORY_REQUESTS
        dlpsgame.MAX_CATEGORY_REQUESTS = 500
        try:
            ctx = self.ctx(FakeHttp([(r"/categories\?parent=", route)]))
            run = dlpsgame.PlatformRun("PS5")
            dlpsgame.expand_categories(ctx, run)
        finally:
            dlpsgame.MAX_CATEGORY_REQUESTS = old
        self.assertIn(1, run.category_ids)                         # found on the second page of children
        self.assertEqual(len(run.category_ids), 1 + 100 + 1 + 3)   # root, 100 + d1, then d2..d4 (depth 4)
        self.assertNotIn(5, run.category_ids)
        self.assertTrue(any("deeper than 4 levels" in line for line in ctx.logs))


    def test_large_tree_is_read_from_the_full_category_list_once(self):
        kids = [{"id": 90000 + i, "slug": f"ps5-sub-{i}", "name": f"PS5 Sub {i}", "parent": 63019} for i in range(30)]
        grandkid = {"id": 99999, "slug": "ps5-deep", "name": "PS5 Deep", "parent": 90007}
        ps4_kid = {"id": 4371, "slug": "ps4-games", "name": "PS4 Games", "parent": 4370}
        full = kids + [grandkid, ps4_kid, {"id": 5, "slug": "news", "name": "News", "parent": 0}]

        def route(url: str):
            q = urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)
            if "parent" in q:
                return {"63019": kids, "4370": [ps4_kid]}.get(q["parent"][0], [])
            return Resp(full, {"X-WP-Total": len(full), "X-WP-TotalPages": 1})
        http = FakeHttp([(r"/categories\?", route)])
        ctx = self.ctx(http)
        cache: dict = {}
        ps5, ps4 = dlpsgame.PlatformRun("PS5"), dlpsgame.PlatformRun("PS4")
        dlpsgame.expand_categories(ctx, ps5, cache)
        dlpsgame.expand_categories(ctx, ps4, cache)
        self.assertTrue(ps5.tree_ok)
        self.assertEqual(len(ps5.category_ids), 1 + 30 + 1)          # root, 30 children, the grandchild
        self.assertIn(99999, ps5.category_ids)
        self.assertNotIn(5, ps5.category_ids)
        # 1 lookup for the root's children, then the 30-wide level goes to the full list (read once for both).
        self.assertEqual(sum(1 for u in http.seen if "parent=63019" in u), 1)
        self.assertEqual(sum(1 for u in http.seen if "categories?per_page=100" in u), 1)
        self.assertEqual(ps4.category_ids, [4370, 4371])
        self.assertTrue(any("large category tree read from the full list of 33 categories" in line for line in ctx.logs))

    def test_large_tree_without_a_full_list_is_marked_incomplete(self):
        kids = [{"id": 90000 + i, "slug": f"s{i}", "parent": 63019} for i in range(30)]
        http = FakeHttp([(r"/categories\?parent=63019&", kids), (r"/categories\?per_page", 500)])
        run = dlpsgame.PlatformRun("PS5")
        dlpsgame.expand_categories(self.ctx(http), run)
        self.assertFalse(run.tree_ok)
        self.assertEqual(len(run.category_ids), 31)                  # what was found is still used


class HeaderTotalsTests(TempRoot, unittest.TestCase):
    def test_site_totals_drive_pagination_and_are_logged(self):
        ps5 = fixture_json("dlpsgame_rest_ps5.json")

        def ps5_route(url: str):
            page = int(urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)["page"][0])
            if page > 2:
                raise AssertionError("requested a page past X-WP-TotalPages")
            return Resp(ps5[:2] if page == 1 else ps5[2:], {"X-WP-Total": 3, "X-WP-TotalPages": 2})
        http = FakeHttp([(r"/posts\?categories=63019,", ps5_route)] + site_routes())
        old = dlpsgame.REST_PER_PAGE
        dlpsgame.REST_PER_PAGE = 2
        try:
            ctx = self.ctx(http)
            dlpsgame.fetch_rest(ctx)
        finally:
            dlpsgame.REST_PER_PAGE = old
        self.assertEqual(sum(1 for u in http.seen if "/posts?categories=63019," in u), 2)
        self.assertTrue(any(line.endswith("PS5: REST posts 3, site total 3, listing posts 6 (complete), topped up 3, final 6")
                            for line in ctx.logs), ctx.logs)
        self.assertTrue(any("PS4: REST posts 3, site total 3, listing posts - (skipped), topped up 0, final 2" in line
                            for line in ctx.logs), ctx.logs)

    def test_client_without_headers_still_works_and_cross_checks_ps4(self):
        class NoHeaders(FakeHttp):
            json_with_headers = None
        http = NoHeaders(site_routes() + [(r"/category/ps4/$", 404)])
        ctx = self.ctx(http)
        drafts = dlpsgame.fetch_rest(ctx)
        self.assertEqual(sum(1 for d in drafts if d.platform == "PS5"), 6)
        self.assertTrue(any("PS5: REST posts 3, site total ?" in line for line in ctx.logs))
        # Without a site total there is nothing to trust, so PS4 is cross-checked too.
        self.assertIn("https://dlpsgame.com/category/ps4/", http.seen)

    def test_http_client_exposes_lower_cased_headers(self):
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                body = json.dumps([{"id": 1}]).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("X-WP-Total", "1320")
                self.send_header("X-WP-TotalPages", "14")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            http = Http(min_interval=0.0)
            url = f"http://127.0.0.1:{server.server_address[1]}/wp-json/wp/v2/posts"
            data, headers = http.json_with_headers(url, deadline=time.monotonic() + 10)
            self.assertEqual(data, [{"id": 1}])
            self.assertEqual((headers["x-wp-total"], headers["x-wp-totalpages"]), ("1320", "14"))
            self.assertEqual(http.get(url), b'[{"id": 1}]')                    # the plain API is unchanged
            self.assertEqual(http.requests, 2)
        finally:
            server.shutdown()
            server.server_close()


class ListingCrossCheckTests(TempRoot, unittest.TestCase):
    def test_listing_parser_keeps_post_cards_and_drops_menus_sidebars_and_footers(self):
        p1 = dlpsgame.parse_listing_posts(fixture("dlpsgame_category_ps5_p1.html"))
        self.assertEqual([e.url.rsplit("/", 2)[-2] for e in p1],
                         ["astro-bot-ps5", "ghost-of-yotei-ps5", "elden-ring-nightreign-ps5", "stellar-blade-ps5"])
        self.assertEqual(p1[0].title, "Astro Bot PS5")            # not "No Comments"
        self.assertEqual(p1[0].cover, "https://dlpsgame.com/wp-content/uploads/2026/09/astro.jpg")   # data-src
        self.assertEqual(p1[2].cover, "https://dlpsgame.com/wp-content/uploads/2026/05/nightreign.jpg")
        p2 = dlpsgame.parse_listing_posts(fixture("dlpsgame_category_ps5_p2.html"))
        self.assertNotIn("https://dlpsgame.com/sidebar-article-ps4/", [e.url for e in p2])
        self.assertEqual(dlpsgame.listing_last_page(fixture("dlpsgame_category_ps5_p1.html"), PS5_LIST), 2)

    def test_listing_parser_handles_heading_cards_without_article_tags(self):
        page = ('<html><body><h1 class="logo"><a href="https://dlpsgame.com/">DLPSGAME</a></h1>'
                '<div class="sidebar-right wrap"><ul class="posts">'
                '<li class="post-item"><a href="https://dlpsgame.com/returnal-ps5/"><img src="https://dlpsgame.com/wp-content/uploads/r.jpg"></a>'
                '<h2 class="post-title"><a href="https://dlpsgame.com/returnal-ps5/">Returnal PS5</a></h2></li>'
                '<li class="post-item"><h2 class="post-title"><a href="https://dlpsgame.com/sackboy-ps5/">Sackboy PS5</a></h2></li>'
                '</ul><div id="sidebar"><h3><a href="https://dlpsgame.com/other-ps4/">Other PS4</a></h3></div></div></body></html>')
        posts = dlpsgame.parse_listing_posts(page)
        self.assertEqual([(e.url, e.title) for e in posts], [("https://dlpsgame.com/returnal-ps5/", "Returnal PS5"),
                                                             ("https://dlpsgame.com/sackboy-ps5/", "Sackboy PS5")])
        self.assertEqual(posts[0].cover, "https://dlpsgame.com/wp-content/uploads/r.jpg")

    def test_posts_only_on_the_listing_are_topped_up(self):
        http = FakeHttp(site_routes())
        ctx = self.ctx(http)
        drafts = {d.pageUrl: d for d in dlpsgame.fetch_rest(ctx)}
        ps5 = {u.rsplit("/", 2)[-2]: d for u, d in drafts.items() if d.platform == "PS5"}
        self.assertEqual(sorted(ps5), ["astro-bot-ps5", "death-stranding-2-ps5", "elden-ring-nightreign-ps5",
                                       "ghost-of-yotei-ps5", "hogwarts-legacy-ps5-ps4", "stellar-blade-ps5"])
        # 1. REST by slug: full bundle, filed under a PS5 child category.
        elden = ps5["elden-ring-nightreign-ps5"]
        self.assertEqual([(r.kind, r.url, r.titleId) for r in elden.releases], [("game", "https://gofile.io/d/nightreign", "PPSA17418")])
        # 2. Its own page (REST did not know it).
        stellar = ps5["stellar-blade-ps5"]
        self.assertEqual((stellar.title, stellar.addedAt, stellar.releases[0].url), ("Stellar Blade", "2025-04-26", "https://1fichier.com/?stellar"))
        # 3. The page 404s: the listing card still puts the game in the catalog.
        ds2 = ps5["death-stranding-2-ps5"]
        self.assertEqual((ds2.title, ds2.cover, ds2.titleIds), ("Death Stranding 2: On the Beach",
                                                                 "https://dlpsgame.com/wp-content/uploads/2025/06/ds2.jpg", ["PPSA22105"]))
        self.assertEqual([(r.format, r.url) for r in ds2.releases], [("PAGE", "https://dlpsgame.com/death-stranding-2-ps5/")])
        # A PS4 post shown on the PS5 listing is not filed as PS5; menu / sidebar / footer links never were candidates.
        self.assertNotIn("https://dlpsgame.com/ratchet-clank-ps4/", drafts)
        for junk in ("sidebar-only-ps4", "footer-only-ps4", "menu-only-ps4", "sidebar-article-ps4"):
            self.assertFalse(any(junk in u for u in http.seen), junk)
        # The cross-gen post stays PS5 (PS5 runs first) and REST precedence is kept for known posts.
        self.assertEqual(drafts["https://dlpsgame.com/hogwarts-legacy-ps5-ps4/"].platform, "PS5")
        self.assertEqual(drafts["https://dlpsgame.com/astro-bot-ps5/"].releases[0].url, "https://1fichier.com/?astro")
        self.assertTrue(any("4 listing posts missing from REST -> 1 via REST by slug, 1 from their pages, "
                            "1 from listing cards, 1 not filed under PS5 (skipped)" in line for line in ctx.logs), ctx.logs)
        # PS4 REST matched its site total, so its ~270 listing pages were not crawled.
        self.assertFalse(any("/category/ps4/" in u for u in http.seen))
        self.assertEqual(http.seen.count("https://dlpsgame.com/category/ps5/page/3/"), 1)   # 404 ended the crawl

    def test_a_missing_page_inside_the_pagination_is_not_the_end(self):
        page1 = fixture("dlpsgame_category_ps5_p1.html").replace("/category/ps5/page/2/", "/category/ps5/page/3/")
        http = FakeHttp([(r"/category/ps5/$", page1), (r"/category/ps5/page/2/$", 404),
                         (r"/category/ps5/page/3/$", fixture("dlpsgame_category_ps5_p2.html"))] + site_routes())
        posts, status = dlpsgame.crawl_listing(self.ctx(http), "PS5")
        self.assertEqual(status, "partial")                    # page 2 of 3 was lost, so not "complete"
        self.assertIn("https://dlpsgame.com/death-stranding-2-ps5/", [p.url for p in posts])   # page 3 still read
        self.assertIn("https://dlpsgame.com/category/ps5/page/4/", http.seen)

    def test_ps4_is_cross_checked_when_rest_falls_short_of_the_site_total(self):
        ps4 = fixture_json("dlpsgame_rest_ps4.json")
        listing = ('<main><article><h2><a href="https://dlpsgame.com/bloodborne-ps4/" rel="bookmark">Bloodborne PS4</a></h2></article>'
                   '<article><h2><a href="https://dlpsgame.com/days-gone-ps4/" rel="bookmark">Days Gone PS4</a></h2></article></main>')
        http = FakeHttp(site_routes(ps4_headers={"X-WP-Total": len(ps4) + 1, "X-WP-TotalPages": 1},
                                    extra=[(r"/category/ps4/$", listing)]))
        ctx = self.ctx(http)
        drafts = {d.pageUrl: d for d in dlpsgame.fetch_rest(ctx)}
        self.assertEqual(drafts["https://dlpsgame.com/days-gone-ps4/"].platform, "PS4")
        self.assertTrue(any("PS4: REST posts 3, site total 4, listing posts 2 (complete), topped up 1, final 3" in line
                            for line in ctx.logs), ctx.logs)


class SnapshotTests(TempRoot, unittest.TestCase):
    OLD = [
        {"name": "Death Stranding 2", "platform": "PS5", "cover_url": "", "page_url": "https://dlpsgame.com/death-stranding-2-ps5/",
         "date": "2025-06-26"},
        {"name": "Removed Game", "platform": "PS4", "cover_url": "", "page_url": "https://dlpsgame.com/removed-game-ps4/"},
    ]

    def test_complete_run_writes_the_full_sorted_compact_snapshot(self):
        self.write_old_snapshot(self.OLD)
        ctx = self.ctx(FakeHttp(site_routes()))
        drafts = dlpsgame.fetch_rest(ctx)
        text = self.snap.read_text(encoding="utf-8")
        rows = json.loads(text)
        self.assertEqual([r["page_url"] for r in rows], sorted(d.pageUrl for d in drafts))
        self.assertEqual(len(rows), 8)                                         # 6 PS5 + 2 PS4, deleted post gone
        self.assertEqual(text.splitlines()[1:-1], [json.dumps(r, ensure_ascii=False, separators=(",", ":")) + ("," if i < 7 else "")
                                                   for i, r in enumerate(rows)])   # one compact row per line
        by = {r["page_url"].rsplit("/", 2)[-2]: r for r in rows}
        self.assertEqual(by["astro-bot-ps5"], {"name": "Astro Bot", "platform": "PS5", "page_url": "https://dlpsgame.com/astro-bot-ps5/",
                                               "cover_url": "https://dlpsgame.com/wp-content/uploads/2026/09/astro-300x300.jpg",
                                               "date": "2026-09-01", "title_ids": ["PPSA01342"]})
        self.assertEqual(by["death-stranding-2-ps5"]["date"], "2025-06-26")    # kept from the old row
        self.assertEqual(by["death-stranding-2-ps5"]["title_ids"], ["PPSA22105"])
        self.assertEqual(by["bloodborne-ps4"]["platform"], "PS4")
        self.assertTrue(set(by["god-of-war-ps4"]) >= {"name", "platform", "page_url", "cover_url"})
        self.assertTrue(any("snapshot written: 8 posts (6 PS5, 2 PS4)" in line for line in ctx.logs))
        # The snapshot round-trips into the same games, and a second identical run leaves the file alone.
        snap = dlpsgame.load_snapshot(self.ctx(FakeHttp()))
        self.assertEqual(sorted((d.title, d.platform) for d in snap), sorted((d.title, d.platform) for d in drafts))
        ctx2 = self.ctx(FakeHttp(site_routes()))
        dlpsgame.fetch_rest(ctx2)
        self.assertTrue(any("snapshot unchanged" in line for line in ctx2.logs))

    def test_partial_run_keeps_the_snapshot_and_tops_up_from_it(self):
        old = self.write_old_snapshot(self.OLD)
        ps4 = fixture_json("dlpsgame_rest_ps4.json")

        def ps4_route(url: str):
            page = int(urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)["page"][0])
            if page == 2:
                return 500
            return Resp(ps4, {"X-WP-Total": 5, "X-WP-TotalPages": 2})
        http = FakeHttp([(r"/posts\?categories=4370,", ps4_route), (r"/category/ps4/$", 404)] + site_routes())
        ctx = self.ctx(http)
        drafts = {d.pageUrl: d for d in dlpsgame.fetch_rest(ctx)}
        self.assertEqual(self.snap.read_bytes(), old)                         # never overwritten by a partial run
        self.assertEqual(drafts["https://dlpsgame.com/removed-game-ps4/"].platform, "PS4")   # PS4 topped up from it
        self.assertTrue(any("incomplete run (PS4): snapshot left as is; 1 posts topped up from it" in line for line in ctx.logs), ctx.logs)

    def test_short_budget_keeps_ps5_whole_and_takes_ps4_from_the_snapshot(self):
        old = self.write_old_snapshot(self.OLD)
        holder: dict = {}

        def ps4_tree(url: str):
            holder["ctx"].deadline = time.monotonic() - 1          # PS5 used the whole budget
            return []
        http = FakeHttp([(r"/categories\?parent=4370", ps4_tree)] + site_routes())
        ctx = holder["ctx"] = self.ctx(http)
        drafts = dlpsgame.fetch_rest(ctx)
        self.assertEqual(sum(1 for d in drafts if d.platform == "PS5"), 6)
        self.assertEqual(sorted(d.pageUrl for d in drafts if d.platform == "PS4"), ["https://dlpsgame.com/removed-game-ps4/"])
        self.assertEqual(self.snap.read_bytes(), old)
        self.assertFalse(any("/posts?categories=4370" in u for u in http.seen))

    def test_debug_limit_never_writes(self):
        old = self.write_old_snapshot(self.OLD)
        drafts = dlpsgame.fetch_rest(self.ctx(FakeHttp(site_routes()), limit=2))
        self.assertEqual(len(drafts), 2)
        self.assertEqual(self.snap.read_bytes(), old)

    def test_shrinking_snapshot_is_refused(self):
        rows = [{"name": f"Game {i}", "platform": "PS5", "cover_url": "", "page_url": f"https://dlpsgame.com/game-{i}-ps5/"}
                for i in range(20)]
        old = self.write_old_snapshot(rows)
        ctx = self.ctx(FakeHttp())
        d = dlpsgame.card_draft(dlpsgame.ListingPost("https://dlpsgame.com/game-1-ps5/", "Game 1 PS5"), "PS5")
        self.assertFalse(dlpsgame.write_snapshot(ctx, [d]))
        self.assertEqual(self.snap.read_bytes(), old)
        self.assertTrue(any("PS5 would shrink from 20 to 1 posts" in line for line in ctx.logs))

    def test_old_and_new_rows_load(self):
        self.write_old_snapshot([
            {"name": "3D MiniGolf", "platform": "PS5", "cover_url": "https://dlpsgame.com/c.jpg", "page_url": "https://dlpsgame.com/3d-minigolf-ps5/"},
            {"name": "Astro Bot", "platform": "ps5", "page_url": "https://dlpsgame.com/astro-bot-ps5/", "cover_url": "",
             "date": "2026-09-01", "title_ids": ["PPSA01342", "CUSA00001", "junk"]},
            {"name": "Broken date", "platform": "PS4", "page_url": "https://dlpsgame.com/x-ps4/", "date": "soon", "title_ids": "CUSA1"},
            {"name": "", "platform": "PS4", "page_url": "https://dlpsgame.com/no-name/"},
            {"name": "Vita game", "platform": "VITA", "page_url": "https://dlpsgame.com/vita/"},
            "not a row",
        ])
        drafts = dlpsgame.load_snapshot(self.ctx(FakeHttp()))
        self.assertEqual([(d.title, d.platform, d.addedAt, d.titleIds) for d in drafts], [
            ("3D MiniGolf", "PS5", "", []), ("Astro Bot", "PS5", "2026-09-01", ["PPSA01342"]), ("Broken date", "PS4", "", [])])
        self.assertEqual([(r.format, r.url, r.password, r.titleId) for r in drafts[1].releases],
                         [("PAGE", "https://dlpsgame.com/astro-bot-ps5/", "DLPSGAME.COM", "")])

    def test_committed_snapshot_still_loads(self):
        drafts = dlpsgame.load_snapshot(make_ctx(FakeHttp(), mode="offline", source_id="dlpsgame"))
        self.assertGreaterEqual(len(drafts), 799)
        self.assertEqual({d.platform for d in drafts}, {"PS4", "PS5"})

    def test_fetched_drafts_merge_into_a_valid_catalog(self):
        drafts = dlpsgame.fetch_rest(self.ctx(FakeHttp(site_routes())))
        sources = [SourceInfo(id="dlpsgame", name="DLPSGAME", kind="page", homepage="https://dlpsgame.com", password="DLPSGAME.COM")]
        cat, report = merge.merge_drafts(drafts, sources, revision=1, generated_at="2026-10-14T00:00:00Z")
        self.assertEqual(validate_catalog(cat.to_dict()), [])
        self.assertEqual(len(cat.games), 8)
        self.assertEqual(report.dropped_releases, {})


class FallbackStillWorksTests(TempRoot, unittest.TestCase):
    def test_rest_down_goes_to_the_sitemap_and_never_writes(self):
        old = self.write_old_snapshot(SnapshotTests.OLD)
        http = FakeHttp([(r"/wp-json/", 403), (r"sitemap", 404), (r"/category/ps5/$", fixture("dlpsgame_category_ps5_p1.html"))])
        ctx = self.ctx(http, budget=20.0)
        drafts = dlpsgame.fetch_online(ctx)
        self.assertTrue(drafts)
        self.assertTrue(any("REST listing failed" in line for line in ctx.logs))
        self.assertEqual(self.snap.read_bytes(), old)

    def test_budget_errors_inside_routes_are_contained(self):
        def boom(url: str):
            raise BudgetExceeded("out of time")
        http = FakeHttp([(r"/category/ps5/page/2/", boom)] + site_routes())
        ctx = self.ctx(http)
        drafts = dlpsgame.fetch_rest(ctx)
        self.assertTrue(any("PS5: REST posts 3, site total 3, listing posts 4 (partial)" in line for line in ctx.logs), ctx.logs)
        # REST covered PS5 completely, so the run is still complete even though the listing was cut short.
        self.assertTrue(self.snap.exists())
        self.assertEqual(sum(1 for d in drafts if d.platform == "PS5"), 5)
        with self.assertRaises(HttpError):
            dlpsgame.fetch_rest(self.ctx(FakeHttp([(r"/wp-json/", 403)])))


if __name__ == "__main__":
    unittest.main()
