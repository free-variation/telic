"""
mandelbrot, vectorized with numpy — the counterpart of
bench/variants/mandelbrot-matrix.telic.

Every pixel's z is iterated at once as an N x N array, so element-wise
multiply/add/subtract and a > mask replace the scalar per-pixel loop. There is
no per-pixel early exit: all max_iter iterations run for every pixel and
`escaped` accumulates the escape mask, so a pixel is in-set iff it never
escaped. That gives the same checksum as the scalar version.

The array ops mirror the telic variant one for one, in place where it is in
place (*!, +!, -!), so the comparison is of the same sequence of kernels.
Grid setup is outside the timed region, as in the telic variant.

Escaped pixels overflow to inf and then nan, exactly as in the telic variant;
the mask has already been counted by then, so the errstate suppression only
silences the warnings.
"""

import sys
import time as _t

import numpy as np


DEFAULT_N = 1000
MAX_ITER = 50


def run(n, max_iter):
    index = np.arange(n, dtype=np.float64)
    cr = np.broadcast_to(2.0 * index / n - 1.5, (n, n)).copy()
    ci = np.broadcast_to((2.0 * index / n - 1.0)[:, None], (n, n)).copy()

    zr = np.zeros((n, n), dtype=np.float64)
    zi = np.zeros((n, n), dtype=np.float64)
    escaped = np.zeros((n, n), dtype=np.float64)
    zr2 = np.empty((n, n), dtype=np.float64)
    zi2 = np.empty((n, n), dtype=np.float64)
    magnitude = np.empty((n, n), dtype=np.float64)

    t0 = _t.perf_counter()
    with np.errstate(over="ignore", invalid="ignore"):
        for _ in range(max_iter):
            np.multiply(zr, zr, out=zr2)
            np.multiply(zi, zi, out=zi2)
            np.add(zr2, zi2, out=magnitude)
            escaped += magnitude > 4.0

            np.multiply(zi, zr, out=zi)
            np.multiply(zi, 2.0, out=zi)
            np.add(zi, ci, out=zi)

            np.subtract(zr2, zi2, out=zr)
            np.add(zr, cr, out=zr)

        checksum = int((escaped == 0.0).sum())
    return _t.perf_counter() - t0, checksum


if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_N
    elapsed, checksum = run(n, MAX_ITER)
    print(f"elapsed: {elapsed:.6f} s")
    print(f"checksum: {checksum}")
