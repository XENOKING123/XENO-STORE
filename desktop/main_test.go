package main

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"sync"
	"testing"
	"time"
)

// fakePS5 answers the store routes the Desktop relies on, like the store does: GET /api/pair says
// whether the Bearer token is paired, POST /api/pair redeems code 123456 for the token "fresh"
// (naming the device that made room when replaced is set), and anything else needs a paired
// token (403 not_paired otherwise).
type fakePS5 struct {
	mu       sync.Mutex
	tokens   map[string]bool
	replaced *string  // all slots taken: the device unpaired to make room
	seen     []string // "<method> <path> <token> <user agent>" per request
}

func (f *fakePS5) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	tok := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
	f.mu.Lock()
	f.seen = append(f.seen, r.Method+" "+r.URL.Path+" "+tok+" "+r.Header.Get("User-Agent"))
	paired := f.tokens[tok]
	f.mu.Unlock()
	switch {
	case r.Method == http.MethodGet && r.URL.Path == "/api/pair":
		writeJSON(w, 200, map[string]any{"ok": true, "port": ps5Port, "local": false, "linked": paired})
	case r.Method == http.MethodPost && r.URL.Path == "/api/pair":
		var in struct {
			Code string `json:"code"`
		}
		_ = json.NewDecoder(r.Body).Decode(&in)
		if in.Code != "123456" {
			writeJSON(w, 403, map[string]any{"ok": false, "error": "bad_code"})
			return
		}
		f.mu.Lock()
		f.tokens["fresh"] = true
		f.mu.Unlock()
		out := map[string]any{"ok": true, "token": "fresh", "paired": true}
		if f.replaced != nil {
			out["replaced"] = *f.replaced
		}
		writeJSON(w, 200, out)
	case !paired:
		writeJSON(w, 403, map[string]any{"ok": false, "error": "not_paired"})
	default:
		writeJSON(w, 200, map[string]any{"ok": true, "path": r.URL.Path})
	}
}

func (f *fakePS5) last() string { f.mu.Lock(); defer f.mu.Unlock(); return f.seen[len(f.seen)-1] }

func newDesktop(t *testing.T, ps5URL, token string) *app {
	a := &app{shares: map[string]string{}, client: &http.Client{Timeout: 5 * time.Second}}
	a.cfgPath = t.TempDir() + "/desktop.json"
	a.cfg = config{PS5: strings.TrimPrefix(ps5URL, "http://"), Token: token}
	return a
}

// do sends a request from the PC browser to the Desktop's own page.
func do(a *app, method, path, body string) *httptest.ResponseRecorder {
	var rd io.Reader
	if body != "" {
		rd = strings.NewReader(body)
	}
	r := httptest.NewRequest(method, "http://127.0.0.1:9096"+path, rd)
	if body != "" {
		r.Header.Set("Content-Type", "application/json")
	}
	w := httptest.NewRecorder()
	a.ServeHTTP(w, r)
	return w
}

func savedToken(t *testing.T, a *app) string {
	b, err := os.ReadFile(a.cfgPath)
	if err != nil {
		t.Fatalf("desktop.json was not saved: %v", err)
	}
	var c config
	if err := json.Unmarshal(b, &c); err != nil {
		t.Fatal(err)
	}
	return c.Token
}

// Unpaired on the PS5 (Settings → Pair a device): loading the store sends the user to this app's
// link page, which pairs again as XENO STORE Desktop, instead of proxying the dead token forever.
func TestUnpairedTokenLinksAgainFromTheDesktopPage(t *testing.T) {
	f := &fakePS5{tokens: map[string]bool{"good": true}}
	ps5 := httptest.NewServer(f)
	defer ps5.Close()
	a := newDesktop(t, ps5.URL, "dead")
	w := do(a, "GET", "/", "")
	if w.Code != http.StatusFound || w.Header().Get("Location") != "/__desktop/?unpaired=1" {
		t.Fatalf("GET / with an unpaired token: %d %q, want 302 to /__desktop/?unpaired=1", w.Code, w.Header().Get("Location"))
	}
	if a.cfg.Token != "" || savedToken(t, a) != "" {
		t.Fatalf("the dead token was kept: %q", a.cfg.Token)
	}
	if a.cfg.PS5 == "" {
		t.Fatal("the console was forgotten too")
	}
	if w := do(a, "GET", "/api/status", ""); w.Code != http.StatusFound || w.Header().Get("Location") != "/__desktop/" {
		t.Fatalf("without a token: %d %q, want 302 to /__desktop/", w.Code, w.Header().Get("Location"))
	}
	// the shell links again with this app's own name
	if w := do(a, "POST", "/__desktop/api/link", `{"code":"123456"}`); w.Code != 200 || a.cfg.Token != "fresh" {
		t.Fatalf("link: %d %s, token %q", w.Code, w.Body.String(), a.cfg.Token)
	}
	if got := f.last(); !strings.HasSuffix(got, " XENO-STORE-Desktop/"+version) {
		t.Fatalf("the shell paired as %q", got)
	}
}

