# anka-oci-tag-editor
#
# Standalone TUI for editing filesystem-layer tar.gz archives.
# Self-contained: depends only on system zlib and ncurses. No anka code.

CC      ?= cc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter
CFLAGS  += $(EXTRA_CFLAGS)
# macOS libncurses has wide-character support built in; Linux splits it out.
ifeq ($(shell uname -s),Linux)
CURSES_LIB := -lncursesw
else
CURSES_LIB := -lncurses
endif
LDLIBS  := -lz $(CURSES_LIB)
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

BIN     := anka-oci-tag-editor
SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:.c=.o)
HDR     := $(wildcard src/*.h)

LIB_OBJ := $(filter-out src/main.o src/tui.o,$(OBJ))
TEST_SUPPORT := tests/test_support.c tests/test_support.h
TEST_BIN := tests/roundtrip_test tests/vm_mode_test

.PHONY: all build clean install test test-dev phase-build screenshots dev

# Sample layers for `make dev` when DEV_ARGS is not set.
DEV_ARGS ?= testdata/pkg-cmake.tar.gz testdata/pkg-jq.tar.gz

# Plain `make` builds the binary and runs the test suite.
all: build test
	@echo "==> Done"

build: $(BIN)
	@echo "==> Binary ready: $(BIN)"

# Announce the compile phase even when every object is already up to date,
# so a no-op build is visible rather than silent.
phase-build:
	@echo "==> Building $(BIN)"

$(OBJ): | phase-build

$(BIN): $(OBJ)
	@echo "==> Linking $@"
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

# Rebuild every object when any header changes (small project, keep it simple).
$(OBJ): $(HDR)

src/%.o: src/%.c
	@echo "==> Compiling $<"
	$(CC) $(CFLAGS) -c -o $@ $<

tests/%_test: tests/%_test.c $(TEST_SUPPORT) $(LIB_OBJ) $(HDR)
	@echo "==> Building $@"
	$(CC) $(CFLAGS) -o $@ $< tests/test_support.c $(LIB_OBJ) $(LDLIBS)

test: $(TEST_BIN)
	@echo "==> Running tests"
	@for t in $(TEST_BIN); do ./$$t || exit 1; done

# README pictures. Not part of `make`: it needs tmux, and you commit the PNGs.
screenshots: build
	@echo "==> Writing screenshots"
	./scripts/screenshots.sh

# Rebuild and restart the editor when a file under src/ changes.
# make dev DEV_ARGS='--vm myvm'   or   make dev DEV_ARGS='layer.tar.gz'
dev:
	./scripts/dev.sh $(DEV_ARGS)

# Keys and reload of `make dev`. Not part of `make`: it needs tmux.
test-dev: build
	@echo "==> Testing make dev"
	./tests/dev_reload_test.sh

clean:
	@echo "==> Cleaning"
	rm -f $(OBJ) $(BIN) $(TEST_BIN)

install: build
	@echo "==> Installing to $(DESTDIR)$(BINDIR)"
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
