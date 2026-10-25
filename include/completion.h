#ifndef BUND_COMPLETION_H
#define BUND_COMPLETION_H

#include <stddef.h>

/* Suggestions d'autocomplétion (commandes /..., sous-commandes git, valeurs usuelles).
   Logique pure, sans terminal : la TUI ne fait que l'afficher. */

typedef struct {
    char insert[96];        /* texte qui remplace tout le contenu de la saisie */
    char label[64];         /* texte affiché dans la fenêtre */
    const char *desc;       /* description courte */
} comp_item;

/* Remplit `out` (au plus `max` éléments) et renvoie leur nombre. */
size_t comp_suggest(const char *buf, comp_item *out, size_t max);

/* Prompts proposés quand la saisie est vide (touche Tab). */
size_t comp_quick_count(void);
const char *comp_quick_label(size_t i);
const char *comp_quick_text(size_t i);

#endif
