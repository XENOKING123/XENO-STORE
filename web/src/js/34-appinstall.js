/* XENO STORE — 34-appinstall.js: the Games-row app (PPSA99500). GET /api/app/status says whether the package is in
   /data/homebrew (or the app is registered); POST /api/app/install starts a console job that downloads, verifies and
   swaps in PPSA99500.ffpkg and asks ShadowMountPlus to rescan. Progress comes from /api/saves/jobs/{id}.
   XS.appInstall.card() is the Settings → Updates card, XS.appInstall.hint() the one-time Discover hint (PS5 only). */
(function () {
  'use strict';
  var XS = window.XS, U = XS.util, S = XS.state, ui = XS.ui, cb = XS.cb, h = U.h, t = XS.t;
  var A = { status: null, at: 0, loading: null, job: null, watch: null };
  var subs = [];
  var HINT_KEY = 'appHint.dismissed';
  var ERRORS = ['sha256_mismatch', 'sha256_unavailable', 'no_space', 'not_package', 'too_large', 'download_failed'];

  /* Views repaint on every change while they are in the document; detached ones drop out. */
  function emit() {
    var now = Date.now();
    subs = subs.filter(function (s) {
      if (s.el.isConnected) { s.seen = true; try { s.fn(); } catch (e) { console.error(e); } return true; }
      return !s.seen && now - s.at < 10000;   /* built but not attached yet */
    });
  }
  function subscribe(el, fn) { subs.push({ el: el, fn: fn, at: Date.now(), seen: false }); }
  S.on('status', function () { if (subs.length) emit(); });

  function running() { return !!(A.job && (A.job.state === 'queued' || A.job.state === 'running')); }
  function noop() { /* offline or not answered: the views keep their last state */ }

  function load(force) {
    if (A.loading) return A.loading;
    if (!force && A.status && Date.now() - A.at < 15000) return Promise.resolve(A.status);
    A.loading = cb.api.get('/api/app/status', { timeout: 10000, quiet: true }).then(function (r) {
      A.loading = null; A.status = r; A.at = Date.now();
      if (r.job && !running()) follow(r.job);   /* an install started earlier, here or on another device */
      emit();
      return r;
    }, function (err) { A.loading = null; throw err; });
    return A.loading;
  }

  function errText(job) {
    var code = job.errorCode || '';
    if (ERRORS.indexOf(code) >= 0) return t('gameapp.err.' + code);
    return job.error || job.message || t('err.unknown');
  }
  /* The console's job messages are English; the phase is shown in the UI language instead. */
  function phaseText(job) {
    var m = String(job.message || '');
    if (/^Verif/.test(m)) return t('gameapp.verifying');
    if (/^Asking|^Installed/.test(m)) return t('gameapp.registering');
    if (!job.total && !job.done) return t('gameapp.checking');
    return job.total ? U.fmtBytes(job.done || 0) + ' / ' + U.fmtBytes(job.total) : U.fmtBytes(job.done || 0);
  }
  function jobView() {
    var j = A.job || {};
    return { id: j.id || '', title: t('gameapp.jobTitle'), state: j.state || 'queued', percent: j.percent, done: j.done, total: j.total, message: phaseText(j), error: j.error };
  }

  function follow(id) {
    if (A.watch && A.job && A.job.id === id) return A.watch;
    if (A.watch) A.watch.stop();
    A.job = { id: id, state: 'queued', done: 0, total: 0, percent: 0, message: '' };
    emit();
    var w = cb.watchJob(id, function (j) { A.job = j; emit(); }, { every: 800, maxPolls: 4000 });
    A.watch = w;
    w.then(function (j) {
      if (A.watch !== w) return;
      A.watch = null; A.job = j;
      if (j.state === 'done') {
        XS.sound && XS.sound.done();
        U.store.set(HINT_KEY, true);
        ui.toast({ cls: 'good', icon: 'gamepad', title: t('gameapp.doneTitle'), desc: t('gameapp.doneDesc'), timeout: 12000 });
      } else if (j.state === 'cancelled') ui.toast({ cls: 'gold', title: t('gameapp.cancelled') });
      else { XS.sound && XS.sound.error(); ui.toast({ cls: 'bad', title: t('gameapp.failedTitle'), desc: errText(j), timeout: 9000 }); }
      load(true).catch(emit);
    }, function (err) {
      if (A.watch !== w) return;
      A.watch = null; A.job = null;
      ui.errorToast(err, t('gameapp.title'));
      load(true).catch(emit);
    });
    return w;
  }

  function install() {
    if (running()) return Promise.resolve(A.job);
    A.job = { id: '', state: 'queued', done: 0, total: 0, percent: 0, message: '' };
    emit();
    return cb.api.post('/api/app/install', {}, { timeout: 20000 }).then(function (r) {
      var job = r.job || {};
      if (!job.id) { A.job = null; emit(); return null; }
      follow(job.id);
      return job;
    }, function (err) {
      A.job = null;
      emit();
      if (err && err.error === 'in_progress') { ui.toast({ cls: 'gold', title: t('gameapp.already') }); load(true).catch(noop); return null; }
      ui.errorToast(err, t('gameapp.failedTitle'));
      return null;
    });
  }
  function cancel() {
    if (!A.job || !A.job.id) return;
    cb.api.savesJobCancel(A.job.id).then(noop, ui.errorToast);   /* the watcher reports the end */
  }

  /* Full repaint only when the shape changes; progress patches the bar in place so controller focus stays put. */
  function view(el, keyOf, build) {
    var last = null;
    return function () {
      var k = keyOf();
      if (k === last) { var row = el.querySelector('.cxjob'); if (row) cb.cxJobPatch(row, jobView()); return; }
      last = k;
      var fid = el.contains(document.activeElement) ? document.activeElement.getAttribute('data-nav-id') : null;
      U.clear(el);
      build();
      if (fid) { var again = el.querySelector('[data-nav-id="' + fid + '"]') || el.querySelector('[data-nav]'); if (again) XS.nav.focus(again, false); }
    };
  }
  function badge() { return h('span.brand-badge.app-badge', [h('img', { src: XS.logoSrc || '/logo.jpg', alt: '' })]); }
  function progressRow() { return cb.cxJobRow(jobView()); }
  function btn(spec, id, icon, label, onClick) {
    var b = h(spec, { nav: id, html: U.icon(icon) + '<span>' + U.escapeHtml(label) + '</span>' });
    b.addEventListener('click', onClick);
    return b;
  }

  /* ---------- Settings → Updates card ---------- */
  function card() {
    var el = h('div.panel.app-card', { data: { navRegion: '' } });
    var paint = view(el, function () {
      var st = A.status;
      return [running(), A.job && A.job.id, st ? [st.installed, st.version, st.size, st.path].join('|') : '-'].join('#');
    }, function () {
      var st = A.status, busy = running(), installed = !!(st && st.installed);
      var chip = busy ? h('span.chip.sm.gold', { text: t('gameapp.installing') })
        : !st ? h('span.chip.sm', { text: '…' })
        : installed ? h('span.chip.sm.good', { html: U.icon('check') + '<span>' + U.escapeHtml(t('gameapp.installed')) + '</span>' })
        : h('span.chip.sm', { text: t('gameapp.notInstalled') });
      var facts = [];
      if (installed && st.version) facts.push(t('gameapp.version', { v: st.version }));
      if (installed && st.size) facts.push(U.fmtBytes(st.size));
      el.appendChild(h('div.app-top', [badge(), h('div.app-main', [
        h('div.app-head', [h('h3', { text: t('gameapp.title') }), chip]),
        h('p.dim.small', { text: t('gameapp.sub') }),
        facts.length ? h('p.mute.small.app-facts', { text: facts.join(' · ') }) : null
      ])]));
      if (busy) el.appendChild(progressRow());
      var acts = h('div.btn-row.app-acts');
      if (busy) {
        var c = btn('button.btn.sm.ghost', 'app-cancel', 'x', t('gameapp.cancel'), cancel);
        if (!A.job || !A.job.id) c.setAttribute('aria-disabled', 'true');
        acts.appendChild(c);
      } else {
        acts.appendChild(btn('button.btn.sm' + (installed ? '' : '.primary'), 'app-install', installed ? 'refresh' : 'download', installed ? t('gameapp.reinstall') : t('gameapp.install'), function () { install(); }));
      }
      if (st && st.path) acts.appendChild(h('span.mute.small.app-path', { text: st.path }));
      el.appendChild(acts);
    });
    subscribe(el, paint);
    paint();
    load(false).catch(function () { if (!A.status) { A.status = { installed: false, size: 0, version: '', path: '' }; A.at = 0; paint(); } });
    return el;
  }

  /* ---------- Discover: one-time hint on a PS5 without the app ---------- */
  function hintWanted() {
    var st = S.data.status || {};
    return st.platform === 'ps5' && !U.store.get(HINT_KEY, false);
  }
  function hint() {
    var host = h('div.app-hint-host');
    var paint = view(host, function () {
      var st = A.status, want = hintWanted();
      if (want && !st) load(false).catch(noop);
      return [want, running(), A.job && A.job.id, st ? st.installed : '-'].join('#');
    }, function () {
      var st = A.status, busy = running();
      if (!st || !(busy || (hintWanted() && !st.installed))) return;
      var box = h('div.app-hint' + (busy ? '.busy' : ''), { data: { navRegion: '' }, role: 'note' });
      box.appendChild(badge());
      var txt = h('div.grow', [h('div.app-hint-t', { text: t('gameapp.hintTitle') }), h('div.app-hint-d.dim', { text: t('gameapp.hintText') })]);
      box.appendChild(txt);
      if (busy) txt.appendChild(progressRow());
      else {
        box.appendChild(btn('button.btn.sm.primary', 'app-hint-install', 'download', t('gameapp.install'), function () { install(); }));
        var no = h('button.btn.sm.ghost', { nav: 'app-hint-dismiss', text: t('gameapp.notNow') });
        no.addEventListener('click', function () { U.store.set(HINT_KEY, true); emit(); });
        box.appendChild(no);
      }
      host.appendChild(box);
    });
    subscribe(host, paint);
    paint();
    return host;
  }

  XS.appInstall = { card: card, hint: hint, install: install, status: load };
})();
