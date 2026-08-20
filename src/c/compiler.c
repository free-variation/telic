
#include "telic.h"

static void enter_compile_scope(Interpreter *interp);
static void leave_compile_scope(Interpreter *interp);
static void compile_locals_decl(Interpreter *interp);
static int barless_locals_follow(void);
static void hoist_assigned_locals(Interpreter *interp);
static int explicit_head_follows(void);
static int global_declared(const char *token);
static int declare_local_in_scope(Interpreter *interp, const char *token);
static void rewrite_tail_calls(int body_start, int body_end);

void rollback_partial_definition(void) {
	if (compiler.compiling_src_start <= 0 || vocab.latest_cfa == 0)
		return;
	int partial_cfa = vocab.latest_cfa;
	vocab.here = partial_cfa - 4;
	vocab.names_here = (int)WORD_NAME(partial_cfa);
	vocab.latest_cfa = (int)WORD_LINK(partial_cfa);
	truncate_quotation_spans();
	compiler.compiling = 0;
	compiler.compiling_src_start = 0;
	compiler.n_local_scopes = 0;
	compiler.n_local_names = 0;
	compiler.local_names_pool_here = 0;
	compiler.loop_begin = 0;
	compiler.leave_chain = 0;
	compiler.do_continue_chain = 0;
	compiler.case_chain = 0;
	compiler.n_active_do_loops = 0;
	compiler.conditional_depth = 0;
	compiler.n_declared_globals = 0;
	compiler.declared_globals_pool_here = 0;
}

static int check_locals_assigned(Interpreter *interp) {
	int scope_idx = compiler.n_local_scopes - 1;
	if (scope_idx < 0)
		return 1;

	int scope_start = compiler.local_scope_starts[scope_idx];
	for (int name_idx = scope_start; name_idx < compiler.n_local_names; name_idx++) {
		if (!compiler.local_fetched[name_idx] || compiler.local_stored[name_idx])
			continue;

		const char *name = &compiler.local_names_pool[compiler.local_name_offsets[name_idx]];
		int shadowed_cfa = find(name);
		rollback_partial_definition();
		if (shadowed_cfa)
			fail(interp, "local '%s' is read but never assigned (a word of that name exists)", name);
		else
			fail(interp, "local '%s' is read but never assigned", name);
		return 0;
	}
	return 1;
}

void p_semicolon(DISPATCH_ARGS) {
	if (compiler.compiling_src_start > 0 && compiler.n_local_scopes > 1) {
		rollback_partial_definition();
		fail(interp, "; : unterminated quotation (a [: has no matching :])");
		return;
	}
	if (compiler.loop_begin != 0) {
		rollback_partial_definition();
		fail(interp, "; : unterminated loop (a begin has no until/again/repeat, or a do no loop)");
		return;
	}
	if (compiler.case_chain != 0) {
		rollback_partial_definition();
		fail(interp, "; : unterminated case (a case has no endcase)");
		return;
	}
	if (compiler.conditional_depth > 0) {
		rollback_partial_definition();
		fail(interp, "; : unterminated conditional (an if has no matching then)");
		return;
	}
	if (!check_locals_assigned(interp))
		return;
	leave_compile_scope(interp);
	emit_call(interp, vocab.exit_cfa);
	rewrite_tail_calls(vocab.latest_cfa + 1, vocab.here);
	if (compiler.compiling_src_start > 0 && vocab.latest_cfa != 0) {
		int src_end = compiler.input_buffer_pos - 1;
		int src_len = src_end - compiler.compiling_src_start;
		src_len = MAX(src_len, 0);
		if (vocab.source_here + src_len + 1 > SOURCE_POOL) {
			fail(interp, "source pool full (max %d bytes); definition source too large to store", SOURCE_POOL);
		} else {
			int source_offset = vocab.source_here;
			memcpy(&vocab.source_pool[vocab.source_here],
					&compiler.input_buffer[compiler.compiling_src_start],
					(size_t)src_len);
			vocab.source_pool[vocab.source_here + src_len] = 0;
			vocab.source_here += src_len + 1;
			WORD_SOURCE(vocab.latest_cfa) = source_offset;
		}
	}
	compiler.compiling = 0;
	compiler.compiling_src_start = 0;

	if (vocab.latest_cfa != 0)
		echo_definition(&vocab.name_pool[WORD_NAME(vocab.latest_cfa)], compiler.definition_redefined, "word");

	DISPATCH(interp);
}

static int try_fuse_cmp_branch(Interpreter *interp) {
	int fused_cfa;

	if (compiler.fuse_prev_cmp == vocab.eq_cfa)
		fused_cfa = vocab.eq_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.lt_cfa)
		fused_cfa = vocab.lt_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.gt_cfa)
		fused_cfa = vocab.gt_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.zeq_cfa)
		fused_cfa = vocab.zeq_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.eq_f_cfa)
		fused_cfa = vocab.eq_f_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.lt_f_cfa)
		fused_cfa = vocab.lt_f_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.gt_f_cfa)
		fused_cfa = vocab.gt_f_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.lte_cfa)
		fused_cfa = vocab.lte_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.gte_cfa)
		fused_cfa = vocab.gte_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.lte_f_cfa)
		fused_cfa = vocab.lte_f_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.gte_f_cfa)
		fused_cfa = vocab.gte_f_zbranch_cfa;
	else
		return 0;

	vocab.here--;
	emit_call(interp, fused_cfa);
	return 1;
}


void p_if(DISPATCH_ARGS) {
	if (!compiler.compiling) {
		fail(interp, "if: only valid inside a colon definition or quotation");
		return;
	}

	if (!try_fuse_cmp_branch(interp))
		emit_call(interp, vocab.zbranch_cfa);
	push(interp, make_float((double)vocab.here));
	emit(interp, 0);
	compiler.conditional_depth++;

	DISPATCH(interp);
}

void p_qif(DISPATCH_ARGS) {
	if (!compiler.compiling) {
		fail(interp, "?if: only valid inside a colon definition or quotation");
		return;
	}

	emit_call(interp, vocab.qzbranch_cfa);
	push(interp, make_float((double)vocab.here));
	emit(interp, 0);
	compiler.conditional_depth++;

	DISPATCH(interp);
}

static int valid_patch_slot(Interpreter *interp, int slot, const char *op) {
	int scope_start = compiler.n_local_scopes > 0
		? compiler.local_scope_dict_starts[compiler.n_local_scopes - 1]
		: vocab.here;
	if (compiler.n_local_scopes == 0 || slot < scope_start || slot >= vocab.here) {
		fail(interp, "%s: no matching control-flow opener", op);
		return 0;
	}
	return 1;
}

