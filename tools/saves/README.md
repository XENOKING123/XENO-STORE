# tools/saves — Save Data Store intake

`intake.py` turns a `save-submission` GitHub issue into a row of
`updates/saves.json` (SPEC §17.22.5). Stdlib only, Python 3.10+.

```
tools/saves/
  intake.py                 parse issue body → validate → capped download → sha256 → zip inspection → index upsert
  site_check.py             read-only live check of the XenoKing site (the store's database backend): list + one
                            small Garlic download; run by .github/workflows/site-check.yml on demand only
  tests/test_site_check.py  site_check.py against tests/host/supabase_mock.py (offline)
  tests/test_intake.py      47 tests (parsing, validation, download against a local HTTP server, zips, param.sfo
                            ACCOUNT_ID, index, end-to-end runs); writes its fixtures into a temp directory
  tests/fixtures/           make_fixtures.py (deterministic) → good-save.zip, garlic-save.zip, sealed-save.zip,
                            raw-backup.zip, issue-good.md, event-opened.json (only the .md/.json samples are committed)
```

| command (from the repository root) | what |
|---|---|
| `python3 -I tools/saves/intake.py --event "$GITHUB_EVENT_PATH" --index updates/saves.json --out result.json` | what the workflow runs |
| `python3 -I tools/saves/intake.py --body issue.md --number 12 --labels approved --file save.zip --dry-run` | check a local zip against an issue body, write nothing |
| `python3 -I tools/saves/intake.py --issue-json issue.json` | input from `gh issue view N --json number,title,body,labels,createdAt,url` |
| `python3 -I tools/saves/intake.py --validate-index updates/saves.json` | schema check of the index (exit 1 on errors) |
| `python3 -I -m unittest discover -s tools/saves/tests -v` | tests (also `make saves-test`, part of `make test` and CI host-tests) |
| `python3 -I tools/saves/tests/fixtures/make_fixtures.py [OUT_DIR]` | write the fixtures (same bytes every run with the same zlib) |
| `python3 -I tools/saves/site_check.py [--backend updates/saves-backend.json] [--garlic-id 94] [--min-rows 50]` | live check of the site backend: ≥ 50 rows with the required columns, Garlic save 94 downloads as a zip with `sce_sys/param.sfo`; writes nothing (exit 1 on a failure) |

Exit codes: 0 the intake ran (the result JSON holds the verdict), 1 tool
error (unreadable index, bad arguments), 2 with `--strict` when the submission
was rejected. The result JSON has `ok`, `status` (`pending` / `published` /
`rejected`), `errors`, `warnings`, `checks` (size, sha256, layout, dataFiles,
manifest), `entry`, `changed`, `comment` (the Markdown the workflow posts).

Zip rules worth knowing: a `sce_sys/param.sfo` whose `ACCOUNT_ID` is still set is refused (a shared save
must not carry the uploader's account id; XENO STORE zeroes it on export). An all-zero `param.sfo` is still
accepted, but the pinned Garlic Save Manager 1.7 refuses such a save on import ("param.sfo is corrupted!");
only builds after 1.7 rebuild it. The other zip checks are listed in
`docs/SAVES.md` §7.

Index rules: entries with `status: "example"` are left untouched at the top;
intake rows are keyed by issue number, sorted by it, written with a fixed key
order, 2-space indent and a trailing newline, so an unchanged run produces the
same bytes and the workflow has nothing to commit. See `docs/SAVES.md` §7 for
the checks and the label → status table.
