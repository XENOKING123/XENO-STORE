"""dlpsgame.com — WordPress REST listing + per-post download bundles.

Port of ``client/src/lib/xenoStore.ts`` (XENO TOOL), made complete and self-checking:

* categories: each platform's root category (PS5 63019, PS4 4370) is expanded to every descendant with
  ``/wp-json/wp/v2/categories?parent=<id>`` (recursive, bounded). A REST ``categories=<root>`` query leaves
  out posts filed only under a child category; the root id alone is the fallback;
* listing: ``/wp-json/wp/v2/posts?categories=<root,children…>&per_page=100&page=N`` with
  ``_embed=wp:featuredmedia`` for covers (first content image as fallback). The site's own totals
  (``X-WP-Total`` / ``X-WP-TotalPages``) bound the pagination and are logged next to the REST count;
* cross-check: the human-visible category listing (``/category/ps5/`` and ``/page/N/``) is crawled for PS5
  on every run, for PS4 only when REST returned fewer posts than the site total (within the remaining
  budget). A post seen there but not via REST is topped up: through REST by slug (batched), else from its
  own page (``draft_from_page_html``, capped), else from the listing card, so the catalog matches the site;
* bundle: every post body carries one or more base64 ``data-payload`` blocks,
  each owned by the closest preceding ``CUSA12345 - REGION (v1.23)`` heading.
  Inside, ``<p>`` sections read ``Game : …``, ``Update 1.37 (Fix 5.05/…) : …``,
  ``DLC (18) : …``, ``Password : …``; mirrors are the ``<a>`` tags (gateway
  links decoded) or, when the site lists hosts as plain text, the page itself.

Each section becomes a release (kind game/update/dlc) with ``mirrors`` and the
first mirror as ``url``. A post without any payload still yields one PAGE
release so the game stays browsable. The site 403s non-browser agents, so the
shared client sends a browser User-Agent and keeps to ~1 request/second.

PS5 runs first and always fully; when the budget runs short, PS4 is the part topped up from the
snapshot. One log line per platform reads ``REST posts, site total, listing posts, topped up, final``.

Snapshot: ``catalog/snapshots/xeno-dlpsgame/catalog.json``, a JSON list of rows ``name``, ``platform``
(PS4|PS5), ``page_url``, ``cover_url`` and, when known, ``date`` (YYYY-MM-DD) and ``title_ids``. After a
COMPLETE online fetch it is rewritten with every PS4+PS5 post (sorted by page_url, one compact row per
line) so offline builds and later partial runs include every game; a partial run never overwrites it.
Offline, each row → one PAGE release.
"""
from __future__ import annotations

import base64
import binascii
import html
import json
import os
import re
import tempfile
import urllib.parse
from dataclasses import dataclass, field

from merge import (
    GameDraft, clean_title, make_release_id, parse_compat, parse_region, parse_title_ids, platform_for_title_id,
    format_from_filename,
)
from schema import Release
from sources.base import BudgetExceeded, HttpError, SourceContext, SourceSpec, page_release, read_json_file
from sources import wpsite

BASE = "https://dlpsgame.com"
SITE_HOST = "dlpsgame.com"
WP_API = BASE + "/wp-json/wp/v2/posts"
CATEGORY_API = BASE + "/wp-json/wp/v2/categories"
CATEGORY_IDS = {"PS5": 63019, "PS4": 4370}
LISTING_URLS = {"PS5": BASE + "/category/ps5/", "PS4": BASE + "/category/ps4/"}
SNAPSHOT_REL = "xeno-dlpsgame/catalog.json"
PASSWORD = "DLPSGAME.COM"
PROVIDER = "DLPSGAME"
ID_PREFIX = "dlps"

REST_FIELDS = "id,date,link,title,content,categories,_embedded"
REST_PER_PAGE = 100
MAX_REST_PAGES = 200                               # 20,000 posts per platform: a runaway guard, not a limit
MAX_CATEGORY_DEPTH = 4
MAX_CATEGORY_REQUESTS = 25                         # per-parent lookups per platform tree, then the full list
MAX_ALL_CATEGORY_PAGES = 30                        # full category list: up to 3,000 categories
MAX_IDS_PER_QUERY = 100                            # category ids per posts query (URL length)
MAX_LISTING_PAGES = {"PS5": 150, "PS4": 400}       # the site shows ~56 PS5 / ~270 PS4 listing pages
LISTING_RESERVE = {"PS5": 10.0, "PS4": 30.0}       # seconds a listing crawl leaves for its own top-up
SLUG_BATCH = 40                                    # slugs per REST lookup
TOPUP_PAGE_CAP = {"PS5": 150, "PS4": 60}           # individual post pages fetched per run
SNAPSHOT_SHRINK_GUARD = 0.8                        # never let a platform lose >20% of its snapshot rows at once
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")

HEADER_RE = re.compile(
    r"((?:CUSA|PPSA|PCAS)\s*\d{5})\s*[-–—]\s*([A-Z]{2,4}(?:\s*\([A-Za-z0-9\s]{1,15}\))?)"
    r"(?:\s*\(\s*v([0-9]+(?:\.[0-9]+){0,3})\s*\))?",
    re.I,
)
PAYLOAD_RE = re.compile(r'data-payload="([^"]+)"')
ANCHOR_RE = re.compile(r'<a[^>]+href="([^"]+)"[^>]*>([\s\S]*?)</a>', re.I)
P_BLOCK_RE = re.compile(r"<p[^>]*>([\s\S]*?)</p>", re.I)
TAG_RE = re.compile(r"<[^>]+>")
INLINE_ID_RE = re.compile(r"((?:C?USA|PPSA|PCAS)\d{5})\s*[-–—]\s*([A-Z]{2,4}(?:\s*\([^)]{1,15}\))?)", re.I)
UPDATE_RE = re.compile(r"^Update\s+([0-9]+(?:\.[0-9]+){0,3})\s*(\(([^)]+)\))?\s*:", re.I)
PASSWORD_RE = re.compile(r"^P[a]?(?:as)?sw[o]rd\s*:\s*(\S[^,]*)", re.I)
IMG_UPLOADS_RE = re.compile(r'<img[^>]+src="\s*(https?://[^"]+/wp-content/uploads/[^"]+\.(?:jpg|jpeg|png|webp))\s*"', re.I)
IMG_ANY_RE = re.compile(r'<img[^>]+src="\s*(https?://[^"\s]+\.(?:jpg|jpeg|png|webp))\s*"', re.I)
EXCLUDE_URL_RE = re.compile(r"guide-download-game|guide-download|tool-download", re.I)
EXCLUDE_LABEL_RE = re.compile(r"guide\s*download|tool\s*download|jdownload", re.I)
NON_HOST_WORD_RE = re.compile(
    r"^(v\d|fix|patch|here|click|link|dlc|update|game|password|works|voice|menu|screen|language|subtitle|region|thank|mega|free)",
    re.I,
)

