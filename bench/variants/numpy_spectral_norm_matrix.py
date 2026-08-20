"""
spectral_norm, matrix-based with numpy — the counterpart of
bench/variants/spectral-norm-matrix.telic.

A is materialized once as an N x N array, so each AtA product is two BLAS
calls rather than an interpreted loop: A@x then A.T@(A@x). The water variant
spells those as dgemm-nn and dgemm-tn over an n x 1 column; here x is 1-D, the
idiomatic numpy spelling, which reaches dgemv instead of dgemm.

Structure matches the water variant: A is built outside the timed region, and
each of `loops` iterations restarts u at ones and runs ten u/v round trips.
"""

import sys
import time as _t

import numpy as np


DEFAULT_N = 260
DEFAULT_LOOPS = 1000


def build_a(n):
    i = np.arange(n, dtype=np.float64)[:, None]
    j = np.arange(n, dtype=np.float64)[None, :]
    return 1.0 / ((i + j) * (i + j + 1.0) / 2.0 + i + 1.0)


def ata_times(a, x):
    return a.T @ (a @ x)


def one_loop(a, n):
    u = np.ones(n, dtype=np.float64)
    v = None
    for _ in range(10):
        v = ata_times(a, u)
        u = ata_times(a, v)
    return u, v


def run(n, loops):
    a = build_a(n)

    t0 = _t.perf_counter()
    for _ in range(loops):
        u, v = one_loop(a, n)
    elapsed = _t.perf_counter() - t0

    return elapsed, float(np.sqrt((u @ v) / (v @ v)))


if __name__ == "__main__":
    loops = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_LOOPS
    n = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_N
    elapsed, estimate = run(n, loops)
    print(f"elapsed: {elapsed:.6f} s")
    print(f"spectral norm estimate: {estimate:.9f}")
