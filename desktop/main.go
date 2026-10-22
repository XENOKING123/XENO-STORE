// XENO STORE Desktop — the PC app for XENO STORE on a PS5.
//
// It finds the console on your network, links to it with the code the PS5 shows in a notification,
// then opens the full store in your PC browser through a local proxy that adds this PC's device
// token. "Send from this PC" serves a game file from the PC over the LAN and queues it on the PS5,
// so the console's segmented downloader pulls it at full network speed.
//
// One binary, Go standard library only. Run it and keep the window open while transfers run.
package main

import (
	"bytes"
	"context"
	"crypto/rand"
	_ "embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/http/httputil"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"sync"
	"time"
)

var version = "dev"

//go:embed shell.html
var shellHTML []byte

//go:embed logo.jpg
var logoJPG []byte

const (
	uiPort    = 9096 // the PC browser talks to this (127.0.0.1 only)
	sharePort = 9097 // the PS5 pulls shared files from this (LAN)
	ps5Port   = 9095
)

type config struct {
	PS5      string `json:"ps5"`   // "192.168.1.50:9095"
	Token    string `json:"token"` // device token from POST /api/pair
	ShareDir string `json:"shareDir"`
}

type app struct {
	mu        sync.Mutex
	cfg       config
	cfgPath   string
	shares    map[string]string // token -> absolute file path
	sharePort int
	client    *http.Client
}

func main() {
	a := &app{shares: map[string]string{}, client: &http.Client{Timeout: 20 * time.Second}}
	a.load()

	share, sp := listen("0.0.0.0", sharePort)
	a.sharePort = sp
	go func() { log.Fatal(http.Serve(share, http.HandlerFunc(a.serveShare))) }()

	ui, port := listen("127.0.0.1", uiPort)
	addr := fmt.Sprintf("http://127.0.0.1:%d/", port)
	fmt.Printf("XENO STORE Desktop %s\n\n  Open %s in your browser (it opens by itself).\n  Keep this window open while files are sent to your PS5.\n\n", version, addr)
	go func() { time.Sleep(400 * time.Millisecond); openBrowser(addr) }()
	log.Fatal(http.Serve(ui, a))
}

func listen(host string, prefer int) (net.Listener, int) {
	l, err := net.Listen("tcp", fmt.Sprintf("%s:%d", host, prefer))
	if err != nil {
		if l, err = net.Listen("tcp", host+":0"); err != nil {
			log.Fatalf("cannot listen on %s: %v", host, err)
		}
	}
	return l, l.Addr().(*net.TCPAddr).Port
}

func openBrowser(u string) {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "windows":
		cmd = exec.Command("rundll32", "url.dll,FileProtocolHandler", u)
	case "darwin":
		cmd = exec.Command("open", u)
	default:
		cmd = exec.Command("xdg-open", u)
	}
	_ = cmd.Start()
}

// ---- config ----------------------------------------------------------------

func (a *app) load() {
	dir, err := os.UserConfigDir()
	if err != nil {
		dir = "."
	}
	a.cfgPath = filepath.Join(dir, "XENO STORE", "desktop.json")
	if b, err := os.ReadFile(a.cfgPath); err == nil {
		_ = json.Unmarshal(b, &a.cfg)
	}
	if a.cfg.ShareDir == "" {
		if home, err := os.UserHomeDir(); err == nil {
			a.cfg.ShareDir = filepath.Join(home, "Downloads")
		}
	}
}

func (a *app) save() {
	_ = os.MkdirAll(filepath.Dir(a.cfgPath), 0o700)
	b, _ := json.MarshalIndent(a.cfg, "", "  ")
	_ = os.WriteFile(a.cfgPath, b, 0o600)
}

func (a *app) snapshot() config { a.mu.Lock(); defer a.mu.Unlock(); return a.cfg }

// ---- routing ---------------------------------------------------------------

