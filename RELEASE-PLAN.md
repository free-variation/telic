# Telic — 1.0-alpha release plan

The gate for 1.0-alpha, in priority order; an entry vanishes as its item
completes. Optional, if the gate finishes early: `upcase`/`downcase` in the
ASCII-first cut (PLAN.md, String operations); the buffer form of
`XGBoosterSaveModel`, so a booster travels inside a serialized frame.

---

## 1. Serialization depth

`bytes>value` roots each in-progress container on the GC root stack, so a
value nested past 62 levels writes but cannot be read, reporting `gc roots
exhausted`. `value>bytes` recurses per level with no guard and takes SIGSEGV
above about 50000 levels of array or frame nesting (a cons tail escapes only
because the compiler makes it a tail call).

### Implementation

1. Park the in-progress container on the data stack, which the collector
   scans and which holds a million slots, instead of `gc_root_push`.
2. Guard the writer with the depth cap printing and `val_cmp` already use,
   erroring `structure too deeply nested` rather than overflowing.

### Acceptance

1. A 10000-level nested frame round-trips through `save-value`/`load-value`.
2. A synthesized 200000-level file errors from both words rather than
   crashing; both cases join `tests/102_serialize.telic`.

---

## 2. Release mechanics

### Implementation

1. Add a `make install` (PREFIX-parameterized) copying the installed
   set: the `telic` binary, `lib/`, and `liblapacke_telic.so`, with
   `data/` only for running the README examples verbatim.
2. Add `make pack` output and `telic-pack.md` to the released set.
3. Run `make acceptance` and read the failures: fix what is a pack gap,
   record what is not.
4. Set VERSION (src/c/telic.h) to the release version and tag the
   commit.
5. Release notes: the benchmark table's provenance line, the platform
   pair (native, wasm) the suites passed on, and the acceptance pass@1
   with its model and date — a description of that run, not a
   threshold, since the tasks and the pack text were developed against
   each other. Name `lib/mcp.telic` as experimental: it has passed no run
   against a real MCP client, and no in-process test can stand in for
   one.

### Acceptance

1. From a clean checkout: `make && make test && make test-wasm &&
   make bench && make pack` all succeed, and `make test-libs` on a host
   with LAPACK and libxgboost.
2. Copy the installed set to a directory outside the repo; `telic`
   starts from any cwd, `"statistics" load-library` and
   `"plot" load-library` load, and `help` answers.
3. The tagged commit's README benchmark table matches a full run on the
   release host.

---

## Decisions to take before the tag

1. Symbol order is interning order, so a new symbol in the embedded library
   reshuffles frame key order and every golden that prints one. Either accept
   it and keep the embedded library free of short generic symbol names, or
   order symbols by name and pay a `strcmp` on each frame lookup.
2. `exact_to_double`'s method, `EXACT_POWER_BIT_CAP`'s value, and citations
   like Acklam's `qnorm` have no home under the no-comments rule: reference
   rows carry behavior and PLAN.md carries constraints, neither carries
   algorithm provenance. Decide where it goes, or decide it goes nowhere.
