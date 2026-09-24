#!/usr/bin/env python3
"""Regenerate data/foreign.arrow, the Arrow file tests/118_arrow.telic reads.

Maintainer tool, run by hand: it needs pyarrow, which the test suite does not.
The fixture exists because read-arrow's widening cannot be tested against a file
telic wrote — write-arrow emits only float64, utf8 and timestamp[us], so the
integer, boolean, float32, large_string and non-microsecond timestamp paths, and
the multi-batch loop, have no other input. The committed file is small (about
1 KB) and changes only when this script does.

  python3 tools/gen-arrow-fixture.py
"""

import os

import pyarrow as pa
import pyarrow.ipc as ipc

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURE = os.path.join(ROOT, "data", "foreign.arrow")

SCHEMA = pa.schema([
    pa.field("i", pa.int64()),
    pa.field("b", pa.bool_()),
    pa.field("f", pa.float32()),
    pa.field("s", pa.large_string()),
    pa.field("t", pa.timestamp("ms")),
    pa.field("q", pa.float64(), metadata={"telic.unit": "kg"}),
])

BATCHES = [
    [[1, 2], [True, False], [1.5, 2.5], ["x", None], [1000, 2500], [10.0, 20.0]],
    [[3], [True], [3.5], ["z"], [3750], [30.0]],
]


def main():
    with ipc.new_file(FIXTURE, SCHEMA) as writer:
        for columns in BATCHES:
            writer.write_batch(pa.record_batch(
                [pa.array(values, field.type) for values, field in zip(columns, SCHEMA)],
                schema=SCHEMA))
    print("wrote %s (%d batches, %d rows, %d bytes)"
          % (FIXTURE, len(BATCHES), sum(len(b[0]) for b in BATCHES), os.path.getsize(FIXTURE)))


if __name__ == "__main__":
    main()
