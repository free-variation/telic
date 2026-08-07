# Water idioms

Compositions that recur across the embedded library, the loadable libraries,
the benchmarks, and working analyses. Each entry is quoted from a real
definition, named in parentheses. The reference documents single words; this
documents how words combine.

## Open literals

`[`, `{`, and `[<` push a mark; `]`, `}`, and `>]` gather whatever the stack
holds above it. Anything may run in between — the delimiters bracket a
computation, not a notation. Each position's value is whatever the code there
left on the stack.

```forth
{ :year y :month m 1 + } date>epoch                 \ an expression as a frame value (days-in-month)
[ width text size - 0 max2 spaces text ] "" join    \ the pad computed inside the array (pad-left)
[ "curl" "-s" url "-H" "x-api-key: " "ANTHROPIC_API_KEY" env + "-d" body ]
                                                    \ an argv assembled mid-literal (lib/claude.h2o)
[ maxx maxy maxz ] frame>json                       \ variables read into a result array (bench/float.h2o)
```

A frame literal of computed columns is ordinary code:

```forth
panel
{ :log_product_dollars panel@product_dollars ln
  :has_pd panel@pd_bookings 0 >
  :tenure_capped panel@tenure dup 10 > 10 mesh
} merge to panel
```

## Code layout

The unit of layout is the **sentence**: one value's story from construction
through consumption to its destination — a `to name`, a store, a print, an
`expect=`. Nothing in the syntax marks it; the layout does.

- A short sentence is one line: `panel@adds_program mean dup 0.1724 0.0005 expect-near`.
- A long sentence breaks at major clauses, continuations indented — subject
  first, a quotation or key list on its own line, the verb and destination
  last:

  ```forth
  2019 2024 range
      [: panel@adds_program panel@fy rot addition-rate :]
      map vector to addition-rate-by-year
  ```

- A frame beyond about three keys goes vertical — one `:key value` per line,
  each value possibly a whole computation, the closing `}` on its own line:

  ```forth
  { :fy_2020 risk-panel@fy 2020 eq
    :fy_2021 risk-panel@fy 2021 eq
    :fy_2022 risk-panel@fy 2022 eq
  } merge
  ```

- A long symbol array likewise, one key per line between `[` and `]`.
- A definition is headed and paragraphed: the `( a b -- c ) \ summary` line
  above the `:`, guard clauses first (ending in `exit`), blank lines between
  the body's paragraphs — guard, work, result.
- An analysis file carries section banners — `\ ---- title ----` — each
  section a sequence of checked sentences.
- A name a sentence stores into must be free of the dictionary, because `to`
  refuses to shadow a word: `to m` and `to log` both fail, `m` being the metre
  unit and `log` the base-10 logarithm. The short nouns are largely spoken
  for — `m` `s` `kg` `day` `week` are units, and `log` `min` `max` `sum`
  `mean` `size` `count` `first` `last` are words — so a variable
  takes a name that says what it holds: `price-column`, `daily-totals`.
  Locals live in their own scope and may shadow freely, but the compiler
  flags a scratch local that shadows a word when it is read before any store.

## Control structures compile

`if`/`else`/`then`, `begin`/`while`/`repeat`/`until`/`again`, and
`leave`/`continue` are compile-time words: they emit branch instructions into
the definition being compiled. They belong inside a `: … ;` or a `[: … :]`
body — at the top level of a file or the REPL there is no definition to emit
into, and the opener errors (`if: only valid inside a colon definition or
quotation`).

- A top-level conditional goes in a quotation, run on the spot:

  ```forth top-level-conditional
  variable checksum 7 to checksum
  [: checksum 7 = 0= if "checksum mismatch" throw then :] execute
  "checked" . cr
  ```
  ```output
  checked
  ```

- A top-level loop is `times`/`i-times` over a quotation, or a defined word.

- Definitions do not nest in branches: a `:` inside a top-level `if`/`then`
  (conditional definition) breaks for the same reason. Define the word
  unconditionally and branch inside it.

## The logic engine