# Category listing pages (cross-check).
_NON_POST_SLUGS = frozenset({
    "category", "tag", "page", "author", "feed", "search", "comments", "wp-admin", "wp-content", "wp-includes",
    "wp-json", "about", "about-us", "contact", "contact-us", "dmca", "privacy-policy", "privacy", "terms",
    "terms-of-service", "faq", "sitemap", "donate", "login", "register", "request", "request-game", "request-games",
})
_STRIP_BLOCKS_RE = re.compile(r"<(aside|footer|nav|script|style)\b[\s\S]*?</\1\s*>", re.I)
_SIDEBAR_RE = re.compile(
    r"<(?:div|section|aside|ul)\b[^>]*?\b(?:id\s*=\s*[\"'](?:sidebar|secondary)\b"
    r"|class\s*=\s*[\"'](?:[^\"']*\s)?(?:sidebar|widget-area)\b)", re.I)
_ARTICLE_RE = re.compile(r"<article\b[^>]*>([\s\S]*?)</article\s*>", re.I)
_A_TAG_RE = re.compile(r"<a\s([^>]*)>([\s\S]*?)</a\s*>", re.I)
_HEADING_TAG_RE = re.compile(r"<h[1-6]\b[^>]*>[\s\S]*?</h[1-6]\s*>", re.I)
_IMG_TAG_RE = re.compile(r"<img\b([^>]*)>", re.I)
_IMG_EXT_RE = re.compile(r"\.(?:jpe?g|png|webp|avif|gif)(?:[?#]|$)", re.I)
_IMG_SKIP_RE = re.compile(r"emoji|smilies|avatar|gravatar|logo|icon|banner-ad", re.I)
_CAT_LINK_RE = re.compile(r"<a\s([^>]*\brel\s*=\s*[\"'][^\"']*\bcategory\b[^\"']*[\"'][^>]*)>([\s\S]*?)</a\s*>", re.I)
_NOT_A_TITLE_RE = re.compile(
    r"^(?:read more|continue(?: reading)?|more|download|\d+|no comments?|\d+ comments?|leave a comment|"
    r"comments?|home|next|prev(?:ious)?|[›«»…]+)$", re.I)
_UPPER_WORD_RE = re.compile(r"ps[1-5]|psvr2?|vr2?|(?:cusa|ppsa)\d{5}|[ivx]{2,4}|dlc|hd|fpkg", re.I)


@dataclass
class Mirror:
    host: str
    url: str
    indirect: bool = False


@dataclass
class BundleVersion:
    titleId: str
    region: str
    baseVersion: str = ""
    game: list[Mirror] | None = None
    updates: list[dict] = field(default_factory=list)   # {version, compat, mirrors}
    dlc: list[dict] = field(default_factory=list)       # {label, mirrors}
    password: str = ""
    languages: str = ""


def host_of(url: str) -> str:
    try:
        return (urllib.parse.urlsplit(url).hostname or "").lower().removeprefix("www.")
    except ValueError:
        return ""


def decode_gateway(url: str) -> str | None:
    """Real destination hidden in a shortener URL (base64/plain ``url=`` params, justpaste redirects)."""
    try:
        parts = urllib.parse.urlsplit(url)
    except ValueError:
        return None
    if "justpaste.it/redirect/" in url:
        segs = [s for s in parts.path.split("/") if s]
        if len(segs) >= 3:
            dest = urllib.parse.unquote(segs[2])
            if dest.startswith("http"):
                return dest
    q = urllib.parse.parse_qs(parts.query)
    for key in ("url", "u", "link", "id", "s"):
        for raw in q.get(key, []):
            if raw.startswith("http"):
                return raw
            try:
                pad = "=" * ((4 - len(raw) % 4) % 4)
                dec = base64.b64decode(raw.replace("-", "+").replace("_", "/") + pad).decode("utf-8", "ignore")
            except (binascii.Error, ValueError):
                continue
            if dec.startswith("http"):
                return dec
    return None


def extract_mirrors(fragment: str, fallback_url: str) -> list[Mirror]:
    out: list[Mirror] = []
    seen: set[str] = set()
    for m in ANCHOR_RE.finditer(fragment):
        url = m.group(1).strip()
        text = html.unescape(TAG_RE.sub("", m.group(2))).strip()
        if EXCLUDE_URL_RE.search(url) or EXCLUDE_LABEL_RE.search(text):
            continue
        dec = decode_gateway(url)
        if dec:
            url = dec
        if not url.startswith(("http://", "https://")) or url in seen:
            continue
        seen.add(url)
        out.append(Mirror(host=text or host_of(url), url=url))
    if not out and fallback_url:
        text = re.sub(r"\s+", " ", TAG_RE.sub(" ", fragment)).strip()
        colon = text.find(":")
        if colon != -1:
            for cand in re.split(r"\s*[–\-|/,]\s*", text[colon + 1:]):
                cand = cand.strip()
                if 2 <= len(cand) <= 24 and re.match(r"^[A-Za-z0-9]+$", cand) and not NON_HOST_WORD_RE.match(cand):
                    if cand in seen:
                        continue
                    seen.add(cand)
                    out.append(Mirror(host=cand, url=fallback_url, indirect=True))
    return out


