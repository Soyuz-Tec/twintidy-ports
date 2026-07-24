#!/bin/sh
# Smoke test: two identical files must be reported as one duplicate
# group; a unique file must not appear in any group.
set -eu

BIN="$1"
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT

mkdir -p "$DIR/sub"
printf 'hello duplicate world\n' > "$DIR/a.txt"
printf 'hello duplicate world\n' > "$DIR/sub/b.txt"
printf 'unique content\n'        > "$DIR/c.txt"

OUT="$("$BIN" "$DIR")"
echo "$OUT"

echo "$OUT" | grep -q "Duplicate group 1" || { echo "FAIL: no duplicate group"; exit 1; }
echo "$OUT" | grep -q "a.txt"             || { echo "FAIL: a.txt missing"; exit 1; }
echo "$OUT" | grep -q "b.txt"             || { echo "FAIL: b.txt missing"; exit 1; }
echo "$OUT" | grep -q "c.txt" && { echo "FAIL: unique file reported"; exit 1; }
echo "$OUT" | grep -q "1 duplicate group(s)" || { echo "FAIL: wrong group count"; exit 1; }

echo "SMOKE OK"
