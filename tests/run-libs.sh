#!/bin/sh
# Loadable-library test harness (tests/lib/*.h2o).
#
# These tests `load` a lib/ library and need its external dependencies —
# LAPACK through the vendored liblapacke_water shared library, and (for the
# xgboost tests) libxgboost. The core `make test` suite excludes them so it
# builds and passes without those deps installed. Native-only: the wasm build
# excludes the FFI, so there is no wasm counterpart.
#
# Each test runs as a program argument (not piped -b, which swallows an
# uncaught throw) from the repo root, so relative "lib/…"/"data/…" paths
# resolve. Two modes:
#   - no <name>.expected: the test validates itself with the built-in test
#     vocabulary (expect / test / test-report — test-report throws on any
#     failure, so the run exits nonzero); passes on exit code 0, showing the
#     file's own output only when it fails.
#   - <name>.expected present (the generated doc tests): passes when the exit
#     code is 0 and stdout matches the golden exactly.

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
bin="$root/water"

(cd "$root" && make all) || { echo "build failed"; exit 1; }

pass=0
fail=0

for input in "$here"/lib/*.h2o; do
    [ -e "$input" ] || { echo "no lib tests found"; exit 1; }
    name=$(basename "$input" .h2o)
    expected="$here/lib/$name.expected"
    actual=$(mktemp "${TMPDIR:-/tmp}/water.XXXXXX")
    (cd "$root" && "$bin" "$input" > "$actual" 2>&1)
    code=$?
    if [ -f "$expected" ]; then
        # A <name>.sed file normalizes both sides before the diff, as in run.sh.
        expected_cmp="$expected"
        actual_cmp="$actual"
        if [ -f "$here/lib/$name.sed" ]; then
            expected_cmp=$(mktemp "${TMPDIR:-/tmp}/water.XXXXXX")
            actual_cmp=$(mktemp "${TMPDIR:-/tmp}/water.XXXXXX")
            sed -E -f "$here/lib/$name.sed" "$expected" > "$expected_cmp"
            sed -E -f "$here/lib/$name.sed" "$actual" > "$actual_cmp"
        fi
        if [ "$code" = 0 ] && diff -q "$expected_cmp" "$actual_cmp" > /dev/null 2>&1; then
            pass=$((pass + 1))
            printf "  ok   %s\n" "$name"
        else
            fail=$((fail + 1))
            printf "  FAIL %s (exit %s)\n" "$name" "$code"
            diff -u "$expected_cmp" "$actual_cmp" | sed 's/^/       /'
        fi
        [ "$expected_cmp" = "$expected" ] || rm -f "$expected_cmp" "$actual_cmp"
    elif [ "$code" = 0 ]; then
        pass=$((pass + 1))
        printf "  ok   %s\n" "$name"
    else
        fail=$((fail + 1))
        printf "  FAIL %s (exit %s)\n" "$name" "$code"
        sed 's/^/       /' "$actual"
    fi
    rm -f "$actual"
done

echo
echo "$pass passed, $fail failed (lib)"

[ "$fail" -eq 0 ]
