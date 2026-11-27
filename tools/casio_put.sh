#!/bin/bash
# Push a file or folder to the Casio's SD card over its web admin. Usage: casio_put.sh <file or folder> [card folder, default downloads]
# One time setup: put the address from GUI menu, Web admin, in ~/.a11-casio on line 1, like http://casio. Line 2 is the 6 digit code, only if "Ask for a code" is on.
set -e
CFG="$HOME/.a11-casio"
[ -f "$CFG" ] || { echo "Missing $CFG. Put the address (like http://casio) on line 1. Add the code on line 2 only if the Casio asks for one."; exit 1; }
URL=$(sed -n 1p "$CFG"); CODE=$(sed -n 2p "$CFG"); CODE="${CODE:-0}"
SRC="$1"; DEST="${2:-downloads}"
[ -e "$SRC" ] || { echo "Nothing at $SRC"; exit 1; }
JAR=$(mktemp); trap 'rm -f "$JAR"' EXIT
curl -sf -c "$JAR" -X POST -H "X-A11: 1" -d "$CODE" "$URL/api/login" >/dev/null || { echo "Could not log in. Check the address and code, and that Mode 1 is running with Tailscale connected."; exit 1; }
put() {  # local file, card path
  local enc; enc=$(python3 -c 'import sys,urllib.parse;print(urllib.parse.quote(sys.argv[1]))' "$2")
  curl -sf -b "$JAR" -H "X-A11: 1" -T "$1" "$URL/api/file?path=$enc" >/dev/null && echo "sent $2" || { echo "FAILED $2"; exit 1; }
}
if [ -d "$SRC" ]; then
  find "$SRC" -type f ! -name '.*' | while read -r f; do put "$f" "$DEST/$(basename "$SRC")/${f#$SRC/}"; done
else
  put "$SRC" "$DEST/$(basename "$SRC")"
fi
