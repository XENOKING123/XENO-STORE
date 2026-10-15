#!/usr/bin/env python3
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
tv_golden.py — the Python reference for GET /api/v1/catalog and GET /api/v1/sources
(PORT-SPEC 0.1 R1, R2, R6 and 1.5.2; map-catalog-art §2-§5). The C builder in
src/tvapi_catalog.c must produce exactly what this module produces, key order
included, for the fixture catalogue and for the embedded revision-10 catalogue.

    import tv_golden
    opts = tv_golden.build(catalog_dict, featured_dict)            # the full TV list
    opts = tv_golden.build(cat, feat, enabled=["archive"])         # what /catalog sends then
    src  = tv_golden.sources(cat, feat, enabled={"vikingfile": False}, acknowledged=True)
    st   = tv_golden.stats(cat, feat)                              # counts, bytes, ranks
    tv_golden.load_repo_catalog(), tv_golden.load_repo_featured()  # catalog/xeno-catalog.json, updates/featured.json

Command line:  python3 -I tests/host/tv_golden.py [catalog.json [featured.json]] [--show-zip] [--out FILE]
(prints stats(); defaults to the repository's catalogue and featured feed).

THE RULES (each is what the C side implements; the fixture covers every one)

Sparse encoding. A catalogue may carry a root "defaults" {"game":{...},"release":{...}}:
  members equal to these (empty) values are left out of the file. expand() puts them
  back (tools/catalog/schema.py expand_catalog, web 10-state.js fillDefaults). src/catalog.c
  never reads "defaults": it reads every member with an empty fallback (jv_get_str(..., ""),
  jv_get_i64(..., 0)), which gives the same result only because every default is empty
  ("", 0, false, null, []); expand() raises ValueError on a non-empty default, since the C
  side would silently disagree. A member still missing reads as catalog.c reads it: "" for
  strings, 0 for numbers, [] for lists, except kind -> "game" (catalog.c fill_release) and
  artworkLayout -> "wide" (the schema default; every real game carries it). Values of the
  wrong JSON type read the way src/json.c reads them: a non-string is "" (jv_str), and an
  integer member takes jv_i64's coercions (a float is truncated, a bool is 0/1, a string
  holding a number is parsed, anything else is 0).

Visibility (R1, tier A), all must hold:
  1. game platform is "PS5" or "PS4" (HOMEBREW is never shown);
  2. the release sourceId is an exposed source: catalogue sources[].kind is "direct" or
     "browser", and the id is not "pkgzone" (homebrew only). Revision 10: archive, vikingfile;
  3. format is PKG, FPKG, FFPKG, exFAT, FFPFSC or FFPFS, or ZIP with show_zip
     (TV_SHOW_ZIP / XENO_TV_SHOW_ZIP=1). RAR, 7z and PAGE are always hidden;
  4. sizeBytes > 0 (missing = 0);
  5. the game id matches [A-Za-z0-9._-]{1,95} and is not "." or "..".
  No title ID is required (TV_REQUIRE_TITLE_ID is off).

Option order inside a game (R2), sort keys in this order, the sort is stable:
  1. kind: game 0, backport 1, update 2, dlc 3, any other kind 9;
  2. source kind: direct 0, browser 1;
  3. health: "flaky" 1, anything else ("ok", "" = not audited) 0;
  4. verifiedAt: set 0, empty 1;
  5. catalogue order.
Games keep catalogue order. A game with no visible release is left out.

Ranks (newOnOrbitRank, 1..100): the featured feed's hero[] ids in feed order, skipping ids
  that are not TV-visible (and repeats); then the TV-visible games with catalogue
  featured > 0, in ascending featured order (ties: catalogue order), skipping games already
  ranked; at most 100 ranks. Ranks are computed over every exposed source: the enabled
  filter never renumbers them. Unranked options carry no newOnOrbitRank key.

