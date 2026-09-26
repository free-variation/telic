#include "telic.h"

#ifndef WASM_ARENA_RESERVE
#define WASM_ARENA_RESERVE ((size_t)1 << 30)
#endif

void *platform_reserve(size_t requested, size_t *reserved_out) {
	size_t reservation = requested ? requested : WASM_ARENA_RESERVE;
	void *base = malloc(reservation);
	if (!base)
		return NULL;
	*reserved_out = reservation;
	return base;
}

void platform_init(void) {
}

int platform_computer_gauges(ComputerGauges *out) {
	(void)out;
	return 0;
}

int platform_edit_file(const char *path) {
	(void)path;
	return 0;
}

int platform_temp_file(char *path, int capacity, const char *suffix) {
	(void)path;
	(void)capacity;
	(void)suffix;
	return 0;
}

int platform_executable_path(char *out, size_t cap) {
	(void)out;
	(void)cap;
	return 0;
}

void platform_tick_every(struct Interpreter *interp, double seconds) {
	(void)interp;
	(void)seconds;
}

void platform_interrupt_begin(struct Interpreter *interp) {
	(void)interp;
}

int platform_repl_begin(struct Interpreter *interp, int want_interactive) {
	(void)interp;
	if (want_interactive) {
		printf("telic %s\n", VERSION);
		printf("%swords%s lists every word; %shelp%s shows a quick start; %sbye%s quits\n",
				term_bold(), term_plain(), term_bold(), term_plain(), term_bold(), term_plain());
	}
	return want_interactive;
}

int platform_read_chunk(char *dst, int dst_avail, int interactive) {
	(void)interactive;
	if (!fgets(dst, dst_avail, stdin))
		return 0;
	return (int)strlen(dst);
}

static void *wasi_qsort_thunk;
static int (*wasi_qsort_cmp)(void *, const void *, const void *);

static int wasi_qsort_trampoline(const void *a, const void *b) {
	return wasi_qsort_cmp(wasi_qsort_thunk, a, b);
}

void platform_qsort_r(void *base, size_t n, size_t size, void *thunk,
		int (*cmp)(void *, const void *, const void *)) {
	wasi_qsort_thunk = thunk;
	wasi_qsort_cmp = cmp;
	qsort(base, n, size, wasi_qsort_trampoline);
}

#define WASM_UNSUPPORTED(fn, word) \
	void fn(DISPATCH_ARGS) { \
		fail(interp, "unsupported on the wasm build"); \
		DISPATCH(interp); \
	}

WASM_UNSUPPORTED(p_start_process, "start-process")
WASM_UNSUPPORTED(p_stdout_to_string, "stdout>string")
WASM_UNSUPPORTED(p_wait, "wait")
WASM_UNSUPPORTED(p_stop_process, "stop")
WASM_UNSUPPORTED(p_running, "running?")
WASM_UNSUPPORTED(p_ffi_open, "ffi-open")
WASM_UNSUPPORTED(p_ffi_function, "ffi-function")
WASM_UNSUPPORTED(p_ffi_variadic, "ffi-variadic")
WASM_UNSUPPORTED(p_ffi_call, "(ffi-call)")
WASM_UNSUPPORTED(p_ffi_free, "ffi-free")
WASM_UNSUPPORTED(p_matrix_to_pointer, "matrix>pointer")
WASM_UNSUPPORTED(p_floats_to_matrix, "floats>matrix")
WASM_UNSUPPORTED(p_segment_to_pointer, "segment>pointer")
WASM_UNSUPPORTED(p_pointer_cell, "pointer-cell")
WASM_UNSUPPORTED(p_pointer_deref, "pointer-deref")
WASM_UNSUPPORTED(p_pointer_long, "pointer-long")
WASM_UNSUPPORTED(p_pointer_string_at, "pointer-string-at")
WASM_UNSUPPORTED(p_pointer_to_address, "pointer>address")

int ffi_register_call_cfa(int cfa) {
	return cfa;
}

Val ffi_pointer_owner_of(int index) {
	(void)index;
	return make_tagged(T_NONE, 0);
}

int platform_run_main(int argc, char **argv, MainBody body) {
	return body(argc, argv);
}

#define NO_WINDOW_SYSTEM(c_name, word_name) \
	void c_name(DISPATCH_ARGS) { \
		(void)chain_ip; \
		(void)chain_sp; \
		fail(interp, "%s needs a window system; the wasm build has none", word_name); \
	}

NO_WINDOW_SYSTEM(p_circle, "circle")
NO_WINDOW_SYSTEM(p_cls, "cls")
NO_WINDOW_SYSTEM(p_fill_circle, "fill-circle")
NO_WINDOW_SYSTEM(p_fill_rect, "fill-rect")
NO_WINDOW_SYSTEM(p_ink, "ink")
NO_WINDOW_SYSTEM(p_line, "line")
NO_WINDOW_SYSTEM(p_paper, "paper")
NO_WINDOW_SYSTEM(p_plot, "plot")
NO_WINDOW_SYSTEM(p_print_at, "print-at")
NO_WINDOW_SYSTEM(p_rect, "rect")
NO_WINDOW_SYSTEM(p_screen_size, "screen-size")
NO_WINDOW_SYSTEM(p_screen_zoom, "screen-zoom")
NO_WINDOW_SYSTEM(p_screen_shader, "screen-shader")
NO_WINDOW_SYSTEM(p_screen_effect, "screen-effect")
NO_WINDOW_SYSTEM(p_screen_frame, "screen-frame")
NO_WINDOW_SYSTEM(p_screen_frames, "screen-frames")
