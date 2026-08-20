"""
mandelbrot — Computer Language Benchmarks Game.

Escape-time over an N x N grid. Matches bench/pyperformance/mandelbrot.telic
(same mapping, iteration cap, and in-set pixel count).
"""

import sys
import time as _t


DEFAULT_ARG = 1000
MAX_ITER = 50


def run_mandelbrot(n):
    count = 0
    for y in range(n):
        ci = 2.0 * y / n - 1.0
        for x in range(n):
            cr = 2.0 * x / n - 1.5
            zr = 0.0
            zi = 0.0
            i = 0
            while i < MAX_ITER:
                zr2 = zr * zr
                zi2 = zi * zi
                if zr2 + zi2 > 4.0:
                    break
                zi = 2.0 * zr * zi + ci
                zr = zr2 - zi2 + cr
                i += 1
            if i >= MAX_ITER:
                count += 1
    return count


if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_ARG
    t0 = _t.perf_counter()
    result = run_mandelbrot(n)
    print(f"elapsed: {_t.perf_counter() - t0:.6f} s")
    print(f"checksum: {result}")
