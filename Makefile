# anka-layer-editor
#
# Standalone TUI for editing filesystem-layer tar.gz archives.
# Self-contained: depends only on system zlib and ncurses. No anka code.

CC      ?= cc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter
LDLIBS  := -lz -lncurses
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

BIN     := anka-layer-editor
SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:.c=.o)
HDR     := $(wildcard src/*.h)

LIB_OBJ := src/archive.o src/tar.o src/tree.o
TEST_BIN := tests/roundtrip_test

.PHONY: all clean install test

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

# Rebuild every object when any header changes (small project, keep it simple).
$(OBJ): $(HDR)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_BIN): tests/roundtrip_test.c $(LIB_OBJ) $(HDR)
	$(CC) $(CFLAGS) -o $@ tests/roundtrip_test.c $(LIB_OBJ) $(LDLIBS)

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -f $(OBJ) $(BIN) $(TEST_BIN)

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
