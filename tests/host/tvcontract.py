#!/usr/bin/env python3
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
tvcontract.py — the /api/v1 contract of the native TV app, as code.

The TV app (ported from Orbit Store 1.2.0, GPL-3.0-or-later) reads every reply
of the /api/v1 compatibility layer with a strict JSON parser and typed readers
that never coerce: a bool is not an int, an int is not a string, and one bad
byte fails the whole document. This module states, per route, what the app
reads and what XENO STORE promises on top (PORT-SPEC 1.3-1.5, the maps), and
checks replies against it.

Quick use (in a unittest.TestCase):

    import tvcontract as tc
    body = tc.strict_json(raw_bytes)               # the app's parser rules
    tc.check_reply("GET /api/v1/catalog", body)    # field tables, strict types
    tc.check_reply("POST /api/v1/downloads/j_1a2b3c/pause", body)   # concrete paths work
    tc.assert_error_envelope(409, body)            # {"error":"<sentence>"}, no "ok"
    tc.assert_error_envelope(401, body, sentence=tc.S["unpaired"])

API
  strict_json(raw)                  parse like APPO/json.cpp, plus our limits; raises
                                    StrictJSONError (an AssertionError) with the reason.
  check_reply(route, value, profile="xeno")
                                    raises AssertionError listing every violation with a
                                    precise path ("$[3].sizeBytes: expected int, got bool").
                                    route: "METHOD /api/v1/..." (pattern or concrete path).
  reply_errors(route, value, profile="xeno") -> list[str]   same, without raising.
  assert_error_envelope(status, body, expect_status=None, sentence=None)
  route_key(method, path) -> "METHOD pattern"               (KeyError when unknown)
  SUCCESS_STATUS[route]             the 2xx code PORT-SPEC 1.4 assigns to a success.
  OPEN_ROUTES                       routes that need no Bearer (PORT-SPEC 1.3).
  S                                 the fixed sentences of PORT-SPEC 1.3-1.5.
  AppBody                           request bodies byte for byte as the app builds them
                                    (ported from APPO/api.cpp), e.g. AppBody.save_sources([..], 1).
  quote(s)                          the app's JSON string quoting (APPO/json.cpp:447-486).
  GATES[route]                      PORT-SPEC 1.4 gate letters: S = 503 while stopping, B = Bearer,
                                    O = POST body must be a JSON object ("" = no gate).
  ALL_ROUTES, SAMPLE_PATHS[route]   every method+pattern (zero/companion included) and a concrete
                                    path for each, for gate sweeps.
  sniff_image(bytes), check_art_image(status, headers, body)   the 200 art reply (PORT-SPEC 1.7).

Profiles
  "xeno" (default)  everything: the app's own needs plus PORT-SPEC/map promises
                    (required keys, enums, regexes, ranges, cross-field rules,
                    keys that must never be sent).
  "app"             only what the reference app itself depends on: JSON types
                    (null reads as missing, so it is accepted), the keys whose
                    absence makes the app drop an item or refuse the document,
                    and the value checks the app performs. The reference app's
                    own fixture backend (APP/host/fixture.cpp) passes this profile.

Field tables cite the app's parser and client lines (APPO = app/src/orbit/ of
the reference bundle, also vendored as app/tv/src/orbit/). The app never reads
unknown keys, so extra keys pass unless a table is "closed" or lists them in
"forbid".
"""
import json
import math
import re

__all__ = [
    "StrictJSONError", "strict_json", "check_reply", "reply_errors", "assert_error_envelope",
    "route_key", "ROUTES", "SUCCESS_STATUS", "OPEN_ROUTES", "S", "AppBody", "quote",
    "MAX_DEPTH", "MAX_SAFE_INT", "MiB",
    "GATES", "SAMPLE_PATHS", "ALL_ROUTES", "sniff_image", "check_art_image", "ART_MIME",
]

MiB = 1024 * 1024
MAX_DEPTH = 64              # APPO/json.hpp:29 kMaxDepth, checked at APPO/json.cpp:166-169
MAX_SAFE_INT = 2 ** 53      # contract-catalog-art §0.4: numbers are doubles, byte counts <= 2^53
RESERVE_GAP = 240 * MiB     # PORT-SPEC 1.5.3: projectedFreeBytes = free - pending - 240 MiB


# --------------------------------------------------------------------------- #
# Strict JSON (the app's parser, APPO/json.cpp:112-445)
# --------------------------------------------------------------------------- #

class StrictJSONError(AssertionError):
    """The bytes would fail the app's whole-document parse (or our stricter limits)."""


_WS = " \t\n\r"   # APPO/json.cpp:147-156 skip_space


def _skip_ws(text, i):
    n = len(text)
    while i < n and text[i] in _WS:
        i += 1
    return i


def _no_constant(name):
    raise StrictJSONError(f"not JSON: the literal {name} (NaN/Infinity are not numbers in JSON)")


def _int_hook(s):
    v = int(s)
    if abs(v) > MAX_SAFE_INT:
        raise StrictJSONError(f"number {s} is above 2^53: the app reads numbers as doubles")
    return v


def _float_hook(s):
    v = float(s)
    if not math.isfinite(v) or abs(v) > MAX_SAFE_INT:
        raise StrictJSONError(f"number {s} is not finite or is above 2^53")
    return v


def _pairs_hook(allow_duplicates):
    def hook(pairs):
        out = {}
        for k, v in pairs:
            if k in out and not allow_duplicates:
                # the app keeps the FIRST occurrence (APPO/json.cpp:83-93), Python the last:
                # a duplicate would make the tests read a different value than the console.
                raise StrictJSONError(f"duplicate key {k!r} in one object")
            out[k] = v
        return out
    return hook


def strict_json(raw, allow_duplicates=False):
    """Parses `raw` (bytes or str) under the app's rules and returns the value.

    Rejects (StrictJSONError): bytes that are not UTF-8, a BOM, a raw control
    byte below 0x20 inside a string, a lone \\uD800-\\uDFFF escape, NaN/Infinity,
    trailing data after the document, any value nested deeper than 64 (the app's
    depth rule: the top-level value is depth 0, `depth > 64` fails), numbers whose
    magnitude is above 2^53 and duplicate keys (the app and Python disagree on
    which one wins). Empty input is rejected too.
    """
    if isinstance(raw, (bytes, bytearray, memoryview)):
        try:
            text = bytes(raw).decode("utf-8")
        except UnicodeDecodeError as e:
            raise StrictJSONError(f"not UTF-8 at byte {e.start}: {e.reason}") from None
    elif isinstance(raw, str):
        text = raw
    else:
        raise TypeError(f"strict_json wants bytes or str, got {type(raw).__name__}")
    if text.startswith("\ufeff"):
        raise StrictJSONError("byte order mark before the document")
    start = _skip_ws(text, 0)
    if start >= len(text):
        raise StrictJSONError("empty document")
    dec = json.JSONDecoder(parse_int=_int_hook, parse_float=_float_hook, parse_constant=_no_constant,
                           object_pairs_hook=_pairs_hook(allow_duplicates), strict=True)
    try:
        value, end = dec.raw_decode(text, start)
    except StrictJSONError:
        raise
    except RecursionError:
        raise StrictJSONError(f"nesting deeper than {MAX_DEPTH}") from None
    except json.JSONDecodeError as e:
        # strict=True gives "Invalid control character at ..." for raw bytes < 0x20 in strings
        raise StrictJSONError(f"not strict JSON: {e.msg} at char {e.pos}") from None
    end = _skip_ws(text, end)
    if end != len(text):
        raise StrictJSONError(f"unexpected data after the document at char {end}")
    _walk_strict(value)
    return value


