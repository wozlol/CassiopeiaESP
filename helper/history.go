// Reading the history of tmux sessions on this computer, from the command line or a page in the browser.
// The page listens on this computer only and needs the token in the link the helper prints.
package main

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"fmt"
	"html"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
)

const webAddr = "127.0.0.1:2224"

type sessInfo struct {
	Name     string
	Windows  string
	Attached string
}

func listSessions() ([]sessInfo, error) {
	tm := tmuxPath()
	if tm == "" {
		return nil, fmt.Errorf("tmux is not installed")
	}
	out, err := exec.Command(tm, "list-sessions", "-F", "#{session_name}\t#{session_windows}\t#{session_attached}").Output()
	if err != nil {
		return nil, nil // no server running means no sessions
	}
	var res []sessInfo
	for _, l := range strings.Split(strings.TrimSpace(string(out)), "\n") {
		f := strings.Split(l, "\t")
		if len(f) == 3 && nameRe.MatchString(f[0]) {
			res = append(res, sessInfo{f[0], f[1], f[2]})
		}
	}
	return res, nil
}

func capturePane(name string, lines int) (string, error) {
	tm := tmuxPath()
	if tm == "" {
		return "", fmt.Errorf("tmux is not installed")
	}
	if !nameRe.MatchString(name) {
		return "", fmt.Errorf("bad session name")
	}
	if lines < 1 || lines > 50000 {
		lines = 2000
	}
	out, err := exec.Command(tm, "capture-pane", "-p", "-J", "-t", name+":", "-S", "-"+strconv.Itoa(lines)).Output()
	if err != nil {
		return "", fmt.Errorf("no such session")
	}
	return strings.TrimRight(string(out), "\n ") + "\n", nil
}

// a11helper history [NAME [LINES]]  prints the session list, or the history of one session.
func historyCLI(args []string) {
	if len(args) == 0 {
		ss, err := listSessions()
		if err != nil {
			fatal(err)
		}
		if len(ss) == 0 {
			fmt.Println("no tmux sessions running")
			return
		}
		fmt.Println("Sessions, read one with: a11helper history NAME")
		for _, s := range ss {
			fmt.Printf("  %-20s %s windows, %s attached\n", s.Name, s.Windows, s.Attached)
		}
		return
	}
	n := 2000
	if len(args) > 1 {
		if v, err := strconv.Atoi(args[1]); err == nil {
			n = v
		}
	}
	text, err := capturePane(args[0], n)
	if err != nil {
		fatal(err)
	}
	fmt.Print(text)
}

func webToken(dir string) string {
	p := filepath.Join(dir, "web_token")
	if b, err := os.ReadFile(p); err == nil && len(strings.TrimSpace(string(b))) >= 16 {
		return strings.TrimSpace(string(b))
	}
	r := make([]byte, 12)
	rand.Read(r)
	t := hex.EncodeToString(r)
	os.WriteFile(p, []byte(t+"\n"), 0600)
	return t
}

const pageHead = `<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>a11helper history</title>
<style>
:root{color-scheme:dark;--bg:#000;--fg:#39ff14;--dim:#1c7a0c}
body{background:var(--bg);color:var(--fg);font:15px/1.35 ui-monospace,Menlo,monospace;margin:0;padding:14px 16px}
a{color:var(--fg)}h1{font-size:17px;margin:0 0 10px}
pre{white-space:pre-wrap;word-break:break-word;margin:10px 0 60px}
.bar{position:sticky;top:0;background:var(--bg);padding:6px 0;border-bottom:1px solid var(--dim)}
.dim{color:var(--dim)}input,select,button{background:#001a00;color:var(--fg);border:1px solid var(--dim);font:inherit;padding:2px 6px}
li{margin:4px 0}
</style>`

