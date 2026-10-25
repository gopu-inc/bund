/* Pilote : compile repl.c avec -Dmain=bund_main et lui envoie des touches scriptées. */
#include <notcurses/notcurses.h>
int bund_main(int argc, char **argv);
int main(int argc, char **argv) {
    if (argc > 1) stub_set_keys(argv[1]);
    char *fake[] = {argv[0], 0};
    return bund_main(1, fake);
}
