UNAME  = $(shell uname -s)

# Compiler selection. An explicit CC (from the environment or `make CC=...`)
# always wins — tested via $(origin) because make predefines CC=cc with origin
# 'default', so `?=` would never fire. When CC is not set explicitly: macOS uses
# clang (Apple clang; gcc < 15 can't compile the musttail dispatch), and Linux
# prefers gcc >= 15 when present (measured faster than clang here on Zen4),
# falling back to clang. Probe order: gcc-15, the /usr/local install, then gcc.
ifeq ($(origin CC),default)
ifeq ($(UNAME),Darwin)
CC := clang
else
CC := $(shell for c in gcc-15 /usr/local/gcc-15/bin/gcc gcc; do \
		v=$$(command -v $$c >/dev/null 2>&1 && $$c -dumpversion 2>/dev/null | cut -d. -f1); \
		if [ -n "$$v" ] && [ "$$v" -ge 15 ] 2>/dev/null; then echo $$c; exit 0; fi; \
	done; echo clang)
endif
endif

# A gcc build statically links libgcc so the binary stays runnable off a
# /usr/local gcc install whose libgcc_s isn't on the default loader path.
ifeq ($(findstring clang,$(CC)),)
LDLIBS_CC = -static-libgcc
endif
# -fno-common: a tentative definition becomes an ordinary .bss definition rather
# than a common symbol. Mach-O gathers commons into __DATA,__common and asks for
# an alignment the segment cannot hold ("reducing alignment ... from 0x8000"),
# and a global tentatively defined in two translation units is a link error here
# instead of being silently merged into one.
CFLAGS = -O3 -march=native -Wall -Wextra -pthread -D_GNU_SOURCE -fno-common
ifneq ($(UNAME),Darwin)
CFLAGS += -flto
endif
LDLIBS = -lm -lffi

SRCS = src/c/core.c src/c/words.c src/c/compiler.c src/c/io.c src/c/collections.c src/c/matrix.c src/c/statistics.c src/c/indexing.c src/c/functional.c src/c/superwords.c src/c/strings.c src/c/help_table.c src/c/logic.c src/c/database.c src/c/foreign.c src/c/platform_posix.c src/c/dimension.c src/c/time.c src/c/exact.c
HDRS = src/c/water.h src/c/platform.h src/c/lib_embed.h src/c/logo_embed.h src/c/repl_highlight_groups.h

WATER_INCS = -I$(PCRE2_SRC) -I$(SQLITE_DIR) -I$(ISOCLINE_DIR)/include
WATER_DEPS = $(PCRE2_LIB) $(SQLITE_OBJ) $(ISOCLINE_OBJ)

# Embedded library, concatenated in this order. Binding is early: a word must
# be defined in an earlier file than every file that uses it (units before the
# constants that use joule, predicates before the words that call them).
FORTH_SRCS = src/forth/core.h2o src/forth/arrays.h2o src/forth/io.h2o src/forth/strings.h2o src/forth/exceptions.h2o src/forth/test.h2o src/forth/matrix.h2o src/forth/subprocess.h2o src/forth/browser.h2o src/forth/logic.h2o src/forth/generators.h2o src/forth/units.h2o src/forth/datasets.h2o src/forth/statistics.h2o src/forth/constants.h2o src/forth/database.h2o src/forth/repl.h2o

# Vendored PCRE2 (see external/pcre2/PROVENANCE; refresh with tools/vendor-pcre2.sh).
PCRE2_DIR    = external/pcre2
PCRE2_SRC    = $(PCRE2_DIR)/src
PCRE2_LIB    = $(PCRE2_DIR)/libpcre2-8.a
PCRE2_CFLAGS = -O2 -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8 -DPCRE2_STATIC -I$(PCRE2_SRC)
PCRE2_OBJS   = $(patsubst %.c,%.o,$(wildcard $(PCRE2_SRC)/pcre2_*.c))

