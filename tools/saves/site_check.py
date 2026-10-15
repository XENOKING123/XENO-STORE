#!/usr/bin/env python3
"""
site_check.py — one-shot live check of the Save Data Store's backend, the
XenoKing site, as consoles use it (docs/SAVES.md §9.1). Read only: it lists
the community collection and downloads ONE small Garlic save, nothing else.

  1. reads the backend JSON (default updates/saves-backend.json): kind
     "supabase", an https url, the public anon key;
  2. GET {url}/rest/v1/ps5_save_collection?select=*&order=created_at.desc&limit=1000
     → at least --min-rows rows, every row carrying the columns consoles rely on
     (id, title_id, game, shared_by, notes, file_path, file_name, file_size,
     created_at, cover_path, garlic_id);
  3. POST {url}/functions/v1/<garlicFunction> {"action":"download","id":<--garlic-id>}
     → a zip (at most --max-bytes) holding sce_sys/param.sfo.

The anon key goes to the backend URL only, and redirects are not followed.
Stdlib only. Exit 0 when every check passes, 1 otherwise (2: bad arguments).

  python3 -I tools/saves/site_check.py                      # the published backend
  python3 -I tools/saves/site_check.py --backend other.json --garlic-id 94 --min-rows 50
"""
import argparse
import io
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
COLUMNS = ("id", "title_id", "game", "shared_by", "notes", "file_path", "file_name", "file_size", "created_at", "cover_path", "garlic_id")
NOT_NULL = ("id", "title_id", "game", "shared_by", "file_path", "file_name", "created_at")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    """The anon key must not follow a redirect to another host."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def opener():
    return urllib.request.build_opener(NoRedirect())


def request(url, key, body=None, accept="application/json", timeout=60, max_bytes=8 << 20):
    hdrs = {"apikey": key, "Authorization": "Bearer " + key, "Accept": accept, "User-Agent": "XENO-STORE-site-check"}
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        hdrs["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, method="POST" if data is not None else "GET", headers=hdrs)
    try:
        with opener().open(req, timeout=timeout) as r:
            out = r.read(max_bytes + 1)
            return r.status, r.headers.get("Content-Type", ""), out
    except urllib.error.HTTPError as e:
        return e.code, e.headers.get("Content-Type", "") if e.headers else "", e.read(64 << 10)


def load_backend(path, allow_http):
    with open(path, encoding="utf-8") as f:
        b = json.load(f)
    problems = []
    if b.get("kind") != "supabase":
        problems.append('kind is %r, not "supabase"' % b.get("kind"))
    u = urllib.parse.urlsplit(str(b.get("url", "")))
    if u.scheme != "https" and not (allow_http and u.scheme == "http" and u.hostname in ("127.0.0.1", "localhost")):
        problems.append("url must be https://<ref>.supabase.co")
    if not u.hostname or u.username or u.password:
        problems.append("url has no host (or carries credentials)")
    if not str(b.get("anonKey", "")).strip():
        problems.append("anonKey is empty")
    return b, problems


def check(args):
    b, problems = load_backend(args.backend, args.allow_http)
    if problems:
        for p in problems:
            print("FAIL backend: " + p)
        return 1
    base = b["url"].rstrip("/")
    key = b["anonKey"].strip()
    fn = b.get("garlicFunction") or "garlic-savedb"
    name = b.get("name") or urllib.parse.urlsplit(base).hostname
    ok = True
    print("site: %s (%s)" % (name, base))

    # 1. the collection
    st, ct, raw = request(base + "/rest/v1/ps5_save_collection?select=*&order=created_at.desc&limit=1000", key, timeout=args.timeout)
    rows = None
    if st != 200:
        print("FAIL list: HTTP %d %s" % (st, raw[:200].decode("utf-8", "replace")))
        ok = False
    else:
        try:
            rows = json.loads(raw)
        except ValueError:
            print("FAIL list: not JSON (%s)" % ct)
            ok = False
    if rows is not None:
        if not isinstance(rows, list):
            print("FAIL list: not an array")
            ok = False
            rows = []
        print("list: %d rows" % len(rows))
        if len(rows) < args.min_rows:
            print("FAIL list: expected at least %d rows" % args.min_rows)
            ok = False
        bad = 0
        for r in rows:
            missing = [c for c in COLUMNS if not isinstance(r, dict) or c not in r]
            empty = [c for c in NOT_NULL if isinstance(r, dict) and c in r and r[c] in (None, "")]
            if missing or empty:
                bad += 1
                if bad <= 5:
                    print("FAIL row %s: missing %s, empty %s" % (r.get("id") if isinstance(r, dict) else "?", missing, empty))
        if bad:
            ok = False
        garlic = sum(1 for r in rows if isinstance(r, dict) and str(r.get("file_path", "")).startswith("garlic:"))
        sharers = len({r.get("shared_by") for r in rows if isinstance(r, dict)})
        print("list: %d Garlic rows, %d bucket rows, %d sharers" % (garlic, len(rows) - garlic, sharers))
        if not any(isinstance(r, dict) and r.get("garlic_id") == args.garlic_id for r in rows):
            print("WARN list: no row with garlic_id %d (downloading it anyway)" % args.garlic_id)

    # 2. one small Garlic save through the function
    st, ct, raw = request("%s/functions/v1/%s" % (base, fn), key, body={"action": "download", "id": args.garlic_id},
                          accept="application/zip, application/json", timeout=args.timeout, max_bytes=args.max_bytes)
    if st != 200:
        print("FAIL download %d: HTTP %d %s" % (args.garlic_id, st, raw[:200].decode("utf-8", "replace")))
        ok = False
    elif len(raw) > args.max_bytes:
        print("FAIL download %d: larger than %d bytes" % (args.garlic_id, args.max_bytes))
        ok = False
    elif "zip" not in ct.lower() and not raw.startswith(b"PK"):
        print("FAIL download %d: not a zip (%s) %s" % (args.garlic_id, ct, raw[:200].decode("utf-8", "replace")))
        ok = False
    else:
        try:
            with zipfile.ZipFile(io.BytesIO(raw)) as z:
                names = z.namelist()
                bad = z.testzip()
        except zipfile.BadZipFile as e:
            print("FAIL download %d: damaged zip (%s)" % (args.garlic_id, e))
            return 1
        sfo = [n for n in names if n == "sce_sys/param.sfo" or n.endswith("/sce_sys/param.sfo")]
        print("download %d: %d bytes, %d entries%s" % (args.garlic_id, len(raw), len(names), (", param.sfo at " + sfo[0]) if sfo else ""))
        if bad:
            print("FAIL download %d: CRC error in %s" % (args.garlic_id, bad))
            ok = False
        if not sfo:
            print("FAIL download %d: no sce_sys/param.sfo in the zip" % args.garlic_id)
            ok = False
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description="Read-only live check of the Save Data Store backend (the XenoKing site).")
    ap.add_argument("--backend", default=os.path.join(ROOT, "updates", "saves-backend.json"), help="backend JSON (default: updates/saves-backend.json)")
    ap.add_argument("--garlic-id", type=int, default=94, help="Garlic save to download (default 94, about 3 KB)")
    ap.add_argument("--min-rows", type=int, default=50, help="minimum number of rows expected (default 50)")
    ap.add_argument("--max-bytes", type=int, default=4 << 20, help="largest download accepted (default 4 MiB)")
    ap.add_argument("--timeout", type=int, default=90, help="seconds per request (default 90)")
    ap.add_argument("--allow-http", action="store_true", help=argparse.SUPPRESS)   # a loopback stand-in in the tests
    args = ap.parse_args(argv)
    if args.garlic_id <= 0 or args.min_rows < 0 or args.max_bytes <= 0:
        ap.error("--garlic-id, --min-rows and --max-bytes must be positive")
    try:
        return check(args)
    except (OSError, ValueError) as e:
        print("FAIL %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