def parse_version_block(decoded: str, header: dict | None, fallback_url: str) -> BundleVersion | None:
    decoded = html.unescape(decoded)
    m = INLINE_ID_RE.search(decoded)
    title_id = ((header or {}).get("titleId") or (m.group(1) if m else "") or "").upper()
    region = ((header or {}).get("region") or (m.group(2) if m else "") or "").upper()
    if not title_id:
        return None
    vm = re.search(r"v([0-9]+(?:\.[0-9]+){0,3})", decoded, re.I)
    base_version = (header or {}).get("version") or (vm.group(1) if vm else "")
    bv = BundleVersion(titleId=title_id, region=region, baseVersion=base_version or "")
    for raw in P_BLOCK_RE.findall(decoded):
        text = re.sub(r"\s+", " ", TAG_RE.sub(" ", raw)).strip()
        if not text or re.match(r"^Thank\b", text, re.I):
            continue
        if re.match(r"^Game\s*:", text, re.I):
            mirrors = extract_mirrors(raw, fallback_url)
            if mirrors or bv.game is None:
                bv.game = mirrors
            continue
        um = UPDATE_RE.match(text)
        if um:
            bv.updates.append({"version": um.group(1), "compat": (um.group(3) or "").strip(),
                               "mirrors": extract_mirrors(raw, fallback_url)})
            continue
        if re.match(r"^DLC\b[^:]*:", text, re.I):
            label = text[: text.index(":")].strip()
            if re.search(r"contents\b", label, re.I):
                continue
            bv.dlc.append({"label": label, "mirrors": extract_mirrors(raw, fallback_url)})
            continue
        pm = PASSWORD_RE.match(text)
        if pm and not bv.password:
            bv.password = re.sub(r"\s+\(.*$", "", pm.group(1)).strip()
            continue
        if re.match(r"^Languages?\s*:", text, re.I) and not re.search(r"voice|subtitle", text, re.I):
            if not bv.languages:
                bv.languages = re.sub(r"^Languages?\s*:\s*", "", text, flags=re.I).strip()
    if bv.game is None and not bv.updates and not bv.dlc:
        return None
    return bv


def parse_bundle(content_html: str, page_url: str) -> list[BundleVersion]:
    """All version blocks of a post body (``content.rendered``)."""
    text = html.unescape(content_html or "")
    headers = [{"titleId": re.sub(r"\s+", "", h.group(1)).upper(), "region": h.group(2).upper(),
                "version": h.group(3) or "", "idx": h.start()} for h in HEADER_RE.finditer(text)]
    versions: list[BundleVersion] = []
    for pm in PAYLOAD_RE.finditer(text):
        owner = None
        for h in headers:
            if h["idx"] < pm.start() and (owner is None or h["idx"] > owner["idx"]):
                owner = h
        try:
            decoded = base64.b64decode(re.sub(r"\s+", "", pm.group(1))).decode("utf-8", "replace")
        except (binascii.Error, ValueError):
            continue
        parsed = parse_version_block(decoded, owner, page_url)
        if parsed:
            versions.append(parsed)
    return versions


def _release(kind: str, mirrors: list[Mirror], *, title_id: str, region: str, version: str, compat: str,
             password: str, page_url: str, notes: str = "") -> Release | None:
    urls = [m.url for m in mirrors if not m.indirect]
    indirect_hosts = [m.host for m in mirrors if m.indirect]
    if not urls and not indirect_hosts:
        return None
    url = urls[0] if urls else page_url
    hosts = ", ".join(dict.fromkeys([m.host for m in mirrors if m.host]))
    note_parts = [p for p in (notes, f"Mirrors: {hosts}" if hosts else "") if p]
    if indirect_hosts and not urls:
        note_parts.append("Links are on the game page")
    return Release(
        id=make_release_id(ID_PREFIX, title_id, kind, version, url), url=url, sourceId=SOURCE.id,
        provider=PROVIDER, format=format_from_filename(url) or "PAGE", kind=kind, titleId=title_id,
        region=parse_region(region), version=version, browserUrl=page_url, mirrors=urls[1:],
        compat=" ".join(parse_compat(f"({compat})" if compat else "")), password=password or PASSWORD,
        notes=" · ".join(note_parts),
    )


def releases_from_bundle(versions: list[BundleVersion], page_url: str) -> list[Release]:
    out: list[Release] = []
    for bv in versions:
        pw = bv.password or PASSWORD
        lang = f"Languages: {bv.languages}" if bv.languages else ""
        if bv.game:
            r = _release("game", bv.game, title_id=bv.titleId, region=bv.region, version=bv.baseVersion, compat="",
                         password=pw, page_url=page_url, notes=lang)
            if r:
                out.append(r)
        for u in bv.updates:
            r = _release("update", u["mirrors"], title_id=bv.titleId, region=bv.region, version=u["version"],
                         compat=u["compat"], password=pw, page_url=page_url)
            if r:
                out.append(r)
        for d in bv.dlc:
            r = _release("dlc", d["mirrors"], title_id=bv.titleId, region=bv.region, version="", compat="",
                         password=pw, page_url=page_url, notes=d["label"])
            if r:
                out.append(r)
    return out


def cover_from_post(post: dict) -> str:
    embedded = post.get("_embedded") or {}
    fm = (embedded.get("wp:featuredmedia") or [{}])[0] or {}
    sizes = ((fm.get("media_details") or {}).get("sizes") or {})
    for key in ("medium_large", "medium"):
        u = (sizes.get(key) or {}).get("source_url")
        if u:
            return str(u)
    if fm.get("source_url"):
        return str(fm["source_url"])
    content = (post.get("content") or {}).get("rendered") or ""
    m = IMG_UPLOADS_RE.search(content) or IMG_ANY_RE.search(content)
    return m.group(1).strip() if m else ""


def draft_from_post(post: dict, platform: str) -> GameDraft | None:
    page_url = str(post.get("link") or "")
    title = clean_title(html.unescape(str((post.get("title") or {}).get("rendered") or "")))
    if not page_url or not title:
        return None
    content = (post.get("content") or {}).get("rendered") or ""
    releases = releases_from_bundle(parse_bundle(content, page_url), page_url)
    if not releases:
        releases = [page_release(SOURCE.id, PROVIDER, page_url, platform=platform, password=PASSWORD, prefix=ID_PREFIX)]
    return GameDraft(
        source=SOURCE.id, title=title, platform=platform, releases=releases, cover=cover_from_post(post),
        addedAt=str(post.get("date") or "")[:10], priority=SOURCE.priority, pageUrl=page_url,
    )


SITE = wpsite.SiteConfig(
    id="dlpsgame", name="DLPSGAME", base=BASE, provider=PROVIDER, password=PASSWORD, priority=60, budget=420.0,
    platforms={"PS5": ["ps5"], "PS4": ["ps4"]},
    category_urls={"PS5": [BASE + "/category/ps5/"], "PS4": [BASE + "/category/ps4/"]}, default_platform="PS4",
    rest=False, id_prefix=ID_PREFIX, description="dlpsgame.com WP REST + decoded bundles; sitemap/category fallback",
)