func (a *app) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	// The UI listener is loopback-only, but a web page could still try to reach it: insist on our own host.
	if h := r.Host; !strings.HasPrefix(h, "127.0.0.1:") && !strings.HasPrefix(h, "localhost:") {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}
	// Only this app's own pages may change anything: refuse cross-site writes (the proxy adds the token).
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		if o := r.Header.Get("Origin"); o != "" && o != "http://"+r.Host {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		if s := r.Header.Get("Sec-Fetch-Site"); s != "" && s != "same-origin" && s != "none" {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
	}
	p := r.URL.Path
	switch {
	case p == "/__desktop" || p == "/__desktop/":
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("Cache-Control", "no-store")
		_, _ = w.Write(shellHTML)
	case p == "/__desktop/logo.jpg":
		w.Header().Set("Content-Type", "image/jpeg")
		_, _ = w.Write(logoJPG)
	case strings.HasPrefix(p, "/__desktop/api/"):
		a.api(w, r, strings.TrimPrefix(p, "/__desktop/api/"))
	default:
		c := a.snapshot()
		if c.PS5 == "" || c.Token == "" {
			http.Redirect(w, r, "/__desktop/", http.StatusFound)
			return
		}
		// The store page itself: when the PS5 says this PC's token was unpaired (Settings → Pair a
		// device), link again from this app's own page rather than sending a dead token forever.
		if p == "/" && r.Method == http.MethodGet && a.unpaired(c) {
			a.dropToken(c)
			http.Redirect(w, r, "/__desktop/?unpaired=1", http.StatusFound)
			return
		}
		a.proxy(c).ServeHTTP(w, r)
	}
}

// unpaired is true only when the PS5 answers that c.Token is no longer paired (not when it does not answer).
func (a *app) unpaired(c config) bool {
	var info map[string]any
	if st, err := a.ps5(c, "GET", "/api/pair", nil, &info); err != nil || st != http.StatusOK {
		return false
	}
	linked, ok := info["linked"].(bool)
	return ok && !linked
}

// dropToken forgets c.Token, unless another console or token was picked meanwhile.
func (a *app) dropToken(c config) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.cfg.PS5 == c.PS5 && a.cfg.Token == c.Token {
		a.cfg.Token = ""
		a.save()
	}
}

// keepToken makes tok this PC's token for the console c points at, unless another one was picked meanwhile.
func (a *app) keepToken(c config, tok string) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.cfg.PS5 == c.PS5 {
		a.cfg.Token = tok
		a.save()
	}
}