void p_then(DISPATCH_ARGS) {
	POP(slot_val);
	int slot = (int)VAL_NUMBER(slot_val);
	if (!valid_patch_slot(interp, slot, "then"))
		return;
	vocab.dict[slot] = (vocab.here - slot);
	compiler.fuse_floor = vocab.here;
	if (compiler.conditional_depth > 0)
		compiler.conditional_depth--;

	DISPATCH(interp);
}

void p_else(DISPATCH_ARGS) {
	POP(slot_val);
	int slot = (int)VAL_NUMBER(slot_val);
	if (!valid_patch_slot(interp, slot, "else"))
		return;
	emit_call(interp, vocab.branch_cfa);
	push(interp, make_float((double)vocab.here));
	emit(interp, 0);
	vocab.dict[slot] = (vocab.here - slot);

	DISPATCH(interp);
}

void p_begin(DISPATCH_ARGS) {
	if (!compiler.compiling) {
		fail(interp, "begin: only valid inside a colon definition or quotation");
		return;
	}

	push(interp, make_float((double)compiler.loop_begin));
	push(interp, make_float((double)compiler.leave_chain));
	push(interp, make_float((double)vocab.here));
	compiler.loop_begin = vocab.here;
	compiler.leave_chain = 0;
	compiler.fuse_floor = vocab.here;

	DISPATCH(interp);
}

static void close_loop(Interpreter *interp) {
	for (int slot = compiler.leave_chain; slot != 0; ) {
		int prior_slot = (int)vocab.dict[slot];
		vocab.dict[slot] = vocab.here - slot;
		slot = prior_slot;
	}
	POP(leave_chain_val);
	POP(loop_begin_val);
	compiler.leave_chain = (int)VAL_NUMBER(leave_chain_val);
	compiler.loop_begin = (int)VAL_NUMBER(loop_begin_val);
}

void p_leave(DISPATCH_ARGS) {
	if (compiler.loop_begin == 0) {
		fail(interp, "leave: not inside a loop");
		return;
	}
	emit_call(interp, vocab.branch_cfa);
	emit(interp, (cell)compiler.leave_chain);
	compiler.leave_chain = vocab.here - 1;

	DISPATCH(interp);
}

void p_continue(DISPATCH_ARGS) {
	if (compiler.loop_begin == 0) {
		fail(interp, "continue: not inside a loop");
		return;
	}

	if (compiler.loop_begin < 0) {
		emit_call(interp, vocab.branch_cfa);
		emit(interp, (cell)compiler.do_continue_chain);
		compiler.do_continue_chain = vocab.here - 1;

		DISPATCH(interp);
	}

	emit_call(interp, vocab.branch_cfa);
	emit(interp, compiler.loop_begin - vocab.here);

	DISPATCH(interp);
}

void p_until(DISPATCH_ARGS) {
	POP(back_val);
	int back = (int)VAL_NUMBER(back_val);
	if (!valid_patch_slot(interp, back, "until"))
		return;
	if (!try_fuse_cmp_branch(interp))
		emit_call(interp, vocab.zbranch_cfa);
	emit(interp, back - vocab.here);
	close_loop(interp);

	DISPATCH(interp);
}

void p_again(DISPATCH_ARGS) {
	POP(back_val);
	int back = (int)VAL_NUMBER(back_val);
	if (!valid_patch_slot(interp, back, "again"))
		return;
	emit_call(interp, vocab.branch_cfa);
	emit(interp, back - vocab.here);
	close_loop(interp);

	DISPATCH(interp);
}

void p_while(DISPATCH_ARGS) {
	if (!try_fuse_cmp_branch(interp))
		emit_call(interp, vocab.zbranch_cfa);
	push(interp, make_float((double)vocab.here));
	emit(interp, 0);

	DISPATCH(interp);
}

void p_repeat(DISPATCH_ARGS) {
	POP(exit_slot_val);
	POP(back_val);
	int exit_slot = (int)VAL_NUMBER(exit_slot_val);
	int back = (int)VAL_NUMBER(back_val);
	if (!valid_patch_slot(interp, back, "repeat") || !valid_patch_slot(interp, exit_slot, "repeat"))
		return;
	emit_call(interp, vocab.branch_cfa);
	emit(interp, back - vocab.here);
	vocab.dict[exit_slot] = (vocab.here - exit_slot);
	close_loop(interp);

	DISPATCH(interp);
}

void p_do(DISPATCH_ARGS) {
	if (!compiler.compiling) {
		fail(interp, "do: only valid inside a colon definition or quotation");
		return;
	}

	char *token = next_token();
	if (!token) {
		fail(interp, "do: expected an index name");
		return;
	}

	int scope_idx = compiler.n_local_scopes - 1;
	int index_slot;
	int local_depth;
	int local_slot_idx;
	(void)local_depth;
	if (find_local(token, &local_depth, &local_slot_idx)) {
		if (reject_outer_local(interp, token))
			return;
		for (int i = 0; i < compiler.n_active_do_loops; i++) {
			if (compiler.do_index_scopes[i] == scope_idx
					&& compiler.do_index_slots[i] == local_slot_idx) {
				fail(interp, "do: %s is already the index of an enclosing do", token);
				return;
			}
		}
		compiler.local_stored[compiler.found_local_name_idx] = 1;
		index_slot = local_slot_idx;
	} else {
		int existing_cfa = find(token);
		if (existing_cfa && (cfa_handler)vocab.dict[existing_cfa] == dovar) {
			fail(interp, "do: %s is a global; pick another index name", token);
			return;
		}

		if (compiler.local_scope_entry_cells[scope_idx] < 0) {
			if (compiler.conditional_depth > 0 || compiler.loop_begin != 0) {
				fail(interp, "do: %s is this body's first local and sits inside a branch; assign a local before the branch", token);
				return;
			}
			compiler.local_scope_entry_cells[scope_idx] = vocab.here;
			emit_call(interp, vocab.enter_locals_cfa);
			emit(interp, 0);
		}
		index_slot = declare_local_in_scope(interp, token);
		if (index_slot < 0)
			return;
	}

	if (compiler.n_active_do_loops >= MAX_LOCAL_SCOPES) {
		fail(interp, "do: loops nested deeper than %d", MAX_LOCAL_SCOPES);
		return;
	}

	int counter_slot = declare_local_in_scope(interp, "do counter");
	if (counter_slot < 0)
		return;
	int delta_slot = declare_local_in_scope(interp, "do delta");
	if (delta_slot < 0)
		return;

	compiler.do_index_scopes[compiler.n_active_do_loops] = scope_idx;
	compiler.do_index_slots[compiler.n_active_do_loops] = index_slot;
	compiler.n_active_do_loops++;

	emit_call(interp, vocab.do_enter_cfa);
	emit(interp, (cell)index_slot);
	emit(interp, (cell)counter_slot);
	emit(interp, (cell)delta_slot);
	int forward_slot = vocab.here;
	emit(interp, 0);

	push(interp, make_float((double)compiler.loop_begin));
	push(interp, make_float((double)compiler.leave_chain));
	push(interp, make_float((double)compiler.do_continue_chain));
	push(interp, make_float((double)-forward_slot));
	compiler.loop_begin = -vocab.here;
	compiler.leave_chain = 0;
	compiler.do_continue_chain = 0;
	compiler.fuse_floor = vocab.here;

	DISPATCH(interp);
}

