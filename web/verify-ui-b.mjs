#!/usr/bin/env node
/*
 * XENO STORE — web/verify-ui-b.mjs: Playwright (Chromium) check of the Phase 2 web features (part B) against the
 * mock API (dev-server.mjs + dev-server-ext-*.mjs). Same conventions as verify-ui.mjs.
 *
 *   1. builds web/dist/index.html (unless --no-build) and starts the mock on a free port (first-run wizard skipped)
 *   2. at 1920×1080: Downloads (install-set groups, lifetime stats, rest-mode pill, turbo chip, debrid label, copy and
 *      backup jobs, ZIP extraction, scheduled row + Start now, paused set + Retry), Library (disk analyzer + clean-up,
 *      Play now, uninstall with the token flow, copy-to-drive sheet, installed homebrew), Updates center (one-tap
 *      download+install), Saves hub (keyboard: Back up → progress → toast; restore; profile transfer), the Saves top
 *      tab (keyboard from its first focus: Saves hub → Back, Save Store options → Settings → Saves → Back), Settings
 *      (Setup via keyboard + run check + launch companion + start Garlic + unavailable companion + ghost cleaner,
 *      Debrid connect, Accounts activate, Saves, Pair/mDNS, Uninstall sheet open/cancel + the final screen), command
 *      palette, shortcuts overlay, large text + high contrast, controller remap legend
 *   3. at 390×844: Downloads, Settings, the Saves tab from the phone bar and its Saves hub entry
 *   4. fails on any console error / uncaught exception, any failed or ≥ 400 request, missing i18n keys
 *
 * Usage: PLAYWRIGHT_BROWSERS_PATH=/opt/pw-browsers node web/verify-ui-b.mjs [--port 9095] [--no-build] [--out DIR] [--headed] [--verbose]
 */
