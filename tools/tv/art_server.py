#!/usr/bin/env python3
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
art_server.py - the artwork host of the live preview world (PORT-SPEC 4.1).

Serves the URLs the TV fixture catalogue uses (tests/host/tv/fixture-catalog.json,
"{{ART}}/art/<gameId>/[fallback/]<name>"), so the backend's /api/v1/art fetches
real pictures over loopback HTTP (the host build allows http:// art from
XENO_HOST_ART_FIXTURE only):

  good names   pictures made with PIL for the screens: a gold/blue gradient that
               depends on the game, the game's title in the app's Inter typeface
               and a small format tag. Covers are 512x512 (WebP lossy, lossless
               and with alpha, PNG, baseline and greyscale JPEG), heroes 1920x1080
               WebP (also PNG and progressive JPEG for the fixture's PNG/JPEG
               games), screenshots (1920x1080 WebP, 1280x720 JPEG and a tall
               720x1280 WebP), plus the exact-cap sizes of PORT-SPEC 1.7.
  bad names    the refused and skipped images of PORT-SPEC 1.7/1.10 (animated
               WebP, a 9000 px side, a 2000x2000 PNG cover, CMYK JPEG, square
               heroes, text behind an image name, empty and truncated files, a
               file past the 12 MiB fetch limit, AVIF, GIF), taken from
               tests/host/tv/artgen.py when it is there, so the backend tests and
               the preview use the same bytes.

Every picture is deterministic (no randomness, cached per path), so two runs
render the same screens and `preview_live.py --compare` stays quiet.

    import art_server
    srv = art_server.ArtServer(titles={"fx-tidewater-ps5": "Tidewater"}).start()
    srv.hostport, srv.base           # -> XENO_HOST_ART_FIXTURE, {{ART}}
    srv.hits("/art/fx-tidewater-ps5/cover.webp"); srv.requests; srv.reset(); srv.stop()
    art_server.make("cover.webp", "Tidewater", seed="fx-tidewater-ps5")   # bytes
    art_server.check(data, "cover")  # [] when the backend's PORT-SPEC 1.7 rules accept it

Command line (python3 -I tools/tv/art_server.py):
    --port N [--catalog catalog.json]   serve until Ctrl-C; GET /ctl/hits, /ctl/reset
    --write DIR [--title T]             write every picture of the set into DIR
    --self-test                         decode every good picture, apply the 1.7 caps,
                                        check every bad one is refused; exit 1 on a miss