void p_loop(DISPATCH_ARGS) {
	POP(marker_val);
	int marker = (int)VAL_NUMBER(marker_val);
	if (marker >= 0) {
		fail(interp, "loop: no matching do");
		return;
	}

	int forward_slot = -marker;
	if (!valid_patch_slot(interp, forward_slot, "loop"))
		return;

	cell index_slot = vocab.dict[forward_slot - 3];
	cell counter_slot = vocab.dict[forward_slot - 2];
	cell delta_slot = vocab.dict[forward_slot - 1];

	int loop_op = vocab.here;
	for (int slot = compiler.do_continue_chain; slot != 0; ) {
		int prior_slot = (int)vocab.dict[slot];
		vocab.dict[slot] = loop_op - slot;
		slot = prior_slot;
	}

	emit_call(interp, vocab.do_loop_cfa);
	emit(interp, index_slot);
	emit(interp, counter_slot);
	emit(interp, delta_slot);
	emit(interp, (cell)(forward_slot + 1 - vocab.here));

	vocab.dict[forward_slot] = vocab.here - forward_slot;

	POP(continue_chain_val);
	compiler.do_continue_chain = (int)VAL_NUMBER(continue_chain_val);
	close_loop(interp);
	if (compiler.n_active_do_loops > 0)
		compiler.n_active_do_loops--;
	compiler.fuse_floor = vocab.here;

	DISPATCH(interp);
}

static void open_quotation(Interpreter *interp) {
	int opener_start = compiler.input_buffer_pos - 2;
	int branch_slot = -1;
	if (compiler.compiling) {
		emit_call(interp, vocab.branch_cfa);
		branch_slot = vocab.here;
		emit(interp, 0);
	}
	int anon_cfa = vocab.here;
	emit(interp, (cell)&docol);
	compiler.fuse_floor = vocab.here;
	compiler.loadn_at = -1;
	enter_compile_scope(interp);
	compiler.compiling = 1;
	push(interp, make_float((double)anon_cfa));
	push(interp, make_float((double)branch_slot));
	push(interp, make_float((double)opener_start));
	push(interp, make_float((double)compiler.loop_begin));
	push(interp, make_float((double)compiler.leave_chain));
	push(interp, make_float((double)compiler.case_chain));
	compiler.loop_begin = 0;
	compiler.leave_chain = 0;
	compiler.case_chain = 0;
}

void p_case(DISPATCH_ARGS) {
	if (!compiler.compiling) {
		fail(interp, "case: only valid inside a colon definition or quotation");
		return;
	}

	push(interp, make_float((double)compiler.case_chain));
	compiler.case_chain = -1;

	DISPATCH(interp);
}

void p_of(DISPATCH_ARGS) {
	if (compiler.case_chain == 0) {
		fail(interp, "of: no enclosing case");
		return;
	}

	emit_call(interp, find("over"));
	emit_call(interp, find("unify?"));
	emit_call(interp, vocab.zbranch_cfa);
	push(interp, make_float((double)vocab.here));
	emit(interp, 0);
	emit_call(interp, find("drop"));
	compiler.conditional_depth++;
	compiler.fuse_floor = vocab.here;

	DISPATCH(interp);
}

void p_endof(DISPATCH_ARGS) {
	if (compiler.case_chain == 0) {
		fail(interp, "endof: no enclosing case");
		return;
	}

	POP(slot_val);
	int slot = (int)VAL_NUMBER(slot_val);
	if (!valid_patch_slot(interp, slot, "endof"))
		return;

	emit_call(interp, vocab.branch_cfa);
	emit(interp, compiler.case_chain > 0 ? (cell)compiler.case_chain : 0);
	compiler.case_chain = vocab.here - 1;

	vocab.dict[slot] = (vocab.here - slot);
	compiler.fuse_floor = vocab.here;
	if (compiler.conditional_depth > 0)
		compiler.conditional_depth--;

	DISPATCH(interp);
}

void p_endcase(DISPATCH_ARGS) {
	if (compiler.case_chain == 0) {
		fail(interp, "endcase: no enclosing case");
		return;
	}

	for (int slot = compiler.case_chain; slot > 0; ) {
		int prior_slot = (int)vocab.dict[slot];
		vocab.dict[slot] = vocab.here - slot;
		slot = prior_slot;
	}

	POP(saved_case_chain_val);
	compiler.case_chain = (int)VAL_NUMBER(saved_case_chain_val);
	compiler.fuse_floor = vocab.here;

	DISPATCH(interp);
}

void p_qcolon(DISPATCH_ARGS) {
	open_quotation(interp);
	if (barless_locals_follow()) {
		compile_locals_decl(interp);
		if (interp->error_flag)
			DISPATCH(interp);
	}
	if (!explicit_head_follows())
		hoist_assigned_locals(interp);

	DISPATCH(interp);
}

static void record_quotation_span(Interpreter *interp, int anon_cfa, int opener_start) {
	if (vocab.n_quotation_spans >= MAX_QUOTATION_SPANS) {
		fail(interp, "quotation span table full (max %d)", MAX_QUOTATION_SPANS);
		return;
	}
	int snippet_len = compiler.input_buffer_pos - opener_start;
	int source_offset = 0;
	if (snippet_len > 0 && vocab.source_here + snippet_len + 1 <= SOURCE_POOL) {
		source_offset = vocab.source_here;
		memcpy(&vocab.source_pool[vocab.source_here],
				&compiler.input_buffer[opener_start], (size_t)snippet_len);
		vocab.source_pool[vocab.source_here + snippet_len] = 0;
		vocab.source_here += snippet_len + 1;
	}
	QuotationSpan *span = &vocab.quotation_spans[vocab.n_quotation_spans++];
	span->start_cfa = anon_cfa;
	span->end_cfa = vocab.here;
	span->source_offset = source_offset;
}

