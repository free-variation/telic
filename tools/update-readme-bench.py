#!/usr/bin/env python3
"""Splice a benchmark report's environment and standalone table into the README.

  bench/run-benchmarks.sh > bench12.md
  python3 tools/update-readme-bench.py bench12.md

The README's Benchmarks section carries the newest report verbatim between

  <!-- bench:begin --> ... <!-- bench:end -->

so the table is never edited cell by cell. Everything outside the markers —
the paragraph describing what `make bench` does — is written by hand and left
alone. Reads the report's "## Environment" bullets and its "## Standalone
benchmarks" table; the verification table stays in the report only.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
README = os.path.join(ROOT, "README.md")
BEGIN = "<!-- bench:begin -->"
END = "<!-- bench:end -->"


def section(text, heading_pattern):
    match = re.search(r"^## +%s.*$" % heading_pattern, text, re.M)
    if not match:
        sys.exit("error: no '## %s' section in the report" % heading_pattern)
    rest = text[match.end():]
    nxt = re.search(r"^## ", rest, re.M)
    return rest[:nxt.start()] if nxt else rest


def environment_line(report):
    bullets = {}
    for line in section(report, "Environment").splitlines():
        found = re.match(r"- \*\*(.+?)\*\*: (.+)$", line.strip())
        if found:
            bullets[found.group(1)] = found.group(2).strip()

    reps = "5"
    found = re.search(r"\((\d+) telic reps, (\d+) python reps", report)
    if found:
        reps = found.group(1)
        reps_py = found.group(2)
    else:
        reps_py = "3"

    parts = []
    for key in ("CPU", "Memory", "Host", "Compiler"):
        if key in bullets:
            parts.append(bullets["Memory"] + " memory" if key == "Memory" else bullets[key])
    if "Python" in bullets:
        parts.append(bullets["Python"].split(" (")[0])
    if "numpy" in bullets:
        parts.append("numpy " + bullets["numpy"].split(" ")[0])

    dated = " (%s)" % bullets["Date"] if "Date" in bullets else ""
    return "Medians of %s telic reps against %s CPython reps%s: %s." % (
        reps, reps_py, dated, ", ".join(parts))


def standalone_table(report):
    lines = []
    for line in section(report, "Standalone benchmarks").splitlines():
        if line.startswith("|"):
            lines.append(line)
        elif lines:
            break
    if not lines:
        sys.exit("error: no table under '## Standalone benchmarks'")
    return "\n".join(lines)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: update-readme-bench.py <report.md>")

    report = open(sys.argv[1], encoding="utf-8").read()
    readme = open(README, encoding="utf-8").read()
    if BEGIN not in readme or END not in readme:
        sys.exit("error: README has no %s / %s markers" % (BEGIN, END))

    block = "%s\n%s\n\n%s\n%s" % (BEGIN, environment_line(report), standalone_table(report), END)
    updated = re.sub(re.escape(BEGIN) + r".*?" + re.escape(END), lambda m: block, readme, flags=re.S)
    open(README, "w", encoding="utf-8").write(updated)

    n_rows = standalone_table(report).count("\n") - 1
    print("README benchmark section updated from %s (%d rows)" % (sys.argv[1], n_rows))


if __name__ == "__main__":
    main()