Fields, in this exact key order (CATALOG_KEYS; [..] = only when present):
  id            release id
  gameId        game id
  titleId       first hit: release titleId (upper-cased); the game's titleIds[0] (upper-cased);
                the first (CUSA|PPSA|PLAS|PCAS|PCJS|PLJS|PLJM|PCJM)[0-9]{5} in the filename,
                then in the percent-decoded URL path; else ""
  platform      game platform lower-cased: "ps5" | "ps4"
  title         game title
  genre         game genres joined with " / " ("" when none)
  tagline, description, publisher     game fields
  releaseDate   game releaseDate when it is exactly [0-9]{4}-[0-9]{2}-[0-9]{2}, else ""
  addedAt       game addedAt + "T00:00:00Z" when it is exactly YYYY-MM-DD, else ""
  [newOnOrbitRank]  int 1..100 (see Ranks)
  artworkLayout game artworkLayout
  provider, sourceId                  release fields
  format        release format; a ZIP (show_zip only) becomes "Unknown"
  [compression]     "zip"           (ZIP only)
  [archiveContents] "unverified"    (ZIP only)
  [extractedBytes]  = sizeBytes     (ZIP only)
  region        release region, else game region, else ""
  version       release version ("" when none)
  filename      release filename, else the percent-decoded last segment of the URL path
  notes         release notes, prefixed for kind update "Update {version}. Install the base
                game first." ("Update. Install the base game first." without a version),
                dlc "Add-on content. Needs the base game.", backport "Backport for older
                firmware."; prefix and notes are joined with one space
  sizeBytes     int
  browserAvailable  source kind == "browser"   (R6: never from browserUrl)
  directAvailable   source kind == "direct"
Never sent: url, browserUrl, mirrors, hashes, password, health, art URLs.

/sources (sources()): options = exposed sources with at least one TV-visible release, in
  catalogue order: {"id","label"=name,"releaseCount"=TV-visible releases whatever the enabled
  state,"downloadReady"=kind == "direct"}; enabled = the enabled option ids in option order
  (a source missing from the settings map counts as enabled, as settings_source_enabled());
  key order: enabled, options, acknowledged, noticeVersion (1).

