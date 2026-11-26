// a11helper: the PC side of the CassiopeiaESP handheld. One program, nothing to install, no system SSH server needed.
// It listens for the handheld over SSH, lets only paired devices in, and only offers a short list of actions:
// attach to a tmux session (created if it does not exist), list sessions, read a session's scrollback, end a session.
// Nothing else is reachable, no plain shell, no port forwarding.
package main

import (
	"bufio"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"crypto/sha512"
	"crypto/subtle"
	"encoding/base64"
	"encoding/json"
	"encoding/pem"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/creack/pty"
	"golang.org/x/crypto/ssh"
)

const version = "0.5"

var hostPub ssh.PublicKey
var devStore *Store
var dropFlag *string
var startDir string // folder new sessions start in, the home folder unless -start says otherwise

var nameRe = regexp.MustCompile(`^[A-Za-z0-9_.-]{1,40}$`)
var deviceNameRe = regexp.MustCompile(`^[A-Za-z0-9 _-]{1,30}$`)

// ---------- paired devices ----------

type Device struct {
	Name  string `json:"name"`
	Key   string `json:"key"` // authorized_keys format
	Added string `json:"added"`
}

type Store struct {
	mu      sync.Mutex
	path    string
	Devices []Device `json:"devices"`
}

func loadStore(path string) *Store {
	s := &Store{path: path}
	if b, err := os.ReadFile(path); err == nil {
		json.Unmarshal(b, s)
	}
	return s
}

func (s *Store) save() {
	b, _ := json.MarshalIndent(s, "", "  ")
	os.WriteFile(s.path, b, 0600)
}

func (s *Store) find(key ssh.PublicKey) *Device {
	s.mu.Lock()
	defer s.mu.Unlock()
	want := key.Marshal()
	for i := range s.Devices {
		k, _, _, _, err := ssh.ParseAuthorizedKey([]byte(s.Devices[i].Key))
		if err == nil && subtle.ConstantTimeCompare(k.Marshal(), want) == 1 {
			return &s.Devices[i]
		}
	}
	return nil
}

func (s *Store) add(name string, key ssh.PublicKey) {
	s.mu.Lock()
	defer s.mu.Unlock()
	line := strings.TrimSpace(string(ssh.MarshalAuthorizedKey(key)))
	for i := range s.Devices { // same name replaces the old key
		if s.Devices[i].Name == name {
			s.Devices[i].Key = line
			s.save()
			return
		}
	}
	s.Devices = append(s.Devices, Device{Name: name, Key: line, Added: time.Now().Format(time.RFC3339)})
	s.save()
}

func (s *Store) revoke(name string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	for i := range s.Devices {
		if s.Devices[i].Name == name {
			s.Devices = append(s.Devices[:i], s.Devices[i+1:]...)
			s.save()
			return true
		}
	}
	return false
}

// ---------- pairing ----------
// A handheld asks to pair, both screens show the same 4 digit code, and the person at this computer types y to allow it.
// The code is made from this computer's host key and the handheld's key, so someone in the middle would show a different code.

type pendingPair struct {
	name   string
	code   string
	answer chan bool
}

var (
	pendMu  sync.Mutex
	pending *pendingPair
)

func pairCode(host ssh.PublicKey, dev ssh.PublicKey) string {
	fp := sha256.Sum256(host.Marshal())
	h := sha512.Sum512(append(fp[:], dev.Marshal()...))
	n := (uint32(h[0])<<24 | uint32(h[1])<<16 | uint32(h[2])<<8 | uint32(h[3])) % 10000
	return fmt.Sprintf("%04d", n)
}

