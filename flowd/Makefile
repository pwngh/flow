# Makefile
#
# Build rules for flowd. Conformant POSIX make: the `.POSIX:` target
# on the first non-comment line engages strict POSIX semantics in BSD
# make and gmake. Run ./configure first; it generates config.mk. The
# Makefile does NOT invoke configure — that is a manual step performed
# once after checkout and again whenever the build environment changes
# (the discipline borrowed from autoconf/automake without the
# autotools).
#
# Artifacts produced:
#
#   libflowd.a   The runtime as a library. Hosts link this directly
#                from C, or via the Python and TypeScript bindings.
#   flowd        The CLI binary. Links statically against libflowd.a.
#                Surface: --version, --help, plus replay/diff
#                subcommands and the test-only --load-ir, all wired
#                through libflowd's public API.
#
# To build from a clean checkout:
#
#     ./configure
#     make
#
# To run the test suite:           make test
# To run the sanitizer matrix:     make sanitize
# To remove build artifacts:       make clean

.POSIX:

include config.mk

PREFIX = /usr/local

# ---------------------------------------------------------------------
# Flags
#
# CONFIG_* are populated by configure; we copy them into the standard
# names ($(CFLAGS) etc.) so a user can override on the command line
# with `make CFLAGS=-O2` in the usual POSIX manner.
#
# GEN_CFLAGS is the relaxed flag set used only when compiling third-
# party code we vendor (cJSON). It is not our code, routinely trips
# -Wpedantic, and isolating its build flags avoids disabling warnings
# globally for our own translation units.
# ---------------------------------------------------------------------

CFLAGS     = $(CONFIG_CFLAGS)
CPPFLAGS   = $(CONFIG_CPPFLAGS)
LDFLAGS    = $(CONFIG_LDFLAGS)
LDLIBS     = $(CONFIG_LDLIBS)
GEN_CFLAGS = $(CONFIG_GEN_CFLAGS) $(CONFIG_CPPFLAGS)

AR      = ar
ARFLAGS = rcs

# ---------------------------------------------------------------------
# Object lists
#
# LIB_OBJS is everything that goes into libflowd.a. main.o is the
# CLI driver and is NOT in the library; it links against libflowd.a
# to produce the flowd binary.
#
# Vendored cJSON is part of the library so that bindings linking only
# against libflowd.a still get JSON parsing.
# ---------------------------------------------------------------------

LIB_OBJS = src/util.o \
           src/sha256_builtin.o \
           src/stub_host.o \
           src/value.o \
           src/ir_load.o \
           src/exec.o \
           src/trace.o \
           src/gateway.o \
           src/adapters/anthropic.o \
           src/flowd.o \
           src/cjson/cJSON.o

CLI_OBJS = src/main.o

# Test binaries link against libflowd.a. They live under tests/api/
# and are built as part of `make test`.
TEST_OBJS = tests/api/test_value.o tests/api/test_exec.o tests/api/test_trace.o tests/api/test_gateway.o tests/api/test_suspension.o tests/api/test_anthropic.o tests/api/test_budget_cancel.o tests/api/test_replay_restore.o tests/api/mock_server.o
TEST_BINS = tests/api/test_value tests/api/test_exec tests/api/test_trace tests/api/test_gateway tests/api/test_suspension tests/api/test_anthropic tests/api/test_budget_cancel tests/api/test_replay_restore

# ---------------------------------------------------------------------
# Top-level targets
# ---------------------------------------------------------------------

all: flowd libflowd.a

libflowd.a: $(LIB_OBJS)
	$(AR) $(ARFLAGS) $@ $(LIB_OBJS)

flowd: $(CLI_OBJS) libflowd.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(CLI_OBJS) libflowd.a $(LDLIBS)

# ---------------------------------------------------------------------
# Object rules
#
# Written explicitly rather than via POSIX suffix rules because suffix
# rules apply only to files in the current directory, and we keep all
# sources under src/. Explicit rules also serve as the canonical record
# of each translation unit's local header dependencies, which is what
# drives correct incremental rebuilds.
# ---------------------------------------------------------------------

src/util.o: src/util.c src/util.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ src/util.c

src/sha256_builtin.o: src/sha256_builtin.c src/sha256_builtin.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ src/sha256_builtin.c

src/stub_host.o: src/stub_host.c src/stub_host.h src/flowd.h src/ir_load.h src/runtime_internal.h src/util.h src/cjson/cJSON.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ src/stub_host.c

src/value.o: src/value.c src/value.h src/util.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ src/value.c

src/ir_load.o: src/ir_load.c src/ir_load.h src/value.h src/util.h src/cjson/cJSON.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ src/ir_load.c

src/exec.o: src/exec.c src/exec.h src/ir_load.h src/value.h src/util.h src/trace.h src/cjson/cJSON.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ src/exec.c

src/trace.o: src/trace.c src/trace.h src/value.h src/util.h src/cjson/cJSON.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ src/trace.c

src/gateway.o: src/gateway.c src/gateway.h src/flowd.h src/util.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ src/gateway.c