def _walk_strict(value):
    """Depth (APPO/json.cpp:166-169) and lone surrogates (APPO/json.cpp:346-360).
    Python joins a valid \\uD83D\\uDE00 pair into one code point, so any code point
    left in U+D800..U+DFFF came from a lone escape."""
    stack = [(value, 0, "$")]
    while stack:
        v, depth, path = stack.pop()
        if depth > MAX_DEPTH:
            raise StrictJSONError(f"{path}: nesting deeper than {MAX_DEPTH}")
        if isinstance(v, str):
            _no_surrogate(v, path)
        elif isinstance(v, list):
            for i, it in enumerate(v):
                stack.append((it, depth + 1, f"{path}[{i}]"))
        elif isinstance(v, dict):
            for k, it in v.items():
                _no_surrogate(k, f"{path} (key)")
                stack.append((it, depth + 1, f"{path}.{k}"))


_SURR = re.compile("[\ud800-\udfff]")


def _no_surrogate(s, path):
    m = _SURR.search(s)
    if m:
        raise StrictJSONError(f"{path}: lone surrogate \\u{ord(m.group(0)):04X}")


# --------------------------------------------------------------------------- #
# Field-table machinery
# --------------------------------------------------------------------------- #

STR, BOOL, INT, NUM, NULL, ARR, OBJ = "str", "bool", "int", "num", "null", "arr", "obj"


class F:
    """One member. `t` is a type token or a tuple of them.

    xeno-profile checks: req, enum, regex (fullmatch), lo/hi (inclusive), minlen/maxlen.
    `app` names the checks the app itself performs, so they also run in the "app"
    profile: any of "req", "enum", "regex", "range", "len", "int" (an integral
    number, as `newOnOrbitRank`), plus `app_range=(lo, hi)` when the app's range
    differs from ours. `items` is the spec of array items, `obj` of a nested object.
    """
    __slots__ = ("t", "req", "enum", "regex", "lo", "hi", "minlen", "maxlen", "items", "obj",
                 "app", "app_range")

    def __init__(self, t, req=True, enum=None, regex=None, lo=None, hi=None, minlen=None, maxlen=None,
                 items=None, obj=None, app=(), app_range=None):
        self.t = t if isinstance(t, tuple) else (t,)
        self.req = req
        self.enum = enum
        self.regex = re.compile(regex) if isinstance(regex, str) else regex
        self.lo, self.hi = lo, hi
        self.minlen, self.maxlen = minlen, maxlen
        self.items, self.obj = items, obj
        self.app = frozenset(app)
        self.app_range = app_range


class Obj:
    """An object. `closed`: in the xeno profile no other key may appear.
    `forbid`: keys that must never be sent (xeno). `post(value, path, err, profile)`
    adds cross-field rules."""

    def __init__(self, fields, closed=False, forbid=(), post=None, src=""):
        self.fields, self.closed, self.forbid, self.post, self.src = fields, closed, tuple(forbid), post, src


class Arr:
    """A top-level array (the app refuses anything else for these routes)."""

    def __init__(self, items, post=None, src=""):
        self.items, self.post, self.src = items, post, src


def _tname(v):
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "bool"
    if isinstance(v, int):
        return "int"
    if isinstance(v, float):
        return "float"
    if isinstance(v, str):
        return "str"
    if isinstance(v, list):
        return "array"
    if isinstance(v, dict):
        return "object"
    return type(v).__name__


def _is(v, t, profile, intcheck):
    if t == STR:
        return isinstance(v, str)
    if t == BOOL:
        return isinstance(v, bool)
    if t == INT:
        if isinstance(v, bool):
            return False
        if isinstance(v, int):
            return True
        # the app reads numbers as doubles; only the xeno profile insists on integers
        return profile == "app" and isinstance(v, float) and (not intcheck or v.is_integer())
    if t == NUM:
        return isinstance(v, (int, float)) and not isinstance(v, bool)
    if t == NULL:
        return v is None
    if t == ARR:
        return isinstance(v, list)
    if t == OBJ:
        return isinstance(v, dict)
    raise ValueError(t)


def _short(v):
    try:
        s = json.dumps(v, ensure_ascii=False)
    except (TypeError, ValueError):
        s = repr(v)
    return s if len(s) <= 60 else s[:57] + "..."


def _check_field(f, v, path, err, profile):
    app = profile == "app"
    if app and v is None and "req" not in f.app:
        return                                    # the app reads null as "missing"
    if not any(_is(v, t, profile, "int" in f.app) for t in f.t):
        want = "|".join(f.t)
        err.append(f"{path}: expected {want}, got {_tname(v)} ({_short(v)})")
        return
    if v is None:
        return

    def on(name):
        return not app or name in f.app

    if f.enum is not None and on("enum") and v not in f.enum:
        err.append(f"{path}: {_short(v)} is not one of {list(f.enum)}")
    if f.regex is not None and on("regex") and isinstance(v, str) and not f.regex.fullmatch(v):
        err.append(f"{path}: {_short(v)} does not match /{f.regex.pattern}/")
    if isinstance(v, (int, float)) and not isinstance(v, bool):
        lo, hi = (f.app_range if (app and f.app_range) else (f.lo, f.hi))
        if (app and (f.app_range or "range" in f.app)) or not app:
            if lo is not None and v < lo:
                err.append(f"{path}: {v} is below {lo}")
            if hi is not None and v > hi:
                err.append(f"{path}: {v} is above {hi}")
    if isinstance(v, (str, list)) and on("len"):
        if f.minlen is not None and len(v) < f.minlen:
            err.append(f"{path}: length {len(v)} is below {f.minlen}")
        if f.maxlen is not None and len(v) > f.maxlen:
            err.append(f"{path}: length {len(v)} is above {f.maxlen}")
    if isinstance(v, list) and f.items is not None:
        for i, it in enumerate(v):
            _check_any(f.items, it, f"{path}[{i}]", err, profile)
    if isinstance(v, dict) and f.obj is not None:
        _check_obj(f.obj, v, path, err, profile)


def _check_obj(spec, v, path, err, profile):
    if not isinstance(v, dict):
        err.append(f"{path}: expected object, got {_tname(v)} ({_short(v)})")
        return
    app = profile == "app"
    for name, f in spec.fields.items():
        p = f"{path}.{name}"
        if name not in v:
            if (f.req and not app) or "req" in f.app:
                err.append(f"{p}: missing")
            continue
        _check_field(f, v[name], p, err, profile)
    if not app:
        for k in v:
            if k in spec.forbid:
                err.append(f"{path}.{k}: must never be sent")
            elif spec.closed and k not in spec.fields:
                err.append(f"{path}.{k}: unexpected key (this object is closed)")
    if spec.post:
        spec.post(v, path, err, profile)


def _check_any(spec, v, path, err, profile):
    if isinstance(spec, Obj):
        _check_obj(spec, v, path, err, profile)
    elif isinstance(spec, Arr):
        if not isinstance(v, list):
            err.append(f"{path}: expected array, got {_tname(v)} ({_short(v)})")
            return
        for i, it in enumerate(v):
            _check_any(spec.items, it, f"{path}[{i}]", err, profile)
        if spec.post:
            spec.post(v, path, err, profile)
    elif isinstance(spec, F):
        _check_field(spec, v, path, err, profile)
    else:
        raise TypeError(spec)


# --------------------------------------------------------------------------- #
# Shared value rules
# --------------------------------------------------------------------------- #

