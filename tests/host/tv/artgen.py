#!/usr/bin/env python3
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
artgen.py — the art set of PORT-SPEC 1.7/1.10, generated at test time, and a
small threaded HTTP art server for the /api/v1/art tests.

Every picture is made with PIL (python3-pil): WebP (lossy VP8, lossless VP8L,
still VP8X with alpha), PNG, JPEG (baseline, progressive, grey, CMYK), AVIF,
GIF and animated WebP all encode natively, so no file is written by hand. The
pixels depend on the request path, so a test can tell which candidate URL the
backend served (primary or fallback) by comparing bytes with bytes_for(path).

    import artgen                                  # tests/host/tv/ on sys.path (tvworld does it)
    srv = artgen.ArtServer().start()               # 127.0.0.1:<random port>
    srv.hostport                                   # "127.0.0.1:41234" -> XENO_HOST_ART_FIXTURE
    srv.base                                       # "http://127.0.0.1:41234" -> {{ART}} in the fixture
    srv.set_status("/art/fx-ink-well-ps5/fallback/cover.png", 500)       # every request, or times=N
    srv.set_delay("/art/fx-tidewater-ps5/cover.webp", 3.0)               # slow answer
    srv.set_bytes("/art/x/cover.webp", b"...", "image/webp")             # any body
    srv.set_redirect("/art/x/cover.webp", srv.url("y", "cover.png"))     # 302 (or code=301/307/308)
    srv.hits("/art/fx-tidewater-ps5/cover.webp")   # request count (path or full URL)
    srv.requests                                   # [(path, {header: value}), ...] in arrival order
    srv.bytes_for("/art/fx-tidewater-ps5/cover.webp")                    # the 200 body for that path
    srv.reset(); srv.stop()

    artgen.make("hero.webp", seed="any")           # bytes of one ART_SET entry
    artgen.describe(data)                          # {"format","width","height","mode","animated","sof"}
    artgen.write_art_set(directory)                # every ART_SET entry as files (tests/host/tv/art/)

Subclass hook: ArtServer._serve(handler, head) answers one request (BaseHTTPRequestHandler,
head=True for HEAD); override it and call super()._serve for the default behaviour.

URL layout served: /art/<gameId>/<name> and /art/<gameId>/fallback/<name>, where <name>
is a key of ART_SET (the same names the fixture catalogue uses). Unknown names give
404, so a typo in the fixture shows up as a missing picture.