src/adapters/anthropic.o: src/adapters/anthropic.c src/adapters/anthropic.h src/flowd.h src/cjson/cJSON.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ src/adapters/anthropic.c

src/flowd.o: src/flowd.c src/flowd.h src/exec.h src/gateway.h src/ir_load.h src/util.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ src/flowd.c

src/main.o: src/main.c src/flowd.h src/ir_load.h src/trace.h src/util.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ src/main.c

# cJSON is third-party. Built with the relaxed GEN_CFLAGS so its
# pedantic warnings don't cascade into our build.
src/cjson/cJSON.o: src/cjson/cJSON.c src/cjson/cJSON.h
	$(CC) $(GEN_CFLAGS) -c -o $@ src/cjson/cJSON.c

# ---------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------

tests/api/test_value.o: tests/api/test_value.c src/value.h src/util.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ tests/api/test_value.c

tests/api/test_value: tests/api/test_value.o libflowd.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/api/test_value.o libflowd.a $(LDLIBS)

tests/api/test_exec.o: tests/api/test_exec.c src/flowd.h src/util.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ tests/api/test_exec.c

tests/api/test_exec: tests/api/test_exec.o libflowd.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/api/test_exec.o libflowd.a $(LDLIBS)

tests/api/test_trace.o: tests/api/test_trace.c src/flowd.h src/trace.h src/util.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ tests/api/test_trace.c

tests/api/test_trace: tests/api/test_trace.o libflowd.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/api/test_trace.o libflowd.a $(LDLIBS)

tests/api/test_gateway.o: tests/api/test_gateway.c src/flowd.h src/trace.h src/util.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ tests/api/test_gateway.c

tests/api/test_gateway: tests/api/test_gateway.o libflowd.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/api/test_gateway.o libflowd.a $(LDLIBS)

tests/api/test_suspension.o: tests/api/test_suspension.c src/flowd.h src/trace.h src/util.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ tests/api/test_suspension.c

tests/api/test_suspension: tests/api/test_suspension.o libflowd.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/api/test_suspension.o libflowd.a $(LDLIBS)

tests/api/mock_server.o: tests/api/mock_server.c tests/api/mock_server.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ tests/api/mock_server.c

tests/api/test_anthropic.o: tests/api/test_anthropic.c tests/api/mock_server.h src/flowd.h src/trace.h src/adapters/anthropic.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ tests/api/test_anthropic.c

tests/api/test_anthropic: tests/api/test_anthropic.o tests/api/mock_server.o libflowd.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/api/test_anthropic.o tests/api/mock_server.o libflowd.a $(LDLIBS) -lpthread

tests/api/test_budget_cancel.o: tests/api/test_budget_cancel.c src/flowd.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ tests/api/test_budget_cancel.c

tests/api/test_budget_cancel: tests/api/test_budget_cancel.o libflowd.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/api/test_budget_cancel.o libflowd.a $(LDLIBS)

tests/api/test_replay_restore.o: tests/api/test_replay_restore.c src/flowd.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ tests/api/test_replay_restore.c

tests/api/test_replay_restore: tests/api/test_replay_restore.o libflowd.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/api/test_replay_restore.o libflowd.a $(LDLIBS)

test: flowd $(TEST_BINS)
	./tests/run.sh

# ---------------------------------------------------------------------
# Sanitizer matrix
#
# Two passes: ASAN+UBSAN, then TSAN. Each rebuilds with the
# appropriate flags into a sibling directory so the release build
# stays warm. valgrind is not available on macOS; ASAN's leak and
# UB detection covers the same surface for our purposes.
#
# The sanitizer flags layer on top of CONFIG_CFLAGS via the SAN_*
# variables passed to the recursive make.
# ---------------------------------------------------------------------

sanitize:
	@echo '=== sanitize: ASAN + UBSAN ==='
	$(MAKE) clean
	$(MAKE) test CFLAGS='$(CONFIG_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
	             LDFLAGS='-fsanitize=address,undefined' \
	             GEN_CFLAGS='$(CONFIG_GEN_CFLAGS) $(CONFIG_CPPFLAGS) -O1 -g -fsanitize=address,undefined'
	@echo '=== sanitize: TSAN ==='
	$(MAKE) clean
	$(MAKE) test CFLAGS='$(CONFIG_CFLAGS) -O1 -g -fsanitize=thread' \
	             LDFLAGS='-fsanitize=thread' \
	             GEN_CFLAGS='$(CONFIG_GEN_CFLAGS) $(CONFIG_CPPFLAGS) -O1 -g -fsanitize=thread'
	@echo '=== sanitize: clean rebuild for release ==='
	$(MAKE) clean
	$(MAKE) all

# ---------------------------------------------------------------------
# Housekeeping
# ---------------------------------------------------------------------

clean:
	rm -f flowd libflowd.a $(LIB_OBJS) $(CLI_OBJS) $(TEST_OBJS) $(TEST_BINS)
	rm -f src/adapters/*.o
	rm -rf tests/corpus tests/diff/work

distclean: clean
	rm -f config.mk

.PHONY: all test sanitize clean distclean
