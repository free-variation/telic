#!/usr/bin/env python
"""
Standalone CPython side of bm_regex_compile: compile the captured pyperformance
regex set (bm_regex_v8 + bm_regex_effbot, via bm_regex_compile) once, cold.

Each process invocation starts with an empty re cache, so a single pass over
the distinct patterns is a cold compile of each — the same thing the telic
bench measures, whose 1024-slot cache is also empty at startup. Neither side
can be looped in-process, since every pass after the first is all cache hits;
the harness samples across many fresh processes and takes the median instead.
"""

import json
import os
import re
import time

here = os.path.dirname(os.path.abspath(__file__))
patterns = json.load(open(os.path.join(here, "..", "fixtures", "regex_compile_patterns.json")))

t0 = time.perf_counter()
for pattern in patterns:
    re.compile(pattern)
elapsed = time.perf_counter() - t0

print(f"elapsed: {elapsed:.6f} s")
print(f"patterns: {len(patterns)}")
