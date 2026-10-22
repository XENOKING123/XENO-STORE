#!/usr/bin/env python3
# XENO STORE TV app - Translation coverage gate for i18n/*.json.
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The app looks text up by its English wording (src/orbit/i18n.cpp), so the
# catalogues in i18n/ must follow the source exactly. This gate extracts every
# tr("...") and trn(n, "...", "...") literal and every /* i18n */-marked entry
# from src/, adds the sentences listed in tools/i18n-external-keys.txt (backend
# replies, catalogue genres and drive names that reach tr() at run time), and
#   fails when en.json lacks a key, when en.json is not key = value, when any
#   catalogue holds a key that is neither in the source nor in the external
#   list (stale), when a value is malformed or its {placeholders} differ, or
#   when a catalogue's coverage is below --min-coverage;
#   warns with the number of missing translations per language (the app shows
#   the English for those).
# With --backend DIR (found automatically at ../../src when this app sits in
# the XENO STORE repository as app/tv) it also warns when a [backend] external
# sentence is not a literal in the backend sources, or when a sentence in
# src/tvapi*.c has no key. Those are warnings only: the backend and the app
# are built separately.
#
# usage: i18n-coverage.py [--app DIR] [--external FILE] [--backend DIR | --no-backend]
#                         [--min-coverage PCT] [--list-missing CODE] [--quiet]

import argparse
import json
import os
import re
import sys

LOCALES = ("en", "de", "es", "fr", "hu", "it", "nl", "pl", "pt-BR", "ru", "tr")
PLURAL_FORMS = ("one", "few", "many", "other")
SOURCE_SUFFIXES = (".cpp", ".hpp", ".h", ".cc", ".c", ".inc")
SKIP_DIRS = ("third_party",)


class SourceError(Exception):
    pass


# ---------------------------------------------------------------- C/C++ lexing

_ESCAPES = {"n": 10, "t": 9, "r": 13, "0": 0, "a": 7, "b": 8, "f": 12, "v": 11,
            "\\": 92, '"': 34, "'": 39, "?": 63}
_RAW = re.compile(r'(?:u8|u|U|L)?R"([^()\\\s]{0,16})\(')
_STR = re.compile(r'(?:u8|u|U|L)?"')
_CHR = re.compile(r"(?:u8|u|U|L)?'")
_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_NUMBER = re.compile(r"\.?[0-9](?:[0-9A-Za-z_.]|'(?=[0-9A-Za-z])|[eEpP][+-])*")


def _decode_string(src, start, line):
    """Reads a "..." body starting after the quote; returns (bytes, end)."""
    out = bytearray()
    i, n = start, len(src)
    while True:
        if i >= n or src[i] == "\n":
            raise SourceError("unterminated string literal on line %d" % line)
        ch = src[i]
        if ch == '"':
            return bytes(out), i + 1
        if ch != "\\":
            out += ch.encode("utf-8")
            i += 1
            continue
        e = src[i + 1] if i + 1 < n else ""
        if e == "x":
            j = i + 2
            while j < n and src[j] in "0123456789abcdefABCDEF":
                j += 1
            if j == i + 2:
                raise SourceError("bad \\x escape on line %d" % line)
            out.append(int(src[i + 2:j], 16) & 0xFF)
            i = j
        elif e in "01234567":
            j = i + 1
            while j < n and j < i + 4 and src[j] in "01234567":
                j += 1
            out.append(int(src[i + 1:j], 8) & 0xFF)
            i = j
        elif e == "u":
            out += chr(int(src[i + 2:i + 6], 16)).encode("utf-8")
            i += 6
        elif e == "U":
            out += chr(int(src[i + 2:i + 10], 16)).encode("utf-8")
            i += 10
        elif e == "\n":  # line splice
            i += 2
        elif e in _ESCAPES:
            out.append(_ESCAPES[e])
            i += 2
        else:
            raise SourceError("unknown escape \\%s on line %d" % (e, line))