void truncate_quotation_spans(void) {
	while (vocab.n_quotation_spans > 0
			&& vocab.quotation_spans[vocab.n_quotation_spans - 1].end_cfa > vocab.here)
		vocab.n_quotation_spans--;
	for (int i = 0; i < vocab.n_quotation_spans; i++)
		if (vocab.quotation_spans[i].source_offset >= vocab.source_here)
			vocab.quotation_spans[i].source_offset = 0;
}

void p_qsemi(DISPATCH_ARGS) {
	if (compiler.loop_begin != 0) {
		fail(interp, ":] : unterminated loop (a begin has no until/again/repeat, or a do no loop)");
		return;
	}
	if (compiler.case_chain != 0) {
		fail(interp, ":] : unterminated case (a case has no endcase)");
		return;
	}
	if (compiler.conditional_depth > 0) {
		fail(interp, ":] : unterminated conditional (an if has no matching then)");
		return;
	}
	if (!check_locals_assigned(interp))
		return;
	leave_compile_scope(interp);
	emit_call(interp, vocab.exit_cfa);
	POP(case_chain_val);
	POP(leave_chain_val);
	POP(loop_begin_val);
	POP(opener_start_val);
	POP(branch_slot_val);
	POP(anon_cfa_val);
	compiler.case_chain = (int)VAL_NUMBER(case_chain_val);
	compiler.leave_chain = (int)VAL_NUMBER(leave_chain_val);
	compiler.loop_begin = (int)VAL_NUMBER(loop_begin_val);
	int opener_start = (int)VAL_NUMBER(opener_start_val);
	int branch_slot = (int)VAL_NUMBER(branch_slot_val);
	int anon_cfa = (int)VAL_NUMBER(anon_cfa_val);
	record_quotation_span(interp, anon_cfa, opener_start);
	if (interp->error_flag)
		return;
	rewrite_tail_calls(anon_cfa + 1, vocab.here);
	if (branch_slot < 0) {
		compiler.compiling = 0;
		push(interp, make_xt(anon_cfa));
	} else {
		vocab.dict[branch_slot] = (vocab.here - branch_slot);
		emit_val_literal(interp, make_xt(anon_cfa));
	}

	DISPATCH(interp);
}

void p_recurse(DISPATCH_ARGS) {
	if (!compiler.compiling || compiler.n_local_scopes <= 0) {
		fail(interp, "recurse: only inside a definition or quotation");
		return;
	}

	int definition_cfa = compiler.local_scope_dict_starts[compiler.n_local_scopes - 1] - 1;
	emit_call(interp, definition_cfa);

	DISPATCH(interp);
}

static int parse_word_cfa(Interpreter *interp, const char *op) {
	char *token = next_token();

	if (!token) {
		fail(interp, "%s : expected a word name", op);
		return 0;
	}
	int target_cfa = find(token);
	if (!target_cfa) {
		fail(interp, "%s : unknown word: %s", op, token);
		return 0;
	}

	return target_cfa;
}


void p_tick(DISPATCH_ARGS) {
	int target_cfa = parse_word_cfa(interp, "'");
	if (!target_cfa)
		return;

	Val value = make_xt(target_cfa);
	if (compiler.compiling)
		emit_val_literal(interp, value);
	else
		push(interp, value);

	DISPATCH(interp);
}

void p_lookup(DISPATCH_ARGS) {
	int target_cfa = parse_word_cfa(interp, "lookup");
	if (!target_cfa)
		return;

	push(interp, make_xt(target_cfa));

	DISPATCH(interp);
}
static void enter_compile_scope(Interpreter *interp) {
	if (compiler.n_local_scopes >= MAX_LOCAL_SCOPES) {
		fail(interp, "compile: locals nesting deeper than %d", MAX_LOCAL_SCOPES);
		return;
	}

	compiler.local_scope_starts[compiler.n_local_scopes] = compiler.n_local_names;
	compiler.local_scope_dict_starts[compiler.n_local_scopes] = vocab.here;
	compiler.local_scope_entry_cells[compiler.n_local_scopes] = -1;
	compiler.local_scope_global_starts[compiler.n_local_scopes] = compiler.n_declared_globals;
	compiler.local_scope_saved_conditionals[compiler.n_local_scopes] = compiler.conditional_depth;
	compiler.conditional_depth = 0;
	compiler.n_local_scopes++;
}

static void leave_compile_scope(Interpreter *interp) {
	if (compiler.n_local_scopes <= 0)
		return;

	compiler.n_local_scopes--;
	compiler.conditional_depth = compiler.local_scope_saved_conditionals[compiler.n_local_scopes];
	compiler.n_declared_globals = compiler.local_scope_global_starts[compiler.n_local_scopes];
	compiler.declared_globals_pool_here = compiler.n_declared_globals > 0
		? compiler.declared_global_offsets[compiler.n_declared_globals - 1]
			+ (int)strlen(&compiler.declared_globals_pool[compiler.declared_global_offsets[compiler.n_declared_globals - 1]]) + 1
		: 0;
	int saved_n_names = compiler.local_scope_starts[compiler.n_local_scopes];
	int n_locals_in_scope = compiler.n_local_names - saved_n_names;

	int entry_cell = compiler.local_scope_entry_cells[compiler.n_local_scopes];
	if (entry_cell >= 0) {
		vocab.dict[entry_cell + 1] = (cell)n_locals_in_scope;
		emit_call(interp, vocab.leave_locals_cfa);
		emit(interp, (cell)n_locals_in_scope);
	}

	if (saved_n_names == 0) {
		compiler.local_names_pool_here = 0;
	} else {
		int last_offset = compiler.local_name_offsets[saved_n_names - 1];
		compiler.local_names_pool_here = last_offset +
			(int)strlen(&compiler.local_names_pool[last_offset]) + 1;
	}
	compiler.n_local_names = saved_n_names;
}

static int barless_locals_follow(void) {
	int saved_position = compiler.input_buffer_pos;
	int saved_line = compiler.input_line;
	int found = 0;
	int names = 0;

	while (1) {
		skip_whitespace_and_comments();
		char *token = next_token();
		if (!token) {
			if (refill_input())
				continue;
			break;
		}
		if (strcmp(token, "|") == 0) {
			found = names > 0;
			break;
		}
		if (strcmp(token, ";") == 0 || strcmp(token, ":]") == 0
				|| strcmp(token, "[:") == 0)
			break;

		double ignored;
		if (token[0] == '"' || parse_float(token, &ignored))
			break;

		names++;
	}

	compiler.input_buffer_pos = saved_position;
	compiler.input_line = saved_line;
	return found;
}