Reach for unification early, not as a last resort: it is the shortest way to
destructure and test nested data, and backtracking search comes with it. The
machinery: `lvar` pushes a fresh logic variable, `| ?x |` declares one fresh
per call, `~` (`unify`) binds through the trail, `amb`/`fail` backtrack, and
a relation is plain frames and sets — `{ :rows <set> :index <frame> }` — so
everything composes with the ordinary collection words. For the data store
itself, pick by fit: the embedded SQLite (`":memory:" db-open`) is the better
engine when the work is aggregation, large tables, or multi-way SQL joins;
the fact database wins when rows are frames you already hold, patterns need
logic variables, or the result must compose with `map`/`filter`/`unify`. The
two interoperate — a `db-query` result is already relation-shaped.

- Unify for its bindings: `~ drop` asserts a structural equation and keeps
  only the side effects. A variable buried anywhere in the term comes out
  bound (tests/074):

  ```forth buried-bind
  lvar to A
  [( 1 2 3 4 5 null )] [( 1 2 A 4 5 null )] ~ drop
  A ? . cr
  ```
  ```output
  3
  ```

- A relation is clauses under `amb`: each clause is a quotation with fresh
  `?`-locals for its own variables, the arguments bound in by `ncurry`,
  alternatives tried in order. `[( H T )]` under unify is Prolog's `[H|T]`.
  Prolog's append, verbatim (tests/074):

  ```forth
  : lappend | >A >B >R |
    A B R [> a b r | a null ~ drop r b ~ drop :] 3 ncurry
    A B R [: | >a >b >r ?H ?T ?R1 |
      a H T cons ~ drop
      r H R1 cons ~ drop
      T b R1 lappend :] 3 ncurry
    amb ;
  ```

- `choose` is n-way `amb` over a cons list — a backtracking iteration that
  commits to the first element the continuation accepts:

  ```forth choose-commit
  [( 1 2 3 null )] [: dup 2 < if fail then . cr :] choose
  ```
  ```output
  2
  ```

- `matches?` is the non-destructive test — unify, roll the trail back, answer
  a flag — so pattern tests compose in straight-line code; `query` is exactly
  `matches?` under `filter` (logic.h2o):

  ```forth
  pattern [> row pattern | pattern row matches? :] curry filter
  ```

- Keep a result past backtracking by snapshotting: `copy` (fresh variables)
  or `reify` (unbound variables become canonical `:_0`, `:_1`, … — ground,
  storable, comparable).

- The fact database: rows are frames, a query pattern is an open record —
  shared keys must unify, a logic variable projects, extra columns are
  ignored:

  ```forth relation-query
  [ :name ] relation
  { :name :ann :age 34 } assert
  { :name :ann } query first frame>array . cr
  ```
  ```output
  [ :name :ann :age 34 ]
  ```

  `bulk-load` for bulk (one sort, not n inserts), `inner-join` to merge two
  relations on a shared column, `create-index` to index a fetched SQLite
  table for pattern queries. A database of several relations is just a frame
  keyed by relation name.

## Counted iteration and folds

`times`, `i-times`, and `fold-times` are the default loop forms; a bare
quotation under them dispatches straight into the body.

- Counted accumulation is `fold-times` — the accumulator never touches the
  data stack, and a primitive combiner runs with no dispatch:

  ```forth fold-times-sum
  0 [: dup f* :] ' f+ 5 fold-times . cr \ sum of squares 0..4
  ```
  ```output
  30
  ```

  The stack-accumulator form `0 swap [: + :] swap i-times` is the fallback
  when the body already leaves values.