// askToPair blocks until the person answers or 90 seconds pass.
func askToPair(name string, host, dev ssh.PublicKey) bool {
	pendMu.Lock()
	if pending != nil {
		pendMu.Unlock()
		return false // one request at a time
	}
	p := &pendingPair{name: name, code: pairCode(host, dev), answer: make(chan bool, 1)}
	pending = p
	pendMu.Unlock()
	logMu.Lock()
	fmt.Printf("\a\n------------------------------------------------------------\n")
	fmt.Printf(" The handheld %q wants to pair with this computer.\n", name)
	fmt.Printf(" Its screen should show this code:  %s\n", p.code)
	fmt.Printf(" If it matches, type y and press Enter. Otherwise type n.\n")
	fmt.Printf("------------------------------------------------------------\n")
	logMu.Unlock()
	var ok bool
	select {
	case ok = <-p.answer:
	case <-time.After(90 * time.Second):
		logf("pairing request timed out")
	}
	pendMu.Lock()
	pending = nil
	pendMu.Unlock()
	return ok
}

func answerPending(yes bool) bool {
	pendMu.Lock()
	p := pending
	pendMu.Unlock()
	if p == nil {
		return false
	}
	select {
	case p.answer <- yes:
	default:
	}
	return true
}

// ---------- finding the handheld's way here ----------
// The handheld sends "A11?" to the local network. This answers with the SSH port and this computer's name.

func discovery(port string) {
	pc, err := net.ListenPacket("udp4", ":2223")
	if err != nil {
		logf("discovery is off, could not use UDP port 2223: %v", err)
		return
	}
	host, _ := os.Hostname()
	host = strings.NewReplacer(" ", "-", "\n", "").Replace(host)
	buf := make([]byte, 64)
	for {
		n, from, err := pc.ReadFrom(buf)
		if err != nil {
			return
		}
		if strings.TrimSpace(string(buf[:n])) == "A11?" {
			pc.WriteTo([]byte(fmt.Sprintf("A11HELPER 1 %s %s", port, host)), from)
		}
	}
}

// ---------- host key ----------

func hostKey(dir string) ssh.Signer {
	path := filepath.Join(dir, "host_key")
	if b, err := os.ReadFile(path); err == nil {
		if s, err := ssh.ParsePrivateKey(b); err == nil {
			return s
		}
	}
	_, priv, _ := ed25519.GenerateKey(rand.Reader)
	blk, err := ssh.MarshalPrivateKey(priv, "a11helper host key")
	if err != nil {
		fatal(err)
	}
	if err := os.WriteFile(path, pem.EncodeToMemory(blk), 0600); err != nil {
		fatal(err)
	}
	s, _ := ssh.NewSignerFromKey(priv)
	return s
}

// Short code for comparing the host key by eye on both screens: first 4 bytes of the SHA 256 of the key blob.
func shortCode(pub ssh.PublicKey) string {
	h := sha256.Sum256(pub.Marshal())
	return fmt.Sprintf("%02X%02X-%02X%02X", h[0], h[1], h[2], h[3])
}

// ---------- ssh server ----------

type ptyReq struct {
	Term string
	Cols uint32
	Rows uint32
	W, H uint32
	Mode string
}

type winch struct{ Cols, Rows, W, H uint32 }

func serve(l net.Listener, cfg *ssh.ServerConfig, store *Store) {
	for {
		nc, err := l.Accept()
		if err != nil {
			return
		}
		go handleConn(nc, cfg, store)
	}
}

func handleConn(nc net.Conn, cfg *ssh.ServerConfig, store *Store) {
	nc.SetDeadline(time.Now().Add(20 * time.Second)) // finish logging in within 20 seconds
	sc, chans, reqs, err := ssh.NewServerConn(nc, cfg)
	if err != nil {
		nc.Close()
		return
	}
	nc.SetDeadline(time.Time{})
	defer sc.Close()
	go ssh.DiscardRequests(reqs)
	dev := sc.Permissions.Extensions["device"]
	pairOnly := sc.Permissions.Extensions["pairing"] == "1"
	logf("%s connected from %s", dev, nc.RemoteAddr())
	for nch := range chans {
		if nch.ChannelType() != "session" {
			nch.Reject(ssh.UnknownChannelType, "only sessions")
			continue
		}
		ch, creqs, err := nch.Accept()
		if err != nil {
			continue
		}
		go session(ch, creqs, dev, pairOnly)
	}
	logf("%s disconnected", dev)
}