RX_VERSION = r"[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}"          # plain N.N.N (PORT-SPEC 1.5.1, 1.5.5)
RX_VERSION_OPT = r"(?:" + RX_VERSION + r")?"
RX_HEX32 = r"[0-9a-f]{32}"
RX_HEX64 = r"[0-9a-f]{64}"
RX_HEX64_OPT = r"(?:[0-9a-f]{64})?"
RX_TITLE_ID = r"[A-Z]{4}[0-9]{5}"
RX_TITLE_ID_OPT = r"(?:[A-Z]{4}[0-9]{5})?"
RX_SAFE_SEGMENT = r"(?!\.\.?$)[A-Za-z0-9._-]{1,128}"         # APPO/api.cpp:18-30 safe_path_segment
RX_GAME_ID = r"(?!\.\.?$)[A-Za-z0-9._-]{1,95}"               # R1: safe_path_segment + our router's 95
RX_DRIVE_ID = r"[A-Za-z0-9_-]{1,63}"                         # APPO/browser_handoff.hpp:17-29
RX_DRIVE_ID_OPT = r"(?:[A-Za-z0-9_-]{1,63})?"
RX_JOB_ID = r"(?!zero-)(?!\.\.?$)[A-Za-z0-9._-]{1,128}"      # APPO/api.cpp:376-389: "zero-" goes to Orbit Zero
RX_DATE = r"[0-9]{4}-[0-9]{2}-[0-9]{2}"
RX_IPV4_OR_EMPTY = r"(?:(?!127\.)(?:[0-9]{1,3}\.){3}[0-9]{1,3})?"   # never 127.x (PORT-SPEC 1.3)
U53 = dict(lo=0, hi=MAX_SAFE_INT)

TV_FORMATS = ("PKG", "FPKG", "FFPKG", "exFAT", "FFPFSC", "FFPFS")   # R1 tier A
IMAGE_FORMATS = ("exFAT", "FFPFSC", "FFPFS", "FFPKG")
JOB_STATUSES = ("queued", "downloading", "verifying", "extracting", "retrying",
                "paused", "complete", "cancelled", "error")         # map-downloads §3.3 (never "inspecting")
APP_JOB_STATUSES = JOB_STATUSES + ("inspecting",)                   # APPO/model.cpp:97-114, 788-821
BROWSER_STATES = ("idle", "opening", "waiting", "waiting-link", "validating", "queued", "failed", "cancelled")
LIBRARY_CAPS = ("list_games", "rescan", "mount_game", "unmount_game", "copy_game_source",
                "move_game_source", "storage_job_status", "storage_job_cancel", "storage_space")
LIBRARY_ACTIONS = ("scan", "mount", "unmount", "copy", "move", "cancel", "measure")


def _err_if(cond, err, msg):
    if cond:
        err.append(msg)


# --------------------------------------------------------------------------- #
# GET /api/v1/system — APPO/model.cpp:131-148 parse_system; APPO/api.cpp:64-88 (object
# required, "unreadable status"); contract-system §2; PORT-SPEC 1.5.1, R7, R11, R12.
# --------------------------------------------------------------------------- #
SYSTEM = Obj({
    "name": F(STR, enum=("XENO STORE",)),
    "version": F(STR, regex=RX_VERSION),              # exact-equality restart check, APPO/store.cpp:644
    "instanceId": F(STR, regex=RX_HEX32),             # non-empty and new per process, APPO/store.cpp:631-651
    "stopping": F(BOOL),
    "platform": F(STR, enum=("ps5", "host")),         # R12; "desktop" would disable downloads
    "paired": F(BOOL),
    "localSessionAvailable": F(BOOL),
    "stateHealthy": F(BOOL),                          # app default true, APPO/model.cpp:141
    "catalogueRevision": F(INT, **U53),               # (revision << 20) | hero-hash, < 2^53
    "preferredStorage": F(STR, regex=RX_DRIVE_ID_OPT),
    "deleteArchives": F(BOOL),
    "concurrentDownloads": F(INT, lo=1, hi=4),        # R7 (the app accepts 1..8, APPO/model.cpp:145-146)
}, src="APPO/model.cpp:131-148")

# --------------------------------------------------------------------------- #
# GET /api/v1/session — APPO/api.cpp:200-212 (token must be exactly 64 chars, else
# "did not open a console session"); APPO/model.cpp:432-439; PORT-SPEC 1.3.
# --------------------------------------------------------------------------- #
SESSION = Obj({
    "token": F(STR, regex=RX_HEX64, minlen=64, maxlen=64, app=("req", "len")),
    "pairCode": F(STR, regex=r"[0-9]{6}"),
    "address": F(STR, regex=RX_IPV4_OR_EMPTY),
}, src="APPO/api.cpp:200-212, APPO/model.cpp:432-439")


# --------------------------------------------------------------------------- #
# GET|POST /api/v1/sources — APPO/model.cpp:150-170 parse_sources; APPO/api.cpp:125-137,
# 461-483 (object required); contract-catalog-art §1-2; PORT-SPEC 1.5.2.
# --------------------------------------------------------------------------- #
def _sources_post(v, path, err, profile):
    if profile == "app":
        return
    opts = [o.get("id") for o in v.get("options", []) if isinstance(o, dict)]
    _err_if(len(set(opts)) != len(opts), err, f"{path}.options: duplicate ids {opts}")
    en = v.get("enabled", [])
    if isinstance(en, list):
        _err_if(len(set(map(str, en))) != len(en), err, f"{path}.enabled: duplicate ids {en}")
        for i, e in enumerate(en):
            _err_if(e not in opts, err, f"{path}.enabled[{i}]: {e!r} is not an option id {opts}")
        order = [o for o in opts if o in en]
        _err_if(order != list(en), err, f"{path}.enabled: not in option order ({en} vs {order})")
    for i, o in enumerate(v.get("options", [])):
        if isinstance(o, dict) and isinstance(o.get("releaseCount"), int):
            _err_if(o["releaseCount"] < 1, err, f"{path}.options[{i}]: a source without TV releases is listed")


SOURCE_OPTION = Obj({
    "id": F(STR, regex=r"[A-Za-z0-9._-]{1,31}"),
    "label": F(STR, minlen=1),
    "releaseCount": F(INT, **U53),
    "downloadReady": F(BOOL),
}, src="APPO/model.cpp:160-168")
SOURCES = Obj({
    "enabled": F(ARR, items=F(STR)),
    "options": F(ARR, items=SOURCE_OPTION),
    "acknowledged": F(BOOL),
    "noticeVersion": F(INT, enum=(1,)),
}, post=_sources_post, src="APPO/model.cpp:150-170")


# --------------------------------------------------------------------------- #
# GET /api/v1/catalog — APPO/model.cpp:202-244 parse_catalog (items with an empty id or
# gameId are skipped; rank 1..100 integral; directAvailable defaults to true);
# APPO/api.cpp:153-166 (array required, 16 MiB cap); contract-catalog-art §3;
# map-catalog-art §5; PORT-SPEC 1.5.2, R1, R6. The key set is closed: nothing else
# (no url, browserUrl, hashes, password, health or art URLs) may leave the server.
# --------------------------------------------------------------------------- #
GAME_LEVEL_KEYS = ("title", "platform", "genre", "tagline", "description", "publisher", "releaseDate",
                   "addedAt", "newOnOrbitRank", "popularRank", "cheats", "screenshotCount", "artworkLayout")
WIRE_KINDS = ("game", "backport", "update", "dlc")
KIND_NOTES = {"update": "Update", "dlc": "Add-on content. Needs the base game.",
              "backport": "Backport for older firmware."}
MAX_SHOTS = 10   # src/tvapi.h TV_ART_MAX_SHOTS


def _catalog_item_post(v, path, err, profile):
    if profile == "app":
        return
    ba, da = v.get("browserAvailable"), v.get("directAvailable")
    if isinstance(ba, bool) and isinstance(da, bool):
        _err_if(ba == da, err, f"{path}: browserAvailable and directAvailable must differ (R6), both {ba}")
    zipped = v.get("format") == "Unknown"
    for k in ("compression", "archiveContents", "extractedBytes"):
        _err_if(zipped != (k in v), err, f"{path}.{k}: present only on a TV_SHOW_ZIP option (format \"Unknown\")")
    if zipped and isinstance(v.get("extractedBytes"), int):
        _err_if(v.get("extractedBytes") != v.get("sizeBytes"), err, f"{path}.extractedBytes: must equal sizeBytes")
    tid, plat = v.get("titleId"), v.get("platform")
    if isinstance(tid, str) and tid and plat in ("ps4", "ps5"):
        _err_if(tid.startswith("CUSA") and plat != "ps4", err, f"{path}: titleId {tid} on a {plat} option")
    prefix, note = KIND_NOTES.get(v.get("kind")), v.get("notes")
    if prefix and isinstance(note, str):
        _err_if(not note.startswith(prefix), err, f"{path}.notes: a {v.get('kind')} option's notes start with {prefix!r}")


