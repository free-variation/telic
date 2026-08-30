# Telic — code conventions

## Working method
- grep locates; it never concludes. Before asserting what code *does* — that it
  duplicates something, that it is unused, that it can be removed, that it
  handles some case — read the definition. If a sentence names a function or
  word and claims something about its behavior, that definition must have been
  read, not pattern-matched. Black-box probing counts as pattern-matching: it
  finds the cases you thought to test, not the ones in the code.
- Legitimate for grep: which files mention a name, line counts, locating a
  symbol before reading around it. Not legitimate: any question whose answer is
  *the complete set* of something — the types a word accepts, the callers it
  has, the cases a switch covers. A pattern search cannot establish
  completeness, because the pattern encodes the guess.
- The files here are small — no file is long enough that reading it is the
  expensive option.

## C (src/c)
- No comments anywhere in .c/.h, with one exception: telic.h may carry
  single-line `//` section headers naming the region below them, above all
  for the per-file blocks of p_* declarations. Constraints a future change
  must honor go in PLAN.md under "Source invariants".
- Tabs for indentation. One statement per line; one declaration per line.
- Descriptive names, nouns not adjectives: `sorted_vector`, never `sorted`;
  no filler names (result, tmp except swap temps, found, val).
- Counts carry n_: n_rows, n_elements, n_terms.
- Hoist repeated field accesses into named locals before use.
- Blank lines separate paragraphs: guard clauses, allocation+check, result.
- Runtime error messages carry no word prefix — the error trace names the
  failing op ("in sort ← rank") and catch delivers `{ :message :trace }`.
  Lowercase, ASCII only ("expected a vector (nx1 or 1xn); got %dx%d").
  Compile-time diagnostics (compiler.c) keep their construct prefix;
  filename contexts keep the filename.
- Multi-statement macros wrap in do { } while (0) unless they exist to leak
  a binding (LOWER_BOUND pattern).
- Domain files own single-representation kernels; words.c owns
  tag-dispatched words; subsystem-sized dispatch families get their own
  file (indexing.c, superwords.c).
- Nothing on the dispatch hot path: DISPATCH/DISPATCH_REGISTERS/docol and
  the REQUIRE macros gain no instructions.

## C file map (src/c)
reference.md rows name each word's owning file; telic.h declarations group
by file in SRCS order. What each file is:
- core.c — the machine: dispatch loop, dictionary and word headers
  (create_header/emit), GC, threaded-code internals (literal/branch/
  locals), execute_cfa/trampolines, load/reload/save, and every
  define_primitive registration.
- words.c — tag-dispatched words: arithmetic/comparison via binary_op/
  unary_op, stack ops, printing, format/interpolate, execute/curry, RNG
  (seed/random/resample-indices-ext), delimited-continuation prompts, the
  complex scalar kernel and its literal parse.
- compiler.c — compile-time words (: ; control flow, locals lists,
  quotations), token scanning, partial-definition rollback.
- collections.c — arrays, sets, frames (path walk, @or), pairs/cons,
  JSON, the literal readers, sample/shuffle.
- matrix.c — matrix kernels: element/column access, reductions,
  cumulative-sum, reshape/transpose/vstack/submatrix, where/select-rows,
  sort/argsort (quicksort + radix instantiations), dgemm.
- statistics.c — stats kernels: var, quantile, correlation-kendall
  (radix pair sort + merge exchange count).
- indexing.c — the @i/!i index/store dispatch family across arrays/
  segments/slices, plus quickened "(word.tag)" specializations.
- functional.c — map/nmap/filter/reduce and the parallel worker pool
  (pmap/pfilter/pmap-reduce, num-cores).
- superwords.c — compile-time fused float/vector op chains (vf*/vvf+
  family).
- strings.c — PCRE2 regex (match/split/replace), substrings, codepoints.
- io.c — files, env, cwd, TSV, stream read/write, stdin/stdout/stderr.
- logic.c — logic variables, unify, the trail, amb/backtracking (the
  fact database is forth: src/forth/logic.telic).
- database.c — SQLite (db-open/db-exec/db-query).
- foreign.c — libffi (ffi-open/ffi-function, matrix>pointer).
- dimension.c — units and quantities (unit/base, dimensional
  arithmetic).
