#!/usr/bin/env python3
# XENO STORE TV app - The mechanical rebrand of the interface catalogues.
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The app is a port of Orbit Store's TV app (GPL-3.0-or-later; see NOTICE). Its
# catalogues (i18n/*.json) are keyed by the English text, so a key that names
# Orbit changes in all 11 files together. This script applies the rewrites of
# PORT-SPEC 2.6 to every key and value:
#   "Orbit Store" and a standalone "Orbit"     -> "XENO STORE"
#   34177                                       -> 9095
#   PPSA99177                                   -> PPSA99500
#   /data/orbit-store/orbit_store.elf           -> /data/xeno-store/xeno-store.elf
# Keys follow the English rules ("an Orbit update" -> "a XENO STORE update").
# Values follow their language: articles and elisions (French "d'Orbit" ->
# "de XENO STORE", Brazilian Portuguese "o Orbit" -> "a XENO STORE", as the web
# UI writes it), and case suffixes (Hungarian "Orbitot" -> "XENO STORE-t",
# Turkish "Orbit'i" -> "XENO STORE'u"). "Orbit Zero" names a product XENO STORE
# does not have; it is left for a hand edit or removal and makes --check fail.
#
# Sentences that are reworded rather than rebranded (PORT-SPEC 2.6, hand
# rewrites) are edited in the catalogues by hand, and the source literals are
# always edited by hand. --check verifies the result: the catalogues are a
# fixed point of the rewrites, no old name, port, title id or path is left
# (outside the credit keys below), the 11 files have the same keys, en.json is
# key = value, and no tr() literal or /* i18n */ entry in src/ still needs the
# rewrite, so source and catalogues match. Run tools/i18n-coverage.py as well.
#
# usage: rebrand-i18n.py [--check] [--app DIR]

import argparse
import importlib.util
import json
import os
import re
import sys

LOCALES = ("en", "de", "es", "fr", "hu", "it", "nl", "pl", "pt-BR", "ru", "tr")
NAME = "XENO STORE"

# Keys whose text names the original project in a credit; "Orbit" may stay there.
CREDIT_KEYS = frozenset()

_HOLD = ""  # stands in for "Orbit Zero" while the rewrites run

COMMON = (
    ("/data/orbit-store/orbit_store.elf", "/data/xeno-store/xeno-store.elf"),
    ("/data/orbit-store/", "/data/xeno-store/"),
    ("/app0/orbit/orbit_store.elf", "/app0/xeno/xeno-store.elf"),
    ("orbit_store.elf", "xeno-store.elf"),
    ("PPSA99177", "PPSA99500"),
    ("34177", "9095"),
)

# Hungarian: XENO STORE takes back-vowel suffixes after a hyphen (STORE is
# read "stor"); the article before it is "a", not "az".
_HU_SUFFIX = {
    "ot": "t", "t": "t", "hez": "hoz", "hoz": "hoz", "höz": "hoz", "ba": "ba", "be": "ba",
    "ban": "ban", "ben": "ban", "ból": "ból", "ből": "ból", "on": "on", "en": "on", "ön": "on",
    "nak": "nak", "nek": "nak", "ra": "ra", "re": "ra", "ról": "ról", "ről": "ról",
    "tól": "tól", "től": "tól", "nál": "nál", "nél": "nál", "tal": "ral", "tel": "ral",
    "ért": "ért", "ig": "ig", "ként": "ként",
}
# Turkish: suffixes after the apostrophe follow "stor" (back, rounded; voiced r).
_TR_SUFFIX = {
    "i": "u", "ı": "u", "e": "a", "a": "a", "in": "un", "ın": "un", "ye": "a", "yi": "u",
    "te": "da", "ta": "da", "de": "da", "da": "da", "ten": "dan", "tan": "dan", "den": "dan",
    "dan": "dan", "le": "la", "la": "la", "teki": "daki", "deki": "daki", "tekiler": "dakiler",
    "ine": "una", "ini": "unu", "inde": "unda", "inden": "undan", "nin": "nun",
    "u": "u", "un": "un", "una": "una", "unu": "unu", "unda": "unda", "undan": "undan",
}
_FR_ELISION = {"d": "de", "qu": "que", "lorsqu": "lorsque", "puisqu": "puisque",
               "jusqu": "jusque", "l": "le"}
_IT_ELISION = {"l": "lo", "dell": "dello", "all": "allo", "dall": "dallo", "nell": "nello",
               "sull": "sullo"}
_PT_ARTICLE = {"o": "a", "O": "A", "do": "da", "Do": "Da", "ao": "à", "Ao": "À", "no": "na",
               "No": "Na", "pelo": "pela", "Pelo": "Pela", "um": "uma", "Um": "Uma",
               "num": "numa", "dum": "duma"}


def _sub_table(pattern, table, text, build):
    def repl(match):
        word = match.group(1)
        return build(table[word]) if word in table else match.group(0)
    return re.sub(pattern, repl, text)