- Indexed fill — initialize a segment or array by index (the shape of
  bench/float.h2o's `build-points`):

  ```forth indexed-fill
  variable xs
  4 double-segment to xs
  [: | >i sxi |
     i fsin to sxi
     xs i sxi !i drop
  :] 4 i-times
  xs 1 @i . cr
  ```
  ```output
  0.841471
  ```

- `begin i n < while … f++ i repeat` is the exception, reached only when the
  body must write the enclosing word's locals — a quotation cannot write a
  frame it does not own (bench/nbody.h2o, `energy`).

- First-element-as-init fold over a pairwise word (lib/statistics.h2o,
  `hstack-all`):

  ```forth head-init-fold
  [ [ 1 ] vector [ 2 ] vector [ 3 ] vector ]
  dup 1 skip swap 0 @i ' hstack reduce matrix>array . cr
  ```
  ```output
  [ 1 2 3 ]
  ```

- Chunked parallel sum: indices as the work list, one partial per worker,
  serial combine (bench/variants/leibniz-parallel.h2o):

  ```forth
  0 chunks 1- range
  chunks 1 ' partial pmap-ext
  0.0 [: f+ :] reduce
  ```

## Higher-order traversal

`map`, `filter`, `reduce`, and `each` are the default way over a collection;
an explicit loop appears only when the body needs an index or writes the
enclosing word's locals. The derived family — `find-first`, `any?`, `all?`,
`sort-by`, `partition`, `flat-map`, `group-with`, `nmap` — keeps common
traversals to one word each (`find-first` and `any?` short-circuit).

- A named word passes by tick where a quotation would only wrap it:

  ```forth
  : print-raw string>codepoints ' emit each ;   \ repl.h2o
  ' file-exists? find-first                     \ find-executable (io.h2o)
  cells ' quantity? all?                        \ column-from-cells (datasets.h2o)
  dataset values ' column>array map transpose   \ dataset-rows (datasets.h2o)
  ```

- Map-then-reduce pipelines read as one sentence — the widest string in an
  array (`padded-column`):

  ```forth
  strings ' size map 0 ' max2 reduce to width
  ```

- `nmap` zips parallel arrays through an n-ary quotation:

  ```forth nmap-zip
  [ 1 2 ] [ 10 20 ] ' + 2 nmap . cr
  ```
  ```output
  [ 11 22 ]
  ```

- `each` is the side-effect traversal — the element is consumed, nothing is
  left:

  ```forth
  [ [ "echo" "a" ] [ "echo" "b" ] ] 2 parallel-run [: :out @ trim . :] each
  ```

## Mask algebra

Comparisons on matrix and array operands answer 1/0 masks; the algebra of
masks, `where`, and `select-rows` replaces row loops.

- Filter rows by value — mask, where, gather (`addition-rate`):

  ```forth
  : addition-rate eq where select-rows mean ;
  ```

- Conjunction is `*`, negation is `0 eq`:

  ```forth
  moves@has_assessment a eq
  moves@has_core c eq *
  moves@has_supplemental s eq *
  where
  ```

  ```forth
  rows@state "" eq 0 eq where    \ the complement: rows whose state is set
  ```

- One index vector, many gathers — compute the row set once, gather every
  parallel column with it:

  ```forth
  panel@fy 2023 <= where to train-rows
  X train-rows select-rows
  y train-rows select-rows
  ```

- Index of the first match, then fetch by it (`path-odds`):

  ```forth
  eq where 0 @e
  ```

- `mesh` is conditional replacement without a loop; the moves:

  ```forth
  dup nan? 0 mesh          \ fill missing with 0
  dup nan? 9999 mesh       \ missing → sentinel, so a comparison can run
  dup 10 > 10 mesh         \ cap at 10
  dup 1e-10 < 1e-10 mesh   \ clamp away from zero (fit-logistic-ridge)
  ```

- Drop missing entirely (`drop-nans`): `dup nan? 0 eq where select-rows`.

## Strings are regex

Searching, testing, splitting, and replacing take PCRE patterns: `has?`,
`index-of`, `split`, `replace`, `match`, `match-all`. Construction and
slicing have their own words — `format`, `join`, `+`, `substring`, the pad
family, the codepoint words — but wherever a string is examined, the pattern
is the argument, and one short regex usually does it:

```forth
browser "^/" has?                  \ absolute path? (env-browser)
dup "\.h2o$" has? not if ".h2o" + then \ ensure a suffix (load-library)
: basename "^.*/" "" replace ;     \ last path component (strings.h2o)
: run " +" split start-process ;   \ tokenize on space runs (subprocess.h2o)
"x=42" "(\w+)=(\d+)" match         \ parse by capture → [ "x=42" "x" "42" ]
```

Anchor with `^`/`$` to test prefixes and suffixes.
The corollary: a pattern argument meaning a literal must escape regex
metacharacters — `"a.b" "." split` splits on every character, `"\." split`
on the dot.

## format

One word carries the text-building load, and its `{n}` placeholders index the
stack from the top, dropping the referenced positions when it runs.

- Report several values at once — pin them, then one template consumes both
  (dev_a4):

  ```forth
  program-adds mean dup 0.24142 0.0005 expect-near
  program-adds var dup 0.38089 0.0005 expect-near
  "program additions per account-year: mean {1:.3f} variance {0:.3f}" format print cr
  ```

- printf specs after the colon: `{0:.4f}` fixed precision, `{0:+.2f}` signed
  (dev_a4's marginal-effects table), `{0:04d}` zero-padded integer, `{0:8}`
  field width.

- Control characters: string literals are raw, so `{tab}` and `{nl}` are how
  tabs and newlines enter a string (README's taste block):

  ```forth
  1.5 250 "{0:d} ms{tab}{1:04.1f} s" format . \ 250 ms	01.5 s
  ```

- Ink directives style terminal output — `{red}`, `{bold}`, `{dim}`, reverted
  by `{plain}` — emitted only when stdout is a tty, so piped output stays
  clean (`help` renders its header this way):

  ```forth
  entry :effect @ entry :word @ "{bold}{0}{plain} {1}" format print-raw cr
  ```

- `"{0}" format` renders any one value to a string; feeding `string>symbol`
  synthesizes keys (`tsv-keys`):

  ```forth
  rows 0 @i size 1 swap range [: "col{0}" format string>symbol :] map
  ```

- Compose an error message, then throw it (`svd`):

  ```forth
  info if info "svd: dgesvd failed (info={0})" format throw then
  ```

## Quantities and units

Quantities compose through ordinary arithmetic; the idioms live at the
boundaries, and dates are the worked example — an instant is epoch seconds as
a quantity in `s`, so the units machinery is the date arithmetic.

- The boundary strip/attach pattern: dimension-blind code (a C primitive, a
  matrix kernel) sits behind a word that strips the unit going in — divide by
  one unit — and re-attaches it coming out — postfix the unit word
  (units.h2o):

  ```forth
  : wall-now (wall-now) s ;
  : epoch>date 1 s / (epoch>date) ;
  ```

  `magnitude` is the polymorphic strip when the unit may vary
  (`dataset>matrix` applies it per column).

- Counting durations by division: instant minus instant is a duration, and
  dividing by one unit counts it (`days-in-month`):

  ```forth
  { :year y :month m 1 + } date>epoch
  { :year y :month m } date>epoch
  - 1 day /
  ```

  The same shape shifts dates — `wall-now 2 hour +` is an instant, and
  `date-shift` adds exact components as `delta :weeks 0 @or week +`.

- Unit tests and transfers via `unit-of`: `unit-of 1 s =` detects an instant
  column (`column-type`'s `:datetime` branch); `x unit-of *` attaches one
  value's unit to another.

- Scaled subunit declaration — the minor unit as a rational fraction of the
  base, so same-dimension operands rescale automatically (`1 $ 50 ¢ +` is
  `1.5 $`):

  ```forth
  base unit $ 1 $ 100 / unit ¢
  ```

  lib/claude.h2o prices calls this way: token counts times a ¢-per-token
  rate, answered as `¢`, printable in either unit.

## Dataset shaping

- Derive columns by merging a frame literal of expressions onto the dataset
  (the open-literals entry applied):

  ```forth
  panel
  { :log_dollars panel@dollars ln
    :has_pd panel@pd_bookings 0 >
  } merge to panel
  ```

- The design-matrix pipeline: `select-columns` for the verbatim numerics,
  categorical levels via `indicators!`, indicator columns as `eq` masks,
  `with-intercept`, then the matrix — keeping the key array so coefficients
  stay addressable by name:

  ```forth
  dup keys dup -rot dataset>matrix
  ```

- The checked pipeline: every materialization is pinned immediately with
  `expect=` / `expect-near`, so an analysis file is its own regression test:

  ```forth
  addition-panel n-rows 36443 expect=
  addition-panel@adds_program mean dup 0.1724 0.0005 expect-near
  ```

## Quotation context

How values reach a quotation body, beyond its own locals.

- Park-and-pick: leave context below a combinator's operands and read it at a
  documented depth; `nip` the leftovers after. The depth comment above the
  word is part of the idiom (`dataset>matrix`):

  ```forth
  \ the dataset arrives below cols, so the quotation reaches it at depth 2 under map
  [: 2 pick swap @ magnitude dup matrix? if as-column else vector then :] map nip
  ```

- Curry a fixed context into a mapped word (`tsv>db`):

  ```forth
  rows 1 skip db statement ' insert-row 2 ncurry map drop
  ```

- Skeleton plus mapper injection: write the loop once taking a mapper xt;
  serial and parallel are one-line instantiations. Sound because each work
  cell is pre-curried and pre-seeded, so the mapper cannot change the result:

  ```forth
  : bootstrap ' map bootstrap-with ;
  : pbootstrap ' pmap bootstrap-with ;
  ```

- `>side … side>` carries a value across code that owns the stack: a handler
  across `catch` (`try-catch`), a shared FFI handle across a block of
  definitions (lib/statistics.h2o), a key xt under a fold via `side-peek`
  (`group-with`).

- Extend a word by type without breaking early binding: capture the old xt in
  a constant, redefine with a type test in front (datasets.h2o does this for
  `select-rows`, `dim`, `filter`, `map`):

  ```forth
  ' select-rows constant (matrix-select-rows) internal

  : select-rows
      over frame? if dataset-select-rows exit then
      (matrix-select-rows) execute ;
  ```

## Continuations and generators

`reset`/`shift`/`resume` are the substrate; exceptions, cleanup brackets, and
coroutines are short compositions over them (exceptions.h2o, generators.h2o).

- Resource brackets guarantee cleanup on both exits — the handler or resource
  rides the side stack across the unwind, which the return stack does not
  survive:

  ```forth resource-brackets
  : ensure >side catch side> execute if throw then ;
  ":memory:" [: "create table t(x)" [ ] db-exec . :] with-db cr
  "echo hi" run :out @ [: read trim print :] with-stream cr
  ```
  ```output
  0
  hi
  ```

- A generator is a word that `yield`s; drive it with `gen-take` (collect n
  values) or `gen-each` (consume until it falls off):

  ```forth generator-drive
  : odds 1 yield 3 yield 5 yield ;
  ' odds 3 gen-take . cr
  ' odds [: . :] gen-each cr
  ```
  ```output
  [ 1 3 5 ]
  1 3 5
  ```

- `start-generator` exposes the raw step for hand-driven iteration — the
  yielded value and a resumable continuation, `resume` for the next; the
  continuation is multi-shot, so a retained copy replays.

## Frames as records

- `@or` reads with a default in one probe — absent keys are ordinary, not
  errors (`help`, `date-shift`):

  ```forth
  entry :examples [ ] @or to examples
  delta :weeks 0 @or week +
  ```

- A path locator gets and sets through nesting, and `!` vivifies the
  intermediate frames:

  ```forth path-vivify
  { } /a/b 5 ! /a/b @ . cr
  ```
  ```output
  5
  ```

- A search path extracts from a tree in one call — `*` any child, `//` any
  depth, `[k>v]` predicates:

  ```forth search-path
  { :a { :n 1 } :b { :n 2 } } /*/n select-values . cr
  ```
  ```output
  [ 1 2 ]
  ```

- Key arithmetic runs through sets — difference, then back to an array
  (`ordered-columns`):

  ```forth
  dataset keys array>set leading-columns array>set difference set>array
  ```

  and the uniformity test is a set collapse (`frames>dataset`):

  ```forth
  rows ' keys map array>set size 1= not if
      "rows have differing keys" throw
  then
  ```

## Numeric kernels

The register for hot loops: locals, unsafe f-words, segments — the shapes the
compiler's fusion targets.

- Gather–compute–writeback: hoist reads into locals, run fused arithmetic,
  store with `!i drop` (bench/float.h2o, `normalize-points`):

  ```forth
  xs i @i to xi ys i @i to yi zs i @i to zi
  xi xi f* yi yi f* f+ zi zi f* f+ fsqrt to norm
  xs i xi norm f/ !i drop
  ```

- In-place matrix chains avoid allocation in an iteration
  (bench/variants/mandelbrot-matrix.h2o, `step`):

  ```forth
  zi zr *! 2.0 *! c-imag +! drop
  ```

- Branch-free mask accumulation — accumulate a condition instead of testing
  per element:

  ```forth
  escaped zr2 zi2 + 4.0 > +! drop
  ```

- Coordinate grids as rank-1 products (`setup`, same file):

  ```forth
  1.0 ones-col cr-row 0.0 n n 0-matrix dgemm-nn
  ```

- Bulk field unpack and writeback: `destruct-to` into pre-declared globals
  with a reused target array; `to-slice!` stores several values in one call
  (bench/nbody.h2o, `pair-force`):

  ```forth
  b1 b1-targets destruct-to
  vx1 vy1 vz1 b1 3 3 to-slice! drop
  ```

- Verify the fusion, don't assume it: `' word see-compiled` shows the
  compiled cells — fused ops like `(lf+)`, `(ll*0!)`, `(=0branch)` confirm
  the loop compiled tight — and `timed` settles what the disassembly leaves
  open:

  ```forth fusion-check
  : sc-demo 1.5 2.5 f+ ; ' sc-demo see-compiled
  ```
  ```output
  : sc-demo   \ 5 cells
   0: (lit) 1.5
   2: (lf+) 2.5
   4: exit
  ;
  ```

## Writing tests

The test vocabulary (test.h2o) rides on `catch`/`throw`: an assertion throws
on failure, `test` catches and tallies, `test-report` throws at the end when
anything failed — so a test file run as a program exits non-zero.

- One assertion word per claim shape:

  ```forth
  flag expect                        \ truthy, else "expectation was false"
  actual expected expect=            \ deep structural =; throws "expected X, got Y"
  actual expected tolerance expect-near \ |actual − expected| ≤ tolerance;
                                     \ matrix/vector operands: largest element-wise gap
  xt expect-throws                   \ passes iff the quotation throws
  ```

- Group claims with `test` — a name and a quotation; the stack is restored and
  the run continues past a failure — and bracket the file with
  `new-tests`/`test-report`:

  ```forth test-group
  new-tests
  "adds" [: 3 4 + 7 expect= :] test
  "rejects a string count" [: [: [ 1 ] "x" ' + reduce :] expect-throws :] test
  test-report
  ```
  ```output
  ok adds
  ok rejects a string count
  2 passed, 0 failed
  ```

- Pin values as they materialize — `dup … expect-near` asserts and keeps the
  value for the sentence that follows, so an analysis file is its own
  regression test (dev_a4):

  ```forth
  program-adds mean dup 0.24142 0.0005 expect-near
  program-adds var dup 0.38089 0.0005 expect-near
  "program additions per account-year: mean {1:.3f} variance {0:.3f}" format print cr
  ```

- Seed anything random first — `42 seed` — so expected values are exact, and
  the vector form of `expect-near` compares whole results at once:

  ```forth
  addition-rate-by-year
  [ 0.1363 0.1300 0.1759 0.1693 0.1483 0.2250 ] vector 0.0005 expect-near
  ```

## External systems

- Subprocess capture: `run-result :out @ trim`.
- Transaction bracket (`tsv>db`):

  ```forth
  db "BEGIN" [ ] db-exec drop
  rows 1 skip db statement ' insert-row 2 ncurry map drop
  db "COMMIT" [ ] db-exec drop
  ```

- Retry-then-rethrow (lib/claude.h2o, `elicit-with-retries`):

  ```forth
  begin
      messages ' try-call curry catch
      0 = if exit then
      -- attempts
      attempts 1 < if throw then
      drop
  again
  ```

- Fallback chain over `none` — try sources in order, each `dup none?` guard
  either exits with the hit or drops and falls through (`xgb-lib-path`,
  `env-browser`):

  ```forth
  "XGBOOST_LIB" env dup none? not if exit then drop
  install-paths [: file-exists? :] find-first
  dup none? if drop "libxgboost.so" then
  ```

- The LAPACK call shape: `copy` the inputs (LAPACK overwrites its arguments),
  `matrix>pointer` each operand, out-parameters as segments, then check
  `info` (`fit-linear`):

  ```forth
  mat copy to a
  1 int-segment to rank
  ...
  (dgelsd) to info
  info if info "fit-linear: dgelsd failed (info={0})" format throw then
  ```
