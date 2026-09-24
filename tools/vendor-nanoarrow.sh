#!/bin/sh
# Vendor nanoarrow into external/nanoarrow from a pinned upstream release.
#
# nanoarrow (Apache-2.0) is the Arrow project's minimal C implementation of the
# Arrow specification. The release tarball is the full source tree; its own
# ci/scripts/bundle.py flattens the pieces we use into three translation units
# — nanoarrow.c, nanoarrow_ipc.c and flatcc.c — plus two include trees. The IPC
# reader and writer need flatcc, the FlatBuffers runtime, which is vendored
# inside the tarball and carries the same licence (see LICENSE.txt).
#
# Needs python3 to run the bundler, as the other generators here do.
#
# To bump: set NANOARROW_VERSION, run this, copy the new hash from the mismatch
# message into NANOARROW_SHA256, review the diff.
#
#   sh tools/vendor-nanoarrow.sh
#
set -eu

NANOARROW_VERSION=0.9.0
NANOARROW_SHA256=801200a0e95e869d5c4bdeb5b535dba58551482bb782b7dc8bd599c8b6e8cacf
NANOARROW_RELEASE="apache-arrow-nanoarrow-${NANOARROW_VERSION}"
NANOARROW_URL="https://github.com/apache/arrow-nanoarrow/releases/download/${NANOARROW_RELEASE}/${NANOARROW_RELEASE}.tar.gz"

root=$(cd "$(dirname "$0")/.." && pwd)
dest="$root/external/nanoarrow"

sha256() {
	if command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d' ' -f1
	else
		sha256sum "$1" | cut -d' ' -f1
	fi
}

work=$(mktemp -d "${TMPDIR:-/tmp}/vendor-nanoarrow.XXXXXX")
trap 'rm -rf "$work"' EXIT

tgz="$work/nanoarrow.tar.gz"
echo "downloading $NANOARROW_URL"
curl -sSL --fail --max-time 300 -o "$tgz" "$NANOARROW_URL"

got=$(sha256 "$tgz")
if [ "$got" != "$NANOARROW_SHA256" ]; then
	echo "sha256 mismatch:" >&2
	echo "  expected $NANOARROW_SHA256" >&2
	echo "  got      $got" >&2
	exit 1
fi
echo "sha256 ok"

tar xzf "$tgz" -C "$work"
upstream="$work/$NANOARROW_RELEASE"

echo "bundling with ci/scripts/bundle.py"
rm -rf "$dest"
mkdir -p "$dest"
(cd "$upstream" && python3 ci/scripts/bundle.py --with-ipc --with-flatcc --output-dir "$dest")

cp "$upstream/LICENSE.txt" "$dest/LICENSE.txt"
cp "$upstream/NOTICE.txt" "$dest/NOTICE.txt"

# The bundler also emits the C++ wrappers; telic is C, and leaving them would
# imply a C++ surface the Makefile never builds.
rm -f "$dest/include/nanoarrow/nanoarrow.hpp" "$dest/include/nanoarrow/nanoarrow_ipc.hpp"

for required in src/nanoarrow.c src/nanoarrow_ipc.c src/flatcc.c \
		include/nanoarrow/nanoarrow.h include/nanoarrow/nanoarrow_ipc.h; do
	if [ ! -f "$dest/$required" ]; then
		echo "bundling check failed: $required is missing" >&2
		exit 1
	fi
done

cat > "$dest/PROVENANCE" <<EOF
nanoarrow vendored source (bundled distribution).

upstream:  https://github.com/apache/arrow-nanoarrow
version:   $NANOARROW_VERSION
tarball:   $NANOARROW_URL
sha256:    $NANOARROW_SHA256

Produced by running the release's own ci/scripts/bundle.py with --with-ipc
--with-flatcc, which flattens the source tree into src/nanoarrow.c,
src/nanoarrow_ipc.c and src/flatcc.c plus the include/ trees. The C++ wrappers
the bundler also emits are deleted; telic builds C only. Compile flags live in
the Makefile (NANOARROW_CFLAGS); src/c/arrow.c is the only caller.
Apache-2.0 (see LICENSE.txt, NOTICE.txt). Produced by
tools/vendor-nanoarrow.sh; do not edit these files by hand, re-run the script
to update.
EOF

echo "vendored nanoarrow $NANOARROW_VERSION into $dest"
