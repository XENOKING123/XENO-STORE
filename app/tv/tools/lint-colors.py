#!/usr/bin/env python3
# XENO STORE TV app - Colour lint: every colour comes from the theme's tokens.
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/lint-colors.py [app dir]
#
# Fails when the app's own code (src/orbit/ and src/main.cpp) spells a colour
# as a Color::rgb(0x...) literal anywhere but in the token table
# (src/orbit/ui.hpp, namespace color) and in the brand (src/orbit/brand.cpp).
# Screens use the named tokens, so the theme changes in one place. The kit
# under src/ui/ and src/gfx/ keeps its own palettes and is not checked.

import re
import sys
from pathlib import Path

ALLOWED = {"src/orbit/ui.hpp", "src/orbit/brand.cpp"}
LITERAL = re.compile(r"Color::rgb\(\s*0x", re.IGNORECASE)


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent
    files = sorted((root / "src" / "orbit").glob("*.[ch]pp")) + [root / "src" / "main.cpp"]
    found = 0
    checked = 0
    for path in files:
        if not path.is_file():
            continue
        relative = path.relative_to(root).as_posix()
        if relative in ALLOWED:
            continue
        checked += 1
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if LITERAL.search(line):
                print(f"{relative}:{number}: colour literal; use a ui::color token: {line.strip()}")
                found += 1
    if checked == 0:
        print(f"lint-colors: no sources under {root}/src/orbit", file=sys.stderr)
        return 2
    if found:
        print(f"lint-colors: {found} colour literal(s) outside "
              f"{' and '.join(sorted(ALLOWED))}", file=sys.stderr)
        return 1
    print(f"lint-colors: {checked} files, no colour literals outside the tokens")
    return 0


if __name__ == "__main__":
    sys.exit(main())
