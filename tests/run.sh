#!/bin/sh
# Golden-output test harness for telic.
#
# Each test is a pair of files in this directory:
#   <name>.telic        — input piped to the REPL on stdin
#   <name>.expected  — exact stdout the REPL should produce
#
# A third file, <name>.stdin, marks a test that reads stdin itself: the program
# is passed as a file argument and <name>.stdin is fed on stdin, since piping
# the program would leave the test nothing to read.
#
# Tests run in alphabetical order. The exit code is 0 if every test
# passes, 1 otherwise — suitable for CI.

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
bin="$root/telic"

(cd "$root" && make all) || { echo "build failed"; exit 1; }

pass=0
fail=0

for input in "$here"/*.telic; do
    [ -e "$input" ] || { echo "no tests found"; exit 1; }
    name=$(basename "$input" .telic)
    expected="$here/$name.expected"
    if [ ! -f "$expected" ]; then
        echo "SKIP $name (no .expected file)"
        continue
    fi
    actual=$(mktemp "${TMPDIR:-/tmp}/telic.XXXXXX")
    # Batch mode (-b): no banner, no per-line prompt — just the program's own
    # output (and errors), so expected files hold exactly what the script prints.
    if [ -f "$here/$name.stdin" ]; then
        "$bin" -b "$input" < "$here/$name.stdin" > "$actual" 2>&1
    else
        "$bin" -b < "$input" > "$actual" 2>&1
    fi
    # A <name>.sed file normalizes nondeterministic output (a wall-clock
    # timestamp) on both sides before the diff.
    expected_cmp="$expected"
    actual_cmp="$actual"
    if [ -f "$here/$name.sed" ]; then
        expected_cmp=$(mktemp "${TMPDIR:-/tmp}/telic.XXXXXX")
        actual_cmp=$(mktemp "${TMPDIR:-/tmp}/telic.XXXXXX")
        sed -E -f "$here/$name.sed" "$expected" > "$expected_cmp"
        sed -E -f "$here/$name.sed" "$actual" > "$actual_cmp"
    fi
    if diff -q "$expected_cmp" "$actual_cmp" > /dev/null 2>&1; then
        pass=$((pass + 1))
        printf "  ok   %s\n" "$name"
    else
        fail=$((fail + 1))
        printf "  FAIL %s\n" "$name"
        diff -u "$expected_cmp" "$actual_cmp" | sed 's/^/       /'
    fi
    [ "$expected_cmp" = "$expected" ] || rm -f "$expected_cmp" "$actual_cmp"
    rm -f "$actual"
done

echo
echo "$pass passed, $fail failed"

echo
sh "$here/cli_tests.sh"
cli_status=$?

echo
sh "$here/gc_pressure.sh"
gc_status=$?

[ "$fail" -eq 0 ] && [ "$cli_status" -eq 0 ] && [ "$gc_status" -eq 0 ]
