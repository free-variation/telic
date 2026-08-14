# Water — 1.0-alpha release plan

The gate for 1.0-alpha, in priority order; an entry vanishes as its item
completes. Optional, if the two items finish early: `upcase`/`downcase` in
the ASCII-first cut (PLAN.md, String operations).

---

## 1. MCP stdio server against a real client

### Acceptance

1. `claude mcp add water -- <path>/water -e '"mcp" load-library
   mcp-serve'`: the client lists `water-eval` and `water-help`, and
   `water-eval` round-trips `3 4 + . cr` → `7`.
2. Two calls naming one session, the second using a word the first
   defined, answer from that session's state; a third naming another
   session reports the word unknown.
3. The client is a modern-era one. A client that opens with `initialize`
   receives −32601 naming revision 2026-07-28 and stops there, so record
   which client and version the run used.
4. Whatever the run exposes goes back into `lib/mcp.h2o` and
   `tests/lib/154_mcp_server.h2o`, since no test here can stand in for a
   real client's request shapes.

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
   make bench && make pack` all succeed, and `make test-libs` on a host
   with LAPACK and libxgboost — it holds the MCP server's test, which
   covers a 1.0 feature the core suite never runs.
2. Copy the installed set to a directory outside the repo; `water`
   starts from any cwd, `"statistics" load-library` and
   `"plot" load-library` load, and `help` answers.
3. The tagged commit's README benchmark table matches a full run on the
   release host.
