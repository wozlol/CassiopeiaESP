// Sync mode. The handheld opens a long connection with the command "sync" and then waits. While it is open, this computer can send it
// files, take files from it, delete files, manage its clipboard and install firmware into a slot, all through one local control socket.
// Only the handheld starts the connection, so nothing listens on the handheld. Only your own user on this computer can use the socket.
//
//	a11helper sync-do status
//	a11helper sync-do put LOCAL_FILE CARD_PATH        CARD_PATH starts with modes/, library/ or media/
//	a11helper sync-do putdir LOCAL_FOLDER CARD_FOLDER copies what is missing or a different size
//	a11helper sync-do get CARD_PATH LOCAL_FILE
//	a11helper sync-do list CARD_FOLDER
//	a11helper sync-do rm CARD_PATH
//	a11helper sync-do rmdir CARD_FOLDER
//	a11helper sync-do clip list | add TEXT | del N | set N TEXT
//	a11helper sync-do install CARD_FILE SLOT          SLOT 1 to 4, not the slot the handheld is running from
package main

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"golang.org/x/crypto/ssh"
)

type syncConn struct {
	ch    ssh.Channel
	rd    *bufio.Reader
	dev   string
	mu    sync.Mutex // one operation at a time
	since time.Time
}

var (
	syncCur *syncConn
	syncMu  sync.Mutex
	cardRe  = regexp.MustCompile(`^(modes|library|media)/[A-Za-z0-9._+()' ,-][A-Za-z0-9._+()' ,/-]{0,150}$`)
)

func validCard(p string) bool {
	return cardRe.MatchString(p) && !strings.Contains(p, "..") && !strings.HasSuffix(p, "/")
}

