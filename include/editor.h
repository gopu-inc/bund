#ifndef BUND_EDITOR_H
#define BUND_EDITOR_H

/* Logique d'édition de texte pure (sans Notcurses) : UTF-8, largeur d'affichage,
   retour à la ligne automatique, déplacement du curseur par ligne visuelle.
   Tout est testable hors terminal. */

#include <stddef.h>
#include <stdint.h>

typedef struct { size_t start, end; } ed_row;   /* octets [start,end), '\n' exclu */
typedef struct { ed_row *rows; size_t n, cap; } ed_layout;

/* Un caractère UTF-8 : renvoie sa largeur d'affichage (0, 1 ou 2) et sa taille en octets.
   Les octets invalides comptent pour 1 colonne et 1 octet. */
int ed_cwidth(const char *s, size_t remaining, size_t *bytes);
size_t ed_text_width(const char *s, size_t bytes);
size_t ed_encode(uint32_t cp, char out[5]);
size_t ed_prev(const char *s, size_t pos);
size_t ed_next(const char *s, size_t len, size_t pos);

/* Découpe `buf` en lignes visuelles de `width` colonnes au plus.
   Le curseur peut occuper une colonne de plus que `width` (fin de ligne pleine
   suivie d'un retour à la ligne) : l'appelant réserve donc width+1 colonnes. */
void ed_layout_build(ed_layout *L, const char *buf, size_t len, int width);
void ed_layout_free(ed_layout *L);

/* Variante pour l'affichage de texte (sorties, accueil) : coupe de préférence après un
   espace. Ne sert pas à la saisie, où le curseur exige une coupe exacte au caractère. */
void ed_layout_build_words(ed_layout *L, const char *buf, size_t len, int width);

/* Ligne et colonne visuelles du curseur placé à l'octet `pos`. */
void ed_locate(const ed_layout *L, const char *buf, size_t pos, size_t *row, size_t *col);

/* Monte (-1) ou descend (+1) d'une ligne visuelle en gardant la colonne. */
size_t ed_move_rows(const ed_layout *L, const char *buf, size_t pos, int dir);

/* Première ligne visible pour que le curseur reste dans une fenêtre de `max_rows`. */
size_t ed_first_visible(size_t cursor_row, size_t total_rows, size_t max_rows, size_t prev_first);

/* Modifications. Renvoient le nombre d'octets insérés / supprimés (0 si refusé). */
size_t ed_insert(char *buf, size_t cap, size_t *len, size_t *pos, const char *s, size_t n);
size_t ed_delete_back(char *buf, size_t *len, size_t *pos);
size_t ed_delete_fwd(char *buf, size_t *len, size_t pos);

#endif
