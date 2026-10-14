<!-- ╔═══════════════════════════════════════════════════════════════════╗ -->
<!-- ║                XENO STORE — PS5 GAME STORE + DOWNLOADER             ║ -->
<!-- ╚═══════════════════════════════════════════════════════════════════╝ -->

<div align="center">

<img src="https://capsule-render.vercel.app/api?type=waving&color=0:060b1c,50:1a2980,100:ffce4a&height=240&section=header&text=XENO%20STORE&fontSize=86&fontColor=ffffff&animation=fadeIn&fontAlignY=38&desc=%F0%9F%8E%AE%20THE%20PS5%20GAME%20STORE%20THAT%20LIVES%20ON%20YOUR%20CONSOLE%20%F0%9F%8E%AE&descAlignY=62&descSize=20" width="100%" alt="XENO STORE"/>

<img src="homebrew/sce_sys/icon0.png" width="128" alt="XENO STORE icon"/>

<br/>

<img src="https://readme-typing-svg.demolab.com?font=Rajdhani&weight=700&size=22&pause=900&color=FFCE4A&center=true&vCenter=true&width=820&lines=%F0%9F%9B%92+1%2C900%2B+PS5+%2B+PS4+games%2C+one+catalog%2C+synced+every+6+hours;%E2%9A%A1+Segmented+downloads+%E2%80%94+up+to+16+connections+per+file;%F0%9F%93%A6+PKG+auto-install+%C2%B7+install+sets+%C2%B7+backports;%F0%9F%92%BE+Save+Data+Store+%C2%B7+backups+%C2%B7+profile+transfer;%F0%9F%93%B1+Runs+on+the+PS5+%E2%80%94+control+it+from+the+couch+or+your+phone" alt="tagline"/>

<br/><br/>