func esc(s string) string {
	s = strings.ReplaceAll(s, `\`, `\\`)
	s = strings.ReplaceAll(s, "\n", `\n`)
	return strings.ReplaceAll(s, "\t", `\t`)
}
func unesc(s string) string {
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		if s[i] == '\\' && i+1 < len(s) {
			i++
			switch s[i] {
			case 'n':
				b.WriteByte('\n')
			case 't':
				b.WriteByte('\t')
			default:
				b.WriteByte(s[i])
			}
			continue
		}
		b.WriteByte(s[i])
	}
	return b.String()
}

// syncSession runs for as long as the handheld stays in sync mode.
func syncSession(ch ssh.Channel, dev string) int {
	c := &syncConn{ch: ch, rd: bufio.NewReaderSize(ch, 1<<16), dev: dev, since: time.Now()}
	line, err := c.rd.ReadString('\n')
	if err != nil || !strings.HasPrefix(line, "HELLO") {
		return 1
	}
	syncMu.Lock()
	old := syncCur
	syncCur = c
	syncMu.Unlock()
	if old != nil {
		old.ch.Close()
	}
	logf("%s opened sync mode", dev)
	for {
		time.Sleep(10 * time.Second)
		c.mu.Lock()
		_, _, err := c.call("PING")
		c.mu.Unlock()
		if err != nil {
			break
		}
	}
	syncMu.Lock()
	if syncCur == c {
		syncCur = nil
	}
	syncMu.Unlock()
	logf("%s left sync mode", dev)
	return 0
}

// call sends one command line and reads reply lines up to the closing one.
// The closing line starts with OK, PONG, END, HAVE or ERR. Lines before it are returned as they came.
func (c *syncConn) call(parts ...string) (lines []string, last string, err error) {
	if _, err = io.WriteString(c.ch, strings.Join(parts, "\t")+"\n"); err != nil {
		return nil, "", err
	}
	return c.readReply()
}

func (c *syncConn) readReply() (lines []string, last string, err error) {
	for {
		l, e := c.rd.ReadString('\n')
		if e != nil {
			return lines, "", e
		}
		l = strings.TrimRight(l, "\r\n")
		for _, p := range []string{"OK", "PONG", "END", "HAVE", "ERR"} {
			if l == p || strings.HasPrefix(l, p+"\t") {
				if p == "ERR" {
					return lines, l, errors.New(strings.TrimPrefix(strings.TrimPrefix(l, "ERR"), "\t"))
				}
				return lines, l, nil
			}
		}
		lines = append(lines, l)
	}
}

type remoteFile struct {
	Path string
	Size int64
}

func (c *syncConn) list(dir string) ([]remoteFile, error) {
	lines, _, err := c.call("LIST", dir)
	if err != nil {
		return nil, err
	}
	var out []remoteFile
	for _, l := range lines {
		f := strings.Split(l, "\t")
		if len(f) == 3 && f[0] == "F" {
			n, _ := strconv.ParseInt(f[2], 10, 64)
			out = append(out, remoteFile{f[1], n})
		}
	}
	return out, nil
}

// put sends one file. It carries on from where an earlier try stopped, if the handheld still has the part it received.
func (c *syncConn) put(local, card string, say func(string)) error {
	if !validCard(card) {
		return errors.New("the card path must start with modes/, library/ or media/ and use plain characters")
	}
	st, err := os.Stat(local)
	if err != nil || !st.Mode().IsRegular() {
		return fmt.Errorf("no such file: %s", local)
	}
	size := st.Size()
	_, last, err := c.call("PUTQ", card, strconv.FormatInt(size, 10))
	if err != nil {
		return err
	}
	have := int64(0)
	if f := strings.Split(last, "\t"); len(f) == 2 {
		have, _ = strconv.ParseInt(f[1], 10, 64)
	}
	if have > size {
		have = 0
	}
	f, err := os.Open(local)
	if err != nil {
		return err
	}
	defer f.Close()
	if _, err = f.Seek(have, io.SeekStart); err != nil {
		return err
	}
	if _, err = io.WriteString(c.ch, fmt.Sprintf("PUT\t%s\t%d\t%d\n", card, size, have)); err != nil {
		return err
	}
	buf := make([]byte, 32*1024)
	sent := have
	t0, lastSay := time.Now(), time.Now()
	for sent < size {
		n, e := f.Read(buf)
		if n > 0 {
			if _, we := c.ch.Write(buf[:n]); we != nil {
				return we
			}
			sent += int64(n)
		}
		if e != nil {
			break
		}
		if time.Since(lastSay) > 2*time.Second {
			lastSay = time.Now()
			rate := float64(sent-have) / 1e6 / time.Since(t0).Seconds()
			say(fmt.Sprintf("%s  %d%%  %.1f MB/s", card, sent*100/size, rate))
		}
	}
	_, _, err = c.readReply()
	return err
}

func (c *syncConn) get(card, local string, say func(string)) error {
	if !validCard(card) {
		return errors.New("bad card path")
	}
	if _, err := io.WriteString(c.ch, "GET\t"+card+"\n"); err != nil {
		return err
	}
	l, err := c.rd.ReadString('\n')
	if err != nil {
		return err
	}
	l = strings.TrimRight(l, "\r\n")
	f := strings.Split(l, "\t")
	if len(f) != 2 || f[0] != "DATA" {
		return errors.New(strings.TrimPrefix(l, "ERR\t"))
	}
	size, _ := strconv.ParseInt(f[1], 10, 64)
	os.MkdirAll(filepath.Dir(local), 0755)
	out, err := os.Create(local + ".part")
	if err != nil {
		return err
	}
	_, err = io.CopyN(out, c.rd, size)
	out.Close()
	if err != nil {
		os.Remove(local + ".part")
		return err
	}
	if _, _, err = c.readReply(); err != nil {
		os.Remove(local + ".part")
		return err
	}
	return os.Rename(local+".part", local)
}

// ---- the local control socket

func syncControl(dir string) {
	path := filepath.Join(dir, "sync.sock")
	os.Remove(path)
	l, err := net.Listen("unix", path)
	if err != nil {
		logf("sync control socket: %v", err)
		return
	}
	os.Chmod(path, 0600)
	for {
		nc, err := l.Accept()
		if err != nil {
			return
		}
		go handleControl(nc)
	}
}

func handleControl(nc net.Conn) {
	defer nc.Close()
	line, err := bufio.NewReader(nc).ReadString('\n')
	if err != nil {
		return
	}
	args := strings.Split(strings.TrimRight(line, "\r\n"), "\t")
	say := func(s string) { fmt.Fprintf(nc, "P\t%s\n", s) }
	fail := func(s string) { fmt.Fprintf(nc, "FAIL\t%s\n", s) }
	if len(args) == 0 || args[0] == "" {
		fail("no command")
		return
	}
	syncMu.Lock()
	c := syncCur
	syncMu.Unlock()
	if args[0] == "status" {
		if c == nil {
			fail("the handheld is not in sync mode. On it: GUI menu, Sync with the computer")
			return
		}
		fmt.Fprintf(nc, "OK\t%s is in sync mode since %s\n", c.dev, c.since.Format("15:04:05"))
		return
	}
	if c == nil {
		fail("the handheld is not in sync mode. On it: GUI menu, Sync with the computer")
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	need := func(n int) bool {
		if len(args) < n+1 {
			fail(fmt.Sprintf("%s needs %d arguments", args[0], n))
			return false
		}
		return true
	}
	switch args[0] {
	case "list":
		if !need(1) {
			return
		}
		fs, err := c.list(args[1])
		if err != nil {
			fail(err.Error())
			return
		}
		sort.Slice(fs, func(i, j int) bool { return fs[i].Path < fs[j].Path })
		var tot int64
		for _, f := range fs {
			say(fmt.Sprintf("%10d  %s", f.Size, f.Path))
			tot += f.Size
		}
		fmt.Fprintf(nc, "OK\t%d files, %.1f MB\n", len(fs), float64(tot)/1e6)
	case "put":
		if !need(2) {
			return
		}
		if err := c.put(args[1], args[2], say); err != nil {
			fail(err.Error())
			return
		}
		fmt.Fprintf(nc, "OK\tsent %s\n", args[2])
	case "putdir":
		if !need(2) {
			return
		}
		root, card := strings.TrimRight(args[1], "/"), strings.Trim(args[2], "/")
		have := map[string]int64{}
		if fs, err := c.list(card); err == nil {
			for _, f := range fs {
				have[card+"/"+f.Path] = f.Size
			}
		}
		var sent, skipped int
		var bytes int64
		err := filepath.Walk(root, func(p string, info os.FileInfo, err error) error {
			if err != nil {
				return err
			}
			name := info.Name()
			if info.IsDir() {
				if strings.HasPrefix(name, ".") && p != root {
					return filepath.SkipDir
				}
				return nil
			}
			if strings.HasPrefix(name, ".") || strings.HasSuffix(name, ".tmp") || strings.HasSuffix(name, ".part") {
				return nil
			}
			rel, _ := filepath.Rel(root, p)
			cp := card + "/" + filepath.ToSlash(rel)
			if sz, ok := have[cp]; ok && sz == info.Size() {
				skipped++
				return nil
			}
			if err := c.put(p, cp, say); err != nil {
				return fmt.Errorf("%s: %v", cp, err)
			}
			sent++
			bytes += info.Size()
			return nil
		})
		if err != nil {
			fail(err.Error())
			return
		}
		fmt.Fprintf(nc, "OK\tsent %d files, %.1f MB, already there %d\n", sent, float64(bytes)/1e6, skipped)
	case "get":
		if !need(2) {
			return
		}
		if err := c.get(args[1], args[2], say); err != nil {
			fail(err.Error())
			return
		}
		fmt.Fprintf(nc, "OK\tsaved %s\n", args[2])
	case "rm", "rmdir":
		if !need(1) {
			return
		}
		if !validCard(strings.TrimRight(args[1], "/") + "") {
			fail("bad card path")
			return
		}
		_, _, err := c.call(strings.ToUpper(args[0]), strings.TrimRight(args[1], "/"))
		if err != nil {
			fail(err.Error())
			return
		}
		fmt.Fprintf(nc, "OK\tremoved %s\n", args[1])
	case "clip":
		if !need(1) {
			return
		}
		switch args[1] {
		case "list":
			lines, _, err := c.call("CLIP", "LIST")
			if err != nil {
				fail(err.Error())
				return
			}
			for _, l := range lines {
				f := strings.SplitN(l, "\t", 3)
				if len(f) == 3 && f[0] == "C" {
					say(fmt.Sprintf("%2s  %s", f[1], strings.ReplaceAll(unesc(f[2]), "\n", " / ")))
				}
			}
			fmt.Fprintf(nc, "OK\t%d clips\n", len(lines))
		case "add":
			if !need(2) {
				return
			}
			if _, _, err := c.call("CLIP", "ADD", esc(strings.Join(args[2:], " "))); err != nil {
				fail(err.Error())
				return
			}
			fmt.Fprintln(nc, "OK\tadded")
		case "del":
			if !need(2) {
				return
			}
			if _, _, err := c.call("CLIP", "DEL", args[2]); err != nil {
				fail(err.Error())
				return
			}
			fmt.Fprintln(nc, "OK\tdeleted")
		case "set":
			if !need(3) {
				return
			}
			if _, _, err := c.call("CLIP", "SET", args[2], esc(strings.Join(args[3:], " "))); err != nil {
				fail(err.Error())
				return
			}
			fmt.Fprintln(nc, "OK\tchanged")
		default:
			fail("clip list, add TEXT, del N, set N TEXT")
		}
	case "install":
		if !need(2) {
			return
		}
		say("installing, this takes up to a minute")
		_, last, err := c.call("INSTALL", args[1], args[2])
		if err != nil {
			fail(err.Error())
			return
		}
		fmt.Fprintf(nc, "OK\t%s\n", strings.TrimPrefix(last, "OK\t"))
	default:
		fail("unknown command " + args[0])
	}
}

// syncCLI is what `a11helper sync-do ...` runs. It talks to the helper that is already running.
func syncCLI(dir string, args []string) int {
	nc, err := net.DialTimeout("unix", filepath.Join(dir, "sync.sock"), 3*time.Second)
	if err != nil {
		fmt.Fprintln(os.Stderr, "the helper is not running on this computer. Start it first, then open sync mode on the handheld.")
		return 2
	}
	defer nc.Close()
	if len(args) == 0 {
		fmt.Fprintln(os.Stderr, "usage: a11helper sync-do status | put | putdir | get | list | rm | rmdir | clip | install, see helper/sync.go")
		return 2
	}
	abs := func(p string) string {
		if a, err := filepath.Abs(p); err == nil {
			return a
		}
		return p
	}
	switch args[0] {
	case "put", "putdir":
		if len(args) > 1 {
			args[1] = abs(args[1])
		}
	case "get":
		if len(args) > 2 {
			args[2] = abs(args[2])
		}
	}
	fmt.Fprintln(nc, strings.Join(args, "\t"))
	rd := bufio.NewReader(nc)
	for {
		l, err := rd.ReadString('\n')
		if err != nil {
			fmt.Fprintln(os.Stderr, "the connection ended before an answer came")
			return 1
		}
		l = strings.TrimRight(l, "\r\n")
		f := strings.SplitN(l, "\t", 2)
		switch f[0] {
		case "P":
			fmt.Println(f[1])
		case "OK":
			fmt.Println(f[1])
			return 0
		case "FAIL":
			fmt.Fprintln(os.Stderr, "failed:", f[1])
			return 1
		}
	}
}
