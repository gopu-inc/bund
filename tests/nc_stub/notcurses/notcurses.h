/* Faux Notcurses pour tester la TUI hors terminal : mêmes noms et signatures
   que l'API réelle pour les fonctions utilisées par BUND. Ne remplace pas un test
   sur un vrai terminal. */
#ifndef NC_STUB_H
#define NC_STUB_H
#include <stdint.h>
#include <stdio.h>

struct notcurses;
struct ncplane;
struct notcurses_options { const char *termtype; uint64_t flags; };
struct ncinput { uint32_t id; int y, x; char utf8[5]; };

#define NCOPTION_SUPPRESS_BANNERS 4
#define NCSTYLE_NONE 0
#define NCSTYLE_BOLD 1
#define PRETERUNICODEBASE 1115008u
#define NCKEY_RESIZE (PRETERUNICODEBASE + 2)
#define NCKEY_UP (PRETERUNICODEBASE + 3)
#define NCKEY_RIGHT (PRETERUNICODEBASE + 4)
#define NCKEY_DOWN (PRETERUNICODEBASE + 5)
#define NCKEY_LEFT (PRETERUNICODEBASE + 6)
#define NCKEY_HOME (PRETERUNICODEBASE + 20)
#define NCKEY_END (PRETERUNICODEBASE + 21)
#define NCKEY_DEL (PRETERUNICODEBASE + 22)
#define NCKEY_BACKSPACE (PRETERUNICODEBASE + 23)
#define NCKEY_ENTER (PRETERUNICODEBASE + 24)

struct notcurses *notcurses_core_init(const struct notcurses_options *, FILE *);
struct ncplane *notcurses_stdplane(struct notcurses *);
int notcurses_stop(struct notcurses *);
int notcurses_render(struct notcurses *);
uint32_t notcurses_getc_blocking(struct notcurses *, struct ncinput *);
void ncplane_dim_yx(const struct ncplane *, unsigned *rows, unsigned *cols);
void ncplane_erase(struct ncplane *);
int ncplane_putstr_yx(struct ncplane *, int y, int x, const char *);
int ncplane_set_fg_rgb8(struct ncplane *, unsigned r, unsigned g, unsigned b);
int ncplane_set_bg_rgb8(struct ncplane *, unsigned r, unsigned g, unsigned b);
void ncplane_set_fg_default(struct ncplane *);
void ncplane_set_bg_default(struct ncplane *);
void ncplane_set_styles(struct ncplane *, unsigned);

/* pilotage par le test */
void stub_set_keys(const char *script);
#endif
