#!/usr/bin/env python3
"""Writes the intake test fixtures deterministically (same bytes every run with
the same zlib).

  python3 -I tools/saves/tests/fixtures/make_fixtures.py [OUT_DIR]

test_intake.py calls write_fixtures() into a temporary directory on every run,
so a fresh checkout needs no extra files. Without OUT_DIR this writes next
to the script (the zips are ignored by git; issue-good.md and
event-opened.json are kept as samples of what XENO STORE prefills).

Writes: good-save.zip (xeno-save-zip layout), garlic-save.zip (Garlic layout in a
folder), sealed-save.zip (contains a sealed key), raw-backup.zip (same-console
backup), issue-good.md (the body XENO STORE prefills), event-opened.json (the
Actions payload for that issue). Every param.sfo is a real PSF with ACCOUNT_ID
zeroed, the way XENO STORE exports it.
"""
import hashlib
import json
import os
import struct
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
STAMP = (2026, 1, 1, 0, 0, 0)
TITLE = "PPSA04029"


def blob(seed: bytes, n: int) -> bytes:
    out = b""
    h = seed
    while len(out) < n:
        h = hashlib.sha256(h).digest()
        out += h
    return out[:n]


def write_zip(path: str, entries, comment: bytes = b"") -> None:
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in entries:
            info = zipfile.ZipInfo(name, date_time=STAMP)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, data)
        if comment:
            z.comment = comment


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def make_sfo(account_id: int = 0, title: str = "Test save") -> bytes:
    """Minimal PSF v1.1 param.sfo: ACCOUNT_ID (8 bytes, binary) and MAINTITLE (utf-8)."""
    entries = [("ACCOUNT_ID", 0x0004, struct.pack("<Q", account_id), 8), ("MAINTITLE", 0x0204, title.encode() + b"\0", 128)]
    keys = b"".join(k.encode() + b"\0" for k, _, _, _ in entries)
    keys += b"\0" * (-len(keys) % 4)
    key_start = 20 + 16 * len(entries)
    data_start = key_start + len(keys)
    table, data, koff = b"", b"", 0
    for k, fmt, val, mx in entries:
        table += struct.pack("<HHIII", koff, fmt, len(val), mx, len(data))
        data += val + b"\0" * (mx - len(val))
        koff += len(k) + 1
    return b"\0PSF" + struct.pack("<IIII", 0x0101, key_start, data_start, len(entries)) + table + keys + data


def write_fixtures(out: str) -> dict:
    """Writes every fixture into `out`; returns {"good": bytes, "goodSha": hex}."""
    os.makedirs(out, exist_ok=True)
    save = blob(b"xeno-save-data", 4096)
    sfo = make_sfo(0)
    icon = b"\x89PNG\r\n\x1a\n" + blob(b"icon0", 200)
    files = [("SAVEDATA00.bin", save), ("sce_sys/param.sfo", sfo), ("sce_sys/icon0.png", icon)]
    manifest = {
        "titleId": TITLE, "dirName": "SAVEDATA00", "kind": "ps5", "format": "xeno-save-zip",
        "gameVersion": "01.004.000", "region": "EUR", "accountIdStripped": True, "sameConsoleOnly": False,
        "createdBy": "XENO STORE 1.0.0", "createdAt": "2026-01-01T00:00:00Z",
        "files": [{"path": n, "size": len(d), "sha256": sha(d)} for n, d in files],
    }
    good = os.path.join(out, "good-save.zip")
    write_zip(good, files + [("xeno-save.json", json.dumps(manifest, indent=1).encode())])
    write_zip(os.path.join(out, "garlic-save.zip"), [
        ("PPSA04029_save/data0.bin", blob(b"garlic", 1500)), ("PPSA04029_save/sce_sys/param.sfo", sfo), ("PPSA04029_save/sce_sys/icon0.png", icon)])
    write_zip(os.path.join(out, "sealed-save.zip"), [("SAVEDATA00.bin", save), ("sce_sys/param.sfo", sfo), ("sce_sys/sealedkey", blob(b"key", 96))])
    write_zip(os.path.join(out, "raw-backup.zip"), [("sdimg_SAVEDATA00", blob(b"img", 2048)), ("sce_sys/keystone", blob(b"ks", 96)), ("xeno-backup.json", b"{}")])
    with open(good, "rb") as f:
        data = f.read()
    block = {
        "titleId": TITLE, "gameId": "cyberpunk-2077-ps5", "title": "Cyberpunk 2077", "category": "chapter",
        "name": "Chapter 4 — all weapons unlocked", "description": "Right after the heist. Every iconic weapon in the stash, 1.2M eddies.",
        "author": "nick", "gameVersion": "01.004.000", "region": "EUR", "dirName": "SAVEDATA00",
        "size": len(data), "sha256": sha(data), "url": "https://pixeldrain.com/api/file/xenoTestGood",
        "format": "xeno-save-zip", "kind": "ps5", "tags": ["ng+", "max money"],
    }
    body = "<!-- Submitted from XENO STORE 1.0.0. Keep the JSON block below unchanged. -->\n\n```json\n" + json.dumps(block, indent=2, ensure_ascii=False) + "\n```\n"
    with open(os.path.join(out, "issue-good.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(body)
    event = {
        "action": "opened",
        "issue": {
            "number": 42, "title": "[save] Cyberpunk 2077 — Chapter 4 — all weapons unlocked", "body": body, "state": "open",
            "labels": [{"name": "save-submission"}], "created_at": "2026-01-02T10:00:00Z", "updated_at": "2026-01-02T10:00:00Z",
            "html_url": "https://github.com/XENOKING123/XENO-STORE/issues/42", "user": {"login": "someone"},
        },
        "repository": {"full_name": "XENOKING123/XENO-STORE"},
    }
    with open(os.path.join(out, "event-opened.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(event, f, indent=2, ensure_ascii=False)
        f.write("\n")
    return {"good": data, "goodSha": sha(data)}


def main() -> None:
    out = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else HERE
    info = write_fixtures(out)
    print("fixtures written to", out, "· good-save.zip sha256", info["goodSha"], "size", len(info["good"]))


if __name__ == "__main__":
    main()