// A paired token is proxied as before, and a PS5 that does not answer keeps the token.
func TestPairedTokenIsProxied(t *testing.T) {
	f := &fakePS5{tokens: map[string]bool{"good": true}}
	ps5 := httptest.NewServer(f)
	defer ps5.Close()
	a := newDesktop(t, ps5.URL, "good")
	if w := do(a, "GET", "/", ""); w.Code != 200 {
		t.Fatalf("GET /: %d %s", w.Code, w.Body.String())
	}
	if got, want := f.last(), "GET / good XENO-STORE-Desktop/"+version; got != want {
		t.Fatalf("the PS5 saw %q, want %q", got, want)
	}
	gone := httptest.NewServer(f)
	gone.Close()
	b := newDesktop(t, gone.URL, "good")
	if w := do(b, "GET", "/", ""); w.Code != http.StatusBadGateway || b.cfg.Token != "good" {
		t.Fatalf("PS5 not answering: %d, token %q", w.Code, b.cfg.Token)
	}
}

// Unpaired while the store is open, the store's own Link page takes a new code through the proxy:
// that token becomes this PC's, so the next requests carry it rather than the dead one.
func TestStoreLinkPageTokenBecomesThisPCs(t *testing.T) {
	f := &fakePS5{tokens: map[string]bool{"good": true}}
	ps5 := httptest.NewServer(f)
	defer ps5.Close()
	a := newDesktop(t, ps5.URL, "dead")
	if w := do(a, "GET", "/api/status", ""); w.Code != http.StatusForbidden {
		t.Fatalf("GET /api/status with a dead token: %d", w.Code)
	}
	if w := do(a, "POST", "/api/pair", `{"code":"000000"}`); w.Code != http.StatusForbidden || a.cfg.Token != "dead" {
		t.Fatalf("a wrong code: %d, token %q", w.Code, a.cfg.Token)
	}
	w := do(a, "POST", "/api/pair", `{"code":"123456"}`)
	if w.Code != 200 || !strings.Contains(w.Body.String(), `"token":"fresh"`) {
		t.Fatalf("POST /api/pair: %d %s (the page still gets its answer)", w.Code, w.Body.String())
	}
	if got := f.last(); !strings.HasSuffix(got, " XENO-STORE-Desktop/"+version) {
		t.Fatalf("the store's Link page paired as %q, not as XENO STORE Desktop", got)
	}
	if a.cfg.Token != "fresh" || savedToken(t, a) != "fresh" {
		t.Fatalf("token after the store's Link page: %q", a.cfg.Token)
	}
	if w := do(a, "GET", "/api/status", ""); w.Code != 200 {
		t.Fatalf("GET /api/status after linking again: %d %s", w.Code, w.Body.String())
	}
	if got, want := f.last(), "GET /api/status fresh XENO-STORE-Desktop/"+version; got != want {
		t.Fatalf("the PS5 saw %q, want %q", got, want)
	}
}

// All slots taken: the PS5 unpairs the device unused the longest, and the shell is told which.
func TestLinkSaysWhichDeviceMadeRoom(t *testing.T) {
	for _, tc := range []struct {
		replaced *string
		want     string
	}{{nil, `{"linked":true,"ok":true}`}, {new(string), `{"linked":true,"ok":true,"replaced":""}`}} {
		f := &fakePS5{tokens: map[string]bool{}, replaced: tc.replaced}
		ps5 := httptest.NewServer(f)
		a := newDesktop(t, ps5.URL, "")
		w := do(a, "POST", "/__desktop/api/link", `{"code":"123456"}`)
		ps5.Close()
		if got := strings.TrimSpace(w.Body.String()); w.Code != 200 || got != tc.want {
			t.Fatalf("link: %d %s, want %s", w.Code, got, tc.want)
		}
	}
	name := "iPhone Safari"
	f := &fakePS5{tokens: map[string]bool{}, replaced: &name}
	ps5 := httptest.NewServer(f)
	defer ps5.Close()
	a := newDesktop(t, ps5.URL, "")
	if w := do(a, "POST", "/__desktop/api/link", `{"code":"123456"}`); !strings.Contains(w.Body.String(), `"replaced":"iPhone Safari"`) {
		t.Fatalf("link: %d %s", w.Code, w.Body.String())
	}
}