# Vendored SQLite (see external/sqlite/PROVENANCE; refresh with tools/vendor-sqlite.sh).
# Compiled once to a cached object and linked into the binary. THREADSAFE=2
# (not 0): each thread uses its own connection per PLAN.md's HTTP worker pool and
# fork-join models; 0 would drop SQLite's internal-global mutexing and corrupt them.
SQLITE_DIR    = external/sqlite
SQLITE_DEFS   = -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0 \
                -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
                -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE \
                -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_USE_ALLOCA
SQLITE_CFLAGS = -O2 -DSQLITE_THREADSAFE=2 $(SQLITE_DEFS)
SQLITE_OBJ    = $(SQLITE_DIR)/sqlite3.o

# Vendored isocline (see external/isocline/PROVENANCE; refresh with tools/vendor-isocline.sh).
# Compiles as a single source unit (src/isocline.c) per its readme.md.
ISOCLINE_DIR    = external/isocline
ISOCLINE_CFLAGS = -O2 -I$(ISOCLINE_DIR)/include
ISOCLINE_OBJ    = $(ISOCLINE_DIR)/isocline.o

# Vendored LAPACKE closure (see external/lapacke/PROVENANCE; refresh with
# tools/vendor-lapacke.sh). C wrappers over the platform's Fortran LAPACK,
# built into a shared library that water dlopens via FFI: Accelerate on
# Darwin (re-exported, so BLAS rides the same handle), OpenBLAS on Linux
# (an ELF dependency, which dlsym searches through the same handle).
# Add a routine: re-vendor with it, then add it to LAPACKE_ROUTINES.
LAPACKE_DIR      = external/lapacke
LAPACKE_SRCS     = $(wildcard $(LAPACKE_DIR)/src/*.c) $(wildcard $(LAPACKE_DIR)/utils/*.c)
LAPACKE_OBJS     = $(patsubst %.c,%.o,$(LAPACKE_SRCS))
LAPACKE_LIB      = $(LAPACKE_DIR)/liblapacke.a
LAPACKE_SHARED   = $(LAPACKE_DIR)/liblapacke_water.so
LAPACKE_ROUTINES = LAPACKE_dgesvd LAPACKE_dgelsd LAPACKE_dgels LAPACKE_dpotrf LAPACKE_dpotrs
LAPACKE_CFLAGS   = -O2 -DNDEBUG -DADD_ -I$(LAPACKE_DIR)/include
ifneq ($(UNAME),Darwin)
LAPACKE_CFLAGS  += -fPIC -ffunction-sections -fdata-sections
endif

all: water $(LAPACKE_SHARED)

water: $(SRCS) $(HDRS) $(WATER_DEPS)
	$(CC) $(CFLAGS) $(WATER_INCS) -o water $(SRCS) $(WATER_DEPS) $(LDLIBS) $(LDLIBS_CC)

$(PCRE2_LIB): $(PCRE2_OBJS)
	ar rcs $@ $(PCRE2_OBJS)

$(PCRE2_SRC)/%.o: $(PCRE2_SRC)/%.c
	$(CC) $(PCRE2_CFLAGS) -c $< -o $@

$(SQLITE_OBJ): $(SQLITE_DIR)/sqlite3.c $(SQLITE_DIR)/sqlite3.h
	$(CC) $(SQLITE_CFLAGS) -c $< -o $@

# src/isocline.c #includes the rest of src/, so the object must depend on all of
# them: listing only isocline.c leaves edits to tty.c and friends unbuilt.
ISOCLINE_SRCS = $(wildcard $(ISOCLINE_DIR)/src/*.c $(ISOCLINE_DIR)/src/*.h $(ISOCLINE_DIR)/include/*.h)

$(ISOCLINE_OBJ): $(ISOCLINE_SRCS)
	$(CC) $(ISOCLINE_CFLAGS) -c $(ISOCLINE_DIR)/src/isocline.c -o $@

# WASM/WASI cross-build (a-shell, standalone runtimes, browser via a WASI shim).
# Shares the vendored pcre2 dependency, compiled no-JIT for wasm, and sqlite
# (compiled single-threaded, THREADSAFE=0). isocline and ffi are unavailable on
# WASI and excluded, their words present as erroring stubs from platform_wasi.c.
# Needs the wasi-sdk toolchain in $(WASI_SDK).
WASI_SDK        = $(HOME)/wasi-sdk
WASI_CC         = $(WASI_SDK)/bin/clang
WASI_AR         = $(WASI_SDK)/bin/llvm-ar
WASI_SYSROOT    = $(WASI_SDK)/share/wasi-sysroot
WASM_SRCS       = src/c/core.c src/c/words.c src/c/compiler.c src/c/io.c src/c/collections.c src/c/matrix.c src/c/statistics.c src/c/indexing.c src/c/functional.c src/c/superwords.c src/c/strings.c src/c/help_table.c src/c/logic.c src/c/database.c src/c/dimension.c src/c/platform_wasi.c src/c/time.c src/c/exact.c
WASM_CFLAGS     = --sysroot $(WASI_SYSROOT) -O2 -I$(PCRE2_SRC) -I$(SQLITE_DIR) -Wno-ignored-pragmas -Wl,-z,stack-size=8388608
WASM_PCRE2_OBJS = $(patsubst %.c,%.wasm.o,$(wildcard $(PCRE2_SRC)/pcre2_*.c))
WASM_PCRE2_LIB  = $(PCRE2_DIR)/libpcre2-8-wasm.a
WASM_SQLITE_OBJ = $(SQLITE_DIR)/sqlite3.wasm.o

wasm: water.wasm

water.wasm: $(WASM_SRCS) $(HDRS) $(WASM_PCRE2_LIB) $(WASM_SQLITE_OBJ)
	$(WASI_CC) $(WASM_CFLAGS) -o water.wasm $(WASM_SRCS) $(WASM_PCRE2_LIB) $(WASM_SQLITE_OBJ)

$(WASM_PCRE2_LIB): $(WASM_PCRE2_OBJS)
	$(WASI_AR) rcs $@ $(WASM_PCRE2_OBJS)

$(PCRE2_SRC)/%.wasm.o: $(PCRE2_SRC)/%.c
	$(WASI_CC) --sysroot $(WASI_SYSROOT) $(PCRE2_CFLAGS) -c $< -o $@

$(WASM_SQLITE_OBJ): $(SQLITE_DIR)/sqlite3.c $(SQLITE_DIR)/sqlite3.h
	$(WASI_CC) --sysroot $(WASI_SYSROOT) -O2 -DSQLITE_THREADSAFE=0 $(SQLITE_DEFS) -c $< -o $@

# Build the LAPACKE shared library that FFI dlopens.
lapacke: $(LAPACKE_SHARED)

ifeq ($(UNAME),Darwin)
# -exported_symbol roots the routines we expose (the linker pulls their closure
# from the archive) and limits the exports to them; -dead_strip drops anything
# unreachable. -framework Accelerate resolves the Fortran dgesvd_/etc.
$(LAPACKE_SHARED): $(LAPACKE_LIB)
	$(CC) -dynamiclib -o $@ $(foreach routine,$(LAPACKE_ROUTINES),-Wl,-exported_symbol,_$(routine)) -Wl,-dead_strip -Wl,-reexport_framework,Accelerate $(LAPACKE_LIB) -framework Accelerate
else
# The version script is the ELF twin of -exported_symbol: the named routines
# stay global, everything else goes local, and --gc-sections drops what they
# don't reach. --whole-archive pulls every member in first, since nothing
# references the routines yet. cblas_dgemm etc. resolve at run time through
# the DT_NEEDED openblas entry, which dlsym searches from the same handle.
LAPACKE_MAP = $(LAPACKE_DIR)/exports.map

$(LAPACKE_MAP): Makefile
	printf '{ global: %s local: *; };\n' '$(foreach routine,$(LAPACKE_ROUTINES),$(routine);)' > $@

$(LAPACKE_SHARED): $(LAPACKE_LIB) $(LAPACKE_MAP)
	$(CC) -shared -o $@ -Wl,--version-script,$(LAPACKE_MAP) -Wl,--gc-sections -Wl,--whole-archive $(LAPACKE_LIB) -Wl,--no-whole-archive -lopenblas
endif

$(LAPACKE_LIB): $(LAPACKE_OBJS)
	ar rcs $@ $(LAPACKE_OBJS)

$(LAPACKE_DIR)/%.o: $(LAPACKE_DIR)/%.c
	$(CC) $(LAPACKE_CFLAGS) -c $< -o $@

src/c/help_table.c: docs/reference.md docs/reference-libraries.md tools/gen-help.py
	python3 tools/gen-help.py

src/c/lib_embed.h: $(FORTH_SRCS) Makefile
	scratch=$$(mktemp -d) && cat $(FORTH_SRCS) > $$scratch/lib.h2o && (cd $$scratch && xxd -i lib.h2o) > src/c/lib_embed.h && rm -rf $$scratch

src/c/logo_embed.h: water-logo.txt
	xxd -i water-logo.txt > src/c/logo_embed.h

# Regenerate the editor syntax files from docs/reference.md (not compiled, so
# on-demand rather than a build dependency). Run after editing reference.md.
editors src/c/repl_highlight_groups.h: docs/reference.md tools/gen-editors.py
	python3 tools/gen-editors.py

vendor-pcre2:
	sh tools/vendor-pcre2.sh

vendor-sqlite:
	sh tools/vendor-sqlite.sh

vendor-isocline:
	sh tools/vendor-isocline.sh

vendor-lapacke:
	sh tools/vendor-lapacke.sh

# The doc tests are generated from the runnable (```forth) documentation
# fences: the README taste pair (committed .expected) and one trio per
# reference section with examples (fully generated, gitignored).
.PHONY: docs-tests
docs-tests:
	python3 tools/gen-docs-tests.py

# The language pack: the whole language in one generated file (plus an
# llms.txt copy) sized for a model's context window.
.PHONY: pack
pack:
	python3 tools/gen-pack.py

test: water docs-tests
	sh tests/run.sh

# The language-pack acceptance battery (RELEASE-PLAN.md, release mechanics): each
# tests/acceptance prompt goes to a model with only water-pack.md as context
# and the generated program is compared against the expected output. Needs
# ANTHROPIC_API_KEY and the anthropic SDK in .venv; costs API tokens, so it
# is opt-in and outside `make test`. Extra flags via ACCEPTANCE_FLAGS
# (--samples N, --no-pack, --repair, --tasks GLOB, --model ID).
.PHONY: acceptance acceptance-refs
acceptance: water pack
	.venv/bin/python tools/run-acceptance.py $(ACCEPTANCE_FLAGS)

# The battery's reference solutions against their expected output — no API
# calls. A language change that breaks one invalidates that task's .expected.
acceptance-refs: water
	@fail=0; for f in tests/acceptance/*.h2o; do \
		./water -b "$$f" | diff -q - "$${f%.h2o}.expected" > /dev/null \
			|| { echo "FAIL $$f"; fail=1; }; \
	done; \
	[ $$fail -eq 0 ] && echo "acceptance reference solutions reproduce"

# Loadable-library golden tests (tests/lib/): they load a lib/ library and need
# its external deps (LAPACK via liblapacke_water, xgboost via libxgboost).
# Excluded from `make test` so the core suite runs without them. Native-only.
test-libs: water $(LAPACKE_SHARED) docs-tests
	sh tests/run-libs.sh

# Runs the golden suite against the wasm build under a WASI runtime. The runner
# finds wasmtime on PATH or ~/.wasmtime/bin; otherwise set WASMTIME=<path>.
test-wasm: water.wasm docs-tests
	sh tests/run-wasm.sh

bench:
	@sh bench/run-benchmarks.sh

clean:
	rm -f water water.wasm $(PCRE2_OBJS) $(PCRE2_LIB) $(WASM_PCRE2_OBJS) $(WASM_PCRE2_LIB) $(SQLITE_OBJ) $(WASM_SQLITE_OBJ) $(ISOCLINE_OBJ) $(LAPACKE_OBJS) $(LAPACKE_LIB) $(LAPACKE_SHARED) $(LAPACKE_DIR)/exports.map

.PHONY: all clean test test-libs test-wasm bench wasm vendor-pcre2 vendor-sqlite vendor-isocline vendor-lapacke lapacke editors
