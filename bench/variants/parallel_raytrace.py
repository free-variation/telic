"""
raytrace, parallel — the counterpart of bench/variants/raytrace-parallel.h2o.

Scanlines are independent, so each row's contribution to the checksum (the sum
of its clamped colour bytes) is computed by a worker and the rows are summed,
which is what the water variant's `height iota ... pmap-reduce` does. The frame
loop repeats that map `loops` times, as the water variant repeats render-frame.

The geometry, surfaces and recursion come from pyperf_raytrace, so both
references run the same tracer; only the pixel loop of Scene.render and the
clamp of Canvas.plot are reproduced here, per row instead of per canvas. Each
worker process builds the scene once and keeps it.

Two modes:

  processes  one row per task in a multiprocessing.Pool
  threads    one row per task in a ThreadPool, where the GIL serialises the
             pure-Python tracer

The reported elapsed time covers pool creation as well as compute, because
water's pmap-reduce spawns its OS threads inside its own timed region; the
setup share is printed separately.
"""

import math
import os
import sys
import time as _t
from multiprocessing import Pool
from multiprocessing.pool import ThreadPool

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "pyperformance"))
import pyperf_raytrace as ref


DEFAULT_LOOPS = 10
DEFAULT_WIDTH = 100
DEFAULT_HEIGHT = 100

view = None


def build_view(width, height):
    scene = ref.build_scene()

    fov_radians = math.pi * (scene.fieldOfView / 2.0) / 180.0
    half_width = math.tan(fov_radians)
    half_height = 0.75 * half_width
    pixel_width = half_width * 2 / (width - 1)
    pixel_height = half_height * 2 / (height - 1)

    eye = ref.Ray(scene.position, scene.lookingAt - scene.position)
    vp_right = eye.vector.cross(ref.Vector.UP).normalized()
    vp_up = vp_right.cross(eye.vector).normalized()

    return (scene, eye, vp_right, vp_up, half_width, half_height,
            pixel_width, pixel_height, width)


def render_row(task):
    global view

    y, width, height = task
    if view is None:
        view = build_view(width, height)
    scene, eye, vp_right, vp_up, half_width, half_height, pixel_width, pixel_height, _ = view

    ycomp = vp_up.scale(y * pixel_height - half_height)
    row_bytes = 0
    for x in range(width):
        xcomp = vp_right.scale(x * pixel_width - half_width)
        ray = ref.Ray(eye.point, eye.vector + xcomp + ycomp)
        for channel in scene.rayColour(ray):
            row_bytes += max(0, min(255, int(channel * 255)))
    return row_bytes


def run(loops, width, height, mode, workers):
    tasks = [(y, width, height) for y in range(height)]

    start = _t.perf_counter()
    pool = ThreadPool(workers) if mode == "threads" else Pool(workers)
    pool.map(int, range(workers))
    setup = _t.perf_counter() - start

    checksum = 0
    for _ in range(loops):
        checksum = sum(pool.map(render_row, tasks))
    elapsed = _t.perf_counter() - start

    pool.close()
    pool.join()

    return elapsed, checksum, setup


if __name__ == "__main__":
    loops = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_LOOPS
    width = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_WIDTH
    height = int(sys.argv[3]) if len(sys.argv) > 3 else DEFAULT_HEIGHT
    mode = sys.argv[4] if len(sys.argv) > 4 else "processes"
    workers = int(sys.argv[5]) if len(sys.argv) > 5 else os.cpu_count()

    elapsed, checksum, setup = run(loops, width, height, mode, workers)
    print(f"elapsed: {elapsed:.6f} s")
    print(f"checksum: {checksum}")
    print(f"pool setup: {setup:.6f} s  mode: {mode}  workers: {workers}")