static int explicit_head_follows(void) {
	int saved_position = compiler.input_buffer_pos;
	int saved_line = compiler.input_line;

	skip_whitespace_and_comments();
	char *token = next_token();
	while (!token && refill_input()) {
		skip_whitespace_and_comments();
		token = next_token();
	}
	int explicit_head = token && strcmp(token, "|") == 0;

	compiler.input_buffer_pos = saved_position;
	compiler.input_line = saved_line;
	return explicit_head;
}

static const char *in_place_update_word(const char *token) {
	if (strcmp(token, "++") == 0)
		return "++";
	if (strcmp(token, "--") == 0)
		return "--";
	if (strcmp(token, "f++") == 0)
		return "f++";
	if (strcmp(token, "f--") == 0)
		return "f--";
	return NULL;
}

static void hoist_assigned_locals(Interpreter *interp) {
	int saved_position = compiler.input_buffer_pos;
	int saved_line = compiler.input_line;
	int scope_idx = compiler.n_local_scopes - 1;
	int scope_start = compiler.local_scope_starts[scope_idx];
	int depth = 0;

	for (;;) {
		skip_whitespace_and_comments();
		if (compiler.input_buffer_pos >= compiler.input_buffer_len) {
			if (refill_input())
				continue;
			break;
		}
		if (compiler.input_buffer[compiler.input_buffer_pos] == '"') {
			if (read_string_literal() < 0) {
				if (refill_input()) {
					compiler.need_more = 0;
					continue;
				}
				break;
			}
			continue;
		}

		char *token = next_token();
		if (!token) {
			if (refill_input())
				continue;
			break;
		}

		if (strcmp(token, "[:") == 0) {
			depth++;
			continue;
		}
		if (strcmp(token, ":]") == 0) {
			if (depth == 0)
				break;
			depth--;
			continue;
		}
		if (strcmp(token, ";") == 0)
			break;
		if (depth != 0)
			continue;

		int declares_index = strcmp(token, "do") == 0;
		int declares_local = declares_index || strcmp(token, "to") == 0;
		const char *assigning_word = declares_index ? "do"
			: declares_local ? "to" : in_place_update_word(token);
		if (!assigning_word)
			continue;

		skip_whitespace_and_comments();
		char *name = next_token();
		if (!name)
			break;
		if (global_declared(name))
			continue;

		int existing_cfa = find(name);
		if (existing_cfa && (cfa_handler)vocab.dict[existing_cfa] == dovar) {
			compiler.input_buffer_pos = saved_position;
			compiler.input_line = saved_line;
			if (declares_index)
				fail(interp, "do: %s is a global; pick another index name", name);
			else
				fail(interp, "%s: %s is a global; declare it in the locals list as ^%s to assign it here, or rename the local", assigning_word, name, name);
			return;
		}

		if (!declares_local)
			continue;

		int already = 0;
		for (int i = scope_start; i < compiler.n_local_names; i++)
			if (strcmp(name, &compiler.local_names_pool[compiler.local_name_offsets[i]]) == 0)
				already = 1;
		if (already)
			continue;

		if (declare_local_in_scope(interp, name) < 0)
			break;
	}

	compiler.input_buffer_pos = saved_position;
	compiler.input_line = saved_line;

	if (compiler.n_local_names > scope_start && compiler.local_scope_entry_cells[scope_idx] < 0) {
		compiler.local_scope_entry_cells[scope_idx] = vocab.here;
		emit_call(interp, vocab.enter_locals_cfa);
		emit(interp, 0);
	}
}

void p_colon(DISPATCH_ARGS) {
	char *token = next_token();
	if (!token) {
		fail(interp, ": expected a name for the new definition");
		return;
	}

	compiler.definition_redefined = find(token) != 0;

	create_header(interp, token, 0);
	emit(interp, (cell)&docol);
	compiler.fuse_floor = vocab.here;
	compiler.loadn_at = -1;
	enter_compile_scope(interp);
	compiler.compiling = 1;
	compiler.loop_begin = 0;
	compiler.leave_chain = 0;

	compiler.compiling_src_start = compiler.input_buffer_pos;

	if (barless_locals_follow()) {
		compile_locals_decl(interp);
		if (interp->error_flag)
			DISPATCH(interp);
	}
	if (!explicit_head_follows())
		hoist_assigned_locals(interp);

	DISPATCH(interp);
}

int create_variable(Interpreter *interp, const char *name) {
	create_header(interp, name, 0);
	emit(interp, (cell)&dovar);
	emit(interp, (cell)make_float(0.0).bits);

	return vocab.latest_cfa;
}


void p_variable(DISPATCH_ARGS) {
	char *token = next_token();
	if (!token) {
		fail(interp, "variable: expected a name");
		return;
	}

	int redefined = find(token) != 0;
	create_variable(interp, token);
	echo_definition(token, redefined, "word");

	DISPATCH(interp);
}

void p_constant(DISPATCH_ARGS) {
	POP(value);
	char *token = next_token();
	if (!token) {
		fail(interp, "constant: expected a name");
		return;
	}
	int redefined = find(token) != 0;
	create_header(interp, token, 2);
	emit(interp, (cell)&docol);
	emit_val_literal(interp, value);
	emit_call(interp, vocab.exit_cfa);
	echo_definition(token, redefined, "word");
	DISPATCH(interp);
}

void p_defer(DISPATCH_ARGS) {
	char *token = next_token();
	if (!token) {
		fail(interp, "defer: expected a name");
		return;
	}

	int redefined = find(token) != 0;
	create_header(interp, token, 0);
	emit(interp, (cell)&dodefer);
	emit(interp, (cell)0);
	emit(interp, (cell)0);
	emit(interp, (cell)0);
	echo_definition(token, redefined, "deferred word");

	DISPATCH(interp);
}

static int deferred_word(Interpreter *interp, const char *op) {
	if (compiler.compiling) {
		fail(interp, "%s: only at top level", op);
		return 0;
	}

	char *token = next_token();
	if (!token) {
		fail(interp, "%s: expected a name", op);
		return 0;
	}

	int deferred_cfa = find(token);
	if (!deferred_cfa) {
		fail(interp, "%s: unknown word: %s", op, token);
		return 0;
	}
	if ((cfa_handler)vocab.dict[deferred_cfa] != dodefer) {
		fail(interp, "%s: not a deferred word: %s", op, token);
		return 0;
	}

	return deferred_cfa;
}

void p_embodies(DISPATCH_ARGS) {
	int deferred_cfa = deferred_word(interp, "embodies");
	if (!deferred_cfa)
		return;

	POP_CALLABLE(target, "embodies");
	if (VAL_TAG(target_val) == T_CURRIED) {
		target = curried_materialize(interp, target_val);
		if (interp->error_flag)
			return;
	}
	if ((cfa_handler)vocab.dict[target] != docol) {
		fail(interp, "embodies: target must be a colon word or quotation");
		return;
	}

	vocab.dict[deferred_cfa + 1] = (cell)target;

	DISPATCH(interp);
}