def draft_from_page_html(page_html: str, page_url: str) -> GameDraft | None:
    """A full post page (sitemap / category fallback): the generic parser for title/cover/platform, the
    deep bundle parser for the releases whenever the page still carries ``data-payload`` blocks."""
    d = wpsite.parse_page(SITE, page_html, page_url)
    if d is None:
        return None
    releases = releases_from_bundle(parse_bundle(page_html, page_url), page_url)
    if releases:
        d.releases = releases
    elif not d.releases:
        d.releases = [page_release(SOURCE.id, PROVIDER, page_url, platform=d.platform, password=PASSWORD, prefix=ID_PREFIX)]
    return d


def fetch_fallback(ctx: SourceContext) -> list[GameDraft]:
    """§17.10 strategies 2 and 3 when the REST listing is unavailable."""
    urls = wpsite.sitemap_post_urls(ctx, SITE)
    drafts: dict[str, GameDraft] = {}
    if urls:
        ctx.info(f"sitemap fallback: {len(urls)} post URLs")
        for url in urls[: SITE.max_posts]:
            if ctx.limit and len(drafts) >= ctx.limit:
                break
            try:
                body = ctx.get_text(url, timeout=30, max_bytes=4 * 1024 * 1024)
            except BudgetExceeded:
                break
            except HttpError:
                continue
            d = draft_from_page_html(body, url)
            if d:
                drafts[url] = d
    if not drafts:
        for d in wpsite.fetch_categories(ctx, SITE):
            drafts.setdefault(d.pageUrl, d)
    if not drafts:
        raise HttpError(BASE, 0, "no posts from sitemap or category pages")
    return ctx.truncate(list(drafts.values()))


def fetch_online(ctx: SourceContext) -> list[GameDraft]:
    try:
        return fetch_rest(ctx)
    except HttpError as e:
        ctx.info(f"REST listing failed ({e}); trying sitemap / category pages")
        return fetch_fallback(ctx)


# --------------------------------------------------------------------------- #
# Complete, self-checking online fetch
# --------------------------------------------------------------------------- #

@dataclass
class ListingPost:
    """One post card of a human-visible category listing page."""
    url: str
    title: str = ""
    cover: str = ""


@dataclass
class PlatformRun:
    """What one platform's fetch saw: the category tree, REST, the listing cross-check, and the verdict."""
    platform: str
    category_ids: list[int] = field(default_factory=list)
    category_names: set[str] = field(default_factory=set)   # slugs + lower-cased names of the tree
    tree_ok: bool = False            # every descendant category was listed
    rest_posts: set[str] = field(default_factory=set)       # url keys of every post REST returned
    rest_ok: bool = False            # the tree expanded and every REST page was read
    site_total: int | None = None    # X-WP-Total of the posts query
    listing: str = "skipped"         # skipped | complete | partial | unavailable
    listing_posts: int = 0
    topped_up: int = 0
    final: int = 0

    @property
    def complete(self) -> bool:
        """Every post of the platform is in hand: REST read it all, or the listing was crawled to its end."""
        return self.rest_ok or self.listing == "complete"

    def needs_cross_check(self) -> bool:
        return self.site_total is None or not self.tree_ok or len(self.rest_posts) < self.site_total

    def summary(self) -> str:
        total = "?" if self.site_total is None else str(self.site_total)
        listing = "-" if self.listing == "skipped" else str(self.listing_posts)
        return (f"{self.platform}: REST posts {len(self.rest_posts)}, site total {total}, listing posts {listing} "
                f"({self.listing}), topped up {self.topped_up}, final {self.final}")


def _int_header(headers: dict[str, str], name: str) -> int | None:
    try:
        return int(str(headers.get(name, "")).strip())
    except ValueError:
        return None


def url_key(url: str) -> str:
    """Comparison key for a post URL: host without ``www.`` + decoded, lower-cased path, no trailing slash."""
    try:
        parts = urllib.parse.urlsplit((url or "").strip())
    except ValueError:
        return ""
    host = (parts.hostname or "").lower().removeprefix("www.")
    if not host:
        return ""
    return host + urllib.parse.unquote(parts.path or "/").rstrip("/").lower()


def slug_of(url: str) -> str:
    try:
        segs = [s for s in urllib.parse.urlsplit(url).path.split("/") if s]
    except ValueError:
        return ""
    return segs[-1].lower() if segs else ""


def is_post_url(url: str) -> bool:
    """A dlpsgame permalink (``https://dlpsgame.com/<slug>/``), not a category/tag/page/feed/site page."""
    try:
        parts = urllib.parse.urlsplit(url)
    except ValueError:
        return False
    if parts.scheme not in ("http", "https") or host_of(url) != SITE_HOST or parts.query:
        return False
    segs = [s for s in parts.path.split("/") if s]
    if len(segs) != 1:
        return False
    seg = urllib.parse.unquote(segs[0]).lower()
    return "." not in seg and seg not in _NON_POST_SLUGS and not EXCLUDE_URL_RE.search(seg)


def _abs_url(href: str) -> str:
    href = html.unescape(href or "").strip()
    if not href or href.startswith(("#", "javascript:", "mailto:")):
        return ""
    url = urllib.parse.urljoin(BASE + "/", href)
    try:
        p = urllib.parse.urlsplit(url)
    except ValueError:
        return ""
    return urllib.parse.urlunsplit((p.scheme, p.netloc, p.path, p.query, ""))


def _attr(attrs: str, name: str) -> str:
    m = re.search(r"(?:^|\s)" + re.escape(name) + r"\s*=\s*([\"'])(.*?)\1", attrs or "", re.I | re.S)
    return html.unescape(m.group(2)).strip() if m else ""


def _cover_in(fragment: str) -> str:
    for m in _IMG_TAG_RE.finditer(fragment or ""):
        for name in ("data-src", "data-lazy-src", "data-original", "src"):
            u = _attr(m.group(1), name)
            if u.startswith(("http://", "https://")) and _IMG_EXT_RE.search(u) and not _IMG_SKIP_RE.search(u):
                return u
    return ""


