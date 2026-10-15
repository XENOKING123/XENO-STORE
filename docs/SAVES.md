# XENO STORE · Save Data Store

The Save Data Store is the community save library built into XENO STORE: PS5
saves shared by other players, checked before they are listed, installed to
your profile with one press. This guide covers how it works on the console,
what the categories and compatibility badges mean, what each re-sign engine
can and cannot do, how to share your own save, and — for the owner — how the
XenoKing site backend, the GitHub index and its review labels work.

PS5 titles only (`PPSA…` ids). PS4 saves (`CUSA…`) are not listed, and there
is no CUSA → PPSA conversion: there is no safe generic converter, so the store
says so instead of pretending.

---

## 1. Where it is on the console

* **Game page → Saves (n)**: every PS5 game page has a Saves tab next to
  Download with the saves for that exact title id.
* **Library → Community Saves**: browse everything, search by game, title id
  or author, category chips, "Compatible with my games", "My saves", and
  **Upload my save**.
* **Discover → New community saves** rail.

Saves are listed from two feeds, merged and deduped (by sha256 / Garlic id):

| feed | what | where it comes from |
|---|---|---|
| **XenoKing** (the live backend) | the community collection of the owner's XenoKing site — `public.ps5_save_collection` of its Supabase project, named in `updates/saves-backend.json` (§9) | every save shared on the site or from XENO STORE, each credited to its sharer; Garlic Save DB saves the site mirrors |
| GitHub index | `updates/saves.json` in this repo (cached 1 h on the console) | submissions through GitHub issues, reviewed by the XENO team (§7) |

A console with no backend settings follows the published
`updates/saves-backend.json`: it fetches it with the catalog and, when its copy
is missing or older than 6 hours, again before it lists saves, so a fresh
console (or one with catalog auto-refresh off) still shows the site's saves.
The list itself is cached for an hour (a refresh on the Saves screen asks
again). Settings → Saves → **Sources** lets you add more `saves.json` URLs
(another curated index), and **Backend** lets you point one console at another
database without changing the repo ("Published default" follows the file,
"Off" hides the community store).

Each XenoKing save shows the sharer (`shared_by`) as its author, the game as
its title, a name made of the first sentence of the sharer's notes (cut to
about 80 characters; "<game> save" when there are no notes — the site's file
names are download names and never shown), the full notes as its description,
the cover when the site has one, and a "On XenoKing" chip.

## 2. Categories

| chip | `category` | typical description |
|---|---|---|
| Maxed Starter | `starter` | start of the game with money, levels, gear, NG+ unlocked |
| 100% Platinum | `platinum` | everything done, platinum-ready or earned |
| Trophy checkpoint | `checkpoint` | right before a boss, a missable, a specific trophy |
| Modded / Glitched | `modded` | edited values, glitched states — use at your own risk |
| Chapter | `chapter` | a specific chapter / act / mission |
| Other | `other` | anything else |

When a submission has no category, the store infers one from the text
(`100%`/`platinum` → platinum, `start`/`ng+`/`max` → starter, `trophy`/`boss`
→ checkpoint, `mod`/`glitch` → modded, `chapter` → chapter). The same rule
runs on the console, in the intake tool and in the database sync, so chips
match everywhere.

## 3. Compatibility (what the badges mean)

For every save the console checks your own installation before you press
anything:

