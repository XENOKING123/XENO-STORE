#!/usr/bin/env python3
"""
garlic_mock.py — a stand-in for Garlic Save Manager (earthonion/garlic-savemgr)
for the host tests, following its src/main.c route by route:

  * every connection is read with ONE recv() of at most 8191 bytes
    (request_thread, main.c:1466); method and URL come from that buffer
    (sscanf "%7s %2047s"), and the upload handler finds Content-Length and the
    blank line only there (main.c:3223-3229) — a request whose headers arrive
    in several pieces fails with {"error":"Bad request"};
  * OPTIONS → 204, non-/api paths → 404 text; /api/download_file runs without
    the lock, everything else under one global mutex (g_mtx);
  * failures are HTTP 200 with {"error": ...}; unknown /api routes answer 404
    with the 20-byte body Garlic really sends ('{"error":"Not found"'
    — main.c:5466 passes length 20 for a 21-character string);
  * /api/status, /api/create_pfs?size= (25 % + 4 MiB, min 32 MiB, 32 KiB
    rounding), POST /api/upload_file?name= (name decoded across the rest of the
    URL, parents created), /api/import_check?uid=, /api/import_finish?uid=
    [&force=1&title_id=&dir_name=] (parse_sfo: -2 when the first 256 bytes are
    zero → {"sfo_zeroed":true,"db_found":false}; re-sign writes the account id
    at 0x1B8 and the user id at 0x660; unmounts, then "copies" the image),
    /api/unmount, /api/saves, /api/mount?idx=, /api/download_file?name=,
    /api/export_title_dec?id= (zip "<TID>/<save_name>/<rel>" of every save of
    the title, stored entries, no Content-Length, connection closed).

"Installed" saves live in self.saves; an import also drops an image marker
below home_root/<uid>/savedata_prospero/<TID>/sdimg_<dir> so the store's own
save listing sees it, as it would on the console.
"""
import io
import json
import os
import shutil
import socket
import struct
import tempfile
import threading
import warnings
import zipfile

MAX_REQ = 8192
NOT_FOUND_20 = b'{"error":"Not found"}'[:20]


def parse_sfo(path):
    """Port of parse_sfo() (main.c:832): (ret, info)."""
    info = {"title_id": "", "dir_name": "", "main_title": "", "sub_title": "", "detail": "", "game_title_id": "",
            "user_param": 0, "blocks": 0, "account_id": 0, "user_id": 0, "format": ""}
    try:
        with open(path, "rb") as f:
            buf = f.read()
    except OSError:
        return -1, info
    if len(buf) < 20:
        return -1, info
    if all(b == 0 for b in buf[:min(len(buf), 256)]):
        return -2, info
    if buf[:4] != b"\0PSF":
        return -1, info
    key_off, data_off, n = struct.unpack("<III", buf[8:20])

    def cstr(b):
        return b.split(b"\0", 1)[0].decode("utf-8", "replace")

    for i in range(n):
        idx = buf[20 + i * 16:36 + i * 16]
        if len(idx) < 16:
            break
        koff = struct.unpack("<H", idx[0:2])[0]
        plen = struct.unpack("<I", idx[4:8])[0]
        doff = struct.unpack("<I", idx[12:16])[0]
        key = cstr(buf[key_off + koff:])
        val = buf[data_off + doff:data_off + doff + plen]
        if key == "TITLE_ID":
            info["title_id"] = cstr(val)[:15]
        elif key == "SAVEDATA_DIRECTORY":
            info["dir_name"] = cstr(val)[:63]
        elif key in ("MAINTITLE", "TITLE"):
            info["main_title"] = cstr(val)[:127]
        elif key == "SUBTITLE":
            info["sub_title"] = cstr(val)[:127]
        elif key == "DETAIL":
            info["detail"] = cstr(val)[:255]
        elif key == "SAVEDATA_BLOCKS" and plen >= 4:
            info["blocks"] = struct.unpack("<I", val[:4])[0]
        elif key in ("PARAMS", "USER_PARAM") and plen >= 4:
            info["user_param"] = struct.unpack("<I", val[:4])[0]
        elif key == "ACCOUNT_ID" and plen >= 8:
            info["account_id"] = struct.unpack("<Q", val[:8])[0]
        elif key == "USER_ID" and plen >= 4:
            info["user_id"] = struct.unpack("<I", val[:4])[0]
        elif key == "FORMAT":
            info["format"] = cstr(val)[:3]
    if not info["game_title_id"]:
        info["game_title_id"] = info["title_id"]
    return 0, info


