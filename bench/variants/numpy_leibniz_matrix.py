"""
Leibniz pi, fully vectorized with numpy — the counterpart of
bench/variants/leibniz-matrix.telic.

The expression is the official numpy entry from the speed-comparison project,
verbatim:

    pi = 4 * (1 / np.arange(1 + (n % 2) * 2 - 2 * n, 2 * n + 1, 4)).sum()

It builds the denominators as one dense int64 array and the reciprocals as a
second float64 array, so the peak is two arrays of rounds/2 elements each — at
rounds = 1e9 that is 16 GB. The telic variant reduces the same sequence.
"""

import sys
import time as _t

import numpy as np


DEFAULT_ROUNDS = 1_000_000_000


def leibniz(n):
    return 4 * (1 / np.arange(1 + (n % 2) * 2 - 2 * n, 2 * n + 1, 4)).sum()


if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_ROUNDS
    t0 = _t.perf_counter()
    value = leibniz(n)
    print(f"elapsed: {_t.perf_counter() - t0:.6f}s")
    print(f"pi: {value:.16f}")