| badge | meaning |
|---|---|
| ✓ Matches your 01.004.000 | the game is installed and the save was made on your version |
| ⚠ Made on 01.004.000, you have 01.000.000 | update the game first (one tap opens the game's Updates); older game + newer save usually fails to load |
| ⚠ Compatibility could not be checked | the save has no version (Garlic layout) — compare before installing |
| ✗ Game not installed | listed so you can download it for later |
| Other regions | same game, different title id — **a save never loads on a different title id** |

Incompatible saves are hidden by default ("Show all" reveals them). The
installed version comes from `/system_data/priv/appmeta/<titleId>/param.json`
(`contentVersion`), falling back to the app's own `param.json`.

## 4. The re-sign engine — three tiers, reported honestly

PS5 saves are sealed to the console and account that created them. To make a
community save load on *your* profile it has to be written through the
console's own save system (or re-signed). XENO STORE picks the best engine it
finds and shows which one it used; `GET /api/saves/engine` reports the same.

| tier | shown as | needs | what it does | limits |
|---|---|---|---|---|
| 1 · **native** | "Native" | a verified PS5 binding of the SaveData mount API | creates/mounts the save for your user and title id, copies the decrypted files in, sets the account id in `param.sfo` to yours, unmounts. Export does the reverse and strips the account id. | **disabled on the console**: the PS4 prototypes it used do not match the PS5 library (`sceSaveDataMount2` takes no title id, `sceSaveDataTransferringMount` takes a parameter struct) and the API is unavailable on 12.x. The host build keeps a simulation of it for the tests |
| 2 · **garlic** | "via Garlic" | [Garlic Save Manager](https://github.com/earthonion/garlic-savemgr) (by earthonion) answering on `127.0.0.1:8082` — XENO STORE starts it when a save needs it (§4.1) | imports the decrypted files through Garlic's own API (`create_pfs` → `upload_file` → `import_check` → `import_finish`, which re-signs the save for your account); shareable exports come from `export_title_dec` | XENO STORE does not bundle or rehost Garlic: it uses a copy already on the console, or downloads the pinned release from the author's own GitHub release and checks its SHA-256 |
| 3 · **raw** | "Same console only" | nothing | copies the folder `/user/home/<uid>/savedata_prospero/<TITLEID>/` directly (after an automatic backup) | works only for saves **made on this console and profile** (your own backups). A community save is never installed this way (it would not load); a profile-to-profile transfer is re-signed through Garlic when it can run and falls back to this copy, with a warning, when it cannot |

The UI labels tier 3 clearly and shows what community saves need when only the
raw tier is available; the host build simulates tier 1 so the whole flow is
covered by `tests/host/test_ext_console.py` (`test_20_store_list_compatibility`,
`test_21_store_install_native_sim`, `test_22_export_upload_submit`), and the
Garlic tier by `tests/host/test_saves_garlic.py` against a stand-in that follows
Garlic's `main.c` route by route (install, wrapped and zeroed saves, export,
profile transfer, auto-start, running-title refusals). The XenoKing backend is
covered by `tests/host/test_saves_site.py` against `supabase_mock.py`, a
stand-in for the site's Supabase project with its exact columns (listing of
the 57 current rows, Garlic and bucket installs, share = upload + insert, the
GitHub fallback with a public copy instead of a private-bucket link, the
user's own submit endpoint, shares before any listing, no fallback to the
site for an unusable Settings backend, a template deployment typed into
Settings with the migrations' bucket policies replayed, no other writes, the
anon key never leaving the site).

### 4.1 Garlic Save Manager status

When the native tier is not available, the Community Saves screen, the game's
Saves tab and the Install sheet show Garlic Save Manager's state
(`GET /api/saves/garlic`):

* **running** — "Garlic Save Manager 1.7 is running": installs and shareable
  exports go through it.
* **not running** — "Needs Garlic Save Manager" with **Start Garlic Save
  Manager** (a copy was found on the console) or **Download and start Garlic**
  (the pinned upstream release is fetched from the author's GitHub release and
  checked by SHA-256). The button runs `POST /api/saves/garlic/start`: the ELF
  goes to the ELF loader on port 9021 and the console waits up to 20 s for
  Garlic to answer. Installing a save does the same on its own, so the button
  is only a shortcut.

Offline-activated accounts (Settings → Accounts) have an account id too, which
tier 1 uses when PSN is absent.

## 5. Installing and downloading a save

On a save card:

* **Install to my saves** — when the native tier is not available it first
  makes sure Garlic Save Manager answers (starting it if needed); if it cannot,
  the job fails within seconds with "needs Garlic Save Manager" (error code
  `garlic_unavailable`) and **nothing is downloaded** — the toast offers Start
  Garlic Save Manager, and Download file still works. Then it downloads the zip
  (console job `save-install`), verifies its sha256 against the index and
  imports it with the engine above. "Back up my current save first" is on by default
  (`.pre-restore` zip under `<drive>/xeno-store/saves/<user>/`). Toast: "Save
  installed — launch the game".
* **Download file** — just saves the zip to
  `<drive>/xeno-store/saves/community/<game>/` (job `save-download`); reach it
  from a PC via XENO TOOL / FTP.
* **Report** — opens a prefilled GitHub issue for a broken or wrong save.

How a XenoKing save is fetched: a save the site mirrors from the Garlic Save DB
(`file_path = "garlic:<id>"`) comes through the site's `garlic-savedb`
function, which signs in to Garlic for each download — when Garlic answers with
an error the job says so ("XenoKing could not fetch this save from Garlic …");
a save stored in the site's own bucket comes through a 5-minute signed URL.