func tmuxPath() string {
	p, err := exec.LookPath("tmux")
	if err != nil {
		return ""
	}
	return p
}

func exitStatus(ch ssh.Channel, code int) {
	ch.SendRequest("exit-status", false, ssh.Marshal(struct{ Code uint32 }{uint32(code)}))
}

func session(ch ssh.Channel, reqs <-chan *ssh.Request, dev string, pairOnly bool) {
	defer ch.Close()
	var pr *ptyReq
	var ptmx *os.File
	var mu sync.Mutex
	started := false
	for req := range reqs {
		switch req.Type {
		case "pty-req":
			var p ptyReq
			if ssh.Unmarshal(req.Payload, &p) == nil && p.Cols < 1000 && p.Rows < 1000 {
				if p.Cols == 0 {
					p.Cols = 80
				}
				if p.Rows == 0 {
					p.Rows = 24
				}
				pr = &p
				req.Reply(true, nil)
			} else {
				req.Reply(false, nil)
			}
		case "window-change":
			var w winch
			if ssh.Unmarshal(req.Payload, &w) == nil {
				mu.Lock()
				if ptmx != nil && w.Cols > 0 && w.Rows > 0 && w.Cols < 1000 && w.Rows < 1000 {
					pty.Setsize(ptmx, &pty.Winsize{Rows: uint16(w.Rows), Cols: uint16(w.Cols)})
				}
				mu.Unlock()
			}
			req.Reply(false, nil)
		case "exec":
			var e struct{ Command string }
			if started || ssh.Unmarshal(req.Payload, &e) != nil {
				req.Reply(false, nil)
				continue
			}
			started = true
			req.Reply(true, nil)
			go func() {
				code := run(ch, strings.Fields(e.Command), pr, &ptmx, &mu, dev, pairOnly)
				exitStatus(ch, code)
				ch.Close()
			}()
		default:
			req.Reply(false, nil) // no shell, no env, no subsystems, no forwarding
		}
	}
	mu.Lock()
	if ptmx != nil {
		ptmx.Close() // the client went away, tmux notices and detaches, the session keeps running
	}
	mu.Unlock()
}