import { spawn, spawnSync } from 'node:child_process';
import { mkdirSync, existsSync, statSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

const here = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const argValue = (name, dflt) => { const i = args.indexOf(name); return i >= 0 && args[i + 1] !== undefined ? args[i + 1] : dflt; };
const PORT = Number(argValue('--port', 9500 + Math.floor(Math.random() * 400)));
const OUT = resolve(argValue('--out', join(here, 'screenshots')));
const BASE = `http://127.0.0.1:${PORT}`;
const require = createRequire(import.meta.url);
const pwPath = ['/opt/node22/lib/node_modules/playwright', 'playwright'].find((p) => { try { require.resolve(p); return true; } catch (e) { return false; } });
if (!pwPath) { console.error('verify-b: playwright not found (expected /opt/node22/lib/node_modules/playwright)'); process.exit(2); }
const { chromium } = require(pwPath);
if (!process.env.PLAYWRIGHT_BROWSERS_PATH && existsSync('/opt/pw-browsers')) process.env.PLAYWRIGHT_BROWSERS_PATH = '/opt/pw-browsers';
mkdirSync(OUT, { recursive: true });

const problems = [];
const note = (m) => console.log('verify-b: ' + m);
const fail = (m) => { problems.push(m); console.log('verify-b: FAIL ' + m); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

if (!args.includes('--no-build')) {
  const b = spawnSync(process.execPath, [join(here, 'build.mjs')], { stdio: 'inherit' });
  if (b.status !== 0) { console.error('verify-b: build failed'); process.exit(1); }
}
const distSize = existsSync(join(here, 'dist', 'index.html')) ? statSync(join(here, 'dist', 'index.html')).size : 0;
const server = spawn(process.execPath, [join(here, 'dev-server.mjs'), '--port', String(PORT), '--art', 'local', '--speed', '6', '--acknowledged'], { stdio: ['ignore', 'pipe', 'pipe'] });
const serverLog = [];
server.stdout.on('data', (d) => serverLog.push(String(d)));
server.stderr.on('data', (d) => serverLog.push(String(d)));
async function waitServer() { for (let i = 0; i < 60; i++) { try { const r = await fetch(BASE + '/api/status'); if (r.ok) return; } catch (e) { /* retry */ } await sleep(250); } throw new Error('dev-server did not start:\n' + serverLog.join('')); }

/* ---------- page helpers ---------- */
function watch(page, label, expected) {
  /* `expected`: a regex for responses a test provokes on purpose (e.g. a wrong pairing code) */
  page.on('console', (msg) => { if (msg.type() === 'error' && !(expected && /status of 403/.test(msg.text()))) fail(`[${label}] console.error: ${msg.text()}`); });
  page.on('pageerror', (err) => fail(`[${label}] uncaught: ${err.message}`));
  page.on('requestfailed', (req) => { if (req.url().indexOf(BASE) === 0 && !(expected && /ERR_ABORTED/.test((req.failure() && req.failure().errorText) || ''))) fail(`[${label}] request failed: ${req.method()} ${req.url()} (${req.failure() && req.failure().errorText})`); });
  page.on('response', (res) => { if (res.url().indexOf(BASE) === 0 && res.status() >= 400 && !(expected && expected.test(res.status() + ' ' + res.url()))) fail(`[${label}] HTTP ${res.status()} for ${res.request().method()} ${res.url()}`); });
}
const activeInfo = (page) => page.evaluate(() => { const a = document.activeElement; return a ? (a.tagName + ' ' + (a.className || '') + ' #' + (a.getAttribute('data-nav-id') || '')) : 'none'; });
const activeId = (page) => page.evaluate(() => (document.activeElement && document.activeElement.getAttribute('data-nav-id')) || '');
async function waitCovers(page, min, ms) { try { await page.waitForFunction((n) => document.querySelectorAll('.cover.loaded').length >= n, min, { timeout: ms || 8000 }); } catch (e) { note('covers still loading (continuing)'); } }
async function shot(page, name) { await page.screenshot({ path: join(OUT, name), fullPage: false }); note('screenshot ' + name); }
const VERBOSE = args.includes('--verbose');
async function expect(cond, msg) { let ok = false; try { ok = await cond(); } catch (e) { ok = false; if (VERBOSE) note('expect threw: ' + e.message); } if (!ok) fail(msg); }
async function noLayer(page) { await page.waitForSelector('.layer', { state: 'detached', timeout: 6000 }); }
async function clickMenuItem(page, text) { await page.waitForSelector('.menu .mi'); const ok = await page.evaluate((tx) => { const it = Array.from(document.querySelectorAll('.menu .mi')).find((m) => m.textContent.indexOf(tx) >= 0); if (it) it.click(); return !!it; }, text); if (!ok) fail('menu item not found: ' + text); }
async function waitToast(page, text, ms) { try { await page.waitForFunction((tx) => Array.from(document.querySelectorAll('#toasts .toast')).some((t) => t.textContent.indexOf(tx) >= 0), text, { timeout: ms || 8000 }); return true; } catch (e) { fail('toast not seen: "' + text + '" (toasts: ' + await page.evaluate(() => Array.from(document.querySelectorAll('#toasts .toast')).map((t) => t.textContent.trim().slice(0, 80)).join(' | ')) + ')'); return false; } }
async function confirmDialog(page) { await page.waitForSelector('.dialog [data-nav-id="dlg-1"]'); await page.click('.dialog [data-nav-id="dlg-1"]'); }
async function keyTo(page, navId, max) {
  /* keyboard-only: walk with arrows until the target has focus (falls back to a programmatic focus and reports it) */
  for (let i = 0; i < (max || 40); i++) {
    const cur = await page.evaluate(() => document.activeElement && document.activeElement.getAttribute('data-nav-id'));
    if (cur === navId) return true;
    await page.keyboard.press('ArrowDown'); await sleep(60);
  }
  note('keyboard walk did not reach ' + navId + ' (focus: ' + await activeInfo(page) + '), focusing it directly');
  await page.focus('[data-nav-id="' + navId + '"]');
  return false;
}
const api = (path, init) => fetch(BASE + path, init).then((r) => r.json());
const put = (path, body) => api(path, { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });

(async () => {
  await waitServer();
  note('dev-server up on ' + BASE + ` (dist ${(distSize / 1024).toFixed(1)} KiB)`);
  const browser = await chromium.launch({ headless: !args.includes('--headed') });
  try {
    /* ================= Link to PS5: the first page on a PC browser ================= */
    {
      const lctx = await browser.newContext({ viewport: { width: 1440, height: 900 }, deviceScaleFactor: 1, locale: 'en-US' });
      const lp = await lctx.newPage();
      watch(lp, 'link', /^403 .*\/api\/pair$/);
      await api('/api/__mock/remote', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ on: true }) });
      await lp.goto(BASE + '/', { waitUntil: 'load' });
      await lp.waitForSelector('.link-page [data-nav-id="link-show"]', { timeout: 10000 }).catch(() => fail('Link to PS5 page did not show for a LAN browser'));
      await shot(lp, 'pc-link.png');
      await lp.click('[data-nav-id="link-show"]');
      await lp.waitForFunction(() => /notification/.test(document.querySelector('.link-status').textContent), null, { timeout: 5000 }).catch(() => fail('"Show code" did not confirm the PS5 notification'));
      await lp.fill('.link-code', '111111');
      await lp.waitForFunction(() => document.querySelector('.link-status').classList.contains('bad'), null, { timeout: 5000 }).catch(() => fail('a wrong code was not refused'));
      await lp.fill('.link-code', '424242');
      await lp.waitForSelector('.link-page', { state: 'detached', timeout: 8000 }).catch(() => fail('the right code did not open the store'));
      await lp.waitForSelector('#scr-discover.active .card', { timeout: 20000 });
      await expect(async () => lp.evaluate(() => localStorage.getItem('xs.token') !== null && XS.state.data.token.length === 64), 'device token not kept after linking');
      await lp.reload({ waitUntil: 'load' });
      await lp.waitForSelector('#scr-discover.active .card', { timeout: 20000 });
      await expect(async () => !(await lp.$('.link-page')), 'a linked browser should go straight to the store');
      await api('/api/__mock/remote', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ on: false }) });
      await lctx.close();
      note('Link to PS5 page OK');
    }

    /* ================= TV / desktop 1920×1080 ================= */
    const ctx = await browser.newContext({ viewport: { width: 1920, height: 1080 }, deviceScaleFactor: 1, locale: 'en-US' });
    const page = await ctx.newPage();
    watch(page, 'tv');
    await page.goto(BASE + '/', { waitUntil: 'load' });
    await page.waitForSelector('#scr-discover.active .card', { timeout: 20000 });
    await page.waitForFunction(() => !!(window.XS && XS.cb && XS.cb.palette), null, { timeout: 5000 });
    await expect(async () => page.evaluate(() => typeof XS.actions.applySettings === 'function' && XS.actions.applySettings._cbWrapped === true), 'applySettings should be wrapped by 32-components-b (accessibility attributes)');
    await expect(async () => page.evaluate(() => { const ids = Array.from(document.querySelectorAll('#tabs .tab')).map((a) => a.getAttribute('data-nav-id')); return ids.indexOf('tab:saves') === ids.indexOf('tab:browse') + 1 && document.querySelector('[data-nav-id="tab:saves"]').textContent.trim() === 'Saves'; }), 'the tab bar should have a Saves tab right after Browse');

    /* ---------- Downloads: groups, stats, rest pill, turbo, via, copy, backup, extracting ---------- */
    await page.click('[data-nav-id="tab:downloads"]');
    await page.waitForSelector('#scr-downloads.active .jobgroup', { timeout: 10000 });
    await page.waitForFunction(() => { const s = document.querySelector('#scr-downloads .dl-stats'); return s && !s.hidden && /Lifetime/.test(s.textContent); }, null, { timeout: 8000 }).catch(() => fail('lifetime stats line (/api/stats) missing on Downloads'));
    await expect(async () => !!(await page.$('#scr-downloads .stat.pill.rest')), 'rest-mode pill missing');
    await expect(async () => page.evaluate(() => /awake|Rest mode/.test(document.querySelector('#scr-downloads .stat.pill.rest').textContent)), 'rest-mode pill text missing');
    await expect(async () => (await api('/api/queue')).summary.restMode.pill === await page.evaluate(() => document.querySelector('#scr-downloads .stat.pill.rest .v').textContent), 'rest-mode pill should show summary.restMode.pill verbatim (dl.c shape)');
    await page.waitForFunction(() => { const s = document.querySelector('#scr-downloads .dl-sched'); return s && !s.hidden && /Download window 22:00 – 07:00/.test(s.textContent) && /open now|closed/.test(s.textContent); }, null, { timeout: 6000 }).catch(() => fail('download window line (summary.schedule) missing on Downloads'));
    await expect(async () => !!(await page.$('#scr-downloads .job .chip.turbo')), 'turbo result chip ("Fastest mirror") missing on the running row');
    await expect(async () => page.evaluate(() => /Fastest mirror/.test(document.querySelector('#scr-downloads .job .chip.turbo').textContent)), 'turbo chip should read "Fastest mirror: …"');
    await expect(async () => !!(await page.$('#scr-downloads .job .chip.via')), '"via Real-Debrid" label missing');
    await expect(async () => page.evaluate(() => Array.from(document.querySelectorAll('#scr-downloads .job.st-paused .stats')).some((x) => x.textContent.indexOf(XS.t('dl2.restartHeld')) >= 0)), 'a download a safe start left paused should say so (job.restart "held", dl2.restartHeld)');
    await expect(async () => !!(await page.$('#scr-downloads .job.type-copy')), 'copy job row missing');
    await expect(async () => !!(await page.$('#scr-downloads .job.st-extracting')), 'ZIP extracting row missing');
    await expect(async () => !!(await page.$('#scr-downloads .jobgroup .ghead .bar')), 'install-set group progress bar missing');
    await expect(async () => (await page.$$('#scr-downloads .jobgroup .members .job.member')).length >= 3, 'install-set members should be listed under the group');
    await waitCovers(page, 6);
    await page.evaluate(() => { const g = document.querySelector('#scr-downloads .jobgroup .ghead'); if (g) g.focus(); });
    await sleep(500);
    await shot(page, 'tv-b-downloads.png');
    /* queued tab: scheduled row + Start now */
    await page.click('[data-nav-id="dltab-queued"]');
    await page.waitForSelector('#scr-downloads .job.st-queued', { timeout: 6000 });
    const startNow = await page.$('#scr-downloads [data-nav-id^="job-j_sched"][data-nav-id$="-startnow"]');
    await expect(async () => !!startNow, 'scheduled row should offer "Start now"');
    await expect(async () => page.evaluate(() => Array.from(document.querySelectorAll('#scr-downloads .job.st-queued')).some((r) => /Scheduled for 22:00/.test(r.textContent))), '"Scheduled for 22:00" missing on the scheduled row');
    await shot(page, 'tv-b-downloads-queued.png');
    if (startNow) {
      await startNow.click();
      await waitToast(page, 'starts now');
      await page.waitForFunction(() => fetch('/api/queue').then((r) => r.json()).then((q) => q.jobs.some((j) => j.startNow === true && j.scheduledFor === '22:00' && !/^j_sched/.test(j.id))), null, { timeout: 6000, polling: 300 }).catch(() => fail('Start now should turn the scheduled entry into a real queued job with startNow:true'));
      await expect(async () => serverLog.some((l) => /startNow j_sched[0-9a-f]+ via \/startNow/.test(l)), 'the UI should call POST /api/queue/{id}/startNow (the dl_action name)');
    }
    /* failed tab: paused install set + Retry */
    await page.click('[data-nav-id="dltab-failed"]');
    await page.waitForSelector('#scr-downloads .jobgroup.gs-paused', { timeout: 6000 });
    await expect(async () => page.evaluate(() => /Install set paused at: update/i.test(document.querySelector('#scr-downloads .jobgroup.gs-paused').textContent)), '"Install set paused at: update…" missing');
    await expect(async () => !!(await page.$('#scr-downloads .jobgroup.gs-paused [data-nav-id^="group-"][data-nav-id$="-retry"]')), 'paused set should offer Retry');
    await waitCovers(page, 3);
    await sleep(300);
    await shot(page, 'tv-b-downloads-set-paused.png');
    await page.click('#scr-downloads .jobgroup.gs-paused [data-nav-id^="group-"][data-nav-id$="-retry"]');
    await waitToast(page, 'Retrying');
    await page.waitForFunction(() => !document.querySelector('#scr-downloads .jobgroup.gs-paused'), null, { timeout: 8000 }).catch(() => fail('after Retry the paused set should leave the Failed tab'));
    /* finished tab: backup job, extracted ZIP (after the mock finishes extracting) */
    await page.click('[data-nav-id="dltab-finished"]');
    await page.waitForSelector('#scr-downloads .job.type-backup', { timeout: 6000 }).catch(() => fail('save backup job row missing in Finished'));
    await page.waitForSelector('#scr-downloads .job.st-extracted', { timeout: 25000 }).catch(() => fail('ZIP should reach the "extracted" state'));
    await expect(async () => page.evaluate(() => { const r = document.querySelector('#scr-downloads .job.st-extracted'); return r && /Extracted/.test(r.textContent); }), 'extracted row should show the "Extracted · n files" chip');
    /* an installed PKG whose package was removed (downloads.deletePkgAfterInstall): says so, and offers no Verify / Open location */
    const pkgGone = await page.evaluate(() => { const r = Array.from(document.querySelectorAll('#scr-downloads .job.st-installed')).find((x) => /Installed/.test(x.textContent) && /Package removed/.test(x.textContent)); if (!r) return false; r.querySelector('[data-nav-id$="-more"]').click(); return true; });
    if (!pkgGone) fail('an installed PKG whose package was removed should show "Installed" and "Package removed" (install.pkgDeleted)');
    else {
      await page.waitForSelector('.menu .mi');
      await expect(async () => page.evaluate(() => { const tx = Array.from(document.querySelectorAll('.menu .mi')).map((m) => m.textContent).join('|'); return !/Verify hash|Open location/.test(tx) && /Log/i.test(tx); }), 'the menu of a removed package should not offer Verify hash or Open location');
      await page.keyboard.press('Escape');
      await page.waitForSelector('.menu', { state: 'detached', timeout: 4000 }).catch(() => fail('the job menu should close on Escape'));
    }
    await sleep(300);
    await shot(page, 'tv-b-downloads-finished.png');
    /* turbo details sheet from the row menu */
    await page.click('[data-nav-id="dltab-active"]');
    await page.waitForSelector('#scr-downloads .job .chip.turbo');
    await page.evaluate(() => { const row = document.querySelector('#scr-downloads .job .chip.turbo').closest('.job'); row.querySelector('[data-nav-id$="-more"]').click(); });
    await clickMenuItem(page, 'Mirror race results');
    await page.waitForSelector('.sheet [data-nav-id="turbo-close"]');
    await expect(async () => (await page.$$('.sheet .lrow')).length === 3, 'turbo sheet should list the 3 raced mirrors');
    await page.keyboard.press('Escape'); await noLayer(page);

    /* ---------- Library: analyzer, play now, uninstall, copy to drive, homebrew ---------- */
    await page.click('[data-nav-id="tab:library"]');
    await page.waitForSelector('#scr-library.active .usage-card', { timeout: 10000 });
    await page.waitForSelector('#scr-library .inst-acts', { timeout: 8000 });
    await page.waitForSelector('#scr-library .files .frow', { timeout: 8000 });
    await page.waitForSelector('#scr-library .hbrow', { timeout: 8000 }).catch(() => fail('installed homebrew apps list missing'));
    await expect(async () => !!(await page.$('#scr-library .hbrow .chip.warn')), 'an installed homebrew app should show "Update available"');
    await expect(async () => (await page.$$('#scr-library .lib-tile')).length >= 3, 'Library quick entries missing');
    await expect(async () => !!(await page.$('#scr-library .ubar i.bad')), 'disk analyzer should show the partials segment');
    await waitCovers(page, 6);
    await sleep(300);
    await shot(page, 'tv-b-library.png');
    await page.evaluate(() => { const c = document.querySelector('#scr-library .usage-card'); if (c) c.scrollIntoView(); });
    await sleep(200);
    await shot(page, 'tv-b-disk-analyzer.png');
    const clean = await page.$('#scr-library [data-nav-id^="usage-clean-"]');
    if (!clean) fail('Clean up partials button missing'); else { await clean.click(); await confirmDialog(page); await waitToast(page, 'freed'); }
    /* play now */
    await page.click('#scr-library [data-nav-id^="play:"]');
    await waitToast(page, 'Launching');
    /* uninstall a game: menu → double confirm → token flow */
    const instCount = (await api('/api/library')).installed.length;
    await page.click('#scr-library [data-nav-id^="instmore:"]');
    await clickMenuItem(page, 'Uninstall game');
    await confirmDialog(page); await confirmDialog(page);
    await waitToast(page, 'uninstalled');
    await expect(async () => (await api('/api/library')).installed.length === instCount - 1, 'uninstall should remove the title from /api/library.installed');
    /* copy to another drive */
    await page.waitForFunction(() => !document.querySelector('#scr-library .files .busy-row') && Array.from(document.querySelectorAll('#scr-library .files .frow')).some((r) => !/Partial/.test(r.textContent)), null, { timeout: 10000 }).catch(() => fail('no complete file row to copy after the rescan'));
    await page.evaluate(() => { const row = Array.from(document.querySelectorAll('#scr-library .files .frow')).find((r) => !/Partial/.test(r.textContent)); if (row) row.click(); });
    await clickMenuItem(page, 'Copy to another drive');
    await page.waitForSelector('.sheet [data-nav-id="tr-start"]');
    await page.click('.sheet [data-nav-id="tr-root-0"]');
    await page.waitForFunction(() => { const l = document.querySelector('.sheet [data-space]'); return l && /Fits|Not enough/.test(l.textContent); }, null, { timeout: 6000 }).catch(() => fail('transfer sheet should show the space check result'));
    await shot(page, 'tv-b-transfer-sheet.png');
    const fits = await page.evaluate(() => /Fits/.test(document.querySelector('.sheet [data-space]').textContent));
    if (fits) {
      await page.click('.sheet [data-nav-id="tr-start"]');
      await waitToast(page, 'added to the queue');
      await expect(async () => (await api('/api/queue')).jobs.some((j) => j.type === 'copy' && j.state === 'queued'), 'copy job should be queued through /api/library/transfer');
    } else { note('first destination has no space for the first file: closing the sheet'); await page.keyboard.press('Escape'); await noLayer(page); }
    /* updates center */
    await page.click('[data-nav-id="lib-entry-updates"]');
    await page.waitForSelector('#scr-updates.active .uprow', { timeout: 10000 });
    await waitCovers(page, 3);
    await sleep(400);
    await shot(page, 'tv-b-updates.png');
    const upCount = (await api('/api/queue')).jobs.length;
    await page.click('#scr-updates [data-nav-id^="upgo:"]');
    await waitToast(page, 'added');
    await expect(async () => (await api('/api/queue')).jobs.length > upCount, 'one-tap update should queue a job via /api/queue/set');
    await page.keyboard.press('Escape');
    await page.waitForSelector('#scr-library.active', { timeout: 6000 });

    /* ---------- Saves hub, keyboard only: entry → Back up → progress → toast ---------- */
    await page.evaluate(() => document.querySelector('[data-nav-id="lib-entry-saveshub"]').focus());
    await page.keyboard.press('Enter');
    await page.waitForSelector('#scr-saveshub.active .srow', { timeout: 10000 });
    await page.waitForSelector('#scr-saveshub .sh-backups .brow', { timeout: 8000 });
    await waitCovers(page, 3);
    await sleep(300);
    await shot(page, 'tv-b-saves-hub.png');
    const backupsBefore = (await page.$$('#scr-saveshub .sh-backups .brow')).length;
    await page.evaluate(() => document.querySelector('#scr-saveshub [data-nav-id^="save-backup:"]').focus());
    await page.keyboard.press('Enter');
    await page.waitForSelector('#scr-saveshub .cxjob', { timeout: 6000 }).catch(() => fail('backup progress row (.cxjob) missing after Back up'));
    await sleep(500);
    await shot(page, 'tv-b-saves-hub-progress.png');
    await waitToast(page, 'backed up', 20000);
    await page.waitForFunction((n) => document.querySelectorAll('#scr-saveshub .sh-backups .brow').length > n, backupsBefore, { timeout: 8000 }).catch(() => fail('backups list should grow after a backup'));
    /* restore the newest backup */
    await page.click('#scr-saveshub .sh-backups .brow');
    await clickMenuItem(page, 'Restore this backup');
    await page.waitForSelector('.sheet [data-nav-id="rs-go"]');
    await shot(page, 'tv-b-saves-restore.png');
    await page.click('.sheet [data-nav-id="rs-go"]');
    await waitToast(page, 'restored');
    /* transfer to the other profile (Beta) */
    await page.click('#scr-saveshub [data-nav-id^="save-more:"]');
    await clickMenuItem(page, 'Transfer to profile');
    await page.waitForSelector('.sheet [data-nav-id="ts-go"]');
    await expect(async () => page.evaluate(() => /Beta/.test(document.querySelector('.sheet .sheet-head').textContent)), 'transfer sheet should carry the Beta label');
    await page.click('.sheet [data-nav-id="ts-go"]');
    await waitToast(page, 'transferred');
    await waitToast(page, 're-signed for');                                       // re-signed through Garlic Save Manager
    /* back up all */
    await page.click('[data-nav-id="sh-backup-all"]');
    await confirmDialog(page);
    await waitToast(page, 'started');
    await page.keyboard.press('Escape');
    await page.waitForSelector('#scr-library.active', { timeout: 6000 });

    /* ---------- Saves tab, keyboard only from its first focus (Upload my save): Down reaches the Save Store options,
       Left the Saves hub; both open above the tab and ○ comes back to it ---------- */
    await page.click('[data-nav-id="tab:saves"]');
    await page.waitForSelector('#scr-saves.active [data-nav-id="sv-entry-backups"]', { timeout: 10000 });
    await page.waitForSelector('#scr-saves.active .save-card', { timeout: 10000 });
    await waitCovers(page, 6);
    await sleep(300);
    await shot(page, 'tv-b-saves-tab.png');
    await expect(async () => page.evaluate(() => document.querySelector('#tabs .tab.active').getAttribute('data-nav-id') === 'tab:saves' && location.hash === '#saves'), 'the Saves tab should be highlighted with #saves in the address');
    await page.waitForFunction(() => document.activeElement && document.activeElement.getAttribute('data-nav-id') === 'sv-upload', null, { timeout: 3000 }).catch(async () => fail('the Saves tab should start on Upload my save (got ' + await activeInfo(page) + ')'));
    await page.keyboard.press('ArrowDown');
    await expect(async () => (await activeId(page)) === 'sv-entry-options', 'Down from Upload my save should reach the Save Store options (got ' + await activeInfo(page) + ')');
    await page.keyboard.press('ArrowLeft');
    await expect(async () => (await activeId(page)) === 'sv-entry-backups', 'Left from the Save Store options should reach the Saves hub (got ' + await activeInfo(page) + ')');
    await page.keyboard.press('Enter');
    await page.waitForSelector('#scr-saveshub.active .srow', { timeout: 10000 }).catch(() => fail('the Saves hub entry should open the Saves hub'));
    await expect(async () => page.evaluate(() => document.querySelector('#scr-saveshub .kicker').textContent === 'Saves'), 'the Saves hub belongs to the Saves section (kicker)');
    await page.keyboard.press('Escape');
    await page.waitForSelector('#scr-saves.active', { timeout: 6000 }).catch(() => fail('Back from the Saves hub should return to the Saves tab'));
    await page.waitForFunction(() => !XS.router.busy(), null, { timeout: 4000 });
    await page.waitForFunction(() => document.activeElement && document.activeElement.getAttribute('data-nav-id') === 'sv-entry-backups', null, { timeout: 3000 }).catch(async () => fail('Back from the Saves hub should land on its entry (got ' + await activeInfo(page) + ')'));
    await page.keyboard.press('ArrowRight');
    await expect(async () => (await activeId(page)) === 'sv-entry-options', 'Right from the Saves hub entry should reach the Save Store options (got ' + await activeInfo(page) + ')');
    await page.keyboard.press('Enter');
    await page.waitForSelector('#scr-settings.active [data-nav-id="sv-nick"]', { timeout: 8000 }).catch(() => fail('Save Store options should open Settings → Saves'));
    await page.waitForFunction(() => { const a = document.activeElement; return !!a && !!a.closest('[data-group="saves"]'); }, null, { timeout: 3000 }).catch(async () => fail('Save Store options should focus the first Saves setting (got ' + await activeInfo(page) + ')'));
    await sleep(300);
    await shot(page, 'tv-b-saves-options.png');
    await page.keyboard.press('Escape');
    await page.waitForSelector('#scr-saves.active', { timeout: 6000 }).catch(() => fail('Back from the Save Store options should return to the Saves tab'));
    await page.waitForFunction(() => !XS.router.busy(), null, { timeout: 4000 });
    await expect(async () => page.evaluate(() => XS.router.stack.length === 1 && location.hash === '#saves' && document.querySelector('#tabs .tab.active').getAttribute('data-nav-id') === 'tab:saves'), 'after Back from the options the Saves tab should be highlighted with #saves (got ' + await page.evaluate(() => XS.router.stack.map((e) => e.id).join('>') + ' ' + location.hash) + ')');

    /* ---------- Settings → Setup (keyboard), run check, launch companion, install companion, ghost cleaner ---------- */
    await page.keyboard.press('o');
    await page.waitForSelector('#scr-settings.active .settings');
    await page.evaluate(() => document.querySelector('[data-nav-id="set-nav-general"]').focus());
    await keyTo(page, 'set-nav-setup', 20);
    await page.keyboard.press('Enter');
    await sleep(300);
    await expect(async () => page.evaluate(() => document.activeElement && document.activeElement.getAttribute('data-nav-id') === 'setup-check'), 'Enter on the Setup nav entry should focus the checklist row (got ' + await activeInfo(page) + ')');
    await page.keyboard.press('Enter');
    await page.waitForSelector('#scr-setup.active .check-row', { timeout: 10000 });
    await page.evaluate(() => document.querySelector('[data-nav-id="setup-run"]').focus());
    await page.keyboard.press('Enter');
    await waitToast(page, 'checks');
    await expect(async () => !!(await page.$('#scr-setup .pm-panel .qr svg')), 'Payload Manager QR missing');
    await expect(async () => (await page.$$('#scr-setup .payload-row')).length >= 5, 'companion payloads list missing');
    await expect(async () => (await page.$$('#scr-setup .ghost-row')).length >= 4, 'ghost icon list missing');
    await sleep(300);
    await shot(page, 'tv-b-setup.png');
    await expect(async () => !!(await page.$('#scr-setup [data-nav-id="check-shadowmount-launch"]')), '"Launch ShadowMountPlus now" should be offered when a copy exists and the API is silent');
    await page.click('#scr-setup [data-nav-id="check-shadowmount-launch"]');
    await waitToast(page, 'sent to the loader');
    await page.waitForFunction(() => { const r = document.querySelector('#scr-setup [data-nav-id="check-shadowmount"]'); return r && r.classList.contains('ok'); }, null, { timeout: 12000 }).catch(() => fail('after launching ShadowMountPlus the re-check should pass'));
    /* Garlic Save Manager: labelled check (not the raw id) with the saves engine's own start; a companion with no
       upstream download is listed without a Download button */
    await expect(async () => page.evaluate(() => { const r = document.querySelector('#scr-setup [data-nav-id="check-garlic"] .name'); return !!r && /Garlic Save Manager/.test(r.textContent) && !/^garlic/.test(r.textContent); }), 'the Garlic check should show its label, not the raw id');
    await expect(async () => !(await page.$('#scr-setup [data-nav-id="pl-np-fake-signout-install"]')) && !!(await page.$('#scr-setup [data-nav-id="pl-np-fake-signout"] .chip.warn')), 'an unavailable companion must be marked and must not offer a download');
    /* the profile transfer above started Garlic on demand: stop it in the mock, re-check, then start it from here */
    await page.evaluate(() => fetch('/api/__mock/garlic', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ mode: 'startable' }) }));
    await page.evaluate(() => document.querySelector('[data-nav-id="setup-run"]').click());
    await page.waitForSelector('#scr-setup [data-nav-id="check-garlic-start"]', { timeout: 10000 }).catch(() => fail('a stopped Garlic Save Manager should offer Start on its check'));
    await page.click('#scr-setup [data-nav-id="check-garlic-start"]');
    await waitToast(page, 'Garlic Save Manager is running', 15000);
    await page.waitForFunction(() => { const r = document.querySelector('#scr-setup [data-nav-id="check-garlic"]'); return r && r.classList.contains('ok'); }, null, { timeout: 12000 }).catch(() => fail('after starting Garlic Save Manager the re-check should pass'));
    /* only the pinned release can provide Garlic: the saves panel offers Download-and-start with the author credit,
       reports the version once the store started it, and none for a Garlic started outside the store */
    await page.evaluate(() => fetch('/api/__mock/garlic', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ mode: 'download' }) }));
    const mountGarlicPanel = () => page.evaluate(() => XS.saves.garlic(true).then(() => {
      let host = document.querySelector('#vt-garlic');
      if (!host) { host = document.createElement('div'); host.id = 'vt-garlic'; document.querySelector('#scr-setup').appendChild(host); }
      host.innerHTML = '';
      host.appendChild(XS.saves.garlicPanel({ always: true, nav: 'vt-garlic' }));
    }));
    await mountGarlicPanel();
    await page.waitForSelector('#vt-garlic [data-nav-id="vt-garlic-start"]', { timeout: 8000 }).catch(() => fail('a Garlic that can only be downloaded should offer its start button in the saves panel'));
    await expect(async () => page.evaluate(() => document.querySelector('#vt-garlic [data-nav-id="vt-garlic-start"]').textContent.trim() === XS.t('saves.garlic.install')), 'download mode: the Garlic panel should offer "' + 'Download and start Garlic"');
    await expect(async () => page.evaluate(() => { const ref = document.createElement('div'); ref.innerHTML = XS.util.icon('download'); const svg = document.querySelector('#vt-garlic [data-nav-id="vt-garlic-start"] svg'); return !!svg && svg.outerHTML === ref.firstElementChild.outerHTML; }), 'download mode: the Garlic start button should carry the download icon');
    await expect(async () => page.evaluate(() => document.querySelector('#vt-garlic').textContent.indexOf(XS.t('saves.garlic.credit')) >= 0), 'download mode: the Garlic panel should credit the author and the sha256-checked download');
    await page.click('#vt-garlic [data-nav-id="vt-garlic-start"]');
    await page.waitForFunction(() => { const el = document.querySelector('#vt-garlic .sv-garlic'); return !!el && el.textContent.indexOf(XS.t('saves.garlic.runningVersion', { v: '1.7' })) >= 0; }, null, { timeout: 15000 }).catch(() => fail('after the store started the downloaded Garlic the panel should name its version (1.7)'));
    await page.evaluate(() => fetch('/api/__mock/garlic', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ mode: 'running' }) }));
    await mountGarlicPanel();
    await page.waitForFunction(() => { const el = document.querySelector('#vt-garlic .sv-garlic'); return !!el && el.textContent.trim() === XS.t('saves.garlic.running'); }, null, { timeout: 8000 }).catch(() => fail('a Garlic started outside the store should be shown as running with no version'));
    await page.evaluate(() => document.querySelector('#vt-garlic').remove());
    await page.click('#scr-setup [data-nav-id="pl-etahen-dpi-helper-install"]');
    await waitToast(page, 'ready', 20000);
    await page.evaluate(() => { const r = document.querySelector('#scr-setup .ghost-row'); if (r) r.scrollIntoView(); });
    await sleep(200);
    await shot(page, 'tv-b-setup-ghosts.png');
    await page.click('#scr-setup [data-nav-id="ghost-PPSA01234-delete"]');
    await confirmDialog(page); await confirmDialog(page);
    await waitToast(page, 'deleted');
    await page.waitForFunction(() => !document.querySelector('#scr-setup [data-nav-id="ghost-PPSA01234"]'), null, { timeout: 8000 }).catch(() => fail('deleted ghost folder should leave the list'));
    await page.keyboard.press('Escape');
    await page.waitForSelector('#scr-settings.active', { timeout: 6000 });

    /* ---------- Settings → Debrid / Accounts / Saves / Pair ---------- */
    await page.click('[data-nav-id="set-nav-debrid"]');
    await page.waitForSelector('#scr-settings .debrid-row', { timeout: 8000 });
    await sleep(500);
    await expect(async () => page.evaluate(() => /••••/.test(document.querySelector('#scr-settings [data-nav-id="debrid-realdebrid"]').textContent)), 'connected service should show a masked key');
    await shot(page, 'tv-b-settings-debrid.png');
    await page.click('[data-nav-id="debrid-alldebrid-connect"]');
    await page.waitForSelector('.dialog [data-nav-id="prompt-input"]');
    await page.fill('.dialog [data-nav-id="prompt-input"]', 'alldebrid-test-key-123456');
    await page.keyboard.press('Enter');
    await waitToast(page, 'AllDebrid connected', 10000);
    await page.waitForFunction(() => { const r = document.querySelector('#scr-settings [data-nav-id="debrid-alldebrid"]'); return r && r.classList.contains('on'); }, null, { timeout: 8000 }).catch(() => fail('AllDebrid row should turn connected'));
    await page.click('[data-nav-id="set-nav-accounts"]');
    await page.waitForSelector('#scr-settings .acc-row', { timeout: 8000 });
    await sleep(800);
    await shot(page, 'tv-b-settings-accounts.png');
    await page.click('[data-nav-id="acc-2-activate"]');
    await page.waitForSelector('.sheet [data-nav-id="act-go"]');
    await shot(page, 'tv-b-accounts-activate.png');
    await page.click('.sheet [data-nav-id="act-go"]');
    await confirmDialog(page);
    await waitToast(page, 'activated');
    await page.waitForFunction(() => !!document.querySelector('#scr-settings [data-nav-id="acc-2-clear"]'), null, { timeout: 8000 }).catch(() => fail('activated slot should offer Clear'));
    /* fake sign-in: the sign-in payload is offered, the sign-out entry (no upstream asset) is listed without a button */
    await page.click('[data-nav-id="acc-fake"]');
    await page.waitForSelector('.dialog [data-nav-id="fake-0"]', { timeout: 8000 }).catch(() => fail('the fake sign-in dialog should list the companion payloads'));
    await expect(async () => page.evaluate(() => {
      const rows = Array.from(document.querySelectorAll('.dialog .lrow'));
      const signin = rows.find((r) => /sign-in/i.test(r.querySelector('.name').textContent)), signout = rows.find((r) => /sign-out/i.test(r.querySelector('.name').textContent));
      return !!signin && !!signin.querySelector('button') && !!signout && !signout.querySelector('button') && /no sign-out payload/.test(signout.textContent) && /Sign out/.test(document.querySelector('.dialog p').textContent);
    }), 'fake sign-in dialog: the sign-out entry must not offer a download, and the text should say how to sign out');
    await page.keyboard.press('Escape');
    await page.waitForFunction(() => !document.querySelector('.dialog'), null, { timeout: 6000 }).catch(() => fail('the fake sign-in dialog should close'));
    await page.click('[data-nav-id="set-nav-saves"]');
    await page.waitForSelector('#scr-settings [data-nav-id="sv-root"]');
    await sleep(800);
    await shot(page, 'tv-b-settings-saves.png');
    await page.click('[data-nav-id="set-nav-pair"]');
    await page.waitForSelector('#scr-settings [data-nav-id="pair-local"]');
    await sleep(700);
    await expect(async () => page.evaluate(() => /xeno\.local:/.test(document.querySelector('#scr-settings [data-nav-id="pair-local"]').textContent)), 'Pair a device should show http://xeno.local:<port>');
    await expect(async () => page.evaluate(() => /Answering on this network/.test(document.querySelector('#scr-settings [data-nav-id="pair-local"]').textContent)), 'Pair a device should read the mDNS state from the {ok, mdns:{…}} envelope');
    await expect(async () => !!(await page.$('#scr-settings [data-nav-id="pair-mdns"]')), 'mDNS toggle missing');
    await expect(async () => page.evaluate(() => document.querySelectorAll('#scr-settings .pd-row button[data-nav-id$="-unpair"]').length === 4), 'Pair a device should list the paired devices, each with Unpair');
    await sleep(200);
    await shot(page, 'tv-b-settings-pair.png');
    /* downloads group: Phase 2 rows */
    await page.click('[data-nav-id="set-nav-downloads"]');
    await sleep(300);
    for (const id of ['set-prefer', 'set-turbo', 'set-autosets', 'set-order', 'set-delpkg', 'set-restresume', 'set-restartresume', 'set-extract', 'set-delarchive', 'set-schedule']) await expect(async () => !!(await page.$('#scr-settings [data-nav-id="' + id + '"]')), 'Settings → Downloads row missing: ' + id);
    const dlSetting = (key, want) => page.waitForFunction(([k, w]) => fetch('/api/settings').then((r) => r.json()).then((b) => (b.settings || b).downloads[k] === w), [key, want], { timeout: 4000, polling: 200 }).then(() => true, () => false);
    await expect(async () => !(await page.evaluate(() => document.querySelector('#scr-settings [data-nav-id="set-delpkg"]').getAttribute('aria-checked') === 'true')), '"Delete the PKG after installing" should be off by default');
    await page.click('#scr-settings [data-nav-id="set-delpkg"]');
    await expect(() => dlSetting('deletePkgAfterInstall', true), 'the "Delete the PKG after installing" toggle should save downloads.deletePkgAfterInstall');
    await page.click('#scr-settings [data-nav-id="set-delpkg"]');
    await expect(() => dlSetting('deletePkgAfterInstall', false), 'turning "Delete the PKG after installing" off again should save it');
    await expect(async () => page.evaluate(() => document.querySelector('#scr-settings [data-nav-id="set-restartresume"]').getAttribute('aria-checked') === 'true'), '"Resume after a restart" should be on by default');
    await page.click('#scr-settings [data-nav-id="set-restartresume"]');
    await expect(() => dlSetting('resumeAfterRestart', false), 'the "Resume after a restart" toggle should save downloads.resumeAfterRestart');
    await page.click('#scr-settings [data-nav-id="set-restartresume"]');
    await expect(() => dlSetting('resumeAfterRestart', true), 'turning "Resume after a restart" on again should save it');
    await page.click('[data-nav-id="set-schedule"]');
    await page.waitForSelector('.sheet [data-nav-id="sch-save"]');
    await page.click('.sheet [data-nav-id="sch-preset-1"]');
    await page.click('.sheet [data-nav-id="sch-save"]');
    await waitToast(page, '00:00 and 08:00');
    await expect(async () => (await api('/api/settings')).downloads.scheduleStart === '00:00', 'schedule window should be saved');

    /* ---------- Settings → Updates: the app card names a newer app (updates/tv-app.json) and updates it ---------- */
    await api('/api/__mock/app', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ installed: true, version: '01.003.001', latest: '1.4.0' }) });
    await page.click('[data-nav-id="set-nav-updates"]');
    await page.waitForSelector('#scr-settings [data-nav-id="upd-check"]');
    await page.click('#scr-settings [data-nav-id="upd-check"]');   /* Check for updates also reads the app's release again */
    await page.waitForFunction(() => { const c = document.querySelector('#scr-settings .app-card'); return c && /Update available: 1\.4\.0/.test(c.textContent); }, null, { timeout: 10000 }).catch(() => fail('the app card should say "Update available: 1.4.0" when tv-app.json names a newer app'));
    await expect(async () => page.evaluate(() => { const b = document.querySelector('#scr-settings .app-card [data-nav-id="app-install"]'); return !!b && /Update the app/.test(b.textContent) && b.classList.contains('primary'); }), 'the app card button should read "Update the app" (primary) when a newer app exists');
    await expect(async () => serverLog.some((l) => /tv-app\.json read again \(\?check=1\)/.test(l)), 'Check for updates should ask GET /api/app/status?check=1');
    await page.evaluate(() => { const c = document.querySelector('#scr-settings .app-card'); if (c) c.scrollIntoView({ block: 'center' }); });
    await sleep(300);
    await shot(page, 'tv-b-settings-app-update.png');
    await page.click('#scr-settings .app-card [data-nav-id="app-install"]');
    await waitToast(page, 'XENO STORE app installed', 20000);
    await page.waitForFunction(() => { const c = document.querySelector('#scr-settings .app-card'); return c && !/Update available/.test(c.textContent) && /Reinstall/.test(c.textContent); }, null, { timeout: 8000 }).catch(() => fail('after the update the app card should show the installed app (Reinstall / update), not "Update available"'));
    /* the app as the folder /data/homebrew/PPSA99500: no update offered, a hint instead of the button (an install would add a second copy) */
    await api('/api/__mock/app', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ version: '01.003.001', latest: '1.4.0', copy: 'folder' }) });
    await page.click('#scr-settings [data-nav-id="upd-check"]');
    await page.waitForFunction(() => { const c = document.querySelector('#scr-settings .app-card'); return c && /XENO STORE does not update/.test(c.textContent); }, null, { timeout: 10000 }).catch(() => fail('a folder copy of the app should get the "does not update" hint on the app card'));
    await expect(async () => page.evaluate(() => { const c = document.querySelector('#scr-settings .app-card'); return !c.querySelector('[data-nav-id="app-install"]') && !/Update available/.test(c.textContent); }), 'a folder copy of the app should get no install button and no "Update available"');
    await api('/api/__mock/app', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ copy: '' }) });

    /* ---------- Uninstall sheet, keyboard: open from the nav, cancel with Escape ---------- */
    await page.evaluate(() => document.querySelector('[data-nav-id="set-nav-general"]').focus());
    await keyTo(page, 'set-nav-uninstall', 20);
    await page.keyboard.press('Enter');
    await sleep(300);
    await expect(async () => page.evaluate(() => document.activeElement && document.activeElement.getAttribute('data-nav-id') === 'uninstall-open'), 'Enter on the Uninstall nav entry should focus the uninstall row (got ' + await activeInfo(page) + ')');
    await page.keyboard.press('Enter');
    await page.waitForSelector('.sheet [data-nav-id="un-go"]');
    await expect(async () => page.evaluate(() => document.querySelector('.sheet [data-nav-id="un-keep"]').classList.contains('on')), '"Keep downloaded games" should be on by default');
    await expect(async () => page.evaluate(() => !document.querySelector('.sheet [data-nav-id="un-pm"]')), 'the uninstall sheet should not offer a Payload Manager copy the store never places');
    await sleep(300);
    await shot(page, 'tv-b-uninstall-sheet.png');
    await page.keyboard.press('Escape');
    await noLayer(page);
    await expect(async () => page.evaluate(() => !!document.querySelector('#scr-settings.active')), 'cancelling the uninstall sheet should return to Settings');

    /* ---------- Command palette + shortcuts overlay ---------- */
    await page.click('[data-nav-id="tab:discover"]');
    await page.waitForSelector('#scr-discover.active');
    await page.keyboard.press('Control+k');
    await page.waitForSelector('.palette [data-nav-id="palette-input"]', { timeout: 5000 });
    await page.fill('.palette [data-nav-id="palette-input"]', 'debrid');
    await page.waitForFunction(() => document.querySelectorAll('.palette .pal-item').length >= 1 && /Debrid/.test(document.querySelector('.palette .pal-item').textContent), null, { timeout: 4000 }).catch(() => fail('palette should list "Settings → Debrid" for "debrid"'));
    await sleep(200);
    await shot(page, 'tv-b-palette.png');
    await page.keyboard.press('Enter');
    await page.waitForSelector('#scr-settings.active', { timeout: 6000 });
    await expect(async () => page.evaluate(() => !!document.querySelector('[data-nav-id="set-nav-debrid"].on')), 'palette entry should open Settings → Debrid');
    await page.waitForFunction(() => !XS.router.busy() && !XS.ui.layers.length && !document.querySelector('.layer'), null, { timeout: 5000 });
    await page.keyboard.press('Control+k');
    await page.waitForSelector('.palette [data-nav-id="palette-input"]');
    await page.fill('.palette [data-nav-id="palette-input"]', 'community');
    await page.waitForFunction(() => { const it = document.querySelector('.palette .pal-item'); return !!it && /Saves/.test(it.textContent) && /Screen/.test(it.textContent); }, null, { timeout: 4000 }).catch(async () => fail('palette should find the Saves tab by "community" (Community Saves) (' + await page.evaluate(() => Array.from(document.querySelectorAll('.palette .pal-item')).map((e) => e.textContent.trim().slice(0, 40)).join(' | ')) + ')'));
    await page.fill('.palette [data-nav-id="palette-input"]', 'god of');
    await page.waitForFunction(() => Array.from(document.querySelectorAll('.palette .pal-item .kind')).some((k) => /Game/.test(k.textContent)), null, { timeout: 4000 }).catch(async () => fail('palette should find catalog games (' + await page.evaluate(() => JSON.stringify({ active: document.activeElement && (document.activeElement.className + '/' + document.activeElement.value), palettes: document.querySelectorAll('.palette').length, layers: XS.ui.layers.map((l) => l.key).join('+'), busy: XS.router.busy(), items: Array.from(document.querySelectorAll('.palette .pal-item')).map((e) => e.textContent.trim().slice(0, 30)), search: (function () { try { return XS.ca.search('god of', 2).map((x) => x.g.title); } catch (e) { return 'ERR ' + e.message; } })() })) + ')'));
    await page.keyboard.press('Escape'); await noLayer(page);
    await page.keyboard.press('?');
    await page.waitForSelector('.dialog .sc-row', { timeout: 5000 });
    await shot(page, 'tv-b-shortcuts.png');
    await page.keyboard.press('Escape'); await noLayer(page);

    /* ---------- Accessibility: large text + high contrast; controller remap legend ---------- */
    await put('/api/settings', { general: { largeText: true, highContrast: true } });
    await page.evaluate(() => fetch('/api/settings').then((r) => r.json()).then((s) => { XS.state.set('settings', s); XS.actions.applySettings(s); }));
    await page.waitForFunction(() => document.documentElement.getAttribute('data-large-text') === '1' && document.documentElement.getAttribute('data-contrast') === 'high', null, { timeout: 4000 }).catch(() => fail('large text / high contrast attributes not applied'));
    await page.click('[data-nav-id="tab:discover"]');
    await page.waitForSelector('#scr-discover.active .card');
    await waitCovers(page, 10);
    await sleep(400);
    await shot(page, 'tv-b-a11y-discover.png');
    await page.click('[data-nav-id="tab:downloads"]');
    await page.waitForSelector('#scr-downloads.active .job');
    await sleep(400);
    await shot(page, 'tv-b-a11y-downloads.png');
    await expect(async () => page.evaluate(() => parseFloat(getComputedStyle(document.querySelector('.lrow .name, .job .ttl h4')).fontSize) >= 18), 'large text should enlarge row titles');
    await put('/api/settings', { general: { largeText: false, highContrast: false, controllerSwapConfirm: true } });
    await page.evaluate(() => fetch('/api/settings').then((r) => r.json()).then((s) => { XS.state.set('settings', s); XS.actions.applySettings(s); }));
    await page.waitForFunction(() => !document.documentElement.hasAttribute('data-large-text') && document.documentElement.getAttribute('data-swap-confirm') === '1', null, { timeout: 4000 }).catch(() => fail('swap attribute not applied'));
    await page.evaluate(() => { const r = document.querySelector('#scr-downloads .job'); if (r) r.focus(); });
    await sleep(300);
    await expect(async () => page.evaluate(() => { const first = document.querySelector('#legend .hint .glyph'); return first && first.classList.contains('circle'); }), 'with swap on, the legend select hint should show ○ (got ' + await page.evaluate(() => { const f = document.querySelector('#legend .hint .glyph'); return f ? f.getAttribute('class') : 'none'; }) + ')');
    await expect(async () => page.evaluate(() => { const pads = navigator.getGamepads && navigator.getGamepads(); return navigator.getGamepads && navigator.getGamepads._cbWrapped === true && (pads === null || typeof pads === 'object'); }), 'navigator.getGamepads should be wrapped for the ✕/○ swap');
    await put('/api/settings', { general: { controllerSwapConfirm: false } });
    await page.evaluate(() => fetch('/api/settings').then((r) => r.json()).then((s) => { XS.state.set('settings', s); XS.actions.applySettings(s); }));
    await sleep(200);

    /* ---------- Uninstall for real: final screen ---------- */
    await page.evaluate(() => XS.router.go('settings', { group: 'uninstall' }));
    await page.waitForSelector('#scr-settings.active [data-nav-id="uninstall-open"]');
    await page.click('[data-nav-id="uninstall-open"]');
    await page.waitForSelector('.sheet [data-nav-id="un-go"]');
    await page.click('.sheet [data-nav-id="un-go"]');
    await confirmDialog(page);
    await page.waitForSelector('.uninstalled', { timeout: 10000 });
    await expect(async () => page.evaluate(() => { const tx = document.querySelector('.uninstalled').textContent; return /removed/.test(tx) && /\/data\/homebrew\/XENO-STORE/.test(tx) && /Kept/.test(tx) && /\/mnt\/ext0\/xeno-store/.test(tx); }), 'final screen should list what was removed (home-screen app + data dir) and kept (downloads on every drive)');
    await sleep(400);
    await shot(page, 'tv-b-uninstalled.png');

    const missing = await page.evaluate(() => XS.i18n.missing());
    if (missing.length) fail('missing i18n keys: ' + missing.join(', '));
    await ctx.close();

    /* ================= phone 390×844 ================= */
    const phone = await browser.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true, locale: 'en-US' });
    const pp = await phone.newPage();
    watch(pp, 'phone');
    await pp.goto(BASE + '/#downloads', { waitUntil: 'load' });
    await pp.waitForSelector('#scr-downloads.active .job', { timeout: 20000 });
    /* by now the seeded install sets may have finished (6× speed): show whichever tab holds one */
    if (!(await pp.$('#scr-downloads .jobgroup'))) { await pp.click('[data-nav-id="dltab-finished"]'); await pp.waitForSelector('#scr-downloads .jobgroup', { timeout: 8000 }).catch(() => fail('phone: no install-set group in Active or Finished')); }
    await waitCovers(pp, 3);
    await sleep(500);
    await shot(pp, 'phone-b-downloads.png');
    await expect(async () => pp.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth + 1 && document.querySelector('#scr-downloads').scrollWidth <= window.innerWidth + 1), 'phone Downloads has horizontal overflow');
    await pp.evaluate(() => XS.router.go('settings', { group: 'debrid' }));
    await pp.waitForSelector('#scr-settings.active .debrid-row', { timeout: 8000 });
    await sleep(400);
    await shot(pp, 'phone-b-settings.png');
    await expect(async () => pp.evaluate(() => document.querySelector('#scr-settings').scrollWidth <= window.innerWidth + 1), 'phone Settings has horizontal overflow');
    await expect(async () => !!(await pp.$('#phonebar [data-nav-id="pb:saves"]')), 'the phone bar should have a Saves place');
    await pp.click('[data-nav-id="pb:saves"]');
    await pp.waitForSelector('#scr-saves.active [data-nav-id="sv-entry-options"]', { timeout: 10000 });
    await pp.waitForSelector('#scr-saves.active .save-card', { timeout: 10000 });
    await waitCovers(pp, 3);
    await sleep(400);
    await shot(pp, 'phone-b-saves-tab.png');
    await expect(async () => pp.evaluate(() => document.querySelector('#phonebar [data-nav-id="pb:saves"]').classList.contains('active') && document.querySelector('#scr-saves').scrollWidth <= window.innerWidth + 1), 'phone Saves tab: Saves highlighted in the phone bar, no horizontal overflow');
    await pp.click('[data-nav-id="sv-entry-backups"]');
    await pp.waitForSelector('#scr-saveshub.active .srow', { timeout: 10000 });
    await sleep(400);
    await shot(pp, 'phone-b-saves-hub.png');
    await expect(async () => pp.evaluate(() => document.querySelector('#scr-saveshub').scrollWidth <= window.innerWidth + 1), 'phone Saves hub has horizontal overflow');
    const missingPhone = await pp.evaluate(() => XS.i18n.missing());
    if (missingPhone.length) fail('phone: missing i18n keys: ' + missingPhone.join(', '));
    await phone.close();
  } finally {
    await browser.close();
    server.kill();
  }
  console.log('');
  if (problems.length) { console.log(`verify-b: ${problems.length} problem(s)`); problems.forEach((p) => console.log('  - ' + p)); process.exit(1); }
  console.log(`verify-b: OK — screenshots in ${OUT} (dist ${(distSize / 1024).toFixed(1)} KiB)`);
})().catch((err) => { console.error('verify-b: crashed: ' + (err.stack || err)); console.error(serverLog.slice(-20).join('')); server.kill(); if (problems.length) problems.forEach((p) => console.log('  - ' + p)); process.exit(1); });