static void rewrite_deferred_calls(int deferred_cfa, int target_cfa) {
	cell dodefer_handler = (cell)&dodefer;
	cell docol_handler = (cell)&docol;
	cell exit_handler = vocab.dict[vocab.exit_cfa];

	for (int cfa = vocab.latest_cfa; cfa != 0; cfa = (int)WORD_LINK(cfa)) {
		if (vocab.dict[cfa] != docol_handler)
			continue;

		int cursor = cfa + 1;
		int depth = 0;
		while (1) {
			cell handler = vocab.dict[cursor];

			if (handler == exit_handler) {
				cursor++;
				if (depth == 0)
					break;
				depth--;
				continue;
			}

			if (handler == docol_handler && quotation_starts_at(cursor)) {
				cursor++;
				depth++;
				continue;
			}

			if (handler == dodefer_handler) {
				if ((int)vocab.dict[cursor + 1] == deferred_cfa) {
					vocab.dict[cursor] = docol_handler;
					vocab.dict[cursor + 1] = (cell)target_cfa;
				} 
				cursor += 2;
				continue;
			}

			if (handler == docol_handler || handler == (cell)&dovar
					|| handler == (cell)&dounit || handler == (cell)&dosym) {
				cursor += 2;
				continue;
			}

			cursor += op_cell_count(cursor);
		}
	}
}


void p_embodies_final(DISPATCH_ARGS) {
	int deferred_cfa = deferred_word(interp, "embodies!");
	if (!deferred_cfa)
		return;

	POP_CALLABLE(target, "embodies!");
	if (VAL_TAG(target_val) == T_CURRIED) {
		target = curried_materialize(interp, target_val);
		if (interp->error_flag)
			return;
	}
	if ((cfa_handler)vocab.dict[target] != docol) {
		fail(interp, "embodies!: target must be a colon word or quotation");
		return;
	}

	rewrite_deferred_calls(deferred_cfa, target);

	vocab.dict[deferred_cfa] = (cell)&docol;
	vocab.dict[deferred_cfa + 1] = (cell)&docol;
	vocab.dict[deferred_cfa + 2] = (cell)target;
	vocab.dict[deferred_cfa + 3] = vocab.dict[vocab.exit_cfa];
	dict_is_handler[deferred_cfa + 1] = 1;
	dict_is_handler[deferred_cfa + 2] = 0;
	dict_is_handler[deferred_cfa + 3] = 1;

	DISPATCH(interp);
}

static int reaches_exit(int cursor, int body_end) {
	cell exit_handler = vocab.dict[vocab.exit_cfa];
	cell branch_handler = vocab.dict[vocab.branch_cfa];
	cell leave_handler = vocab.dict[vocab.leave_locals_cfa];

	while (cursor < body_end) {
		cell handler = vocab.dict[cursor];

		if (handler == exit_handler)
			return 1;

		if (handler == branch_handler) {
			int target = cursor + 1 + (int)vocab.dict[cursor + 1];
			if (target <= cursor)
				return 0;
			cursor = target;
			continue;
		}
		
		if (handler == leave_handler) {
			cursor += 2;
			continue;
		}
		return 0;
	}
	return 0;
}

static int definition_has_locals(int body_start) {
	cell handler = vocab.dict[body_start];
	return handler == vocab.dict[vocab.enter_locals_cfa]
		|| handler == vocab.dict[vocab.enter_locals_to_cfa]
		|| handler == vocab.dict[vocab.enter_locals_mixed_cfa];
}

static int body_has_tail_hazard(int body_start, int body_end) {
	cell docol_handler = (cell)&docol;
	int has_locals = definition_has_locals(body_start);

	int cursor = body_start;
	while (cursor < body_end) {
		cell handler = vocab.dict[cursor];

		if (handler == docol_handler && quotation_starts_at(cursor)) {
			if (has_locals)
				return 1;
			cursor = quotation_extent_end(cursor);
			continue;
		}
		if (handler == (cell)&p_tor || handler == (cell)&p_rfrom
				|| handler == (cell)&p_rfetch || handler == (cell)&p_reset
				|| handler == (cell)&p_shift || handler == (cell)&p_shift_with
				|| handler == (cell)&p_fail)
			return 1;
		if (handler == docol_handler || handler == (cell)&dovar
				|| handler == (cell)&dounit || handler == (cell)&dosym
				|| handler == (cell)&dodefer)
			cursor += 2;
		else
			cursor += op_cell_count(cursor);
	}
	return 0;
}

static void rewrite_tail_calls(int body_start, int body_end) {
	cell docol_handler = (cell)&docol;
	cell tailcall_handler = vocab.dict[vocab.tailcall_cfa];

	if (body_has_tail_hazard(body_start, body_end))
		return;

	int cursor = body_start;
	while (cursor < body_end) {
		cell handler = vocab.dict[cursor];

		if (handler == docol_handler && quotation_starts_at(cursor)) {
			cursor = quotation_extent_end(cursor);
			continue;
		}

		if (handler == docol_handler) {
			if (reaches_exit(cursor + 2, body_end))
				vocab.dict[cursor] = tailcall_handler;
			cursor += 2;
			continue;
		}

		if (handler == (cell)&dovar || handler == (cell)&dounit
				|| handler == (cell)&dosym || handler == (cell)&dodefer) {
			cursor += 2;
			continue;
		}

		cursor += op_cell_count(cursor);
	}
}
			

static int global_declared(const char *token) {
	for (int i = 0; i < compiler.n_declared_globals; i++)
		if (strcmp(token, &compiler.declared_globals_pool[compiler.declared_global_offsets[i]]) == 0)
			return 1;

	return 0;
}

static int declare_global_in_scope(Interpreter *interp, const char *token) {
	int target_cfa = find(token);
	if (!target_cfa || (cfa_handler)vocab.dict[target_cfa] != dovar) {
		fail(interp, "|: ^%s names no global variable", token);
		return 0;
	}

	int name_len = (int)strlen(token);
	if (compiler.declared_globals_pool_here + name_len + 1 > LOCAL_NAMES_POOL_SIZE
			|| compiler.n_declared_globals >= MAX_LOCAL_NAMES) {
		fail(interp, "|: too many declared globals");
		return 0;
	}

	int offset = compiler.declared_globals_pool_here;
	memcpy(&compiler.declared_globals_pool[offset], token, (size_t)name_len);
	compiler.declared_globals_pool[offset + name_len] = 0;
	compiler.declared_globals_pool_here += name_len + 1;
	compiler.declared_global_offsets[compiler.n_declared_globals++] = offset;

	return 1;
}