Download counts: the XenoKing site has no download counter, so the console
calls none (a template deployment that names one in its config —
`"downloadCounter": "increment_downloads"` — or one typed into Settings that
the console recognises as the template, §9.2, gets a best-effort call after a
verified download or install). The GitHub index has no counter either, so its
rows show whatever `downloads` value the index file carries (0 for intake
rows) and the "Most downloaded" rail and ranks only appear once counts exist.

## 6. Sharing your own save (Upload my save)

Library → Community Saves → **Upload my save**:

1. pick the profile, then the PS5 title (only installed titles with saves are
   offered; name, cover, installed version and region are filled in from the
   catalog / appmeta),
2. pick the save directory,
3. the engine **exports** it: decrypted files + `sce_sys/param.sfo` with the
   account id zeroed + `sce_sys/icon0.png` + the `xeno-save.json` manifest.
   The decrypted files come from the native tier or, without it, from Garlic
   Save Manager's `export_title_dec` (started if needed), repacked by the
   console: sealed keys (`sealedkey`, `keystone`) and raw images are dropped,
   `ACCOUNT_ID` is zeroed and checked again in the finished zip. The wizard
   says "Account ID removed" only when the console zeroed it ("No account ID
   in this save" when the save has no `param.sfo`). When neither engine is
   available, or the account id cannot be removed, **nothing is exported for
   sharing**: the wizard stops, explains why and offers to start Garlic. A raw
   copy only loads on this console, so it is never uploaded — the upload step
   itself also refuses raw images, sealed keys, `xeno-backup.json`,
   `sameConsoleOnly` manifests and a `param.sfo` that still carries an account
   id (`not_shareable`).
4. **upload** — to **XenoKing** (the default whenever the site is the
   backend: the zip goes into the site's `ps5-saves` bucket as
   `xeno/<yyyymmdd>-<16 hex>.zip`, never overwriting anything), or to
   pixeldrain, catbox (≤ 200 MB) or your own endpoint (Settings → Saves →
   Upload host "custom": multipart `file` → `{url}`),
5. fill the form: category chips, name, description (≤ 300), tags, nickname —
   game, version, region, size and sha256 are already there,
6. **submit**:
   * uploaded to XenoKing: the console inserts one row into the site's
     collection — `title_id`, `game`, `shared_by` (your nickname), `notes`
     ("<name> — <description>", trimmed to 300 characters, your tags added as
     `#tag` words when they fit), `file_path` (the uploaded object),
     `file_name` (`<TITLEID>_<title>.zip`) and `file_size` — nothing else
     (the site keeps no category, version or region: its chip is inferred
     from the notes like any site row, §2). The save is **live on the
     XenoKing site and in the store at once** (the
     console rebuilds its list right after the insert); there is no GitHub
     step.
   * when the site refuses the row, or the zip went to another host, the
     wizard says so and falls back to your own submit endpoint (Settings →
     Saves → Submit endpoint, when set) or the GitHub hand-off below (the
     reason is in the console log too). A zip that is only in the site's
     bucket cannot go to GitHub as it is — that object opens with the site's
     key only — so the console first uploads the same export to a public host
     (your Settings upload host, pixeldrain when that is XenoKing) and the
     issue carries that link; if that upload fails too, nothing is handed off
     and the wizard asks you to try again.
   * GitHub (no site backend, or the fallback): the console shows a QR code
     and a short link to a prefilled issue; open it on your phone and press
     *Submit new issue*. The checks (§7) run within minutes and comment on the
     issue; the XENO team labels it `approved` and it appears in every store
     within the hour.
   * a template deployment (§9.2: marked `"moderated": true` in its config, or
     recognised when typed into Settings) inserts the row as `pending`; it
     goes live when its owner publishes it.

Privacy: no account id, console id or PSN name leaves the console. The only
name attached to a save is the nickname you type.

### The xeno-save-zip format

```
SAVEDATA00.bin …           decrypted save files at the zip root
sce_sys/param.sfo          ACCOUNT_ID zeroed
sce_sys/icon0.png          used as the card cover
xeno-save.json             {titleId, dirName, kind:"ps5", format:"xeno-save-zip", gameVersion, region,
                            accountIdStripped:true, sameConsoleOnly:false, createdBy:"XENO STORE 1.0.0",
                            createdAt, files:[{path,size,sha256}]}   (+ engine:"garlic" on Garlic exports)
```

Garlic's "decrypted save zip" (same files, optionally wrapped in one folder,
no manifest) is accepted as-is everywhere: the title id comes from the index
row, version/region from the row when present, otherwise the badge says
"could not be checked". XENO uploads use the same layout plus the manifest, so
they also work in Garlic and Apollo-style tools.

