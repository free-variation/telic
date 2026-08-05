# Water — 1.0-alpha release plan

The gate for 1.0-alpha, in priority order. Five items; four already have
PLAN.md entries (this file holds the release scope and its acceptance
criteria; PLAN.md keeps the full backlog, and an entry there shrinks or
vanishes as its item lands).

**Definition of done.** The language's identity claims are: the
documentation is the corpus (README: "designed to fit into a single LLM
prompt"); everything is verified (goldens, wasm parity, seeded
determinism); a small continuation substrate carries all control flow.
1.0-alpha is the point where outside readers and models test those claims
and where semantics freeze. The release ships when the divergence set
between what the docs state and what the binary does is empty, and when
both identity claims have artifacts: a doc harness (item 1) and a pack
(item 2).

**Not in scope.** Symbol collection, fuzzing, and FastCGI bind to the
long-lived-server case; the statistics program (PLAN §1–5), FFI
callbacks, kanren streams, `repr`, lazy sequences, and the loader hash
are additive and freeze no semantics. All stay in PLAN.md. One optional
item: `upcase`/`downcase` in the ASCII-first cut (PLAN.md, String
operations) is the only first-hour user-visible gap in the deferred set —
take it if the four required items land early, defer it otherwise.

---

## 1. Executable documentation

The identity claim "the docs are the corpus" is unverified: no harness
runs the README or reference examples, and the README logic example was
broken for an unknown span until a session happened to run it. For this
language a broken example corrupts the only training signal the language
has — a model reading the docs in-context generates from it directly.

### Semantics

1. Every runnable example in README.md and docs/reference.md executes
   under `make test` and its output is pinned.
2. A marker distinguishes runnable snippets from illustrative-only ones;
   unmarked fenced code defaults to runnable, so opting out is the
   visible act.
3. Nondeterministic examples (`wall-now`, unseeded draws) are either
   skip-marked or their output normalized; to settle at implementation,
   with normalization preferred (a skipped example is an unverified one).

### Implementation

1. Write `tools/gen-docs-tests.py` in the gen-help.py / gen-editors.py
   family: extract the `## A taste` fence from README.md and each marked
   snippet from docs/reference.md into generated `.h2o`/`.expected` pairs
   that tests/run.sh picks up like any other golden.
2. Wire it into the Makefile so `make test` regenerates the pairs before
   running the suite; generated pairs are never hand-edited, matching the
   help_table.c rule.
3. First run will surface currently-broken or nondeterministic examples;
   fix or mark each, inspecting every pinned line before accepting, as
   the golden rule requires.
4. Add the wasm dimension: doc-derived tests join the wasm suite, with
   native-only examples (FFI, statistics library, subprocesses) listed in
   tests/wasm-skip.txt like any other skip.

### Acceptance

1. `make test` fails when any runnable example's output drifts.
2. Discriminating case: re-introduce the historical README logic-example
   breakage in a scratch branch — `make test` must fail naming the
   extracted test; today's suite passes silently on it.
3. The README taste block's pinned output is byte-identical native and
   wasm (minus skip-listed native-only lines).

---

## 2. Language pack

README line 12 claims the doc system fits a single LLM prompt; no
artifact backs the claim. The pack is the distribution form for the
stated audience, and it supplies the release's acceptance test.

### Semantics

1. One generated file — `water-pack.md` (emit `llms.txt` as a copy or
   symlink per the emerging convention) — containing the whole language:
   the reference word tables, the README taste block, the tokenizer's
   self-delimiting rules, the idiom notes a generator cannot derive
   (locals are uninitialized by design; matrices for numbers, arrays for
   structure; resampling patterns; quotation-locals rules), and a few
   verified programs from examples/.
2. Generated, never written: the pack cannot drift from its sources.
3. Budgeted: the generator counts tokens (chars/4) against a ~50k target
   and names what to trim when it overflows, so growth is a visible
   decision rather than silent bloat.

### Implementation

1. Write `tools/gen-pack.py` beside gen-help.py and gen-editors.py,
   reading docs/reference.md, README.md, and a curated examples list.
2. Add `make pack` producing `water-pack.md`; add the token-budget check
   as a hard failure with the overflow named per section.
3. Once item 1 lands, embed a few of its verified input/output pairs as
   few-shot material (PLAN.md's open question — resolve as: include,
   because verified pairs are the cheapest correctness signal a pack can
   carry).
4. To settle at implementation: one pack or two tiers (lean core + full).
   Start with one; add a tier only when the budget forces a cut that
   costs generation quality.

### Acceptance

1. `make pack` output is under the token target and regenerating twice
   is byte-identical.
