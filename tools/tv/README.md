<!--
Copyright (C) 2026 XENO STORE contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->

# TV app tools

This folder holds the tools for the TV app in `app/tv/`:

| File | Owner and purpose |
|---|---|
| `preview_live.py` | The live preview harness. It renders every screen of the TV app against the real host backend and checks each run. Details below. |
| `world.py` | The harness's temporary test world: catalogue, feeds, settings, Library files, the backend, the loaders and the recording proxy. |
| `fake_loader.py` | A stand-in for the PS5 ELF loader, so start-up and restart run for real on a PC. |
| `art_server.py` | Serves the generated covers and heroes, plus the images the backend must refuse. |
| `contact_sheet.py` | Writes the review page `index.html` for a run, or for any folder of PNGs. |
| `scenarios.json` | The harness's data: scenario groups, world settings, captures and expectations. |
| `stub_preview.py` | A stand-in for the preview binary (`--dry-run`), so the harness can be tested without building the app. |
| `check_param.py`, `make_source_bundle.py` | Packaging tools (see their own docstrings). |

Every script runs under `python3 -I` with the standard library and PIL (`python3-pil`).

## Live preview: `preview_live.py`

The goal is PORT-SPEC 4.1. Every screen of the ported app is rendered at 1920x1080 against **our real host backend**, in a deterministic test world. The pictures go to PNG files for review, and six checks run automatically on each run.

```sh
make assets && make host            # build/host/xeno-store
make tv-preview                     # builds app/tv/build/preview/xeno_preview (and the fixture pictures)
python3 -I tools/tv/preview_live.py                  # English, every group -> build/tv-preview/
python3 -I tools/tv/preview_live.py --langs all      # plus the locale sweep in the 10 other languages
python3 -I tools/tv/preview_live.py --scenarios 'discover,settings-*' --compare old-run/
python3 -I tools/tv/preview_live.py --dry-run        # no app build needed: stub_preview.py stands in
```

The exit status is 0 when every check passed, 1 when a check failed, and 2 when the run could not start. The run cannot start when the backend, the preview, its assets or `tests/host/tvworld.py` are missing, or when nothing matched the filters.

`make tv-preview-live` runs it (`LANGS=all` adds the locale sweep). It builds the host backend and rebuilds the preview binary only when it is missing or older than the app's sources.

### Options

| Option | Meaning |
|---|---|
| `--out DIR` | The output folder (default `build/tv-preview`). The harness clears its own entries there first: `<lang>/`, `logs/`, `thumbs/`, `report.json` and `index.html`. |
| `--langs en\|all\|de,fr` | `all` adds the locale sweep: scenarios.json `"sweep"` (`discover`, `game`, `downloads`, `library-details`, `settings-*`) in every `app/tv/i18n/*.json` locale except English, written to `<out>/<lang>/`. Review these pictures for text overflow. |
| `--scenarios a,b*` | Run only these scenarios (comma list, `*` allowed), in their groups. |
| `--groups g,h` | Run only these groups. |
| `--compare DIR` | Compares each picture with the same file in a previous `--out`, as the mean absolute luma difference (0-255). Pictures over `--compare-threshold` (default 6) are listed in `report.json` `compare.flagged`. `--compare-fail` makes a flagged picture fail the run. |
| `--backend-bin`, `--preview`, `--assets`, `--table` | Alternative paths. `$XENO_BIN` also sets the backend. |
| `--dry-run` | Runs `stub_preview.py` instead of the app. |
| `--direct` | The preview talks to the backend port directly, with no recording proxy. Check 3 then sees nothing. |
| `--keep` | Keeps the temporary worlds (their paths are printed). |
| `--llvmpipe-threads N` | Sets `LP_NUM_THREADS` for the preview. Use it on a shared machine. |
| `--no-sheet` | Skips `index.html`. |

### What one run does

For every language and every group of `scenarios.json`:

