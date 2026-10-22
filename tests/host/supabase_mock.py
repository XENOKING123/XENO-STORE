#!/usr/bin/env python3
"""
supabase_mock.py — a local stand-in for the XenoKing site's Supabase project
(the Save Data Store's live backend), for the host tests. It answers only
what the site answers, the way PostgREST / Storage / Edge Functions do:

  * PostgREST on public.ps5_save_collection with the site's EXACT columns
    (id, title_id, game, shared_by, notes, file_path, file_name, file_size,
    created_at, cover_path, garlic_id):
      GET  /rest/v1/ps5_save_collection?select=*&order=created_at.desc&limit=N
      POST /rest/v1/ps5_save_collection   (anon INSERT; an unknown column is
           400 PGRST204, a missing NOT NULL column 400 23502, a wrong type
           400 22P02; Prefer: return=representation → 201 + the rows)
    Anything else under /rest/v1 (other tables, PATCH/DELETE, rpc/*) is
    refused like the site would (404 PGRST205 / PGRST202, 405) and logged.
  * Storage, private bucket ps5-saves:
      POST /storage/v1/object/ps5-saves/<path>        upload (x-upsert:false → 409 on an existing object)
      POST /storage/v1/object/sign/ps5-saves/<path>   {"expiresIn":N} → {"signedURL":"/object/sign/…?token=…"}
      POST /storage/v1/object/sign/ps5-saves          {"expiresIn":N,"paths":[…]} → [{path,signedURL,error}]
      GET  /storage/v1/object/sign/ps5-saves/<path>?token=…   the object (no key needed)
      GET  /storage/v1/object/ps5-saves/<path>        the object, with the key
  * POST /functions/v1/garlic-savedb {"action":"download","id":N} → the zip
    (application/zip), or a JSON error when Garlic has no such save.

Every request lands in self.log (method, path, query, lower-cased headers,
body, Host) so a test can assert what a console sent — and what it never
did. self.fail[...] makes one kind of call fail: "list", "insert", "upload",
"sign", "garlic" (HTTP status + JSON), "garlic200" (HTTP 200 with a JSON
error, as an edge function that catches its own error does), "garlic_url"
(JSON {"url": …} pointing elsewhere) and "list_redirect" (a 302 elsewhere).
self.list_delay (seconds, default 0) holds every table GET that long before it
answers: a slow site, for the store's "building" listing.
The apikey and Authorization: Bearer headers must carry the anon key on
every call except a signed GET.

template=True turns it into a deployment of backend/supabase/ instead (the
columns migration 0004 adds — category, game_version, region, tags,
downloads, status, source, sha256 — with their defaults; 0004's insert policy
status = 'pending' AND source = 'xeno' AND garlic_id IS NULL, 403 / 42501
otherwise; the increment_downloads(id) RPC). upload_patterns, when given, are
the object-name regexes the bucket's insert policy allows (any other upload
is refused with 403, as row-level security does).
"""
import datetime
import hashlib
import http.server
import json
import re
import threading
import time
import urllib.parse
import uuid

COLUMNS = {"id": "uuid", "title_id": "text", "game": "text", "shared_by": "text", "notes": "text", "file_path": "text",
           "file_name": "text", "file_size": "bigint", "created_at": "timestamptz", "cover_path": "text", "garlic_id": "integer"}
NOT_NULL = ("title_id", "game", "shared_by", "file_path", "file_name")
TABLE = "ps5_save_collection"
# backend/supabase/migrations/0004_xeno_save_store.sql
TEMPLATE_COLUMNS = {"category": "text", "game_version": "text", "region": "text", "tags": "text[]", "downloads": "integer",
                    "status": "text", "source": "text", "sha256": "text"}
TEMPLATE_DEFAULTS = {"tags": [], "downloads": 0, "status": "published", "source": "xeno"}


def _now_iso():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="microseconds")