2. The acceptance test PLAN.md already names, made concrete: a fixed set
   of task prompts (≥10, spanning matrices, frames, regex, logic,
   datasets) given to a strong model with only the pack as context; the
   generated programs run under the item-1 harness. Record the pass
   count in the release notes; the test is the release's measure, so a
   regression in it blocks the tag.
3. Discriminating case: a task whose natural solution needs an idiom
   note (e.g. locals-are-uninitialized) — a model with the pack writes
   the assignment before read; a model without it plausibly does not.

---

## 3. `dynamic-wind`

The one item whose cost grows with adoption: unwind semantics are what
users write against, so they must be settled before users exist. Today a
`fail` backtrack or a `shift` capture unwinds past `ensure`/`with-db`
without running cleanup, and db/stream/FFI handles have no GC
finalization — the registry slot leaks silently. The full design is in
PLAN.md ("Guaranteed cleanup across every exit"); this section binds it
to the release.

### Semantics

(As specified in PLAN.md, normative for the release:)

1. `dynamic-wind ( before body after -- )`: `after` runs on every exit
   from `body` — normal return, `throw`, an interpreter error caught by
   `catch`, a `fail` backtrack, and a `shift` capture; innermost `after`
   first when nested regions unwind together.
2. `before` runs on the initial call and on every `resume` re-entry,
   outermost first; multi-shot `resume` runs the pair repeatedly by
   design.
3. Both run on the region's live data stack; on an unwind, `after` runs
   with locals and the trail already rewound to just outside its region.
4. `ensure`, `with-db`, and `with-stream` re-base onto `dynamic-wind`,
   closing their current hole with no interface change.

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
   (observable via `db-close` idempotence or a probe word). The current
   `catch`-based `ensure` gives the opposite answer; that divergence is
   the point of the feature.
3. Benchmark suite before/after shows no regression on the
   non-continuation rows (the dispatch path is untouched; verify, not
   assume).
4. Both native and wasm suites pass.

---

## 4. `ranks` midranks

docs/reference.md documents `correlation-spearman` as drifting from the
midrank definition on tied data. A statistics-pitched language releasing
a correlation that is wrong on ties, with the wrongness documented in
place of fixed, contradicts the project's own rule against documenting
limitations. Small; PLAN.md §2b.

### Semantics

1. `ranks` assigns tied values their midrank (the mean of the positions
   they occupy), so `correlation-spearman` matches the standard
   definition on tied data.
2. `correlation-spearman` answers `null` for a constant vector, agreeing
   with `correlation-pearson`'s convention.

### Implementation

1. Rework `ranks` in src/forth/statistics.h2o (embedded, wasm-capable):
   after the first `argsort`, average positions within runs of equal
   values instead of the second `argsort`. NaN handling stays as today
   (NaNs sort last; the correlations already delete incomplete pairs
   upstream).
2. Update the `ranks` and `correlation-spearman` reference rows to state
   present behavior only — the drift sentence goes away, no comparison
   with the previous implementation.
3. Regenerate affected goldens, inspecting every changed line.

### Acceptance

1. Golden on tied data cross-checked once against R's
   `cor(x, y, method = "spearman")`, with the R value and version noted
   in-test, per the test-data convention.
2. Discriminating case: `x = [ 1 2 2 3 ]` against a y with the tie
   broken — index-order ranks and midranks give different rho; the
   golden pins the midrank value.
3. Constant-vector input answers `null`, not a number.
4. Both native and wasm suites pass (the word is embedded, so both run
   the same definition).

---

## 5. Release mechanics

Absent from PLAN.md because it is process, not future work. LICENSE
exists; VERSION is 0.26.0 (src/c/water.h); the Makefile has no install
or release target.

### Implementation

1. Define an installation. `load-library` resolves `lib/<name>` beside
   the binary via `binary-dir`, and the statistics library locates
   `liblapacke_water.so` the same way — so an installation is: the
   `water` binary, `lib/`, and `liblapacke_water.so` in one directory
   (plus `data/` only for running the README examples verbatim). Either
   add a `make install` (PREFIX-parameterized, copying exactly that set)
   or state the copy in one README sentence; a `make install` is the
   stronger form because it is testable.
2. Add `make pack` output (item 2) and `water-pack.md` to the released
   set.
3. Set VERSION to the release version and tag the commit. The `water`
   word prints the version, so the installed binary self-identifies.
4. Release notes: the benchmark table's provenance line, the item-2
   acceptance pass count, and the platform pair (native, wasm) the
   suites passed on.

### Acceptance

1. From a clean checkout: `make && make test && make test-wasm &&
   make bench && make pack` all succeed.
2. Copy the installed set to a directory outside the repo; `water`
   starts from any cwd, `"statistics" load-library` and
   `"plot" load-library` load, and `help` answers — proving no
   repo-relative path survives in the binary's resolution.
3. The tagged commit's README benchmark table matches a full run on the
   release host, per the refresh rule.
