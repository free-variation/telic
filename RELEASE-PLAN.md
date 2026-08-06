# Water — 1.0-alpha release plan

The gate for 1.0-alpha, in priority order; an entry vanishes as its item
completes. Items 1–2 have PLAN.md entries. Optional, if the three items
finish early: `upcase`/`downcase` in the ASCII-first cut (PLAN.md, String
operations).

---

## 1. Language pack — acceptance battery

1. A fixed set of task prompts (≥10, spanning matrices, frames, regex,
   logic, datasets) given to a strong model with only `make pack`'s
   `water-pack.md` as context; the generated programs run under the
   golden harness. Record the pass count in the release notes; a
   regression blocks the tag.
2. Discriminating case: a task whose natural solution needs an idiom
   note (locals-are-uninitialized) — a model with the pack writes the
   assignment before read.

---

## 2. `dynamic-wind`

### Semantics

As specified in PLAN.md ("Guaranteed cleanup across every exit"),
normative for the release:

1. `dynamic-wind ( before body after -- )`: `after` runs on every exit
   from `body` — normal return, `throw`, an interpreter error caught by
   `catch`, a `fail` backtrack, and a `shift` capture; innermost `after`
   first when nested regions unwind together.
2. `before` runs on the initial call and on every `resume` re-entry,
   outermost first; multi-shot `resume` runs the pair repeatedly by
   design.
3. Both run on the region's live data stack; on an unwind, `after` runs
   with locals and the trail already rewound to just outside its region.
4. `ensure`, `with-db`, and `with-stream` re-base onto `dynamic-wind`
   with no interface change.

### Implementation

1. Wind mark on the return stack: three cells (`after` xt, `before` xt,
   a mark of a new wind kind — the mark's kind field widens from one bit
   to two) plus a `wind_depth` counter.
2. `unwind_to` keeps its one-assignment truncation when `wind_depth` is
   zero; otherwise it walks from `rsp` down to the target, rewinding
   locals and the trail at each wind mark and running its `after`,
   saving and restoring `unwinding`/`unwind_target` around the re-entry.
3. `shift` runs each captured `after` innermost-first before truncating;
   the marks stay in the captured slice so both xts travel with the
   continuation. `resume` runs each spliced `before` outermost-first
   before jumping to the resume point.
4. Per-instruction dispatch, calls, `exit`, and `tailcall` gain no
   instructions; the one unconditional addition is the
   `wind_depth == 0` test in `unwind_to`.
5. Reference rows for `dynamic-wind`, `ensure`, `with-db`, `with-stream`;
   golden tests; PLAN.md entry shrinks to residuals.

### Acceptance

1. Goldens covering all five exit paths of Semantics 1, plus nested
   regions (inner `after` observed before outer) and a multi-shot
   `resume` (before/after pair count equals entry count).
2. Discriminating case: `with-db` around a body that `fail`s to an
   enclosing `amb` — the connection must be closed after the backtrack
   (observable via `db-close` idempotence or a probe word); the current
   `catch`-based `ensure` gives the opposite answer.
3. Benchmark suite before/after shows no regression on the
   non-continuation rows.
4. Both native and wasm suites pass.

---

## 3. Release mechanics

### Implementation

1. Add a `make install` (PREFIX-parameterized) copying the installed
   set: the `water` binary, `lib/`, and `liblapacke_water.so`, with
   `data/` only for running the README examples verbatim.
2. Add `make pack` output (item 1) and `water-pack.md` to the released
   set.
3. Set VERSION (src/c/water.h) to the release version and tag the
   commit.
4. Release notes: the benchmark table's provenance line, the item-1
   acceptance pass count, and the platform pair (native, wasm) the
   suites passed on.

### Acceptance

1. From a clean checkout: `make && make test && make test-wasm &&
   make bench && make pack` all succeed.
2. Copy the installed set to a directory outside the repo; `water`
   starts from any cwd, `"statistics" load-library` and
   `"plot" load-library` load, and `help` answers.
3. The tagged commit's README benchmark table matches a full run on the
   release host.
