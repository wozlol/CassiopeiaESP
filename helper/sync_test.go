package main

import (
	"bufio"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"golang.org/x/crypto/ssh"
)

// A fake handheld on the other end of a pipe, following the same protocol as the real one.
type pipeChan struct {
	io.Reader
	io.Writer
}

func (p pipeChan) Close() error                                   { return nil }
func (p pipeChan) CloseWrite() error                              { return nil }
func (p pipeChan) SendRequest(string, bool, []byte) (bool, error) { return true, nil }
func (p pipeChan) Stderr() io.ReadWriter                          { return nil }

var _ ssh.Channel = pipeChan{}

func fakeHandheld(t *testing.T, r io.Reader, w io.Writer, store map[string][]byte, part map[string][]byte) {
	rd := bufio.NewReader(r)
	for {
		line, err := rd.ReadString('\n')
		if err != nil {
			return
		}
		f := strings.Split(strings.TrimRight(line, "\n"), "\t")
		switch f[0] {
		case "PING":
			io.WriteString(w, "PONG\n")
		case "PUTQ":
			io.WriteString(w, "HAVE\t"+strconv.Itoa(len(part[f[1]]))+"\n")
		case "PUT":
			size, _ := strconv.Atoi(f[2])
			off, _ := strconv.Atoi(f[3])
			buf := make([]byte, size-off)
			io.ReadFull(rd, buf)
			store[f[1]] = append(append([]byte{}, part[f[1]][:off]...), buf...)
			io.WriteString(w, "OK\n")
		case "LIST":
			for p, b := range store {
				io.WriteString(w, "F\t"+strings.TrimPrefix(p, f[1]+"/")+"\t"+strconv.Itoa(len(b))+"\n")
			}
			io.WriteString(w, "END\n")
		case "GET":
			b := store[f[1]]
			io.WriteString(w, "DATA\t"+strconv.Itoa(len(b))+"\n")
			w.Write(b)
			io.WriteString(w, "OK\n")
		case "RM":
			delete(store, f[1])
			io.WriteString(w, "OK\n")
		case "CLIP":
			if f[1] == "LIST" {
				io.WriteString(w, "C\t1\thello\\nworld\nEND\n")
			} else {
				io.WriteString(w, "OK\n")
			}
		default:
			io.WriteString(w, "ERR\tunknown\n")
		}
	}
}

func TestSyncProtocol(t *testing.T) {
	hr, hw := io.Pipe() // handheld to helper
	cr, cw := io.Pipe() // helper to handheld
	store := map[string][]byte{}
	part := map[string][]byte{"library/big.bin": []byte("0123")}
	go fakeHandheld(t, cr, hw, store, part)
	c := &syncConn{ch: pipeChan{hr, cw}, rd: bufio.NewReader(hr)}

	dir := t.TempDir()
	data := []byte("0123456789abcdef")
	os.WriteFile(filepath.Join(dir, "big.bin"), data, 0644)
	if err := c.put(filepath.Join(dir, "big.bin"), "library/big.bin", func(string) {}); err != nil {
		t.Fatal(err)
	}
	if string(store["library/big.bin"]) != string(data) {
		t.Fatalf("resumed file is %q", store["library/big.bin"])
	}
	fs, err := c.list("library")
	if err != nil || len(fs) != 1 || fs[0].Size != int64(len(data)) {
		t.Fatalf("list %v %v", fs, err)
	}
	out := filepath.Join(dir, "back.bin")
	if err := c.get("library/big.bin", out, func(string) {}); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(out); string(b) != string(data) {
		t.Fatalf("got back %q", b)
	}
	if _, _, err := c.call("RM", "library/big.bin"); err != nil || len(store) != 0 {
		t.Fatal("rm")
	}
	if _, _, err := c.call("NOPE"); err == nil {
		t.Fatal("an unknown command should fail")
	}
	if validCard("../etc/passwd") || validCard("modes/../x") || !validCard("modes/reader.bin") || validCard("other/x") {
		t.Fatal("card path check")
	}
	if unesc(esc("a\nb\t\\c")) != "a\nb\t\\c" {
		t.Fatal("escape")
	}
}
