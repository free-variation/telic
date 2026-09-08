# Tracing in Telic

`trace ( xt patterns -- … )` runs a quotation and prints, to stderr, one line
for every op the run reaches before that op executes: the op as `see-compiled`
renders it, a bar, and the data stack at that moment, deepest first, the top
four values with `…` when there are more. `patterns` is an array of regex
strings: the first selects ops by their op text, any further ones keep a
selected op only when one of them matches the whole line; `[ ]` prints them
all. The quotation's own output goes to stdout as usual, so the two streams can
be kept apart or read together.

```forth trace-basic
[: 3 4 + :] [ ] trace . cr
```
```output
> (lit) 3                 |
> (lit) 4                 | 3
> +                       | 3 4
> exit                    | 7
7
```

The trace is the actual execution, not a reading of the source. Everything the
quotation reaches is included: the words it calls, the bodies a combinator runs
per element, `catch` handlers, code behind `execute`. Two kinds of cell are left
out because they are interpreter plumbing rather than program: the trampoline
cells a combinator or `execute` dispatches through, and the header cell of a
body, which is never dispatched itself.

A run of any size prints too much to read, which is what the patterns are for.
The first pattern is tested against the op text alone — `"^sort"` selects the
`sort` ops, `"."` every op. Any further patterns are tested against the whole
rendered line — op text, bar, stack window — and a selected op prints only when
one of them matches, so `[ "^exit" "\| 27" ]` shows the exits that leave 27 on
top and `[ "." "nan" ]` every op with a NaN in the stack window. The patterns
see each stack value in full; the printed line cuts a value longer than 48
characters to that width with `…`, except the one holding the match, and a
filtered line prints between blank lines with the match highlighted on a
terminal. A word or quotation that a combinator runs is announced by name
before its body, so `[ "^fit-program" ]` finds a word called only through
`map`.

```forth trace-filtered
: sq-traced | x | x x * ;
[: 5 sq-traced 2 + :] [ "^exit" "\| 27" ] trace . cr
```
```output

> exit                    | 27

27
```

## How it works

Ops in Telic do not return to a loop between steps. `run_inner` dispatches the
first handler, and each handler ends by tail-calling the next one through
`DISPATCH_REGISTERS`, so a run of compiled code is a chain of jumps that comes
back to the loop only when something stops it. The macro has one exit:

```c
if (unlikely(interp->unwinding || interp->error_flag || interp->gc_pending))
	return;
```

When any of those is set, the op that has just finished syncs `ip` and `dsp`
and returns instead of jumping on. The return unwinds through the tail calls
to the C frame that started the chain, which is the loop, and the loop resumes
at `ip`. The collector was already using this: an allocator under pressure sets
`gc_pending`, the next dispatch returns, the loop collects, and dispatch
continues.

Tracing borrows that exit. `gc_pending` is a bit set with two bits,
`GC_PENDING` from the allocators and `TRACE_PENDING` from `trace`. While the
trace bit is up, every dispatch returns to the loop, and the loop, before it
dispatches the next op, calls `trace_step`, which prints the cell at `ip` and
the stack. That ordering is why each line shows the state before its op. The
loop clears only the collector's bit after collecting, and `trace` sets and
clears only its own, so a collection requested mid-trace still happens and does
not end the trace.

Two places start a body without going through the loop's first dispatch:
`execute_xt`, which runs a body on behalf of `execute`, `catch`, or a
`shift-with` handler, and the combinators' fast path, which jumps into a
quotation body per element. `execute_xt` prints the first op itself when the
bit is up. The combinators do not check anything per element; instead
`call_open`, which chooses how a combinator will run its xt once per call,
picks the ordinary `execute_cfa` route whenever a trace is active, and that
route runs each element under a nested `run_inner` whose loop prints every op,
the first included.

`trace` itself is small: it raises the bit, runs the quotation through
`execute_xt`, and lowers the bit unless an enclosing `trace` had raised it.

## Why it costs nothing when off

Nothing was added to the dispatch path. `DISPATCH_REGISTERS` tests the same
word it tested before `trace` existed; the only change is that the word may
now carry a second bit, which is invisible to the test. The loop's branch on
that word was already there and is taken only when the word is nonzero, which
without a trace means a collection is pending. The allocators write the bit
with `|=` rather than `=`, a read-modify-write on a path that runs only under
memory pressure.

The per-element path of a combinator is byte-identical to what it was: the
choice to leave the fast path is made in `call_open`, once per combinator
call, by a test of the same word. The one test that runs while tracing is off
sits in `execute_xt`, once per `execute` or `catch`, not per op or per element.

Measured on a 5 million element `map` with a one-op body, the interpreter
with `trace` built in runs in the same time as the build before it, to within
run-to-run noise.

## Reading a trace

A few things a trace shows that source does not:

- `(branch) n` right before a `(lit) '?` is an inline quotation: its body is
  compiled in place and the outer code jumps over it, then pushes its xt. The
  literal renders as `'?` because an anonymous quotation has no name; on the
  stack it shows as `<xt n>`, its dictionary address.
- `(tailcall) word` is a call in tail position. The caller's frame is replaced,
  so the caller's own `exit` never appears at the end.
- A word the library redefines by type, such as `map` over a dataset, shows its
  wrapper's ops (the type test, then `(lit) 'map execute` reaching the
  original) before the primitive runs.
- Under a combinator, each element's body appears in full, ending in its
  `exit`, with the combinator's source still visible at the bottom of the stack
  because the combinator peeks it rather than popping it.

`see-compiled` shows what a word will do; `trace` shows what it did, with the
values.