def _entry_from_block(block: str) -> ListingPost | None:
    """The post a listing card links to: heading link > rel=bookmark > any post link; title from the best link."""
    headings = [(m.start(), m.end()) for m in _HEADING_TAG_RE.finditer(block)]
    best: tuple[int, int, str, str] | None = None     # (rank, -position, url, text)
    texts: dict[str, str] = {}
    for m in _A_TAG_RE.finditer(block):
        attrs, inner = m.group(1), m.group(2)
        raw = _attr(attrs, "href")
        url = _abs_url(raw)
        if not is_post_url(url):
            continue
        text = wpsite.text_of(inner) or _attr(attrs, "title")
        if "#" in raw or _NOT_A_TITLE_RE.match(text or "x"):
            text = ""
        if text and len(text) > len(texts.get(url_key(url), "")):
            texts[url_key(url)] = text
        if any(a <= m.start() < b for a, b in headings):
            rank = 3
        elif re.search(r"\brel\s*=\s*[\"'][^\"']*\bbookmark\b", attrs, re.I):
            rank = 2
        else:
            rank = 1 if text else 0
        if best is None or rank > best[0]:
            best = (rank, -m.start(), url, text)
    if best is None:
        return None
    url = best[2]
    title = best[3] or texts.get(url_key(url), "")
    if not title:
        img = _IMG_TAG_RE.search(block)
        title = _attr(img.group(1), "alt") if img else ""
    return ListingPost(url=url, title=title, cover=_cover_in(block))


def _first_entry_pos(doc: str) -> int:
    """Offset of the first post card: an ``<article>``, a ``rel=bookmark`` link or a heading linking a post."""
    cands: list[int] = []
    m = _ARTICLE_RE.search(doc)
    if m:
        cands.append(m.start())
    for m in _A_TAG_RE.finditer(doc):
        if re.search(r"\brel\s*=\s*[\"'][^\"']*\bbookmark\b", m.group(1), re.I):
            cands.append(m.start())
            break
    for m in _HEADING_TAG_RE.finditer(doc):
        if any(is_post_url(_abs_url(_attr(a.group(1), "href"))) for a in _A_TAG_RE.finditer(m.group(0))):
            cands.append(m.start())
            break
    return min(cands) if cands else -1


def parse_listing_posts(page_html: str) -> list[ListingPost]:
    """Post cards of one category listing page, in page order.

    Sidebars, footers and menus (recent / popular posts of every platform) are cut away first; then the
    first strategy that finds posts wins: ``<article>`` cards, headings that link a post, ``rel=bookmark``
    links."""
    doc = _STRIP_BLOCKS_RE.sub(" ", page_html or "")
    first = _first_entry_pos(doc)
    if first >= 0:
        # Only a sidebar that starts after the first post card is cut: a layout wrapper
        # (``<div class="… sidebar-right">``) opens before the cards and must stay.
        side = _SIDEBAR_RE.search(doc, first)
        if side:
            doc = doc[: side.start()]
    entries: list[ListingPost] = []
    for m in _ARTICLE_RE.finditer(doc):
        e = _entry_from_block(m.group(1))
        if e:
            entries.append(e)
    if not entries:
        prev_end = 0
        for m in _HEADING_TAG_RE.finditer(doc):
            e = _entry_from_block(m.group(0))
            if e:
                # A card's thumbnail usually sits right before its title.
                e.cover = e.cover or _cover_in(doc[max(prev_end, m.start() - 2000): m.start()])
                entries.append(e)
            prev_end = m.end()
    if not entries:
        for m in _A_TAG_RE.finditer(doc):
            if re.search(r"\brel\s*=\s*[\"'][^\"']*\bbookmark\b", m.group(1), re.I):
                e = _entry_from_block(m.group(0))
                if e:
                    entries.append(e)
    out: list[ListingPost] = []
    seen: set[str] = set()
    for e in entries:
        k = url_key(e.url)
        if k and k not in seen:
            seen.add(k)
            out.append(e)
    return out


def listing_last_page(page_html: str, listing_url: str) -> int:
    """Highest ``/page/N/`` the pagination of a listing page links to (0 when none)."""
    path = re.escape(urllib.parse.urlsplit(listing_url).path.rstrip("/"))
    nums = [int(n) for n in re.findall(path + r"/page/(\d+)", page_html or "", re.I)]
    return max(nums) if nums else 0


def page_categories(page_html: str) -> set[str]:
    """Lower-cased names and slugs of the ``rel="category tag"`` links of a post page."""
    out: set[str] = set()
    for m in _CAT_LINK_RE.finditer(page_html or ""):
        name = wpsite.text_of(m.group(2)).lower()
        if name:
            out.add(name)
        try:
            segs = [s for s in urllib.parse.urlsplit(_attr(m.group(1), "href")).path.split("/") if s]
        except ValueError:
            segs = []
        if len(segs) >= 2 and segs[0].lower() == "category":
            out.add(urllib.parse.unquote(segs[-1]).lower())
    return out


def title_from_slug(url: str) -> str:
    words = [w for w in re.split(r"[-_]+", urllib.parse.unquote(slug_of(url))) if w]
    return " ".join(w.upper() if _UPPER_WORD_RE.fullmatch(w) else w.capitalize() for w in words)


def own_title_ids(values: list, platform: str) -> list[str]:
    """Valid title ids from ``values`` that can belong to ``platform`` (unique, in order)."""
    out: list[str] = []
    for v in values:
        for tid in parse_title_ids(str(v or "")):
            if platform_for_title_id(tid) in ("", platform) and tid not in out:
                out.append(tid)
    return out


def card_draft(lp: ListingPost, platform: str) -> GameDraft | None:
    """A PAGE draft straight from a listing card (title, cover, URL) when the post itself is out of reach."""
    title = clean_title(lp.title) if lp.title else ""
    if not title:
        title = clean_title(title_from_slug(lp.url))
    if not title:
        return None
    rel = page_release(SOURCE.id, PROVIDER, lp.url, platform=platform, password=PASSWORD, prefix=ID_PREFIX)
    return GameDraft(source=SOURCE.id, title=title, platform=platform, releases=[rel], cover=lp.cover,
                     titleIds=own_title_ids([lp.title], platform), priority=SOURCE.priority, pageUrl=lp.url)


def _category_row(row: object) -> tuple[int, int, list[str]] | None:
    """(id, parent, [slug, name]) of a category row, None when it has no usable id."""
    if not isinstance(row, dict):
        return None
    try:
        cid = int(row["id"])
    except (KeyError, TypeError, ValueError):
        return None
    try:
        parent = int(row.get("parent") or 0)
    except (TypeError, ValueError):
        parent = 0
    labels = [html.unescape(str(v)).strip().lower() for v in (row.get("slug"), row.get("name")) if v]
    return cid, parent, labels


