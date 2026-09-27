#!/bin/sh
# Vendor PocketFFT into external/pocketfft from a pinned upstream commit.
#
# PocketFFT (3-clause BSD) is the C99 FFT library NumPy adopted in 1.17: double
# precision, complex and real (half-complex) transforms of any length, with
# Bluestein's algorithm keeping large prime factors at N log N. It is one
# source file and one header with no dependencies beyond libm, so it builds
# for both the native and the wasm targets.
#
# To bump: set POCKETFFT_COMMIT to a new commit, run this, copy the new hash
# from the mismatch message into POCKETFFT_SHA256, review the diff.
#
#   sh tools/vendor-pocketfft.sh
#
set -eu

POCKETFFT_COMMIT=81d171a6d5562e3aaa2c73489b70f564c633ff81
POCKETFFT_SHA256=70d1ac85b271a35a30e9bd709923170d5098413553b2380e33cb419282b8a35d
POCKETFFT_URL="https://codeload.github.com/mreineck/pocketfft/tar.gz/${POCKETFFT_COMMIT}"

root=$(cd "$(dirname "$0")/.." && pwd)
dest="$root/external/pocketfft"

sha256() {
	if command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d' ' -f1
	else
		sha256sum "$1" | cut -d' ' -f1
	fi
}

work=$(mktemp -d "${TMPDIR:-/tmp}/vendor-pocketfft.XXXXXX")
trap 'rm -rf "$work"' EXIT

tgz="$work/pocketfft.tar.gz"
echo "downloading $POCKETFFT_URL"
curl -sSL --fail --max-time 180 -o "$tgz" "$POCKETFFT_URL"

got=$(sha256 "$tgz")
if [ "$got" != "$POCKETFFT_SHA256" ]; then
	echo "sha256 mismatch:" >&2
	echo "  expected $POCKETFFT_SHA256" >&2
	echo "  got      $got" >&2
	exit 1
fi
echo "sha256 ok"

tar xzf "$tgz" -C "$work"
upstream="$work/pocketfft-${POCKETFFT_COMMIT}"

rm -rf "$dest"
mkdir -p "$dest"
cp "$upstream/pocketfft.c" "$dest/pocketfft.c"
cp "$upstream/pocketfft.h" "$dest/pocketfft.h"
cp "$upstream/LICENSE.md" "$dest/LICENSE.md"

# The plan and transform entry points telic builds on; a rename upstream would
# otherwise surface as a link error long after the vendoring.
for required in make_cfft_plan cfft_forward cfft_backward make_rfft_plan rfft_forward rfft_backward; do
	if ! grep -q "$required" "$dest/pocketfft.h"; then
		echo "vendoring check failed: $required is not in pocketfft.h" >&2
		exit 1
	fi
done

cat > "$dest/PROVENANCE" <<EOF
PocketFFT vendored source (C version).

upstream:  https://github.com/mreineck/pocketfft (branch master)
commit:    $POCKETFFT_COMMIT
tarball:   $POCKETFFT_URL
sha256:    $POCKETFFT_SHA256

One C99 source file and its header. 3-clause BSD (see LICENSE.md). Produced
by tools/vendor-pocketfft.sh; do not edit these files by hand, re-run the
script to update.
EOF

echo "vendored PocketFFT ${POCKETFFT_COMMIT} into $dest"
