"""
binary-trees — Computer Language Benchmarks Game.

Allocation / GC stress: build and traverse many short-lived binary trees.
Matches bench/pyperformance/binary-trees.telic (same tree shape and checksum).
"""

import sys
import time as _t


DEFAULT_ARG = 16


def make_tree(depth):
    if depth == 0:
        return None
    return (make_tree(depth - 1), make_tree(depth - 1))


def tree_check(node):
    if node is None:
        return 1
    left, right = node
    return 1 + tree_check(left) + tree_check(right)


def run_trees(max_depth):
    min_depth = 4
    total = 0
    total += tree_check(make_tree(max_depth + 1))
    total += tree_check(make_tree(max_depth))
    d = min_depth
    while d <= max_depth:
        iterations = 1 << (max_depth - d + min_depth)
        s = 0
        for _ in range(iterations):
            s += tree_check(make_tree(d))
        total += s
        d += 2
    return total


if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_ARG
    sys.setrecursionlimit(10000)
    loops = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    t0 = _t.perf_counter()
    for _ in range(loops):
        result = run_trees(n)
    print(f"elapsed: {_t.perf_counter() - t0:.6f} s")
    print(f"checksum: {result}")
