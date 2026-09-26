#!/bin/sh
# Vendor Tigr into external/tigr from a pinned upstream commit.
#
# Tigr (public domain) opens a window holding a pixel buffer and blits it. It
# is two files that compile as plain C: on macOS it reaches Cocoa through the
# Objective-C runtime rather than vendoring a windowing toolkit, so no
# Objective-C compiler is needed. Link flags live in the Makefile (TIGR_LIBS).
#
# To bump: set TIGR_COMMIT to a new commit, run this, copy the new hash from
# the mismatch message into TIGR_SHA256, review the diff.
#
#   sh tools/vendor-tigr.sh
#
set -eu

TIGR_COMMIT=f7bf2abbf6b26e649ced5691003e87b61c03762e
TIGR_SHA256=da89cdd16d4234c31b9a2d1e40b32489892ef1bfe1c6eecae219216e95c9696b
TIGR_URL="https://codeload.github.com/erkkah/tigr/tar.gz/${TIGR_COMMIT}"

root=$(cd "$(dirname "$0")/.." && pwd)
dest="$root/external/tigr"

sha256() {
	if command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d' ' -f1
	else
		sha256sum "$1" | cut -d' ' -f1
	fi
}

work=$(mktemp -d "${TMPDIR:-/tmp}/vendor-tigr.XXXXXX")
trap 'rm -rf "$work"' EXIT

tgz="$work/tigr.tar.gz"
echo "downloading $TIGR_URL"
curl -sSL --fail --max-time 180 -o "$tgz" "$TIGR_URL"

got=$(sha256 "$tgz")
if [ "$got" != "$TIGR_SHA256" ]; then
	echo "sha256 mismatch:" >&2
	echo "  expected $TIGR_SHA256" >&2
	echo "  got      $got" >&2
	exit 1
fi
echo "sha256 ok"

tar xzf "$tgz" -C "$work"
upstream="$work/tigr-${TIGR_COMMIT}"

rm -rf "$dest"
mkdir -p "$dest"
cp "$upstream/tigr.c" "$dest/tigr.c"
cp "$upstream/tigr.h" "$dest/tigr.h"
cp "$upstream/UNLICENSE" "$dest/UNLICENSE"

# graphics.c calls these three and nothing else; a rename upstream would
# otherwise surface as a link error long after the vendoring.
for required in tigrWindow tigrUpdate tigrClosed; do
	if ! grep -q "$required" "$dest/tigr.h"; then
		echo "vendoring check failed: $required is not in tigr.h" >&2
		exit 1
	fi
done

cat > "$dest/PROVENANCE" <<EOF
Tigr vendored source.

upstream:  https://github.com/erkkah/tigr
commit:    $TIGR_COMMIT
tarball:   $TIGR_URL
sha256:    $TIGR_SHA256

Two files compiled as one plain-C translation unit; compile flags live in the
Makefile (TIGR_CFLAGS) and the per-platform link flags in TIGR_LIBS. Public
domain (see UNLICENSE). src/c/graphics.c is the only caller, and every Tigr call
happens on thread 0 — see PLAN.md's source invariants. Produced by
tools/vendor-tigr.sh; do not edit these files by hand, re-run the script to
update.
EOF

echo "vendored Tigr ${TIGR_COMMIT} into $dest"