class SupabaseMock:
    def __init__(self, anon_key, bucket="ps5-saves", function="garlic-savedb", template=False, upload_patterns=None):
        self.anon_key = anon_key
        self.bucket = bucket
        self.function = function
        self.template = template
        self.columns = dict(COLUMNS, **TEMPLATE_COLUMNS) if template else dict(COLUMNS)
        self.upload_patterns = upload_patterns
        self.rows = []
        self.objects = {}            # path -> (bytes, content_type)
        self.garlic = {}             # garlic id -> zip bytes
        self.tokens = {}             # token -> path
        self.log = []
        self.fail = {}
        self.list_delay = 0.0
        self.lock = threading.Lock()
        mock = self

        class H(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def _handle(self, method):
                n = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(n) if n else b""
                u = urllib.parse.urlsplit(self.path)
                entry = {"method": method, "path": u.path, "query": u.query, "headers": {k.lower(): v for k, v in self.headers.items()},
                         "body": body, "host": self.headers.get("Host", "")}
                with mock.lock:
                    mock.log.append(entry)
                try:
                    st, ctype, out, extra = mock.route(method, u.path, urllib.parse.parse_qs(u.query), entry["headers"], body)
                except Exception as e:  # a bug in the mock must not look like a console bug
                    st, ctype, out, extra = 500, "application/json", json.dumps({"message": "mock error: %s" % e}).encode(), {}
                entry["status"] = st
                self.send_response(st)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(out)))
                for k, v in (extra or {}).items():
                    self.send_header(k, v)
                self.end_headers()
                if method != "HEAD":
                    self.wfile.write(out)

            def do_GET(self):
                self._handle("GET")

            def do_HEAD(self):
                self._handle("HEAD")

            def do_POST(self):
                self._handle("POST")

            def do_PUT(self):
                self._handle("PUT")

            def do_PATCH(self):
                self._handle("PATCH")

            def do_DELETE(self):
                self._handle("DELETE")

        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.srv.daemon_threads = True
        self.port = self.srv.server_address[1]
        self.base = "http://127.0.0.1:%d" % self.port

    # ---- lifecycle ---------------------------------------------------------
    def start(self):
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        return self

    def stop(self):
        self.srv.shutdown()
        self.srv.server_close()

    # ---- fixtures ----------------------------------------------------------
    def add_row(self, **row):
        r = {k: None for k in self.columns}
        r["id"] = str(uuid.uuid4())
        r["created_at"] = _now_iso()
        if self.template:
            r.update({k: (list(v) if isinstance(v, list) else v) for k, v in TEMPLATE_DEFAULTS.items()})
        r.update(row)
        self.rows.append(r)
        return r

    def put_object(self, path, data, ctype="application/zip"):
        self.objects[path] = (data, ctype)

    def requests(self, method=None, prefix=""):
        with self.lock:
            return [e for e in self.log if (method is None or e["method"] == method) and e["path"].startswith(prefix)]

    # ---- helpers -----------------------------------------------------------
    @staticmethod
    def _json(st, obj, extra=None):
        return st, "application/json; charset=utf-8", json.dumps(obj).encode(), extra or {}

    def _authorized(self, h):
        return h.get("apikey") == self.anon_key and h.get("authorization") == "Bearer " + self.anon_key

    def _failure(self, key):
        f = self.fail.get(key)
        if not f:
            return None
        st, obj = f
        return self._json(st, obj)

    def _check_row(self, row):
        if not isinstance(row, dict):
            return self._json(400, {"code": "PGRST102", "details": None, "hint": None, "message": "All object keys must match"})
        for k in row:
            if k not in self.columns:
                return self._json(400, {"code": "PGRST204", "details": None, "hint": None,
                                        "message": "Could not find the '%s' column of '%s' in the schema cache" % (k, TABLE)})
        for k in NOT_NULL:
            if row.get(k) is None:
                return self._json(400, {"code": "23502", "details": None, "hint": None,
                                        "message": 'null value in column "%s" of relation "%s" violates not-null constraint' % (k, TABLE)})
        for k, v in row.items():
            t = self.columns[k]
            if v is None:
                continue
            if t == "text[]" and not (isinstance(v, list) and all(isinstance(x, str) for x in v)):
                return self._json(400, {"code": "22P02", "details": None, "hint": None, "message": "malformed array literal for %s" % k})
            if t in ("bigint", "integer") and (isinstance(v, bool) or not isinstance(v, int)):
                return self._json(400, {"code": "22P02", "details": None, "hint": None, "message": 'invalid input syntax for type %s: "%s"' % (t, v)})
            if t in ("text", "uuid", "timestamptz") and not isinstance(v, str):
                return self._json(400, {"code": "22P02", "details": None, "hint": None, "message": "invalid input for %s" % k})
        return None

    # ---- routes ------------------------------------------------------------
    def route(self, method, path, q, h, body):
        signed_get = method in ("GET", "HEAD") and path.startswith("/storage/v1/object/sign/")
        if not signed_get and not self._authorized(h):
            return self._json(401, {"message": "Invalid API key", "hint": "Double check your Supabase `anon` or `service_role` API key."})
        if path.startswith("/rest/v1/"):
            return self._rest(method, path[len("/rest/v1/"):], q, h, body)
        if path.startswith("/storage/v1/"):
            return self._storage(method, path[len("/storage/v1/"):], q, h, body)
        if path.startswith("/functions/v1/"):
            return self._function(method, path[len("/functions/v1/"):], h, body)
        return self._json(404, {"message": "no route"})

    def _rest(self, method, rel, q, h, body):
        if self.template and rel == "rpc/increment_downloads" and method == "POST":
            doc = json.loads(body or b"{}")
            with self.lock:
                for r in self.rows:
                    if r["id"] == doc.get("id") and r.get("status") == "published":
                        r["downloads"] = (r.get("downloads") or 0) + 1
                        return self._json(200, r["downloads"])
            return self._json(200, None)
        if rel.startswith("rpc/"):
            return self._json(404, {"code": "PGRST202", "details": None, "hint": None,
                                    "message": "Could not find the function public.%s without parameters in the schema cache" % rel[4:]})
        if rel != TABLE:
            return self._json(404, {"code": "PGRST205", "details": None, "hint": None, "message": "Could not find the table 'public.%s' in the schema cache" % rel})
        if method == "GET":
            if self.list_delay > 0:
                time.sleep(self.list_delay)
            if "list_redirect" in self.fail:
                return 302, "text/plain", b"", {"Location": self.fail["list_redirect"]}
            f = self._failure("list")
            if f:
                return f
            rows = list(self.rows)
            order = (q.get("order") or ["created_at.desc"])[0]
            col, _, direction = order.partition(".")
            if col in self.columns:
                rows.sort(key=lambda r: (r.get(col) is None, r.get(col) or ""), reverse=direction == "desc")
            for k, v in q.items():
                if k in self.columns and v[0].startswith("eq."):
                    rows = [r for r in rows if str(r.get(k)) == v[0][3:]]
            sel = (q.get("select") or ["*"])[0]
            if sel != "*":
                cols = sel.split(",")
                unknown = [c for c in cols if c not in self.columns]
                if unknown:
                    return self._json(400, {"code": "42703", "details": None, "hint": None, "message": "column %s.%s does not exist" % (TABLE, unknown[0])})
                rows = [{c: r.get(c) for c in cols} for r in rows]
            limit = int((q.get("limit") or ["1000"])[0])
            return self._json(200, rows[:min(limit, 1000)])     # the project's max-rows is 1000
        if method == "POST":
            f = self._failure("insert")
            if f:
                return f
            try:
                doc = json.loads(body or b"null")
            except ValueError:
                return self._json(400, {"code": "PGRST102", "details": None, "hint": None, "message": "Empty or invalid json"})
            items = doc if isinstance(doc, list) else [doc]
            for it in items:
                bad = self._check_row(it)
                if bad:
                    return bad
                if self.template:      # 0004: "Consoles submit pending xeno saves" (after the column defaults)
                    full = dict(TEMPLATE_DEFAULTS, **it)
                    if not (full.get("status") == "pending" and full.get("source") == "xeno" and full.get("garlic_id") is None):
                        return self._json(403, {"code": "42501", "details": None, "hint": None,
                                                "message": 'new row violates row-level security policy for table "%s"' % TABLE})
            made = []
            with self.lock:
                for it in items:
                    made.append(self.add_row(**it))
            if "return=representation" in h.get("prefer", ""):
                return self._json(201, made)
            return 201, "application/json", b"", {}
        return self._json(405, {"code": "PGRST117", "details": None, "hint": None, "message": "Unsupported HTTP method: %s" % method})

    def _storage(self, method, rel, q, h, body):
        sign_prefix = "object/sign/%s" % self.bucket
        obj_prefix = "object/%s/" % self.bucket
        if rel == sign_prefix and method == "POST":         # batch sign
            f = self._failure("sign")
            if f:
                return f
            doc = json.loads(body or b"{}")
            out = []
            for p in doc.get("paths") or []:
                if p in self.objects:
                    out.append({"path": p, "signedURL": self._sign(p), "error": None})
                else:
                    out.append({"path": p, "signedURL": None, "error": "Either the object does not exist or you do not have access to it"})
            return self._json(200, out)
        if rel.startswith(sign_prefix + "/"):
            p = urllib.parse.unquote(rel[len(sign_prefix) + 1:])
            if method == "POST":
                f = self._failure("sign")
                if f:
                    return f
                doc = json.loads(body or b"{}")
                if not isinstance(doc.get("expiresIn"), int) or doc["expiresIn"] <= 0:
                    return self._json(400, {"statusCode": "400", "error": "Invalid Input", "message": "expiresIn must be a positive integer"})
                if p not in self.objects:
                    return self._json(400, {"statusCode": "404", "error": "not_found", "message": "Object not found"})
                return self._json(200, {"signedURL": self._sign(p)})
            if method in ("GET", "HEAD"):
                tok = (q.get("token") or [""])[0]
                if self.tokens.get(tok) != p or p not in self.objects:
                    return self._json(400, {"statusCode": "400", "error": "InvalidJWT", "message": "invalid signature"})
                data, ctype = self.objects[p]
                return 200, ctype, data, {}
        if rel.startswith(obj_prefix):
            p = urllib.parse.unquote(rel[len(obj_prefix):])
            if method == "POST":
                f = self._failure("upload")
                if f:
                    return f
                if self.upload_patterns is not None and not any(re.fullmatch(rx, p) for rx in self.upload_patterns):
                    return self._json(403, {"statusCode": "403", "error": "Unauthorized", "message": "new row violates row-level security policy"})
                if p in self.objects and h.get("x-upsert", "false") != "true":
                    return self._json(400, {"statusCode": "409", "error": "Duplicate", "message": "The resource already exists"})
                self.objects[p] = (body, h.get("content-type", "application/octet-stream"))
                return self._json(200, {"Key": "%s/%s" % (self.bucket, p), "Id": str(uuid.uuid4())})
            if method in ("GET", "HEAD"):
                if p not in self.objects:
                    return self._json(400, {"statusCode": "404", "error": "not_found", "message": "Object not found"})
                data, ctype = self.objects[p]
                return 200, ctype, data, {}
            return self._json(405, {"statusCode": "405", "error": "Method not allowed", "message": method})
        return self._json(404, {"statusCode": "404", "error": "not_found", "message": "no such storage route"})

    def _sign(self, p):
        tok = hashlib.sha256(("%s:%s" % (p, uuid.uuid4())).encode()).hexdigest()
        self.tokens[tok] = p
        return "/object/sign/%s/%s?token=%s" % (self.bucket, urllib.parse.quote(p), tok)

    def _function(self, method, name, h, body):
        if name != self.function or method != "POST":
            return self._json(404, {"code": "NOT_FOUND", "message": "Requested function was not found"})
        try:
            doc = json.loads(body or b"{}")
        except ValueError:
            doc = {}
        action = doc.get("action", "list")
        if action != "download":
            return self._json(400, {"error": "unsupported action %s" % action})
        f = self._failure("garlic")
        if f:
            return f
        if "garlic200" in self.fail:
            return self._json(200, {"error": self.fail["garlic200"]})
        if "garlic_url" in self.fail:
            return self._json(200, {"url": self.fail["garlic_url"]})
        gid = doc.get("id")
        if not isinstance(gid, int) or gid <= 0:
            return self._json(400, {"error": "bad id"})
        data = self.garlic.get(gid)
        if data is None:
            return self._json(500, {"error": "Garlic download 404"})
        return 200, "application/zip", data, {"Content-Disposition": 'attachment; filename="garlic_%d.zip"' % gid}