- time.c — wall-clock instants, date frames, ISO/strftime formatting.
- exact.c — arbitrary-precision rationals: the mag_* functions on 32-bit-word
  digit arrays (add/mul/divmod/gcd/decimal/double conversions), the exact
  algebra behind the polymorphic words' T_EXACT branches, the `p/q` and
  oversized-integer literal parse, rationalize, and
  float>exact/exact>float/numerator/denominator.
- serialize.c — the binary value format: writer, reader, the back-reference
  table that preserves sharing and cycles (value>bytes/bytes>value).
- platform_posix.c / platform_wasi.c — subprocesses, threads, timing per
  platform (wasi stubs the process words).
- help_table.c — generated by gen-help.py from docs/reference.md; never
  edited.

## C word implementation
- Words are void p_<name>(DISPATCH_ARGS), registered in core.c. Name
  mapping: > becomes _to_ (p_string_to_chars), ? drops (p_has), ! becomes
  _set/_store/_inplace or drops (p_env_set, p_slice_store, p_add_inplace,
  p_set_add); libc collisions take a trailing underscore (p_emit_).
- Choosing a new word's form — test these predicates in order, take the
  first that holds, and state the chosen form and predicate before
  writing the body:
  1. Re-enters the interpreter (execute_xt/call_step) or tail-calls
     push-style shared helpers (binary_op/unary_op/push_quantity) →
     interp-state: POP_*/PEEK_* + push, end DISPATCH(interp).
  2. Allocates GC objects while heap operands are live → PEEK_*_AT
     reads (operands stay stack-rooted), register exit.
  3. Otherwise → REQUIRE_STACK_DEPTH + chain_sp reads +
     REQUIRE_CHAIN_TAG, end DISPATCH_REGISTERS.
  Never choose a form by imitating neighboring words; neighbors may
  differ in exactly the property the predicate tests.
- Words are register-threaded by default: work on chain_ip/chain_sp, open
  with REQUIRE_STACK_DEPTH/ROOM, end DISPATCH_REGISTERS; kernels return
  handles for the word to write into chain_sp. A word may be interp-state
  (POP_*/PEEK_* + push, end DISPATCH(interp), blank line before it) only
  for one of two decidable reasons: it re-enters the interpreter
  (execute_xt/call_step — the stack moves under it), or its tail calls
  push-style shared helpers (binary_op/unary_op/push_quantity). Never by
  convenience or perceived cost. Early success exits dispatch inside the
  branch.
- SYNC_REGISTERS before fail/allocation only in ops that read operands
  from chain_ip (their resume point diverges from the entry spill) or
  that deliberately expose an adjusted sp. A plain word's registers equal
  the spill until its final DISPATCH_REGISTERS, so its body carries no
  syncs — and it must not allocate after writing result slots above sp
  (they sit above the spilled dsp, invisible to GC).
- Float fast path first in polymorphic words: stay in registers, exit via
  DISPATCH_REGISTERS in the if; then SYNC_REGISTERS (sp minus consumed
  operands) and the tag chain.
- Errors: the detector calls fail(interp, ...) then bare return. Callers
  re-check with `if (interp->error_flag) return;` after every fallible
  call. Helpers signal with -1 (handle-producers), 0 (did-it-work), or
  NULL (pointer-producers) — fail was already called; the sentinel only
  stops the caller.
- POP_* for consumed scalars; PEEK_*_AT for heap operands that must stay
  stack-rooted across allocation, committed at the end by dsp adjustment
  or in-place overwrite. Raw Val is x_val, unwrapped is x.
- gc_root_push right after allocation, pop before the final stack write;
  every error path pops before returning; roots never live across
  DISPATCH. Zero a fill-incrementally array before rooting it.
- static by default; only p_* words and telic.h API are extern. Helpers
  sit directly above their first user; forward-declare only recursion.
- Word families are SHOUT-CASE macros taking (c_name, "word-name", op),
  instantiations listed immediately below, one per line.
- Quickened specializations guard their assumed tags and demote via
  RETARGET_OP(generic) + MUSTTAIL on mismatch; the generic retargets to
  them at the matching branch. Register each as an internal primitive
  named "(word.tag)" beside the (@i.array) block in core.c — that covers
  see-compiled. Same operand width as the generic, always.
- int for lengths, indices, handles; size_t only as explicit casts at
  malloc/memcpy sites; int64_t to guard overflow before clamping. All
  casts explicit.
- Grow by doubling from a small constant through a checked realloc temp;
  GROW_IF_FULL for arena arrays, GROW_IF_FULL_SYS for malloc-owned.
