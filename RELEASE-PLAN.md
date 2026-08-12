# Water — 1.0-alpha release plan

The gate for 1.0-alpha, in priority order; an entry vanishes as its item
completes. Optional, if the two items finish early: `upcase`/`downcase` in
the ASCII-first cut (PLAN.md, String operations).

---

## 1. MCP stdio server

### Semantics

1. `water -e '"mcp" load-library mcp-serve'` runs an MCP stdio server
   (protocol revision 2025-06-18): newline-delimited JSON-RPC on
   stdin/stdout, stdout carries protocol messages only, EOF on stdin
   ends the server.
2. Tools: `water-eval` — run Water source in the server's interpreter
   and return everything it printed; state persists across calls, so a
   word defined in one call is usable in the next. An error in
   evaluated code returns as `isError: true` carrying catch's
   `{ :message :trace }`, never a protocol error. `water-help` — a
   word's reference entry as text.
3. Handshake per spec: `initialize` answers the client's
   protocolVersion if supported (else 2025-06-18), capabilities
   `{tools: {}}`, serverInfo, and a short `instructions` string
   (code is Water; state persists across `water-eval` calls; printed
   output is the result). `notifications/initialized` and `ping`
   handled; unknown methods answer JSON-RPC −32601.
4. Remote access is a bridge, not Water code: the stdio server behind
   a stdio→Streamable-HTTP gateway (mcp-proxy, Supergateway), one
   child process per session. One README line states this.

### Implementation

1. Two C primitives, each useful beyond MCP: `read-line
   ( stream -- s|none )` — bounded read, `none` at EOF (the FastCGI
   entry's bounded-read need) — and a stdout-capture word
   `( xt -- s )` — stdout redirected to a temp file around
   `execute_xt` via dup/dup2 and restored, contents returned — which
   keeps evaluated prints off the protocol channel and doubles as a
   test utility.
2. lib/mcp.h2o owns the rest: `read-line` → `json>frame` → dispatch
   on `:method` → `frame>json` plus newline → stdout `write`; eval via
   `write-file` + `load` under `catch`.
3. Reference rows and example pairs for the two primitives
   (reference.md) and the lib words (reference-libraries.md); a golden
   test driving a scripted initialize / tools/list / tools/call
   session through the server.

### Acceptance

1. `claude mcp add water -- <path>/water -e '"mcp" load-library
   mcp-serve'`: the client lists both tools, and `water-eval`
   round-trips `3 4 + . cr` → `7`.
2. Discriminating case: the second of two `water-eval` calls uses a
   word the first defined — passes only with a persistent session; a
   spawn-per-call design fails it.
3. A `water-eval` whose code errors (an undefined word) answers
   `isError: true` with the message, and the protocol stream stays
   parseable afterward.
4. Native suite passes; the golden is wasm-skipped if the capture
   primitive stays posix-only.

---

## 2. Release mechanics

### Implementation

1. Add a `make install` (PREFIX-parameterized) copying the installed
   set: the `water` binary, `lib/`, and `liblapacke_water.so`, with
   `data/` only for running the README examples verbatim.
2. Add `make pack` output and `water-pack.md` to the released set.
3. Run `make acceptance` and read the failures: fix what is a pack gap,
   record what is not.
4. Set VERSION (src/c/water.h) to the release version and tag the
   commit.
5. Release notes: the benchmark table's provenance line, the platform
   pair (native, wasm) the suites passed on, and the acceptance pass@1
   with its model and date — a description of that run, not a
   threshold, since the tasks and the pack text were developed against
   each other.

### Acceptance

1. From a clean checkout: `make && make test && make test-wasm &&
   make bench && make pack` all succeed.
2. Copy the installed set to a directory outside the repo; `water`
   starts from any cwd, `"statistics" load-library` and
   `"plot" load-library` load, and `help` answers.
3. The tagged commit's README benchmark table matches a full run on the
   release host.
