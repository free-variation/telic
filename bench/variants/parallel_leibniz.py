"""
Leibniz pi, parallel — the counterpart of bench/variants/leibniz-parallel.h2o.

The series splits into `chunks` contiguous blocks of terms. Worker j sums the
terms k in [j*chunk_size, (j+1)*chunk_size) with term k = (-1)^k / (2k+1);
chunk_size is even, so every block starts on an even k (sign +1). The partial
sums are added and scaled by 4, exactly as the water variant's pmap-ext /
reduce pair does.

Two modes, since the tool decides what parallelism CPython gets:

  processes  one block per worker in a multiprocessing.Pool; each interpreter
             runs its own bytecode, so the blocks are genuinely concurrent
  threads    one block per worker in a ThreadPool; the arithmetic is pure
             Python, so the GIL serialises it and the total is the serial time

Workers receive two ints and return a float, so nothing large crosses a process
boundary. The reported elapsed time covers pool creation as well as compute,
because water's pmap-ext spawns its OS threads inside its own timed region;
the setup share is printed separately.
"""

import sys
import time as _t
from multiprocessing import Pool
from multiprocessing.pool import ThreadPool


DEFAULT_TERMS = 1000000000
DEFAULT_CHUNKS = 16


def partial_sum(task):
    start, end = task

    acc = 0.0
    sign = 1.0 if start % 2 == 0 else -1.0
    for k in range(start, end):
        acc += sign / (2.0 * k + 1.0)
        sign = -sign
    return acc


def blocks(terms, count):
    edges = [terms * i // count for i in range(count + 1)]
    return [(edges[i], edges[i + 1]) for i in range(count) if edges[i] < edges[i + 1]]


def run(terms, mode, workers, chunks):
    tasks = blocks(terms, chunks)

    start = _t.perf_counter()
    pool = ThreadPool(workers) if mode == "threads" else Pool(workers)
    pool.map(int, range(workers))
    setup = _t.perf_counter() - start

    pi_est = 4.0 * sum(pool.map(partial_sum, tasks))
    elapsed = _t.perf_counter() - start

    pool.close()
    pool.join()

    return elapsed, pi_est, setup


if __name__ == "__main__":
    import os

    terms = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_TERMS
    mode = sys.argv[2] if len(sys.argv) > 2 else "processes"
    workers = int(sys.argv[3]) if len(sys.argv) > 3 else os.cpu_count()
    chunks = int(sys.argv[4]) if len(sys.argv) > 4 else DEFAULT_CHUNKS

    elapsed, pi_est, setup = run(terms, mode, workers, chunks)
    print(f"elapsed: {elapsed:.6f} s")
    print(f"pi: {pi_est:.10f}")
    print(f"pool setup: {setup:.6f} s  mode: {mode}  workers: {workers}  blocks: {chunks}")