def _catalog_post(v, path, err, profile):
    if profile == "app":
        return
    seen_ids, first, closed_games, last_gid, popular = set(), {}, set(), None, {}
    for i, o in enumerate(v):
        if not isinstance(o, dict):
            continue
        rid, gid = o.get("id"), o.get("gameId")
        _err_if(rid in seen_ids, err, f"{path}[{i}].id: duplicate release id {rid!r}")
        seen_ids.add(rid)
        if gid != last_gid:
            _err_if(gid in closed_games, err, f"{path}[{i}]: options of {gid!r} are not contiguous")
            if last_gid is not None:
                closed_games.add(last_gid)
            last_gid = gid
        if gid not in first:
            first[gid] = (i, o)
            pr = o.get("popularRank")
            if isinstance(pr, int):
                _err_if(pr in popular, err, f"{path}[{i}].popularRank: {pr} is also {popular.get(pr)!r}'s")
                popular.setdefault(pr, gid)
            continue
        j, f0 = first[gid]
        for k in GAME_LEVEL_KEYS:
            _err_if(o.get(k) != f0.get(k), err,
                    f"{path}[{i}].{k}: differs from the first option of {gid!r} at [{j}] ({_short(o.get(k))} vs {_short(f0.get(k))})")


CATALOG_ITEM = Obj({
    "id": F(STR, regex=RX_SAFE_SEGMENT, minlen=1, app=("req", "len")),
    "gameId": F(STR, regex=RX_GAME_ID, minlen=1, app=("req", "len")),
    "titleId": F(STR, regex=RX_TITLE_ID_OPT),
    "platform": F(STR, enum=("ps5", "ps4")),           # lowercase: PS4 badge, filters, Library match
    "title": F(STR),
    "genre": F(STR),
    "tagline": F(STR),
    "description": F(STR),
    "publisher": F(STR),
    "releaseDate": F(STR, regex=r"(?:" + RX_DATE + r")?"),
    "addedAt": F(STR, regex=r"(?:" + RX_DATE + r"T00:00:00Z)?"),   # a date-only value parses as 0 (APPO/model.cpp:66-95)
    "newOnOrbitRank": F(INT, req=False, lo=1, hi=100, app=("int", "range")),
    "popularRank": F(INT, lo=1, hi=MAX_SAFE_INT),      # 1 = the most popular TV game; one per game
    "cheats": F(INT, req=False, lo=1, hi=MAX_SAFE_INT),
    "screenshotCount": F(INT, req=False, lo=1, hi=MAX_SHOTS),   # GET /api/v1/art/{id}/shot/{0..n-1}
    "artworkLayout": F(STR),
    "provider": F(STR),
    "sourceId": F(STR, minlen=1),
    "format": F(STR, enum=TV_FORMATS + ("Unknown",)),
    "compression": F(STR, req=False, enum=("zip",)),
    "archiveContents": F(STR, req=False, enum=("unverified",)),
    "extractedBytes": F(INT, req=False, lo=1, hi=MAX_SAFE_INT),
    "region": F(STR),
    "version": F(STR),
    "filename": F(STR, minlen=1),
    "kind": F(STR, enum=WIRE_KINDS),                   # what the option is, to group a game's options by
    "notes": F(STR),
    "sizeBytes": F(INT, lo=1, hi=MAX_SAFE_INT),        # R1: sizeBytes > 0
    "browserAvailable": F(BOOL),
    "directAvailable": F(BOOL),
}, closed=True, forbid=("url", "browserUrl", "mirrors", "sha256", "md5", "sha1", "password", "health",
                        "cover", "coverFallback", "hero", "heroFallback", "screenshots", "tint"),
   post=_catalog_item_post, src="APPO/model.cpp:202-244")
CATALOG = Arr(CATALOG_ITEM, post=_catalog_post, src="APPO/api.cpp:153-166")


# --------------------------------------------------------------------------- #
# GET|POST /api/v1/catalog/updates — APPO/model.cpp:441-453; APPO/api.cpp:528-553;
# contract-catalog-art §4-5; map-catalog-art §6; PORT-SPEC 1.5.2.
# --------------------------------------------------------------------------- #
CATALOGUE_STATUS = Obj({
    "available": F(BOOL),
    "busy": F(BOOL),
    "cached": F(BOOL),
    "revision": F(INT, **U53),
    "gameCount": F(INT, **U53),
    "checkedAt": F(INT, **U53),          # Unix seconds, 0 = never
    "checkAfter": F(INT, **U53),         # Unix seconds
    "error": F(STR),
}, src="APPO/model.cpp:441-453")


# --------------------------------------------------------------------------- #
# GET|POST /api/v1/favorites — APPO/model.cpp:309-318 (non-empty strings, first
# occurrence kept); APPO/api.cpp:228-255 (bare array); PORT-SPEC 1.5.2.
# --------------------------------------------------------------------------- #
def _favorites_post(v, path, err, profile):
    if profile != "app":
        _err_if(len(set(map(repr, v))) != len(v), err, f"{path}: duplicate ids")


FAVORITES = Arr(F(STR, regex=r"[^\x00-\x1f]{1,95}"), post=_favorites_post, src="APPO/model.cpp:309-319")


# --------------------------------------------------------------------------- #
# GET /api/v1/downloads, POST /api/v1/downloads/{id}/{action} — APPO/model.cpp:267-307
# parse_jobs (items without an id are dropped; sorted by order); APPO/api.cpp:323-349,
# 372-404; contract-downloads §3, §5; map-downloads §3.2-3.3; PORT-SPEC 1.5.3.
# --------------------------------------------------------------------------- #
def _job_post(v, path, err, profile):
    if profile == "app":
        return
    st = v.get("status")
    num = {k: v.get(k) for k in ("received", "total", "speed", "retryAt", "extracted", "extractTotal")}
    if not all(isinstance(x, int) and not isinstance(x, bool) for x in num.values()):
        return
    _err_if(num["total"] > 0 and num["received"] > num["total"], err,
            f"{path}.received: {num['received']} is above total {num['total']}")
    _err_if(st == "retrying" and v.get("error") != "", err, f"{path}.error: must be \"\" while retrying (it hides the countdown)")
    _err_if(st != "retrying" and num["retryAt"] != 0, err, f"{path}.retryAt: only a retrying job has one")
    _err_if(st != "extracting" and (num["extracted"] or num["extractTotal"]), err,
            f"{path}.extracted: only an extracting job reports extraction progress")
    _err_if(st != "downloading" and num["speed"] != 0, err, f"{path}.speed: only a downloading job has a speed")
    _err_if(v.get("delivery") != "torbox" and v.get("deliveryPhase") != "", err,
            f"{path}.deliveryPhase: only for TorBox jobs")
    _err_if(st == "complete" and num["total"] > 0 and num["received"] != num["total"], err,
            f"{path}.received: a complete job reports received == total")


def _jobs_post(v, path, err, profile):
    if profile == "app":
        return
    ids = [j.get("id") for j in v if isinstance(j, dict)]
    _err_if(len(set(ids)) != len(ids), err, f"{path}: duplicate job ids")
    orders = [j.get("order") for j in v if isinstance(j, dict)]
    _err_if(orders != sorted(orders) or len(set(orders)) != len(orders), err,
            f"{path}: order must be strictly increasing in reply order ({orders})")


