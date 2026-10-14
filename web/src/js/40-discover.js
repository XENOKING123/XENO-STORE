/* XENO STORE — 40-discover.js: popular-games showcase stage (featured.hero first, then popularity), rails incl. owner-curated featured rows,
   backports, community saves, homebrew essentials; announcement banner; prefetch; surprise me; All games grid. */
(function () {
  'use strict';
  var XS = window.XS, U = XS.util, S = XS.state, ui = XS.ui, ca = XS.ca, h = U.h, t = XS.t;
  var PAGE = 96;
  var page = 0, root = null, gridEl = null;
  var dirty = false;   /* something changed while Discover was hidden (recent games, job set): re-render on the next show */

  /* ---------- Showcase stage: a moving line-up of the most popular games ---------- */
  var PERIOD = 7500, PERIOD_CALM = 12000, IDLE_RESUME = 20000, TICK = 250;
  var stage = null, stageTimer = null;

  function featured() { var st = S.data.status; return (st && st.featured) || {}; }
  function cmpPopular(a, b) {
    var d = (Number(b.popularity) || 0) - (Number(a.popularity) || 0); if (d) return d;
    var fa = a.featured > 0 ? a.featured : 1e9, fb = b.featured > 0 ? b.featured : 1e9;
    if (fa !== fb) return fa - fb;
    var ma = typeof a.metacritic === 'number' ? a.metacritic : -1, mb = typeof b.metacritic === 'number' ? b.metacritic : -1;
    if (ma !== mb) return mb - ma;
    return String(a.title).localeCompare(String(b.title));
  }
  /* Owner-curated ids first (§17.1), then the popular ranking; up to 10 games with art. */
  function showcasePool() {
    var out = [], seen = {};
    function add(g) { if (g && ca.isGame(g) && (g.hero || g.cover) && !seen[g.id] && out.length < 10) { seen[g.id] = 1; out.push(g); } }
    (featured().hero || []).forEach(function (id) { add(S.game(id)); });
    S.data.games.filter(ca.isGame).slice().sort(cmpPopular).forEach(add);
    return out;
  }
  function period() { return U.reducedMotion() ? PERIOD_CALM : PERIOD; }

  function renderStage(container, ambient) {
    stopStage();
    var pool = showcasePool();
    if (!pool.length) { stage = null; return; }
    var keepId = stage && stage.pool[stage.idx] ? stage.pool[stage.idx].id : null;
    var startIdx = 0;
    pool.forEach(function (g, i) { if (g.id === keepId) startIdx = i; });
    var el = h('section.hero.stage', { data: { navFrame: '' }, 'aria-label': t('discover3.showcase') });
    var a = h('div.art.a'), b = h('div.art.b');
    el.appendChild(a); el.appendChild(b);
    var body = h('div.stage-body');
    var rkNo = h('span.rk-no'), rkLbl = h('span.rk-lbl');
    body.appendChild(h('div.stage-rank', [rkNo, rkLbl]));
    var title = h('h1.stage-title');
    body.appendChild(title);
    var tag = h('p.tagline.clamp2');
    body.appendChild(tag);
    var chips = h('div.chips.stage-chips');
    body.appendChild(chips);
    var acts = h('div.actions');
    var view = h('button.btn.primary.lg', { nav: 'stage-view', html: U.icon('play') + '<span>' + U.escapeHtml(t('discover.viewGame')) + '</span>', data: { hints: 'select' } });
    view.addEventListener('click', function () { var g = current(); if (g) XS.router.go('game', { id: g.id }); });
    var fav = h('button.btn.lg.icon-only.fav', { nav: 'stage-fav', html: U.icon('heart'), 'aria-label': t('game.favorite') });
    fav.addEventListener('click', function () { var g = current(); if (g) XS.actions.toggleFavorite(g.id); });
    var more = h('button.btn.lg.ghost', { nav: 'stage-more', html: U.icon('star') + '<span>' + U.escapeHtml(t('discover3.allPopular')) + '</span>' });
    more.addEventListener('click', function () { XS.router.go('browse', { sort: 'popular' }); });
    acts.appendChild(view); acts.appendChild(fav); acts.appendChild(more);
    body.appendChild(acts);
    el.appendChild(body);
    var strip = h('div.stage-strip', { data: { navRegion: '' }, role: 'list', 'aria-label': t('discover3.lineup') });
    var marker = h('i.strip-marker');
    strip.appendChild(marker);
    var tiles = pool.map(function (g, i) {
      var tile = h('button.strip-tile', { nav: 'stage-strip:' + i, data: { game: g.id, hints: 'select' }, 'aria-label': t('discover3.tileLabel', { title: g.title, n: i + 1 }) });
      tile.appendChild(ui.cover(g.cover, g.coverFallback, g.title));
      tile.appendChild(h('span.rk', { text: String(i + 1) }));
      var pending = null;
      tile.addEventListener('focus', function () {
        if (pending) clearTimeout(pending);
        pending = setTimeout(function () { pending = null; if (stage && stage.idx !== i && document.activeElement === tile) select(i, 'focus'); }, 160);
      });
      tile.addEventListener('click', function () {
        if (stage && stage.idx === i) XS.router.go('game', { id: g.id }); else select(i, 'tap');
      });
      strip.appendChild(tile);
      return tile;
    });
    el.appendChild(strip);
    var bar = h('div.stage-bar', [h('i')]);
    el.appendChild(bar);
    el.appendChild(h('span.stage-paused', { text: t('discover3.paused') }));
    var ambA = h('i.g.a', [h('b')]), ambB = h('i.g.b', [h('b')]);
    U.clear(ambient);
    ambient.appendChild(ambA); ambient.appendChild(ambB);
    stage = { el: el, pool: pool, idx: -1, a: a, b: b, live: null, body: body, rkNo: rkNo, rkLbl: rkLbl, title: title, tag: tag, chips: chips, view: view, fav: fav,
      strip: strip, marker: marker, tiles: tiles, bar: bar, ambA: ambA, ambB: ambB, ambLive: null, elapsed: 0, last: Date.now(), lastInput: 0, held: false };
    function noteInput() { if (stage) stage.lastInput = Date.now(); }
    el.addEventListener('keydown', noteInput);
    el.addEventListener('focusin', noteInput);
    el.addEventListener('pointerdown', noteInput);
    container.appendChild(el);
    select(startIdx, 'init');
    startStage();
  }
  function current() { return stage ? stage.pool[stage.idx] : null; }

  function select(i, reason) {
    var s = stage;
    if (!s || !s.pool[i]) return;
    var g = s.pool[i];
    s.idx = i; s.elapsed = 0; s.last = Date.now();
    /* art: the incoming layer fades in over the still-opaque outgoing one (opacity only) */
    var incoming = s.live === s.a ? s.b : s.a, outgoing = s.live;
    U.clear(incoming);
    incoming.classList.remove('live', 'out');
    var cov = ui.cover(g.hero || g.cover, g.heroFallback || g.coverFallback, g.title);
    incoming.appendChild(cov);
    incoming.classList.toggle('tall', !g.hero);
    var swapped = false;
    function swap() {
      if (swapped || stage !== s || s.idx !== i) return;
      swapped = true;
      if (outgoing) { outgoing.classList.add('out'); outgoing.classList.remove('live'); }
      incoming.classList.add('live');
      s.live = incoming;
      if (outgoing) setTimeout(function () { if (outgoing !== s.live) { outgoing.classList.remove('out'); U.clear(outgoing); } }, 950);
    }
    var img = cov.querySelector('img');
    if (!outgoing || reason === 'init' || !img || U.reducedMotion()) swap();
    else {
      img.addEventListener('load', swap);
      img.addEventListener('error', function () { setTimeout(swap, 250); });
      setTimeout(swap, 1500);   /* slow CDN: never leave the old slide up for long */
    }
    /* body: patch the persistent nodes so a focused button keeps its focus */
    var curated = (featured().hero || []).indexOf(g.id) >= 0;
    s.rkNo.textContent = t('discover3.rank', { n: i + 1 });
    s.rkLbl.textContent = curated ? t('discover2.kickerStaff') : t('discover3.popularNow');
    s.title.textContent = g.title;
    s.tag.textContent = g.tagline || '';
    s.tag.style.display = g.tagline ? '' : 'none';
    U.clear(s.chips);
    s.chips.appendChild(ui.platChip(g.platform));
    if (g.genres && g.genres[0]) s.chips.appendChild(h('span.chip', { text: g.genres[0] }));
    if (g.sizeMax) s.chips.appendChild(h('span.chip', { text: ui.sizeRange(g) }));
    var mc = ca.metacritic(g.metacritic); if (mc) s.chips.appendChild(mc);
    if (g.verified) s.chips.appendChild(h('span.chip.gold', { html: U.icon('check') + '<span>' + U.escapeHtml(t('common.verified')) + '</span>' }));
    if (g.cheats > 0) s.chips.appendChild(h('span.chip.gold', { html: U.icon('zap') + '<span>' + U.escapeHtml(t('game2.cheatsChip', { n: g.cheats })) + '</span>' }));
    s.view.setAttribute('data-game', g.id);
    s.fav.setAttribute('data-game', g.id);
    s.fav.classList.toggle('on', S.isFav(g.id));
    s.body.classList.remove('in');
    void s.body.offsetWidth;   /* restart the staggered reveal */
    s.body.classList.add('in');
    /* line-up: live tile + the on-air marker */
    s.tiles.forEach(function (tile, k) { tile.classList.toggle('live', k === i); });
    var tile = s.tiles[i];
    if (tile) s.marker.style.transform = 'translate3d(' + tile.offsetLeft + 'px, 0, 0)';
    /* progress bar: a fresh <i> restarts the fill */
    U.clear(s.bar);
    var fill = h('i');
    fill.style.animationDuration = period() + 'ms';
    s.bar.appendChild(fill);
    /* ambient: the screen breathes with the slide's colour */
    var tint = ca.hashTint(g.title, g.tint);
    var ambIn = s.ambLive === s.ambA ? s.ambB : s.ambA, ambOut = s.ambLive;
    ambIn.style.setProperty('--tint', tint);
    ambIn.classList.add('live');
    if (ambOut) ambOut.classList.remove('live');
    s.ambLive = ambIn;
    ca.ambient(g.cover || g.hero, tint).then(function (c) { if (stage === s && s.idx === i && c) ambIn.style.setProperty('--tint', c); });
    /* preload the next two slides */
    if (!ca.safeMode()) [1, 2].forEach(function (k) { var n = s.pool[(i + k) % s.pool.length]; if (n && n !== g) { try { var im = new Image(); im.src = n.hero || n.cover; } catch (e) { /* ignore */ } } });
  }

  function stageHeld(now) {
    if (!root || !root.classList.contains('active') || document.hidden) return true;
    if (XS.ui.layers.length || ca.screensaver.active() || ca.safeMode()) return true;
    return stage.el.contains(document.activeElement) && now - stage.lastInput < IDLE_RESUME;
  }
  function tick() {
    if (!stage || !stage.el.isConnected) { stopStage(); return; }
    var now = Date.now(), dt = now - stage.last;
    stage.last = now;
    var held = stageHeld(now);
    if (held !== stage.held) { stage.held = held; stage.el.classList.toggle('held', held); }
    if (held || stage.pool.length < 2) return;
    stage.elapsed += dt;
    if (stage.elapsed >= period()) select((stage.idx + 1) % stage.pool.length, 'auto');
  }
  function startStage() { stopStage(); if (stage) { stage.last = Date.now(); stageTimer = setInterval(tick, TICK); } }
  function stopStage() { if (stageTimer) clearInterval(stageTimer); stageTimer = null; }
  /* Discover's top bar floats over the stage: clear while the stage is in view, glass once the user scrolls. */
  function setUnderBar(active) {
    var de = document.documentElement;
    de.classList.toggle('on-discover', !!active);
    de.classList.toggle('stage-top', !!active && !!stage && !!root && root.scrollTop < 40);
  }

  function jobsByGame() {
    var m = {};
    S.data.queue.jobs.forEach(function (j) { if (!m[j.gameId] || rank(j) < rank(m[j.gameId])) m[j.gameId] = j; });
    return m;
  }
  function rank(j) { return ['downloading', 'connecting', 'verifying', 'installing', 'queued', 'paused', 'failed', 'done', 'installed', 'cancelled'].indexOf(j.state); }
  function jobGame(j) {
    var g = S.game(j.gameId);
    if (g && ca.isGame(g)) return g;
    var syn = { id: j.gameId, title: j.title, platform: j.platform, cover: j.cover, coverFallback: '', releases: [], sizeMax: j.total, _synthetic: true };
    if (g && g.platform === 'HOMEBREW') { for (var k in g) if (k !== 'cover') syn[k] = g[k]; syn._homebrew = true; }
    else if (j.type === 'save-install' || j.type === 'save-download' || j.kind === 'app') syn._homebrew = j.kind === 'app';
    return syn;
  }
  function games(list) { return (list || []).filter(ca.isGame); }

  function rails(container) {
    var L = S.data.lists, jobs = jobsByGame();
    function add(id, title, list, opts) {
      opts = opts || {};
      opts.id = id; opts.jobs = jobs; opts.limit = opts.limit || 20;
      if (!list || !list.length) return;
      var sec = ui.rail(title, list, opts);
      if (sec) container.appendChild(sec);
    }
    var cont = S.data.queue.jobs.filter(function (j) { return ['downloading', 'connecting', 'queued', 'paused', 'verifying', 'installing', 'failed'].indexOf(j.state) >= 0; });
    /* homebrew installs show as app cards (proxied cover, opens the app page); everything else as game cards */
    if (cont.length) add('continue', t('discover.continue'), cont.map(jobGame), { more: function () { XS.router.go('downloads'); }, render: function (g) { return g._homebrew && XS.homebrew ? XS.homebrew.cardFromGame(g, 'rail.continue') : ui.card(g, { job: jobs[g.id], navPrefix: 'rail.continue' }); } });
    add('new', t('discover.newOnXeno'), games(L.newest), { more: function () { XS.router.go('browse', { sort: 'added' }); } });
    /* owner-curated rows from updates/featured.json (§17.1) */
    (featured().rows || []).forEach(function (row, i) {
      var list = (row.ids || []).map(S.game).filter(ca.isGame);
      var title = ca.localized(row.title, '');
      if (list.length && title) add('featured-' + i, title, list);
    });
    add('latest', t('discover.latestReleases'), games(L.latest), { more: function () { XS.router.go('browse', { sort: 'newest' }); } });
    add('trending', t('discover.trending'), games(L.trending), { more: function () { XS.router.go('browse', { sort: 'popular' }); } });
    add('verified', t('discover.verifiedFast'), games(L.verified), { more: function () { XS.router.go('browse', { verified: 1, source: 'archive' }); } });
    var bps = S.data.games.filter(function (g) { return ca.isGame(g) && ca.counts(g).backport > 0; }).sort(function (a, b) { return (b.popularity || 0) - (a.popularity || 0); }).slice(0, 30);
    if (bps.length) add('backports', t('discover2.backports'), bps, { more: function () { XS.router.go('browse', { backports: 1 }); } });
    add('light', t('discover.light'), games(L.light), { more: function () { XS.router.go('browse', { size: 'le10', sort: 'smallest' }); } });
    add('massive', t('discover.massive'), games(L.massive), { more: function () { XS.router.go('browse', { size: 'gt100', sort: 'largest' }); } });
    add('ps4', t('discover.ps4'), games(L.ps4), { more: function () { XS.router.go('browse', { platform: 'PS4' }); } });
    var mcs = S.data.games.filter(function (g) { return ca.isGame(g) && typeof g.metacritic === 'number' && g.metacritic >= 85; }).sort(function (a, b) { return b.metacritic - a.metacritic; }).slice(0, 30);
    if (mcs.length) add('acclaimed', t('discover2.acclaimed'), mcs, { more: function () { XS.router.go('browse', { sort: 'metacritic' }); } });
    var favs = S.data.favorites.map(S.game).filter(ca.isGame);
    if (favs.length) add('favorites', t('discover.yourFavorites'), favs, { more: function () { XS.router.go('browse', { favorites: 1 }); } });
    var recent = S.data.recent.map(S.game).filter(ca.isGame);
    if (recent.length) add('recent', t('discover.recentlyViewed'), recent);
    /* community saves + homebrew essentials: async rails that fill in when the API answers */
    container.appendChild(savesRailHost());
    container.appendChild(homebrewRailHost());
    var genres = S.data.genres.slice(0, 16);
    if (genres.length) {
      var sec = ui.rail(t('discover.popularGenres'), genres, { id: 'genres', total: genres.length, render: function (gn) { return ui.genreTile(gn.name, gn.count); } });
      if (sec) container.appendChild(sec);
    }
  }
  function savesRailHost() {
    var host = h('div', { data: { railHost: 'saves' } });
    if (!XS.saves || !S.data.serverOk) return host;
    XS.saves.all().then(function (all) {
      if (!all.length || !host.isConnected) return;
      var goSaves = function () { XS.router.go('saves'); };
      var sp = XS.saves.spotlight(all, { id: 'd-spot', compact: true, max: 5, more: goSaves });
      var list = all.slice().sort(function (a, b) { return (b.createdAt || '') < (a.createdAt || '') ? -1 : 1; }).slice(0, 12);
      var sec = XS.saves.rail(t('discover2.newSaves'), list, { id: 'saves', more: goSaves });
      U.clear(host);
      if (sp) host.appendChild(sp);
      if (sec) host.appendChild(sec);
    }, function () { /* offline: no rail */ });
    return host;
  }
  function homebrewRailHost() {
    var host = h('div', { data: { railHost: 'homebrew' } });
    if (!XS.homebrew || !S.data.serverOk) return host;
    XS.homebrew.essentials(12).then(function (apps) {
      if (!apps.length) return;
      var sec = XS.homebrew.rail(t('discover2.homebrewEssentials'), apps, { id: 'hb', more: function () { XS.router.go('homebrew'); } });
      if (sec && host.isConnected) { U.clear(host); host.appendChild(sec); }
    }, function () { /* offline: no rail */ });
    return host;
  }

  function allGames(container) {
    var list = games(S.data.lists.alpha);
    var pages = Math.max(1, Math.ceil(list.length / PAGE));
    page = Math.min(page, pages - 1);
    var sec = h('section.all-games');
    sec.appendChild(h('div.section-head', [h('h2.section-title', [t('discover.allGames'), h('span.count', { text: t('common.nGames', { n: list.length }) })]), h('span.spacer'), h('span.mute', { text: t('common.pageOf', { page: page + 1, pages: pages }) })]));
    gridEl = h('div.vgrid', { data: { navRegion: '' } });
    sec.appendChild(gridEl);
    ui.vgrid(gridEl, list.slice(page * PAGE, page * PAGE + PAGE), root, function (g) { return ui.card(g); });
    sec.appendChild(pager(pages));
    container.appendChild(sec);
  }
  function pager(pages) {
    var p = h('div.pager', { data: { navRegion: '' } });
    function btn(label, target, on, id) {
      var b = h('button.pg' + (on ? '.on' : ''), { nav: 'pg-' + id, text: label });
      if (target === null) b.setAttribute('aria-disabled', 'true');
      b.addEventListener('click', function () { if (target === null) return; page = target; render(); XS.nav.remember('discover', 'pg-' + (target + 1)); XS.nav.restore(); });
      return b;
    }
    p.appendChild(btn(t('common.prev'), page > 0 ? page - 1 : null, false, 'prev'));
    var win = [];
    for (var i = 0; i < pages; i++) if (i < 2 || i >= pages - 2 || Math.abs(i - page) <= 2) win.push(i);
    var last = -1;
    win.forEach(function (i) { if (last >= 0 && i - last > 1) p.appendChild(h('span.mute', { text: '…' })); p.appendChild(btn(String(i + 1), i, i === page, i + 1)); last = i; });
    p.appendChild(btn(t('common.next'), page < pages - 1 ? page + 1 : null, false, 'next'));
    return p;
  }

  /* Full render keeps the user's place: scroll offset and the focused element (by data-nav-id) survive. */
  function render() {
    if (!root) return;
    stopStage();
    dirty = false;
    featuredChanged();   /* the rendered featured set is now current */
    var scroll = root.scrollTop;
    var fid = root.contains(document.activeElement) ? document.activeElement.getAttribute('data-nav-id') : null;
    U.clear(root);
    var ambient = h('div.stage-ambient', { 'aria-hidden': 'true' });
    root.appendChild(ambient);
    var inner = h('div.screen-inner');
    root.appendChild(inner);
    if (!S.data.catalog) {
      inner.appendChild(h('div.hero.skel', { style: { minHeight: '440px', marginTop: '22px' } }));
      inner.appendChild(ui.skeletonRail(8)); inner.appendChild(ui.skeletonRail(8));
      return;
    }
    renderStage(inner, ambient);
    inner.classList.toggle('with-stage', !!stage);
    var top = h('div.discover-top');
    var req = h('button.btn.ghost', { nav: 'request-game', html: U.icon('plus') + '<span>' + U.escapeHtml(t('game2.requestGame')) + '</span>', title: t('game2.requestSub') });
    req.addEventListener('click', function () { ca.requestGame(''); });
    top.appendChild(req);
    top.appendChild(h('span.spacer'));
    var surprise = h('button.btn', { nav: 'surprise', html: U.icon('shuffle') + '<span>' + U.escapeHtml(t('discover.surpriseMe')) + '</span>' });
    surprise.addEventListener('click', function () { var pool = games(S.data.games); var g = pool[Math.floor(Math.random() * pool.length)]; if (g) XS.router.go('game', { id: g.id }); });
    top.appendChild(surprise);
    inner.appendChild(top);
    rails(inner);
    allGames(inner);
    root.scrollTop = scroll;
    if (fid) { var again = root.querySelector('[data-nav-id="' + fid + '"]'); if (again) XS.nav.focus(again, false); }
    setUnderBar(root.classList.contains('active'));
  }

  /* Only patch what changed on queue/favorite updates: no full re-render while the user is browsing. */
  function patchFavorites() {
    if (!root) return;
    U.$$('.card[data-game]', root).forEach(function (c) { c.classList.toggle('fav', S.isFav(c.getAttribute('data-game'))); });
    U.$$('.btn.fav[data-game]', root).forEach(function (b) { b.classList.toggle('on', S.isFav(b.getAttribute('data-game'))); });
  }
  var lastContinueKey = '';
  function patchQueue() {
    if (!root || !root.firstChild) return;
    var cont = S.data.queue.jobs.filter(function (j) { return ['downloading', 'connecting', 'queued', 'paused', 'verifying', 'installing', 'failed'].indexOf(j.state) >= 0; });
    var key = cont.map(function (j) { return j.id + ':' + j.state; }).join(',');
    if (key !== lastContinueKey) { lastContinueKey = key; if (root.classList.contains('active')) render(); else dirty = true; return; }
    if (!root.classList.contains('active')) return;
    var m = jobsByGame();
    U.$$('[data-rail="continue"] .card[data-game]', root).forEach(function (c) {
      var j = m[c.getAttribute('data-game')]; if (!j) return;
      var bar = c.querySelector('.prog i'); if (bar && j.total) bar.style.width = U.pct(j.written, j.total).toFixed(1) + '%';
      var sub = c.querySelector('.sub span'); if (sub) sub.textContent = t('dl.state.' + j.state) + (j.total ? ' · ' + U.fmtPct(U.pct(j.written, j.total)) : '');
    });
  }
  var lastFeaturedKey = '';
  function featuredChanged() {
    var key = JSON.stringify(featured().hero || []) + '|' + JSON.stringify((featured().rows || []).map(function (r) { return r.ids; }));
    if (key === lastFeaturedKey) return false;
    lastFeaturedKey = key; return true;
  }

  XS.discover = { stage: { index: function () { return stage ? stage.idx : -1; }, size: function () { return stage ? stage.pool.length : 0; }, select: function (i) { select(i, 'tap'); } } };

  XS.router.register('discover', {
    tab: true, icon: 'compass', label: function () { return t('nav.discover'); },
    mount: function (el) {
      root = el;
      /* prefetch the next 6 covers of the rail the user walks along (§17.7) */
      el.addEventListener('focusin', function (e) { var card = e.target && e.target.closest ? e.target.closest('.card') : null; if (card && card.parentNode && card.parentNode.classList.contains('rail-track')) ca.prefetchRail(card.parentNode, card); });
      el.addEventListener('scroll', function () { if (el.classList.contains('active')) setUnderBar(true); }, { passive: true });
      ca.banner();
      ca.screensaver.setup();
    },
    show: function () { if (!root.firstChild || !S.data.catalog || dirty) render(); else { startStage(); patchFavorites(); } setUnderBar(true); },
    hide: function () { stopStage(); setUnderBar(false); },
    refresh: render
  });
  S.on('catalog', function () { if (root) { lastContinueKey = ''; render(); if (root.classList.contains('active')) XS.nav.restore(); } });
  S.on('favorites', patchFavorites);
  S.on('queue', patchQueue);
  S.on('recent', function () { dirty = true; });   /* "Recently viewed" refreshes when the user comes back, never under their focus */
  S.on('status', function () { ca.banner(); if (root && S.data.catalog && featuredChanged()) { if (root.classList.contains('active') && !root.contains(document.activeElement)) render(); else dirty = true; } });
})();
