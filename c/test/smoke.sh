#!/bin/sh
# End-to-end checks for the C port: duplicate detection, the safety model,
# category filtering, and exit codes.
#
# The fixture is created beside this script rather than in the OS temp
# directory: on Windows the temp path lives under AppData, which the safety
# model treats as protected, so a temp fixture would correctly yield nothing.
set -eu

BIN="$1"
case "$BIN" in
    /*) ABS_BIN="$BIN" ;;
    *)  ABS_BIN="$(pwd)/${BIN#./}" ;;
esac

DIR="$(pwd)/smoke-fixture-$$"
trap 'rm -rf "$DIR"' EXIT
mkdir -p "$DIR/docs" "$DIR/copies" "$DIR/node_modules/pkg"

# A duplicate pair that must be reported.
printf 'hello duplicate world\n' > "$DIR/docs/a.txt"
printf 'hello duplicate world\n' > "$DIR/copies/b.txt"
# A unique file that must not be.
printf 'unique content\n' > "$DIR/docs/c.txt"
# Duplicates the safety model must hide.
printf 'library payload\n' > "$DIR/node_modules/pkg/x.js"
printf 'library payload\n' > "$DIR/node_modules/pkg/y.js"
printf 'binary payload\n' > "$DIR/docs/tool1.exe"
printf 'binary payload\n' > "$DIR/docs/tool2.exe"

fail() { echo "FAIL: $1"; exit 1; }

# --- duplicate detection -------------------------------------------------
set +e
OUT="$("$ABS_BIN" "$DIR")"
CODE=$?
set -e
echo "$OUT"

[ "$CODE" -eq 1 ] || fail "expected exit 1 when duplicates are found, got $CODE"
echo "$OUT" | grep -q "Duplicate group 1" || fail "no duplicate group reported"
echo "$OUT" | grep -q "a.txt" || fail "a.txt missing"
echo "$OUT" | grep -q "b.txt" || fail "b.txt missing"
echo "$OUT" | grep -q "1 duplicate group(s)" || fail "expected exactly one group"
echo "$OUT" | grep -q "sha256 " || fail "group hash missing"

# --- safety model --------------------------------------------------------
echo "$OUT" | grep -q "c.txt" && fail "unique file reported as duplicate"
echo "$OUT" | grep -q "node_modules" && fail "protected directory leaked into results"
echo "$OUT" | grep -q "\.exe" && fail "protected extension leaked into results"

# --- category filter -----------------------------------------------------
set +e
IMAGES="$("$ABS_BIN" --category images "$DIR")"
IMAGES_CODE=$?
set -e
[ "$IMAGES_CODE" -eq 0 ] || fail "expected exit 0 when no image duplicates exist"
echo "$IMAGES" | grep -q "No duplicates found" || fail "category filter did not restrict results"

# --- size filter ---------------------------------------------------------
set +e
BIG="$("$ABS_BIN" --min-size 1000000 "$DIR")"
BIG_CODE=$?
set -e
[ "$BIG_CODE" -eq 0 ] || fail "expected exit 0 when the size floor excludes everything"

# --- surface inventory ---------------------------------------------------
SURFACE="$("$ABS_BIN" --surface "$DIR")"
echo "$SURFACE" | grep -q "user file(s)" || fail "surface inventory missing totals"
echo "$SURFACE" | grep -q "Text" || fail "surface inventory missing category stats"

# --- invocation errors ---------------------------------------------------
set +e
"$ABS_BIN" >/dev/null 2>&1
NOARG=$?
"$ABS_BIN" --not-a-flag "$DIR" >/dev/null 2>&1
BADFLAG=$?
set -e
[ "$NOARG" -eq 2 ] || fail "expected exit 2 with no arguments, got $NOARG"
[ "$BADFLAG" -eq 2 ] || fail "expected exit 2 for an unknown flag, got $BADFLAG"

echo "SMOKE OK"