ART_SET[name] = (what it is, the outcome PORT-SPEC 1.7 expects when it is a candidate).
Python 3, standard library + PIL only; run with python3 -I.
"""
import hashlib
import io
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlsplit

from PIL import Image, ImageDraw

__all__ = ["ART_SET", "MIME", "make", "describe", "write_art_set", "ArtServer",
           "COVER_PIXEL_CAP", "HERO_PIXEL_CAP", "MAX_SIDE", "MAX_FETCH_BYTES"]

COVER_PIXEL_CAP = 1048576        # PNG/JPEG covers (PORT-SPEC 1.7)
HERO_PIXEL_CAP = 3686400         # PNG/JPEG heroes, 2560x1440
MAX_SIDE = 8192                  # the app refuses any side above this (APPO/image.cpp)
MAX_FETCH_BYTES = 12 * 1024 * 1024
MIME = {"webp": "image/webp", "png": "image/png", "jpeg": "image/jpeg", "avif": "image/avif", "gif": "image/gif"}

# Gold / blue palette of the TV theme, for recognisable test pictures.
_PALETTE = [(212, 175, 55), (240, 200, 90), (24, 38, 74), (46, 84, 160), (120, 150, 210), (16, 18, 24)]


def _colours(seed):
    h = hashlib.sha256(seed.encode("utf-8")).digest()
    a, b = _PALETTE[h[0] % len(_PALETTE)], _PALETTE[h[1] % len(_PALETTE)]
    if a == b:
        b = _PALETTE[(h[1] + 1) % len(_PALETTE)]
    return a, b, h


def _picture(w, h, seed, mode="RGB"):
    """A cheap, seed-dependent picture: two palette colours, bands and a mark."""
    a, b, d = _colours(seed)
    im = Image.new("RGB", (w, h), a)
    dr = ImageDraw.Draw(im)
    bands = 6
    for i in range(bands):
        if i % 2:
            dr.rectangle([0, i * h // bands, w, (i + 1) * h // bands], fill=b)
    x, y = d[2] * w // 256, d[3] * h // 256
    dr.rectangle([x // 2, y // 2, x // 2 + max(1, w // 5), y // 2 + max(1, h // 5)], fill=(d[4], d[5], d[6]))
    if mode == "RGBA":
        im = im.convert("RGBA")
        im.putalpha(200)
    elif mode != "RGB":
        im = im.convert(mode)
    return im


def _save(im, fmt, **kw):
    buf = io.BytesIO()
    im.save(buf, fmt, **kw)
    return buf.getvalue()


def _webp(w, h, seed, **kw):
    return _save(_picture(w, h, seed), "WEBP", quality=70, **kw)


def _anim_webp(w, h, seed):
    frames = [_picture(w, h, seed + str(i)) for i in range(3)]
    return _save(frames[0], "WEBP", save_all=True, append_images=frames[1:], duration=120, loop=0)


def _too_big(seed):
    head = _webp(64, 64, seed)
    return head + b"\0" * (MAX_FETCH_BYTES + 1 - len(head))


# name: (description, expected outcome as a candidate, builder(seed) -> bytes)
_BUILDERS = {
    "cover.webp": ("512x512 lossy WebP (VP8)", "served", lambda s: _webp(512, 512, s)),
    "cover-lossless.webp": ("512x512 lossless WebP (VP8L)", "served", lambda s: _webp(512, 512, s, lossless=True)),
    "cover-alpha.webp": ("512x512 WebP with alpha (VP8X without the animation bit)", "served",
                         lambda s: _save(_picture(512, 512, s, "RGBA"), "WEBP", quality=70)),
    "hero.webp": ("1920x1080 lossy WebP", "served", lambda s: _webp(1920, 1080, s)),
    "cover.png": ("512x512 PNG", "served", lambda s: _save(_picture(512, 512, s), "PNG")),
    "hero.png": ("1920x1080 PNG (2,073,600 px, under the hero cap)", "served",
                 lambda s: _save(_picture(1920, 1080, s), "PNG")),
    "cover.jpg": ("512x512 baseline JPEG (SOF0, 3 components)", "served",
                  lambda s: _save(_picture(512, 512, s), "JPEG", quality=80)),
    "hero.jpg": ("1920x1080 progressive JPEG (SOF2)", "served",
                 lambda s: _save(_picture(1920, 1080, s), "JPEG", quality=80, progressive=True)),
    "cover-gray.jpg": ("512x512 greyscale JPEG (1 component)", "served",
                       lambda s: _save(_picture(512, 512, s, "L"), "JPEG", quality=80)),
    "max-1024.png": ("1024x1024 PNG: exactly the cover cap of 1,048,576 px", "served",
                     lambda s: _save(_picture(1024, 1024, s), "PNG")),
    "hero-2560.png": ("2560x1440 PNG: exactly the hero cap of 3,686,400 px", "served",
                      lambda s: _save(_picture(2560, 1440, s), "PNG")),
    "hero-ratio-140.webp": ("1400x1000 WebP: w == 1.4*h, wide enough for a hero", "served",
                            lambda s: _webp(1400, 1000, s)),
    "shot.webp": ("1920x1080 lossy WebP screenshot", "served", lambda s: _webp(1920, 1080, s)),
    "shot.jpg": ("1280x720 baseline JPEG screenshot", "served",
                 lambda s: _save(_picture(1280, 720, s), "JPEG", quality=80)),
    "shot-tall.webp": ("720x1280 WebP: a tall screenshot (only a hero must be wide)", "served",
                       lambda s: _webp(720, 1280, s)),
    "cover.avif": ("512x512 AVIF (never requested: .avif URLs are skipped)", "skipped",
                   lambda s: _save(_picture(512, 512, s), "AVIF")),
    "cover.gif": ("512x512 GIF (skipped by URL, and not decodable by the app)", "skipped",
                  lambda s: _save(_picture(512, 512, s), "GIF")),
    "anim.webp": ("512x512 animated WebP (VP8X with the animation bit)", "refused",
                  lambda s: _anim_webp(512, 512, s)),
    "huge-9000.png": ("9000x100 PNG: a side above 8192", "refused",
                      lambda s: _save(_picture(9000, 100, s), "PNG")),
    "big-2000.png": ("2000x2000 PNG: 4,000,000 px, over the cover cap (and square for a hero)", "refused",
                     lambda s: _save(_picture(2000, 2000, s), "PNG")),
    "hero-2561.png": ("2561x1441 PNG: just over the hero cap", "refused",
                      lambda s: _save(_picture(2561, 1441, s), "PNG")),
    "cmyk.jpg": ("512x512 CMYK JPEG (4 components: stb cannot decode it)", "refused",
                 lambda s: _save(_picture(512, 512, s, "CMYK"), "JPEG", quality=80)),
    "hero-square.webp": ("1080x1080 WebP as a hero: not wide (w < 1.4*h)", "refused as hero",
                         lambda s: _webp(1080, 1080, s)),
    "hero-square.png": ("1080x1080 PNG as a hero: not wide", "refused as hero",
                        lambda s: _save(_picture(1080, 1080, s), "PNG")),
    "hero-ratio-139.webp": ("1390x1000 WebP: just under 1.4, not wide enough for a hero", "refused as hero",
                            lambda s: _webp(1390, 1000, s)),
    "not-image.webp": ("text bytes behind a .webp name: no WebP/PNG/JPEG magic", "refused",
                       lambda s: b"<html><body>not an image " + s.encode("utf-8") + b"</body></html>"),
    "empty.png": ("0 bytes", "refused", lambda s: b""),
    "truncated.png": ("the first 100 bytes of a PNG", "refused",
                      lambda s: _save(_picture(512, 512, s), "PNG")[:100]),
    "too-big.webp": ("a WebP padded past the 12 MiB fetch limit", "refused", _too_big),
}

ART_SET = {name: (desc, outcome) for name, (desc, outcome, _) in _BUILDERS.items()}
_CACHE = {}
_CACHE_MU = threading.Lock()


def make(name, seed=""):
    """The bytes of ART_SET[name] for `seed` (deterministic within a process; cached)."""
    if name not in _BUILDERS:
        raise KeyError(f"{name!r} is not in ART_SET")
    key = (name, seed)
    with _CACHE_MU:
        if key in _CACHE:
            return _CACHE[key]
    data = _BUILDERS[name][2](seed or name)
    with _CACHE_MU:
        _CACHE[key] = data
    return data


def content_type(name):
    ext = name.rsplit(".", 1)[-1].lower()
    return {"webp": MIME["webp"], "png": MIME["png"], "jpg": MIME["jpeg"], "jpeg": MIME["jpeg"],
            "avif": MIME["avif"], "gif": MIME["gif"]}.get(ext, "application/octet-stream")


def _jpeg_sof(data):
    """(marker, precision, components) of the first SOFn segment, or None."""
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


def describe(data):
    """What a picture is, read the way the backend's sniff/probe reads it:
    {"format": webp|png|jpeg|avif|gif|None, "width", "height", "mode", "animated",
     "webp_chunk": "VP8 "|"VP8L"|"VP8X"|None, "sof": (marker, precision, components)|None}."""
    out = {"format": None, "width": 0, "height": 0, "mode": "", "animated": False, "webp_chunk": None, "sof": None}
    if len(data) >= 12 and data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        out["format"] = "webp"
        out["webp_chunk"] = data[12:16].decode("latin-1")
        out["animated"] = out["webp_chunk"] == "VP8X" and len(data) > 20 and bool(data[20] & 0x02)
    elif data[:8] == b"\x89PNG\r\n\x1a\n":
        out["format"] = "png"
    elif data[:3] == b"\xff\xd8\xff":
        out["format"] = "jpeg"
        out["sof"] = _jpeg_sof(data)
    elif data[4:12] == b"ftypavif":
        out["format"] = "avif"
    elif data[:6] in (b"GIF87a", b"GIF89a"):
        out["format"] = "gif"
    try:
        with Image.open(io.BytesIO(data)) as im:
            out["width"], out["height"], out["mode"] = im.width, im.height, im.mode
            out["animated"] = out["animated"] or bool(getattr(im, "is_animated", False))
    except Exception:
        pass
    return out


def write_art_set(directory, seed=""):
    """Writes every ART_SET entry into `directory` (created); returns {name: path}."""
    os.makedirs(directory, exist_ok=True)
    out = {}
    for name in _BUILDERS:
        p = os.path.join(directory, name)
        with open(p, "wb") as f:
            f.write(make(name, seed or name))
        out[name] = p
    return out


def _path_of(url_or_path):
    p = urlsplit(url_or_path).path if "://" in url_or_path else url_or_path.split("?", 1)[0]
    return unquote(p)


class ArtServer:
    """A threaded HTTP server for /art/<gameId>/[fallback/]<name>, with per-path
    overrides. One instance per test class is the usual pattern."""

    def __init__(self, host="127.0.0.1", port=0):
        self._host, self._port = host, port
        self._mu = threading.Lock()
        self._status = {}          # path -> [code, remaining or None]
        self._delay = {}           # path -> seconds
        self._bytes = {}           # path -> (data, content type)
        self._redirect = {}        # path -> (code, location)
        self._hits = {}
        self.requests = []
        self.httpd = None

    # ---- lifecycle --------------------------------------------------------
    def start(self):
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, fmt, *args):
                pass

            def do_GET(self):
                owner._serve(self, head=False)

            def do_HEAD(self):
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

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()

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

    # ---- overrides --------------------------------------------------------
    def set_status(self, path, code, times=None):
        """Answer `code` (with a short text body) for `path`; `times` limits it to the
        next N requests, after which the picture is served again."""
        with self._mu:
            self._status[_path_of(path)] = [int(code), times]

    def set_delay(self, path, seconds):
        with self._mu:
            self._delay[_path_of(path)] = float(seconds)

    def set_bytes(self, path, data, ctype="application/octet-stream"):
        with self._mu:
            self._bytes[_path_of(path)] = (bytes(data), ctype)

    def set_redirect(self, path, location, code=302):
        """Answer `code` with `Location: location` for `path` (an absolute URL or a path)."""
        with self._mu:
            self._redirect[_path_of(path)] = (int(code), location)

    def clear(self, path=None):
        with self._mu:
            if path is None:
                self._status.clear(), self._delay.clear(), self._bytes.clear(), self._redirect.clear()
            else:
                p = _path_of(path)
                self._status.pop(p, None), self._delay.pop(p, None), self._bytes.pop(p, None)
                self._redirect.pop(p, None)

    def reset(self):
        """Clears overrides, counters and the request log."""
        self.clear()
        with self._mu:
            self._hits.clear()
            self.requests.clear()

    def hits(self, path=None):
        with self._mu:
            if path is None:
                return dict(self._hits)
            return self._hits.get(_path_of(path), 0)

    # ---- content ----------------------------------------------------------
    @staticmethod
    def _split(path):
        """'/art/<gid>/[fallback/]<name>' -> (gid, name) or None."""
        parts = path.split("/")
        if len(parts) == 4 and parts[1] == "art":
            return parts[2], parts[3]
        if len(parts) == 5 and parts[1] == "art" and parts[3] == "fallback":
            return parts[2], parts[4]
        return None

    def bytes_for(self, path):
        """(data, content type) served with 200 for `path`, or None (404)."""
        p = _path_of(path)
        with self._mu:
            if p in self._bytes:
                return self._bytes[p]
        sp = self._split(p)
        if not sp or sp[1] not in _BUILDERS:
            return None
        return make(sp[1], seed=p), content_type(sp[1])

    def _serve(self, h, head):
        p = _path_of(h.path)
        with self._mu:
            self._hits[p] = self._hits.get(p, 0) + 1
            self.requests.append((p, {k: v for k, v in h.headers.items()}))
            delay = self._delay.get(p, 0.0)
            red = self._redirect.get(p)
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
        if red and code is None:
            try:
                h.send_response(red[0])
                h.send_header("Location", red[1])
                h.send_header("Content-Length", "0")
                h.send_header("Connection", "close")
                h.end_headers()
            except (BrokenPipeError, ConnectionResetError):
                pass
            h.close_connection = True
            return
        if code is None:
            got = self.bytes_for(p)
            if got is None:
                code = 404
        if code is not None and code != 200:
            body = f"status {code}\n".encode()
            ctype = "text/plain"
        else:
            body, ctype = got if code is None else self.bytes_for(p) or (b"", "application/octet-stream")
            code = 200
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


if __name__ == "__main__":
    import sys
    if len(sys.argv) > 1:
        for name, path in write_art_set(sys.argv[1]).items():
            d = describe(open(path, "rb").read())
            print(f"{name:22} {os.path.getsize(path):>9}  {d['format']} {d['width']}x{d['height']} {ART_SET[name][1]}")
    else:
        print(__doc__)