static void compile_locals_decl(Interpreter *interp) {
	if (!compiler.compiling || compiler.n_local_scopes <= 0) {
		fail(interp, "|: only valid inside a colon definition or quotation");
		return;
	}

	int scope_idx = compiler.n_local_scopes - 1;
	int scope_dict_start = compiler.local_scope_dict_starts[scope_idx];
	if (vocab.here != scope_dict_start) {
		if (compiler.n_local_names > compiler.local_scope_starts[scope_idx])
			fail(interp, "|: locals are declared in one list; `to` declares the rest");
		else
			fail(interp, "|: locals must be declared at the head of the body");
		return;
	}

	int scope_start = compiler.local_scope_starts[scope_idx];
	int receive_slots[MAX_LOCAL_NAMES];
	int n_received = 0;
	int lvar_slots[MAX_LOCAL_NAMES];
	int n_lvars = 0;
	int n_globals = 0;

	while (1) {
		skip_whitespace_and_comments();
		char *token = next_token();
		if (!token) {
			if (refill_input())
				continue;
			fail(interp, "|: unterminated locals declaration (no closing |)");
			return;
		}
		if (strcmp(token, "|") == 0)
			break;

		if (token[0] == '^' && token[1] != 0) {
			if (!declare_global_in_scope(interp, token + 1))
				return;
			n_globals++;
			continue;
		}

		int has_lvar_marker = 0;
		if (token[0] == '?' && token[1] != 0) {
			has_lvar_marker = 1;
			token++;
		}
		int has_receive_marker = !has_lvar_marker;

		for (int i = scope_start; i < compiler.n_local_names; i++) {
			if (strcmp(token, &compiler.local_names_pool[compiler.local_name_offsets[i]]) == 0) {
				fail(interp, "|: local '%s' declared twice", token);
				return;
			}
		}

		int name_len = (int)strlen(token);
		if (compiler.local_names_pool_here + name_len + 1 > LOCAL_NAMES_POOL_SIZE) {
			fail(interp, "|: local names pool full");
			return;
		}
		if (compiler.n_local_names >= MAX_LOCAL_NAMES) {
			fail(interp, "|: too many local names (max %d)", MAX_LOCAL_NAMES);
			return;
		}

		int slot = compiler.n_local_names - scope_start;

		int offset = compiler.local_names_pool_here;
		memcpy(&compiler.local_names_pool[offset], token, (size_t)name_len);
		compiler.local_names_pool[offset + name_len] = 0;
		compiler.local_names_pool_here += name_len + 1;
		compiler.local_fetched[compiler.n_local_names] = 0;
		compiler.local_stored[compiler.n_local_names] = 0;
		compiler.local_name_offsets[compiler.n_local_names++] = offset;

		if (has_receive_marker)
			receive_slots[n_received++] = slot;
		if (has_lvar_marker)
			lvar_slots[n_lvars++] = slot;
	}

	int n_declared = compiler.n_local_names - scope_start;
	if (n_declared == 0) {
		if (n_globals == 0)
			fail(interp, "|: empty locals list; omit it");
		return;
	}

	for (int i = 0; i < n_received; i++)
		compiler.local_stored[scope_start + receive_slots[i]] = 1;
	for (int i = 0; i < n_lvars; i++)
		compiler.local_stored[scope_start + lvar_slots[i]] = 1;

	compiler.local_scope_entry_cells[scope_idx] = vocab.here;

	if (n_received == n_declared) {
		emit_call(interp, vocab.enter_locals_to_cfa);
		emit(interp, (cell)n_declared);
		emit(interp, (cell)n_declared);
	} else if (n_received == 0) {
		emit_call(interp, vocab.enter_locals_cfa);
		emit(interp, (cell)n_declared);
	} else {
		emit_call(interp, vocab.enter_locals_mixed_cfa);
		emit(interp, (cell)n_declared);
		emit(interp, (cell)n_received);
		for (int i = 0; i < n_received; i++)
			emit(interp, (cell)receive_slots[i]);
	}

	if (n_lvars > 0) {
		int lvar_cfa = find("lvar");
		for (int i = 0; i < n_lvars; i++) {
			emit_call(interp, lvar_cfa);
			emit_call(interp, vocab.local_store_0depth_cfa);
			emit(interp, (cell)lvar_slots[i]);
		}
	}
}

void p_bar(DISPATCH_ARGS) {
	compile_locals_decl(interp);
	if (!interp->error_flag)
		hoist_assigned_locals(interp);

	DISPATCH(interp);
}

void p_to_var(DISPATCH_ARGS) {
	int var_cfa = (int)vocab.dict[interp->ip++];
	POP(value);
	vocab.dict[var_cfa + 1] = (cell)value.bits;

	DISPATCH(interp);
}

int reject_outer_local(Interpreter *interp, const char *token) {
	if (compiler.found_local_scope >= compiler.n_local_scopes - 1)
		return 0;

	rollback_partial_definition();
	fail(interp, "%s is not bound in this quotation; pass it in or use pick", token);
	return 1;
}

static void emit_local_store(Interpreter *interp, int local_depth, int local_slot_idx) {
	if (try_fuse_local_acc(interp, local_depth, local_slot_idx))
		return;
	if (try_fuse_local_arith_store(interp, local_depth, local_slot_idx))
		return;
	if (try_fuse_stack_local_store(interp, local_depth, local_slot_idx))
		return;

	if (local_depth == 0) {
		emit_call(interp, vocab.local_store_0depth_cfa);
		emit(interp, (cell)local_slot_idx);
	} else {
		emit_call(interp, vocab.local_store_cfa);
		emit(interp, (cell)local_depth);
		emit(interp, (cell)local_slot_idx);
	}
}

static int declare_local_in_scope(Interpreter *interp, const char *token) {
	int name_len = (int)strlen(token);

	if (compiler.local_names_pool_here + name_len + 1 > LOCAL_NAMES_POOL_SIZE) {
		fail(interp, "to: local names pool full");
		return -1;
	}
	if (compiler.n_local_names >= MAX_LOCAL_NAMES) {
		fail(interp, "to: too many local names (max %d)", MAX_LOCAL_NAMES);
		return -1;
	}

	int offset = compiler.local_names_pool_here;
	memcpy(&compiler.local_names_pool[offset], token, (size_t)name_len);
	compiler.local_names_pool[offset + name_len] = 0;
	compiler.local_names_pool_here += name_len + 1;
	compiler.local_fetched[compiler.n_local_names] = 0;
	compiler.local_stored[compiler.n_local_names] = 1;
	compiler.local_name_offsets[compiler.n_local_names++] = offset;

	return compiler.n_local_names - 1 - compiler.local_scope_starts[compiler.n_local_scopes - 1];
}

