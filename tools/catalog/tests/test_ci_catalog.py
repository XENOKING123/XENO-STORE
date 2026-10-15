"""The CI catalog contract.

* ``build_catalog.py --offline`` never clobbers a richer catalog committed by the
  scheduled online sync (.github/workflows/catalog-sync.yml) unless ``--force`` is
  given, so ``make catalog`` on a synced checkout keeps the full catalog.
* The sync workflow's Summary step runs against the committed files and prints the
  PS5 / PS4 game counts of the compact catalog.
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

import build_catalog  # noqa: E402
from schema import expand_catalog  # noqa: E402

PROJECT = Path(HERE).parent.parent.parent
# One small, stable snapshot source keeps every build here well under a second.
BASE_ARGS = ["--offline", "--quiet", "--no-seeds", "--sources", "feed-verified"]


def run_build(*extra: str) -> tuple[int, str]:
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = build_catalog.main(BASE_ARGS + list(extra))
    return rc, out.getvalue()


def run_validate(path: Path) -> int:
    with contextlib.redirect_stdout(io.StringIO()):
        return build_catalog.main(["--validate", "--out", str(path), "--quiet"])


class OfflineGuardTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="xeno-guard-"))
        self.cat = self.tmp / "xeno-catalog.json"
        self.report = self.tmp / "report.json"
        self.revision = self.tmp / "revision.json"

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def build(self, limit: int, *extra: str) -> tuple[int, str]:
        return run_build("--limit", str(limit), "--out", str(self.cat), *extra)

    def mark_online(self) -> None:
        rep = json.loads(self.report.read_text(encoding="utf-8"))
        rep["mode"] = "online"
        self.report.write_text(json.dumps(rep), encoding="utf-8")

    def snapshot(self) -> dict[str, bytes]:
        return {p.name: p.read_bytes() for p in (self.cat, self.report, self.revision)}

    def games(self) -> int:
        return len(json.loads(self.cat.read_text(encoding="utf-8"))["games"])

    def test_richer_online_catalog_is_kept(self):
        rc, _ = self.build(20)
        self.assertEqual(rc, 0)
        self.mark_online()
        before = self.snapshot()
        rc, out = self.build(3)
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.snapshot(), before, "catalog, revision.json and report.json must stay untouched")
        self.assertIn("NOT written", out)
        self.assertIn("--force", out)
        self.assertEqual(run_validate(self.cat), 0)

    def test_force_replaces_the_online_catalog(self):
        self.build(20)
        self.mark_online()
        rc, out = self.build(3, "--force")
        self.assertEqual(rc, 0, out)
        self.assertNotIn("NOT written", out)
        self.assertEqual(self.games(), 3)
        self.assertEqual(json.loads(self.report.read_text(encoding="utf-8"))["mode"], "offline")
        self.assertEqual(run_validate(self.cat), 0)

    def test_offline_catalog_is_rebuilt(self):
        self.build(20)
        rc, out = self.build(3)
        self.assertEqual(rc, 0, out)
        self.assertNotIn("NOT written", out)
        self.assertEqual(self.games(), 3)

    def test_online_catalog_with_fewer_games_is_replaced(self):
        self.build(3)
        self.mark_online()
        rc, out = self.build(20)
        self.assertEqual(rc, 0, out)
        self.assertNotIn("NOT written", out)
        self.assertEqual(self.games(), 20)

    def test_guard_only_protects_a_valid_file_the_report_describes(self):
        self.build(20)
        self.mark_online()
        guard = build_catalog.offline_overwrite_guard
        self.assertTrue(guard(self.cat, self.report, 3))
        self.assertEqual(guard(self.cat, self.report, 20), "", "equal size: nothing to lose")
        self.assertEqual(guard(self.tmp / "missing.json", self.report, 3), "")
        self.assertEqual(guard(self.cat, self.tmp / "missing-report.json", 3), "")

        rep = json.loads(self.report.read_text(encoding="utf-8"))
        rep["revision"] = rep["revision"] + 7          # a stale report speaks for another file
        self.report.write_text(json.dumps(rep), encoding="utf-8")
        self.assertEqual(guard(self.cat, self.report, 3), "")
        rep["revision"] -= 7
        self.report.write_text(json.dumps(rep), encoding="utf-8")
        self.assertTrue(guard(self.cat, self.report, 3))

        doc = json.loads(self.cat.read_text(encoding="utf-8"))
        doc["games"][0]["platform"] = "PS9"            # an invalid file is not worth keeping
        self.cat.write_text(json.dumps(doc), encoding="utf-8")
        self.assertEqual(guard(self.cat, self.report, 3), "")

        self.cat.write_text("{not json", encoding="utf-8")
        self.assertEqual(guard(self.cat, self.report, 3), "")


def workflow_script(workflow: str, step: str) -> str:
    """The ``python3 - <<'PY'`` body of a named step, dedented exactly as the shell feeds it."""
    text = (PROJECT / ".github" / "workflows" / workflow).read_text(encoding="utf-8")
    start = text.index(f"- name: {step}\n")
    body = text.index("<<'PY'\n", start) + len("<<'PY'\n")
    lines = []
    for line in text[body:].splitlines():
        if line.strip() == "PY":
            break
        lines.append(line)
    return textwrap.dedent("\n".join(lines)) + "\n"


class SyncSummaryTests(unittest.TestCase):
    def test_summary_prints_platform_counts_of_the_committed_catalog(self):
        code = workflow_script("catalog-sync.yml", "Summary")
        env = {k: v for k, v in os.environ.items() if k != "GITHUB_STEP_SUMMARY"}
        proc = subprocess.run([sys.executable, "-"], input=code, cwd=PROJECT, env=env, capture_output=True,
                              text=True, timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        with open(PROJECT / "catalog" / "xeno-catalog.json", encoding="utf-8") as fh:
            games = expand_catalog(json.load(fh))["games"]
        ps5 = sum(1 for g in games if g["platform"] == "PS5")
        ps4 = sum(1 for g in games if g["platform"] == "PS4")
        self.assertIn(f"PS5 games: {ps5}", proc.stdout)
        self.assertIn(f"PS4 games: {ps4}", proc.stdout)
        with open(PROJECT / "catalog" / "report.json", encoding="utf-8") as fh:
            platforms = json.load(fh).get("platforms") or {}
        if platforms:
            self.assertEqual((platforms.get("PS5"), platforms.get("PS4")), (ps5, ps4),
                             "report.json and the committed catalog disagree")


if __name__ == "__main__":
    unittest.main()