func run(ch ssh.Channel, args []string, pr *ptyReq, ptmx **os.File, mu *sync.Mutex, dev string, pairOnly bool) int {
	say := func(s string) { io.WriteString(ch.Stderr(), s+"\n") }
	if len(args) == 0 {
		say("no command")
		return 2
	}
	if args[0] == "drop" && !pairOnly {
		return dropCmd(ch, args[1:], say, dev)
	}
	if args[0] == "ping" && !pairOnly {
		fmt.Fprintf(ch, "a11helper %s\n", version)
		return 0
	}
	if pairOnly {
		if args[0] != "pair" || len(args) != 3 || !deviceNameRe.MatchString(args[1]) {
			say("pairing connections can only ask to pair")
			return 2
		}
		blob, err := base64.StdEncoding.DecodeString(args[2])
		if err != nil {
			say("bad key")
			return 2
		}
		key, err := ssh.ParsePublicKey(blob)
		if err != nil {
			say("bad key")
			return 2
		}
		if !askToPair(args[1], hostPub, key) {
			say("denied")
			return 1
		}
		devStore.add(args[1], key)
		logf("paired %q", args[1])
		fmt.Fprint(ch, "ok\n")
		return 0
	}
	tm := tmuxPath()
	if tm == "" {
		say("tmux is not installed on this computer")
		return 3
	}
	switch args[0] {
	case "list":
		out, _ := exec.Command(tm, "list-sessions", "-F", "#{session_name}\t#{session_windows}\t#{session_attached}\t#{session_activity}").Output()
		ch.Write(out)
		return 0
	case "capture": // capture NAME [LINES]
		if len(args) < 2 || !nameRe.MatchString(args[1]) {
			say("bad session name")
			return 2
		}
		lines := 2000
		if len(args) > 2 {
			if n, err := strconv.Atoi(args[2]); err == nil && n > 0 && n <= 20000 {
				lines = n
			}
		}
		out, err := exec.Command(tm, "capture-pane", "-p", "-J", "-t", args[1]+":", "-S", "-"+strconv.Itoa(lines)).Output()
		if err != nil {
			say("no such session")
			return 1
		}
		ch.Write(out)
		return 0
	case "kill":
		if len(args) < 2 || !nameRe.MatchString(args[1]) {
			say("bad session name")
			return 2
		}
		if exec.Command(tm, "kill-session", "-t", args[1]+":").Run() != nil {
			say("no such session")
			return 1
		}
		logf("%s ended session %s", dev, args[1])
		return 0
	case "attach":
		if len(args) < 2 || !nameRe.MatchString(args[1]) || pr == nil {
			say("attach needs a session name and a terminal")
			return 2
		}
		// mouse on for this session only, so the handheld can scroll back through history with wheel events, and emacs keys so
		// Esc leaves tmux scroll mode
		c := exec.Command(tm, "new-session", "-A", "-s", args[1], "-c", startDir, ";", "set-option", "mouse", "on", ";", "set-option", "mode-keys", "emacs")
		term := pr.Term
		if term == "" || len(term) > 40 {
			term = "xterm-256color"
		}
		c.Env = append(os.Environ(), "TERM="+term)
		f, err := pty.StartWithSize(c, &pty.Winsize{Rows: uint16(pr.Rows), Cols: uint16(pr.Cols)})
		if err != nil {
			say("cannot start tmux: " + err.Error())
			return 1
		}
		mu.Lock()
		*ptmx = f
		mu.Unlock()
		logf("%s attached to %s", dev, args[1])
		done := make(chan struct{})
		go func() { io.Copy(ch, f); close(done) }()
		go func() { io.Copy(f, ch); f.Close() }()
		c.Wait()
		<-done
		logf("%s left %s", dev, args[1])
		return 0
	}
	say("unknown command")
	return 2
}

// ---------- console ----------

var logMu sync.Mutex

func logf(format string, a ...any) {
	logMu.Lock()
	defer logMu.Unlock()
	fmt.Printf("%s  %s\n", time.Now().Format("15:04:05"), fmt.Sprintf(format, a...))
}

func fatal(err error) {
	fmt.Fprintln(os.Stderr, "error:", err)
	os.Exit(1)
}

func addresses() []string {
	var out []string
	addrs, _ := net.InterfaceAddrs()
	for _, a := range addrs {
		if ipn, ok := a.(*net.IPNet); ok && !ipn.IP.IsLoopback() && ipn.IP.To4() != nil && !ipn.IP.IsLinkLocalUnicast() {
			out = append(out, ipn.IP.String())
		}
	}
	return out
}

