"""
Leibniz pi, fully vectorized with numpy — the counterpart of
bench/variants/leibniz-matrix.h2o.

Both build the whole denominator sequence as one dense array of doubles and
reduce it: pi = sum(4 / seq(-2*rounds+1, 2*rounds, step 4)). At rounds = 1e9
that array is 8 GB, so the division is done in place (out=seq) to keep the peak
at one array, matching the water variant's in-place /!.

Assumes rounds is even, as the water variant does.
"""

import sys
import time as _t

import numpy as np


DEFAULT_ROUNDS = 1_000_000_000


def leibniz(rounds):
    start = -2.0 * rounds + 1.0
    end = 2.0 * rounds
    seq = np.arange(start, end, 4.0, dtype=np.float64)
    np.divide(4.0, seq, out=seq)
    return seq.sum()


if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_ROUNDS
    t0 = _t.perf_counter()
    value = leibniz(n)
    print(f"elapsed: {_t.perf_counter() - t0:.6f}s")
    print(f"pi: {value:.15f}")