def all_categories(ctx: SourceContext, cache: dict) -> list[dict] | None:
    """Every category of the site (``categories?per_page=100&page=N``), read once per run; None on failure."""
    if "all" in cache:
        return cache["all"]
    rows: list[dict] | None = []
    for page in range(1, MAX_ALL_CATEGORY_PAGES + 1):
        try:
            data, headers = ctx.get_json_headers(f"{CATEGORY_API}?per_page=100&page={page}&_fields=id,slug,name,parent",
                                                 timeout=25)
        except (HttpError, BudgetExceeded) as e:
            ctx.info(f"full category list failed ({e})")
            rows = None
            break
        if not isinstance(data, list):
            rows = None
            break
        rows.extend(r for r in data if isinstance(r, dict))
        pages = _int_header(headers, "x-wp-totalpages")
        if len(data) < 100 or (pages is not None and page >= pages):
            break
    else:
        ctx.info(f"full category list longer than {MAX_ALL_CATEGORY_PAGES} pages")
        rows = None
    cache["all"] = rows
    return rows


def expand_categories(ctx: SourceContext, run: PlatformRun, cache: dict | None = None) -> None:
    """Root category + every descendant (``categories?parent=<id>``, breadth first, bounded depth/requests).

    A REST ``categories=<root>`` query leaves out posts filed only under a child category, which is why the
    old listing stopped short of the site's totals. A tree too large for the request cap is read from the
    site's full category list instead (one request per 100 categories, shared by both platforms). On failure
    the ids found so far (at least the root) are used and ``tree_ok`` stays False."""
    cache = {} if cache is None else cache
    root = CATEGORY_IDS[run.platform]
    ids, names, seen = [root], {run.platform.lower()}, {root}
    frontier, requests, ok, depth, too_big = [root], 0, True, 0, False
    while frontier and ok and not too_big:
        if depth >= MAX_CATEGORY_DEPTH:
            ctx.info(f"{run.platform}: category tree deeper than {MAX_CATEGORY_DEPTH} levels; not expanding further")
            break
        if requests + len(frontier) > MAX_CATEGORY_REQUESTS:
            too_big = True
            break
        depth += 1
        children: list[int] = []
        for parent in frontier:
            page = 1
            while True:
                if requests >= MAX_CATEGORY_REQUESTS:       # a parent with several pages of children
                    too_big = True
                    break
                requests += 1
                url = f"{CATEGORY_API}?parent={parent}&per_page=100&page={page}&_fields=id,slug,name,parent"
                try:
                    rows, headers = ctx.get_json_headers(url, timeout=25)
                except (HttpError, BudgetExceeded) as e:
                    ctx.info(f"{run.platform}: category tree lookup failed ({e}); using "
                             + ("the root category only" if len(ids) == 1 else f"the {len(ids)} categories found so far"))
                    ok = False
                    break
                if not isinstance(rows, list):
                    ok = False
                    break
                for row in rows:
                    parsed = _category_row(row)
                    if parsed is None or parsed[0] in seen:
                        continue
                    seen.add(parsed[0])
                    ids.append(parsed[0])
                    children.append(parsed[0])
                    names.update(parsed[2])
                pages = _int_header(headers, "x-wp-totalpages")
                if len(rows) < 100 or (pages is not None and page >= pages):
                    break
                page += 1
            if not ok or too_big:
                break
        frontier = children
    if too_big:
        full = all_categories(ctx, cache)
        if full is None:
            ctx.info(f"{run.platform}: category tree larger than {MAX_CATEGORY_REQUESTS} lookups; "
                     f"using the {len(ids)} categories found so far")
            ok = False
        else:
            kids: dict[int, list[tuple[int, list[str]]]] = {}
            for row in full:
                parsed = _category_row(row)
                if parsed:
                    kids.setdefault(parsed[1], []).append((parsed[0], parsed[2]))
            ids, names, seen, queue = [root], {run.platform.lower()}, {root}, [root]
            while queue:
                for cid, labels in kids.get(queue.pop(0), []):
                    if cid not in seen:
                        seen.add(cid)
                        ids.append(cid)
                        names.update(labels)
                        queue.append(cid)
            ok = True
            ctx.info(f"{run.platform}: large category tree read from the full list of {len(full)} categories")
    run.category_ids, run.category_names, run.tree_ok = ids, names, ok
    ctx.info(f"{run.platform}: category {root} + {len(ids) - 1} subcategories" + ("" if ok else " (tree incomplete)"))


def fetch_rest_platform(ctx: SourceContext, run: PlatformRun, drafts: dict[str, GameDraft]) -> None:
    """Every post of the platform's category tree via WP REST, paginated by the site's own X-WP-TotalPages."""
    platform = run.platform
    ok = run.tree_ok
    total, total_known = 0, True
    ids = run.category_ids or [CATEGORY_IDS[platform]]
    for start in range(0, len(ids), MAX_IDS_PER_QUERY):
        cats = ",".join(str(i) for i in ids[start:start + MAX_IDS_PER_QUERY])
        page, total_pages, failures = 1, None, 0
        while True:
            if ctx.limit and len(drafts) >= ctx.limit:
                ok = False
                break
            if total_pages is not None and page > total_pages:
                break
            if page > MAX_REST_PAGES:
                ctx.info(f"{platform}: REST stopped at the {MAX_REST_PAGES}-page guard")
                ok = False
                break
            if ctx.time_left() <= 0:
                ctx.info(f"{platform}: budget reached at REST page {page}")
                ok = False
                break
            url = (f"{WP_API}?categories={cats}&per_page={REST_PER_PAGE}&page={page}"
                   f"&_embed=wp:featuredmedia&_fields={REST_FIELDS}")
            try:
                posts, headers = ctx.get_json_headers(url, timeout=40)
            except BudgetExceeded:
                ctx.info(f"{platform}: budget reached at REST page {page}")
                ok = False
                break
            except HttpError as e:
                if e.status == 400 and page > 1:      # rest_post_invalid_page_number: past the last page
                    break
                if page == 1 and not drafts:
                    raise
                ok = False
                failures += 1
                ctx.info(f"{platform}: REST page {page} failed ({e})")
                if page == 1 or total_pages is None or failures >= 3:
                    break
                page += 1
                continue
            if page == 1:
                site_total = _int_header(headers, "x-wp-total")
                total_pages = _int_header(headers, "x-wp-totalpages")
                if site_total is None:
                    total_known = False
                else:
                    total += site_total
            if not isinstance(posts, list):
                if page == 1 and not drafts:
                    raise HttpError(url, 200, "REST posts endpoint did not return a list")
                ok = False
                break
            for p in posts:
                if not isinstance(p, dict):
                    continue
                key = url_key(str(p.get("link") or ""))
                if not key:
                    continue
                run.rest_posts.add(key)
                d = draft_from_post(p, platform)
                if d:
                    drafts.setdefault(key, d)
            if not posts or (total_pages is None and len(posts) < REST_PER_PAGE):
                break
            page += 1
    run.rest_ok = ok
    run.site_total = total if total_known else None


