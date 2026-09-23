#!/bin/sh
# Vendor sqlite-vec into external/sqlite-vec from a pinned upstream release.
#
# sqlite-vec (MIT or Apache-2.0, at your option) is a vector-search extension
# for SQLite. Its amalgamation is one sqlite-vec.c + sqlite-vec.h, compiled
# with -DSQLITE_CORE so it links into the binary as a built-in rather than a
# loadable extension — the vendored SQLite is built with
# SQLITE_OMIT_LOAD_EXTENSION, so runtime loading is off. database.c registers
# it per connection with sqlite3_vec_init. Compile flags live in the Makefile
# (SQLITE_VEC_CFLAGS).
#
# The release tarball carries only the two source files, so the two licence
# files come from the repository.
#
# To bump: set SQLITE_VEC_VERSION, run this, copy the new hash from the
# mismatch message into SQLITE_VEC_SHA256, review the diff.
#
#   sh tools/vendor-sqlite-vec.sh
#
set -eu

SQLITE_VEC_VERSION=0.1.9
SQLITE_VEC_SHA256=3acd67cb4aff080c7050926fd3cf8227905fe5b7ee3829d8ee5024ab1283cf61
SQLITE_VEC_TAG="v${SQLITE_VEC_VERSION}"
SQLITE_VEC_URL="https://github.com/asg017/sqlite-vec/releases/download/${SQLITE_VEC_TAG}/sqlite-vec-${SQLITE_VEC_VERSION}-amalgamation.tar.gz"
SQLITE_VEC_LICENSE_BASE="https://raw.githubusercontent.com/asg017/sqlite-vec/${SQLITE_VEC_TAG}"

root=$(cd "$(dirname "$0")/.." && pwd)
dest="$root/external/sqlite-vec"

sha256() {
	if command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d' ' -f1
	else
		sha256sum "$1" | cut -d' ' -f1
	fi
}

work=$(mktemp -d "${TMPDIR:-/tmp}/vendor-sqlite-vec.XXXXXX")
trap 'rm -rf "$work"' EXIT

tgz="$work/sqlite-vec.tar.gz"
echo "downloading $SQLITE_VEC_URL"
curl -sSL --fail --max-time 180 -o "$tgz" "$SQLITE_VEC_URL"

got=$(sha256 "$tgz")
if [ "$got" != "$SQLITE_VEC_SHA256" ]; then
	echo "sha256 mismatch:" >&2
	echo "  expected $SQLITE_VEC_SHA256" >&2
	echo "  got      $got" >&2
	exit 1
fi
echo "sha256 ok"

tar xzf "$tgz" -C "$work"

rm -rf "$dest"
mkdir -p "$dest"
cp "$work/sqlite-vec.c" "$dest/sqlite-vec.c"
cp "$work/sqlite-vec.h" "$dest/sqlite-vec.h"

for licence in LICENSE-MIT LICENSE-APACHE; do
	echo "downloading $licence"
	curl -sSL --fail --max-time 60 -o "$dest/$licence" "$SQLITE_VEC_LICENSE_BASE/$licence"
done

# -DSQLITE_CORE selects the built-in path: sqlite3_vec_init takes the
# connection directly instead of going through sqlite3_api_routines. If a
# release ever drops that switch the extension would demand the loadable ABI
# the vendored SQLite does not provide, so check rather than assume.
if ! grep -q SQLITE_CORE "$dest/sqlite-vec.c"; then
	echo "vendoring check failed: sqlite-vec.c has no SQLITE_CORE switch" >&2
	exit 1
fi

cat > "$dest/PROVENANCE" <<EOF
sqlite-vec vendored source (amalgamation).

upstream:  https://github.com/asg017/sqlite-vec
version:   $SQLITE_VEC_VERSION
tarball:   $SQLITE_VEC_URL
sha256:    $SQLITE_VEC_SHA256

Built as one translation unit with -DSQLITE_CORE; compile flags live in the
Makefile (SQLITE_VEC_CFLAGS). Registered per connection by p_db_open in
src/c/database.c, so no runtime extension loading is involved. Dual-licensed
MIT or Apache-2.0 at your option (see LICENSE-MIT, LICENSE-APACHE). Produced
by tools/vendor-sqlite-vec.sh; do not edit these files by hand, re-run the
script to update.
EOF

echo "vendored sqlite-vec $SQLITE_VEC_VERSION into $dest"