def rewrite(text, code):
    """The rebranded text for language code ("en" for keys)."""
    text = text.replace("Orbit Zero", _HOLD)
    for old, new in COMMON:
        text = text.replace(old, new)
    if code == "en":
        text = re.sub(r"\b([Aa])n Orbit\b", r"\1 " + NAME, text)
    elif code == "de":
        text = text.replace("Orbit-Store-", NAME + "-")
    elif code == "fr":
        text = _sub_table(r"\b(\w+)[’'](?=Orbit\b)", _FR_ELISION, text, lambda w: w + " ")
    elif code == "it":
        text = _sub_table(r"\b(\w+)[’'](?=Orbit\b)", _IT_ELISION, text, lambda w: w + " ")
    elif code == "pt-BR":
        text = _sub_table(r"\b(\w+) (?=Orbit\b)", _PT_ARTICLE, text, lambda w: w + " ")
    elif code == "hu":
        text = re.sub(r"\b([Aa])z (?=Orbit)", r"\1 ", text)
        text = _sub_table(r"(?<!\w)Orbit(?: Store)?-?(\w+)", _HU_SUFFIX, text,
                          lambda s: NAME + "-" + s)
    elif code == "tr":
        # An unknown suffix keeps "Orbit", so that --check reports it.
        text = re.sub(r"(?<!\w)Orbit(?: Store)?(['’])(\w+)",
                      lambda m: NAME + m.group(1) + _TR_SUFFIX[m.group(2)]
                      if m.group(2) in _TR_SUFFIX else m.group(0), text)
        text = re.sub(r"(?<!\w)O(?=rbit(?: Store)?['’]\w)", "\uE001", text)
    text = text.replace("Orbit Store", NAME)
    text = re.sub(r"(?<!\w)Orbit(?!\w)", NAME, text)
    return text.replace(_HOLD, "Orbit Zero").replace("\uE001", "O")


def rewrite_value(value, code):
    if isinstance(value, str):
        return rewrite(value, code)
    return {form: rewrite(text, code) for form, text in value.items()}


# Left-overs that --check reports.
_FORBIDDEN = re.compile(r"Orbit|orbit_store|orbit-store|34177|PPSA99177")
_GLUED = re.compile(re.escape(NAME) + r"(?=[^\W\d_])")  # "XENO STOREot": a missed suffix


def problems_in(text, code, credit):
    found = []
    if _FORBIDDEN.search(text) and not credit:
        found.append("still names %r" % _FORBIDDEN.search(text).group(0))
    if _GLUED.search(text):
        found.append("a suffix is glued to %s" % NAME)
    if code == "hu" and re.search(r"\b[Aa]z " + re.escape(NAME), text):
        found.append("Hungarian article \"az\" before %s" % NAME)
    if code in ("fr", "it") and re.search(r"[’']" + re.escape(NAME), text):
        found.append("an elided article before %s" % NAME)
    if code == "pt-BR" and re.search(r"\b(?:[Oo]|[Dd]o|[Nn]o|[Pp]elo) " + re.escape(NAME), text):
        found.append("masculine article before %s (the web UI writes \"a %s\")" % (NAME, NAME))
    return found


def load_extractor(here):
    path = os.path.join(here, "i18n-coverage.py")
    spec = importlib.util.spec_from_file_location("i18n_coverage", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description="Rebrand the TV app's catalogues.")
    parser.add_argument("--app", default=os.path.dirname(here), help="the app folder (app/tv)")
    parser.add_argument("--check", action="store_true", help="verify only; change nothing")
    args = parser.parse_args(argv)
    i18n_dir = os.path.join(os.path.abspath(args.app), "i18n")

    errors = []
    results = {}
    for code in LOCALES:
        path = os.path.join(i18n_dir, code + ".json")
        with open(path, encoding="utf-8") as handle:
            raw = handle.read()
        data = json.loads(raw)
        out, origin = {}, {}
        for key, value in data.items():
            new_key = rewrite(key, "en")
            new_value = rewrite_value(value, code)
            if new_key in out and out[new_key] != new_value:
                errors.append("%s.json: %r and %r both become %r" % (code, origin[new_key], key, new_key))
                continue
            out[new_key] = new_value
            origin[new_key] = key
        text = json.dumps(out, indent=2, ensure_ascii=False) + "\n"
        results[code] = (path, raw, text, out)

    changed = [code for code, (_, raw, text, _) in results.items() if raw != text]
    if args.check:
        for code in changed:
            errors.append("%s.json: the rewrites still change it (run without --check)" % code)
    else:
        for code in changed:
            path, _, text, _ = results[code]
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(text)
            print("rewrote %s.json" % code)

    # Verify what is (or would be) written.
    key_sets = {code: set(out) for code, (_, _, _, out) in results.items()}
    for code, keys in key_sets.items():
        if keys != key_sets["en"]:
            errors.append("%s.json: %d keys differ from en.json" % (code, len(keys ^ key_sets["en"])))
    for code, (_, _, _, out) in results.items():
        for key, value in out.items():
            credit = key in CREDIT_KEYS
            texts = [value] if isinstance(value, str) else list(value.values())
            for problem in problems_in(key, "en", credit):
                errors.append("%s.json: key %r: %s" % (code, key, problem))
            for text in texts:
                for problem in problems_in(text, code, credit):
                    errors.append("%s.json: %r: %s" % (code, key, problem))
            if code == "en" and isinstance(value, str) and value != key:
                errors.append("en.json: %r is not key = value" % key)

    # The source literals must have been rebranded by hand the same way.
    src = os.path.join(os.path.abspath(args.app), "src")
    if os.path.isdir(src):
        coverage = load_extractor(here)
        try:
            literals = coverage.extract_tree(src)
        except coverage.SourceError as err:
            errors.append("src: %s" % err)
            literals = {}
        for literal, entry in sorted(literals.items()):
            if rewrite(literal, "en") != literal or problems_in(literal, "en", literal in CREDIT_KEYS):
                errors.append("src: %r (%s) still needs the rewrite: %r" % (
                    literal, ", ".join(entry["where"][:2]), rewrite(literal, "en")))

    for text in errors:
        print("error: %s" % text)
    if errors:
        print("rebrand-i18n: %d problems" % len(errors))
        return 1
    print("rebrand-i18n: %s" % ("OK" if args.check else "done, OK"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