JOB = Obj({
    "id": F(STR, regex=RX_JOB_ID, minlen=1, app=("req", "len", "regex")),
    "releaseId": F(STR),
    "gameId": F(STR),
    "titleId": F(STR, regex=RX_TITLE_ID_OPT),
    "title": F(STR),
    "storageId": F(STR, regex=RX_DRIVE_ID_OPT),
    "filename": F(STR),
    "path": F(STR),
    "format": F(STR),
    "status": F(STR, enum=JOB_STATUSES),
    "error": F(STR),
    "verification": F(STR, enum=("sha256", "size")),
    "delivery": F(STR, enum=("direct", "torbox")),     # never "zero"
    "deliveryPhase": F(STR, enum=("", "Preparing with TorBox", "Downloading via TorBox")),
    "canRefreshLink": F(BOOL, enum=(False,)),           # XENO has no link refresh
    "canEnterArchivePassword": F(BOOL),
    "received": F(INT, **U53),
    "total": F(INT, **U53),                             # == the catalogue sizeBytes (APPO/model.cpp:823-836)
    "speed": F(INT, **U53),
    "retryAt": F(INT, **U53),
    "extracted": F(INT, **U53),
    "extractTotal": F(INT, **U53),
    "order": F(INT, lo=1, hi=1 << 31),
}, forbid=("computerReceived", "downloadSpeed", "sendTotal", "ok"), post=_job_post, src="APPO/model.cpp:267-307")
JOBS = Arr(JOB, post=_jobs_post, src="APPO/api.cpp:323-349")

# POST /api/v1/downloads -> 201 {"id"} — APPO/api.cpp:351-370 (a missing id only skips the focus)
CREATED = Obj({"id": F(STR, regex=RX_JOB_ID)}, src="APPO/api.cpp:364-369")


# --------------------------------------------------------------------------- #
# GET /api/v1/storage — APPO/model.cpp:246-265 parse_storage (items without an id are
# dropped); APPO/api.cpp:214-226; contract-downloads §6; map-downloads §2.2, §6; R4.
# --------------------------------------------------------------------------- #
def _drive_label_ok(did, label):
    m = re.fullmatch(r"usb([0-7])", did)
    if m:
        return label == f"USB storage {int(m.group(1)) + 1}"
    if did == "ext1":
        return label == "M.2 SSD"
    m = re.fullmatch(r"ext([0-9])", did)
    if m:
        return label == f"Extended storage {int(m.group(1)) + 1}"
    if did == "internal":
        return label == "Internal storage"
    return bool(label)       # host root<i>: the XS label (basename)


def _drive_post(v, path, err, profile):
    if profile == "app":
        return
    f, p, pr = v.get("freeBytes"), v.get("pendingBytes"), v.get("projectedFreeBytes")
    if all(isinstance(x, int) and not isinstance(x, bool) for x in (f, p, pr)):
        _err_if(pr != f - p - RESERVE_GAP, err,
                f"{path}.projectedFreeBytes: {pr} != freeBytes - pendingBytes - 240 MiB = {f - p - RESERVE_GAP}")
    did, label = v.get("id"), v.get("label")
    if isinstance(did, str) and isinstance(label, str):
        _err_if(not _drive_label_ok(did, label), err, f"{path}.label: {label!r} is not the R4 label of {did!r}")
        _err_if((did == "internal") == bool(v.get("external")), err, f"{path}.external: wrong for {did!r}")
    pth = v.get("path")
    _err_if(isinstance(pth, str) and not pth.endswith("/homebrew"), err, f"{path}.path: {pth!r} is not <root>/homebrew")


def _drives_post(v, path, err, profile):
    if profile == "app":
        return
    ids = [d.get("id") for d in v if isinstance(d, dict)]
    _err_if(len(set(ids)) != len(ids), err, f"{path}: duplicate drive ids {ids}")


DRIVE = Obj({
    "id": F(STR, regex=RX_DRIVE_ID, minlen=1, app=("req", "len")),
    "label": F(STR, minlen=1),
    "path": F(STR, minlen=1),
    "freeBytes": F(INT, **U53),
    "totalBytes": F(INT, **U53),
    "pendingBytes": F(INT, **U53),
    "projectedFreeBytes": F(INT, lo=-MAX_SAFE_INT, hi=MAX_SAFE_INT),   # may be negative
    "external": F(BOOL),
}, post=_drive_post, src="APPO/model.cpp:246-265")
STORAGE = Arr(DRIVE, post=_drives_post, src="APPO/api.cpp:214-226")

# Download settings — contract-downloads §7; map-downloads §7; R7.
PREFERRED = Obj({"preferredStorage": F(STR, regex=RX_DRIVE_ID_OPT)}, src="APPO/api.cpp:485-498")
LIMIT = Obj({"concurrentDownloads": F(INT, lo=1, hi=4, app=("req", "range"), app_range=(1, 8))},
            src="APPO/api.cpp:500-513 (1..8 or 'Could not save the setting.'); R7 1..4")
EXTRACTION = Obj({"deleteArchives": F(BOOL)}, src="APPO/api.cpp:515-526")


# --------------------------------------------------------------------------- #
# GET /api/v1/debrid — APPO/model.cpp:172-200 parse_debrid (ignored unless provider is
# "torbox"; a host counts only with a numeric maxFileSize >= 0); APPO/api.cpp:139-151;
# map-downloads §8.
# --------------------------------------------------------------------------- #
def _debrid_post(v, path, err, profile):
    if profile == "app":
        return
    ids = [h.get("sourceId") for h in v.get("hosts", []) if isinstance(h, dict)]
    _err_if(len(set(ids)) != len(ids), err, f"{path}.hosts: duplicate sourceIds {ids}")


DEBRID = Obj({
    "provider": F(STR, enum=("torbox",), app=("req", "enum")),
    "available": F(BOOL),
    "connected": F(BOOL),
    "busy": F(BOOL),
    "hosts": F(ARR, items=Obj({
        "sourceId": F(STR, minlen=1),
        "available": F(BOOL),
        "maxFileSize": F(INT, **U53),
    })),
}, post=_debrid_post, src="APPO/model.cpp:172-190 (Debrid::supports 192-200)")


# --------------------------------------------------------------------------- #
# GET|POST /api/v1/browser — APPO/api.cpp:168-185 (object; state in the list; active
# needs an id, else "unreadable verification status"; 16 KiB cap); APPO/model.cpp:118-129;
# map-downloads §9; PORT-SPEC 1.5.3 (never 404).
# --------------------------------------------------------------------------- #
def _browser_post(v, path, err, profile):
    if v.get("active") is True:
        _err_if(not v.get("id"), err, f"{path}.id: an active session needs an id (APPO/api.cpp:181)")


BROWSER = Obj({
    "available": F(BOOL),
    "active": F(BOOL),
    "id": F(STR),
    "state": F(STR, enum=BROWSER_STATES, app=("req", "enum")),
    "title": F(STR),
    "message": F(STR),
    "jobId": F(STR),
}, post=_browser_post, src="APPO/api.cpp:168-185, APPO/model.cpp:118-129")


# --------------------------------------------------------------------------- #
# GET|POST /api/v1/library, POST /api/v1/library/actions (202) — APPO/model.cpp:344-416
# parse_library (games without a titleId are dropped; sizes null = -1); APPO/api.cpp:
# 257-270, 287-321; contract-library §1, §6; map-library §3, §6; PORT-SPEC 1.5.4.
# --------------------------------------------------------------------------- #
def _library_post(v, path, err, profile):
    if profile == "app":
        return
    _err_if(v.get("status") == "ready" and v.get("message") != "", err,
            f"{path}.message: must be \"\" on a ready snapshot (it reads as a warning)")
    pairs = [(g.get("titleId"), g.get("sourceKey")) for g in v.get("games", []) if isinstance(g, dict)]
    _err_if(len(set(pairs)) != len(pairs), err, f"{path}.games: duplicate (titleId, sourceKey) pairs")
    for i, g in enumerate(v.get("games", [])):
        if isinstance(g, dict):
            _err_if(g.get("titleId") == "PPSA99500", err, f"{path}.games[{i}]: our own PPSA99500 is hidden")
            fmt = g.get("format")
            if fmt in TV_FORMATS:
                img = fmt in IMAGE_FORMATS
                _err_if(g.get("managed") != img or g.get("canManageSource") != img, err,
                        f"{path}.games[{i}]: managed/canManageSource must be {img} for {fmt}")
            _err_if(g.get("sizeBytes") is None and g.get("sizeStatus") == "ready", err,
                    f"{path}.games[{i}].sizeStatus: \"ready\" needs a size")