// proxy forwards the store UI and API to the PS5 with this PC's device token. The store's gate wants
// the Host and Origin of the console itself, so both are rewritten.
func (a *app) proxy(c config) http.Handler {
	target := &url.URL{Scheme: "http", Host: c.PS5}
	rp := httputil.NewSingleHostReverseProxy(target)
	base := rp.Director
	rp.Director = func(r *http.Request) {
		base(r)
		r.Host = c.PS5
		if r.Header.Get("Origin") != "" {
			r.Header.Set("Origin", "http://"+c.PS5)
		}
		r.Header.Del("Referer")
		r.Header.Set("Authorization", "Bearer "+c.Token)
		r.Header.Set("User-Agent", "XENO-STORE-Desktop/"+version) // the name Settings → Pair a device shows
	}
	// Unpaired while the store is open, the store's own Link page takes a new code: the token it
	// hands out becomes this PC's, or every later request would still carry the dead one.
	rp.ModifyResponse = func(res *http.Response) error {
		if res.Request.Method != http.MethodPost || res.Request.URL.Path != "/api/pair" || res.StatusCode != http.StatusOK {
			return nil
		}
		b, err := io.ReadAll(res.Body)
		res.Body.Close()
		if err != nil {
			return err
		}
		res.Body = io.NopCloser(bytes.NewReader(b))
		var out struct {
			Token string `json:"token"`
		}
		if json.Unmarshal(b, &out) == nil && out.Token != "" {
			a.keepToken(c, out.Token)
		}
		return nil
	}
	rp.ErrorHandler = func(w http.ResponseWriter, r *http.Request, err error) {
		writeJSON(w, http.StatusBadGateway, map[string]any{"ok": false, "error": "ps5_unreachable",
			"message": "Your PS5 is not answering on " + c.PS5 + ". Is XENO STORE running on it?"})
	}
	return rp
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func fail(w http.ResponseWriter, status int, code, msg string) {
	writeJSON(w, status, map[string]any{"ok": false, "error": code, "message": msg})
}

// ps5 calls the console's API directly (not through the browser).
func (a *app) ps5(c config, method, path string, body any, out any) (int, error) {
	var rd io.Reader
	if body != nil {
		b, _ := json.Marshal(body)
		rd = bytes.NewReader(b)
	}
	req, err := http.NewRequest(method, "http://"+c.PS5+path, rd)
	if err != nil {
		return 0, err
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	req.Header.Set("User-Agent", "XENO-STORE-Desktop/"+version) // the name Settings → Pair a device shows
	if c.Token != "" {
		req.Header.Set("Authorization", "Bearer "+c.Token)
	}
	res, err := a.client.Do(req)
	if err != nil {
		return 0, err
	}
	defer res.Body.Close()
	b, _ := io.ReadAll(io.LimitReader(res.Body, 1<<20))
	if out != nil {
		_ = json.Unmarshal(b, out)
	}
	return res.StatusCode, nil
}

func (a *app) api(w http.ResponseWriter, r *http.Request, name string) {
	var in map[string]any
	if r.Method == http.MethodPost {
		_ = json.NewDecoder(io.LimitReader(r.Body, 1<<16)).Decode(&in)
	}
	str := func(k string) string { s, _ := in[k].(string); return strings.TrimSpace(s) }
	c := a.snapshot()
	switch name {
	case "state":
		linked := false
		reachable := false
		if c.PS5 != "" {
			var info map[string]any
			if st, err := a.ps5(c, "GET", "/api/pair", nil, &info); err == nil && st == 200 {
				reachable = true
				linked, _ = info["linked"].(bool)
			}
		}
		writeJSON(w, 200, map[string]any{"ok": true, "version": version, "ps5": c.PS5, "reachable": reachable,
			"linked": linked && c.Token != "", "shareDir": c.ShareDir, "os": runtime.GOOS})
	case "discover":
		writeJSON(w, 200, map[string]any{"ok": true, "consoles": discover()})
	case "select":
		host := str("host")
		if !strings.Contains(host, ":") {
			host = fmt.Sprintf("%s:%d", host, ps5Port)
		}
		if _, _, err := net.SplitHostPort(host); err != nil || host == "" {
			fail(w, 400, "bad_host", "Type the PS5's IP address, like 192.168.1.50")
			return
		}
		probe := config{PS5: host}
		var info map[string]any
		if st, err := a.ps5(probe, "GET", "/api/pair", nil, &info); err != nil || st != 200 {
			fail(w, 502, "ps5_unreachable", "No XENO STORE answered on "+host+". Start XENO STORE on the PS5 and try again.")
			return
		}
		a.mu.Lock()
		if a.cfg.PS5 != host {
			a.cfg.Token = ""
		}
		a.cfg.PS5 = host
		a.save()
		a.mu.Unlock()
		writeJSON(w, 200, map[string]any{"ok": true, "ps5": host})
	case "show":
		var out map[string]any
		st, err := a.ps5(c, "POST", "/api/pair/show", map[string]any{}, &out)
		if err != nil {
			fail(w, 502, "ps5_unreachable", err.Error())
			return
		}
		writeJSON(w, st, out)
	case "link":
		var out map[string]any
		st, err := a.ps5(config{PS5: c.PS5}, "POST", "/api/pair", map[string]any{"code": str("code")}, &out)
		if err != nil {
			fail(w, 502, "ps5_unreachable", err.Error())
			return
		}
		if tok, _ := out["token"].(string); st == 200 && tok != "" {
			a.mu.Lock()
			a.cfg.Token = tok
			a.save()
			a.mu.Unlock()
			res := map[string]any{"ok": true, "linked": true}
			if gone, ok := out["replaced"].(string); ok { // all slots were taken: the device that made room ("" = no name)
				res["replaced"] = gone
			}
			writeJSON(w, 200, res)
			return
		}
		writeJSON(w, st, out)
	case "forget":
		a.mu.Lock()
		a.cfg.PS5, a.cfg.Token = "", ""
		a.save()
		a.mu.Unlock()
		writeJSON(w, 200, map[string]any{"ok": true})
	case "files":
		dir := str("dir")
		if dir == "" {
			dir = c.ShareDir
		}
		list, err := listGameFiles(dir)
		if err != nil {
			fail(w, 400, "bad_dir", err.Error())
			return
		}
		a.mu.Lock()
		a.cfg.ShareDir = dir
		a.save()
		a.mu.Unlock()
		writeJSON(w, 200, map[string]any{"ok": true, "dir": dir, "parent": filepath.Dir(dir), "entries": list})
	case "send":
		a.send(w, c, str("path"))
	default:
		fail(w, 404, "not_found", "unknown desktop route")
	}
}

// ---- discovery ---------------------------------------------------------------

// discover looks for XENO STORE on :9095 across this PC's IPv4 /24 networks, plus xeno.local.
func discover() []map[string]any {
	var cands []string
	seen := map[string]bool{}
	add := func(ip string) {
		if !seen[ip] {
			seen[ip] = true
			cands = append(cands, ip)
		}
	}
	if ips, err := net.DefaultResolver.LookupHost(context.Background(), "xeno.local"); err == nil {
		for _, ip := range ips {
			if net.ParseIP(ip).To4() != nil {
				add(ip)
			}
		}
	}
	ifaces, _ := net.Interfaces()
	for _, ifc := range ifaces {
		if ifc.Flags&net.FlagUp == 0 || ifc.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, _ := ifc.Addrs()
		for _, ad := range addrs {
			ipn, ok := ad.(*net.IPNet)
			if !ok {
				continue
			}
			v4 := ipn.IP.To4()
			if v4 == nil || !v4.IsPrivate() {
				continue
			}
			for i := 1; i < 255; i++ {
				add(fmt.Sprintf("%d.%d.%d.%d", v4[0], v4[1], v4[2], i))
			}
		}
	}
	var mu sync.Mutex
	var found []map[string]any
	sem := make(chan struct{}, 96)
	var wg sync.WaitGroup
	cl := &http.Client{Timeout: 1500 * time.Millisecond}
	for _, ip := range cands {
		wg.Add(1)
		sem <- struct{}{}
		go func(ip string) {
			defer wg.Done()
			defer func() { <-sem }()
			hostport := fmt.Sprintf("%s:%d", ip, ps5Port)
			cn, err := net.DialTimeout("tcp", hostport, 350*time.Millisecond)
			if err != nil {
				return
			}
			cn.Close()
			res, err := cl.Get("http://" + hostport + "/api/pair")
			if err != nil {
				return
			}
			defer res.Body.Close()
			var info map[string]any
			if json.NewDecoder(io.LimitReader(res.Body, 1<<16)).Decode(&info) != nil || res.StatusCode != 200 {
				return
			}
			if _, ok := info["port"]; !ok {
				return
			}
			mu.Lock()
			found = append(found, map[string]any{"host": hostport, "ip": ip})
			mu.Unlock()
		}(ip)
	}
	wg.Wait()
	sort.Slice(found, func(i, j int) bool { return found[i]["host"].(string) < found[j]["host"].(string) })
	return found
}

// ---- send from this PC -----------------------------------------------------------

var gameExt = map[string]string{".pkg": "PKG", ".fpkg": "FPKG", ".ffpkg": "FFPKG", ".ffpfsc": "FFPFSC", ".ffpfs": "FFPFS",
	".exfat": "exFAT", ".zip": "ZIP", ".rar": "RAR", ".7z": "7z"}

func listGameFiles(dir string) ([]map[string]any, error) {
	ents, err := os.ReadDir(dir)
	if err != nil {
		return nil, errors.New("cannot open that folder")
	}
	out := []map[string]any{}
	for _, e := range ents {
		name := e.Name()
		if strings.HasPrefix(name, ".") {
			continue
		}
		full := filepath.Join(dir, name)
		if e.IsDir() {
			out = append(out, map[string]any{"name": name, "path": full, "dir": true})
			continue
		}
		f, ok := gameExt[strings.ToLower(filepath.Ext(name))]
		if !ok {
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		out = append(out, map[string]any{"name": name, "path": full, "size": info.Size(), "format": f})
	}
	sort.Slice(out, func(i, j int) bool {
		di, dj := out[i]["dir"] == true, out[j]["dir"] == true
		if di != dj {
			return di
		}
		return strings.ToLower(out[i]["name"].(string)) < strings.ToLower(out[j]["name"].(string))
	})
	return out, nil
}

// lanIP is this PC's address on the route to the PS5.
func lanIP(ps5 string) string {
	cn, err := net.Dial("udp", ps5)
	if err != nil {
		return ""
	}
	defer cn.Close()
	return cn.LocalAddr().(*net.UDPAddr).IP.String()
}

func (a *app) send(w http.ResponseWriter, c config, path string) {
	if c.PS5 == "" || c.Token == "" {
		fail(w, 409, "not_linked", "Link to your PS5 first.")
		return
	}
	st, err := os.Stat(path)
	if err != nil || st.IsDir() {
		fail(w, 400, "bad_file", "That file is not there any more.")
		return
	}
	format, ok := gameExt[strings.ToLower(filepath.Ext(path))]
	if !ok {
		fail(w, 400, "bad_file", "Only game files can be sent (.pkg, .ffpkg, .ffpfsc, .exfat, .zip, .rar, .7z).")
		return
	}
	ip := lanIP(c.PS5)
	if ip == "" {
		fail(w, 502, "no_route", "This PC has no network route to the PS5.")
		return
	}
	tb := make([]byte, 16)
	_, _ = rand.Read(tb)
	tok := hex.EncodeToString(tb)
	a.mu.Lock()
	a.shares[tok] = path
	a.mu.Unlock()
	name := filepath.Base(path)
	link := fmt.Sprintf("http://%s:%d/__share/%s/%s", ip, a.sharePort, tok, url.PathEscape(name))
	title := strings.TrimSuffix(name, filepath.Ext(name))
	var out map[string]any
	code, err := a.ps5(c, "POST", "/api/queue", map[string]any{"url": link, "filename": name, "format": format, "title": title}, &out)
	if err != nil {
		fail(w, 502, "ps5_unreachable", err.Error())
		return
	}
	if code != 200 {
		writeJSON(w, code, out)
		return
	}
	log.Printf("sending %s (%d bytes) to the PS5", name, st.Size())
	writeJSON(w, 200, map[string]any{"ok": true, "job": out["job"], "url": link})
}

// serveShare answers the PS5's segmented downloader: Range requests on the registered file only.
func (a *app) serveShare(w http.ResponseWriter, r *http.Request) {
	parts := strings.SplitN(strings.TrimPrefix(r.URL.Path, "/__share/"), "/", 2)
	if !strings.HasPrefix(r.URL.Path, "/__share/") || len(parts) != 2 {
		http.NotFound(w, r)
		return
	}
	a.mu.Lock()
	path, ok := a.shares[parts[0]]
	a.mu.Unlock()
	if !ok {
		http.NotFound(w, r)
		return
	}
	f, err := os.Open(path)
	if err != nil {
		http.Error(w, "gone", http.StatusGone)
		return
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		http.Error(w, "gone", http.StatusGone)
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	http.ServeContent(w, r, filepath.Base(path), st.ModTime(), f)
}
