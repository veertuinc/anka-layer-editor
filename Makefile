# anka-layer-editor
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

BIN     := anka-layer-editor
SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:.c=.o)
HDR     := $(wildcard src/*.h)

LIB_OBJ := src/archive.o src/tar.o src/tree.o src/session.o src/util.o
TEST_BIN := tests/roundtrip_test

.PHONY: all build clean install test phase-build

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

$(TEST_BIN): tests/roundtrip_test.c $(LIB_OBJ) $(HDR)
	@echo "==> Building tests"
	$(CC) $(CFLAGS) -o $@ tests/roundtrip_test.c $(LIB_OBJ) $(LDLIBS)

test: $(TEST_BIN)
	@echo "==> Running tests"
	./$(TEST_BIN)

clean:
	@echo "==> Cleaning"
	rm -f $(OBJ) $(BIN) $(TEST_BIN)

install: build
	@echo "==> Installing to $(DESTDIR)$(BINDIR)"
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
