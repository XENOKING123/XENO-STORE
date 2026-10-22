/* XENO STORE mock — Link to PS5 (pairing) routes for the web UI's first page on a PC / phone.
 * The mock answers as the console itself ("local") until POST /api/__mock/remote {"on":true} makes it
 * behave like a LAN browser; the code is always 424242 and is printed to the log on "show". */
export function register({ route, state }) {
  const { ApiError, log } = state;
  const CODE = '424242';
  const TOKEN = 'a'.repeat(64);
  let remote = false;
  let shownAt = 0;
  const bearer = (req) => String((req && req.headers && req.headers.authorization) || '').replace(/^Bearer\s+/i, '');
  route('POST', /^\/api\/__mock\/remote$/, (q, body) => { remote = !!(body && body.on); return { ok: true, remote }; });
  route('GET', /^\/api\/pair$/, (q, body, mm, req) => ({
    ok: true, url: `http://192.168.1.50:${state.PORT}/`, ip: '192.168.1.50', port: 9095, pinRequired: false,
    code: remote ? '' : CODE, cooldown: 0, local: !remote, linked: bearer(req) === TOKEN
  }));
  route('POST', /^\/api\/pair\/show$/, () => {
    if (Date.now() - shownAt < 3000) throw Object.assign(new ApiError(429, 'cooldown', 'Wait a moment before showing the code again.'), { retryAfter: 3 });
    shownAt = Date.now();
    log('info', `pair: code ${CODE} shown on the "console"`);
    return { ok: true, shown: true, retryAfter: 10 };
  });
  route('POST', /^\/api\/pair$/, (q, body) => {
    if (String((body && body.code) || '') !== CODE) throw new ApiError(403, 'bad_code', 'Pairing failed. Check the code shown on the console.');
    return { ok: true, token: TOKEN, paired: true };
  });
  /* Settings → Pair a device: the paired list (this browser is the iPhone once linked), unpair one / all others.
   * Like the console, only the console itself or a linked browser may look or unpair. */
  const now = Math.floor(Date.now() / 1000);
  let devices = [
    { id: '3f9a1c2b7d4e5a60', name: 'iPhone Safari', firstSeen: now - 9 * 86400, lastSeen: now - 40, token: TOKEN },
    { id: 'a07c5e12b9d84f31', name: 'Windows Chrome', firstSeen: now - 30 * 86400, lastSeen: now - 3 * 3600 },
    { id: '5d2e8b40c1f7a963', name: 'XENO STORE Desktop', firstSeen: now - 12 * 86400, lastSeen: now - 4 * 86400 },
    { id: 'c48b0f6e2a9d1735', name: '', firstSeen: 0, lastSeen: 0 }
  ];
  const caller = (req) => { if (remote && bearer(req) !== TOKEN) throw new ApiError(403, 'not_paired', 'Pair this device first.'); return bearer(req); };
  const listing = (self) => ({ devices: devices.map((d) => ({ id: d.id, name: d.name, firstSeen: d.firstSeen, lastSeen: d.lastSeen, current: !!d.token && d.token === self })), max: 16 });
  route('GET', /^\/api\/pair\/devices$/, (q, body, mm, req) => Object.assign({ ok: true }, listing(caller(req))));
  route('POST', /^\/api\/pair\/unpair$/, (q, body, mm, req) => {
    const self = caller(req);
    const id = String((body && body.id) || '');
    if (!id) throw new ApiError(400, 'bad_request', 'id is required');
    if (!devices.some((d) => d.id === id)) throw new ApiError(404, 'not_found', 'No paired device has that id.');
    devices = devices.filter((d) => d.id !== id);
    return Object.assign({ ok: true, unpaired: true }, listing(self));
  });
  route('POST', /^\/api\/pair\/unpair-others$/, (q, body, mm, req) => {
    const self = caller(req);
    const before = devices.length;
    devices = devices.filter((d) => d.token && d.token === self);
    return Object.assign({ ok: true, unpaired: before - devices.length }, listing(self));
  });
}
