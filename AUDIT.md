### `dgemm` / row-and-column reductions — pop-before-alloc UAF
- File: `matrix.c:453-478` (`p_dgemm_helper`), `:1409-1414`
  (`REDUCE_AXIS_HANDLER`: row/column sums/maxes/mins)
- Operands are popped (dsp decremented) before a GC-capable target allocation
  (`object_new_matrix_raw` / `matrix_sum_rows` et al.), then read.
  `object_alloc_slot` calls `gc()` synchronously on table exhaustion
  (`core.c:230`); at that moment the popped operands are above dsp and in no
  root. Peers `p_add`/`p_sort`/`p_argsort` leave operands on the stack.
- Severity: crash (needs full object table; not scriptable short-term).

### Integer overflow at scale
- Matrix broadcast result counts: `matrix.c:4-36` (`MATRIX_ELEMENTWISE_OP`),
  `:1646` (`p_augment`), `:1675` (`p_vstack`) — no `rows > INT_MAX/cols` guard;
  `object_new_matrix_sized` allocates via size_t product, then consumers
  compute `int n_elements = rows*columns` (`matrix.c:162,178,230,1097,1221,1439`;
  `statistics.c:60,124,168`, the last three UB before the size_t cast) and see
  wrapped-negative counts. Needs ~17 GB.
- `concat` length: `collections.c:673` — `a->len + b->len` int sum, no guard,
  unlike `reshape`/`0-matrix`. Needs 2^31 elements.
- `match-all` span index: `strings.c:217,226,244` — `count*num_groups*2`
  evaluated in int, wraps for pathological match counts. Needs multi-GB.

### `gc_root_push` unchecked on 64-root exhaustion
- `gc_root_push` (`water.h:1598`) fails without incrementing `n_gc_roots`; a
  following unconditional `gc_root_pop` then decrements an outer caller's root.
  Sites lacking the `if (error_flag) return;` guard: `words.c:1347`
  (`p_execute_catching`, OOM path also leaks 2 roots), `logic.c:228-234`
  (`p_amb`), `collections.c:289` (`p_array_to_cons`), `:173` (`p_frameclose`),
  `:952` (`p_array_to_frame`). Needs 64 live roots at entry.

### Minor
- `string_codepoint_count` capacity race under `pmap`: `strings.c:14-20` —
  memoizes by writing `string->capacity`; concurrent workers write the same
  value (benign torn write).