1. **World** (`world.py`): a temporary folder holding:
   - `data/catalog.json` (revision 2000000000), built from `tests/host/tv/fixture-catalog.json` with `{{FILES}}` and `{{ART}}` pointing at this run's servers;
   - `data/feeds/featured.json`;
   - `settings.json`, with sources acknowledged or not depending on the group;
   - the Library files of `tvworld.LIBRARY_FILES` on roots `R1` and `R2`: `PPSA01234.exfat`, `CUSA00001.pkg`, a catalogue-named `.pkg`, a `.xpart`, `PPSA99500.ffpkg`, and `<data>/user/app/PPSA05555`;
   - the TV app image and install record of the group's `tvApp` state;
   - the starter's copies: the bundled ELF (the host binary) with its version file, and for `crash-fallback` a saved ELF that exits at once with version 9.9.9.
2. **Servers** shared by the whole run:
   - `tests/host/range_server.py` for the release files. `slow/` keeps jobs downloading, `f404/` fails them, and `r429/` makes them retry.
   - `art_server.py` for the art.
   - A feed server with the store update feed (the next version, a copy of the host binary), the TV app feed, `catalog/revision.json` (answered after 2 s so a refresh shows as busy) and a GitHub API stand-in.
3. **Backend**: `build/host/xeno-store --port B --data D --root R1 --root R2 --no-browser`. Its environment holds `XENO_HOST_ART_FIXTURE`, `XENO_HOST_RESOLVE_HOST`, `XENO_TVAPI_UPDATES=1`, `XENO_APP_BASE`, `XENO_GITHUB_API_BASE`, and `XENO_PORT_LOADER`, which points at a record-only sentinel loader.
   - For the start-up groups the backend stays down. `fake_loader.py` listens instead, and starts the backend when the app sends it the bytes.
4. **Proxy**: the app is given a recording proxy rather than the backend port. The proxy listens only while the backend listens, so a stopped backend shows up as a refused connection, as on the console. It logs every request's method, path, status and error body to `logs/<lang>/<group>/requests.jsonl`.
5. **Seeder** (in `preview_live.py`): drives `/api/v1` as the app does. It uses one request per connection, a Bearer token from `GET /api/v1/session`, and JSON bodies. The group's `seed` steps are:
   - `pair`, `acknowledge` and `settings` (concurrent downloads).
   - `downloads`, which makes the six Downloads rows on R1: complete, error (404), paused, cancelled with the partial kept, downloading (throttled 64 MiB), and retrying. The retrying row gets 429 until the world closes and then succeeds. range_server has no 503 route, and 429 takes the same backoff path.
   - `downloads-light`, `finished-download` and `library-copy`.
   - `block-queue`, which turns `queue.json` into a directory so that `/system` reports `stateHealthy:false`.