func main() {
	listen := flag.String("listen", ":2222", "address to listen on, use 127.0.0.1:2222 for testing on this computer only")
	cfgDir := flag.String("config", "", "settings folder, default is a11helper inside your user settings folder")
	startFlag := flag.String("start", "", "folder new sessions start in, default is your home folder")
	dropFlag = flag.String("drop", "", "folder the handheld may pull files from, default is a11-drop in your home folder")
	flag.Parse()
	dir := *cfgDir
	if dir == "" {
		base, err := os.UserConfigDir()
		if err != nil {
			fatal(err)
		}
		dir = filepath.Join(base, "a11helper")
	}
	os.MkdirAll(dir, 0700)

	// admin shortcut: a11helper addkey NAME KEYFILE
	if a := flag.Args(); len(a) == 3 && a[0] == "addkey" {
		b, err := os.ReadFile(a[2])
		if err != nil {
			fatal(err)
		}
		k, _, _, _, err := ssh.ParseAuthorizedKey(b)
		if err != nil || !deviceNameRe.MatchString(a[1]) {
			fatal(errors.New("bad key file or device name"))
		}
		loadStore(filepath.Join(dir, "devices.json")).add(a[1], k)
		fmt.Println("added", a[1])
		return
	}

	// a11helper send FILE [modes|library|media]: put a file where the handheld can pull it
	if a := flag.Args(); len(a) >= 1 && a[0] == "send" {
		sendCLI(a[1:])
		return
	}

	startDir = *startFlag
	if startDir == "" {
		startDir, _ = os.UserHomeDir()
	}
	if st, err := os.Stat(startDir); err != nil || !st.IsDir() {
		fatal(fmt.Errorf("the start folder %q does not exist", startDir))
	}
	initDrop(*dropFlag)
	// a11helper history [NAME [LINES]]: read tmux history from this computer's terminal
	if a := flag.Args(); len(a) >= 1 && a[0] == "history" {
		historyCLI(a[1:])
		return
	}

	store := loadStore(filepath.Join(dir, "devices.json"))
	signer := hostKey(dir)

	cfg := &ssh.ServerConfig{
		MaxAuthTries: 3,
		NoClientAuth: true, // the callback below decides, only the "pair" user gets in without a key
		PublicKeyCallback: func(c ssh.ConnMetadata, key ssh.PublicKey) (*ssh.Permissions, error) {
			if c.User() == "a11" {
				if d := store.find(key); d != nil {
					return &ssh.Permissions{Extensions: map[string]string{"device": d.Name}}, nil
				}
			}
			return nil, errors.New("not paired")
		},
		// Anyone may knock as user "pair", that only offers the pairing request, which a person must approve here.
		NoClientAuthCallback: func(c ssh.ConnMetadata) (*ssh.Permissions, error) {
			if c.User() == "pair" {
				return &ssh.Permissions{Extensions: map[string]string{"pairing": "1"}}, nil
			}
			return nil, errors.New("not paired")
		},
	}
	cfg.AddHostKey(signer)
	hostPub = signer.PublicKey()
	devStore = store

	l, err := net.Listen("tcp", *listen)
	if err != nil {
		fatal(err)
	}
	_, port, _ := net.SplitHostPort(l.Addr().String())
	fmt.Printf("a11helper %s\n", version)
	if tmuxPath() == "" {
		fmt.Println("WARNING: tmux was not found, install it for sessions to work.")
	}
	fmt.Printf("Listening on port %s at:", port)
	for _, a := range addresses() {
		fmt.Printf(" %s", a)
	}
	fmt.Printf("\nPaired devices: %d. Only paired devices can log in, nothing else is offered.\n", len(store.Devices))
	fmt.Printf("New sessions start in: %s\nDrop folder for the handheld: %s\n", startDir, dropDir)
	fmt.Println("Ready. On the handheld open the GUI menu, then Sessions. Type quit here to stop.")
	go discovery(port)
	go serve(l, cfg, store)
	startWeb(dir)

	in := bufio.NewScanner(os.Stdin)
	for in.Scan() {
		f := strings.Fields(in.Text())
		if len(f) == 0 {
			continue
		}
		switch f[0] {
		case "y", "yes":
			if !answerPending(true) {
				fmt.Println("nothing is waiting to be allowed")
			}
		case "n", "no":
			if !answerPending(false) {
				fmt.Println("nothing is waiting to be allowed")
			}
		case "devices":
			for _, d := range store.Devices {
				fmt.Printf("  %s  added %s\n", d.Name, d.Added)
			}
			if len(store.Devices) == 0 {
				fmt.Println("  none")
			}
		case "revoke":
			if len(f) > 1 && store.revoke(strings.Join(f[1:], " ")) {
				fmt.Println("revoked")
			} else {
				fmt.Println("no such device")
			}
		case "web", "history":
			fmt.Println(webLink(dir))
		case "quit", "exit":
			return
		default:
			fmt.Println("y or n answers a pairing request. Also: web (history page link), devices, revoke NAME, quit")
		}
	}
	select {} // no console attached, keep serving
}
