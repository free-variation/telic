#!/bin/sh
# Example-program test harness (tests/examples/*.telic).
#
# These play the programs in examples/ (games, demos) rather than test the
# language, so they are kept out of `make test` and the wasm suite. Each is a
# golden pair run in batch mode like tests/run.sh, with the same <name>.stdin
# convention: with that file present the program goes in as a file argument
# and <name>.stdin is fed on stdin.

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
bin="$root/telic"

(cd "$root" && make all) || { echo "build failed"; exit 1; }

pass=0
fail=0

for input in "$here"/examples/*.telic; do
    [ -e "$input" ] || { echo "no example tests found"; exit 1; }
    name=$(basename "$input" .telic)
    expected="$here/examples/$name.expected"
    actual=$(mktemp "${TMPDIR:-/tmp}/telic.XXXXXX")
    if [ -f "$here/examples/$name.stdin" ]; then
        "$bin" -b "$input" < "$here/examples/$name.stdin" > "$actual" 2>&1
    else
        "$bin" -b < "$input" > "$actual" 2>&1
    fi
    if diff -q "$expected" "$actual" > /dev/null 2>&1; then
        pass=$((pass + 1))
        printf "  ok   %s\n" "$name"
    else
        fail=$((fail + 1))
        printf "  FAIL %s\n" "$name"
        diff -u "$expected" "$actual" | sed 's/^/       /'
    fi
    rm -f "$actual"
done

echo
echo "$pass passed, $fail failed (examples)"

[ "$fail" -eq 0 ]