Python 3 standard library + PIL (python3-pil), nothing else.
"""
import argparse
import hashlib
import importlib
import io
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlsplit

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
TESTS_TV = os.path.join(REPO, "tests", "host", "tv")
FONT_DIRS = (os.path.join(REPO, "app", "tv", "third_party", "fonts"),
             "/usr/share/fonts/truetype/dejavu")
FONT_FILES = ("Inter-SemiBold.ttf", "DejaVuSans-Bold.ttf")

# PORT-SPEC 1.7 (what the backend accepts and passes through unchanged)
COVER_PIXEL_CAP = 1048576           # PNG/JPEG covers
HERO_PIXEL_CAP = 3686400            # PNG/JPEG heroes and screenshots (2560x1440)
MAX_SIDE = 8192                     # any side (the app refuses more)
MAX_FETCH_BYTES = 12 * 1024 * 1024  # cx_fetch maximum
HERO_MIN_RATIO = 1.4                # a hero must be at least 1.4 times as wide as high

# The TV theme: gold on dark, with the blue accents of the art set.
GOLD = [(212, 175, 55), (240, 200, 90), (184, 134, 11), (250, 222, 140)]
BLUE = [(24, 38, 74), (46, 84, 160), (16, 28, 56), (70, 110, 190)]
INK = (14, 15, 20)

MIME = {"webp": "image/webp", "png": "image/png", "jpg": "image/jpeg", "jpeg": "image/jpeg",
        "avif": "image/avif", "gif": "image/gif"}

# name -> (kind, width, height, format, options, description)
GOOD = {
    "cover.webp": ("cover", 512, 512, "WEBP", {"quality": 82}, "512x512 lossy WebP"),
    "cover-lossless.webp": ("cover", 512, 512, "WEBP", {"lossless": True}, "512x512 lossless WebP"),
    "cover-alpha.webp": ("cover", 512, 512, "WEBP-ALPHA", {"quality": 82}, "512x512 WebP with alpha"),
    "cover.png": ("cover", 512, 512, "PNG", {}, "512x512 PNG"),
    "cover.jpg": ("cover", 512, 512, "JPEG", {"quality": 85}, "512x512 baseline JPEG"),
    "cover-gray.jpg": ("cover", 512, 512, "JPEG-L", {"quality": 85}, "512x512 greyscale JPEG"),
    "max-1024.png": ("cover", 1024, 1024, "PNG", {}, "1024x1024 PNG, exactly the cover cap"),
    "hero.webp": ("hero", 1920, 1080, "WEBP", {"quality": 80}, "1920x1080 lossy WebP"),
    "hero.png": ("hero", 1920, 1080, "PNG", {}, "1920x1080 PNG"),
    "hero.jpg": ("hero", 1920, 1080, "JPEG", {"quality": 85, "progressive": True}, "1920x1080 progressive JPEG"),
    "hero-2560.png": ("hero", 2560, 1440, "PNG", {}, "2560x1440 PNG, exactly the hero cap"),
    "hero-ratio-140.webp": ("hero", 1400, 1000, "WEBP", {"quality": 80}, "1400x1000 WebP, ratio exactly 1.4"),
    "shot.webp": ("shot", 1920, 1080, "WEBP", {"quality": 80}, "1920x1080 lossy WebP screenshot"),
    "shot.jpg": ("shot", 1280, 720, "JPEG", {"quality": 85}, "1280x720 baseline JPEG screenshot"),
    "shot-tall.webp": ("shot", 720, 1280, "WEBP", {"quality": 80}, "720x1280 WebP screenshot (any shape)"),
}

# name -> (kind the catalogue uses it as, description). Built by artgen when present.
BAD = {
    "anim.webp": ("cover", "animated WebP"),
    "huge-9000.png": ("cover", "9000x100 PNG: a side above 8192"),
    "big-2000.png": ("cover", "2000x2000 PNG: over the cover pixel cap"),
    "hero-2561.png": ("hero", "2561x1441 PNG: just over the hero pixel cap"),
    "cmyk.jpg": ("cover", "CMYK JPEG (4 components)"),
    "hero-square.webp": ("hero", "1080x1080 WebP used as a hero"),
    "hero-square.png": ("hero", "1080x1080 PNG used as a hero"),
    "hero-ratio-139.webp": ("hero", "1390x1000 WebP: under the hero ratio"),
    "not-image.webp": ("cover", "HTML behind a .webp name"),
    "empty.png": ("cover", "0 bytes"),
    "truncated.png": ("cover", "the first 100 bytes of a PNG"),
    "too-big.webp": ("cover", "a WebP padded past the 12 MiB fetch limit"),
    "cover.avif": ("cover", "AVIF (the backend never requests .avif URLs)"),
    "cover.gif": ("cover", "GIF (the backend never requests .gif URLs)"),
}

_CACHE = {}
_CACHE_MU = threading.Lock()
_FONTS = {}
_artgen = None


# --------------------------------------------------------------------------- #
# Pictures
# --------------------------------------------------------------------------- #

def _digest(seed):
    return hashlib.sha256(seed.encode("utf-8")).digest()


def _font(size):
    if size in _FONTS:
        return _FONTS[size]
    font = None
    for d in FONT_DIRS:
        for f in FONT_FILES:
            p = os.path.join(d, f)
            if os.path.exists(p):
                try:
                    font = ImageFont.truetype(p, size)
                    break
                except OSError:
                    pass
        if font:
            break
    if font is None:
        try:
            font = ImageFont.load_default(size=size)     # Pillow >= 10.1
        except TypeError:
            font = ImageFont.load_default()
    _FONTS[size] = font
    return font


def _gradient(w, h, a, b, d):
    """A two-colour diagonal gradient: a 2x2 seed scaled up bilinearly (fast at any size)."""
    mid = tuple((x + y) // 2 for x, y in zip(a, b))
    seed = Image.new("RGB", (2, 2))
    corners = [a, mid, mid, b] if d[0] & 1 else [mid, a, b, mid]
    seed.putdata(corners)
    return seed.resize((w, h), Image.BILINEAR)


def _wrap(draw, text, font, width):
    words, lines, line = text.split(), [], ""
    for word in words:
        trial = (line + " " + word).strip()
        if line and draw.textlength(trial, font=font) > width:
            lines.append(line)
            line = word
        else:
            line = trial
    if line:
        lines.append(line)
    return lines or [text]


def picture(w, h, title, seed, tag=""):
    """An RGBA picture: the game's gradient, a few soft bands, the title and a tag.
    Covers carry the title on a dark label at the bottom, like box art, and the
    tag top right; wide pictures (heroes) carry the title small on the right,
    where the app's own hero text (bottom left) does not cover it, with the tag
    under it."""
    wide = w >= HERO_MIN_RATIO * h
    d = _digest(seed or title)
    gold, blue = GOLD[d[1] % len(GOLD)], BLUE[d[2] % len(BLUE)]
    a, b = (gold, blue) if d[3] & 1 else (blue, gold)
    im = _gradient(w, h, a, b, d)
    over = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    dr = ImageDraw.Draw(over)
    # Diagonal light bands, placed by the seed.
    for i in range(3):
        x0 = (d[4 + i] * w) // 256
        span = max(8, w // (5 + (d[7 + i] % 4)))
        dr.polygon([(x0, 0), (x0 + span, 0), (x0 + span - h // 2, h), (x0 - h // 2, h)],
                   fill=(255, 255, 255, 18 + (d[10 + i] % 20)))
    if not wide:   # a dark label behind the title, like box art
        dr.rectangle([0, int(h * 0.62), w, h], fill=INK + (150,))
    im = Image.alpha_composite(im.convert("RGBA"), over)
    dr = ImageDraw.Draw(im)
    unit = min(w, h)
    size = max(12, unit // (14 if wide else 10))
    font = _font(size)
    if wide:
        x0, y = int(w * 0.58), int(h * 0.40)
        lines = _wrap(dr, title, font, int(w * 0.36))[:2]
    else:
        x0, y = int(w * 0.07), int(h * 0.62) + max(6, unit // 40)
        lines = _wrap(dr, title, font, int(w * 0.86))[:3]
    for line in lines:
        dr.text((x0 + 2, y + 2), line, font=font, fill=(0, 0, 0, 200))
        dr.text((x0, y), line, font=font, fill=(250, 236, 200, 255))
        y += int(size * 1.15)
    if tag:
        small = _font(max(10, unit // 26))
        tw = dr.textlength(tag, font=small)
        pad = max(4, unit // 80)
        if wide:   # under the title: the app's top bar covers the top right of a hero
            x1, y1 = x0 + int(tw) + 2 * pad, y + pad
        else:
            x1, y1 = w - int(w * 0.04), int(h * 0.05)
        dr.rectangle([x1 - tw - 2 * pad, y1, x1, y1 + int(small.size * 1.4) + pad], outline=gold + (255,),
                     width=max(1, unit // 256))
        dr.text((x1 - tw - pad, y1 + pad // 2), tag, font=small, fill=gold + (255,))
    # Gold rule along the bottom edge: the theme's accent.
    dr.rectangle([0, h - max(3, unit // 90), w, h], fill=gold + (255,))
    return im


def _encode(im, fmt, opts):
    buf = io.BytesIO()
    if fmt == "WEBP-ALPHA":
        im = im.copy()
        im.putalpha(235)
        im.save(buf, "WEBP", **opts)
    elif fmt == "JPEG-L":
        im.convert("L").save(buf, "JPEG", **opts)
    elif fmt in ("JPEG", "PNG"):
        im.convert("RGB").save(buf, fmt, **opts)
    else:
        im.convert("RGB").save(buf, fmt, **opts)
    return buf.getvalue()


def _load_artgen():
    global _artgen
    if _artgen is None:
        if os.path.isdir(TESTS_TV) and TESTS_TV not in sys.path:
            sys.path.insert(0, TESTS_TV)
        try:
            _artgen = importlib.import_module("artgen")
        except Exception:
            _artgen = False
    return _artgen or None


def _bad_fallback(name, seed):
    """The bad set without tests/host/tv/artgen.py (or with a PIL that lacks AVIF)."""
    if name == "anim.webp":
        frames = [picture(256, 256, "anim", seed + str(i)) for i in range(3)]
        buf = io.BytesIO()
        frames[0].save(buf, "WEBP", save_all=True, append_images=frames[1:], duration=120, loop=0)
        return buf.getvalue()
    sizes = {"huge-9000.png": (9000, 100), "big-2000.png": (2000, 2000), "hero-2561.png": (2561, 1441),
             "hero-square.png": (1080, 1080)}
    if name in sizes:
        w, h = sizes[name]
        return _encode(Image.new("RGB", (w, h), BLUE[0]), "PNG", {})
    if name == "cmyk.jpg":
        buf = io.BytesIO()
        Image.new("CMYK", (512, 512), (0, 80, 200, 20)).save(buf, "JPEG", quality=80)
        return buf.getvalue()
    if name in ("hero-square.webp", "hero-ratio-139.webp"):
        w, h = (1080, 1080) if name == "hero-square.webp" else (1390, 1000)
        return _encode(picture(w, h, name, seed), "WEBP", {"quality": 70})
    if name == "not-image.webp":
        return b"<html><body>not an image " + seed.encode("utf-8") + b"</body></html>"
    if name == "empty.png":
        return b""
    if name == "truncated.png":
        return _encode(picture(512, 512, name, seed), "PNG", {})[:100]
    if name == "too-big.webp":
        head = _encode(picture(64, 64, name, seed), "WEBP", {"quality": 70})
        return head + b"\0" * (MAX_FETCH_BYTES + 1 - len(head))
    if name == "cover.gif":
        buf = io.BytesIO()
        picture(128, 128, name, seed).convert("P").save(buf, "GIF")
        return buf.getvalue()
    if name == "cover.avif":   # a valid ftyp box: enough for a sniffer, never decoded
        return b"\x00\x00\x00\x1cftypavif\x00\x00\x00\x00avifmif1miaf"
    raise KeyError(name)


def make(name, title="", seed=""):
    """The bytes of one picture of the set (deterministic; cached)."""
    key = (name, title, seed)
    with _CACHE_MU:
        if key in _CACHE:
            return _CACHE[key]
    if name in GOOD:
        kind, w, h, fmt, opts, _ = GOOD[name]
        tag = {"WEBP": "WEBP", "WEBP-ALPHA": "WEBP+A", "PNG": "PNG", "JPEG": "JPEG", "JPEG-L": "JPEG L"}[fmt]
        data = _encode(picture(w, h, title or seed or name, seed or title or name, tag), fmt, opts)
    elif name in BAD:
        ag = _load_artgen()
        data = None
        if ag is not None and name in getattr(ag, "ART_SET", {}):
            try:
                data = ag.make(name, seed=seed or name)
            except Exception:
                data = None
        if data is None:
            data = _bad_fallback(name, seed or name)
    else:
        raise KeyError(name)
    with _CACHE_MU:
        _CACHE[key] = data
    return data


def content_type(name):
    return MIME.get(name.rsplit(".", 1)[-1].lower(), "application/octet-stream")


# --------------------------------------------------------------------------- #
# The PORT-SPEC 1.7 acceptance rules, as the backend applies them
# --------------------------------------------------------------------------- #

def _jpeg_sof(data):
    i = 2
    while i + 4 <= len(data):
        if data[i] != 0xFF:
            return None
        m = data[i + 1]
        if m in (0xD8, 0x01) or 0xD0 <= m <= 0xD7:
            i += 2
            continue
        seg = int.from_bytes(data[i + 2:i + 4], "big")
        if 0xC0 <= m <= 0xCF and m not in (0xC4, 0xC8, 0xCC):
            if i + 10 > len(data):
                return None
            return m, data[i + 4], data[i + 9]
        i += 2 + seg
    return None


def sniff(data):
    if len(data) >= 12 and data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return "webp"
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        return "png"
    if data[:3] == b"\xff\xd8\xff":
        return "jpeg"
    return None


def check(data, kind):
    """The reasons the backend would refuse `data` as a `kind` ("cover", "hero" or "shot");
    [] when it is served. Also decodes the picture fully, as the app would."""
    why = []
    if len(data) > MAX_FETCH_BYTES:
        why.append(f"{len(data)} bytes, over the 12 MiB fetch limit")
    fmt = sniff(data)
    if fmt is None:
        return why + ["not WebP, PNG or JPEG"]
    if fmt == "webp" and data[12:16] == b"VP8X" and len(data) > 20 and data[20] & 0x02:
        why.append("animated WebP")
    try:
        with Image.open(io.BytesIO(data)) as im:
            w, h = im.size
            if getattr(im, "is_animated", False):
                why.append("animated")
            im.load()
    except Exception as e:  # noqa: BLE001
        return why + [f"does not decode ({e.__class__.__name__})"]
    if w > MAX_SIDE or h > MAX_SIDE:
        why.append(f"{w}x{h}: a side over {MAX_SIDE}")
    if fmt in ("png", "jpeg"):
        cap = COVER_PIXEL_CAP if kind == "cover" else HERO_PIXEL_CAP
        if w * h > cap:
            why.append(f"{w}x{h} = {w * h} px, over the {kind} cap of {cap}")
    if fmt == "jpeg":
        sof = _jpeg_sof(data)
        if sof is None:
            why.append("no JPEG SOF")
        else:
            marker, precision, comps = sof
            if marker not in (0xC0, 0xC1, 0xC2):
                why.append(f"JPEG SOF 0x{marker:02X} (only SOF0/1/2)")
            if precision != 8:
                why.append(f"{precision}-bit JPEG")
            if comps not in (1, 3):
                why.append(f"JPEG with {comps} components")
    if kind == "hero" and w < HERO_MIN_RATIO * h:
        why.append(f"{w}x{h} is not wide enough for a hero (w < 1.4 h)")
    return why


# --------------------------------------------------------------------------- #
# The server
# --------------------------------------------------------------------------- #

def _path_of(url_or_path):
    p = urlsplit(url_or_path).path if "://" in url_or_path else url_or_path.split("?", 1)[0]
    return unquote(p)


def titles_from_catalog(catalog):
    """{gameId: title} from a catalogue dict (or a path to one)."""
    if isinstance(catalog, str):
        with open(catalog, encoding="utf-8") as f:
            catalog = json.load(f)
    out = {}
    for g in (catalog or {}).get("games", []):
        if isinstance(g, dict) and isinstance(g.get("id"), str):
            out[g["id"]] = g.get("title") or g["id"]
    return out


def _pretty(game_id):
    words = [w for w in game_id.replace("_", "-").split("-") if w and w not in ("fx", "ps4", "ps5")]
    return " ".join(w.capitalize() for w in words) or game_id


class ArtServer:
    """Threaded HTTP server for /art/<gameId>/[fallback/]<name>. Same members as
    tests/host/tv/artgen.ArtServer (hostport, base, hits, requests, reset,
    set_status, set_delay, set_bytes, bytes_for), plus titles for the pictures."""

    def __init__(self, titles=None, host="127.0.0.1", port=0):
        self.titles = dict(titles or {})
        self._host, self._port = host, port
        self._mu = threading.Lock()
        self._status, self._delay, self._bytes, self._hits = {}, {}, {}, {}
        self.requests = []          # (path, status, bytes, unix time)
        self.httpd = None

    def start(self):
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, fmt, *args):
                pass

            def do_GET(self):  # noqa: N802
                owner._serve(self, head=False)

            def do_HEAD(self):  # noqa: N802
                owner._serve(self, head=True)

        self.httpd = ThreadingHTTPServer((self._host, self._port), Handler)
        self.httpd.daemon_threads = True
        threading.Thread(target=self.httpd.serve_forever, name="art-server", daemon=True).start()
        return self

    def stop(self):
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()
            self.httpd = None

    @property
    def port(self):
        return self.httpd.server_address[1]

    @property
    def hostport(self):
        return f"{self._host}:{self.port}"

    @property
    def base(self):
        return f"http://{self.hostport}"

    def url(self, game_id, name, fallback=False):
        return f"{self.base}/art/{game_id}/{'fallback/' if fallback else ''}{name}"

    def set_status(self, path, code, times=None):
        with self._mu:
            self._status[_path_of(path)] = [int(code), times]

    def set_delay(self, path, seconds):
        with self._mu:
            self._delay[_path_of(path)] = float(seconds)

    def set_bytes(self, path, data, ctype="application/octet-stream"):
        with self._mu:
            self._bytes[_path_of(path)] = (bytes(data), ctype)

    def reset(self):
        with self._mu:
            self._status.clear(), self._delay.clear(), self._bytes.clear(), self._hits.clear()
            self.requests.clear()

    def hits(self, path=None):
        with self._mu:
            if path is None:
                return dict(self._hits)
            return self._hits.get(_path_of(path), 0)

    def served(self):
        """[(path, status)] of every answered request, in order."""
        with self._mu:
            return [(p, s) for p, s, _, _ in self.requests]

    @staticmethod
    def split(path):
        parts = path.split("/")
        if len(parts) == 4 and parts[1] == "art":
            return parts[2], parts[3]
        if len(parts) == 5 and parts[1] == "art" and parts[3] == "fallback":
            return parts[2], parts[4]
        return None

    def bytes_for(self, path):
        p = _path_of(path)
        with self._mu:
            if p in self._bytes:
                return self._bytes[p]
        sp = self.split(p)
        if not sp or (sp[1] not in GOOD and sp[1] not in BAD):
            return None
        gid, name = sp
        title = self.titles.get(gid) or _pretty(gid)
        # The fallback picture looks different from the primary one (seed = path).
        return make(name, title, seed=p), content_type(name)

    def _ctl(self, h, p):
        if p == "/ctl/hits":
            body = json.dumps({"hits": self.hits(), "served": self.served()}).encode()
        elif p == "/ctl/reset":
            self.reset()
            body = b'{"ok":true}'
        else:
            return False
        h.send_response(200)
        h.send_header("Content-Type", "application/json")
        h.send_header("Content-Length", str(len(body)))
        h.send_header("Connection", "close")
        h.end_headers()
        h.wfile.write(body)
        return True

    def _serve(self, h, head):
        p = _path_of(h.path)
        if p.startswith("/ctl/") and self._ctl(h, p):
            h.close_connection = True
            return
        with self._mu:
            self._hits[p] = self._hits.get(p, 0) + 1
            delay = self._delay.get(p, 0.0)
            st = self._status.get(p)
            code = None
            if st:
                code = st[0]
                if st[1] is not None:
                    st[1] -= 1
                    if st[1] <= 0:
                        del self._status[p]
        if delay:
            time.sleep(delay)
        got = None
        if code is None:
            try:
                got = self.bytes_for(p)
            except Exception:  # noqa: BLE001  (a builder failed: answer 500, the backend falls back)
                code = 500
            if got is None and code is None:
                code = 404
        if code is not None and code != 200:
            body, ctype = f"status {code}\n".encode(), "text/plain"
        else:
            body, ctype = got if got is not None else (self.bytes_for(p) or (b"", "application/octet-stream"))
            code = 200
        with self._mu:
            self.requests.append((p, code, len(body), time.time()))
        try:
            h.send_response(code)
            h.send_header("Content-Type", ctype)
            h.send_header("Content-Length", str(len(body)))
            h.send_header("Connection", "close")
            h.end_headers()
            if not head:
                h.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass
        h.close_connection = True


# --------------------------------------------------------------------------- #
# Command line
# --------------------------------------------------------------------------- #

def write_set(directory, title="XENO STORE", seed="xeno-store"):
    os.makedirs(directory, exist_ok=True)
    out = {}
    for name in list(GOOD) + list(BAD):
        p = os.path.join(directory, name)
        with open(p, "wb") as f:
            f.write(make(name, title, seed=seed + "/" + name))
        out[name] = p
    return out


def self_test(verbose=True):
    """Every good picture decodes and passes the 1.7 rules at its kind; every bad
    one is refused (or is an AVIF/GIF the backend never asks for). Returns the
    list of failures."""
    fails = []
    for name, (kind, w, h, fmt, _, desc) in GOOD.items():
        data = make(name, "Self Test Title", seed="self-test/" + name)
        why = check(data, kind)
        with Image.open(io.BytesIO(data)) as im:
            size = im.size
        if size != (w, h):
            why.append(f"size {size}, expected {(w, h)}")
        if verbose:
            print(f"good {name:22} {len(data):>9} B  {size[0]}x{size[1]} {kind:5} {'ok' if not why else why}")
        if why:
            fails.append(f"{name}: {why}")
    for name, (kind, desc) in BAD.items():
        data = make(name, "Self Test Title", seed="self-test/" + name)
        why = check(data, kind)
        skipped = name.endswith((".avif", ".gif"))
        ok = bool(why) or skipped
        if verbose:
            print(f"bad  {name:22} {len(data):>9} B  {kind:5} {'refused: ' + '; '.join(why) if why else ('skipped by URL' if skipped else 'ACCEPTED')}")
        if not ok:
            fails.append(f"{name}: accepted, but PORT-SPEC 1.7 refuses it ({desc})")
    return fails


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", type=int)
    ap.add_argument("--catalog", help="catalogue JSON for the titles")
    ap.add_argument("--write", metavar="DIR")
    ap.add_argument("--title", default="XENO STORE")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        fails = self_test()
        print("art_server self-test:", "OK" if not fails else f"{len(fails)} failure(s)")
        for f in fails:
            print("  " + f)
        return 1 if fails else 0
    if a.write:
        for name, p in write_set(a.write, a.title).items():
            print(f"{name:22} {os.path.getsize(p):>9}  {p}")
        return 0
    if a.port is None:
        ap.print_help()
        return 2
    srv = ArtServer(titles_from_catalog(a.catalog) if a.catalog else {}, port=a.port).start()
    print(f"art_server listening on {srv.base}", flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
    finally:
        srv.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