LIBRARY_GAME = Obj({
    "titleId": F(STR, regex=RX_TITLE_ID, minlen=1, app=("req", "len")),
    "title": F(STR),
    "platform": F(STR, enum=("ps5", "ps4", "unknown")),
    "format": F(STR, enum=TV_FORMATS + ("Folder", "")),   # "" = installed-only row (map-library §3.4)
    "path": F(STR, minlen=1),
    "runtimePath": F(STR),
    "sourceKey": F(STR, regex=RX_HEX64),
    "location": F(STR, minlen=1),
    "installed": F(BOOL),
    "mounted": F(BOOL),
    "onDrive": F(BOOL),
    "managed": F(BOOL),
    "canManageSource": F(BOOL),
    "installedSizeBytes": F((INT, NULL), **U53),
    "sizeBytes": F((INT, NULL), **U53),
    "sizeStatus": F(STR, enum=("ready", "unknown", "unavailable")),
}, src="APPO/model.cpp:355-377")
LIBRARY_ACTION = Obj({
    "id": F(STR),
    "action": F(STR, enum=LIBRARY_ACTIONS),
    "titleId": F(STR, regex=RX_TITLE_ID_OPT),
    "state": F(STR, enum=("queued", "running", "complete", "error")),
    "message": F(STR),
}, src="APPO/model.cpp:383-392")
STORAGE_JOB = Obj({
    "id": F(INT, lo=1, hi=MAX_SAFE_INT),                 # the bar is drawn only when id > 0
    "operation": F(STR, enum=("copy", "move")),
    "state": F(STR, minlen=1),
    "titleId": F(STR),
    "source": F(STR),
    "destination": F(STR),
    "error": F(STR),
    "totalBytes": F(INT, **U53),
    "processedBytes": F(INT, **U53),
    "speed": F(INT, **U53),
    "active": F(BOOL),
    "cancellable": F(BOOL),
    "cancelRequested": F(BOOL),
}, src="APPO/model.cpp:393-411")
LIBRARY = Obj({
    "status": F(STR, enum=("idle", "ready", "unavailable", "unsupported", "error")),
    "message": F(STR),
    "provider": F(STR),
    "providerVersion": F(STR),
    "busy": F(BOOL),
    "stale": F(BOOL),
    "checkedAt": F(INT, **U53),
    "updatedAt": F(INT, **U53),
    "refreshAfter": F(INT, **U53),
    "games": F(ARR, items=LIBRARY_GAME),
    "capabilities": F(ARR, items=F(STR, enum=LIBRARY_CAPS)),
    "action": F((OBJ, NULL), obj=LIBRARY_ACTION),
    "storageJob": F((OBJ, NULL), obj=STORAGE_JOB),
    "storageError": F(STR),
    "storageBusy": F(BOOL),
}, post=_library_post, src="APPO/model.cpp:344-416")


# --------------------------------------------------------------------------- #
# GET|POST /api/v1/library/storage — APPO/model.cpp:418-430 + 328-340; APPO/api.cpp:272-285;
# contract-library §4; map-library §5.
# --------------------------------------------------------------------------- #
def _lib_drive_post(v, path, err, profile):
    if profile == "app":
        return
    t, f, u = v.get("totalBytes"), v.get("freeBytes"), v.get("usedBytes")
    if all(isinstance(x, int) and not isinstance(x, bool) for x in (t, f, u)):
        _err_if(u != max(0, t - f), err, f"{path}.usedBytes: {u} != max(0, totalBytes - freeBytes) = {max(0, t - f)}")


LIBRARY_DRIVE = Obj({
    "id": F(STR, regex=RX_HEX64),
    "path": F(STR, minlen=1),
    "label": F(STR, minlen=1),
    "filesystem": F(STR),
    "readOnly": F(BOOL),
    "totalBytes": F(INT, **U53),
    "freeBytes": F(INT, **U53),
    "usedBytes": F(INT, **U53),
}, post=_lib_drive_post, src="APPO/model.cpp:328-340")


def _lib_storage_post(v, path, err, profile):
    if profile == "app":
        return
    for key in ("drives", "destinations"):
        ids = [d.get("id") for d in v.get(key, []) if isinstance(d, dict)]
        _err_if(len(set(ids)) != len(ids), err, f"{path}.{key}: duplicate ids")
    for i, d in enumerate(v.get("destinations", [])):
        if isinstance(d, dict):
            _err_if(d.get("readOnly") is not False, err, f"{path}.destinations[{i}].readOnly: only writable roots are destinations")


LIBRARY_STORAGE = Obj({
    "drives": F(ARR, items=LIBRARY_DRIVE),
    "destinations": F(ARR, items=LIBRARY_DRIVE),
    "busy": F(BOOL),
    "stale": F(BOOL),
    "updatedAt": F(INT, **U53),
    "error": F(STR),
}, post=_lib_storage_post, src="APPO/model.cpp:418-430")


# --------------------------------------------------------------------------- #
# GET|POST /api/v1/updates — APPO/model.cpp:455-474; APPO/api.cpp:555-579; contract-system
# §5; map-system §5.3-5.5; PORT-SPEC 1.5.5.
# --------------------------------------------------------------------------- #
SERVICE_UPDATE = Obj({
    "available": F(BOOL),
    "updateAvailable": F(BOOL),
    "busy": F(BOOL),
    "restartRequired": F(BOOL),
    "runningVersion": F(STR, regex=RX_VERSION),
    "instanceId": F(STR, regex=RX_HEX32),
    "savedVersion": F(STR, regex=RX_VERSION_OPT),
    "latestVersion": F(STR, regex=RX_VERSION_OPT),
    "checksum": F(STR, regex=RX_HEX64_OPT),
    "phase": F(STR, enum=("idle", "checking", "checked", "downloading", "saving", "saved", "error", "stopping")),
    "error": F(STR),
    "received": F(INT, **U53),
    "retryAt": F(INT, **U53),
    "checkedAt": F(INT, **U53),
    "checkAfter": F(INT, **U53),
    "automaticCheckAfter": F(INT, req=False, **U53),
}, src="APPO/model.cpp:455-474")

# --------------------------------------------------------------------------- #
# GET|POST /api/v1/tv-app — APPO/model.cpp:476-493; APPO/api.cpp:581-605; contract-system §6;
# map-system §6.2; R11 (wire value "orbit" kept).
# --------------------------------------------------------------------------- #
TV_APP = Obj({
    "available": F(BOOL),
    "installed": F(STR, enum=("none", "orbit", "manual", "folder", "blocked")),
    "installedVersion": F(STR, regex=RX_VERSION_OPT),
    "latestVersion": F(STR, regex=RX_VERSION_OPT),
    "checksum": F(STR, regex=RX_HEX64_OPT),
    "size": F(INT, **U53),
    "phase": F(STR, enum=("idle", "checking", "checked", "downloading", "verifying", "installing", "installed", "error")),
    "error": F(STR),
    "busy": F(BOOL),
    "received": F(INT, **U53),
    "retryAt": F(INT, **U53),
    "checkedAt": F(INT, **U53),
    "checkAfter": F(INT, **U53),
    "updateAvailable": F(BOOL, req=False),
    "automaticCheckAfter": F(INT, req=False, **U53),
}, src="APPO/model.cpp:476-493")

# GET /api/v1/art/{id}/{kind} 202 — APPO/api.cpp:406-430 (202 = pending, body unread);
# PORT-SPEC 1.7 replies {"pending":true}. A 200 carries image bytes: see check_art_image().
# GET /api/v1/art/{id}/shot/{n} (XENO: the n-th screenshot, n < screenshotCount) answers the same way.
ART_PENDING = Obj({"pending": F(BOOL, enum=(True,))}, src="PORT-SPEC 1.7")


