CC ?= cc
PREFIX ?= /usr/local
CFLAGS ?= -std=c11 -Wall -Wextra -O2
NOTCURSES_CFLAGS := $(shell pkg-config --cflags notcurses-core 2>/dev/null)
NOTCURSES_LIBS := $(shell pkg-config --libs notcurses-core 2>/dev/null)
CPPFLAGS += -Iinclude -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
ifneq ($(strip $(NOTCURSES_LIBS)),)
CPPFLAGS += -DBUND_HAVE_NOTCURSES $(NOTCURSES_CFLAGS)
LDLIBS += $(NOTCURSES_LIBS)
endif

LIB_SRC := src/util.c src/config.c src/llm.c src/tools.c src/agent.c src/git.c \
	src/session.c src/ext.c src/memory.c src/editor.c src/completion.c
LIB_OBJ := $(LIB_SRC:.c=.o)

all: bund

bund: $(LIB_OBJ) src/repl.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/test_core: $(LIB_OBJ) tests/test_core.o tests/tui_stub.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/test_editor: src/editor.c tests/test_editor.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

tests/test_completion: src/completion.c tests/test_completion.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

test: tests/test_core tests/test_editor tests/test_completion
	./tests/test_core
	./tests/test_editor
	./tests/test_completion

%.o: %.c include/bund.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# Simulation de la TUI sans terminal (faux Notcurses) :
#   make tui-sim && STUB_ROWS=30 STUB_COLS=40 ./tests/tui_sim 'git {SNAP 1}'
tui-sim:
	sh tests/build_tui_sim.sh

install: bund
	install -d $(PREFIX)/bin
	install -m 755 bund $(PREFIX)/bin/bund

clean:
	rm -rf tests/.sim_obj tests/tui_sim; rm -f $(LIB_OBJ) src/repl.o tests/test_core.o tests/tui_stub.o bund tests/test_core tests/test_editor tests/test_completion

.PHONY: all test install clean tui-sim
