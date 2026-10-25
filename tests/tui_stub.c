/* Headless test builds do not link repl.c (which owns main and Notcurses). */
int tui_is_active(void) { return 0; }
int tui_confirm(const char *question) {
    (void)question;
    return 0;
}
void tui_refresh(void) {}
