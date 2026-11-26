// The drop folder: files the handheld may pull over its connection, for new firmware images and small files.
// Put files under ~/a11-drop/modes, library or media. On the handheld, GUI menu, Get files. Nothing else on the computer is reachable this way.
package main

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

var dropDir string
var dropPathRe = regexp.MustCompile(`^(modes|library|media)/[A-Za-z0-9._+ /-]{1,160}$`)

func initDrop(dir string) {
	if dir == "" {
		home, _ := os.UserHomeDir()
		dir = filepath.Join(home, "a11-drop")
	}
	dropDir = dir
	for _, sub := range []string{"modes", "library", "media"} {
		os.MkdirAll(filepath.Join(dropDir, sub), 0755)
	}
}

// a11helper send FILE [modes|library|media]  copies a file into the drop folder.
func sendCLI(args []string) {
	if len(args) < 1 {
		fatal(fmt.Errorf("usage: a11helper send FILE [modes|library|media]"))
	}
	sub := "modes"
	if len(args) > 1 {
		sub = args[1]
	}
	if sub != "modes" && sub != "library" && sub != "media" {
		fatal(fmt.Errorf("the folder must be modes, library or media"))
	}
	initDrop(*dropFlag)
	src, err := os.Open(args[0])
	if err != nil {
		fatal(err)
	}
	defer src.Close()
	dst := filepath.Join(dropDir, sub, filepath.Base(args[0]))
	out, err := os.Create(dst + ".part")
	if err != nil {
		fatal(err)
	}
	if _, err := io.Copy(out, src); err != nil {
		out.Close()
		fatal(err)
	}
	out.Close()
	os.Rename(dst+".part", dst)
	st, _ := os.Stat(dst)
	fmt.Printf("ready for the handheld: %s (%d bytes)\n", dst, st.Size())
}

func dropCmd(ch io.Writer, args []string, say func(string), dev string) int {
	if dropDir == "" {
		initDrop("")
	}
	if len(args) == 0 {
		say("drop list, or drop get PATH")
		return 2
	}
	switch args[0] {
	case "list":
		var lines []string
		for _, sub := range []string{"modes", "library", "media"} {
			filepath.Walk(filepath.Join(dropDir, sub), func(p string, info os.FileInfo, err error) error {
				if err != nil || info.IsDir() || strings.HasPrefix(info.Name(), ".") || strings.HasSuffix(info.Name(), ".part") || !info.Mode().IsRegular() {
					return nil
				}
				rel, _ := filepath.Rel(dropDir, p)
				rel = filepath.ToSlash(rel)
				if dropPathRe.MatchString(rel) && len(lines) < 400 {
					lines = append(lines, fmt.Sprintf("%s\t%d", rel, info.Size()))
				}
				return nil
			})
		}
		sort.Strings(lines)
		for _, l := range lines {
			fmt.Fprintln(ch, l)
		}
		return 0
	case "get":
		if len(args) != 2 || !dropPathRe.MatchString(args[1]) || strings.Contains(args[1], "..") {
			say("bad path")
			return 2
		}
		full := filepath.Join(dropDir, filepath.FromSlash(args[1]))
		real, err := filepath.EvalSymlinks(full)
		root, _ := filepath.EvalSymlinks(dropDir)
		if err != nil || !strings.HasPrefix(real, root+string(os.PathSeparator)) {
			say("no such file")
			return 1
		}
		f, err := os.Open(real)
		if err != nil {
			say("no such file")
			return 1
		}
		defer f.Close()
		st, err := f.Stat()
		if err != nil || !st.Mode().IsRegular() || st.Size() > 3_900_000_000 {
			say("not a file the handheld can take")
			return 1
		}
		fmt.Fprintf(ch, "A11DROP %d\n", st.Size())
		io.Copy(ch, f)
		logf("%s took %s (%d bytes)", dev, args[1], st.Size())
		return 0
	}
	say("drop list, or drop get PATH")
	return 2
}