def tokenize(src, keep_preprocessor=False):
    """Tokens (kind, value, line): STR (bytes), MARK (an /* i18n */ comment),
    ID, NUM, CHR and P (punctuation). Comments are dropped; preprocessor lines
    are dropped unless keep_preprocessor is set (the backend's #define texts)."""
    toks = []
    i, n, line = 0, len(src), 1
    line_start = True
    while i < n:
        c = src[i]
        if c == "\n":
            line += 1
            i += 1
            line_start = True
            continue
        if c in " \t\r\f\v":
            i += 1
            continue
        if c == "\\" and i + 1 < n and src[i + 1] == "\n":
            line += 1
            i += 2
            continue
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            if j < 0:
                raise SourceError("unterminated comment on line %d" % line)
            body = src[i + 2:j]
            if body.strip() == "i18n":
                toks.append(("MARK", None, line))
            line += body.count("\n")
            i = j + 2
            continue
        if c == "#" and line_start and not keep_preprocessor:
            j = i
            while True:
                k = src.find("\n", j)
                if k < 0:
                    k = n
                    break
                if src[k - 1] == "\\":
                    j = k + 1
                    continue
                break
            line += src.count("\n", i, k)
            i = k
            continue
        line_start = False
        m = _RAW.match(src, i)
        if m:
            close = ")" + m.group(1) + '"'
            j = src.find(close, m.end())
            if j < 0:
                raise SourceError("unterminated raw string on line %d" % line)
            body = src[m.end():j]
            toks.append(("STR", body.encode("utf-8"), line))
            line += body.count("\n")
            i = j + len(close)
            continue
        m = _STR.match(src, i)
        if m:
            value, i = _decode_string(src, m.end(), line)
            toks.append(("STR", value, line))
            continue
        m = _CHR.match(src, i)
        if m:
            j = m.end()
            while j < n and src[j] != "'":
                if src[j] == "\n":
                    raise SourceError("unterminated character literal on line %d" % line)
                j += 2 if src[j] == "\\" else 1
            toks.append(("CHR", src[i:j + 1], line))
            i = j + 1
            continue
        m = _IDENT.match(src, i)
        if m:
            toks.append(("ID", m.group(0), line))
            i = m.end()
            continue
        m = _NUMBER.match(src, i)
        if m:
            toks.append(("NUM", m.group(0), line))
            i = m.end()
            continue
        if src.startswith("::", i) or src.startswith("->", i):
            toks.append(("P", src[i:i + 2], line))
            i += 2
            continue
        toks.append(("P", c, line))
        i += 1
    return toks


def _strings_at(toks, k):
    """Adjacent string literals from k, joined as C++ does: (text, next index)."""
    parts = []
    while k < len(toks) and toks[k][0] == "STR":
        parts.append(toks[k][1])
        k += 1
    if not parts:
        return None, k
    try:
        return b"".join(parts).decode("utf-8"), k
    except UnicodeDecodeError:
        raise SourceError("string on line %d is not UTF-8" % toks[k - 1][2])


def _skip_argument(toks, k):
    depth = 0
    while k < len(toks):
        kind, value = toks[k][0], toks[k][1]
        if kind == "P" and value in "([{":
            depth += 1
        elif kind == "P" and value in ")]}":
            if depth == 0:
                return k
            depth -= 1
        elif kind == "P" and value == "," and depth == 0:
            return k
        k += 1
    return k


def extract_file(path):
    """[(key, kind, line, one_form)] for one source file: kind is "tr", "mark"
    or "plural" (trn's English plural, which is the key; one_form is its
    singular)."""
    with open(path, encoding="utf-8") as handle:
        toks = tokenize(handle.read())
    found = []
    for k, (kind, value, line) in enumerate(toks):
        call = kind == "ID" and value in ("tr", "trn") and k + 1 < len(toks) and toks[k + 1][1] == "("
        if kind == "MARK":
            text, _ = _strings_at(toks, k + 1)
            if text is None:
                raise SourceError("/* i18n */ without a string literal on line %d" % line)
            found.append((text, "mark", line, None))
        elif call and value == "tr":
            text, _ = _strings_at(toks, k + 2)
            if text is not None:
                found.append((text, "tr", line, None))
        elif call and value == "trn":
            j = _skip_argument(toks, k + 2)
            if j >= len(toks) or toks[j][1] != ",":
                continue
            one, j = _strings_at(toks, j + 1)
            if one is None or j >= len(toks) or toks[j][1] != ",":
                continue
            other, _ = _strings_at(toks, j + 1)
            if other is not None:
                found.append((other, "plural", line, one))
    return found


