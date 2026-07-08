# anka-layer-editor / layeredit
#
# Standalone TUI for editing filesystem-layer tar.gz archives.
# Self-contained: depends only on system zlib and ncurses. No anka code.
CC      ?= cc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter
LDLIBS  := -lz -lncurses
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
BIN     := layeredit
SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:.c=.o)
HDR     := $(wildcard src/*.h)
# Always rebuild from scratch (sequential so -j cannot race clean vs compile).
all:
	$(MAKE) --no-print-directory clean
	$(MAKE) --no-print-directory $(BIN)
$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)
# Rebuild every object when any header changes (small project, keep it simple).
$(OBJ): $(HDR)
src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<
clean:
	rm -f $(OBJ) $(BIN)
install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
.PHONY: all clean install