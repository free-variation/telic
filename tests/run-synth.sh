#!/bin/sh
# Audible synthesizer tests (tests/synth/*.telic).
#
# These open the audio output device and play sound, so they need a machine
# with one and are kept out of `make test` (and the wasm suite, which has no
# device). Each is a golden pair run in batch mode like tests/run.sh; the
# output is shown as it is printed, so a sound's description appears while
# it plays.

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
bin="$root/telic"

(cd "$root" && make all) || { echo "build failed"; exit 1; }

pass=0
fail=0

for input in "$here"/synth/*.telic; do
    [ -e "$input" ] || { echo "no synth tests found"; exit 1; }
    name=$(basename "$input" .telic)
    expected="$here/synth/$name.expected"
    actual=$(mktemp "${TMPDIR:-/tmp}/telic.XXXXXX")
    printf "  %s\n" "$name"
    "$bin" -b < "$input" 2>&1 | tee "$actual" | sed 's/^/       /'
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
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