def extract_tree(src_dir):
    """{key: {"kind": "plain"|"plural", "one": set, "where": [file:line]}}."""
    keys = {}
    for root, dirs, files in os.walk(src_dir):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        for name in sorted(files):
            if not name.endswith(SOURCE_SUFFIXES):
                continue
            path = os.path.join(root, name)
            try:
                items = extract_file(path)
            except (SourceError, UnicodeDecodeError) as err:
                raise SourceError("%s: %s" % (os.path.relpath(path, src_dir), err))
            for text, kind, line, one in items:
                entry = keys.setdefault(text, {"kinds": set(), "one": set(), "where": []})
                entry["kinds"].add("plural" if kind == "plural" else "plain")
                if one is not None:
                    entry["one"].add(one)
                entry["where"].append("%s:%d" % (os.path.relpath(path, src_dir), line))
    return keys


def c_tokens(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return tokenize(handle.read(), keep_preprocessor=True)


def string_macros(toks, macros):
    """Adds to macros {NAME: [token, ...]} each '#define NAME' whose body is
    string literals and names only, on one line ("..." APP_TITLE_ID "...")."""
    for k in range(len(toks) - 3):
        if (toks[k][:2] == ("P", "#") and toks[k + 1][:2] == ("ID", "define")
                and toks[k + 2][0] == "ID"):
            line, body, j = toks[k + 2][2], [], k + 3
            while j < len(toks) and toks[j][2] == line and toks[j][0] in ("STR", "ID"):
                body.append(toks[j])
                j += 1
            ends_line = j == len(toks) or toks[j][2] != line
            if body and ends_line and any(t[0] == "STR" for t in body):
                macros[toks[k + 2][1]] = body


def _expand(tok, macros, depth=0):
    """The string bytes a STR token or a string macro stands for, else None."""
    if tok[0] == "STR":
        return tok[1]
    body = macros.get(tok[1]) if tok[0] == "ID" and depth < 8 else None
    if body is None:
        return None
    parts = [_expand(t, macros, depth + 1) for t in body]
    return None if any(p is None for p in parts) else b"".join(parts)


def c_literals(toks, macros=None):
    """Every string literal (adjacent ones joined) in a C file's tokens,
    #defines included. With macros, names of string macros between or next to
    literals are expanded, so "at /data/homebrew/" APP_TITLE_ID "." is found
    whole."""
    macros = macros or {}
    out, k = set(), 0
    while k < len(toks):
        if toks[k][0] == "STR" or (toks[k][0] == "ID" and toks[k][1] in macros
                                   and k + 1 < len(toks) and toks[k + 1][0] == "STR"
                                   and toks[k - 1][:2] != ("ID", "define")):
            parts = []
            while k < len(toks):
                value = _expand(toks[k], macros)
                if value is None:
                    break
                parts.append(value)
                k += 1
            if not parts:
                # A string macro whose body names something that is not a
                # string ("/user/app/" LI_TITLE_ID): no literal starts here.
                k += 1
                continue
            try:
                out.add(b"".join(parts).decode("utf-8"))
            except UnicodeDecodeError:  # bytes such as a file signature, not a sentence
                pass
        else:
            k += 1
    return out


# ------------------------------------------------------------------ catalogues

def read_external(path):
    """{key: kind} from the external list: one English key per line, '#'
    comments, [backend] / [data] lines that set the kind of what follows, and
    "..." JSON strings for keys with leading or trailing spaces."""
    keys, kind = {}, "backend"
    with open(path, encoding="utf-8") as handle:
        for number, raw in enumerate(handle, 1):
            text = raw.rstrip("\n")
            if not text.strip() or text.lstrip().startswith("#"):
                continue
            if text in ("[backend]", "[data]"):
                kind = text[1:-1]
                continue
            if text.startswith('"'):
                # A JSON string, for keys with leading or trailing spaces.
                try:
                    text = json.loads(text)
                except ValueError:
                    raise SourceError("%s:%d: bad JSON string" % (path, number))
            elif text != text.strip():
                raise SourceError("%s:%d: stray spaces around %r (quote it as JSON)" % (path, number, text))
            keys[text] = kind
    return keys


def placeholders(text):
    return set(re.findall(r"\{([A-Za-z_][A-Za-z0-9_]*)\}", text))


def check_value(key, value):
    """Problems with one catalogue value, as sentences."""
    want = placeholders(key)
    if isinstance(value, str):
        have = placeholders(value)
        if have != want:
            return ["placeholders %s, expected %s" % (sorted(have), sorted(want))]
        return []
    if not isinstance(value, dict) or "other" not in value:
        return ["neither a string nor a plural object with an \"other\" form"]
    problems = []
    for form, text in value.items():
        if form not in PLURAL_FORMS or not isinstance(text, str):
            problems.append("bad plural form %r" % form)
            continue
        have = placeholders(text)
        if have - want:
            problems.append("form %s has unknown placeholders %s" % (form, sorted(have - want)))
        if (want - {"count"}) - have:
            problems.append("form %s drops %s" % (form, sorted((want - {"count"}) - have)))
    return problems


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description="Translation coverage gate for the TV app.")
    parser.add_argument("--app", default=os.path.dirname(here), help="the app folder (app/tv)")
    parser.add_argument("--external", default=None, help="default: tools/i18n-external-keys.txt")
    parser.add_argument("--backend", default=None, help="backend sources to cross-check (warnings)")
    parser.add_argument("--no-backend", action="store_true", help="skip the backend cross-check")
    parser.add_argument("--min-coverage", type=float, default=0.0,
                        help="fail when a language translates less than this percentage")
    parser.add_argument("--list-missing", metavar="CODE", help="print the keys CODE lacks")
    parser.add_argument("--quiet", action="store_true", help="print problems only")
    args = parser.parse_args(argv)

    app = os.path.abspath(args.app)
    i18n_dir = os.path.join(app, "i18n")
    external_path = args.external or os.path.join(app, "tools", "i18n-external-keys.txt")
    errors, warnings = [], []

    def fail(text):
        errors.append(text)

    try:
        source = extract_tree(os.path.join(app, "src"))
        external = read_external(external_path)
    except (OSError, SourceError) as err:
        print("i18n-coverage: %s" % err, file=sys.stderr)
        return 1

    catalogues = {}
    for code in LOCALES:
        path = os.path.join(i18n_dir, code + ".json")
        try:
            with open(path, encoding="utf-8") as handle:
                data = json.load(handle)
        except (OSError, ValueError) as err:
            fail("%s.json: %s" % (code, err))
            continue
        if not isinstance(data, dict):
            fail("%s.json: not a JSON object" % code)
            continue
        catalogues[code] = data
    extra = sorted(f[:-5] for f in os.listdir(i18n_dir)
                   if f.endswith(".json") and f[:-5] not in LOCALES)
    if extra:
        fail("unknown catalogues in i18n/: %s (the app loads only %s)" % (", ".join(extra), ", ".join(LOCALES)))
    en = catalogues.get("en", {})

    # The source against en.json.
    for key, entry in sorted(source.items()):
        where = ", ".join(entry["where"][:3])
        if len(entry["kinds"]) > 1:
            fail("used both as tr() and as trn() plural: %r (%s)" % (key, where))
            continue
        if key not in en:
            fail("en.json lacks %r (%s)" % (key, where))
            continue
        plural = "plural" in entry["kinds"]
        if plural and not isinstance(en[key], dict):
            fail("en.json has %r as a string, but %s uses it as a plural" % (key, where))
        elif not plural and not isinstance(en[key], str):
            fail("en.json has %r as a plural, but %s uses it as a string" % (key, where))
        elif plural:
            if len(entry["one"]) > 1:
                warnings.append("%r has different singulars in the source: %s" % (key, sorted(entry["one"])))
            if en[key].get("one") not in entry["one"]:
                fail("en.json's singular of %r is %r, the source says %s" % (key, en[key].get("one"), sorted(entry["one"])))
    for key in sorted(external):
        if key not in en:
            fail("en.json lacks the external key %r" % key)

    # Every catalogue: values, staleness, coverage.
    known = set(source) | set(external)
    for code, data in catalogues.items():
        stale = sorted(k for k in data if k not in known)
        for key in stale[:25]:
            fail("%s.json: stale key %r (not in src/ or in the external list)" % (code, key))
        if len(stale) > 25:
            fail("%s.json: %d more stale keys" % (code, len(stale) - 25))
        for key, value in data.items():
            for problem in check_value(key, value):
                fail("%s.json: %r: %s" % (code, key, problem))
            if code == "en":
                if isinstance(value, str) and value != key:
                    fail("en.json: the value of %r differs from its key" % key)
                if isinstance(value, dict) and value.get("other") != key:
                    fail("en.json: the plural %r has another \"other\" form" % key)

    report = []
    for code in LOCALES[1:]:
        data = catalogues.get(code)
        if data is None:
            continue
        missing = [k for k in en if k not in data]
        share = 100.0 * (len(en) - len(missing)) / len(en) if en else 100.0
        report.append("  %-5s %4d/%d translated (%.1f%%)%s" % (
            code, len(en) - len(missing), len(en), share,
            ", %d missing" % len(missing) if missing else ""))
        if missing:
            warnings.append("%s.json: %d missing translations (English is shown)" % (code, len(missing)))
        if share < args.min_coverage:
            fail("%s.json: %.1f%% translated, below %.1f%%" % (code, share, args.min_coverage))
        if args.list_missing == code:
            for key in missing:
                print("missing %s: %r" % (code, key))

    # Optional: the backend's sentences.
    backend = None if args.no_backend else args.backend
    if backend is None and not args.no_backend:
        guess = os.path.normpath(os.path.join(app, "..", "..", "src"))
        if os.path.isfile(os.path.join(guess, "tvapi.c")):
            backend = guess
    if backend:
        literals, tv_literals, macros, files = set(), set(), {}, {}
        for name in sorted(os.listdir(backend)):
            if name.endswith((".c", ".h")) and name != "assets.h":
                files[name] = c_tokens(os.path.join(backend, name))
                string_macros(files[name], macros)
        for name, toks in files.items():
            found = c_literals(toks, macros)
            literals |= found
            if name.startswith("tvapi"):
                tv_literals |= found
        for key, kind in sorted(external.items()):
            if kind == "backend" and key not in literals:
                warnings.append("backend: no literal %r in %s" % (key, backend))
        for text in sorted(tv_literals):
            if (re.match(r"[A-Z][^%\n]*[a-z][^%\n]*[.…]$", text) and " " in text
                    and text not in en and "{" not in text):
                warnings.append("backend: sentence without a key: %r" % text)

    if not args.quiet:
        plural = sum(1 for e in source.values() if "plural" in e["kinds"])
        print("i18n-coverage: %d source keys (%d plural), %d external keys, en.json %d keys" % (
            len(source), plural, len(external), len(en)))
        for line in report:
            print(line)
        if backend:
            print("  backend cross-check: %s" % backend)
    for text in warnings:
        print("warning: %s" % text)
    for text in errors:
        print("error: %s" % text)
    if errors:
        print("i18n-coverage: FAILED (%d errors)" % len(errors))
        return 1
    if not args.quiet:
        print("i18n-coverage: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
