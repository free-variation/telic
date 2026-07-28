#!/bin/sh
# Loadable-library test harness (tests/lib/*.h2o).
#
# These tests `load` a lib/ library and need its external dependencies —
# LAPACK through the vendored liblapacke_water shared library, and (for the
# xgboost tests) libxgboost. The core `make test` suite excludes them so it
# builds and passes without those deps installed. Native-only: the wasm build
# excludes the FFI, so there is no wasm counterpart.
#
# Each test validates itself with the built-in test vocabulary (expect / test /
# test-report): a passing run prints `ok <name>` lines and exits 0; test-report
# throws on any failure, which a program-file run turns into a nonzero exit. So
# the harness runs each file as a program argument (not piped -b, which swallows
# an uncaught throw) and passes on exit code 0, showing the file's own output
# only when it fails. Run from the repo root (make test-libs does), so the
# tests' relative "lib/…"/"data/…" paths resolve.

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
    out=$(cd "$root" && "$bin" "$input" 2>&1)
    code=$?
    if [ "$code" = 0 ]; then
        pass=$((pass + 1))
        printf "  ok   %s\n" "$name"
    else
        fail=$((fail + 1))
        printf "  FAIL %s (exit %s)\n" "$name" "$code"
        printf "%s\n" "$out" | sed 's/^/       /'
    fi
done

echo
echo "$pass passed, $fail failed (lib)"

[ "$fail" -eq 0 ]