- Cleanup is inline per error path (free/fclose/finalize/gc_root_pop
  before return); no goto.
- Message formats: "expected X; got %s" (literal type phrase); half-open
  ranges "[%d, %d) out of bounds for length %d"; "(max %d)".
- Printing words fflush(stdout) before DISPATCH. Unused params silenced
  with (void)x; at the top.
- Typedefs CamelCase; enum members domain-prefixed SHOUT; kernel context
  structs XxxContext built with designated initializers. File-scope state
  grouped at top, static, domain-prefixed. static inline for hot
  predicates, always_inline only for the hottest telic.h helpers,
  MUSTTAIL for tail recursion. Near-duplicate words factor through a
  callback + op-string helper, leaving p_* as two-liners. File-local
  #define tunables sit beside their use; file-private POP_X macros copy
  the telic.h family's shape.

## telic.h layout (single-line section headers only; this is its table of contents)
1. Guard, VERSION, includes, cell typedef.
2. Capacity constants grouped by subsystem: dictionary/pools, stacks/
   locals, GC/arena, workers, logic, regex/JSON, databases, trace/print.
3. Value model: Tag, Val, NaN-box macros, make_* constructors.
4. Object model: ObjectKind, Object, segment inlines, Pair.
5. Memory: HandleSpace, PairPool, Arena, alloc contexts, GROW_*/OBJECT_AT
   /MAT macros.
6. Program state: QuotationSpan, Vocabulary, Interpreter, Compiler,
   DISPATCH_ARGS, cfa_handler, WORD_* macros, CallContext, HelpEntry.
7. Word-body macros: POP_*, NEW_*, PEEK_*, FRAME_LOOKUP, LOWER_BOUND.
8. Declarations grouped by owning .c file in SRCS order, alphabetical
   within a file: API functions first, then all p_* words.
9. Tail: inline functions whose bodies call declared functions (push/pop,
   rpush/rpop, gc_root_*, truthy, frame_walk, call_step).
New constant → its subsystem cluster in 2. New type → lowest layer that
can express it. New declaration → its file's block, alphabetical slot. A
new inline that calls functions → the tail.

