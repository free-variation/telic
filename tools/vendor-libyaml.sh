#!/bin/sh
# Vendor libyaml into external/libyaml from a pinned upstream release.
#
# libyaml (MIT) is the C YAML 1.1 parser and emitter PyYAML and Ruby's Psych
# build on. telic uses its event API: the parser's events build values in
# src/c/yaml.c, which types the scalars itself by YAML 1.2's core schema, and
# the emitter writes frame>yaml's output. Plain C with no dependencies beyond
# libc, so it builds for both the native and the wasm targets.
#
# To bump: set LIBYAML_TAG to a new release tag, run this, copy the new hash
# from the mismatch message into LIBYAML_SHA256, review the diff.
#
#   sh tools/vendor-libyaml.sh
#
set -eu

LIBYAML_TAG=0.2.5
LIBYAML_SHA256=fa240dbf262be053f3898006d502d514936c818e422afdcf33921c63bed9bf2e
LIBYAML_URL="https://codeload.github.com/yaml/libyaml/tar.gz/refs/tags/${LIBYAML_TAG}"

root=$(cd "$(dirname "$0")/.." && pwd)
dest="$root/external/libyaml"

sha256() {
	if command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d' ' -f1
	else
		sha256sum "$1" | cut -d' ' -f1
	fi
}

work=$(mktemp -d "${TMPDIR:-/tmp}/vendor-libyaml.XXXXXX")
trap 'rm -rf "$work"' EXIT

tgz="$work/libyaml.tar.gz"
echo "downloading $LIBYAML_URL"
curl -sSL --fail --max-time 180 -o "$tgz" "$LIBYAML_URL"

got=$(sha256 "$tgz")
if [ "$got" != "$LIBYAML_SHA256" ]; then
	echo "sha256 mismatch:" >&2
	echo "  expected $LIBYAML_SHA256" >&2
	echo "  got      $got" >&2
	exit 1
fi
echo "sha256 ok"

tar xzf "$tgz" -C "$work"
upstream="$work/libyaml-${LIBYAML_TAG}"

rm -rf "$dest"
mkdir -p "$dest/src" "$dest/include"
cp "$upstream"/src/*.c "$upstream/src/yaml_private.h" "$dest/src/"
cp "$upstream/include/yaml.h" "$dest/include/yaml.h"
cp "$upstream/License" "$dest/License"

# The event API entry points telic builds on; a rename upstream would
# otherwise surface as a link error long after the vendoring.
for required in yaml_parser_initialize yaml_parser_set_input_string yaml_parser_parse yaml_emitter_initialize yaml_emitter_set_output yaml_emitter_emit yaml_scalar_event_initialize; do
	if ! grep -q "$required" "$dest/include/yaml.h"; then
		echo "vendoring check failed: $required is not in yaml.h" >&2
		exit 1
	fi
done

cat > "$dest/PROVENANCE" <<EOF
libyaml vendored source.

upstream:  https://github.com/yaml/libyaml (tag $LIBYAML_TAG)
tarball:   $LIBYAML_URL
sha256:    $LIBYAML_SHA256

The library's src/ and include/yaml.h. MIT (see License). Produced by
tools/vendor-libyaml.sh; do not edit these files by hand, re-run the script
to update.
EOF

echo "vendored libyaml ${LIBYAML_TAG} into $dest"