## 7. What the checks enforce (GitHub intake)

`tools/saves/intake.py` runs in `.github/workflows/saves-intake.yml` on every
`save-submission` issue (opened, edited, labeled):

1. **Fields** — PPSA title id; title, name, nickname lengths; description
   ≤ 300; category from the list (or inferred); version like `01.004.000`;
   region letters; `dirName`; size ≤ 2 GiB; 64-hex sha256; a plain `https://`
   link without credentials; up to 10 short tags; `format` must be
   `xeno-save-zip` and `kind` `ps5`.
2. **Download** — streamed with a cap (never more than the declared size, never
   more than 2 GiB); HTML pages instead of files, HTTP errors and empty files are
   reported in plain words.
3. **Hash + size** must match the submission exactly.
4. **Zip** — must contain decrypted save files at the root (or in one wrapper
   folder), optionally `sce_sys/param.sfo` / `icon0.png`; must **not** contain
   `sealedkey`, `keystone`, `sdimg_*` raw images, `xeno-backup.json`, absolute
   or `..` paths or symlinks; `sce_sys/param.sfo`, when present, must not carry
   an `ACCOUNT_ID` (zeroed, or an all-zero file); a `xeno-save.json` manifest
   must match the title id, must not say `sameConsoleOnly` or
   `accountIdStripped:false`, and every file it lists must be in the zip with
   the right size.
5. **Duplicates** — the same file (sha256) already published or pending under
   another issue is refused.

Then the index row is written with status from the labels:

| labels on the issue | status | consoles |
|---|---|---|
| none / only `save-submission` | `pending` | hidden |
| `approved` | `published` | listed |
| `rejected` (wins over approved) | `rejected` | hidden, issue closed |
| any failed check, whatever the labels | `rejected` (new issue: nothing recorded; known entry: kept with `reasons`) | hidden |

Every run posts (or updates) one comment on the issue with the outcome, the
size, the hash, the layout and the file count, signed "XENO team · automated
intake". The index commit message is `saves: intake #<n> [skip ci]`.

`updates/saves.json` ships with three entries of `status: "example"` that show
the record shape; the app ignores any status other than `published`.

Run the checks yourself:

```sh
cd xeno-store
python3 -I tools/saves/intake.py --body issue.md --number 1 --file save.zip --dry-run   # local zip, no write
python3 -I tools/saves/intake.py --validate-index updates/saves.json
python3 -I -m unittest discover -s tools/saves/tests -v      # or: make saves-test (part of make test and CI)
```

The tests write their fixtures (zips, issue body, Actions event) into a
temporary directory with `tools/saves/tests/fixtures/make_fixtures.py`, so a
fresh checkout needs nothing else.

## 8. For the owner: moderation day to day

