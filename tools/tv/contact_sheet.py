#!/usr/bin/env python3
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
contact_sheet.py - the review page of the live TV preview (PORT-SPEC 4.1):
<out>/index.html, one grid per language, the pictures grouped by scenario
with captions, each thumbnail linking to the full 1920x1080 PNG, failed
pictures outlined with the reason, and a link to report.json.

    python3 -I tools/tv/contact_sheet.py build/tv-preview            # uses build/tv-preview/report.json
    python3 -I tools/tv/contact_sheet.py some/folder/of/pngs --title "Fixture pictures"
    python3 -I tools/tv/contact_sheet.py app/tv/build/preview/pictures --dest build/fixture-sheet

With a report.json (preview_live.py writes one) the order, groups, captions and
check results come from it. Without one, every folder of <out> holding PNGs is
a section (named after the folder), and PNGs directly in <out> make one more;
a picture's scenario is then its name up to the last "-".

Thumbnails go to <dest>/thumbs/ (JPEG, 480 px wide) and the page to
<dest>/index.html; <dest> is <out> unless --dest names another folder (for
pictures in a folder that must stay as it is). The page is self-contained:
no script, no font or style from the network. Python 3 standard library + PIL.
"""
import argparse
import html
import json
import os
import sys
import time

from PIL import Image

THUMB_W = 480

CSS = """
:root { --bg:#0e0f14; --panel:#171922; --line:#2a2d3a; --text:#ece6d6; --dim:#9a968c; --gold:#d4af37;
        --bad:#e5534b; --ok:#57ab5a; }
* { box-sizing:border-box; }
body { margin:0; background:var(--bg); color:var(--text); font:15px/1.45 system-ui, -apple-system, "Segoe UI", sans-serif; }
header { position:sticky; top:0; z-index:2; background:rgba(14,15,20,.96); border-bottom:1px solid var(--line);
         padding:12px 20px; display:flex; flex-wrap:wrap; gap:8px 18px; align-items:baseline; }
header h1 { margin:0; font-size:18px; color:var(--gold); font-weight:600; }
header nav a { color:var(--text); text-decoration:none; padding:2px 8px; border:1px solid var(--line); border-radius:6px; margin-right:4px; }
header nav a:hover { border-color:var(--gold); }
.meta { color:var(--dim); font-size:13px; }
main { padding:8px 20px 40px; }
.summary { display:flex; flex-wrap:wrap; gap:8px; margin:14px 0 6px; }
.check { border:1px solid var(--line); border-radius:8px; padding:6px 10px; background:var(--panel); font-size:13px; }
.check b { margin-right:6px; }
.check.ok b { color:var(--ok); } .check.bad b { color:var(--bad); }
details.fails { margin:6px 0 12px; } details.fails li { color:var(--bad); font-size:13px; }
section.lang { margin-top:26px; }
section.lang > h2 { font-size:20px; border-bottom:2px solid var(--gold); padding-bottom:4px; }
.scenario { margin:18px 0 6px; }
.scenario h3 { font-size:15px; margin:0 0 8px; color:var(--gold); font-weight:600; }
.scenario h3 small { color:var(--dim); font-weight:400; margin-left:8px; }
.grid { display:grid; grid-template-columns:repeat(auto-fill, minmax(240px, 1fr)); gap:12px; }
figure { margin:0; background:var(--panel); border:1px solid var(--line); border-radius:8px; overflow:hidden; }
figure.bad { border-color:var(--bad); box-shadow:0 0 0 1px var(--bad); }
figure a { display:block; }
figure img { display:block; width:100%; height:auto; aspect-ratio:16/9; object-fit:cover; background:#000; }
figcaption { padding:6px 8px; font-size:13px; word-break:break-word; }
figcaption .why { display:block; color:var(--bad); font-size:12px; margin-top:2px; }
figcaption .note { display:block; color:var(--dim); font-size:12px; }
.missing { aspect-ratio:16/9; display:flex; align-items:center; justify-content:center; color:var(--bad); font-size:13px; }
@media (max-width:600px) { header, main { padding-left:12px; padding-right:12px; } .grid { grid-template-columns:1fr 1fr; } }
"""


def _rel(path, base):
    return os.path.relpath(path, base).replace(os.sep, "/")


def make_thumb(src, dst, width=THUMB_W):
    if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
        return dst
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with Image.open(src) as im:
        im = im.convert("RGB")
        h = max(1, round(im.height * width / im.width))
        im.resize((width, h), Image.LANCZOS).save(dst, "JPEG", quality=82)
    return dst


def scan(out):
    """Sections without a report: {section: [(scenario, capture, path)]}."""
    sections = {}
    entries = sorted(os.listdir(out))
    loose = [e for e in entries if e.lower().endswith(".png") and os.path.isfile(os.path.join(out, e))]
    for d in entries:
        p = os.path.join(out, d)
        if os.path.isdir(p) and d not in ("thumbs", "logs") and not d.startswith("."):
            pngs = sorted(f for f in os.listdir(p) if f.lower().endswith(".png"))
            if pngs:
                sections[d] = [(_scenario_of(f), f[:-4], os.path.join(p, f)) for f in pngs]
    if loose:
        sections[os.path.basename(os.path.abspath(out))] = [(_scenario_of(f), f[:-4], os.path.join(out, f)) for f in loose]
    return sections


def _scenario_of(filename):
    stem = filename[:-4]
    return stem.rsplit("-", 1)[0] if "-" in stem else stem


def from_report(out, report):
    """Sections from report.json pictures: {lang: [(scenario, capture, path, picture record)]}."""
    sections = {}
    for p in report.get("pictures", []):
        sections.setdefault(p["lang"], []).append(p)
    return sections


def build(out, report=None, title="XENO STORE TV app - live preview", dest=None):
    out = os.path.abspath(out)
    dest = os.path.abspath(dest or out)
    thumbs = os.path.join(dest, "thumbs")
    parts = []
    nav = []
    summary = ""
    if report:
        sections = from_report(out, report)
        checks = report.get("checks", {})
        cells = []
        for key, c in checks.items():
            cls = "ok" if c.get("ok") else "bad"
            cells.append(f'<div class="check {cls}"><b>{"PASS" if c.get("ok") else "FAIL"}</b>{html.escape(c.get("title", key))}'
                         f' <span class="meta">({len(c.get("failures", []))} failure(s))</span></div>')
        fails = []
        for key, c in checks.items():
            for f in c.get("failures", [])[:200]:
                fails.append(f"<li>{html.escape(key)}: {html.escape(str(f))}</li>")
        for f in report.get("setupFailures", []):
            fails.append(f"<li>setup: {html.escape(str(f))}</li>")
        summary = (f'<div class="summary">{"".join(cells)}</div>'
                   + (f'<details class="fails" open><summary>{len(fails)} failure(s)</summary><ul>{"".join(fails)}</ul></details>'
                      if fails else ""))
    else:
        sections = scan(out)
    total = 0
    for lang, items in sections.items():
        anchor = "lang-" + "".join(ch if ch.isalnum() else "-" for ch in lang)
        nav.append(f'<a href="#{anchor}">{html.escape(lang)}</a>')
        groups = {}
        order = []
        for it in items:
            if isinstance(it, dict):
                key = (it.get("group", ""), it.get("scenario", ""))
                rec = it
                path = os.path.join(out, it["file"]) if it.get("file") else ""
                cap = it.get("capture", "")
            else:
                key = ("", it[0])
                rec = {}
                cap, path = it[1], it[2]
            if key not in groups:
                groups[key] = []
                order.append(key)
            groups[key].append((cap, path, rec))
        blocks = []
        for key in order:
            group, scen = key
            figs = []
            for cap, path, rec in groups[key]:
                total += 1
                bad = rec.get("ok") is False
                why = "".join(f'<span class="why">{html.escape(w)}</span>' for w in rec.get("failures", []))
                note = ""
                if rec.get("stddev") is not None:
                    note = f'<span class="note">luma sd {rec["stddev"]:.1f}, {rec.get("detail", 0.0):.1%} detail'
                    if rec.get("compare") is not None:
                        note += f', diff {rec["compare"]:.2f}'
                    note += "</span>"
                if path and os.path.exists(path):
                    t = make_thumb(path, os.path.join(thumbs, anchor, os.path.basename(path)[:-4] + ".jpg"))
                    img = (f'<a href="{html.escape(_rel(path, dest))}"><img loading="lazy" src="{html.escape(_rel(t, dest))}" '
                           f'alt="{html.escape(cap)}"></a>')
                else:
                    img = '<div class="missing">missing</div>'
                figs.append(f'<figure class="{"bad" if bad else ""}">{img}<figcaption>{html.escape(cap)}{why}{note}</figcaption></figure>')
            sub = f"<small>{html.escape(group)}</small>" if group and group != scen else ""
            blocks.append(f'<div class="scenario"><h3>{html.escape(scen)}{sub}</h3><div class="grid">{"".join(figs)}</div></div>')
        parts.append(f'<section class="lang" id="{anchor}"><h2>{html.escape(lang)}</h2>{"".join(blocks)}</section>')
    stamp = time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime())
    report_path = os.path.join(out, "report.json")
    report_link = (f' &middot; <a href="{html.escape(_rel(report_path, dest))}" style="color:var(--gold)">report.json</a>'
                   if os.path.exists(report_path) else "")
    mode = f' &middot; {html.escape(report.get("mode", ""))}' if report and report.get("mode") else ""
    doc = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>TV Preview Sheet</title><style>{CSS}</style></head>
<body><header><h1>{html.escape(title)}</h1><nav>{"".join(nav)}</nav>
<span class="meta">{total} picture(s) &middot; {stamp}{mode}{report_link}</span></header>
<main>{summary}{"".join(parts) or "<p>No pictures.</p>"}</main></body></html>
"""
    os.makedirs(dest, exist_ok=True)
    path = os.path.join(dest, "index.html")
    with open(path, "w", encoding="utf-8") as f:
        f.write(doc)
    return path, total


def main(argv=None):
    ap = argparse.ArgumentParser(description="Write <out>/index.html for a folder of preview pictures.")
    ap.add_argument("out")
    ap.add_argument("--report", help="report.json (default: <out>/report.json when present)")
    ap.add_argument("--title", default="XENO STORE TV app - live preview")
    ap.add_argument("--dest", help="write index.html and thumbs/ here instead of into <out>")
    a = ap.parse_args(argv)
    rp = a.report or os.path.join(a.out, "report.json")
    report = None
    if os.path.exists(rp):
        with open(rp, encoding="utf-8") as f:
            report = json.load(f)
    path, n = build(a.out, report, a.title, a.dest)
    print(f"contact sheet: {path} ({n} pictures)")
    return 0 if n else 1


if __name__ == "__main__":
    sys.exit(main())
