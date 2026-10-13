"""The committed catalog report and revision keep the shape the sync workflow reads.

The scheduled sync (.github/workflows/catalog-sync.yml) prints a per-source
summary from catalog/report.json before it commits; if the two drift apart the
job fails and no fresh catalog is ever published.
"""
from __future__ import annotations

import json
import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


def load(rel: str):
    with open(os.path.join(PROJECT, rel), encoding="utf-8") as f:
        return json.load(f)


class ReportShapeTests(unittest.TestCase):
    def test_sources_is_a_list_of_runs(self):
        rep = load("catalog/report.json")
        self.assertIsInstance(rep.get("sources"), list)
        self.assertTrue(rep["sources"], "the report lists at least one source run")
        for run in rep["sources"]:
            for key in ("id", "status", "games", "releases"):
                self.assertIn(key, run)

    def test_workflow_summary_reads_the_report(self):
        # Mirrors the Summary step: it must run against the committed files.
        rev = load("catalog/revision.json")
        rep = load("catalog/report.json")
        lines = ["revision %s · %s games" % (rev.get("revision"), rev.get("games"))]
        for info in rep.get("sources") or []:
            lines.append("  %-14s %-9s %6s games  %6s releases" % (info.get("id", "?"), info.get("status", "?"), info.get("games", ""), info.get("releases", "")))
        self.assertGreater(len(lines), 1)

    def test_catalog_has_several_sources(self):
        cat = load("catalog/xeno-catalog.json")
        ids = {r.get("sourceId") for g in cat["games"] for r in g.get("releases", [])}
        self.assertGreaterEqual(len(ids), 3, ids)


if __name__ == "__main__":
    unittest.main()