6. **Preview**: `app/tv/build/preview/xeno_preview <assets> <tmp> <names>` runs once per group. Its environment holds:
   - `XENO_BACKEND=127.0.0.1:<proxy>`, `XENO_TODAY=2026-10-15`, `XENO_LANG`, `XENO_APP_VERSION` and `XENO_SCENARIOS` (the group's scenarios);
   - `XENO_LOADER_PORT` when the group has a loader;
   - `XENO_STARTER_{SAVED,BUNDLED}_{ELF,VERSION}`;
   - the llvmpipe variables (`EGL_PLATFORM=surfaceless`, `LIBGL_ALWAYS_SOFTWARE=1`, `GALLIUM_DRIVER=llvmpipe`).
7. **Lines from the preview** (stdout, PORT-SPEC 2.9):
   - `TVMARK {"scenario":..,"mark":..}`: the harness changes the world at once. `stop-backend` kills the backend, `remove-drive` renames R2 away, and `restore-drive` brings it back.
   - `TVSTATE {...}`: ends a scenario. The harness then moves its pictures to `<out>/<lang>/<capture>.png`. When a group has a `variant` (the `tvApp` states), the name gets `.<variant>` before `.png`.

When the preview answers `--list` (one JSON line per scenario: name, fixture, live, ownWorld, waitReady, captures, marks), the harness takes the capture and mark names from it. Any difference from `scenarios.json` is listed under `report.json` `scenarioList.drift`. A planned scenario that the binary does not have, or cannot run live, is a setup failure and is skipped. Live scenarios that no group runs are listed under `scenarioList.notCovered`.

### Groups (`scenarios.json`)

| Group | World | Scenarios |
|---|---|---|
| `offline` | no backend, no loader | `offline` |
| `no-loader` | no backend, closed loader port | `no-loader` |
| `cold-start` | backend down, fake loader | `cold-start` (1 send) |
| `crash-fallback` | as cold-start, plus a saved copy that exits at once | `crash-fallback` (2 sends: `copy=saved`, then `copy=bundled`) |
| `setup` | sources not acknowledged | `setup` |
| `downloads` | the six seeded rows | `downloads`, `downloads-finished`, `downloads-dialog`, `delete-cancelled`, `downloads-actions`, `queued`, `download`, `game`, `game-viking`, `game-ps4`, `game-library` |
| `store` | one finished download | `discover` (with the art check), `browse*`, `library-details`, `library-storage`, `settings`, `settings-*`, `library` (`remove-drive` mark) |
| `library-copy` | default | `library-copy` (the app starts and follows the copy) |
| `tv-app-orbit`, `tv-app-manual` | TV app image installed by the app, or by hand | `settings-tv-app` (pictures `*.orbit.png`, `*.manual.png`) |
| `restart` | fake loader, one throttled download | `restart` (1 send, drain, no relaunch by the store) |
| `connection-lost` | default | `connection-lost` (`stop-backend` mark) |
| `state-unhealthy` | blocked queue file | `state-unhealthy` |

Order matters inside a group: `downloads-actions` deletes the cancelled job, and `settings` changes the default drive before `settings-storage` sets it back to automatic.

`worldDefaults` and each group's `world` member accept these keys:

| Key | Values |
|---|---|
| `backend` | `up` or `down` |
| `loader` | `none`, `closed` or `fake` |
| `saved` | `crash` |
| `acknowledged` | true or false |
| `library` | true or false |
| `tvApp` | `none`, `orbit` or `manual` |
| `updates` | true or false |
| `concurrentDownloads` | a number |
| `seed` | a list of seed steps |
| `releaseRoutes` | `{releaseId: range_server route}` |
| `loaderDelay` | seconds before the fake loader runs a copy (default 1) |

### The six checks

The run fails on any of these, and every failure is listed in `report.json` and on the contact sheet.

1. **Captures.** Every expected capture exists, and the PNG is not uniform: its luma standard deviation is above 4, and more than 1% of its pixels differ from the most common value by more than 2.
2. **State.** Each scenario's `TVSTATE` has empty `linkError`, `libraryError` and `downloadsError`, unless the scenario expects one (`offline`, `no-loader`, `connection-lost`). No toast, `*Error` field or `errorsSeen` entry contains "unreadable". Every mark the scenario lists was reached.
3. **Backend.** The proxy saw no 5xx on `/api/v1/*`. There are two exceptions: an art 502, and in `restart` a 503 whose body says the store "is stopping". There are no requests to `/zero/companion`. Repeated identical failures are counted once, with "(N times)".
4. **Loader.**
   - The fake loader received exactly the expected sends: 1 for `cold-start`, 2 for `crash-fallback` and 1 for `restart`.
   - For `crash-fallback`, `copy=saved` then `copy=bundled` appear, in that order, in the app's starter log (TVSTATE `starterLog` or the preview's stderr).
   - For `restart`, the store log shows `stop drain done` with no workers left, no self-relaunch (`update: restart requested` or `relaunch failed`), and a second instance starting.
   - In every group, the sentinel on the store's own `XENO_PORT_LOADER` received nothing.
5. **Art.** After `discover`, the art server served the featured heroes and the covers of the first four "New on" games. `GET /api/v1/art/<id>/<kind>` then answers 200 from the cache (502 would be a FAILED entry).
6. **Timing.** Each scenario finishes within its `maxSeconds` (default 60; `crash-fallback` 120, because it waits 30 s by design), and its TVSTATE lists no `timeouts`.

Problems with the run itself also fail it, and are listed as `setupFailures`: a seed step that failed, a preview that crashed or hung, an unreadable TVSTATE line, or an unknown mark.

### Output

