#include <notcurses/notcurses.h>

#include "editor.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXR 80
#define MAXC 240
typedef struct { char s[8]; unsigned char bgr, bgg, bgb, bgset; } cell;
static cell grid[MAXR][MAXC];
static int ROWS = 24, COLS = 80;
static unsigned cur_bg[3];
static int cur_bgset;
static FILE *out;
static long overflow_attempts, oob_attempts;
static uint32_t keys[8192];
static size_t nkeys, kpos;
static int exhausted;

static int parse_hex(const char *s) { return (int)strtol(s, NULL, 10); }

void stub_set_keys(const char *sc) {
    nkeys = kpos = 0;
    for (const char *p = sc; *p;) {
        if (*p == '{') {
            const char *e = strchr(p, '}');
            if (!e) break;
            char tok[48];
            size_t n = (size_t)(e - p - 1);
            if (n >= sizeof tok) n = sizeof tok - 1;
            memcpy(tok, p + 1, n);
            tok[n] = '\0';
            uint32_t k = 0;
            if (!strcmp(tok, "ENTER")) k = NCKEY_ENTER;
            else if (!strcmp(tok, "UP")) k = NCKEY_UP;
            else if (!strcmp(tok, "DOWN")) k = NCKEY_DOWN;
            else if (!strcmp(tok, "LEFT")) k = NCKEY_LEFT;
            else if (!strcmp(tok, "RIGHT")) k = NCKEY_RIGHT;
            else if (!strcmp(tok, "HOME")) k = NCKEY_HOME;
            else if (!strcmp(tok, "END")) k = NCKEY_END;
            else if (!strcmp(tok, "BS")) k = NCKEY_BACKSPACE;
            else if (!strcmp(tok, "DEL")) k = NCKEY_DEL;
            else if (!strcmp(tok, "TAB")) k = '\t';
            else if (!strcmp(tok, "CTRL+O")) k = 15;
            else if (!strcmp(tok, "CTRL+S")) k = 19;
            else if (!strcmp(tok, "CTRL+C")) k = 3;
            else if (!strcmp(tok, "CTRL+L")) k = 12;
            else if (!strcmp(tok, "RESIZE")) k = NCKEY_RESIZE;
            else if (!strncmp(tok, "SNAP", 4)) k = 0xFFFFFF00u + (uint32_t)parse_hex(tok + 4 + (tok[4] == ' '));
            else if (!strncmp(tok, "SIZE ", 5)) {   /* {SIZE 30x50} */
                k = 0xFFFFE000u;
                int r = 0, c = 0;
                sscanf(tok + 5, "%dx%d", &r, &c);
                ROWS = r < MAXR ? r : MAXR;
                COLS = c < MAXC ? c : MAXC;
            }
            if (k && nkeys < 8190) keys[nkeys++] = k;
            p = e + 1;
        } else {
            size_t b;
            ed_cwidth(p, strlen(p), &b);
            if (!b) b = 1;
            uint32_t cp = (unsigned char)p[0];
            if (b == 2) cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
            else if (b == 3) cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
            else if (b == 4) cp = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
            if (nkeys < 8190) keys[nkeys++] = cp;
            p += b;
        }
    }
}

static void clear_grid(void) {
    for (int y = 0; y < MAXR; y++)
        for (int x = 0; x < MAXC; x++) { grid[y][x].s[0] = ' '; grid[y][x].s[1] = 0; grid[y][x].bgset = 0; }
}

