"""
MonteCarlo pi, parallel — the counterpart of bench/variants/monte-carlo-parallel.h2o.

Estimates pi by sampling the unit square and counting the points inside the
quarter circle: estimate = 4 * (#inside / #samples). scimark's own generator is
a strict serial sequence, so the water variant draws from its native random
stream instead, giving each worker a deterministic sub-stream of one base seed.
This reference does the same with one random.Random per worker, seeded from the
same base, so its estimate is also reproducible run to run — it is a different
generator from water's, so the two estimates agree to about four digits rather
than exactly.

Two modes:

  processes  one sample block per worker in a multiprocessing.Pool
  threads    one sample block per worker in a ThreadPool, where the GIL
             serialises the pure-Python sampling loop

The reported elapsed time covers pool creation as well as compute, because
water's pmap-ext spawns its OS threads inside its own timed region; the setup
share is printed separately.
"""

import sys
import time as _t
from multiprocessing import Pool
from multiprocessing.pool import ThreadPool
from random import Random


DEFAULT_SAMPLES = 20000000
DEFAULT_WORKERS = 10
BASE_SEED = 12345


def count_under_curve(task):
    samples, seed = task

    draw = Random(seed).random
    under_curve = 0
    for _ in range(samples):
        x = draw()
        y = draw()
        if x * x + y * y <= 1.0:
            under_curve += 1
    return under_curve


def run(samples, mode, workers):
    chunk = samples // workers
    tasks = [(chunk, BASE_SEED + j) for j in range(workers)]

    start = _t.perf_counter()
    pool = ThreadPool(workers) if mode == "threads" else Pool(workers)
    pool.map(int, range(workers))
    setup = _t.perf_counter() - start

    under_curve = sum(pool.map(count_under_curve, tasks))
    elapsed = _t.perf_counter() - start

    pool.close()
    pool.join()

    return elapsed, 4.0 * under_curve / (chunk * workers), setup


if __name__ == "__main__":
    samples = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SAMPLES
    mode = sys.argv[2] if len(sys.argv) > 2 else "processes"
    workers = int(sys.argv[3]) if len(sys.argv) > 3 else DEFAULT_WORKERS

    elapsed, estimate, setup = run(samples, mode, workers)
    print(f"elapsed: {elapsed:.6f} s")
    print(f"estimate: {estimate:.6f}")
    print(f"pool setup: {setup:.6f} s  mode: {mode}  workers: {workers}")