void p_to(DISPATCH_ARGS) {
	char *token = next_token();
	if (!token) {
		fail(interp, "to: expected a name");
		return;
	}

	if (compiler.compiling) {
		int local_depth, local_slot_idx;
		if (find_local(token, &local_depth, &local_slot_idx)) {
			if (reject_outer_local(interp, token))
				return;
			compiler.local_stored[compiler.found_local_name_idx] = 1;
			emit_local_store(interp, local_depth, local_slot_idx);
			return;
		}
	}

	int target_cfa = find(token);
	if (!target_cfa) {
		if (compiler.compiling) {
			int scope_idx = compiler.n_local_scopes - 1;

			if (compiler.local_scope_entry_cells[scope_idx] < 0) {
				if (compiler.conditional_depth > 0 || compiler.loop_begin != 0) {
					fail(interp, "to: %s is this body's first local and sits inside a branch; assign it before the branch", token);
					return;
				}

				compiler.local_scope_entry_cells[scope_idx] = vocab.here;
				emit_call(interp, vocab.enter_locals_cfa);
				emit(interp, 0);
			}

			int declared_slot = declare_local_in_scope(interp, token);
			if (declared_slot < 0)
				return;

			emit_local_store(interp, 0, declared_slot);
			return;
		}
		target_cfa = create_variable(interp, token);
	}

	cfa_handler h = (cfa_handler)vocab.dict[target_cfa];
	if (h != dovar) {
		fail(interp, "to: %s is already a word, not a variable", token);
		return;
	}

	if (compiler.compiling) {
		if (!superword_try_fuse_store(interp, target_cfa)) {
			emit_call(interp, vocab.to_var_cfa);
			emit(interp, (cell)target_cfa);
		}
	} else {
		POP(value);
		vocab.dict[target_cfa + 1] = (cell)value.bits;
	}

	DISPATCH(interp);
}

static void compile_local_unary(Interpreter *interp, const char *op,
                                int depth0_cfa, int fallback_cfa) {
	char *token = next_token();
	if (!token) {
		fail(interp, "%s: expected a name", op);
		return;
	}
	if (!compiler.compiling) {
		fail(interp, "%s: only valid inside a colon definition", op);
		return;
	}
	int depth, slot;
	if (find_local(token, &depth, &slot)) {
		if (reject_outer_local(interp, token))
			return;
		compiler.local_fetched[compiler.found_local_name_idx] = 1;
		if (depth == 0) {
			emit_call(interp, depth0_cfa);
			emit(interp, (cell)slot);
		} else {
			emit_call(interp, vocab.local_fetch_cfa);
			emit(interp, (cell)depth);
			emit(interp, (cell)slot);
			emit_call(interp, fallback_cfa);
			emit_call(interp, vocab.local_store_cfa);
			emit(interp, (cell)depth);
			emit(interp, (cell)slot);
		}
		return;
	}

	int target_cfa = find(token);
	if (!target_cfa) {
		fail(interp, "%s: unknown variable: %s; declare it with variable", op, token);
		return;
	}
	if ((cfa_handler)vocab.dict[target_cfa] != dovar) {
		fail(interp, "%s: %s is not a variable", op, token);
		return;
	}
	emit_call(interp, target_cfa);
	emit_call(interp, fallback_cfa);
	emit_call(interp, vocab.to_var_cfa);
	emit(interp, (cell)target_cfa);
}

void p_increment(DISPATCH_ARGS) {
	compile_local_unary(interp, "++",
	                    vocab.local_incr_0depth_cfa,
	                    vocab.inc_cfa);

	DISPATCH(interp);
}

void p_decrement(DISPATCH_ARGS) {
	compile_local_unary(interp, "--",
	                    vocab.local_decr_0depth_cfa,
	                    vocab.dec_cfa);

	DISPATCH(interp);
}

void p_f_increment(DISPATCH_ARGS) {
	compile_local_unary(interp, "f++",
	                    vocab.local_finc_0depth_cfa,
	                    vocab.finc_cfa);

	DISPATCH(interp);
}

void p_f_decrement(DISPATCH_ARGS) {
	compile_local_unary(interp, "f--",
	                    vocab.local_fdec_0depth_cfa,
	                    vocab.fdec_cfa);

	DISPATCH(interp);
}

void p_inline(DISPATCH_ARGS) {
	int latest = vocab.latest_cfa;
	if (!latest) {
		fail(interp, "inline: no recent definition");
		return;
	}

	WORD_FLAGS(latest) |= 2;

	DISPATCH(interp);
}

void p_symbol(DISPATCH_ARGS) {
	char *token = next_token();
	if (!token) {
		fail(interp, "symbol: expected a name");
		return;
	}
	if (token[0] == ':')
		token++;

	create_header(interp, token, 0);
	emit(interp, (cell)&dosym);

	emit(interp, (cell)intern_symbol(interp, token));

	DISPATCH(interp);
}

void p_string_to_symbol(DISPATCH_ARGS) {
	PEEK_STRING_AT(string, 0, "string>symbol");
	int symbol_cfa = intern_symbol(interp, string->bytes);
	if (interp->error_flag)
		return;

	chain_sp[-1] = make_symbol(symbol_cfa);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_internal(DISPATCH_ARGS) {
	if (vocab.latest_cfa == 0) {
		fail(interp, "internal: no definition to mark");
		return;
	}
	WORD_FLAGS(vocab.latest_cfa) |= 4;

	DISPATCH(interp);
}

void p_forget(DISPATCH_ARGS) {
	char *token = next_token();
	if (!token) {
		fail(interp, "forget: expected a name");
		return;
	}
	int target_cfa = find(token);
	if (!target_cfa) {
		fail(interp, "forget: unknown word: %s", token);
		return;
	}
	vocab.here = target_cfa - 4;
	vocab.forget_generation++;
	vocab.names_here = (int)WORD_NAME(target_cfa);
	vocab.latest_cfa = (int)WORD_LINK(target_cfa);

	int max_src_end = 1;
	for (int surviving_cfa = vocab.latest_cfa; surviving_cfa != 0; surviving_cfa = (int)WORD_LINK(surviving_cfa)) {
		int src_offset = (int)WORD_SOURCE(surviving_cfa);
		if (src_offset > 0) {
			int src_end = src_offset + (int)strlen(&vocab.source_pool[src_offset]) + 1;
			max_src_end = MAX(max_src_end, src_end);
		}
	}
	vocab.source_here = max_src_end;
	truncate_quotation_spans();

	DISPATCH(interp);
}
