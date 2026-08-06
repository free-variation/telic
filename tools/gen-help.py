#!/usr/bin/env python3
"""Generate src/c/help_table.c from docs/reference.md.

Maintainer tool: run only when reference.md changes. The generated file is
committed, so the normal cc build needs no Python.

Contract: in any markdown table whose first column header is "Word", a row is
a word entry iff its first cell is a single bare backtick token (no spaces).
Column 2 is the stack effect (or, for superwords, the usage syntax), column 3
the one-line summary. Tables that also carry Ops/Alloc/O columns contribute
those three cost strings; tables without them leave the cost fields NULL.

A word's examples follow its section's table as fence pairs: a fence tagged
``forth <word>`` (or ``forth-noexec <word>`` for one that must not run)
holding the code, immediately followed by a fence tagged ``output`` holding
exactly what it prints. Pairs land in help_examples[], sorted by word; a
fence naming an unknown word is an error.
"""

import re
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REFERENCE = os.path.join(ROOT, "docs", "reference.md")
REFERENCE_LIBRARIES = os.path.join(ROOT, "docs", "reference-libraries.md")
OUTPUT = os.path.join(ROOT, "src", "c", "help_table.c")
WORDLIST = os.path.join(ROOT, "forth-words.txt")

# Split a table row on pipes that are not backslash-escaped.
ROW_SPLIT = re.compile(r"(?<!\\)\|")
# A bare single-token word cell: one backtick group, no internal whitespace.
WORD_CELL = re.compile(r"^`([^`\s]+)`$")


def clean(cell):
    text = cell.strip()
    text = text.replace(r"\|", "|")
    text = text.replace("`", "")
    return text.strip()


def split_row(line):
    parts = ROW_SPLIT.split(line.strip())
    # A pipe-delimited row has empty first and last fields; drop them.
    if parts and parts[0].strip() == "":
        parts = parts[1:]
    if parts and parts[-1].strip() == "":
        parts = parts[:-1]
    return parts


def is_separator(parts):
    return all(re.fullmatch(r":?-{2,}:?", p.strip()) for p in parts) and parts


def c_string(value):
    if value is None:
        return "NULL"
    escaped = value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
    return '"%s"' % escaped


EXAMPLE_FENCE = re.compile(r"^```forth(-noexec)?\s+(\S+)\s*$")


def read_fence_body(lines, i):
    body = []
    while i < len(lines) and lines[i] != "```":
        body.append(lines[i])
        i += 1
    if i >= len(lines):
        sys.stderr.write("error: unterminated fence\n")
        sys.exit(1)
    return "\n".join(body), i + 1


def parse_file(path, entries, sections, examples):
    with open(path, encoding="utf-8") as handle:
        lines = handle.read().splitlines()

    i = 0
    section_index = len(sections) - 1
    while i < len(lines):
        line = lines[i]
        if line.startswith("## "):
            sections.append(line[3:].strip())
            section_index = len(sections) - 1
            i += 1
            continue
        fence = EXAMPLE_FENCE.match(line)
        if fence:
            word = fence.group(2)
            code, i = read_fence_body(lines, i + 1)
            if i >= len(lines) or lines[i] != "```output":
                sys.stderr.write("error: %s example for %r lacks its ```output fence\n"
                                 % (path, word))
                sys.exit(1)
            output, i = read_fence_body(lines, i + 1)
            examples.append((word, code, output))
            continue
        if not line.lstrip().startswith("|"):
            i += 1
            continue

        header = split_row(line)
        # Need a header row, a separator row, then body rows.
        if i + 1 >= len(lines) or not is_separator(split_row(lines[i + 1])):
            i += 1
            continue

        i += 2  # past header + separator
        is_word_table = header and header[0].strip() == "Word"
        cost_columns = [h.strip() for h in header[3:6]] == ["Ops", "Alloc", "O"] \
            if len(header) >= 6 else False

        while i < len(lines) and lines[i].lstrip().startswith("|"):
            row = split_row(lines[i])
            i += 1
            if not is_word_table or len(row) < 3:
                continue
            match = WORD_CELL.match(row[0].strip())
            if not match:
                continue  # syntax/construct row, not a word
            name = match.group(1).replace("\\|", "|")
            effect = clean(row[1])
            summary = clean(row[2])
            if cost_columns and len(row) >= 6:
                ops = clean(row[3])
                alloc = clean(row[4])
                order = clean(row[5])
            else:
                ops = alloc = order = None
            if name in entries:
                sys.stderr.write("note: %r cross-listed; keeping first occurrence\n" % name)
                continue
            entries[name] = (name, effect, summary, ops, alloc, order, section_index)


def parse():
    entries = {}
    sections = []
    examples = []
    for path in (REFERENCE, REFERENCE_LIBRARIES):
        parse_file(path, entries, sections, examples)

    unknown = sorted({word for word, _, _ in examples} - set(entries))
    if unknown:
        sys.stderr.write("error: examples for words with no reference row: %s\n"
                         % ", ".join(unknown))
        sys.exit(1)

    uncovered = sorted(set(entries) - {word for word, _, _ in examples})
    if uncovered:
        sys.stderr.write("error: words with no example fence: %s\n"
                         % ", ".join(uncovered))
        sys.exit(1)

    examples.sort(key=lambda example: example[0].encode())
    return sorted(entries.values(), key=lambda entry: entry[0]), sections, examples


def emit(entries, sections, examples):
    out = []
    out.append('#include "water.h"')
    out.append("")
    out.append("const char *const help_section_names[] = {")
    for section in sections:
        out.append("\t%s," % c_string(section))
    out.append("};")
    out.append("")
    out.append("const int help_section_count = %d;" % len(sections))
    out.append("")
    out.append("const HelpEntry help_entries[] = {")
    for name, effect, summary, ops, alloc, order, section_index in entries:
        fields = ", ".join(c_string(value) for value in (name, effect, summary, ops, alloc, order))
        out.append("\t{ %s, %d }," % (fields, section_index))
    out.append("};")
    out.append("")
    out.append("const int help_entry_count = %d;" % len(entries))
    out.append("")
    out.append("const HelpExample help_examples[] = {")
    for word, code, output in examples:
        out.append("\t{ %s, %s, %s }," % (c_string(word), c_string(code), c_string(output)))
    if not examples:
        out.append("\t{ NULL, NULL, NULL },")
    out.append("};")
    out.append("")
    out.append("const int help_example_count = %d;" % len(examples))
    out.append("")
    return "\n".join(out)


def emit_wordlist(entries):
    # One word per line, for rlwrap -f completion.
    return "".join(name + "\n" for name, *_ in entries)


def main():
    entries, sections, examples = parse()
    with open(OUTPUT, "w", encoding="utf-8") as handle:
        handle.write(emit(entries, sections, examples))
    with open(WORDLIST, "w", encoding="utf-8") as handle:
        handle.write(emit_wordlist(entries))
    exampled = len({word for word, _, _ in examples})
    sys.stderr.write("wrote %d entries (%d sections, %d examples on %d words) to %s and %s\n"
                     % (len(entries), len(sections), len(examples), exampled, OUTPUT, WORDLIST))


if __name__ == "__main__":
    main()
