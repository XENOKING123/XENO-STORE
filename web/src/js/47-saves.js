/* XENO STORE — 47-saves.js: PS5 Save Data Store UI (§17.22): the game page "Saves (n)" tab, the Community Saves
   screen (id "saves"), save detail sheet, Install / Download / Report flows and the Upload-my-save wizard.
   Uses /api/saves/* exactly as SPEC §17.22.7; the re-sign engine tier is shown honestly (native / garlic / raw), with
   Garlic Save Manager's state (GET /api/saves/garlic) and a Start / Install action when community saves need it. */
(function () {
  'use strict';
  var XS = window.XS, U = XS.util, S = XS.state, ui = XS.ui, ca = XS.ca, h = U.h, t = XS.t;
  var root = null;
  var CATS = ['starter', 'platinum', 'checkpoint', 'modded', 'chapter', 'other'];
  var SORTS = ['newest', 'popular', 'version'];
  var cache = { all: null, allAt: 0, building: false, byGame: {}, engine: null, engineAt: 0, users: null, garlic: null, garlicAt: 0, site: null };
  var scr = { q: '', cat: '', sort: 'newest', compatOnly: false };

  /* ---------- data ---------- */
  function normalize(sv) {
    var s = {};
    for (var k in sv) s[k] = sv[k];
    s.id = String(s.id || '');
    s.category = CATS.indexOf(s.category) >= 0 ? s.category : inferCategory((s.name || '') + ' ' + (s.description || '') + ' ' + (s.notes || '') + ' ' + (s.tags || []).join(' '));
    s.name = s.name || s.file_name || s.game || t('saves.untitled');
    s.title = s.title || s.game || (S.data.byTitleId[s.titleId] ? S.data.byTitleId[s.titleId].title : s.titleId || '');
    s.author = s.author || s.shared_by || '';
    s.gameVersion = s.gameVersion || s.game_version || '';
    s.createdAt = s.createdAt || s.created_at || '';
    s.downloads = Number(s.downloads) || 0;
    s.size = Number(s.size || s.file_size) || 0;
    s.tags = Array.isArray(s.tags) ? s.tags : [];
    s.compat = compatOf(sv);
    var g = s.gameId ? S.game(s.gameId) : (s.titleId ? S.data.byTitleId[s.titleId] : null);
    s._game = g || null;
    if (!s.cover && !s.cover_path && g) s._coverFallback = g.cover;
    return s;
  }
  /* The console sends a flat compat string (match | update_game | not_installed | unknown) next to installedVersion,
     versionCompare (save vs installed) and otherRegion / installedTitleId; the badges use {status, installedVersion}. */
  function compatOf(sv) {
    var c = sv.compat, o = {};
    if (c && typeof c === 'object') { for (var k in c) o[k] = c[k]; if (!o.status) o.status = 'unknown'; return o; }
    var flat = String(c || ''), st = 'unknown';
    if (flat === 'match' || flat === 'ok') st = Number(sv.versionCompare) < 0 ? 'newer' : 'ok';
    else if (flat === 'newer' || flat === 'older') st = 'newer';
    else if (flat === 'update_game' || flat === 'update') st = 'update';
    else if (flat === 'other_region' || flat === 'region') st = 'region';
    else if (flat === 'not_installed' || flat === 'notInstalled') st = sv.otherRegion ? 'region' : 'notInstalled';
    return { status: st, installedVersion: sv.installedVersion || '', installedTitleId: sv.installedTitleId || '' };
  }
  function inferCategory(text) {
    var x = String(text || '').toLowerCase();
    if (/100\s*%|platinum|all trophies/.test(x)) return 'platinum';
    if (/\b(start|starter|ng\+|new game\+|max|maxed|unlocked)\b/.test(x)) return 'starter';
    if (/\b(trophy|boss|checkpoint)\b/.test(x)) return 'checkpoint';
    if (/\b(mod|modded|glitch|glitched|hack)\b/.test(x)) return 'modded';
    if (/\b(chapter|act|mission|level)\b/.test(x)) return 'chapter';
    return 'other';
  }
  function fetchAll(force) {
    if (!force && cache.all && Date.now() - cache.allAt < 120000) return Promise.resolve(cache.all);
    return ca.api.savesStore({}).then(function (r) {
      noteSite(r);
      cache.all = (r.saves || []).map(normalize);
      cache.building = !!r.building;
      cache.allAt = cache.building && !cache.all.length ? 0 : Date.now();   /* the console is still fetching the index: ask again soon */
      return cache.all;
    });
  }
  function fetchGame(g, force) {
    var key = g.id;
    var c = cache.byGame[key];
    if (!force && c && Date.now() - c.at < 120000) return Promise.resolve(c.list);
    var tid = (g.titleIds || [])[0] || '';
    return ca.api.savesStore(tid ? { titleId: tid, gameId: g.id } : { gameId: g.id }).then(function (r) {
      noteSite(r);
      var list = (r.saves || []).map(normalize);
      cache.byGame[key] = { list: list, at: Date.now() };
      return list;
    });
  }
  /* GET /api/saves/store says whether the console's backend is the save site (the owner's XenoKing
     collection): siteUpload + backendName. The share wizard then uploads there and the save is live at once. */
  function noteSite(r) { if (r && typeof r.siteUpload === 'boolean') cache.site = { on: r.siteUpload, name: r.backendName || 'XenoKing' }; }
  function siteInfo() {
    if (cache.site) return Promise.resolve(cache.site);
    return fetchAll().then(function () { return cache.site || { on: false, name: '' }; }, function () { return { on: false, name: '' }; });
  }
  function engine() {
    if (cache.engine && Date.now() - cache.engineAt < 300000) return Promise.resolve(cache.engine);
    var st = S.data.status;
    if (st && st.savesEngine && st.savesEngine.tier) { cache.engine = st.savesEngine; cache.engineAt = Date.now(); return Promise.resolve(cache.engine); }
    return ca.api.savesEngine().then(function (r) { cache.engine = { tier: r.tier || 'raw', detail: r.detail || '' }; cache.engineAt = Date.now(); return cache.engine; }, function () { return { tier: 'unknown', detail: '' }; });
  }
  function tierText(tier) { return t('saves.tier.' + (['native', 'garlic', 'raw'].indexOf(tier) >= 0 ? tier : 'unknown')); }
  function tierHint(tier) { return t('saves.tierHint.' + (['native', 'garlic', 'raw'].indexOf(tier) >= 0 ? tier : 'unknown')); }
  function enginePill(e) {
    var tier = e ? e.tier : 'unknown';
    var cls = tier === 'native' ? '.good' : tier === 'garlic' ? '.acc' : tier === 'raw' ? '.warn' : '';
    return h('span.chip.engine' + cls, { title: tierHint(tier) + (e && e.detail ? ' · ' + e.detail : '') }, [ui.svg('shield'), h('span', { text: t('saves.engine', { tier: tierText(tier) }) })]);
  }
  function users() {
    if (cache.users) return Promise.resolve(cache.users);
    return ca.api.savesUsers().then(function (r) { cache.users = r.users || []; return cache.users; });
  }
  function compatible(s) { return s.compat.status === 'ok' || s.compat.status === 'newer'; }

  /* ---------- Garlic Save Manager (the re-signer community saves need without the native engine) ---------- */
  function garlic(force) {
    if (!force && cache.garlic && Date.now() - cache.garlicAt < 30000) return Promise.resolve(cache.garlic);
    return ca.api.get('/api/saves/garlic', { timeout: 8000 }).then(function (r) { cache.garlic = r.garlic || null; cache.garlicAt = Date.now(); return cache.garlic; }, function () { return null; });
  }
  var garlicBusy = null;
  /* POST /api/saves/garlic/start → a console job (find or download the ELF, send it to the loader, wait for it). */
  function startGarlic(btn) {
    if (garlicBusy) return garlicBusy;
    if (btn) btn.classList.add('busy');
    ui.toast({ cls: 'gold', title: t('saves.garlic.starting') });
    garlicBusy = ca.api.post('/api/saves/garlic/start', {}, { timeout: 20000 }).then(function (r) {
      var j = r.job || {};
      if (j.state === 'done' || j.state === 'failed' || j.state === 'cancelled' || !j.id || !(XS.cb && XS.cb.watchJob)) return j;
      return XS.cb.watchJob(j.id, null, { every: 1000, maxPolls: 150 });
    }).then(function (j) {
      cache.engine = null;
      return garlic(true).then(function (gs) {
        if (gs && gs.running) { XS.sound.done(); ui.toast({ cls: 'good', title: t('saves.garlic.started'), desc: gs.version ? 'v' + gs.version : '' }); }
        else ui.toast({ cls: 'bad', title: t('saves.garlic.startFailed'), desc: (j && j.error) || (gs && gs.detail) || '', timeout: 9000 });
        return gs;
      });
    }, function (err) { ui.errorToast(err, t('saves.garlic.start')); return null; }).then(function (gs) {
      garlicBusy = null;
      if (btn) btn.classList.remove('busy');
      return gs;
    });
    return garlicBusy;
  }
  /* Status panel, shown when the native engine is not available (opts.always: whatever the tier). Fills in async. */
  function garlicPanel(opts) {
    opts = opts || {};
    var box = h('div.sv-garlic');
    Promise.all([engine(), garlic()]).then(function (a) {
      var e = a[0], gs = a[1];
      if (!gs || (!opts.always && e && e.tier === 'native')) return;
      paintGarlic(box, gs, opts);
    });
    return box;
  }
  function paintGarlic(box, gs, opts) {
    U.clear(box);
    var pre = opts.nav || 'sv-garlic';
    if (gs.running) {
      box.appendChild(h('div.panel.note', { style: { marginTop: '12px' } }, [h('div.row', [ui.svg('check'), h('span', { text: gs.version ? t('saves.garlic.runningVersion', { v: gs.version }) : t('saves.garlic.running') })])]));
      return;
    }
    var label = gs.elfPath ? t('saves.garlic.start') : t('saves.garlic.install');
    var btn = h('button.btn.sm.primary', { nav: pre + '-start', html: U.icon(gs.elfPath ? 'play' : 'download') + '<span>' + U.escapeHtml(label) + '</span>' });
    btn.addEventListener('click', function () {
      startGarlic(btn).then(function (ns) { if (!ns) return; paintGarlic(box, ns, opts); if (ns.running && opts.onStarted) opts.onStarted(ns); });
    });
    var lines = [h('div.row', [ui.svg('alert'), h('b', { text: t('saves.garlic.neededShort') })]), h('p', { text: t('saves.garlic.needed') })];
    lines.push(h('p.dim.small', { text: gs.available ? t('saves.garlic.canStart') : t('saves.garlic.cannotStart') + (gs.detail ? ' (' + gs.detail + ')' : '') }));
    if (!gs.elfPath) lines.push(h('p.dim.small', { text: t('saves.garlic.credit') }));
    lines.push(h('div', { style: { marginTop: '8px' } }, [btn]));
    box.appendChild(h('div.panel.note.warn', { style: { marginTop: '12px' } }, lines));
  }

  /* ---------- widgets ---------- */
  function compatBadge(s, long) {
    var c = s.compat || {}, st = c.status || 'unknown';
    var cls = st === 'ok' || st === 'newer' ? '.good' : st === 'update' || st === 'region' ? '.warn' : st === 'notInstalled' ? '.bad' : '';
    var icon = st === 'ok' || st === 'newer' ? 'check' : st === 'notInstalled' ? 'x' : st === 'unknown' ? 'info' : 'alert';
    if (long) return h('div.compat-line' + cls, [ui.svg(icon), h('span', { text: compatText(s) })]);
    var text = t('saves.compat.short.' + (['ok', 'newer', 'update', 'region', 'notInstalled', 'unknown'].indexOf(st) >= 0 ? st : 'unknown'));
    return h('span.chip.sm.compat' + cls, { title: compatText(s) }, [ui.svg(icon), h('span', { text: text })]);
  }
  function compatText(s) {
    var c = s.compat || {}, st = c.status || 'unknown';
    if (c.message) return c.message;
    if (st === 'ok') return t('saves.compat.ok', { v: c.installedVersion || s.gameVersion || '' });
    if (st === 'newer') return t('saves.compat.newer', { v: s.gameVersion || '', mine: c.installedVersion || '' });
    if (st === 'update') return t('saves.compat.update', { v: s.gameVersion || '', mine: c.installedVersion || '' });
    if (st === 'region') return t('saves.compat.region', { tid: s.titleId || '' });
    if (st === 'notInstalled') return t('saves.compat.notInstalled');
    return t('saves.compat.unknown');
  }
  function coverEl(s) { return ui.cover(ca.api.imageUrl(s.cover || s.cover_path || s.screenshot || ''), s._coverFallback || '', s.title); }
  function catChip(cat) { return h('span.chip.sm.cat.' + cat, { text: t('saves.cat.' + cat) }); }
  function metaLine(s) {
    var parts = [];
    if (s.author) parts.push(t('saves.by', { name: s.author }));
    if (s.createdAt) parts.push(U.fmtDate(s.createdAt));
    if (s.size) parts.push(U.fmtBytes(s.size));
    if (s.downloads) parts.push(t('saves.nDownloads', { n: s.downloads }));
    if (s.gameVersion) parts.push('v' + s.gameVersion);
    if (s.region) parts.push(s.region);
    return parts.join(' · ');
  }
  /* Vertical card for rails / grids */
  function card(s, prefix) {
    var c = h('button.save-card', { nav: (prefix || 'sv') + ':' + s.id, data: { hints: 'open', save: s.id } });
    c.appendChild(coverEl(s));
    c.appendChild(h('div.t.clamp2', { text: s.name }));
    c.appendChild(h('div.g.ellipsis', { text: s.title }));
    var row = h('div.row2', [catChip(s.category), compatBadge(s)]);
    c.appendChild(row);
    c.addEventListener('click', function () { openSave(s); });
    return c;
  }

  /* ---------- Save spotlight: the showcase band on the Saves screen and Discover ----------
     The most downloaded saves (compatible ones first) on the game's art, with Install front and centre.
     Auto-advances like the Discover stage; holds while you browse inside it, on other screens and in layers. */
  var SPOT_PERIOD = 8000, SPOT_IDLE = 20000;
  function spotPool(all, n) {
    var pool = all.filter(function (s) { return s.url || s.id; }).slice();
    pool.sort(function (a, b) {
      var ca2 = compatible(a) ? 1 : 0, cb2 = compatible(b) ? 1 : 0;
      return cb2 - ca2 || b.downloads - a.downloads || ((b.createdAt || '') < (a.createdAt || '') ? -1 : (b.createdAt || '') > (a.createdAt || '') ? 1 : 0);
    });
    return pool.slice(0, n || 6);
  }
  function spotArt(s) {
    var g = s._game;
    var src = (g && (g.hero || g.cover)) || s.cover || s.cover_path || s.screenshot || '';
    var fb = (g && (g.heroFallback || g.coverFallback)) || s._coverFallback || '';
    return ui.cover(ca.api.imageUrl(src), fb, s.title);
  }
  function spotlight(list, opts) {
    opts = opts || {};
    var pool = spotPool(list, opts.max || 6);
    if (!pool.length) return null;
    var pre = opts.id || 'spot';
    var el = h('section.sv-spot' + (opts.compact ? '.compact' : ''), { data: { navRegion: '' }, 'aria-label': t('saves3.spotlight') });
    var artA = h('i.sv-art.live'), artB = h('i.sv-art');
    el.appendChild(h('div.sv-spot-art', [artA, artB]));
    var body = h('div.sv-spot-body');
    el.appendChild(body);
    var picks = h('div.sv-spot-picks');
    var tiles = pool.map(function (s, i) {
      var b = h('button.sv-pick', { nav: pre + '-pick:' + i, data: { hints: 'select' }, 'aria-label': s.name + ' — ' + s.title }, [coverEl(s), h('span.n.ellipsis', { text: s.name })]);
      b.addEventListener('click', function () { if (st.idx === i) openSave(s); else select(i); });
      b.addEventListener('focus', function () { if (st.idx !== i) select(i); });
      picks.appendChild(b);
      return b;
    });
    if (pool.length > 1) el.appendChild(picks);
    var bar = h('div.sv-spot-bar', [h('i')]);
    if (pool.length > 1) el.appendChild(bar);
    var st = { idx: -1, elapsed: 0, last: Date.now(), lastInput: 0, held: false, timer: 0, live: artA };
    el.addEventListener('keydown', function () { st.lastInput = Date.now(); }, true);
    el.addEventListener('pointerdown', function () { st.lastInput = Date.now(); }, true);
    function paintBody(s, i) {
      U.clear(body);
      var rankText = s.downloads > 0 ? t('saves3.rank', { n: i + 1 }) : t('saves3.fresh');
      body.appendChild(h('div.sv-spot-kicker', [h('span.rk-no', { text: String(i + 1) }), h('span', { text: t('saves3.spotlight') + ' · ' + rankText })]));
      body.appendChild(h('h2.sv-spot-name', { text: s.name }));
      body.appendChild(h('div.sv-spot-game.ellipsis', { text: s.title }));
      body.appendChild(h('div.chips', [catChip(s.category), compatBadge(s)].concat(s.tags.slice(0, 3).map(function (x) { return h('span.chip.sm', { text: '#' + x }); }))));
      if (s.description && !opts.compact) body.appendChild(h('p.sv-spot-desc.clamp2', { text: s.description }));
      body.appendChild(h('div.sv-spot-meta', { text: metaLine(s) }));
      var inst = h('button.btn.primary.lg', { nav: pre + '-install', html: U.icon('download') + '<span>' + U.escapeHtml(t('saves.install')) + '</span>' });
      inst.addEventListener('click', function () { installSheet(s); });
      var det = h('button.btn.lg', { nav: pre + '-details', html: U.icon('info') + '<span>' + U.escapeHtml(t('saves.details')) + '</span>' });
      det.addEventListener('click', function () { openSave(s); });
      var row = h('div.btn-row', [inst, det]);
      if (opts.more) { var mo = h('button.btn.lg.ghost', { nav: pre + '-more', html: U.icon('grid') + '<span>' + U.escapeHtml(t('saves3.browse')) + '</span>' }); mo.addEventListener('click', opts.more); row.appendChild(mo); }
      body.appendChild(row);
      body.appendChild(h('div.sv-spot-note', [ui.svg('shield'), h('span', { text: t('saves3.autoResign') })]));
      if (!U.reducedMotion()) { body.classList.remove('in'); void body.offsetWidth; body.classList.add('in'); }
    }
    function select(i) {
      if (i === st.idx || !pool[i]) return;
      var s = pool[i];
      var next = st.live === artA ? artB : artA;
      U.clear(next); next.appendChild(spotArt(s));
      st.live.classList.remove('live'); st.live.classList.add('out');
      next.classList.remove('out'); next.classList.add('live');
      var prev = st.live; st.live = next;
      setTimeout(function () { prev.classList.remove('out'); }, 950);
      st.idx = i;
      tiles.forEach(function (b, k) { b.classList.toggle('live', k === i); });
      paintBody(s, i);
      st.elapsed = 0;
      var fill = bar.firstChild; if (fill) { bar.replaceChild(h('i'), fill); }
    }
    function held(now) {
      if (!el.isConnected || document.hidden) return true;
      var scrEl = el.closest('.screen'); if (scrEl && !scrEl.classList.contains('active')) return true;
      if (XS.ui.layers.length || (ca.screensaver && ca.screensaver.active()) || (ca.safeMode && ca.safeMode())) return true;
      return el.contains(document.activeElement) && now - st.lastInput < SPOT_IDLE;
    }
    function tick() {
      if (!el.isConnected) { clearInterval(st.timer); return; }
      var now = Date.now(), dt = now - st.last; st.last = now;
      var hd2 = held(now);
      if (hd2 !== st.held) { st.held = hd2; el.classList.toggle('held', hd2); }
      if (hd2 || pool.length < 2) return;
      st.elapsed += dt;
      if (st.elapsed >= (U.reducedMotion() ? SPOT_PERIOD * 1.5 : SPOT_PERIOD)) select((st.idx + 1) % pool.length);
    }
    artA.appendChild(spotArt(pool[0]));
    st.idx = 0;
    tiles.forEach(function (b, k) { b.classList.toggle('live', k === 0); });
    paintBody(pool[0], 0);
    if (pool.length > 1) st.timer = setInterval(tick, 250);
    el._spot = { select: select, size: pool.length, index: function () { return st.idx; } };
    return el;
  }

  /* Horizontal row for the game tab */
  function rowCard(s, i) {
    var r = h('div.save-row', { data: { save: s.id } });
    var open = h('button.save-open', { nav: 'svr-' + i, data: { hints: 'open' } });
    open.appendChild(coverEl(s));
    var main = h('div.main');
    main.appendChild(h('div.l1', [h('span.name.ellipsis', { text: s.name }), catChip(s.category), compatBadge(s)]));
    if (s.description) main.appendChild(h('div.desc.clamp2', { text: s.description }));
    main.appendChild(h('div.meta', { text: metaLine(s) }));
    open.appendChild(main);
    open.addEventListener('click', function () { openSave(s); });
    r.appendChild(open);
    var acts = h('div.acts');
    var inst = h('button.btn.sm.primary', { nav: 'svi-' + i, html: U.icon('download') + '<span>' + U.escapeHtml(t('saves.install')) + '</span>' });
    inst.addEventListener('click', function () { installSheet(s); });
    var dl = h('button.btn.sm', { nav: 'svd-' + i, html: U.icon('folder') + '<span>' + U.escapeHtml(t('saves.downloadFile')) + '</span>' });
    dl.addEventListener('click', function () { downloadFile(s); });
    var rep = h('button.btn.sm.ghost.icon-only', { nav: 'svp-' + i, html: U.icon('alert'), 'aria-label': t('saves.report'), title: t('saves.report') });
    rep.addEventListener('click', function () { report(s); });
    acts.appendChild(inst); acts.appendChild(dl); acts.appendChild(rep);
    r.appendChild(acts);
    return r;
  }
  function rail(title, list, opts) {
    opts = opts || {};
    if (!list || !list.length) return null;
    var sec = h('section.rail', { data: { rail: opts.id || 'saves' } });
    var head = h('div.section-head', [h('h2.section-title', [title, h('span.count', { text: t('saves.nSaves', { n: list.length }) })])]);
    if (opts.more) { head.appendChild(h('span.spacer')); head.appendChild(h('button.btn.ghost.sm', { nav: 'more:' + (opts.id || 'saves'), text: t('common.seeAll'), on: { click: opts.more } })); }
    sec.appendChild(head);
    var track = h('div.rail-track', { data: { navRegion: '' } });
    list.forEach(function (s) { track.appendChild(card(s, 'rail-' + (opts.id || 'saves'))); });
    sec.appendChild(track);
    return sec;
  }
  function catChips(cur, onPick, prefix) {
    var wrap = h('div.chips.cat-chips', { data: { navRegion: '' } });
    [''].concat(CATS).forEach(function (c) {
      var b = h('button.chip' + (cur === c ? '.on' : ''), { nav: (prefix || 'cat') + '-' + (c || 'all'), text: c ? t('saves.cat.' + c) : t('filter.all'), 'aria-pressed': cur === c ? 'true' : 'false' });
      b.addEventListener('click', function () { onPick(c); });
      wrap.appendChild(b);
    });
    return wrap;
  }

  /* ---------- Game page tab ---------- */
  function renderGameTab(container, g, opts) {
    opts = opts || {};
    U.clear(container);
    var state = { cat: '', showAll: false, list: null, eng: null };
    var head = h('div.sv-head');
    var pill = h('span', { 'data-engine': '' }); head.appendChild(pill);
    head.appendChild(h('span.spacer'));
    var up = h('button.btn', { nav: 'sv-upload-game', html: U.icon('upload') + '<span>' + U.escapeHtml(t('saves.uploadMine')) + '</span>' });
    up.addEventListener('click', function () { uploadWizard({ titleId: (g.titleIds || [])[0] || '', gameId: g.id }); });
    head.appendChild(up);
    container.appendChild(head);
    container.appendChild(garlicPanel({ nav: 'sv-garlic-game' }));
    var chipsHost = h('div'); container.appendChild(chipsHost);
    var listHost = h('div.sv-list', { data: { navRegion: '' } }); container.appendChild(listHost);
    listHost.appendChild(ca.busyRow(t('saves.loading')));
    engine().then(function (e) { state.eng = e; var fresh = enginePill(e); if (pill.parentNode) pill.parentNode.replaceChild(fresh, pill); pill = fresh; });
    function paintChips() { U.clear(chipsHost); chipsHost.appendChild(catChips(state.cat, function (c) { state.cat = c; paintChips(); paintList(); }, 'svcat')); }
    function paintList() {
      U.clear(listHost);
      var all = state.list || [];
      var mine = all.filter(function (s) { return !g.titleIds || !g.titleIds.length || g.titleIds.indexOf(s.titleId) >= 0 || !s.titleId; });
      var others = all.filter(function (s) { return mine.indexOf(s) < 0; });
      var filtered = state.cat ? mine.filter(function (s) { return s.category === state.cat; }) : mine;
      var anyCompat = mine.some(compatible);
      var hidden = 0;
      var shown = filtered.filter(function (s) { if (state.showAll || !anyCompat || compatible(s)) return true; hidden++; return false; });
      if (!all.length) {
        listHost.appendChild(ui.emptyState(t('saves.emptyGameTitle'), t('saves.emptyGameHint'), [h('button.btn.primary', { nav: 'sv-upload-first', html: U.icon('upload') + '<span>' + U.escapeHtml(t('saves.uploadMine')) + '</span>', on: { click: function () { uploadWizard({ titleId: (g.titleIds || [])[0] || '', gameId: g.id }); } } })]));
        return;
      }
      if (!anyCompat && mine.length) listHost.appendChild(h('div.panel.note', [h('div.row', [ui.svg('info'), h('span.dim', { text: mine.some(function (s) { return s.compat.status === 'notInstalled'; }) ? t('saves.noteNotInstalled') : mine.some(function (s) { return s.compat.status === 'update'; }) ? t('saves.noteUpdate') : t('saves.noteUnknown') })])]));
      if (!shown.length && filtered.length) listHost.appendChild(h('div.panel.note', [h('span.dim', { text: t('saves.allHidden', { n: filtered.length }) })]));
      shown.forEach(function (s, i) { listHost.appendChild(rowCard(s, i)); });
      if (hidden > 0 || state.showAll) {
        var tg = h('button.btn.ghost.sm', { nav: 'sv-showall', text: state.showAll ? t('saves.hideIncompatible') : t('saves.showAll', { n: hidden }) });
        tg.addEventListener('click', function () { state.showAll = !state.showAll; paintList(); ca.focusIn(listHost, 'sv-showall'); });
        listHost.appendChild(h('div.sv-foot', [tg]));
      }
      if (others.length) {
        listHost.appendChild(h('div.section-head', [h('h3.section-title', [t('saves.otherRegions'), h('span.count', { text: t('saves.nSaves', { n: others.length }) })])]));
        listHost.appendChild(h('div.panel.note.warn', [h('div.row', [ui.svg('alert'), h('span', { text: t('saves.otherRegionsWarn') })])]));
        others.forEach(function (s, i) { listHost.appendChild(rowCard(s, 100 + i)); });
      }
    }
    fetchGame(g).then(function (list) { state.list = list; if (opts.onCount) opts.onCount(list.length); paintChips(); paintList(); }, function (err) { U.clear(listHost); listHost.appendChild(ui.emptyState(t('saves.loadFailed'), ui.errorText(err), [h('button.btn', { nav: 'sv-retry', text: t('common.retry'), on: { click: function () { renderGameTab(container, g, opts); } } })])); });
  }

  /* ---------- Save detail sheet ---------- */
  function openSave(s) {
    var handle = ui.sheet({
      title: s.name, sub: s.title + (s.titleId ? ' · ' + s.titleId : ''), key: 'save:' + s.id,
      body: function (body) {
        var hero = h('div.cover-hero'); hero.appendChild(coverEl(s));
        hero.appendChild(h('div.grow', [h('div.chips', [catChip(s.category), s.region ? h('span.chip.sm', { text: s.region }) : null, s.gameVersion ? h('span.chip.sm', { text: 'v' + s.gameVersion }) : null, s.sourceName ? h('span.chip.sm.gold', { text: t('saves.fromSite', { name: s.sourceName }) }) : null, s.source === 'garlic' || s.garlic_id ? h('span.chip.sm.acc', { text: t('saves.fromGarlic') }) : null]), h('div.meta', { text: metaLine(s) })]));
        body.appendChild(hero);
        if (s.description) body.appendChild(h('p.sv-desc', { text: s.description }));
        if (s.tags.length) body.appendChild(h('div.chips', s.tags.map(function (x) { return h('span.chip.sm', { text: '#' + x }); })));
        body.appendChild(h('h3', { text: t('saves.compatibility') }));
        var cp = h('div.panel.compat-panel', [h('div.row', [compatBadge(s, true)])]);
        if (s.compat.status === 'update' && s._game) { var ub = h('button.btn.sm', { nav: 'sv-update-game', html: U.icon('refresh') + '<span>' + U.escapeHtml(t('saves.openUpdates')) + '</span>' }); ub.addEventListener('click', function () { ui.closeLayer(handle); if (XS.gamePage) XS.gamePage.openOptions(s._game); }); cp.appendChild(h('div', { style: { marginTop: '10px' } }, [ub])); }
        if (s.compat.status === 'region') cp.appendChild(h('p.dim.small', { text: t('saves.otherRegionsWarn') }));
        body.appendChild(cp);
        body.appendChild(h('h3', { text: t('saves.engineTitle') }));
        var eng = h('div.panel', [h('div.row', [h('span', { 'data-engine': '' })])]);
        body.appendChild(eng);
        engine().then(function (e) { var ph = eng.querySelector('[data-engine]'); if (ph) { ph.parentNode.replaceChild(enginePill(e), ph); eng.appendChild(h('p.dim.small', { text: tierHint(e.tier) })); } });
        var kv = h('dl.kv');
        function add(k, v) { if (!v) return; kv.appendChild(h('dt', { text: k })); kv.appendChild(h('dd', { text: v })); }
        add(t('saves.fileSize'), s.size ? U.fmtBytes(s.size) : '');
        add(t('saves.dirName'), s.dirName || '');
        add(t('saves.sha256'), s.sha256 ? String(s.sha256).slice(0, 16) + '…' : '');
        add(t('saves.format'), s.format || (s.garlic_id ? 'garlic-zip' : ''));
        body.appendChild(h('h3', { text: t('saves.details') }));
        body.appendChild(h('div.panel', [kv]));
        if (s._game) { var gb = h('button.lrow', { nav: 'sv-game', data: { hints: 'open' } }); gb.appendChild(ui.cover(s._game.cover, s._game.coverFallback, s._game.title)); gb.appendChild(h('div.main', [h('div.name', { text: s._game.title }), h('div.desc', { text: t('saves.openGame') })])); gb.appendChild(h('span.val', { html: U.icon('chevR') })); gb.addEventListener('click', function () { ui.closeLayer(handle); XS.router.go('game', { id: s._game.id }); }); body.appendChild(h('div', { style: { marginTop: '12px' } }, [gb])); }
      },
      foot: [
        h('button.btn.ghost', { nav: 'sv-report', html: U.icon('alert') + '<span>' + U.escapeHtml(t('saves.report')) + '</span>', on: { click: function () { report(s); } } }),
        h('button.btn', { nav: 'sv-download', html: U.icon('folder') + '<span>' + U.escapeHtml(t('saves.downloadFile')) + '</span>', on: { click: function () { downloadFile(s); } } }),
        h('span.spacer'),
        h('button.btn.primary.lg', { nav: 'sv-install', 'data-autofocus': '', html: U.icon('download') + '<span>' + U.escapeHtml(t('saves.install')) + '</span>', on: { click: function () { installSheet(s); } } })
      ]
    });
    return handle;
  }

  /* ---------- Install to my saves (§17.22.4) ---------- */
  function installSheet(s) {
    var st = { user: null, backupFirst: !(S.data.settings && S.data.settings.saves && S.data.settings.saves.autoBackup === false) };
    var handle = ui.sheet({
      title: t('saves.installTitle'), sub: s.name, key: 'svinstall:' + s.id, cls: 'narrow',
      body: function (body) {
        body.appendChild(h('div.panel.compat-panel', [h('div.row', [compatBadge(s, true)])]));
        if (s.compat.status === 'notInstalled') body.appendChild(h('div.panel.note.warn', [h('div.row', [ui.svg('alert'), h('span', { text: t('saves.installNotInstalled') })])]));
        else if (s.compat.status === 'update') body.appendChild(h('div.panel.note.warn', [h('div.row', [ui.svg('alert'), h('span', { text: t('saves.installUpdateFirst') })])]));
        else if (s.compat.status === 'region') body.appendChild(h('div.panel.note.warn', [h('div.row', [ui.svg('alert'), h('span', { text: t('saves.otherRegionsWarn') })])]));
        body.appendChild(h('h3', { text: t('saves.profile') }));
        var ul = h('div.list', { 'data-users': '' });
        ul.appendChild(ca.busyRow(t('saves.loadingUsers')));
        body.appendChild(ul);
        users().then(function (list) {
          U.clear(ul);
          if (!list.length) { ul.appendChild(h('div.panel', [h('span.dim', { text: t('saves.noUsers') })])); return; }
          list.forEach(function (u, i) {
            if (!st.user && (u.foreground || i === 0)) st.user = u;
            var sel = st.user === u;
            var row = h('button.lrow' + (sel ? '.sel' : ''), { nav: 'svu-' + i, data: { hints: 'select' } });
            row.appendChild(h('span.ic', { html: U.icon('gamepad') }));
            row.appendChild(h('div.main', [h('div.name', { text: u.name || String(u.id) }), h('div.desc', { text: (u.foreground ? t('saves.foregroundUser') + ' · ' : '') + 'id ' + u.id })]));
            row.appendChild(h('span.check', { html: sel ? U.icon('check') : '' }));
            row.addEventListener('click', function () { st.user = u; U.$$('.lrow', ul).forEach(function (x) { x.classList.remove('sel'); x.querySelector('.check').innerHTML = ''; }); row.classList.add('sel'); row.querySelector('.check').innerHTML = U.icon('check'); });
            ul.appendChild(row);
          });
        }, function (err) { U.clear(ul); ul.appendChild(h('div.panel', [h('span.warn', { text: ui.errorText(err) })])); });
        body.appendChild(h('h3', { text: t('saves.options') }));
        body.appendChild(ui.toggleRow({ id: 'sv-backup', icon: 'shield', label: t('saves.backupFirst'), desc: t('saves.backupFirstHint'), value: st.backupFirst, onChange: function (v) { st.backupFirst = v; } }));
        var eng = h('div.panel', { style: { marginTop: '12px' } }, [h('div.row', [h('span', { 'data-engine': '' })])]);
        body.appendChild(eng);
        engine().then(function (e) { var ph = eng.querySelector('[data-engine]'); if (ph) { ph.parentNode.replaceChild(enginePill(e), ph); eng.appendChild(h('p.dim.small', { text: tierHint(e.tier) })); } });
        body.appendChild(garlicPanel({ nav: 'sv-install-garlic' }));
        body.appendChild(h('p.dim.small', { text: t('saves.installSteps') }));
      },
      foot: [
        h('button.btn.ghost', { nav: 'sv-cancel', text: t('common.cancel'), on: { click: function () { ui.closeLayer(handle); } } }),
        h('span.spacer'),
        h('button.btn.primary.lg', { nav: 'sv-install-go', 'data-autofocus': '', 'data-go': '', html: U.icon('download') + '<span>' + U.escapeHtml(t('saves.installGo')) + '</span>', on: { click: go } })
      ]
    });
    function go() {
      var btn = handle.foot.querySelector('[data-go]');
      if (!st.user) { ui.toast({ cls: 'bad', title: t('saves.pickProfile') }); return; }
      btn.classList.add('busy');
      ca.api.savesInstall(s.id, { user: st.user.id, backupFirst: st.backupFirst }).then(function (r) {
        btn.classList.remove('busy');
        XS.actions.refreshQueue();
        if (r.job && r.job.id) { watch[r.job.id] = { save: s, kind: 'install' }; watchConsole(r.job, s, 'install'); }
        ui.closeLayer(handle);
        ui.toast({ cls: 'gold', title: t('saves.installQueued'), desc: s.name, action: { label: t('game.viewDownloads'), onClick: function () { XS.router.go('downloads'); } } });
      }, function (err) { btn.classList.remove('busy'); ui.errorToast(err, t('saves.install')); });
    }
  }
  var watch = {};
  function doneToast(kind, path, note) {
    XS.sound.done();
    ui.toast({ cls: 'good', title: kind === 'install' ? t('saves.installedToast') : t('saves.downloadedToast'), desc: (kind === 'install' ? t('saves.installedToastSub') : (path || '')) + (note ? ' — ' + note : ''), timeout: note ? 10000 : 7000 });
  }
  function failedToast(j, s, kind) {
    var needs = j.errorCode === 'garlic_unavailable';
    ui.toast({ cls: 'bad', title: needs ? t('saves.garlic.neededShort') : kind === 'install' ? t('saves.installFailed') : t('saves.downloadFailed'), desc: j.error || s.name, timeout: 10000,
      action: needs ? { label: t('saves.garlic.start'), onClick: function () { startGarlic(); } } : undefined });
  }
  /* Save installs/downloads run as console jobs (/api/saves/jobs/{id}: a result object, no filename); the queue
     watcher below covers hosts that report them as download-queue jobs instead. Whichever settles first reports. */
  function watchConsole(job, s, kind) {
    var id = job.id;
    if (!(XS.cb && XS.cb.watchJob) || job.filename !== undefined || job.result === undefined) return;
    XS.cb.watchJob(id, null, { every: 1500, maxPolls: 2400 }).then(function (j) {
      if (!watch[id]) return;
      delete watch[id];
      if (j.state === 'done') doneToast(kind, j.result && j.result.zipPath, j.result && j.result.note);
      else if (j.state === 'failed') failedToast(j, s, kind);
    }, function () { /* not a console job */ });
  }
  S.on('queue', function () {
    var ids = Object.keys(watch); if (!ids.length) return;
    S.data.queue.jobs.forEach(function (j) {
      var w = watch[j.id]; if (!w) return;
      if (j.state === 'done' || j.state === 'installed') { delete watch[j.id]; doneToast(w.kind, j.path); }
      else if (j.state === 'failed' || j.state === 'cancelled') { delete watch[j.id]; if (j.state === 'failed') failedToast(j, w.save, w.kind); }
    });
  });
  function downloadFile(s) {
    ca.api.savesDownload(s.id, {}).then(function (r) {
      XS.actions.refreshQueue();
      if (r.job && r.job.id) { watch[r.job.id] = { save: s, kind: 'download' }; watchConsole(r.job, s, 'download'); }
      ui.toast({ cls: 'gold', title: t('saves.downloadQueued'), desc: r.job && r.job.path ? r.job.path : s.name, action: { label: t('game.viewDownloads'), onClick: function () { XS.router.go('downloads'); } } });
    }, function (err) { ui.errorToast(err, t('saves.downloadFile')); });
  }
  function report(s) { ca.linkDialog({ title: t('saves.report'), hint: t('saves.reportHint'), url: ca.issueUrl('save', { save: s }), display: t('game2.issueOnGithub'), openLabel: t('game2.openIssue') }); }

  /* ---------- Community Saves screen ---------- */
  var buildingRetries = 0;
  function render() {
    if (!root) return;
    U.clear(root);
    var inner = h('div.screen-inner.saves-screen');
    root.appendChild(inner);
    var head = h('div.page-head');
    head.appendChild(h('div.grow', [h('div.kicker', { text: t('saves.kicker') }), h('h1', { text: t('saves.title') }), h('p.dim', { text: t('saves.subtitle') })]));
    var pill = h('span', { 'data-engine': '' }); head.appendChild(pill);
    engine().then(function (e) { var fresh = enginePill(e); if (pill.parentNode) pill.parentNode.replaceChild(fresh, pill); });
    var mine = h('button.btn.lg', { nav: 'sv-mine', html: U.icon('library') + '<span>' + U.escapeHtml(t('saves.mySaves')) + '</span>' });
    mine.addEventListener('click', function () { XS.router.go('library', { section: 'saves' }); });
    head.appendChild(mine);
    var up = h('button.btn.primary.lg', { nav: 'sv-upload', 'data-autofocus': '', html: U.icon('upload') + '<span>' + U.escapeHtml(t('saves.uploadMine')) + '</span>' });
    up.addEventListener('click', function () { uploadWizard({}); });
    head.appendChild(up);
    inner.appendChild(head);
    inner.appendChild(garlicPanel({ nav: 'sv-garlic' }));
    var bar = h('div.browse-bar', { data: { navRegion: '' } });
    var sb = ui.searchBox({ id: 'sv-search', placeholder: t('saves.searchPlaceholder'), value: scr.q, onInput: function (v) { scr.q = v; live(); } });
    bar.appendChild(sb);
    var sort = h('button.btn', { nav: 'sv-sort', html: U.icon('sort') + '<span>' + U.escapeHtml(t('saves.sort.' + scr.sort)) + '</span>' });
    sort.addEventListener('click', function () { ui.menu(sort, SORTS.map(function (s2) { return { label: t('saves.sort.' + s2), checked: scr.sort === s2, onSelect: function () { scr.sort = s2; render(); ca.focusIn(root, 'sv-sort'); } }; }), { title: t('browse.sortBy') }); });
    bar.appendChild(sort);
    var comp = h('button.chip.quick' + (scr.compatOnly ? '.on' : ''), { nav: 'sv-compat', text: t('saves.compatibleOnly'), 'aria-pressed': scr.compatOnly ? 'true' : 'false' });
    comp.addEventListener('click', function () { scr.compatOnly = !scr.compatOnly; render(); ca.focusIn(root, 'sv-compat'); });
    bar.appendChild(comp);
    var rf = h('button.iconbtn', { nav: 'sv-refresh', html: U.icon('refresh'), 'aria-label': t('common.refresh'), title: t('common.refresh') });
    rf.addEventListener('click', function () { cache.all = null; cache.byGame = {}; render(); });
    bar.appendChild(rf);
    inner.appendChild(bar);
    inner.appendChild(catChips(scr.cat, function (c) { scr.cat = c; render(); ca.focusIn(root, 'cat-' + (c || 'all')); }));
    var newestHost = h('div'); inner.appendChild(newestHost);
    var meta = h('div.browse-meta'); inner.appendChild(meta);
    var gridHost = h('div', { data: { navRegion: '' } }); inner.appendChild(gridHost);
    gridHost.appendChild(ca.busyRow(t('saves.loading')));
    var live = U.debounce(function () { paintGrid(); }, 120);
    function paintGrid() {
      var all = cache.all || [];
      var q = scr.q.trim().toLowerCase();
      var list = all.filter(function (s) {
        if (scr.cat && s.category !== scr.cat) return false;
        if (scr.compatOnly && !compatible(s)) return false;
        if (q && ((s.title || '') + ' ' + (s.name || '') + ' ' + (s.titleId || '') + ' ' + (s.author || '') + ' ' + s.tags.join(' ')).toLowerCase().indexOf(q) < 0) return false;
        return true;
      });
      list.sort(scr.sort === 'popular' ? function (a, b) { return b.downloads - a.downloads || cmpDate(a, b); } : scr.sort === 'version' ? function (a, b) { return ca.semverCmp(b.gameVersion, a.gameVersion) || cmpDate(a, b); } : cmpDate);
      U.clear(meta); meta.appendChild(h('span', { text: t('saves.nSaves', { n: list.length }) }));
      U.clear(gridHost);
      if (!list.length) { gridHost.appendChild(ui.emptyState(q ? t('browse.emptyQueryTitle', { q: scr.q }) : t('saves.emptyTitle'), t('saves.emptyHint'), [h('button.btn.primary', { nav: 'sv-upload2', html: U.icon('upload') + '<span>' + U.escapeHtml(t('saves.uploadMine')) + '</span>', on: { click: function () { uploadWizard({}); } } })])); return; }
      var grid = h('div.grid.saves-grid');
      list.slice(0, 200).forEach(function (s) { grid.appendChild(card(s, 'grid')); });
      gridHost.appendChild(grid);
    }
    function cmpDate(a, b) { return (b.createdAt || '') < (a.createdAt || '') ? -1 : (b.createdAt || '') > (a.createdAt || '') ? 1 : 0; }
    fetchAll().then(function () {
      if (cache.building && !cache.all.length && buildingRetries < 8) {
        /* first start: the console is still fetching the community index */
        buildingRetries++;
        U.clear(gridHost); gridHost.appendChild(ca.busyRow(t('saves.building')));
        setTimeout(function () { if (root && root.isConnected && gridHost.isConnected) render(); }, 4000);
        return;
      }
      buildingRetries = 0;
      if (!scr.q && !scr.cat && !scr.compatOnly) {
        var sp = spotlight(cache.all, { id: 'sv-spot' }); if (sp) newestHost.appendChild(sp);
        var nw = cache.all.slice().sort(cmpDate).slice(0, 12); var sec = rail(t('discover2.newSaves'), nw, { id: 'newest' }); if (sec) newestHost.appendChild(sec);
        var pop = cache.all.filter(function (x) { return x.downloads > 0; }).sort(function (a, b) { return b.downloads - a.downloads; }).slice(0, 12);
        var sec2 = pop.length >= 3 ? rail(t('saves.sort.popular'), pop, { id: 'popular' }) : null; if (sec2) newestHost.appendChild(sec2);
        var works = cache.all.filter(compatible).sort(cmpDate).slice(0, 12);
        var sec3 = works.length ? rail(t('saves3.worksHere'), works, { id: 'works' }) : null; if (sec3) newestHost.appendChild(sec3);
        newestHost.appendChild(h('h2.sv-all-head', { text: t('saves3.allSaves') }));
      }
      paintGrid();
    }, function (err) { U.clear(gridHost); gridHost.appendChild(ui.emptyState(t('saves.loadFailed'), ui.errorText(err), [h('button.btn', { nav: 'sv-retry', text: t('common.retry'), on: { click: render } })])); });
  }

  /* ---------- Upload my save wizard (§17.22.5) ---------- */
  function uploadWizard(prefill) {
    prefill = prefill || {};
    var sv = (S.data.settings && S.data.settings.saves) || {};
    var st = { step: 0, user: null, users: [], titles: null, title: null, dir: null, exp: null, host: '', hostPicked: false, site: cache.site, up: null, sub: null, eng: null,
      meta: { category: '', name: '', description: '', tags: '', nickname: sv.nickname || '' }, busy: false, q: '' };
    /* default host: the save site whenever its backend is on, else the setting (pixeldrain when it says "site") */
    function defaultHost() { if (st.site && st.site.on) return 'site'; var x = sv.uploadHost; return x && x !== 'site' ? x : 'pixeldrain'; }
    st.host = defaultHost();
    siteInfo().then(function (si) { st.site = si; if (!st.hostPicked) st.host = defaultHost(); if (STEPS[st.step] === 'upload' && !st.up && !st.busy) paint(); });
    var STEPS = ['profile', 'title', 'dir', 'export', 'upload', 'meta', 'done'];
    var hd = ui.sheet({ title: t('saves.wizTitle'), key: 'svwiz', cls: 'wide', body: function () { /* painted below */ } });
    engine().then(function (e) { st.eng = e; if (st.step === 3) paint(); });
    function stepBar() { var s = h('div.steps'); STEPS.forEach(function (_, i) { s.appendChild(h('i' + (i <= st.step ? '.on' : ''))); }); return s; }
    function foot(backFn, nextLabel, nextFn, nextDisabled) {
      U.clear(hd.foot);
      if (backFn) hd.foot.appendChild(h('button.btn.ghost', { nav: 'wz-back', text: t('common.back'), on: { click: backFn } }));
      hd.foot.appendChild(h('span.dim.wz-step', { text: t('saves.wizStep', { i: st.step + 1, n: STEPS.length, name: t('saves.wizSteps.' + STEPS[st.step]) }) }));
      hd.foot.appendChild(h('span.spacer'));
      if (nextFn) { var nb = h('button.btn.primary.lg', { nav: 'wz-next', 'data-next': '', text: nextLabel, on: { click: nextFn } }); if (nextDisabled) nb.setAttribute('disabled', ''); hd.foot.appendChild(nb); }
    }
    function setNextEnabled(on) { var nb = hd.foot.querySelector('[data-next]'); if (nb) nb.toggleAttribute('disabled', !on); }
    function selectRow(list, nav, icon, name, desc, selected, onPick, coverSrc) {
      var row = h('button.lrow' + (selected ? '.sel' : ''), { nav: nav, data: { hints: 'select' } });
      if (coverSrc !== undefined) row.appendChild(ui.cover(coverSrc.src, coverSrc.fallback, name)); else row.appendChild(h('span.ic', { html: U.icon(icon) }));
      row.appendChild(h('div.main', [h('div.name.ellipsis', { text: name }), desc ? h('div.desc', { text: desc }) : null]));
      row.appendChild(h('span.check', { html: selected ? U.icon('check') : '' }));
      row.addEventListener('click', function () { U.$$('.lrow', list).forEach(function (x) { x.classList.remove('sel'); x.querySelector('.check').innerHTML = ''; }); row.classList.add('sel'); row.querySelector('.check').innerHTML = U.icon('check'); onPick(); });
      return row;
    }
    function paint() {
      var body = hd.body; U.clear(body); U.clear(hd.foot);
      body.appendChild(stepBar());
      var s = STEPS[st.step];
      if (s === 'profile') {
        body.appendChild(h('div.wiz-hero', [h('span.ic.okring.gold', { html: U.icon('upload').replace('<svg ', '<svg style="width:30px;height:30px;fill:none;stroke:currentColor;stroke-width:2.2" ') }), h('div', [h('h2', { text: t('saves.wizWelcome') }), h('p', { text: t('saves.wizWelcomeText') })])]));
        body.appendChild(h('h3', { text: t('saves.profile') }));
        var ul = h('div.list'); ul.appendChild(ca.busyRow(t('saves.loadingUsers'))); body.appendChild(ul);
        body.appendChild(h('p.dim.small', { text: t('saves.privacyNote') }));
        foot(null, t('common.next'), function () { if (st.user) { st.step = 1; st.titles = null; paint(); } }, !st.user);
        users().then(function (list) {
          U.clear(ul); st.users = list;
          if (!list.length) { ul.appendChild(h('div.panel', [h('span.dim', { text: t('saves.noUsers') })])); return; }
          list.forEach(function (u, i) { if (!st.user && (u.foreground || i === 0)) st.user = u; ul.appendChild(selectRow(ul, 'wz-user-' + i, 'gamepad', u.name || String(u.id), (u.foreground ? t('saves.foregroundUser') + ' · ' : '') + 'id ' + u.id, st.user === u, function () { st.user = u; setNextEnabled(true); })); });
          setNextEnabled(!!st.user);
          requestAnimationFrame(function () { XS.nav.focusFirst(hd.el); });
        }, function (err) { U.clear(ul); ul.appendChild(h('div.panel', [h('span.warn', { text: ui.errorText(err) })])); });
      } else if (s === 'title') {
        body.appendChild(h('h3', { text: t('saves.wizPickTitle') }));
        var sb = ui.searchBox({ id: 'wz-title-search', placeholder: t('saves.wizTitleSearch'), value: st.q, onInput: function (v) { st.q = v; paintTitles(); } });
        body.appendChild(sb);
        var tl = h('div.list', { style: { marginTop: '10px' } }); tl.appendChild(ca.busyRow(t('saves.loading'))); body.appendChild(tl);
        foot(function () { st.step = 0; paint(); }, t('common.next'), function () { if (st.title) { st.step = 2; paint(); } }, !st.title);
        function paintTitles() {
          U.clear(tl);
          var q = st.q.trim().toLowerCase();
          var list = (st.titles || []).filter(function (x) { return x.kind !== 'ps4' && (!q || ((x.title || '') + ' ' + x.titleId).toLowerCase().indexOf(q) >= 0); });
          if (!list.length) { tl.appendChild(h('div.panel', [h('span.dim', { text: st.titles && st.titles.length ? t('browse.emptyQueryTitle', { q: st.q }) : t('saves.wizNoSaves') })])); return; }
          list.forEach(function (x, i) {
            var g = S.data.byTitleId[x.titleId] || (x.gameId ? S.game(x.gameId) : null);
            var name = x.title || (g ? g.title : x.titleId);
            tl.appendChild(selectRow(tl, 'wz-title-' + i, 'box', name, x.titleId + (x.size ? ' · ' + U.fmtBytes(x.size) : '') + (x.mtime ? ' · ' + ca.fmtWhen(x.mtime) : ''), st.title === x, function () { st.title = x; st.dir = null; setNextEnabled(true); }, { src: x.cover || (g ? g.cover : ''), fallback: g ? g.coverFallback : '' }));
          });
        }
        if (st.titles) paintTitles();
        else ca.api.savesList(st.user ? st.user.id : '').then(function (r) {
          st.titles = (r.saves || []).map(function (x) { var g = S.data.byTitleId[x.titleId] || (x.gameId ? S.game(x.gameId) : null); if (g) { x.title = x.title || g.title; x.cover = x.cover || g.cover; x.gameId = x.gameId || g.id; x.region = x.region || g.region; } return x; });
          if (prefill.titleId && !st.title) { st.title = st.titles.filter(function (x) { return x.titleId === prefill.titleId; })[0] || null; }
          paintTitles(); setNextEnabled(!!st.title);
          if (st.title) { var sel = tl.querySelector('.lrow.sel'); if (sel) XS.nav.focus(sel, false); }
        }, function (err) { U.clear(tl); tl.appendChild(h('div.panel', [h('span.warn', { text: ui.errorText(err) })])); });
      } else if (s === 'dir') {
        var x = st.title, g = S.data.byTitleId[x.titleId] || (x.gameId ? S.game(x.gameId) : null);
        var hero = h('div.cover-hero'); hero.appendChild(ui.cover(x.cover || (g ? g.cover : ''), g ? g.coverFallback : '', x.title || x.titleId));
        var auto = h('div.chips');
        auto.appendChild(h('span.chip.sm.mono', { text: x.titleId }));
        if (x.region || (g && g.region)) auto.appendChild(h('span.chip.sm', { text: x.region || g.region }));
        var verChip = h('span.chip.sm', { 'data-ver': '', text: x.version ? 'v' + x.version : t('saves.wizVersionLoading') }); auto.appendChild(verChip);
        hero.appendChild(h('div.grow', [h('h4', { text: x.title || x.titleId }), auto, h('div.meta', { text: t('saves.wizAutoFilled') })]));
        body.appendChild(hero);
        if (!x.version) ca.api.savesInstalledVersion(x.titleId).then(function (r) { if (r.version) { x.version = r.version; verChip.textContent = 'v' + r.version; } else verChip.textContent = t('saves.wizVersionUnknown'); if (r.region && !x.region) x.region = r.region; }, function () { verChip.textContent = t('saves.wizVersionUnknown'); });
        body.appendChild(h('h3', { text: t('saves.wizPickDir') }));
        var dl = h('div.list'); body.appendChild(dl);
        /* the console lists folder names in "dirs" and their image size/time in "dirInfo" */
        var dirs = (Array.isArray(x.dirInfo) && x.dirInfo.length ? x.dirInfo : Array.isArray(x.dirs) ? x.dirs : [])
          .map(function (d) { return typeof d === 'string' ? { name: d } : d; }).filter(function (d) { return d && d.name; });
        if (!dirs.length) dirs = [{ name: x.dirName || 'SAVEDATA00', size: x.size, mtime: x.mtime }];
        dirs.forEach(function (d, i) {
          if (st.dir && st.dir.name === d.name) st.dir = d;
          if (!st.dir && i === 0) st.dir = d;
          dl.appendChild(selectRow(dl, 'wz-dir-' + i, 'folder', d.name, [d.size ? U.fmtBytes(d.size) : '', d.mtime ? ca.fmtWhen(d.mtime) : '', d.title || ''].filter(Boolean).join(' · '), st.dir === d, function () { st.dir = d; setNextEnabled(true); }));
        });
        body.appendChild(h('p.dim.small', { text: t('saves.wizDirHint') }));
        foot(function () { st.step = 1; paint(); }, t('saves.wizExport'), function () { st.step = 3; st.exp = null; paint(); }, !st.dir);
      } else if (s === 'export') {
        body.appendChild(h('h3', { text: t('saves.wizSteps.export') }));
        var ep = h('div.panel'); body.appendChild(ep);
        var eng = h('div.row', [st.eng ? enginePill(st.eng) : h('span.dim', { text: t('saves.loading') })]); body.appendChild(h('div.panel', { style: { marginTop: '12px' } }, [eng, h('p.dim.small', { text: st.eng ? t('saves.wizExportEngine', { tier: tierText(st.eng.tier) }) + ' ' + tierHint(st.eng.tier) : '' })]));
        foot(function () { st.step = 2; paint(); }, t('common.next'), function () { if (shareable()) { st.step = 4; paint(); } }, !shareable());
        if (st.exp) paintExport(ep);
        else {
          ep.appendChild(h('div.row', [h('span.skel', { style: { width: '18px', height: '18px', borderRadius: '50%', flex: '0 0 auto' } }), h('span.dim', { text: t('saves.wizExporting', { dir: st.dir.name }) })]));
          ca.api.savesExport({ user: st.user ? st.user.id : '', titleId: st.title.titleId, dirName: st.dir.name }).then(function (r) {
            st.exp = r; paintExport(ep); setNextEnabled(shareable());
            var m = r.manifest || {}; if (m.gameVersion && !st.title.version) st.title.version = m.gameVersion; if (m.region && !st.title.region) st.title.region = m.region;
            ca.focusIn(hd.el, shareable() ? 'wz-next' : 'wz-back');
          }, function (err) { U.clear(ep); ep.appendChild(h('div.row', [ui.svg('alert'), h('span.warn', { text: ui.errorText(err) })])); ep.appendChild(h('div', { style: { marginTop: '10px' } }, [h('button.btn.sm', { nav: 'wz-retry', text: t('common.retry'), on: { click: function () { st.exp = null; paint(); } } })])); });
        }
        function paintExport(box) {
          U.clear(box);
          var r = st.exp, m = r.manifest || {};
          if (r.shareable === false) { paintNotShareable(box, r); return; }
          var kv = h('dl.kv');
          function add(k, v) { if (!v) return; kv.appendChild(h('dt', { text: k })); kv.appendChild(h('dd', { text: v })); }
          add(t('saves.wizZip'), r.zipPath || '');
          add(t('saves.fileSize'), r.size ? U.fmtBytes(r.size) : '');
          add(t('saves.wizFiles'), m.files ? t('saves.nFiles', { n: m.files.length }) : '');
          add(t('saves.sha256'), r.sha256 ? String(r.sha256).slice(0, 24) + '…' : '');
          add(t('saves.gameVersion'), m.gameVersion || st.title.version || '');
          add(t('saves.region'), m.region || st.title.region || '');
          add(t('saves.engineTitle'), r.tier ? tierText(r.tier) : '');
          box.appendChild(h('div.row', [ui.svg('check'), h('b', { text: t('saves.wizExported') })]));
          box.appendChild(kv);
          /* only claim what the console reported: the account id was zeroed, or there was none to remove */
          var chips = [];
          var good = function (text) { return h('span.chip.sm.good', { html: U.icon('check') + '<span>' + U.escapeHtml(text) + '</span>' }); };
          if (r.accountId === 'none') chips.push(good(t('saves.wizNoAccountId')));
          else if (r.accountIdStripped === true || (r.accountIdStripped === undefined && m.accountIdStripped === true)) chips.push(good(t('saves.wizAccountStripped')));
          chips.push(good(t('saves.wizNoSealedKeys')));
          box.appendChild(h('div.chips', { style: { marginTop: '10px' } }, chips));
        }
        /* raw tier (no native engine, no Garlic) or an account id that could not be removed: stop here */
        function paintNotShareable(box, r) {
          box.appendChild(h('div.row', [ui.svg('alert'), h('b', { text: t('saves.wizNotShareable') })]));
          var why = r.reason === 'account_id' ? t('saves.wizAccountIdKept') : r.reason === 'no_save' ? t('saves.wizNoSaveFolder') : t('saves.wizNeedsGarlic');
          box.appendChild(h('p', { text: why }));
          if (r.detail) box.appendChild(h('p.dim.small', { text: r.detail }));
          if (r.reason !== 'account_id' && r.reason !== 'no_save') box.appendChild(garlicPanel({ always: true, nav: 'wz-garlic', onStarted: function () { st.exp = null; paint(); } }));
          box.appendChild(h('div', { style: { marginTop: '10px' } }, [h('button.btn.sm', { nav: 'wz-retry', text: t('common.retry'), on: { click: function () { st.exp = null; paint(); } } })]));
        }
      } else if (s === 'upload') {
        body.appendChild(h('h3', { text: t('saves.wizHost') }));
        var hosts = [{ id: 'pixeldrain', label: 'pixeldrain', sub: t('saves.hostPixeldrain') }, { id: 'catbox', label: 'catbox', sub: t('saves.hostCatbox') }];
        if (st.site && st.site.on) hosts.unshift({ id: 'site', label: st.site.name, sub: t('saves.hostSite', { name: st.site.name }) });
        if (sv.uploadEndpoint) hosts.push({ id: 'custom', label: t('saves.hostCustom'), sub: sv.uploadEndpoint });
        if (!hosts.some(function (x) { return x.id === st.host; })) st.host = hosts[0].id;
        body.appendChild(ui.segControl({ id: 'wz-host', value: st.host, options: hosts.map(function (x) { return { value: x.id, label: x.label }; }), onChange: function (v) { st.host = v; st.hostPicked = true; } }));
        body.appendChild(h('p.dim.small', { text: hosts.map(function (x) { return x.label + ': ' + x.sub; }).join(' · ') }));
        var up = h('div.panel', { style: { marginTop: '14px' } }); body.appendChild(up);
        if (st.up) paintUploaded(up);
        else up.appendChild(h('div.row', [ui.svg('upload'), h('span.dim', { text: t('saves.wizUploadReady', { size: st.exp && st.exp.size ? U.fmtBytes(st.exp.size) : '' }) })]));
        foot(function () { st.step = 3; paint(); }, st.up ? t('common.next') : t('saves.wizUploadGo'), function () {
          if (st.up) { st.step = 5; paint(); return; }
          if (st.busy) return; st.busy = true;
          U.clear(up);
          var bar = h('div.bar.gold.striped', [h('i', { style: { width: '100%' } })]);
          var hostName = (hosts.filter(function (x) { return x.id === st.host; })[0] || { label: st.host }).label;
          up.appendChild(h('div.row', [h('span.dim', { text: t('saves.wizUploading', { size: st.exp && st.exp.size ? U.fmtBytes(st.exp.size) : '', host: hostName }) })]));
          up.appendChild(h('div', { style: { marginTop: '10px' } }, [bar]));
          up.appendChild(h('p.dim.small', { text: t('saves.wizUploadNote') }));
          setNextEnabled(false);
          ca.api.savesUpload({ zipPath: st.exp.zipPath, host: st.host }).then(function (r) {
            st.busy = false; st.up = r; paintUploaded(up);
            U.clear(hd.foot); foot(function () { st.step = 3; paint(); }, t('common.next'), function () { st.step = 5; paint(); });
            ca.focusIn(hd.el, 'wz-next');
          }, function (err) { st.busy = false; U.clear(up); up.appendChild(h('div.row', [ui.svg('alert'), h('span.warn', { text: ui.errorText(err) })])); setNextEnabled(true); });
        });
        function paintUploaded(box) { U.clear(box); box.appendChild(h('div.row', [ui.svg('check'), h('b', { text: t('saves.wizUploaded') })])); var kv = h('dl.kv'); kv.appendChild(h('dt', { text: 'URL' })); kv.appendChild(h('dd.mono', { text: st.up.url || '' })); if (st.up.size) { kv.appendChild(h('dt', { text: t('saves.fileSize') })); kv.appendChild(h('dd', { text: U.fmtBytes(st.up.size) })); } if (st.up.sha256) { kv.appendChild(h('dt', { text: t('saves.sha256') })); kv.appendChild(h('dd.mono', { text: String(st.up.sha256).slice(0, 24) + '…' })); } box.appendChild(kv); }
      } else if (s === 'meta') {
        var m = st.meta;
        body.appendChild(h('h3', { text: t('saves.wizCategory') }));
        body.appendChild(catChipsPick());
        var f1 = h('div.field', [h('label', { text: t('saves.wizName') }), h('input.input', { nav: 'wz-name', type: 'text', maxlength: '80', value: m.name, placeholder: t('saves.wizNamePlaceholder'), on: { input: function (e) { m.name = e.target.value; validate(); } } })]);
        var counter = h('span.counter', { text: m.description.length + ' / 300' });
        var f2 = h('div.field', [h('label', [t('saves.wizDescription'), ' ', counter]), h('textarea.input.desc', { nav: 'wz-desc', maxlength: '300', placeholder: t('saves.wizDescriptionPlaceholder'), on: { input: function (e) { m.description = e.target.value.slice(0, 300); counter.textContent = m.description.length + ' / 300'; validate(); } } })]);
        f2.querySelector('textarea').value = m.description;
        var f3 = h('div.field', [h('label', { text: t('saves.wizTags') }), h('input.input', { nav: 'wz-tags', type: 'text', value: m.tags, placeholder: 'ng+, max money, chapter 4', on: { input: function (e) { m.tags = e.target.value; } } })]);
        var f4 = h('div.field', [h('label', { text: t('saves.wizNickname') }), h('input.input', { nav: 'wz-nick', type: 'text', maxlength: '32', value: m.nickname, placeholder: t('saves.wizNicknamePlaceholder'), on: { input: function (e) { m.nickname = e.target.value; validate(); } } })]);
        body.appendChild(h('div.wz-form', [f1, f2, f3, f4]));
        var autoKv = h('dl.kv');
        [[t('game.titleId'), st.title.titleId], [t('saves.gameVersion'), st.title.version || (st.exp && st.exp.manifest && st.exp.manifest.gameVersion) || t('saves.wizVersionUnknown')], [t('saves.region'), st.title.region || ''], [t('saves.fileSize'), st.up && st.up.size ? U.fmtBytes(st.up.size) : ''], [t('saves.sha256'), st.up && st.up.sha256 ? String(st.up.sha256).slice(0, 16) + '…' : '']].forEach(function (kv) { if (!kv[1]) return; autoKv.appendChild(h('dt', { text: kv[0] })); autoKv.appendChild(h('dd', { text: kv[1] })); });
        body.appendChild(h('h3', { text: t('saves.wizAutoFilled') }));
        body.appendChild(h('div.panel', [autoKv]));
        body.appendChild(h('p.dim.small', { text: t('saves.privacyNote') }));
        foot(function () { st.step = 4; paint(); }, t('saves.wizSubmit'), submit, !valid());
        function validate() { setNextEnabled(valid()); }
        function valid() { return !!(m.category && m.name.trim() && m.nickname.trim()); }
        function catChipsPick() {
          var wrap = h('div.chips', { data: { navRegion: '' } });
          CATS.forEach(function (c) { var b = h('button.chip' + (m.category === c ? '.on' : ''), { nav: 'wz-cat-' + c, text: t('saves.cat.' + c), role: 'radio', 'aria-checked': m.category === c ? 'true' : 'false' }); b.addEventListener('click', function () { m.category = c; U.$$('.chip', wrap).forEach(function (x) { x.classList.toggle('on', x === b); x.setAttribute('aria-checked', x === b ? 'true' : 'false'); }); validate(); }); wrap.appendChild(b); });
          return wrap;
        }
        function submit() {
          if (!valid() || st.busy) return; st.busy = true;
          var nb = hd.foot.querySelector('[data-next]'); if (nb) nb.classList.add('busy');
          var payload = { titleId: st.title.titleId, gameId: st.title.gameId || '', title: st.title.title || '', category: m.category, name: m.name.trim(), description: m.description.trim(), tags: m.tags.split(',').map(function (x) { return x.trim(); }).filter(Boolean).slice(0, 10), nickname: m.nickname.trim(), gameVersion: st.title.version || (st.exp && st.exp.manifest && st.exp.manifest.gameVersion) || '', region: st.title.region || '', dirName: st.dir.name, size: (st.up && st.up.size) || (st.exp && st.exp.size) || 0, sha256: (st.up && st.up.sha256) || (st.exp && st.exp.sha256) || '', url: st.up ? st.up.url : '', storagePath: st.up && st.up.storagePath ? st.up.storagePath : '', host: st.up && st.up.host ? st.up.host : st.host, zipPath: st.exp ? st.exp.zipPath : '' };
          ca.api.savesSubmit(payload).then(function (r) { st.busy = false; st.sub = r; if (m.nickname && m.nickname !== sv.nickname) ca.api.settingsPut({ saves: { nickname: m.nickname } }).then(function (rr) { if (rr.settings) S.set('settings', rr.settings); }, function () { /* ignore */ }); cache.all = null; cache.byGame = {}; st.step = 6; paint(); }, function (err) { st.busy = false; if (nb) nb.classList.remove('busy'); ui.errorToast(err, t('saves.wizSubmit')); });
        }
      } else if (s === 'done') {
        var r = st.sub || {};
        /* method "site": one row inserted into the save site's collection, live there and in the store at once —
           no GitHub hand-off. "github": the prefilled issue (also the fallback when the site refused, siteError). */
        var onSite = r.method === 'site' || r.method === 'supabase';
        /* a fallback reply names no site (its "backend" is the hand-off): the name then comes from the store listing */
        var siteName = (onSite ? r.backendName || r.backend : '') || (st.site && st.site.name) || 'XenoKing';
        var heroText = onSite ? (r.status === 'pending' ? t('saves.wizDoneBackend', { status: r.status }) : t('saves.wizDoneSite', { name: siteName })) : r.issueUrl ? t('saves.wizDoneIssue') : t('saves.wizDoneBackend', { status: r.status || 'pending' });
        body.appendChild(h('div.wiz-hero', [h('span.ic.okring', { html: U.icon('check').replace('<svg ', '<svg style="width:30px;height:30px;fill:none;stroke:currentColor;stroke-width:2.5" ') }), h('div', [h('h2', { text: t('saves.wizDoneTitle') }), h('p', { 'data-done': onSite ? 'site' : r.issueUrl ? 'github' : 'backend', text: heroText })])]));
        /* the site did not take it: the GitHub hand-off below carries a public copy (publicUrl) when the zip was only in the site's bucket */
        if (!onSite && r.siteError && r.issueUrl) body.appendChild(h('div.panel.note.warn', { 'data-site-fallback': '' }, [h('div.row', [ui.svg('alert'), h('span', { text: t('saves.wizSiteFallback', { name: siteName }) })]), h('p.dim.small', { text: String(r.siteError) }), r.publicUrl ? h('p.dim.small.mono', { text: String(r.publicUrl) }) : null]));
        if (!onSite && r.issueUrl) {
          body.appendChild(h('div', { style: { marginTop: '8px' } }, [ui.qr(r.issueUrl)]));
          body.appendChild(h('div.pair-url.small', { text: t('game2.issueOnGithub') }));
          var ob = h('button.btn.primary.lg', { nav: 'wz-open-issue', 'data-autofocus': '', html: U.icon('external') + '<span>' + U.escapeHtml(t('game2.openIssue')) + '</span>' }); ob.addEventListener('click', function () { U.openExternal(r.issueUrl); });
          var cb = h('button.btn', { nav: 'wz-copy', html: U.icon('copy') + '<span>' + U.escapeHtml(t('game2.copyUrl')) + '</span>' }); cb.addEventListener('click', function () { U.copyText(r.issueUrl).then(function (ok) { ui.toast({ cls: ok ? 'good' : 'bad', title: ok ? t('toast.copied') : t('toast.copyFailed') }); }); });
          body.appendChild(h('div.btn-row', { style: { justifyContent: 'center', marginTop: '14px' } }, [ob, cb]));
          body.appendChild(h('p.dim.small', { style: { textAlign: 'center', marginTop: '12px' }, text: t('saves.wizReviewNote') }));
        } else {
          var kv = h('dl.kv');
          if (onSite) { kv.appendChild(h('dt', { text: t('saves.backend') })); kv.appendChild(h('dd', { text: siteName })); }
          if (r.id) { kv.appendChild(h('dt', { text: 'ID' })); kv.appendChild(h('dd.mono', { text: String(r.id) })); }
          if (!onSite && r.backend) { kv.appendChild(h('dt', { text: t('saves.backend') })); kv.appendChild(h('dd', { text: String(r.backend) })); }
          if (r.url) { kv.appendChild(h('dt', { text: 'URL' })); kv.appendChild(h('dd.mono', { text: String(r.url) })); }
          if (kv.childNodes.length) body.appendChild(h('div.panel', [kv]));
        }
        U.clear(hd.foot);
        hd.foot.appendChild(h('span.spacer'));
        hd.foot.appendChild(h('button.btn.primary.lg', { nav: 'wz-done', text: t('common.done'), on: { click: function () { ui.closeLayer(hd); } } }));
      }
      requestAnimationFrame(function () { if (!hd.el.contains(document.activeElement)) XS.nav.focusFirst(hd.el); });
    }
    function shareable() { return !!(st.exp && st.exp.shareable !== false && st.exp.zipPath); }
    paint();
    return hd;
  }

  XS.router.register('saves', {
    tab: false, icon: 'shield', label: function () { return t('saves.title'); },
    mount: function (el) { root = el; },
    show: function (params) { if (params && params.q !== undefined) scr.q = params.q; if (params && params.category) scr.cat = params.category; render(); },
    refresh: render
  });
  XS.saves = {
    count: function (g) { return fetchGame(g).then(function (l) { return l.length; }); },
    newest: function (n) { return fetchAll().then(function (list) { return list.slice().sort(function (a, b) { return (b.createdAt || '') < (a.createdAt || '') ? -1 : 1; }).slice(0, n || 12); }); },
    spotlight: spotlight, all: function () { return fetchAll(); },
    rail: rail, card: card, renderGameTab: renderGameTab, openSave: openSave, install: installSheet, download: downloadFile, report: report, upload: uploadWizard,
    engine: engine, garlic: garlic, startGarlic: startGarlic, categories: CATS, invalidate: function () { cache.all = null; cache.byGame = {}; }
  };
})();