| Path | Content |
|---|---|
| `<out>/<lang>/<capture>[.<variant>].png` | The pictures. |
| `<out>/index.html` | The contact sheet: one grid per language, grouped by scenario. Failed pictures are outlined with the reason. Each thumbnail links to its full PNG, and the page links to `report.json`. |
| `<out>/report.json` | `ok`, `mode`, `checks.{captures,state,backend,loader,art,timing}` (each with `ok` and `failures`), `setupFailures`, `scenarioList`, `groups[]`, `pictures[]` and `compare`. `groups[]` holds each group's seed results, marks, every TVSTATE, the request count, the loader sends and the art results. `pictures[]` holds each picture's stats and failures. |
| `<out>/logs/<lang>/<group>/` | The group's logs: `preview.out` and `preview.err`, `requests.jsonl`, `store-log.txt` (the store's own log), `backend-N.out`, `loader.jsonl`, `sentinel.jsonl`, the loader's child logs, and `seed.json`. |

No golden pictures are committed. Review happens on the contact sheet, and `--compare` against a previous run flags large changes in a pull request.

## The other tools

**`fake_loader.py`** accepts a connection and reads the ELF until the sender closes (64 MiB at most). It writes the bytes to a temporary file, checks the ELF magic, ELFCLASS64, little-endian and x86-64, and logs `received N bytes`. What happens next depends on the bytes:

- A `--host-binary` (the world's bundled copy, or the update the store saved) runs after `--delay` seconds as `<file> --port P --data D --no-browser --root ...`.
- A `--plain` file (the crash stand-in) runs with no arguments.
- Anything else is logged and not run.

`--record-only` only records. Events go one JSON object per line to `--log`. On SIGTERM it stops its children.

```sh
python3 -I tools/tv/fake_loader.py --port 9021 --host-binary build/host/xeno-store \
    --backend-port 9095 --data /tmp/w/data --root /tmp/w/R1 --log /tmp/w/loader.jsonl --delay 1
```

**`art_server.py`** serves `{{ART}}/art/<gameId>/[fallback/]<name>`.

- The good pictures are deterministic PIL images with the game's title on a gold/blue gradient. Covers are 512x512 WebP, PNG or JPEG. Heroes are 1920x1080, plus the exact-cap sizes of PORT-SPEC 1.7.
- The bad pictures are the refused cases: animated WebP, a 9000 px side, a 2000x2000 PNG cover, a PNG hero just over 2560x1440, CMYK JPEG, square or 1.39:1 heroes, text, empty or truncated files, a file over 12 MiB, AVIF and GIF. They come from `tests/host/tv/artgen.py` when it is present.

`--self-test` checks every picture against the PORT-SPEC 1.7 caps. `--write DIR` dumps the set. `--port N` serves it on its own.

**`contact_sheet.py`** builds the review page.

- With a `report.json`, it follows the report.
- Without one, each folder of PNGs is a section.
- `--dest DIR` writes the page and its thumbnails elsewhere, for a folder that must stay as it is (for example the fixture pictures of `make tv-preview`).

```sh
python3 -I tools/tv/contact_sheet.py app/tv/build/preview/pictures --dest build/fixture-sheet --title "Fixture pictures"
```

**`stub_preview.py`** speaks the preview's interface: arguments, environment, `--list`, TVMARK and TVSTATE. It talks to the backend over HTTP, runs the starter against the fake loader, and writes a labelled picture per capture. These environment switches test the checks themselves:

| Switch | Effect |
|---|---|
| `XENO_STUB_UNIFORM=<capture>` | Writes a blank picture. |
| `XENO_STUB_SKIP=<capture>` | Leaves the picture out. |
| `XENO_STUB_NO_LIST=1` | `--list` fails, like an older preview. |
| `XENO_STUB_LIST_DROP=a,b` | Leaves those scenarios out of `--list`. |
| `XENO_STUB_LIST_RENAME=<capture>` | Lists that capture under another name. |
| `XENO_STUB_START_WAIT=<s>` | Sets how long a started copy has to answer. |

## Adding a scenario

1. Add the scenario to `app/tv/host/preview_main.cpp` with `live_ok` set. Pictures are `capture` steps, and changes to the world are `mark(...)` steps.
2. Add it to `scenarios.json`: its captures, any `expect` (`linkError`, `marks`, `loaderSends`, `starterOrder`, `allow503`, `drain`, `art`) and `maxSeconds`, and put it in a group whose world fits. A new world needs a group of its own.
3. A new mark also needs a case in `World.on_mark` and an entry in `world.MARKS`.
4. Run `python3 -I tools/tv/preview_live.py --scenarios <name>`. `report.json` `scenarioList.drift` shows any name that differs from the binary.