## Forth (src/forth, lib/, tests)
- The embedded library is src/forth/*.telic, concatenated in FORTH_SRCS
  order (Makefile) and burned into the binary. Binding is early, so a
  word must be defined in an earlier file (or earlier in the same file)
  than every use; changing the order changes what compiles. A new
  embedded word goes in the file owning its domain, and its reference
  row's prefix names that file. lib/ holds the loadable libraries.
- Stack-effect comment line above each definition: ( a b -- c ) \ summary.
- Markers postfix after ; — inline, internal.
- Plumbing words are marked internal.
- C escape hatches are parenthesized primitives wrapped by the public
  word: `: wall-now (wall-now) s ;`. Fully-parameterized primitives carry
  -ext, wrapped by a defaulting word.
- Lib overrides a C word by redefinition (statistics.telic's dgemm-*).
  Binding is early: earlier compilations keep the old target; a
  self-reference recurses, so capture the old xt with `'` first if
  needed.
- LAPACK-free stats live in the embedded library (wasm-capable);
  statistics.telic accelerates one by redefining it in toto (early
  binding makes partial masking useless). The word's golden runs native
  (masked) and wasm (unmasked) and must agree, pinning both copies.
- Locals: `>name` receives from the stack at entry, bare names are
  scratch that reads as null until assigned; quotations receive with
  `|> a b |`.
- Counted loops inside a definition are `start limit delta do k … loop`:
  the body compiles inline, so it reads and writes the enclosing word's
  locals, and each iteration's loop control is one instruction. `times` /
  `i-times` over a quotation serve the top level and callers holding an
  xt. `begin`/`while` remains for loops whose iteration count is not
  fixed at entry (a fixpoint pass, a bit scan).
- Quotations: write them bare (`[: ... :]`); receive into locals
  (`[>`/`[: |`) when a value is reused past a `dup` or the quotation
  crosses `curry`. Measured: bare is the fastest form — under
  map/times/i-times it dispatches straight into the body, and
  `local swap @i` fuses to one op ((@i.swap.l0)/(@i.swap.l1)) —
  gather-elements 32%, the rows>dataset inner 15%; frames cost nothing
  per iteration either (built once per loop, refilled per element), so
  the choice is brevity, not speed, except where the swap fusions
  apply. Check fusion with see-compiled; never trust a derived fusion
  analysis over a timing — frame changes renumber the locals levels
  ops compile against.

## Docs and generated files
- PLAN.md is a focused list of FUTURE WORK, written imperatively: no
  completed work, no status narration, no discussion of present or past
  state. When a feature lands, its entry shrinks to the residuals or
  vanishes.
- README describes the language as it is: terse present-tense capability
  statements, one line per feature. No past narratives, no reflections or
  design history — detail belongs in docs/reference.md. No parenthetical asides
  that answer an objection nobody raised, and no per-word behavioral detail:
  the README says what exists, the reference says how it behaves. Measurements appear
  only in the Benchmarks section, as the newest bench report's standalone
  table plus the provenance line naming host, versions and reps; refresh it
  from a full run, never edit a cell by hand.
- docs/reference.md (built-ins and embedded src/forth words) and
  docs/reference-libraries.md (loadable lib/*.telic words) are the source of
  truth: gen-help.py (help table, automatic in make) reads both and merges
  them into one name-sorted table, so a lib word answers to help/man/apropos
  once its library is loaded; gen-editors.py (make editors) reads
  reference.md only; gen-docs-tests.py (automatic in make test) extracts
  the runnable (```forth) documentation fences into golden-test inputs,
  routing load-library sections to tests/lib/, and each reference
  section's ```forth <word> / ```output example pairs into generated
  golden trios (tests/9*_docs_*, gitignored). A word's example pairs sit
  after its section's table — self-contained, seeded, newline-terminated,
  both stacks left empty — and surface in help/man as :examples. The
  explanatory docs (continuations, logic, regression, idioms) carry the
  same pair format with a free-text label instead of a word name; all of
  one doc's pairs become one trio (tests/901_docs_<stem>), indented
  fences allowed inside list bullets, and a bare ```forth fence there is
  display-only (sketches and fragments stay untagged). gen-pack.py
  (make pack) concatenates the README taste, both references, and
  idioms.md verbatim into telic-pack.md + llms.txt (gitignored), with a
  hard-fail chars/4 token budget. Never
  hand-edit a generated file: help_table.c,
  forth-words.txt, repl_highlight_groups.h,
  editors/vim/syntax/telic.vim, editors/vscode/syntaxes/telic.tmLanguage.json,
  tests/090_readme_taste.telic, tests/lib/090_readme_taste_fit.telic,
  telic-pack.md, llms.txt.
  The rest of editors/ (vim indent/, ftplugin/, ftdetect/, and vscode's
  package.json, language-configuration.json, README.md) has no generator and
  is edited by hand.
- Every built-in/embedded word gets a reference.md row; a loadable-library
  word gets a reference-libraries.md row. The words canary group
  "undocumented" must stay empty (it checks built-in/embedded words only;
  loaded lib words show under "this session").
- New words: reference row (reference.md for built-in/embedded,
  reference-libraries.md for lib/) plus an example fence pair, README
  mention if user-facing, golden test, wasm suite run.

## Tests
- Golden pairs in tests/ (see run.sh); regenerate with ./telic -b <
  tests/NNN_name.telic > tests/NNN_name.expected — inspect every changed
  line before accepting. A golden line you cannot explain is a bug report,
  not a baseline: accepting one makes the defect the contract, and the test
  then protects it. `009_see` pinned `:    \ 9 cells` — a definition with no
  name — for as long as it took someone to ask why.
- A test that reads stdin adds tests/NNN_name.stdin; both runners then pass
  the program as a file argument and feed that file, so regenerate with
  ./telic -b tests/NNN_name.telic < tests/NNN_name.stdin > tests/NNN_name.expected.
- Tests that `load` a lib/ library needing external deps (LAPACK, xgboost)
  live in tests/lib/ and run via `make test-libs` (tests/run-libs.sh),
  native-only and excluded from `make test` so the core suite builds without
  those deps. A new such test goes in tests/lib/, not the wasm-skip list.
  Pure-forth lib tests (e.g. lib/plot.telic) stay in the core suite.
- Seeded RNG for anything random; both native and wasm suites must pass.
- Header comment names the word, stack effect, semantics. Sections split
  with `\ === title ===`. Every output line carries an aligned trailing
  expected-value comment. Error cases grouped at the end with the reason
  parenthesized. `clear` between sections.
