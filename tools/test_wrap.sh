#!/bin/sh
# Checks the menu word wrap on this computer, no board needed.
set -e
d=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
cc -Wall -I"$d/components/ui/include" "$d/components/ui/wrap.c" "$d/tools/test_wrap.c" -o "$out/test_wrap"
"$out/test_wrap"