# --------------------------------------------------------------------------- #
# Route table
# --------------------------------------------------------------------------- #
ROUTES = {
    "GET /api/v1/system": SYSTEM,
    "GET /api/v1/session": SESSION,
    "GET /api/v1/sources": SOURCES,
    "POST /api/v1/sources": SOURCES,
    "GET /api/v1/catalog": CATALOG,
    "GET /api/v1/catalog/updates": CATALOGUE_STATUS,
    "POST /api/v1/catalog/updates": CATALOGUE_STATUS,
    "GET /api/v1/art/{id}/{kind}": ART_PENDING,
    "GET /api/v1/art/{id}/shot/{n}": ART_PENDING,
    "GET /api/v1/favorites": FAVORITES,
    "POST /api/v1/favorites": FAVORITES,
    "GET /api/v1/downloads": JOBS,
    "POST /api/v1/downloads": CREATED,
    "POST /api/v1/downloads/{id}/{action}": JOBS,
    "GET /api/v1/storage": STORAGE,
    "POST /api/v1/storage/preferred": PREFERRED,
    "POST /api/v1/settings/downloads": LIMIT,
    "POST /api/v1/settings/extraction": EXTRACTION,
    "GET /api/v1/debrid": DEBRID,
    "GET /api/v1/browser": BROWSER,
    "POST /api/v1/browser": BROWSER,
    "GET /api/v1/library": LIBRARY,
    "POST /api/v1/library": LIBRARY,
    "GET /api/v1/library/storage": LIBRARY_STORAGE,
    "POST /api/v1/library/storage": LIBRARY_STORAGE,
    "POST /api/v1/library/actions": LIBRARY,
    "GET /api/v1/updates": SERVICE_UPDATE,
    "POST /api/v1/updates": SERVICE_UPDATE,
    "GET /api/v1/tv-app": TV_APP,
    "POST /api/v1/tv-app": TV_APP,
}

# The success code of each route (PORT-SPEC 1.4). /zero/companion only ever answers 404.
SUCCESS_STATUS = {k: 200 for k in ROUTES}
SUCCESS_STATUS.update({
    "POST /api/v1/catalog/updates": 202,
    "GET /api/v1/art/{id}/{kind}": 202,          # 202 while fetching; 200 carries the bytes
    "GET /api/v1/art/{id}/shot/{n}": 202,
    "POST /api/v1/downloads": 201,
    "POST /api/v1/library/actions": 202,
    "POST /api/v1/updates": 202,
    "POST /api/v1/tv-app": 202,
})

# No Bearer needed (PORT-SPEC 1.3 step 2). /zero/companion has no gate at all.
OPEN_ROUTES = ("GET /api/v1/system", "GET /api/v1/session", "GET /api/v1/sources", "GET /api/v1/catalog",
               "GET /api/v1/catalog/updates", "GET /api/v1/zero/companion", "POST /api/v1/zero/companion")

# PORT-SPEC 1.4 gate column (R3 order: S, then B, then O). Session also needs a loopback peer.
GATES = {
    "GET /api/v1/system": "", "GET /api/v1/session": "S",
    "GET /api/v1/zero/companion": "", "POST /api/v1/zero/companion": "",
    "GET /api/v1/sources": "S", "POST /api/v1/sources": "SBO",
    "GET /api/v1/catalog": "S",
    "GET /api/v1/catalog/updates": "S", "POST /api/v1/catalog/updates": "SBO",
    "GET /api/v1/art/{id}/{kind}": "SB", "GET /api/v1/art/{id}/shot/{n}": "SB",
    "GET /api/v1/favorites": "SB", "POST /api/v1/favorites": "SBO",
    "GET /api/v1/downloads": "SB", "POST /api/v1/downloads": "SBO", "POST /api/v1/downloads/{id}/{action}": "SBO",
    "GET /api/v1/storage": "SB", "POST /api/v1/storage/preferred": "SBO",
    "POST /api/v1/settings/downloads": "SBO", "POST /api/v1/settings/extraction": "SBO",
    "GET /api/v1/debrid": "SB",
    "GET /api/v1/browser": "SB", "POST /api/v1/browser": "SBO",
    "GET /api/v1/library": "SB", "POST /api/v1/library": "SBO",
    "GET /api/v1/library/storage": "SB", "POST /api/v1/library/storage": "SBO",
    "POST /api/v1/library/actions": "SBO",
    "GET /api/v1/updates": "B", "POST /api/v1/updates": "SBO",
    "GET /api/v1/tv-app": "SB", "POST /api/v1/tv-app": "SBO",
}
ALL_ROUTES = tuple(GATES)
SAMPLE_PATHS = {k: k.split(" ", 1)[1].replace("{id}/{kind}", "fx-tidewater-ps5/cover")
                .replace("{id}/shot/{n}", "fx-tidewater-ps5/shot/0")
                .replace("{id}/{action}", "j_000000/pause") for k in GATES}

_PATTERNS = sorted({k.split(" ", 1)[1] for k in ROUTES} | {"/api/v1/zero/companion"},
                   key=lambda p: (-p.count("/"), p))


def route_key(method, path):
    """"GET", "/api/v1/downloads/j_1a2b3c/pause?x" -> "GET /api/v1/downloads/{id}/{action}"."""
    path = path.split("?", 1)[0]
    segs = path.strip("/").split("/")
    for pat in _PATTERNS:
        psegs = pat.strip("/").split("/")
        if len(psegs) == len(segs) and all(p.startswith("{") or p == s for p, s in zip(psegs, segs)):
            key = f"{method.upper()} {pat}"
            if key in ROUTES or pat == "/api/v1/zero/companion":
                return key
    raise KeyError(f"{method} {path}: not an /api/v1 route of the contract")


def _resolve(route):
    if route in ROUTES:
        return route, ROUTES[route]
    method, _, path = route.partition(" ")
    key = route_key(method, path)
    if key not in ROUTES:
        raise KeyError(f"{route}: this route has no success body (it only answers with errors)")
    return key, ROUTES[key]


def reply_errors(route, value, profile="xeno"):
    """Every contract violation of `value` (already parsed) as a list of strings."""
    if profile not in ("xeno", "app"):
        raise ValueError(profile)
    _, spec = _resolve(route)
    err = []
    _check_any(spec, value, "$", err, profile)
    return err


def check_reply(route, value, profile="xeno", limit=25):
    """Raises AssertionError listing the violations (with paths) of a success reply.
    `value` may be bytes/str (then strict_json runs first) or a parsed value."""
    key, _ = _resolve(route)
    if isinstance(value, (bytes, bytearray, str)):
        value = strict_json(value)
    err = reply_errors(key, value, profile)
    if err:
        more = f"\n  ... and {len(err) - limit} more" if len(err) > limit else ""
        raise AssertionError(f"{key} ({profile} contract): {len(err)} problem(s)\n  " + "\n  ".join(err[:limit]) + more)
    return value


# --------------------------------------------------------------------------- #
# Error envelope (PORT-SPEC 1.3; APPO/api.cpp:44-62 shows body.error to the player)
# --------------------------------------------------------------------------- #
_CODE = re.compile(r"^[a-z_]+$")


