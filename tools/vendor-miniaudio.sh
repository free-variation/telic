#!/bin/sh
# Vendor miniaudio into external/miniaudio from a pinned upstream release.
#
# miniaudio (public domain or MIT No Attribution) plays and records audio
# through each platform's own API — Core Audio on macOS; ALSA, PulseAudio,
# JACK or OSS on Linux — which it loads at run time, so it links nothing but
# libpthread and libm. It is one header; miniaudio.c only includes it with
# MINIAUDIO_IMPLEMENTATION defined.
#
# To bump: set MINIAUDIO_TAG and MINIAUDIO_COMMIT to a new release, run this,
# copy the new hash from the mismatch message into MINIAUDIO_SHA256, review the
# diff.
#
#   sh tools/vendor-miniaudio.sh
#
set -eu

MINIAUDIO_TAG=0.11.25
MINIAUDIO_COMMIT=9634bedb5b5a2ca38c1ee7108a9358a4e233f14d
MINIAUDIO_SHA256=1a3a79b80fc6f0b0cc155e28b954a598e0ddfa2db64e2afa8466be88c476fa55
MINIAUDIO_URL="https://codeload.github.com/mackron/miniaudio/tar.gz/${MINIAUDIO_COMMIT}"

root=$(cd "$(dirname "$0")/.." && pwd)
dest="$root/external/miniaudio"

sha256() {
	if command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d' ' -f1
	else
		sha256sum "$1" | cut -d' ' -f1
	fi
}

work=$(mktemp -d "${TMPDIR:-/tmp}/vendor-miniaudio.XXXXXX")
trap 'rm -rf "$work"' EXIT

tgz="$work/miniaudio.tar.gz"
echo "downloading $MINIAUDIO_URL"
curl -sSL --fail --max-time 180 -o "$tgz" "$MINIAUDIO_URL"

got=$(sha256 "$tgz")
if [ "$got" != "$MINIAUDIO_SHA256" ]; then
	echo "sha256 mismatch:" >&2
	echo "  expected $MINIAUDIO_SHA256" >&2
	echo "  got      $got" >&2
	exit 1
fi
echo "sha256 ok"

tar xzf "$tgz" -C "$work"
upstream="$work/miniaudio-${MINIAUDIO_COMMIT}"

rm -rf "$dest"
mkdir -p "$dest"
cp "$upstream/miniaudio.h" "$dest/miniaudio.h"
cp "$upstream/miniaudio.c" "$dest/miniaudio.c"
cp "$upstream/LICENSE" "$dest/LICENSE"

# The device, node-graph and engine entry points are the ones telic's audio
# builds on; a rename upstream would otherwise surface as a link error long
# after the vendoring.
for required in ma_device_init ma_node_graph_init ma_engine_init; do
	if ! grep -q "$required" "$dest/miniaudio.h"; then
		echo "vendoring check failed: $required is not in miniaudio.h" >&2
		exit 1
	fi
done

cat > "$dest/PROVENANCE" <<EOF
miniaudio vendored source.

upstream:  https://github.com/mackron/miniaudio
release:   $MINIAUDIO_TAG
commit:    $MINIAUDIO_COMMIT
tarball:   $MINIAUDIO_URL
sha256:    $MINIAUDIO_SHA256

A single header, miniaudio.h; miniaudio.c defines MINIAUDIO_IMPLEMENTATION and
includes it. Public domain or MIT No Attribution, at the user's choice (see
LICENSE). Produced by tools/vendor-miniaudio.sh; do not edit these files by
hand, re-run the script to update.
EOF

echo "vendored miniaudio ${MINIAUDIO_TAG} into $dest"