func startWeb(dir string) {
	tok := webToken(dir)
	guard := func(h http.HandlerFunc) http.HandlerFunc {
		return func(w http.ResponseWriter, r *http.Request) {
			// Only this computer, only with the token, and only under its own name so a web page elsewhere cannot talk to it.
			if r.Host != webAddr && r.Host != "localhost:2224" {
				http.Error(w, "not allowed", 403)
				return
			}
			if subtle.ConstantTimeCompare([]byte(r.URL.Query().Get("t")), []byte(tok)) != 1 {
				http.Error(w, "Open the link the helper printed, it includes the token.", 403)
				return
			}
			w.Header().Set("Cache-Control", "no-store")
			w.Header().Set("X-Content-Type-Options", "nosniff")
			w.Header().Set("Referrer-Policy", "no-referrer")
			w.Header().Set("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'")
			h(w, r)
		}
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/", guard(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" {
			http.NotFound(w, r)
			return
		}
		ss, err := listSessions()
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		fmt.Fprint(w, pageHead, "<h1>tmux sessions on this computer</h1>")
		if err != nil {
			fmt.Fprintf(w, "<p>%s</p>", html.EscapeString(err.Error()))
			return
		}
		if len(ss) == 0 {
			fmt.Fprint(w, "<p class=dim>No tmux sessions are running.</p>")
		}
		fmt.Fprint(w, "<ul>")
		for _, s := range ss {
			fmt.Fprintf(w, `<li><a href="/s/%s?t=%s">%s</a> <span class=dim>%s windows, %s attached</span>`, s.Name, tok, html.EscapeString(s.Name), s.Windows, s.Attached)
		}
		fmt.Fprint(w, "</ul><p class=dim>Read only. Programs that take over the whole screen keep their own display, tmux has no lines to give back for those.</p>")
	}))
	mux.HandleFunc("/raw/", guard(func(w http.ResponseWriter, r *http.Request) {
		n, _ := strconv.Atoi(r.URL.Query().Get("n"))
		text, err := capturePane(strings.TrimPrefix(r.URL.Path, "/raw/"), n)
		if err != nil {
			http.Error(w, err.Error(), 404)
			return
		}
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		if r.URL.Query().Get("dl") != "" {
			w.Header().Set("Content-Disposition", `attachment; filename="`+strings.TrimPrefix(r.URL.Path, "/raw/")+`.txt"`)
		}
		fmt.Fprint(w, text)
	}))
	mux.HandleFunc("/s/", guard(func(w http.ResponseWriter, r *http.Request) {
		name := strings.TrimPrefix(r.URL.Path, "/s/")
		if !nameRe.MatchString(name) {
			http.NotFound(w, r)
			return
		}
		n := 2000
		if v, err := strconv.Atoi(r.URL.Query().Get("n")); err == nil && v > 0 && v <= 50000 {
			n = v
		}
		text, err := capturePane(name, n)
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		fmt.Fprint(w, pageHead)
		fmt.Fprintf(w, `<div class=bar><a href="/?t=%s">all sessions</a> &nbsp; <b>%s</b> &nbsp;
lines <select id=n onchange="go()">`, tok, html.EscapeString(name))
		for _, o := range []int{500, 2000, 5000, 20000} {
			sel := ""
			if o == n {
				sel = " selected"
			}
			fmt.Fprintf(w, "<option%s>%d</option>", sel, o)
		}
		fmt.Fprintf(w, `</select> &nbsp; <label><input type=checkbox id=live> live</label> &nbsp;
<input id=f placeholder="filter lines" size=18> &nbsp; <a href="/raw/%s?t=%s&n=%d&dl=1">save as text</a></div>`, name, tok, n)
		if err != nil {
			fmt.Fprintf(w, "<p>%s</p>", html.EscapeString(err.Error()))
			return
		}
		fmt.Fprintf(w, "<pre id=t>%s</pre>", html.EscapeString(text))
		fmt.Fprintf(w, `<script>
const T=%q,NAME=%q;let raw=document.getElementById('t').textContent;
function go(){location.href='/s/'+NAME+'?t='+T+'&n='+document.getElementById('n').value}
function show(){const q=document.getElementById('f').value.toLowerCase();
 document.getElementById('t').textContent=q?raw.split('\n').filter(l=>l.toLowerCase().includes(q)).join('\n'):raw}
document.getElementById('f').oninput=show;
window.scrollTo(0,document.body.scrollHeight);
setInterval(async()=>{if(!document.getElementById('live').checked)return;
 const atEnd=innerHeight+scrollY>=document.body.scrollHeight-40;
 const r=await fetch('/raw/'+NAME+'?t='+T+'&n='+document.getElementById('n').value);
 if(r.ok){raw=await r.text();show();if(atEnd)window.scrollTo(0,document.body.scrollHeight)}},3000);
</script>`, tok, name)
	}))
	l, err := net.Listen("tcp", webAddr)
	if err != nil {
		fmt.Printf("History page is off, could not use %s: %v\n", webAddr, err)
		return
	}
	go http.Serve(l, mux)
	fmt.Printf("History page, this computer only: http://%s/?t=%s\n", webAddr, tok)
}

func webLink(dir string) string { return fmt.Sprintf("http://%s/?t=%s", webAddr, webToken(dir)) }