![version](https://img.shields.io/badge/version-1.1.1-FFCE4A?style=for-the-badge)
![platform](https://img.shields.io/badge/PS5-on--console%20payload-2F7CF6?style=for-the-badge&logo=playstation&logoColor=white)
![catalog](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fraw.githubusercontent.com%2FXENOKING123%2FXENO-STORE%2Fmain%2Fcatalog%2Frevision.json&query=%24.games&suffix=%20games&label=catalog&color=2ea043&style=for-the-badge)
![verified](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fraw.githubusercontent.com%2FXENOKING123%2FXENO-STORE%2Fmain%2Fcatalog%2Frevision.json&query=%24.verifiedReleases&suffix=%20verified%20releases&label=audit&color=2f7cf6&style=for-the-badge)
![license](https://img.shields.io/badge/license-GPL--3.0-4c6ef5?style=for-the-badge)

[![release](https://img.shields.io/github/v/release/XENOKING123/XENO-STORE?style=for-the-badge&color=8957e5&label=latest)](../../releases/latest)
[![downloads](https://img.shields.io/github/downloads/XENOKING123/XENO-STORE/total?style=for-the-badge&color=eb5757&label=downloads)](../../releases)
[![stars](https://img.shields.io/github/stars/XENOKING123/XENO-STORE?style=for-the-badge&color=f7c948)](../../stargazers)
[![build](https://img.shields.io/github/actions/workflow/status/XENOKING123/XENO-STORE/build.yml?branch=main&style=for-the-badge&label=build)](../../actions/workflows/build.yml)
[![catalog sync](https://img.shields.io/github/actions/workflow/status/XENOKING123/XENO-STORE/catalog-sync.yml?branch=main&style=for-the-badge&label=catalog%20sync)](../../actions/workflows/catalog-sync.yml)

![discord](https://img.shields.io/badge/Discord-XENOKING123.-5865F2?style=for-the-badge&logo=discord&logoColor=white)

<br/>

### 🟢 [**⬇️  DOWNLOAD XENO STORE  ⬇️**](../../releases/latest)

<sub>`XENO-STORE.pkg` (Games-row app) · `xeno-store-browser.elf` · `xeno-store.elf` · `XENO-STORE.zip` (homebrew folder) · `SHA256SUMS`</sub>

</div>

---

<div align="center">

## 💎 What is XENO STORE?

</div>

> **XENO STORE** is a **native PS5 game store and download manager** that runs
> *on the console itself*. Send one payload, open the store in the PS5 browser
> (or on your phone), pick a game, pick a drive, press ✕. The console downloads it
> with up to **16 parallel connections**, verifies it, **installs the package** or
> drops the image where **ShadowMountPlus** finds it, and keeps going while the
> browser is closed. No PC in the loop. 🔥

<div align="center">

<table>
<tr>
<td align="center">🛒<br/><b>1,965</b><br/><sub>games in the catalog</sub></td>
<td align="center">🌍<br/><b>14</b><br/><sub>languages</sub></td>
<td align="center">⚡<br/><b>16</b><br/><sub>connections per download</sub></td>
<td align="center">💾<br/><b>Saves</b><br/><sub>backup · restore · share</sub></td>
<td align="center">📱<br/><b>Phone</b><br/><sub>pair by QR or xeno.local</sub></td>
</tr>
</table>

</div>

---

## ✨ Features

| | Feature | What you get |
|:--:|:--|:--|
| 🛒 | **One big catalog** | **1,965 PS5 + PS4 games / 2,592 releases** merged from a verified catalog feed, Archive.org, dlpsgame, SuperPSX, pkg.games and more, with cover art, screenshots, Metacritic, genres and a **6-hour auto sync** (no app update needed). |
| ⚡ | **Fast downloads** | Segmented transfer with **work-stealing chunks**, per-host adaptive windows, resume with sidecars, mirror failover, **Turbo start** (races mirrors and keeps the fastest), bandwidth limit, schedule window, lifetime stats. |
| 📦 | **PKG auto-install** | Packages install the moment they finish (native AppInstUtil, with DPI / etaHEN DPI fallback). **Install sets**: game → backport → update → DLC, installed in order, one tap. |
| 🧩 | **Images just work** | exFAT / FFPFSC / FFPKG dumps land in `homebrew/` and XENO STORE pokes **ShadowMountPlus** to scan them. ZIP archives are **extracted on the console**. |
| 🔁 | **Backports** | Releases built for lower firmware are tagged with their target firmware; the store offers the right backport for *your* console and lists them in Updates center. |
| 🚀 | **Debrid support** | Connect **Real-Debrid, AllDebrid, TorBox or Premiumize** with your own key and the store unrestricts supported hosts into direct, range-capable links. Keys never leave the console except to the chosen service. |
| 💾 | **Save Data Store** | Community saves for PS5 titles with a **compatibility check** (installed? same region? same version?), one-tap install with automatic pre-backup, and **one-click share** of your own save. |
| 🗄️ | **Saves hub** | Back up any profile's saves to ZIP, restore them, transfer between profiles on the same console, "Back up all saves now". |
| 🏠 | **Homebrew tab** | Apps, emulators and tools listed from pkg-zone with one-tap install and "Update available" badges. |
| 👤 | **Accounts** | Offline account activation (registry slot method), plus NP fake sign-in / sign-out companions for offline trophies. |
| 🧰 | **Companions** | ShadowMountPlus, package installer daemon, kstuff, app dumper, Garlic — downloaded with a hash check and sent to the local ELF loader. **Readiness checklist** tells you what is still missing. |
| 🕹️ | **Couch-grade UI** | DualSense spatial navigation, PS glyph legend, 7 themes, 14 languages, large-text and high-contrast modes, cover-wall screensaver, command palette on desktop, phone layout with bottom tabs. |
| 🌐 | **xeno.local** | Built-in mDNS answers `http://xeno.local:9095` on your Wi-Fi, so phones and laptops find the console without typing an IP. |
| 🧹 | **Clean uninstall** | Settings → Uninstall removes exactly what the store created (and keeps your games). Ghost-icon cleaner for leftover `/data/homebrew` folders. |
| 🔒 | **No accounts, no telemetry** | Local network only. The only outbound traffic is the catalog / feed sync you can turn off, plus the downloads you start. |

---

## 🏁 Why XENO STORE

<div align="center">

<table>
<tr>
<td align="center" width="25%">⚡<br/><b>Faster</b><br/><sub>16 parallel connections, mirror racing, Turbo start, debrid</sub></td>
<td align="center" width="25%">📦<br/><b>One press</b><br/><sub>download → verify → install, game + backport + update + DLC in order</sub></td>
<td align="center" width="25%">🛡️<br/><b>Never corrupt</b><br/><sub>fsync checkpoints, resume after rest mode, every byte accounted for</sub></td>
<td align="center" width="25%">💾<br/><b>Saves built in</b><br/><sub>backup · restore · transfer · community store</sub></td>
</tr>
<tr>
<td align="center">🕹️<br/><b>Made for the couch</b><br/><sub>controller-first, glass design, 7 themes</sub></td>
<td align="center">📱<br/><b>Phone + PC</b><br/><sub>pair with a 6-digit code or <code>xeno.local</code></sub></td>
<td align="center">🌍<br/><b>14 languages</b><br/><sub>Arabic right-to-left included</sub></td>
<td align="center">🔄<br/><b>Always fresh</b><br/><sub>catalog syncs every 6 h, no reinstall</sub></td>
</tr>
</table>

</div>

---

## 📸 Screenshots

| Discover | Game page | Download options |
|:--:|:--:|:--:|
| ![Discover](docs/screenshots/discover.png) | ![Game](docs/screenshots/game.png) | ![Download options](docs/screenshots/download-options.png) |

| Downloads | Library | Browse facets |
|:--:|:--:|:--:|
| ![Downloads](docs/screenshots/downloads.png) | ![Library](docs/screenshots/library.png) | ![Browse facets](docs/screenshots/browse-facets.png) |

| Save Data Store | Homebrew | Settings |
|:--:|:--:|:--:|
| ![Save Data Store](docs/screenshots/saves-store.png) | ![Homebrew](docs/screenshots/homebrew.png) | ![Settings](docs/screenshots/settings.png) |

| Pair a device | Phone layout | |
|:--:|:--:|:--:|
| ![Pair a device](docs/screenshots/pair.png) | ![Phone](docs/screenshots/phone-discover.png) | |

<sub>
1920×1080 captures from the real UI (`docs/screenshots/`); the phone shot is 390×844. These are genuine renders of the shipped build —
the cover art and screenshots you see are <b>neutral placeholders</b> (marked <code>MOCK ART · XENO STORE</code>) because the build machine has no CDN access; on a
real console every tile pulls the actual box art and media from the catalog's art sources.
</sub>

---

## 🚀 Install

You need a jailbroken PS5 with an ELF loader listening on **port 9021** (etaHEN, the ps5-payload-dev `elfldr`, XENO TOOL's Payloads tab all do). Grab the files from the **[latest release](../../releases/latest)** and pick **one** route — they all run the same store, so settings, queue and saves carry over:

### 1 · Games-row app — `XENO-STORE.pkg` ⭐ recommended

A real application tile in the PS5 **Games home row** (title id `PPSA99500`), with the XENO icon.

```text
1.  ⬇️   Download  XENO-STORE.pkg
2.  💿   Install it with your package installer (USB, the DPI daemon or etaHEN DPI)
3.  🎮   "XENO STORE" appears in the Games row — press ✕
         The app starts the store if it is not running yet and opens it full screen
```

<sub>A debug package built and verified by <code>make ffpkg</code>; it needs FPKG-capable firmware like any fake package. On firmware that cannot install fake packages, use route 2 or 3.</sub>

### 2 · Payload — `xeno-store-browser.elf` (works everywhere)

```text
1.  ⬇️   Download  xeno-store-browser.elf
2.  📡   XENO TOOL → Payloads → Send file  (or any loader on port 9021)
3.  🌐   The PS5 browser opens the store by itself
         (sent xeno-store.elf instead? open http://<ps5-ip>:9095 or http://xeno.local:9095)
```

### 3 · Homebrew folder — `XENO-STORE.zip` (etaHEN / itemzflow launcher)

```text
1.  ⬇️   Download  XENO-STORE.zip
2.  📂   Extract it to  /data/homebrew/XENO-STORE/  (eboot.bin, sce_sys/, homebrew.json)
3.  🏠   Start XENO STORE from your homebrew launcher
```

### 🔁 Auto-start — Payload Manager

Add this URL as a **source** in Payload Manager and enable auto-start for **XENO STORE**, so the download service is up after every boot:

```text
https://raw.githubusercontent.com/XENOKING123/XENO-STORE/main/updates/payloads.json
```

<sub>Settings → Setup shows the same URL as a QR code. Every release writes the sha256 of each file into that list, so Payload Manager verifies what it downloads.</sub>

> 💡 **Updates** are built in: Settings → Updates → *Check* → *Install update* downloads the new version, checks its sha256 and stages it for the next start.

---

## ✅ First run & readiness checklist

The welcome sheet asks three things: which **sources** you want to see, which **drive** downloads go to, and a notice you acknowledge once. After that **Settings → Setup** runs a checklist (also available any time):

| Check | Why it matters | Fix offered |
|:--|:--|:--|
| ShadowMountPlus API (`127.0.0.1:10101`) | Mounts exFAT / FFPFSC dumps so they show on the home screen | **Launch ShadowMountPlus now** when a copy is on the console, or install it from Companions |
| Package installer (`:9040`) | Fallback for PKG install when the native call is refused | Install the companion daemon |
| etaHEN DPI (`:12800`) | Optional second fallback | Load etaHEN before the store |
| Internet | Catalog sync, cover art, downloads | Shows the exact host that failed |
| Default drive writable + free space | Nothing starts without room (256 MB reserve + the file) | Pick another drive / Disk analyzer |
| Firmware | PS5 fake packages (FPKG) need a supported firmware — **11.60 or lower as of October 2026** | Explains what still works on newer firmware (images via ShadowMountPlus, homebrew PKGs, saves) |
| Payload Manager | Auto-start on boot | QR + URL of `payloads.json` |

---

## 🎮 Controls

| Button | Action |
|:--:|:--|
| **D-pad / left stick** | Move focus (rails scroll, grids are virtualized) |
| **✕** | Select / Download |
| **○** | Back (also closes sheets and the gallery) |
| **□** | Favorite the focused game |
| **△** | Search |
| **L1 / R1** | Previous / next tab (Discover · Browse · Homebrew · Library · Downloads) |
| **Options** | Settings |
| **L2 held while the store opens** | **Safe mode** — no animations, sounds or online enrichment (also a Settings toggle) |
| **Settings → General → Swap ✕/○** | Japanese layout |

Keyboard (phone / desktop): arrows, Enter, Esc, `F` favorite, `Q`/`E` tabs, `/` search, `?` shortcuts, `Ctrl/Cmd+K` command palette. Mouse and touch work everywhere.

---

## 📂 Formats

| Format | What happens after download | Needs |
|:--|:--|:--|
| **PKG / FPKG** | Saved to `<drive>/xeno-store/pkg/` and **installed automatically** (native → DPI `:9040` → etaHEN DPI `:12800`). Install sets install game → backport → update → DLC in order. | FPKG: firmware ≤ 11.60 + kstuff/etaHEN as of Oct 2026 |
| **exFAT · FFPFSC · FFPKG · FFPFS** | Saved to `<drive>/homebrew/` and ShadowMountPlus is asked to scan. | ShadowMountPlus |
| **ZIP** | Saved to `<drive>/xeno-store/files/` and **extracted on the console** (folder dumps → `homebrew/<name>/`, single images straight to the image file); the archive is deleted after a successful extract (both toggles in Settings). | — |
| **RAR · 7z** | Downloaded only — there is no RAR/7z engine in the payload. The UI says so and points to XENO TOOL on the desktop to extract and send. | XENO TOOL |
| **PAGE** | A web page that lists mirrors: **Get links** resolves direct files server-side where allowed, or **Open on PS5 browser**; human checks (Vikingfile-style) finish in the browser and you paste the file link with **Enter file link** — from the TV or your phone. | — |

Full details, paths and sidecar formats: [`docs/FORMATS.md`](docs/FORMATS.md).

---

## 💾 Save Data Store

<img src="docs/screenshots/saves-store.png" width="100%" alt="Save Data Store"/>

A community library of PS5 saves — maxed starters, 100% platinums, trophy checkpoints, chapter picks — **re-signed for your account on this console** so they load like your own. Every save is checked before it touches your card:

* **Compatibility first.** Each card shows whether the game is **installed**, whether its **region** matches and whether the **version** lines up; "Compatible with my games" filters the grid to saves you can actually use.
* **Honest re-sign tiers.** The store reports the engine it will use, never fakes it: **Native** (the Sony save API, when kstuff-style patches are loaded), **Garlic** (Garlic Save Manager on `:8082`), or **Raw** (a same-console copy). The current tier is shown on the header pill.
* **Safe installs.** Download is sha256-checked, the install **backs up your current save first**, and a save for the wrong title id is refused outright.
* **Share yours in one flow.** *Upload my save* exports the chosen save, strips the account id, uploads it to a file host and opens a pre-filled submission — your save joins the library for everyone.

The companion **Saves hub** (Library → Saves hub) backs up any profile to ZIP, restores it, transfers saves between profiles on the same console, and has a "Back up all saves now". Full walkthrough: [`docs/SAVES.md`](docs/SAVES.md).

---

## 🚀 Debrid

Connect **Real-Debrid, AllDebrid, TorBox or Premiumize** with your own API key (Settings → Debrid). When a release points at a host your service supports, XENO STORE **unrestricts it into a direct, range-capable link** and the segmented engine downloads it at full speed with resume — no browser step, no waiting-room page.

* The key is validated against the service's own user endpoint before it is stored, and it **never leaves the console** except to the service you chose.
* Supported hosts are resolved transparently; unsupported links fall back to the normal flow (direct download, or **Enter file link** for pages that need a human check).
* Add or forget a key any time; forgetting it clears it from the console immediately.

---

## 📱 Phone pairing

<img src="docs/screenshots/pair.png" width="100%" alt="Pair a device screen"/>

Settings → **Pair a device** shows a QR code and the two addresses `http://<ps5-ip>:9095` and `http://xeno.local:9095`.
Scan it with any phone on the same Wi-Fi: same store, phone layout, bottom tabs. Everything works from the phone —
browse, queue, pause, **Enter file link** (paste the link you unlocked in a mobile browser), the Save Data Store upload wizard,
and "Add to home screen" via the PWA manifest.

**Proving the phone is in the room.** When a device changes anything on the console, the store can ask it to pair with a **6-digit code**:

```text
1.  🔢   The console shows a 6-digit code as a PS5 toast (Pair a device → "Show code" re-shows it, 30 s cooldown)
2.  📲   On the phone, open the store, enter the code
3.  🔑   The phone gets a bearer token (up to 16 remembered), and from then on it acts without re-entering
```

The literal code is only ever returned to the console's own browser (loopback); a phone must read it off the TV, so a device that cannot see the screen cannot pair. An optional 4-digit **PIN** in Settings → Pairing adds a second gate for every mutating request. The console browser itself is trusted automatically via `/api/session`.

---

## ❓ FAQ

<details>
<summary><b>Does it need XENO TOOL on a PC?</b></summary>
<br/>
No. XENO TOOL is one convenient way to <i>send</i> the ELF; after that the store is self-contained. XENO TOOL is still handy for RAR/7z extraction and for sending files from a PC.
</details>

<details>
<summary><b>Where do the games come from? Do you host them?</b></summary>
<br/>
No. The catalog lists releases published by the sources in <code>catalog/sources.json</code> (Archive.org items, a verified catalog feed, game sites). XENO STORE downloads straight from them; it never hosts, re-uploads or proxies game files.
</details>

<details>
<summary><b>Why does a release say "Open on PS5 browser"?</b></summary>
<br/>
Some hosts require a human check or a page with mirrors. XENO STORE does not bypass CAPTCHAs. Finish the page in the browser (TV or phone), copy the direct link and use <b>Enter file link</b>.
</details>

<details>
<summary><b>Will my console sleep during a 100 GB download?</b></summary>
<br/>
Not while "Keep console awake" is on (default): the store ticks the power service every 30 s while anything downloads. If you put it to rest anyway, downloads resume from their checkpoints when the network comes back.
</details>

<details>
<summary><b>Is the Save Data Store safe to use?</b></summary>
<br/>
It only lists saves marked <code>published</code>, checks the sha256 of every download, refuses saves for a different title ID, warns when the game version differs, and backs up your current save before installing. Re-signing uses the native save API when kstuff-style patches are loaded, Garlic Save Manager when it answers on <code>:8082</code>, or a same-console raw copy — the tier in use is shown, never faked.
</details>

<details>
<summary><b>What does the host build do?</b></summary>
<br/>
<code>make host</code> builds the same C code for Linux/macOS with the PS5 calls stubbed (installs "succeed" after 3 s, saves mount to temp folders). It exists for tests and for working on the UI without a console; it is not a game downloader for PCs.
</details>

<details>
<summary><b>Is this legal?</b></summary>
<br/>
XENO STORE is a GPL-3.0 download manager and catalog browser. Only download content you have the right to use. See the disclaimer below.
</details>

---

## 🛠️ Troubleshooting

| Symptom | What to do |
|:--|:--|
| "XENO STORE already running on :9095" toast | A copy is still running — just open the address. Use Settings → Updates → *Stop XENO STORE* before sending a new ELF. |
| "Downloads unavailable on this firmware" | The network libraries did not load (`libSceHttp` / `libSceSsl`). Browsing still works; retry after a reboot and report your firmware in an issue. |
| Download fails with *provider_page* | The host answered with a web page instead of the file. Open the page and use **Enter file link**. |
| *source_changed* | The file on the server changed under a paused download. **Restart** from zero or choose another source. |
| *no_space* | Needs the file + 256 MB reserve on that drive. Library → Disk analyzer → *Clean up partials*, or pick another drive. |
| PKG shows *Install failed* | Check Settings → Setup: the native installer was refused (firmware) and no DPI daemon answered. Install the companion daemon or load etaHEN, then press **Install** on the job. |
| Image downloaded but no icon on the home screen | ShadowMountPlus is not running — the checklist offers **Launch ShadowMountPlus now**. |
| Phone cannot open `xeno.local` | Some Android browsers ignore mDNS; use the IP shown next to it. Guest Wi-Fi networks often block device-to-device traffic. |
| Store looks wrong / stuck after an update | Settings → Diagnostics → *Export settings*, then reload the page once (the UI is cached with an ETag). |

More in [`docs/GUIDE.md`](docs/GUIDE.md#troubleshooting).

---

## 🧰 Build from source

<details>
<summary><b>Click to expand</b></summary>

<br/>

```bash
make web        # node web/build.mjs            → web/dist/index.html (single file, ES2017, no deps)
make catalog    # tools/catalog/build_catalog.py --offline + --validate → catalog/xeno-catalog.json
make assets     # tools/gen_assets.py           → src/assets.h (UI + catalog + logo + icon, gzip variants)
make host       # host binary build/host/xeno-store (-DXENO_HOST=1, OpenSSL when pkg-config finds it)
make test       # make host + tests/host/run.sh  (100+ tests across 5 suites)
make ps5        # xeno-store.elf + xeno-store-browser.elf (needs PS5_PAYLOAD_SDK)
make ps5-check  # compile-only check of every PS5 translation unit
make pkg        # tools/make_homebrew.py        → dist/XENO-STORE/ + dist/XENO-STORE.zip
make ffpkg      # tools/make_ffpkg.py           → dist/XENO-STORE.pkg (needs fpkg_build)
make launcher   # app/build_launcher.sh         → app/dist/xeno-store-launcher.elf
make all        # web catalog assets ps5 pkg
make clean      # build outputs (distclean also removes web/dist)
```

**Requirements:** gcc or clang, make, Python 3.10+ (stdlib only), Node 22 (no npm packages), OpenSSL dev headers for HTTPS in the host build,
and the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk) v0.40 with LLVM 18 for `make ps5`:

```bash
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make ps5 pkg
```

Compiler flags are `-std=gnu11 -Wall -Wextra -Werror -O2 -fstack-protector-strong`. The PS5 build links only libraries proven to load on every firmware
(`SceSystemService`, `SceUserService`, `SceIpmi`, `SceAppInstUtil`, `SceNotification`, `kernel_sys`); everything else (`libSceHttp`, `libSceSsl`, `libSceNet`, `libSceSaveData`, registry, launcher) is resolved at runtime with `dlsym` and degrades gracefully.

UI development without a console: `node web/dev-server.mjs --port 9095` serves the bundle with a mock of the whole API, and `web/verify-ui.mjs` runs the Playwright walk (screenshots + keyboard-only flow + "no console errors").

</details>

---

## 🔄 Catalog sync & audit

* **`catalog-sync.yml`** runs every 6 hours (`17 */6 * * *`) and on demand: `build_catalog.py --online` scrapes every source with a per-source time budget (a broken site falls back to its last good data, never breaks the build), then `audit.py --budget-seconds 1500` checks release links (Archive.org metadata with sizes and hashes, `HEAD` + 1-byte `Range` for direct hosts, "does the page still mention the game" for page sources), then `--validate` guards the result. Changes are committed as `catalog: sync (revision N) [catalog-sync]`.
* **Audit verdicts** are `ok`, `flaky`, `dead` and `page`; `dead` links are dropped at the next build, `release.health` is set and `game.verified` means "at least one direct release audited ok". Results live in `catalog/snapshots/audit/results.json` and are re-checked after 7 days.
* The console checks `catalog/revision.json` every 6 hours (Settings → Catalog → Auto-refresh) and downloads the new catalog only when the revision grew; `updates/featured.json`, `messages.json`, `companions.json` and `saves-backend.json` ride along, so Discover rows, announcements and companions change **without an app update**.
* **`build.yml`** builds the UI, builds + validates the catalog, runs the host tests, cross-compiles the ELFs, and on `main` publishes the `v<VERSION>` release with `SHA256SUMS`, then writes the hashes into `updates/payloads.json`.

---

## 🏆 Credits

<div align="center">

**XENO STORE stands on the shoulders of the scene.** Thank you to everyone below — please support their projects too.

| Project / person | What we learned or use |
|:--|:--|
| **[ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk)** — John Törnblom | The toolchain every payload here is built with, `elfldr`, `websrv`, `offact` |
| **[LightningMods](https://github.com/LightningMods)** | etaHEN, ShadowMount and the DPI protocol that make one-tap installs possible |
| **[maj0r / CheatRunner](https://discord.gg/E4g6fEqp46)** | The on-console cheat engine behind the "⚡ cheats in XENO TOOL" chips |
| **Cheat creators** — LM, Buzzer, Super Death, GoldHEN Team, Yharnam, PS4Trainer and everyone in the etaHEN & GoldHEN communities | The cheat database that powers those counts |
| **[hejran7](https://github.com/hejran7)** | Major coding work across the XENO projects |
| **[Archive.org](https://archive.org)** | Hosting with Range support, metadata, sizes and hashes |
| **[dlpsgame](https://dlpsgame.com)** and the other game sites in `catalog/sources.json` | Listings, mirrors and versions |
| **[prosperopatches](https://prosperopatches.com) / [orbispatches](https://orbispatches.com)** | Cover and hero artwork CDN |
| **[Garlic Save Manager](https://garlicsaves.com)** (ELF Arsenal) | Save re-signing worker and the decrypted-save ZIP layout the Save Data Store stays compatible with |
| **[offact](https://github.com/ps5-payload-dev)** | The offline account activation method vendored in this repo |
| **[earthonion / np-fake-signin](https://github.com/earthonion/np-fake-signin)** | NP fake sign-in / sign-out companions |
| **[miniz](https://github.com/richgel999/miniz)** | The single-file ZIP engine used for extraction and save backups |
| **[ps5upload](https://github.com/phantomptr/ps5upload)** (phantomptr, GPL-3.0) | The `fpkg_build` engine that builds and verifies the Games-row `.pkg` |
| **[qrcode-generator](https://github.com/kazuhikoarase/qrcode-generator)** — Kazuhiko Arase (MIT) | Pairing and share QR codes |

</div>

---

## ⚠️ Disclaimer

XENO STORE is a download manager and catalog browser. It does **not** host, distribute or sell games, and it does not bypass DRM, CAPTCHAs or access controls.
Content listed by third-party sources is their responsibility; you are responsible for complying with the laws of your country and for owning the games you download.
Modifying a console can void its warranty and may violate the platform holder's terms. The authors accept no liability for data loss, bans or hardware damage. **Back up your saves before using Accounts or the Save Data Store.**

<div align="center">

## ⚖️ License

**GNU GPL-3.0-or-later** — see [`LICENSE`](LICENSE). Third-party notices are in `src/third_party/README.md` and the in-app **About** screen.

<br/>

<img src="https://capsule-render.vercel.app/api?type=waving&color=0:ffce4a,50:1a2980,100:060b1c&height=150&section=footer&text=Made%20by%20XENOKING&fontSize=30&fontColor=ffffff&fontAlignY=70" width="100%" alt="footer"/>

<sub>⭐ If XENO STORE replaced a PC in your workflow, drop a star — it helps more people find it. ⭐</sub>

</div>
