#!/usr/bin/env python3
"""Generate src/c/bundle_embed.h, the files a bundled program carries.

A bundle is a telic binary built with -DTELIC_BUNDLE (make bundle): it runs
the entry file at startup, passing every command-line argument to it as args,
and load, read-file and file-exists? find the embedded files before the
disk. The entry file and every .telic file under the directory named as the
entry without its extension are embedded, each under its path relative to
the entry's directory, so examples/deadline.telic carries deadline.telic and
deadline/*.telic and its "deadline/text.telic" load resolves inside the
binary. Each asset path after the entry is embedded under that path as
written, relative to the directory make runs in, which is the working
directory the program names its files from: examples/hobbit.telic reads
"data/hobbit.tzx".

  python3 tools/gen-bundle.py examples/deadline.telic
  python3 tools/gen-bundle.py examples/hobbit.telic data/hobbit.tzx
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "src", "c", "bundle_embed.h")


def source_files(entry):
    base = os.path.dirname(entry)
    paths = [entry]
    directory = os.path.splitext(entry)[0]
    for parent, _, names in sorted(os.walk(directory)):
        paths += [os.path.join(parent, name) for name in sorted(names) if name.endswith(".telic")]
    return [(path, os.path.relpath(path, base)) for path in paths]


def c_bytes(data):
    rows = [", ".join(f"0x{byte:02x}" for byte in data[k:k + 16]) for k in range(0, len(data), 16)]
    return ",\n\t".join(rows)


def main():
    entry = sys.argv[1]
    files = source_files(entry) + [(asset, os.path.normpath(asset)) for asset in sys.argv[2:]]
    arrays = []
    rows = []
    for index, (path, name) in enumerate(files):
        with open(path, "rb") as source:
            data = source.read()
        arrays.append(f"static const unsigned char bundle_file_{index}[] = {{\n\t{c_bytes(data)}\n}};\n")
        rows.append(f'\t{{ "{name}", bundle_file_{index}, {len(data)} }},\n')
    with open(OUT, "w") as out:
        out.writelines(arrays)
        out.write("\nstatic const BundledFile bundled_files[] = {\n")
        out.writelines(rows)
        out.write("};\n\n")
        out.write(f"static const int n_bundled_files = {len(files)};\n")
        out.write(f'static const char bundle_entry[] = "{os.path.relpath(entry, os.path.dirname(entry))}";\n')


main()
