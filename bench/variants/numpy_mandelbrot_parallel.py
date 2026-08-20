"""
mandelbrot, parallel — the counterpart of bench/variants/mandelbrot-parallel.telic.

Rows are independent, so each row block's in-set count is computed by a worker
and the counts are summed. Three modes, because Python's parallel story depends
entirely on which tool you reach for:

  processes  numpy per row block in a multiprocessing.Pool (default; two blocks
             per worker measured fastest — more blocks lose to per-task
             overhead, fewer lose to load imbalance)
  threads    numpy per row block in a ThreadPool; numpy's ufuncs release the
             GIL, so this parallelizes without any interprocess transfer
  scalar     the per-pixel Python loop with early exit, in a Pool — the same
             algorithm the water variant runs, rather than the same shape

Workers receive only (row_start, row_end) and return an int, so nothing large
crosses a process boundary. The reported elapsed time covers pool creation as
well as compute, because water's pmap-reduce spawns its OS threads inside its
own timed region; the setup share is printed separately, and for 16 CPython
processes it exceeds the compute.

The numpy modes iterate every pixel max_iter times with no early exit, as
bench/variants/mandelbrot-matrix.telic does; the scalar mode exits early per
pixel. All three give the serial checksum.
"""

import sys
import time as _t
from multiprocessing import Pool
from multiprocessing.pool import ThreadPool

import numpy as np


DEFAULT_N = 1000
MAX_ITER = 50


def count_block_numpy(task):
    row_start, row_end, n, max_iter = task

    columns = np.arange(n, dtype=np.float64)
    cr = 2.0 * columns / n - 1.5
    rows = np.arange(row_start, row_end, dtype=np.float64)
    ci = (2.0 * rows / n - 1.0)[:, None]

    shape = (row_end - row_start, n)
    zr = np.zeros(shape)
    zi = np.zeros(shape)
    escaped = np.zeros(shape)

    with np.errstate(over="ignore", invalid="ignore"):
        for _ in range(max_iter):
            zr2 = zr * zr
            zi2 = zi * zi
            escaped += (zr2 + zi2) > 4.0
            zi = zi * zr * 2.0 + ci
            zr = zr2 - zi2 + cr

        return int((escaped == 0.0).sum())


def count_block_scalar(task):
    row_start, row_end, n, max_iter = task

    count = 0
    for y in range(row_start, row_end):
        ci = 2.0 * y / n - 1.0
        for x in range(n):
            cr = 2.0 * x / n - 1.5
            zr = zi = 0.0
            iterations = 0
            while iterations < max_iter:
                zr2 = zr * zr
                zi2 = zi * zi
                if zr2 + zi2 > 4.0:
                    break
                zi = zr * zi * 2.0 + ci
                zr = zr2 - zi2 + cr
                iterations += 1
            if iterations >= max_iter:
                count += 1
    return count


def blocks(n, count):
    edges = [n * i // count for i in range(count + 1)]
    return [(edges[i], edges[i + 1]) for i in range(count) if edges[i] < edges[i + 1]]


def run(n, max_iter, mode, workers, chunks):
    worker = count_block_scalar if mode == "scalar" else count_block_numpy
    tasks = [(start, end, n, max_iter) for start, end in blocks(n, chunks)]

    start = _t.perf_counter()
    pool = ThreadPool(workers) if mode == "threads" else Pool(workers)
    pool.map(int, range(workers))
    setup = _t.perf_counter() - start

    checksum = sum(pool.map(worker, tasks))
    elapsed = _t.perf_counter() - start

    pool.close()
    pool.join()

    return elapsed, checksum, setup


if __name__ == "__main__":
    import os

    n = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_N
    mode = sys.argv[2] if len(sys.argv) > 2 else "processes"
    workers = int(sys.argv[3]) if len(sys.argv) > 3 else os.cpu_count()
    chunks = int(sys.argv[4]) if len(sys.argv) > 4 else workers * 2

    elapsed, checksum, setup = run(n, MAX_ITER, mode, workers, chunks)
    print(f"elapsed: {elapsed:.6f} s")
    print(f"checksum: {checksum}")
    print(f"pool setup: {setup:.6f} s  mode: {mode}  workers: {workers}  blocks: {chunks}")
