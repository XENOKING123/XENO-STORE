/* XENO STORE — 10-state.js: store + catalog indexes + derived lists. pub/sub by key. */
(function () {
  'use strict';
  var XS = window.XS, U = XS.util;
  var data = {
    catalog: null, games: [], byId: {}, byTitleId: {}, lists: {}, genres: [], sources: [],
    settings: null, queue: { jobs: [], summary: {} }, favorites: [], collections: [], recent: [],
    status: null, storage: [], library: null, serverOk: true, online: true, pin: U.store.get('pin', ''), token: U.store.get('token', '')
  };
  var subs = {};
  var state = {
    data: data,
    get: function (k) { return data[k]; },
    set: function (k, v) { data[k] = v; state.emit(k, v); },
    on: function (k, fn) { (subs[k] = subs[k] || []).push(fn); return function () { state.off(k, fn); }; },
    off: function (k, fn) { var a = subs[k] || []; var i = a.indexOf(fn); if (i >= 0) a.splice(i, 1); },
    emit: function (k, v) { var a = subs[k] || []; for (var i = 0; i < a.length; i++) { try { a[i](v); } catch (e) { console.error(e); } } },

    /* Builds every index the screens need once per catalog load (never per render). */
    setCatalog: function (cat) {
      data.catalog = cat;
      var games = (cat && cat.games) || [];
      var byId = {}, byTitle = {}, genreCount = {};
      var dg = (cat && cat.defaults && cat.defaults.game) || null, dr = (cat && cat.defaults && cat.defaults.release) || null;
      var coverUse = {};
      for (var c0 = 0; c0 < games.length; c0++) { var cv = games[c0].cover; if (cv) coverUse[cv] = (coverUse[cv] || 0) + 1; }
      for (var i = 0; i < games.length; i++) {
        var g = games[i];
        /* art worth a front row: its own cover, not a shared collection thumbnail */
        g._artOk = !!g.cover && coverUse[g.cover] === 1 && g.cover.indexOf('archive.org/services/img') < 0;
        /* compact catalogs leave empty members out; put them back so every screen sees the full shape */
        if (dg) fillDefaults(g, dg);
        if (!g.releases) g.releases = [];
        if (dr) for (var ri = 0; ri < g.releases.length; ri++) fillDefaults(g.releases[ri], dr);
        byId[g.id] = g;
        g._nt = U.normTitle(g.title);
        g._series = U.seriesKey(g.title);
        g._search = (g.title + ' ' + (g.titleIds || []).join(' ') + ' ' + (g.publisher || '') + ' ' + (g.developer || '')).toLowerCase();
        g._kinds = kinds(g);
        g._hasHash = hasHash(g);
        g._direct = isDirect(g);
        var tids = g.titleIds || [];
        for (var t = 0; t < tids.length; t++) byTitle[tids[t]] = g;
        var gs = g.genres || [];
        for (var k = 0; k < gs.length; k++) genreCount[gs[k]] = (genreCount[gs[k]] || 0) + 1;
      }
      data.games = games; data.byId = byId; data.byTitleId = byTitle;
      data.sources = (cat && cat.sources) || [];
      var genres = Object.keys(genreCount).map(function (n) { return { name: n, count: genreCount[n] }; });
      genres.sort(function (a, b) { return b.count - a.count || a.name.localeCompare(b.name); });
      data.genres = genres;
      data.lists = buildLists(games);
      state.emit('catalog', cat);
    },
    game: function (id) { return data.byId[id] || null; },
    isFav: function (id) { return data.favorites.indexOf(id) >= 0; },
    jobsFor: function (gameId) { return data.queue.jobs.filter(function (j) { return j.gameId === gameId; }); },
    activeJobFor: function (gameId) {
      var js = state.jobsFor(gameId);
      var order = ['downloading', 'connecting', 'verifying', 'installing', 'queued', 'paused', 'failed', 'done', 'installed', 'cancelled'];
      js.sort(function (a, b) { return order.indexOf(a.state) - order.indexOf(b.state); });
      return js[0] || null;
    },
    defaultRoot: function () {
      var roots = data.storage || [];
      for (var i = 0; i < roots.length; i++) if (roots[i]['default']) return roots[i];
      var s = data.settings && data.settings.downloads && data.settings.downloads.defaultRoot;
      for (var j = 0; j < roots.length; j++) if (roots[j].path === s) return roots[j];
      return roots[0] || null;
    },
    sourceEnabled: function (sourceId) {
      var s = data.settings && data.settings.sources && data.settings.sources.enabled;
      if (!s) return true;
      if (sourceId in s) return !!s[sourceId];
      return s.other !== false;
    },
    similar: function (g, limit) {
      var out = [], gs = g.genres || [];
      if (!gs.length) return out;
      var scored = [];
      for (var i = 0; i < data.games.length; i++) {
        var o = data.games[i];
        if (o === g || o._nt === g._nt) continue;
        var n = 0, og = o.genres || [];
        for (var k = 0; k < og.length; k++) if (gs.indexOf(og[k]) >= 0) n++;
        if (n) scored.push({ n: n + (o.platform === g.platform ? .5 : 0) + (o.popularity || 0) * .01, g: o });
      }
      scored.sort(function (a, b) { return b.n - a.n; });
      for (var j = 0; j < scored.length && out.length < (limit || 20); j++) out.push(scored[j].g);
      return out;
    },
    series: function (g, limit) {
      var out = [];
      if (!g._series) return out;
      for (var i = 0; i < data.games.length && out.length < (limit || 20); i++) {
        var o = data.games[i];
        if (o !== g && o._series === g._series) out.push(o);
      }
      return out;
    }
  };

  function fillDefaults(o, d) {
    for (var k in d) {
      if (Object.prototype.hasOwnProperty.call(d, k) && !Object.prototype.hasOwnProperty.call(o, k)) {
        var v = d[k];
        o[k] = Array.isArray(v) ? [] : (v && typeof v === 'object' ? {} : v);
      }
    }
  }
  function kinds(g) {
    var k = {};
    var rs = g.releases || [];
    for (var i = 0; i < rs.length; i++) {
      var kind = rs[i].kind || (rs[i].format === 'PAGE' ? 'page' : 'game');
      if (rs[i].format === 'PAGE') kind = 'page';
      k[kind] = (k[kind] || 0) + 1;
    }
    return k;
  }
  function hasHash(g) {
    var rs = g.releases || [];
    for (var i = 0; i < rs.length; i++) if (rs[i].sha256 || rs[i].md5 || rs[i].sha1) return true;
    return false;
  }
  function isDirect(g) {
    var rs = g.releases || [];
    for (var i = 0; i < rs.length; i++) if (rs[i].sourceId === 'archive' || rs[i].sourceId === 'xeno') return true;
    return false;
  }
  function buildLists(games) {
    var all = games.slice();
    function by(fn, desc) { var a = all.slice(); a.sort(function (x, y) { var p = fn(x), q = fn(y); return desc ? (q > p ? 1 : q < p ? -1 : 0) : (p > q ? 1 : p < q ? -1 : 0); }); return a; }
    /* front rows: PS5 before PS4, real art before placeholders, then popularity */
    function front(a, b) {
      return ((b.platform === 'PS5') - (a.platform === 'PS5')) || ((b._artOk ? 1 : 0) - (a._artOk ? 1 : 0)) || ((b.popularity || 0) - (a.popularity || 0));
    }
    function sorted(list, key) {
      return list.slice().sort(function (a, b) { var p = key(a), q = key(b); return (q > p ? 1 : q < p ? -1 : 0) || front(a, b); });
    }
    var GB = 1e9;
    var today = new Date().toISOString().slice(0, 10);
    return {
      topPs5: all.filter(function (g) { return g.platform === 'PS5' && g._artOk; }).sort(function (a, b) { return (b.popularity || 0) - (a.popularity || 0); }).slice(0, 40),
      newest: sorted(all.filter(function (g) { return g._artOk; }), function (g) { return (g.addedAt || '').slice(0, 10); }).slice(0, 40),
      latest: sorted(all.filter(function (g) { return g.releaseDate && g.releaseDate <= today && g._artOk; }), function (g) { return g.releaseDate; }).slice(0, 40),
      trending: all.slice().sort(function (a, b) { return ((b.popularity || 0) - (a.popularity || 0)) || front(a, b); }).filter(function (g) { return g._artOk; }).slice(0, 40),
      verified: all.filter(function (g) { return g._direct && g._hasHash && g.verified; }).sort(function (a, b) { return (b.popularity || 0) - (a.popularity || 0); }).slice(0, 40),
      light: all.filter(function (g) { return g.sizeMax > 0 && g.sizeMax < 10 * GB; }).sort(function (a, b) { return (b.popularity || 0) - (a.popularity || 0); }).slice(0, 40),
      massive: all.filter(function (g) { return g.sizeMin > 80 * GB; }).sort(function (a, b) { return b.sizeMax - a.sizeMax; }).slice(0, 40),
      ps4: all.filter(function (g) { return g.platform === 'PS4'; }).sort(function (a, b) { return (b.popularity || 0) - (a.popularity || 0); }).slice(0, 40),
      featured: all.filter(function (g) { return g.featured > 0; }).sort(function (a, b) { return a.featured - b.featured; }),
      alpha: by(function (g) { return g._nt; }, false)
    };
  }
  XS.state = state;
  /* Screen registry: screens register themselves at load time; 50-router.js takes over the same object. */
  XS.router = { defs: {}, order: [], register: function (id, def) { this.defs[id] = def; this.order.push(id); } };
})();