Ported from the port's catalogue simulation (tvapp/sim/flatten2.py), made deterministic and
completed with the R2 health/verified keys and the sparse-encoding reader.
"""
import collections
import json
import os
import re
import sys
from urllib.parse import unquote, urlsplit

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(HERE))

TV_FORMATS = ("PKG", "FPKG", "FFPKG", "exFAT", "FFPFSC", "FFPFS")
ALWAYS_HIDDEN_FORMATS = ("RAR", "7z", "PAGE")
GAME_PLATFORMS = ("PS5", "PS4")
EXPOSED_KINDS = ("direct", "browser")
HIDDEN_SOURCES = ("pkgzone",)
KIND_ORDER = {"game": 0, "backport": 1, "update": 2, "dlc": 3}
SAFE_GAME_ID = re.compile(r"(?!\.\.?$)[A-Za-z0-9._-]{1,95}")
TITLE_ID_RX = re.compile(r"(CUSA|PPSA|PLAS|PCAS|PCJS|PLJS|PLJM|PCJM)[0-9]{5}")
DATE_RX = re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}")
MAX_RANK = 100
NOTICE_VERSION = 1
NOTE_UPDATE = "Update {v}. Install the base game first."
NOTE_UPDATE_NO_VERSION = "Update. Install the base game first."
NOTE_DLC = "Add-on content. Needs the base game."
NOTE_BACKPORT = "Backport for older firmware."
CATALOG_KEYS = ("id", "gameId", "titleId", "platform", "title", "genre", "tagline", "description", "publisher",
                "releaseDate", "addedAt", "newOnOrbitRank", "artworkLayout", "provider", "sourceId", "format",
                "compression", "archiveContents", "extractedBytes", "region", "version", "filename", "notes",
                "sizeBytes", "browserAvailable", "directAvailable")
SOURCES_KEYS = ("enabled", "options", "acknowledged", "noticeVersion")
OPTION_KEYS = ("id", "label", "releaseCount", "downloadReady")

# Reader fallbacks for members absent from the file and from "defaults" (src/catalog.c).
_GAME_FALLBACK = {"artworkLayout": "wide"}
_RELEASE_FALLBACK = {"kind": "game"}


# --------------------------------------------------------------------------- #
# Loading and the sparse encoding
# --------------------------------------------------------------------------- #

def load_json(path):
    with open(path, "rb") as f:
        return json.loads(f.read().decode("utf-8"))


def load_repo_catalog():
    """The catalogue the host binary embeds (Makefile assets -> tools/gen_assets.py)."""
    return load_json(os.path.join(PROJECT, "catalog", "xeno-catalog.json"))


def load_repo_featured():
    """The featured feed the console caches as <data>/feeds/featured.json."""
    return load_json(os.path.join(PROJECT, "updates", "featured.json"))


def _empty(v):
    return v is None or v is False or v == 0 and not isinstance(v, bool) or v == "" or v == [] or v == {}


def _fill(obj, defaults):
    out = dict(obj)
    for k, v in defaults.items():
        if k not in out:
            out[k] = list(v) if isinstance(v, list) else (dict(v) if isinstance(v, dict) else v)
    return out


def expand(catalog):
    """Returns a copy of the catalogue with every game/release member present (see the
    module docstring). Accepts the compact (with "defaults") and the full form."""
    d = catalog.get("defaults") or {}
    dg, dr = d.get("game") or {}, d.get("release") or {}
    for part, dd in (("game", dg), ("release", dr)):
        bad = sorted(k for k, v in dd.items() if not _empty(v))
        if bad:
            raise ValueError(f"defaults.{part} has non-empty values for {bad}: src/catalog.c ignores "
                             f"\"defaults\" and would read those members as empty")
    out = {k: v for k, v in catalog.items() if k not in ("games", "defaults")}
    games = []
    for g in catalog.get("games") or []:
        eg = _fill(_fill({k: v for k, v in g.items() if k != "releases"}, dg), _GAME_FALLBACK)
        eg["releases"] = [_fill(_fill(r, dr), _RELEASE_FALLBACK) for r in (g.get("releases") or [])]
        games.append(eg)
    out["games"] = games
    return out


def _s(obj, key):
    """String member as catalog.c reads it: jv_get_str(x, key, "") (non-strings read as "")."""
    v = obj.get(key)
    return v if isinstance(v, str) else ""


_C_INT = re.compile(r"\s*([+-]?[0-9]+)")


def _i(obj, key):
    """Integer member as src/json.c jv_i64(v, 0) reads it (json.c:465-475)."""
    v = obj.get(key)
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, int):
        return v
    if isinstance(v, float):
        return int(v) if v == v and abs(v) < 9.2e18 else 0
    if isinstance(v, str):
        m = _C_INT.match(v)
        return int(m.group(1)) if m else 0
    return 0


# --------------------------------------------------------------------------- #
# The rules
# --------------------------------------------------------------------------- #

def source_map(catalog):
    return {s.get("id"): s for s in (catalog.get("sources") or []) if isinstance(s, dict)}


def exposed_sources(catalog):
    """Exposed source ids in catalogue order (rule 2)."""
    return [s["id"] for s in (catalog.get("sources") or [])
            if isinstance(s, dict) and s.get("kind") in EXPOSED_KINDS and s.get("id") not in HIDDEN_SOURCES]


def visible(game, rel, exposed, show_zip=False):
    """Rules 1-5 on an expanded game and release."""
    if _s(game, "platform") not in GAME_PLATFORMS:
        return False
    if _s(rel, "sourceId") not in exposed:
        return False
    fmt = _s(rel, "format")
    if fmt not in TV_FORMATS and not (show_zip and fmt == "ZIP"):
        return False
    if not _i(rel, "sizeBytes") > 0:
        return False
    return bool(SAFE_GAME_ID.fullmatch(_s(game, "id")))


def _url_path(url):
    try:
        return unquote(urlsplit(url).path)
    except ValueError:
        return ""


def title_id(game, rel):
    t = _s(rel, "titleId")
    if t:
        return t.upper()
    tids = game.get("titleIds") or []
    if tids and isinstance(tids[0], str) and tids[0]:
        return tids[0].upper()
    for text in (_s(rel, "filename"), _url_path(_s(rel, "url"))):
        m = TITLE_ID_RX.search(text)
        if m:
            return m.group(0)
    return ""


def release_filename(rel):
    fn = _s(rel, "filename")
    if fn:
        return fn
    return _url_path(_s(rel, "url")).rsplit("/", 1)[-1]


def notes(rel):
    kind, text = _s(rel, "kind") or "game", _s(rel, "notes")
    if kind == "update":
        v = _s(rel, "version")
        pre = NOTE_UPDATE.format(v=v) if v else NOTE_UPDATE_NO_VERSION
    elif kind == "dlc":
        pre = NOTE_DLC
    elif kind == "backport":
        pre = NOTE_BACKPORT
    else:
        return text
    return pre + (" " + text if text else "")


def sort_key(rel, index, srcs):
    kind_rank = KIND_ORDER.get(_s(rel, "kind") or "game", 9)
    src_rank = 0 if srcs.get(_s(rel, "sourceId"), {}).get("kind") == "direct" else 1
    health_rank = 1 if _s(rel, "health") == "flaky" else 0
    verified_rank = 0 if _s(rel, "verifiedAt") else 1
    return (kind_rank, src_rank, health_rank, verified_rank, index)


def tv_game_ids(ecat, show_zip=False):
    exposed = exposed_sources(ecat)
    return {g["id"] for g in ecat["games"] for r in g["releases"] if visible(g, r, exposed, show_zip)}


def ranks(ecat, featured, show_zip=False):
    """{gameId: rank} over every exposed source (enabled or not)."""
    tv = tv_game_ids(ecat, show_zip)
    rank = {}
    hero = (featured or {}).get("hero") or [] if isinstance(featured, dict) else []
    for gid in hero:
        if isinstance(gid, str) and gid in tv and gid not in rank and len(rank) < MAX_RANK:
            rank[gid] = len(rank) + 1
    feat = [(g_i, g) for g_i, g in enumerate(ecat["games"]) if _i(g, "featured") > 0 and g["id"] in tv]
    for _, g in sorted(feat, key=lambda t: (_i(t[1], "featured"), t[0])):
        if g["id"] not in rank and len(rank) < MAX_RANK:
            rank[g["id"]] = len(rank) + 1
    return rank


def _option(g, r, srcs, rank):
    fmt = _s(r, "format")
    zipped = fmt == "ZIP"
    rd = _s(g, "releaseDate")
    ad = _s(g, "addedAt")
    kind = srcs.get(_s(r, "sourceId"), {}).get("kind")
    genres = [x for x in (g.get("genres") or []) if isinstance(x, str)]
    o = {
        "id": _s(r, "id"),
        "gameId": _s(g, "id"),
        "titleId": title_id(g, r),
        "platform": _s(g, "platform").lower(),
        "title": _s(g, "title"),
        "genre": " / ".join(genres),
        "tagline": _s(g, "tagline"),
        "description": _s(g, "description"),
        "publisher": _s(g, "publisher"),
        "releaseDate": rd if DATE_RX.fullmatch(rd) else "",
        "addedAt": ad + "T00:00:00Z" if DATE_RX.fullmatch(ad) else "",
    }
    if g["id"] in rank:
        o["newOnOrbitRank"] = rank[g["id"]]
    o["artworkLayout"] = _s(g, "artworkLayout")
    o["provider"] = _s(r, "provider")
    o["sourceId"] = _s(r, "sourceId")
    o["format"] = "Unknown" if zipped else fmt
    if zipped:
        o["compression"] = "zip"
        o["archiveContents"] = "unverified"
        o["extractedBytes"] = _i(r, "sizeBytes")
    o["region"] = _s(r, "region") or _s(g, "region")
    o["version"] = _s(r, "version")
    o["filename"] = release_filename(r)
    o["notes"] = notes(r)
    o["sizeBytes"] = _i(r, "sizeBytes")
    o["browserAvailable"] = kind == "browser"
    o["directAvailable"] = kind == "direct"
    return o


def _enabled_set(catalog, enabled):
    """`enabled`: None (every source on), a list of enabled ids, or the settings map
    {id: bool} where a missing id counts as enabled."""
    ids = exposed_sources(catalog)
    if enabled is None:
        return set(ids)
    if isinstance(enabled, dict):
        return {i for i in ids if enabled.get(i, True)}
    return set(enabled)


def build(catalog, featured=None, show_zip=False, enabled=None, acknowledged=True):
    """The /api/v1/catalog array. `enabled`/`acknowledged` apply the source filter:
    [] while not acknowledged, otherwise only options of enabled sources."""
    if not acknowledged:
        return []
    ecat = expand(catalog)
    srcs = source_map(ecat)
    exposed = exposed_sources(ecat)
    rank = ranks(ecat, featured, show_zip)
    on = _enabled_set(ecat, enabled)
    out = []
    for g in ecat["games"]:
        rels = [(i, r) for i, r in enumerate(r for r in g["releases"] if visible(g, r, exposed, show_zip))]
        rels.sort(key=lambda t: sort_key(t[1], t[0], srcs))
        for _, r in rels:
            if _s(r, "sourceId") in on:
                out.append(_option(g, r, srcs, rank))
    return out


def release_counts(catalog, show_zip=False):
    ecat = expand(catalog)
    exposed = exposed_sources(ecat)
    c = collections.Counter()
    for g in ecat["games"]:
        for r in g["releases"]:
            if visible(g, r, exposed, show_zip):
                c[_s(r, "sourceId")] += 1
    return c


def sources(catalog, featured=None, enabled=None, acknowledged=False, show_zip=False):
    """The /api/v1/sources object (featured is accepted for symmetry; it does not matter)."""
    ecat = expand(catalog)
    srcs = source_map(ecat)
    counts = release_counts(ecat, show_zip)
    opts = []
    for sid in exposed_sources(ecat):
        if counts[sid] > 0:
            s = srcs[sid]
            opts.append({"id": sid, "label": s.get("name", ""), "releaseCount": counts[sid],
                         "downloadReady": s.get("kind") == "direct"})
    on = _enabled_set(ecat, enabled)
    return {"enabled": [o["id"] for o in opts if o["id"] in on], "options": opts,
            "acknowledged": bool(acknowledged), "noticeVersion": NOTICE_VERSION}


def game_count(catalog, show_zip=False):
    """/catalog/updates gameCount: TV-visible games over every exposed source."""
    return len(tv_game_ids(expand(catalog), show_zip))


def compact_bytes(value):
    return len(json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode("utf-8"))


def find_option(options, release_id):
    for o in options:
        if o["id"] == release_id:
            return o
    return None


def stats(catalog, featured=None, show_zip=False):
    """Counts the port's numbers are quoted in (map-catalog-art §2, §5)."""
    ecat = expand(catalog)
    out = build(ecat, featured, show_zip)
    first = {}
    for o in out:
        first.setdefault(o["gameId"], o)
    counts = release_counts(ecat, show_zip)
    rank = ranks(ecat, featured, show_zip)
    hero = (featured or {}).get("hero") or [] if isinstance(featured, dict) else []
    tv = set(first)
    by_source = collections.Counter(o["sourceId"] for o in out)
    return {
        "revision": ecat.get("revision"),
        "options": len(out),
        "games": len(first),
        "bytes": compact_bytes(out),
        "gamesByPlatform": dict(sorted(collections.Counter(o["platform"] for o in first.values()).items())),
        "optionsByPlatform": dict(sorted(collections.Counter(o["platform"] for o in out).items())),
        "optionsBySource": dict(by_source),
        "bytesBySource": {s: compact_bytes([o for o in out if o["sourceId"] == s]) for s in by_source},
        "releaseCount": dict(counts),
        "formats": dict(collections.Counter(o["format"] for o in out).most_common()),
        "kinds": dict(collections.Counter(
            _s(r, "kind") for g in ecat["games"] for r in g["releases"]
            if visible(g, r, exposed_sources(ecat), show_zip)).most_common()),
        "ranked": len(rank),
        "rank1": next((gid for gid, n in rank.items() if n == 1), ""),
        "heroNotVisible": [h for h in hero if h not in tv],
        "titleIdEmpty": sum(1 for o in out if not o["titleId"]),
        "browserAvailable": sum(1 for o in out if o["browserAvailable"]),
        "multiOptionGames": sum(1 for n in collections.Counter(o["gameId"] for o in out).values() if n > 1),
        "maxGameIdLength": max((len(o["gameId"]) for o in out), default=0),
        "maxReleaseIdLength": max((len(o["id"]) for o in out), default=0),
        "maxSizeBytes": max((o["sizeBytes"] for o in out), default=0),
        "datedGames": sum(1 for o in first.values() if o["releaseDate"]),
        "gameCount": len(first),
    }


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    show_zip = "--show-zip" in argv
    out_path = argv[argv.index("--out") + 1] if "--out" in argv else None
    if out_path in args:
        args.remove(out_path)
    cat = load_json(args[0]) if args else load_repo_catalog()
    feat = load_json(args[1]) if len(args) > 1 else (load_repo_featured() if not args else None)
    print(json.dumps(stats(cat, feat, show_zip), indent=1))
    if out_path:
        with open(out_path, "w", encoding="utf-8") as f:
            json.dump(build(cat, feat, show_zip), f, separators=(",", ":"), ensure_ascii=False)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