def url_decode_all(s):
    """Garlic's loop: %XX decoded, everything else copied (no '+' handling)."""
    out = bytearray()
    i = 0
    b = s.encode("latin-1", "replace")
    while i < len(b):
        if b[i] == 0x25 and i + 2 < len(b):       # '%' && r[1] && r[2]
            v = 0
            for ch in b[i + 1:i + 3].decode("latin-1"):   # strtol(hex, NULL, 16)
                if ch not in "0123456789abcdefABCDEF":
                    break
                v = v * 16 + int(ch, 16)
            out.append(v & 0xFF)
            i += 3
        else:
            out.append(b[i])
            i += 1
    return out.decode("utf-8", "replace")


def qparam(url, key, stop_at_amp=True, maxlen=15):
    """strstr(url, "key=") then strncpy(maxlen) and cut at '&' (main.c style)."""
    p = url.find(key + "=")
    if p < 0:
        return ""
    v = url[p + len(key) + 1:][:maxlen]
    if stop_at_amp:
        v = v.split("&", 1)[0]
    return v


class GarlicMock:
    def __init__(self, port, home_root=None, foreground_uid="10000000"):
        self.port = port
        self.home_root = home_root
        self.foreground_uid = foreground_uid
        self.mtx = threading.Lock()
        self.log = []
        self.saves = {}            # (uid_hex, title_id, save_name) -> {"files": {rel: bytes}}
        self.account_ids = {}      # uid_hex -> local account id (registry)
        self.resigned = []
        self.scan = []             # last scan_saves() result, for /api/mount?idx=
        self.mounted = False
        self.mount_point = ""
        self.mounted_path = ""
        self.mounted_idx = -1
        self.enc_ps4 = False
        self.mount_dir = None
        self.fail_upload = {}      # name -> error text (answered while still mounted)
        self.created_sizes = []
        self.sock = None
        self.thread = None
        self.tmp = tempfile.mkdtemp(prefix="garlic-mock-")

    # ---- lifecycle ------------------------------------------------------
    def start(self):
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("127.0.0.1", self.port))
        s.listen(16)
        self.sock = s
        self.thread = threading.Thread(target=self._accept, daemon=True)
        self.thread.start()
        return self

    def stop(self):
        if self.sock:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.sock.close()
            self.sock = None

    def running(self):
        return self.sock is not None

    def requests(self, prefix=""):
        return [r for r in self.log if r["url"].startswith(prefix)]

    def _accept(self):
        sock = self.sock
        while True:
            try:
                c, _ = sock.accept()
            except OSError:
                return
            threading.Thread(target=self._conn, args=(c,), daemon=True).start()

    # ---- HTTP helpers (http_send / http_json) ----------------------------
    @staticmethod
    def _send(c, status, ctype, body):
        if isinstance(body, str):
            body = body.encode()
        hdr = ("HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nAccess-Control-Allow-Origin: *\r\n"
               "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\n"
               "Connection: close\r\n\r\n" % (status, ctype, len(body))).encode()
        c.sendall(hdr + body)

    def _json(self, c, obj):
        self._send(c, "200 OK", "application/json", json.dumps(obj, separators=(",", ":")))

    def _conn(self, c):
        try:
            c.settimeout(30)
            try:
                req = c.recv(MAX_REQ - 1)
            except OSError:
                return
            if not req:
                return
            head = req.split(b"\r\n", 1)[0].decode("latin-1")
            parts = head.split()
            method = (parts[0] if parts else "")[:7]
            url = (parts[1] if len(parts) > 1 else "")[:2047]
            low = req.lower()
            cl = None
            i = low.find(b"content-length:")
            if i >= 0:
                try:
                    cl = int(req[i + 15:].split(b"\r\n", 1)[0].strip() or b"0")
                except ValueError:
                    cl = 0
            entry = {"method": method, "url": url, "firstRecvBytes": len(req), "headersInFirstRecv": b"\r\n\r\n" in req,
                     "contentLength": cl}
            self.log.append(entry)
            if method == "OPTIONS":
                self._send(c, "204 No Content", "text/plain", b"")
                return
            if method == "GET" and url in ("/", "/index.html"):
                self._send(c, "200 OK", "text/html", b"<html>Garlic SaveMgr</html>")
                return
            if not url.startswith("/api/"):
                self._send(c, "404 Not Found", "text/plain", b"Not found")
                return
            if method == "GET" and url.startswith("/api/download_file"):
                self._download_file(c, url)
                return
            with self.mtx:
                self._inner(c, req, method, url, entry)
        except Exception as e:  # pragma: no cover - keeps the mock alive
            self.log.append({"method": "?", "url": "!exception", "error": repr(e)})
        finally:
            try:
                c.close()
            except OSError:
                pass

    # ---- state ------------------------------------------------------------
    def _unmount(self):
        if not self.mounted:
            return -1
        self.mounted = False
        self.mounted_idx = -1
        self.mounted_path = ""
        self.mount_point = ""
        return 0

    def _scan(self):
        self.scan = [k for k in self.saves]
        return self.scan

    def _materialize(self, files):
        d = tempfile.mkdtemp(dir=self.tmp)
        for rel, data in files.items():
            p = os.path.join(d, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as f:
                f.write(data)
        return d

    def _read_tree(self, root):
        out = {}
        for base, dirs, files in os.walk(root):
            for fn in files:
                if fn.startswith("."):
                    continue
                full = os.path.join(base, fn)
                with open(full, "rb") as f:
                    out[os.path.relpath(full, root).replace(os.sep, "/")] = f.read()
        return out

    def _safe_path(self, fname):
        p = os.path.normpath(os.path.join(self.mount_dir, fname))
        if not p.startswith(self.mount_dir + os.sep):
            raise ValueError("path escapes the mount: %r" % fname)
        return p

    # ---- routes -------------------------------------------------------------
    def _inner(self, c, req, method, url, entry):
        if method == "GET" and url == "/api/status":
            return self._json(c, {"mounted": self.mounted, "mount_point": self.mount_point, "save_path": self.mounted_path})
        if method == "GET" and url == "/api/unmount":
            return self._json(c, {"ok": self._unmount() == 0})
        if method == "POST" and url.startswith("/api/upload_file"):
            return self._upload_file(c, req, url, entry)
        if method == "GET" and url.startswith("/api/create_pfs"):
            return self._create_pfs(c, url)
        if method == "GET" and url.startswith("/api/import_check"):
            return self._import_check(c, url)
        if method == "GET" and url.startswith("/api/import_finish"):
            return self._import_finish(c, url)
        if method == "GET" and url == "/api/saves":
            return self._saves(c)
        if method == "GET" and url.startswith("/api/mount") and not url.startswith("/api/mount_copy"):
            return self._mount(c, url)
        if method == "GET" and url.startswith("/api/export_title_dec"):
            return self._export_title_dec(c, url)
        self._send(c, "404 Not Found", "application/json", NOT_FOUND_20)

    def _create_pfs(self, c, url):
        if self.mounted:
            self._unmount()
        is_ps4 = "ps4=1" in url
        self.enc_ps4 = is_ps4
        img = 256 * 1024 * 1024
        sp = url.find("size=")
        if sp >= 0:
            digits = ""
            for ch in url[sp + 5:]:
                if not ch.isdigit():
                    break
                digits += ch
            req_size = int(digits or "0")
            if req_size > 0:
                img = req_size + req_size // 4 + 4 * 1024 * 1024
                if img < 32 * 1024 * 1024:
                    img = 32 * 1024 * 1024
        img = ((img + 32767) // 32768) * 32768
        self.created_sizes.append(img)
        if self.mount_dir:
            shutil.rmtree(self.mount_dir, ignore_errors=True)
        self.mount_dir = tempfile.mkdtemp(dir=self.tmp)
        self.mount_point = "/data/mount_sd"
        self.mounted_path = "/data/save_files/_tmp_enc"
        self.mounted = True
        return self._json(c, {"ok": True})

    def _upload_file(self, c, req, url, entry):
        if not self.mounted:
            return self._json(c, {"error": "No save mounted"})
        fname = ""
        np_ = url.find("name=")
        if np_ >= 0:
            fname = url_decode_all(url[np_ + 5:][:511])
        if not fname:
            return self._json(c, {"error": "Missing name param"})
        clen = entry["contentLength"] or 0
        if clen > 2 * 1024 * 1024 * 1024:
            return self._json(c, {"error": "File too large"})
        he = req.find(b"\r\n\r\n")
        if he < 0:
            return self._json(c, {"error": "Bad request"})
        body = req[he + 4:he + 4 + clen]
        received = len(body)
        try:
            path = self._safe_path(fname)
        except ValueError:
            return self._json(c, {"error": "Cannot create %s" % fname})
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(body)
            while received < clen:
                d = c.recv(min(512 * 1024, clen - received))
                if not d:
                    break
                f.write(d)
                received += len(d)
        entry["name"] = fname
        entry["received"] = received
        if fname in self.fail_upload:
            return self._json(c, {"error": self.fail_upload[fname]})
        return self._json(c, {"ok": True, "name": fname, "size": received})

    def _uid(self, url):
        u = qparam(url, "uid")
        return u if u else self.foreground_uid

    def _import_check(self, c, url):
        if not self.mounted:
            return self._json(c, {"error": "No save mounted"})
        ret, sfo = parse_sfo(os.path.join(self.mount_dir, "sce_sys", "param.sfo"))
        if ret < 0 or not sfo["title_id"] or not sfo["dir_name"]:
            return self._json(c, {"exists": False})
        uid = self._uid(url)
        exists = (uid, sfo["title_id"], "sdimg_" + sfo["dir_name"]) in self.saves
        return self._json(c, {"exists": exists, "title_id": sfo["title_id"], "dir_name": sfo["dir_name"], "main_title": sfo["main_title"]})

    def _import_finish(self, c, url):
        if not self.mounted:
            return self._json(c, {"error": "No save mounted"})
        sfo_path = os.path.join(self.mount_dir, "sce_sys", "param.sfo")
        ret, sfo = parse_sfo(sfo_path)
        force = "force=1" in url
        if ret == -2 and not force:
            return self._json(c, {"sfo_zeroed": True, "db_found": False})   # g_mounted_idx < 0 after create_pfs
        if ret < 0 and not force:
            return self._json(c, {"error": "Cannot parse param.sfo — is sce_sys/param.sfo present?"})
        if ret < 0 and force:
            sfo["title_id"] = qparam(url, "title_id", maxlen=15)
            sfo["dir_name"] = qparam(url, "dir_name", maxlen=63)
            sfo["main_title"] = sfo["title_id"]
            sfo["blocks"] = 96
            sfo["format"] = "ppr"
        if not sfo["title_id"] or not sfo["dir_name"]:
            return self._json(c, {"error": "param.sfo missing TITLE_ID or SAVEDATA_DIRECTORY"})
        self.enc_ps4 = sfo["format"] == "obs"
        uid_hex = qparam(url, "uid")
        if not uid_hex:
            uid_hex = self.foreground_uid
        uid = int(uid_hex, 16)
        aid = self.account_ids.get(uid_hex, 0)
        if aid and os.path.exists(sfo_path):
            with open(sfo_path, "r+b") as f:       # pwrite(): extends a short file
                if self.enc_ps4:
                    f.seek(0x15C)
                    f.write(struct.pack("<Q", aid))
                else:
                    f.seek(0x1B8)
                    f.write(struct.pack("<Q", aid))
                    f.seek(0x660)
                    f.write(struct.pack("<I", uid))
            self.resigned.append({"uid": uid_hex, "aid": aid})
        files = self._read_tree(self.mount_dir)
        self._unmount()
        key = (uid_hex, sfo["title_id"], "sdimg_" + sfo["dir_name"])
        existed = key in self.saves
        self.saves[key] = {"files": files}
        if self.home_root:
            d = os.path.join(self.home_root, uid_hex, "savedata" if self.enc_ps4 else "savedata_prospero", sfo["title_id"])
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "sdimg_" + sfo["dir_name"]), "wb") as f:
                f.write(b"GARLIC-PFS-IMAGE " + json.dumps(sorted(files)).encode())
        return self._json(c, {"ok": True, "title_id": sfo["title_id"], "dir_name": sfo["dir_name"],
                              "main_title": sfo["main_title"], "exists": existed})

    def _saves(self, c):
        rows = []
        for (uid, tid, sname) in self._scan():
            rows.append({"title_id": tid, "save_name": sname, "path": "/user/home/%s/savedata_prospero/%s/%s" % (uid, tid, sname),
                         "dir": "%s_%s" % (tid, sname), "title_name": "", "type": "ps5", "backup": sname.startswith("sdimg_sce_bu_"),
                         "usb": False, "uid": uid})
        return self._json(c, {"saves": rows})

    def _list_dir(self, base, prefix=""):
        out = []
        d = os.path.join(base, prefix) if prefix else base
        for name in sorted(os.listdir(d)):
            if name.startswith("."):
                continue
            rel = prefix + "/" + name if prefix else name
            full = os.path.join(base, rel)
            isdir = os.path.isdir(full)
            out.append({"name": rel, "dir": isdir, "size": 0 if isdir else os.path.getsize(full)})
            if isdir:
                out.extend(self._list_dir(base, rel))
        return out

    def _mount(self, c, url):
        if self.mounted:
            self._unmount()
        p = url.find("idx=")
        idx = int(url[p + 4:].split("&", 1)[0] or "-1") if p >= 0 else -1
        if idx < 0 or idx >= len(self.scan) or self.scan[idx] not in self.saves:
            return self._json(c, {"error": "Invalid index"})
        key = self.scan[idx]
        self.mount_dir = self._materialize(self.saves[key]["files"])
        self.mount_point = "/data/save_mnt/%s_%s" % (key[1], key[2])
        self.mounted_path = "/user/home/%s/savedata_prospero/%s/%s" % key
        self.mounted = True
        self.mounted_idx = idx
        ret, sfo = parse_sfo(os.path.join(self.mount_dir, "sce_sys", "param.sfo"))
        acct = ""
        if ret == 0 and sfo["account_id"]:
            acct = "0x" + "".join("%02X" % b for b in struct.pack("<Q", sfo["account_id"]))
        return self._json(c, {"title_id": sfo["title_id"] or key[1], "save_name": key[2], "mount": self.mount_point,
                              "account_id": acct, "save_title": sfo["main_title"], "sfo_zeroed": ret == -2, "db_found": False,
                              "files": self._list_dir(self.mount_dir)})

    def _download_file(self, c, url):
        if not self.mounted:
            return self._json(c, {"error": "No save mounted"})
        p = url.find("name=")
        fname = url_decode_all(url[p + 5:][:511].split("&", 1)[0]) if p >= 0 else ""
        if not fname:
            return self._json(c, {"error": "Missing name"})
        try:
            path = self._safe_path(fname)
        except ValueError:
            return self._json(c, {"error": "File not found"})
        if not os.path.isfile(path):
            return self._json(c, {"error": "File not found"})
        with open(path, "rb") as f:
            data = f.read()
        hdr = ("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Disposition: attachment; filename=\"%s\"\r\n"
               "Content-Length: %d\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n" % (os.path.basename(fname), len(data)))
        c.sendall(hdr.encode() + data)

    def _export_title_dec(self, c, url):
        tid = url_decode_all(qparam(url, "id", maxlen=63))
        if not tid:
            return self._json(c, {"error": "Missing id"})
        keys = [k for k in self._scan() if k[1] == tid]
        if not keys:
            return self._json(c, {"error": "No saves found for this title"})
        if self.mounted:
            self._unmount()
        bio = io.BytesIO()
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")      # several profiles: duplicate names, as in Garlic's zip
            with zipfile.ZipFile(bio, "w", zipfile.ZIP_STORED) as z:
                for k in keys:
                    for rel, data in sorted(self.saves[k]["files"].items()):
                        z.writestr("%s/%s/%s" % (tid, k[2], rel), data)
        hdr = ("HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nContent-Disposition: attachment; filename=\"%s_saves_dec.zip\"\r\n"
               "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n" % tid)
        c.sendall(hdr.encode() + bio.getvalue())


class FakeLoader(threading.Thread):
    """ELF loader stand-in on a test port: records each payload and calls
    on_payload(bytes) once the sender closes (the payload "starts")."""

    def __init__(self, port, on_payload=None):
        super().__init__(daemon=True)
        self.port = port
        self.on_payload = on_payload
        self.received = []
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", port))
        self.sock.listen(8)
        self.start()

    def run(self):
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
            if not buf:
                continue          # a reachability probe
            self.received.append(buf)
            if self.on_payload:
                self.on_payload(buf)

    def close(self):
        self.sock.close()
