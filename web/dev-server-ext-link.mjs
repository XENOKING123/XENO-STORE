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
}
