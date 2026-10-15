#!/usr/bin/env python3
# XENO STORE - lock-step check of the TV app's param.json against VERSION.
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
check_param.py — the TV app's sce_sys/param.json must name the XENO STORE title and carry the
version in VERSION (PORT-SPEC §2.1, §3.4 'make tv-check').

    python3 tools/tv/check_param.py VERSION app/tv/sce_sys/param.json

Checks:
  - VERSION is plain N.N.N (the app's starter compares xeno/version.txt that way);
  - contentVersion == content_version(VERSION) from tools/make_ffpkg.py (1.2.1 -> 01.002.001), so the
    tag, VERSION and the app's own version stay in lock-step;
  - titleId == PPSA99500, conceptId == 99500 and contentId == IV0000-PPSA99500_00-XENOSTORE0000000,
    the ids tools/make_ffpkg.py writes into the image, and the contentId passes the app kit's rule
    (app/tv/tools/build.sh: [A-Z]{2}NNNN-PPSANNNNN_00-<16 of A-Z0-9>, containing the titleId).

Exit 0 = OK, 1 = a check failed, 2 = a file is missing or unreadable.
"""
import importlib.util
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)


def load_make_ffpkg():
    """tools/make_ffpkg.py by path: one definition of content_version() and of the ids."""
    spec = importlib.util.spec_from_file_location("xeno_make_ffpkg", os.path.join(TOOLS, "make_ffpkg.py"))
    mod = importlib.util.module_from_spec(spec)
    sys.dont_write_bytecode = True
    spec.loader.exec_module(mod)
    return mod


def check(version, param, mf):
    """Returns the list of problems (empty when param.json is right for this VERSION)."""
    errors = []
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        errors.append("VERSION %r is not plain N.N.N" % version)
    else:
        major, minor, patch = (int(p) for p in version.split("."))
        if major > 99 or minor > 999 or patch > 999:
            errors.append("VERSION %s does not fit contentVersion NN.NNN.NNN" % version)
    want = {
        "contentVersion": mf.content_version(version),
        "titleId": mf.TITLE_ID,
        "conceptId": mf.TITLE_ID[4:],
        "contentId": mf.CONTENT_ID,
    }
    for key, value in want.items():
        got = param.get(key)
        if got != value:
            errors.append("%s is %s, expected %s" % (key, json.dumps(got), json.dumps(value)))
    content_id = param.get("contentId")
    title_id = param.get("titleId")
    if not (isinstance(content_id, str) and isinstance(title_id, str)
            and re.fullmatch(r"[A-Z]{2}\d{4}-PPSA\d{5}_00-[A-Z0-9]{16}", content_id) and title_id in content_id):
        errors.append("contentId %s does not pass the app kit's rule or does not contain titleId %s"
                      % (json.dumps(content_id), json.dumps(title_id)))
    if not (isinstance(param.get("contentVersion"), str) and re.fullmatch(r"\d{2}\.\d{3}\.\d{3}", param["contentVersion"])):
        errors.append("contentVersion %s is not NN.NNN.NNN" % json.dumps(param.get("contentVersion")))
    return errors


def main(argv):
    if len(argv) != 3 or argv[1] in ("-h", "--help"):
        print("usage: check_param.py VERSION_FILE PARAM_JSON", file=sys.stderr)
        return 2
    version_file, param_file = argv[1], argv[2]
    try:
        with open(version_file, encoding="utf-8") as fh:
            version = fh.read().strip()
    except OSError as e:
        print("check_param: cannot read %s: %s" % (version_file, e), file=sys.stderr)
        return 2
    try:
        with open(param_file, encoding="utf-8") as fh:
            param = json.load(fh)
    except (OSError, ValueError) as e:
        print("check_param: cannot read %s: %s" % (param_file, e), file=sys.stderr)
        return 2
    if not isinstance(param, dict):
        print("check_param: %s is not a JSON object" % param_file, file=sys.stderr)
        return 1
    errors = check(version, param, load_make_ffpkg())
    for e in errors:
        print("check_param: %s: %s" % (param_file, e), file=sys.stderr)
    if errors:
        print("check_param: set them in %s for VERSION %s (contentVersion follows VERSION in every release)"
              % (param_file, version), file=sys.stderr)
        return 1
    print("check_param: %s matches VERSION %s (%s %s, contentVersion %s)"
          % (param_file, version, param["titleId"], param["contentId"], param["contentVersion"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