* A new issue with the `save-submission` label arrives → the workflow checks
  it and comments. Read the comment (and the save's description), then add
  **`approved`** or **`rejected`**. The workflow runs again, rewrites the index,
  commits, updates its comment and closes the issue.
* To pull a published save: add `rejected` (reopen the issue first if it was
  closed, or edit `updates/saves.json` by hand — the tool keeps hand edits
  as long as the file stays valid; `--validate-index` tells you).
* Submissions by hand (no console): the template at
  `.github/ISSUE_TEMPLATE/save-submission.md` explains every field.
* Labels are the only switch: nobody without triage rights on the repo can
  publish, because only collaborators can add labels.

## 9. For the owner: the database backend (Supabase)

### 9.1 The XenoKing site (live)

`updates/saves-backend.json` names the site: `kind: "supabase"`, `name:
"XenoKing"`, the project URL, its public anon key (the same key every visitor
of the site already gets), bucket `ps5-saves`, function `garlic-savedb`.
Consoles pick a change up within 6 hours (with the catalog check, or the
next time they list saves; `POST /api/feeds/refresh` forces it). What a
console does there, and nothing more:

| step | request |
|---|---|
| list | `GET /rest/v1/ps5_save_collection?select=*&order=created_at.desc&limit=2000` |
| cover | `POST /storage/v1/object/sign/ps5-saves/<cover_path>` (1 h, reused ~50 min) |
| download, bucket row | `POST /storage/v1/object/sign/ps5-saves/<file_path>` (5 min), then a plain GET |
| download, Garlic row | `POST /functions/v1/garlic-savedb {"action":"download","id":<garlic_id>}` |
| share: upload | `POST /storage/v1/object/ps5-saves/xeno/<yyyymmdd>-<16 hex>.zip` (`x-upsert: false`) |
| share: insert | `POST /rest/v1/ps5_save_collection` with the site's own columns (`Prefer: return=representation`) |

No update, no delete, no other table, no RPC, no listing through the
function. The anon key goes with these requests to the site's own URL only
(same scheme, host and port) and never across a redirect. Every share is a
row in the site's collection, shown on the site straight away (the table
takes anonymous inserts by the owner's choice); to remove one, delete the row
(and its `xeno/…` object) in the site's dashboard.

`tools/saves/site_check.py` checks the live site from CI without writing
anything (`.github/workflows/site-check.yml`, run by hand or by a push to a
branch named `site-check`): it lists the rows (≥ 50, required columns) and
downloads one small Garlic save through the function.

### 9.2 Your own deployment (template)

Deploy steps, schema and the edge functions live in
[`backend/supabase/README.md`](../backend/supabase/README.md). In short:

1. run `migrations/0000 → 0001 → 0002 → 0004 → 0005` (0003 is the optional
   Garlic sync cron; 0005 lets consoles upload their shares as
   `xeno/<yyyymmdd>-<16 hex>.zip`, which 0004's bucket policy refused),
2. set the secrets (Garlic cookies for the mirror, optional `XENO_SYNC_TOKEN`,
   optional `XENO_AUTO_PUBLISH=1`),
3. `supabase functions deploy xeno-save-submit` (and `garlic-savedb` for the
   mirror),
4. put the project URL + anon key into `updates/saves-backend.json`
   (`backend/supabase/config/saves-backend.example.json` is the template:
   keep its `"moderated": true` and `"downloadCounter"`) or into Settings →
   Saves → Backend on a console. Settings has no such flags: the console
   asks the table once (`GET …?select=status&limit=1` — 200 on the template,
   "no such column" on the XenoKing site) and, for the template, sends
   `status: "pending"`, `source: "xeno"` with each share and calls
   `increment_downloads` after a download. The URL must be `https://`; a
   setting that cannot be used (no key, a plain-`http` URL kept from an older
   version) never falls back to the published site — the console lists the
   GitHub index only and says why (`backendError`).

Console uploads land as `status = 'pending'` (row-level security refuses
anything else from the public key); publish them from the Table editor or let
`xeno-save-submit` publish what passes its server-side check. The function
reads only the zip's central directory (one 1 MiB `Range` request) to refuse
sealed keys, raw images and bad paths — it does not hash a 2 GiB object; the
console verifies the sha256 after download.

Garlic mirror rows (`file_path = 'garlic:<id>'`) stream from garlicsaves.com
through the function on demand and depend on a live Garlic session (cookies
expire after days to weeks; refresh them when downloads start failing — the
XENO uploads and the GitHub index keep working regardless).

## 10. Honest limits

* **Re-signing needs the native tier or Garlic.** Without save patches on your
  firmware and without Garlic (and without a way to start it: no copy on the
  console and no ELF loader on port 9021), community saves can only be
  downloaded (and restored as-is, which works for saves made on the same
  console), and your own saves cannot be shared.
* **Version and region matter.** A save made on a newer game version may not
  load on an older one; a save never loads on another region's title id. The
  badges tell you before you install; "Show all" is there for people who know
  what they are doing.
* **Modded / glitched saves** can break trophies or progression. They are
  labelled, not hidden.
* **No PS4 saves, no conversion.**
* **Hosting is third-party** (the XenoKing site's Supabase project, Garlic,
  pixeldrain, catbox). Garlic downloads depend on the site's Garlic session;
  links can die; "Report" sends the XENO team a prefilled issue.
* **Shares from XENO STORE are not reviewed first**: they go straight into the
  XenoKing collection (the owner's choice). The console still refuses
  anything that is not a real decrypted export (§6, step 3) before uploading.
* **Nothing is faked**: every engine tier, every check and every failure is
  shown as it happened, with the reason.
