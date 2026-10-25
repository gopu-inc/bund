#!/bin/sh
# Compile la TUI contre un faux Notcurses (tests/nc_stub) pour la rejouer sans terminal.
set -e
cd "$(dirname "$0")/.."
OUT=tests/.sim_obj
mkdir -p "$OUT"
F="-std=c11 -g -Wall -Wextra -Iinclude -Itests/nc_stub -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
 -DBUND_HAVE_NOTCURSES -DBUND_ASSUME_TTY -DBUND_ASSUME_UTF8"
${CC:-cc} $F -Dmain=bund_main -c src/repl.c -o "$OUT/repl.o"
for f in util config llm tools agent git session memory editor completion; do
    ${CC:-cc} $F -c "src/$f.c" -o "$OUT/$f.o"
done
${CC:-cc} $F -c tests/nc_stub/nc_stub.c -o "$OUT/stub.o"
${CC:-cc} $F -c tests/tui_sim.c -o "$OUT/drv.o"
${CC:-cc} -o tests/tui_sim "$OUT"/*.o
echo "OK : tests/tui_sim"
echo "Exemple : STUB_ROWS=30 STUB_COLS=40 ./tests/tui_sim 'git {SNAP 1}'"
echo "Touches : texte libre, {ENTER} {TAB} {UP} {DOWN} {LEFT} {RIGHT} {HOME} {END} {BS} {DEL} {CTRL+O} {CTRL+S} {CTRL+C} {CTRL+L} {SNAP n}"