struct notcurses *notcurses_core_init(const struct notcurses_options *o, FILE *f) {
    (void)o; (void)f;
    const char *r = getenv("STUB_ROWS"), *c = getenv("STUB_COLS");
    if (r) ROWS = atoi(r);
    if (c) COLS = atoi(c);
    if (ROWS > MAXR) ROWS = MAXR;
    if (COLS > MAXC) COLS = MAXC;
    out = fdopen(dup(1), "w");
    clear_grid();
    return (struct notcurses *)&grid;
}
struct ncplane *notcurses_stdplane(struct notcurses *n) { return (struct ncplane *)n; }
int notcurses_stop(struct notcurses *n) {
    (void)n;
    fprintf(out, "\n[stub] écritures hors écran ignorées : %ld ; débordements à droite coupés : %ld\n",
            oob_attempts, overflow_attempts);
    fflush(out);
    return 0;
}
void ncplane_dim_yx(const struct ncplane *p, unsigned *r, unsigned *c) { (void)p; *r = (unsigned)ROWS; *c = (unsigned)COLS; }
void ncplane_erase(struct ncplane *p) { (void)p; clear_grid(); }
int ncplane_set_fg_rgb8(struct ncplane *p, unsigned r, unsigned g, unsigned b) { (void)p; (void)r; (void)g; (void)b; return 0; }
int ncplane_set_bg_rgb8(struct ncplane *p, unsigned r, unsigned g, unsigned b) {
    (void)p;
    cur_bg[0] = r; cur_bg[1] = g; cur_bg[2] = b; cur_bgset = 1;
    return 0;
}
void ncplane_set_fg_default(struct ncplane *p) { (void)p; }
void ncplane_set_bg_default(struct ncplane *p) { (void)p; cur_bgset = 0; }
void ncplane_set_styles(struct ncplane *p, unsigned s) { (void)p; (void)s; }

int ncplane_putstr_yx(struct ncplane *p, int y, int x, const char *s) {
    (void)p;
    if (y < 0 || y >= ROWS || x < 0 || x >= COLS) { oob_attempts++; return -1; }
    int written = 0;
    for (size_t i = 0; s[i];) {
        size_t b;
        int w = ed_cwidth(s + i, strlen(s + i), &b);
        if (!b) b = 1;
        if (w == 0) w = 0;
        if (x + w > COLS) { overflow_attempts++; return -written - 1; }
        if (w > 0 || 1) {
            cell *c = &grid[y][x];
            size_t cp = b < sizeof c->s - 1 ? b : sizeof c->s - 1;
            memcpy(c->s, s + i, cp);
            c->s[cp] = 0;
            c->bgset = (unsigned char)cur_bgset;
            c->bgr = (unsigned char)cur_bg[0]; c->bgg = (unsigned char)cur_bg[1]; c->bgb = (unsigned char)cur_bg[2];
            if (w == 2 && x + 1 < COLS) { grid[y][x + 1].s[0] = 0; grid[y][x + 1].bgset = c->bgset; }
        }
        x += w ? w : 0;
        written += w;
        i += b;
    }
    return written;
}

int notcurses_render(struct notcurses *n) { (void)n; return 0; }

static void snapshot(int id) {
    fprintf(out, "\n===== SNAP %d (%dx%d) =====\n", id, ROWS, COLS);
    int cy = -1, cx = -1;
    for (int y = 0; y < ROWS; y++) {
        char line[MAXC * 5];
        size_t n = 0;
        for (int x = 0; x < COLS; x++) {
            cell *c = &grid[y][x];
            if (c->bgset && c->bgr == 0 && c->bgg == 230 && c->bgb == 200) { cy = y; cx = x; }
            if (c->s[0] == 0) continue;
            size_t l = strlen(c->s);
            memcpy(line + n, c->s, l);
            n += l;
        }
        while (n && line[n - 1] == ' ') n--;
        line[n] = 0;
        fprintf(out, "%02d|%s\n", y, line);
    }
    fprintf(out, "curseur: %d,%d\n", cy, cx);
    fflush(out);
}

uint32_t notcurses_getc_blocking(struct notcurses *n, struct ncinput *ni) {
    (void)n;
    ni->id = 0;
    for (;;) {
        if (kpos >= nkeys) {
            if (++exhausted > 10) { fprintf(out, "[stub] script épuisé\n"); fflush(out); exit(3); }
            return 4;
        }
        uint32_t k = keys[kpos++];
        if (k >= 0xFFFFFF00u) { snapshot((int)(k - 0xFFFFFF00u)); continue; }
        if (k == 0xFFFFE000u) { clear_grid(); continue; }
        return k;
    }
}