def assert_error_envelope(status, body, expect_status=None, sentence=None):
    """A refusal: a non-2xx status and {"error":"<human sentence>"} with no "ok" key.
    `body` may be raw bytes/str (parsed strictly) or a parsed value."""
    if expect_status is not None and status != expect_status:
        raise AssertionError(f"status {status}, expected {expect_status}; body {_short(body) if not isinstance(body, bytes) else body[:200]!r}")
    if 200 <= status < 300:
        raise AssertionError(f"status {status} is a success, not a refusal")
    if isinstance(body, (bytes, bytearray, str)):
        body = strict_json(body)
    if not isinstance(body, dict):
        raise AssertionError(f"error body is {_tname(body)}, not an object: {_short(body)}")
    if "ok" in body:
        raise AssertionError(f"error body carries the XENO code envelope key \"ok\": {_short(body)}")
    err = body.get("error")
    if not isinstance(err, str) or not err.strip():
        raise AssertionError(f"error body has no sentence in \"error\": {_short(body)}")
    if _CODE.match(err):
        raise AssertionError(f"\"error\" is a machine code, not a sentence: {err!r}")
    if err != err.strip() or re.search(r"[\x00-\x1f]", err):
        raise AssertionError(f"\"error\" has stray whitespace or control characters: {err!r}")
    if sentence is not None and err != sentence:
        raise AssertionError(f"\"error\" is {err!r}, expected {sentence!r}")
    return body


# --------------------------------------------------------------------------- #
# Art replies (PORT-SPEC 1.7)
# --------------------------------------------------------------------------- #
ART_MIME = {"webp": "image/webp", "png": "image/png", "jpeg": "image/jpeg"}


def sniff_image(data):
    """The app's magic-byte sniff (APPO/image.cpp:262-273): "webp", "png", "jpeg" or None."""
    if len(data) >= 12 and data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return "webp"
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        return "png"
    if data[:3] == b"\xff\xd8\xff":
        return "jpeg"
    return None


def check_art_image(status, headers, body):
    """A 200 art reply: decodable bytes whose Content-Type matches the sniffed format,
    with Cache-Control: no-store. `headers` keys are matched case-insensitively."""
    h = {k.lower(): v for k, v in dict(headers).items()}
    if status != 200:
        raise AssertionError(f"art status {status}, expected 200")
    kind = sniff_image(body)
    if kind is None:
        raise AssertionError(f"art bytes are not WebP, PNG or JPEG (starts {bytes(body[:12])!r})")
    ct = h.get("content-type", "").split(";")[0].strip()
    if ct != ART_MIME[kind]:
        raise AssertionError(f"Content-Type {ct!r} for {kind} bytes")
    if "no-store" not in h.get("cache-control", ""):
        raise AssertionError(f"Cache-Control is {h.get('cache-control')!r}, expected no-store")
    return kind


# --------------------------------------------------------------------------- #
# Fixed sentences (PORT-SPEC 1.3-1.5, Appendix A). Use S["key"].
# --------------------------------------------------------------------------- #
S = {
    "stopping": "XENO STORE is stopping. It will be back in a moment.",
    "unpaired": "Pair this device with your PS5 to continue.",
    "not_object": "Expected a JSON object.",
    "use_json": "Use application/json.",
    "not_available": "This request is not available in this version of XENO STORE.",
    "pair_again": "Pair this device again from Settings.",
    "session_remote": "Pair this device using the code shown on your PS5.",
    "pairing_save": "Could not save pairing.",
    "zero": "Orbit Zero is not part of XENO STORE.",
    "refresh_action": "Expected refresh action.",
    "refresh_network": "Catalogue refresh needs the console's network. Try again shortly.",
    "refresh_busy": "The catalogue is already refreshing.",
    "refresh_wait": "Please wait before refreshing the catalogue again.",
    "fav_bad": "Expected a game and favourite choice.",
    "fav_unknown": "Game is not in this catalogue.",
    "fav_full": "Favourites are full.",
    "sources_select": "Select the sources you want to enable.",
    "sources_unknown": "Unknown or duplicate source.",
    "sources_notice": "Read and acknowledge the current download notice before saving sources.",
    "limit": "Choose a whole number from 1 to 4.",
    "art_unknown": "Unknown artwork.",
    "art_failed": "This artwork could not be loaded.",
    "art_busy": "Artwork is busy. Try again shortly.",
    "browser_message": ("Vikingfile asks for a human check, so XENO STORE cannot fetch it by itself. "
                        "Download it with TorBox, or open this game in the browser version."),
    "browser_gone": "This verification session is no longer active.",
    "browser_action": "Unknown browser action.",
}


# --------------------------------------------------------------------------- #
# Request bodies, byte for byte as the app builds them.
# Ported from Orbit Store app/src/orbit/api.cpp and json.cpp (GPL-3.0-or-later).
# --------------------------------------------------------------------------- #
def quote(value):
    """APPO/json.cpp:447-486: escapes ", \\, \\n, \\r, \\t and other bytes < 0x20 as
    \\u00xx; everything else (UTF-8 included) is copied as is."""
    out = ['"']
    for ch in value:
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif ord(ch) < 0x20:
            out.append("\\u%04x" % ord(ch))
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


class AppBody:
    """POST bodies as the app sends them (UTF-8 str; encode before sending)."""

    @staticmethod
    def save_sources(enabled, notice_version=1):                 # APPO/api.cpp:461-473
        return ('{"enabled":[' + ",".join(quote(e) for e in enabled) + '],"acknowledged":true,"noticeVersion":'
                + str(int(notice_version)) + "}")

    @staticmethod
    def favourite(game_id, favourite):                           # APPO/api.cpp:242-248
        return '{"gameId":' + quote(game_id) + (',"favorite":true}' if favourite else ',"favorite":false}')

    @staticmethod
    def refresh_catalogue():                                     # APPO/api.cpp:542-546
        return '{"action":"refresh"}'

    @staticmethod
    def create_download(release_id, storage_id, torbox=False):   # APPO/api.cpp:351-361
        return ('{"releaseId":' + quote(release_id) + ',"storageId":' + quote(storage_id)
                + (',"delivery":"torbox"' if torbox else "") + "}")

    @staticmethod
    def job_action(action, delete_partial=False, password=""):  # APPO/api.cpp:394-397
        if action == "archive-password":
            return '{"password":' + quote(password) + "}"
        return '{"deletePartial":true}' if delete_partial else "{}"

    @staticmethod
    def preferred_storage(storage_id):                           # APPO/api.cpp:485-490
        return '{"storageId":' + quote(storage_id) + "}"

    @staticmethod
    def download_limit(limit):                                   # APPO/api.cpp:500-505
        return '{"concurrentDownloads":' + str(int(limit)) + "}"

    @staticmethod
    def delete_archives(enabled):                                # APPO/api.cpp:515-519
        return '{"deleteArchives":true}' if enabled else '{"deleteArchives":false}'

    @staticmethod
    def browser_start(release_id, storage_id):                   # APPO/api.cpp:189-193
        return '{"action":"start","releaseId":' + quote(release_id) + ',"storageId":' + quote(storage_id) + "}"

    @staticmethod
    def browser_cancel(session_id):                              # APPO/api.cpp:195-198
        return '{"action":"cancel","id":' + quote(session_id) + "}"

    @staticmethod
    def library_refresh():                                       # APPO/api.cpp:261, 275
        return "{}"

    @staticmethod
    def library_action(action, request_id, title_id="", source_key="", destination_id="", job_id=0,
                       confirmed=False):                         # APPO/api.cpp:287-313
        body = '{"action":' + quote(action)
        if title_id:
            body += ',"titleId":' + quote(title_id)
        if source_key:
            body += ',"sourceKey":' + quote(source_key)
        if destination_id:
            body += ',"destinationId":' + quote(destination_id)
        if job_id > 0:
            body += ',"jobId":' + str(int(job_id))
        if confirmed:
            body += ',"confirmed":true'
        return body + ',"requestId":' + quote(request_id) + "}"

    @staticmethod
    def update(action, version="", checksum="", automatic=False, confirmed=False, replace=False):
        body = '{"action":' + quote(action)                     # APPO/api.cpp:435-457 update_body
        if version:
            body += ',"version":' + quote(version)
        if checksum:
            body += ',"checksum":' + quote(checksum)
        if automatic:
            body += ',"automatic":true'
        if confirmed:
            body += ',"confirmed":true'
        if replace:
            body += ',"replace":true'
        return body + "}"