def crawl_listing(ctx: SourceContext, platform: str) -> tuple[list[ListingPost], str]:
    """Post cards of ``/category/<platform>/`` and ``/page/N/`` until a page brings no new post (or 404s).

    Status: ``complete`` (ran to the end), ``partial`` (budget / page cap / failed pages) or ``unavailable``
    (page 1 failed or showed no recognisable post)."""
    base = LISTING_URLS[platform]
    found: dict[str, ListingPost] = {}
    status, last_hint, failures, page = "complete", 0, 0, 0
    while True:
        page += 1
        if page > MAX_LISTING_PAGES[platform]:
            ctx.info(f"{platform}: listing stopped at the {MAX_LISTING_PAGES[platform]}-page cap")
            status = "partial"
            break
        if ctx.time_left() <= LISTING_RESERVE[platform]:
            ctx.info(f"{platform}: budget reached at listing page {page}")
            status = "partial"
            break
        url = base if page == 1 else f"{base}page/{page}/"
        try:
            body = ctx.get_text(url, timeout=30, max_bytes=4 * 1024 * 1024)
        except BudgetExceeded:
            ctx.info(f"{platform}: budget reached at listing page {page}")
            status = "partial"
            break
        except HttpError as e:
            if page == 1:
                ctx.info(f"{platform}: category listing unavailable ({e}); relying on REST")
                status = "unavailable"
                break
            if e.status == 404 and not (last_hint and page <= last_hint):
                break                             # past the last page
            failures += 1
            status = "partial"
            ctx.info(f"{platform}: listing page {page} failed ({e})")
            if failures >= 3:
                break
            continue
        entries = parse_listing_posts(body)
        if page == 1:
            last_hint = listing_last_page(body, base)
            if not entries:
                ctx.info(f"{platform}: no post recognised on {url}; relying on REST")
                status = "unavailable"
                break
        new = 0
        for lp in entries:
            k = url_key(lp.url)
            if k not in found:
                found[k] = lp
                new += 1
        if not new:
            if last_hint and page < last_hint:
                ctx.info(f"{platform}: listing page {page} of {last_hint} brought no new post")
                status = "partial"
            break
    return list(found.values()), status


def top_up(ctx: SourceContext, run: PlatformRun, missing: list[ListingPost], drafts: dict[str, GameDraft]) -> int:
    """Add listing posts REST did not return: REST by slug (batched), then each page (capped), then the card."""
    platform = run.platform
    pending = {url_key(lp.url): lp for lp in missing}
    tree = set(run.category_ids)
    by_rest = by_page = by_card = skipped = 0
    items = list(pending.items())
    for i in range(0, len(items), SLUG_BATCH):
        if ctx.time_left() <= 5:
            break
        q = urllib.parse.urlencode({"slug": ",".join(slug_of(lp.url) for _, lp in items[i:i + SLUG_BATCH]),
                                    "per_page": "100", "_embed": "wp:featuredmedia", "_fields": REST_FIELDS})
        try:
            posts = ctx.get_json(f"{WP_API}?{q}", timeout=40)
        except BudgetExceeded:
            break
        except HttpError as e:
            ctx.info(f"{platform}: REST slug lookup failed ({e})")
            break
        for p in posts if isinstance(posts, list) else []:
            if not isinstance(p, dict):
                continue
            k = url_key(str(p.get("link") or ""))
            if k not in pending:
                continue
            cats = p.get("categories")
            if run.tree_ok and isinstance(cats, list) and cats and not tree.intersection(cats):
                pending.pop(k)            # shown near the listing (a widget), but not filed under this platform
                skipped += 1
                continue
            d = draft_from_post(p, platform)
            if d:
                pending.pop(k)
                drafts.setdefault(k, d)
                by_rest += 1
    fetched = 0
    for k, lp in list(pending.items()):
        if fetched >= TOPUP_PAGE_CAP[platform] or ctx.time_left() <= 5:
            break
        fetched += 1
        try:
            body = ctx.get_text(lp.url, timeout=30, max_bytes=4 * 1024 * 1024)
        except BudgetExceeded:
            break
        except HttpError:
            continue
        cats = page_categories(body)
        if run.tree_ok and cats and not cats & run.category_names:
            pending.pop(k)
            skipped += 1
            continue
        d = draft_from_page_html(body, lp.url)
        if d is None:
            continue
        d.platform = platform
        d.cover = d.cover or lp.cover
        pending.pop(k)
        drafts.setdefault(k, d)
        by_page += 1
    for k, lp in pending.items():
        d = card_draft(lp, platform)
        if d:
            drafts.setdefault(k, d)
            by_card += 1
    ctx.info(f"{platform}: {len(missing)} listing posts missing from REST -> {by_rest} via REST by slug, "
             f"{by_page} from their pages, {by_card} from listing cards"
             + (f", {skipped} not filed under {platform} (skipped)" if skipped else ""))
    return by_rest + by_page + by_card


def cross_check(ctx: SourceContext, run: PlatformRun, drafts: dict[str, GameDraft]) -> None:
    posts, run.listing = crawl_listing(ctx, run.platform)
    run.listing_posts = len(posts)
    missing = [lp for lp in posts if url_key(lp.url) not in drafts]
    if missing:
        run.topped_up = top_up(ctx, run, missing, drafts)


def fetch_rest(ctx: SourceContext) -> list[GameDraft]:
    """PS5 first and always fully (tree → REST → listing cross-check → top-up), then PS4 with what is left.

    A complete run rewrites the snapshot with every post; an incomplete one keeps the snapshot and tops the
    unfinished platform(s) up from it."""
    drafts: dict[str, GameDraft] = {}
    runs: list[PlatformRun] = []
    category_cache: dict = {}
    for platform in ("PS5", "PS4"):
        run = PlatformRun(platform)
        runs.append(run)
        if ctx.limit and len(drafts) >= ctx.limit:
            break
        if ctx.time_left() <= 0:
            ctx.info(f"{platform}: no budget left")
            ctx.info(run.summary())
            continue
        expand_categories(ctx, run, category_cache)
        fetch_rest_platform(ctx, run, drafts)    # raises HttpError only while nothing at all was fetched
        if not ctx.limit and (platform == "PS5" or run.needs_cross_check()):
            cross_check(ctx, run, drafts)
        run.final = sum(1 for d in drafts.values() if d.platform == platform)
        ctx.info(run.summary())
    if not drafts:
        raise HttpError(WP_API, 0, "no posts returned")
    if ctx.limit:
        return ctx.truncate(list(drafts.values()))
    incomplete = [r.platform for r in runs if not r.complete]
    if not incomplete:
        write_snapshot(ctx, list(drafts.values()))
    else:
        added = 0
        try:
            for d in load_snapshot(ctx):
                k = url_key(d.pageUrl)
                if d.platform in incomplete and k not in drafts:
                    drafts[k] = d
                    added += 1
        except (OSError, ValueError):
            pass
        ctx.info(f"incomplete run ({', '.join(incomplete)}): snapshot left as is; {added} posts topped up from it")
    return list(drafts.values())


# --------------------------------------------------------------------------- #
# Snapshot
# --------------------------------------------------------------------------- #

def snapshot_row(d: GameDraft, old: dict | None = None) -> dict:
    """One snapshot row; a value the live draft lacks (date, title ids, cover) is kept from the old row."""
    old = old or {}
    row = {"name": d.title, "platform": d.platform, "page_url": d.pageUrl,
           "cover_url": d.cover or str(old.get("cover_url") or "")}
    date = (d.addedAt or "")[:10]
    if not DATE_RE.match(date):
        date = str(old.get("date") or "")[:10]
    if DATE_RE.match(date):
        row["date"] = date
    old_ids = old.get("title_ids") if isinstance(old.get("title_ids"), list) else []
    ids = own_title_ids(list(d.titleIds) + [r.titleId for r in d.releases], d.platform) or own_title_ids(old_ids, d.platform)
    if ids:
        row["title_ids"] = ids
    return row


def write_snapshot(ctx: SourceContext, drafts: list[GameDraft]) -> bool:
    """Rewrite the snapshot with every post of a COMPLETE online run (sorted by page_url, one compact row per
    line). Refuses to shrink a platform below ``SNAPSHOT_SHRINK_GUARD`` of what the snapshot already holds."""
    path = ctx.snapshot(SNAPSHOT_REL)
    old_rows: list[dict] = []
    try:
        data = read_json_file(path)
        old_rows = [r for r in data if isinstance(r, dict)] if isinstance(data, list) else []
    except (OSError, ValueError):
        pass
    old_by_key = {url_key(str(r.get("page_url") or "")): r for r in old_rows}
    rows: list[dict] = []
    seen: set[str] = set()
    for d in drafts:
        k = url_key(d.pageUrl)
        if d.platform not in ("PS4", "PS5") or not d.title or not k or k in seen:
            continue
        seen.add(k)
        rows.append(snapshot_row(d, old_by_key.get(k)))
    rows.sort(key=lambda r: r["page_url"])
    counts = {p: sum(1 for r in rows if r["platform"] == p) for p in ("PS5", "PS4")}
    for p in ("PS5", "PS4"):
        before = sum(1 for r in old_rows if str(r.get("platform") or "").upper() == p)
        if before and counts[p] < SNAPSHOT_SHRINK_GUARD * before:
            ctx.info(f"snapshot NOT written: {p} would shrink from {before} to {counts[p]} posts")
            return False
    text = "[\n" + ",\n".join(json.dumps(r, ensure_ascii=False, separators=(",", ":")) for r in rows) + "\n]\n"
    try:
        if path.read_text(encoding="utf-8") == text:
            ctx.info(f"snapshot unchanged: {len(rows)} posts ({counts['PS5']} PS5, {counts['PS4']} PS4)")
            return True
    except OSError:
        pass
    tmp = ""
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(prefix=".catalog-", suffix=".json", dir=path.parent)
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        os.replace(tmp, path)
    except OSError as e:
        ctx.info(f"snapshot NOT written: {e}")
        if tmp:
            try:
                os.unlink(tmp)
            except OSError:
                pass
        return False
    ctx.info(f"snapshot written: {len(rows)} posts ({counts['PS5']} PS5, {counts['PS4']} PS4) -> {SNAPSHOT_REL}")
    return True


def load_snapshot(ctx: SourceContext) -> list[GameDraft]:
    """Snapshot rows → PAGE drafts. Rows may carry ``date`` (→ addedAt) and ``title_ids``; old rows without
    them still load."""
    rows = read_json_file(ctx.snapshot(SNAPSHOT_REL))
    if not isinstance(rows, list):
        raise ValueError("dlpsgame snapshot is not a JSON list")
    out: list[GameDraft] = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        page_url = str(row.get("page_url") or "").strip()
        title = clean_title(str(row.get("name") or ""))
        platform = str(row.get("platform") or "").upper()
        if not page_url or not title or platform not in ("PS4", "PS5"):
            continue
        date = str(row.get("date") or "")[:10]
        ids = row.get("title_ids") if isinstance(row.get("title_ids"), list) else []
        rel = page_release(SOURCE.id, PROVIDER, page_url, platform=platform, password=PASSWORD, prefix=ID_PREFIX)
        out.append(GameDraft(source=SOURCE.id, title=title, platform=platform, releases=[rel],
                             titleIds=own_title_ids(ids, platform), addedAt=date if DATE_RE.match(date) else "",
                             cover=str(row.get("cover_url") or ""), priority=SOURCE.priority, pageUrl=page_url))
    return ctx.truncate(out)


SOURCE = SourceSpec(
    id="dlpsgame", name="DLPSGAME", priority=60, budget=420.0, release_source_ids=("dlpsgame",),
    online=fetch_online, snapshot=load_snapshot,
    description="dlpsgame.com WordPress REST listing with decoded download bundles",
)
