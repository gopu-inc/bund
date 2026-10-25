#include "editor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg)                                \
    do {                                                \
        if (cond) printf("ok    : %s\n", msg);          \
        else { printf("FAIL  : %s\n", msg); fails++; }  \
    } while (0)

static void where(const char *s, int width, size_t pos, size_t *nrows, size_t *r, size_t *c) {
    ed_layout L;
    ed_layout_build(&L, s, strlen(s), width);
    ed_locate(&L, s, pos, r, c);
    *nrows = L.n;
    ed_layout_free(&L);
}

/* Invariants : les lignes couvrent le texte, aucune ne dépasse la largeur,
   le curseur reste dans [0, width]. */
static int check_invariants(const char *buf, size_t len, size_t pos, int width) {
    ed_layout L;
    ed_layout_build(&L, buf, len, width);
    int ok = L.n > 0;
    for (size_t i = 0; ok && i < L.n; i++) {
        if (L.rows[i].end < L.rows[i].start || L.rows[i].end > len) ok = 0;
        if (ed_text_width(buf + L.rows[i].start, L.rows[i].end - L.rows[i].start) > (size_t)width) ok = 0;
        if (i + 1 < L.n && L.rows[i + 1].start < L.rows[i].end) ok = 0;
    }
    size_t r, c;
    ed_locate(&L, buf, pos, &r, &c);
    if (r >= L.n || c > (size_t)width) ok = 0;
    ed_layout_free(&L);
    return ok;
}

int main(void) {
    size_t n, r, c;

    where("abcdefghij", 10, 10, &n, &r, &c);
    CHECK(n == 2 && r == 1 && c == 0, "ligne exactement pleine : le curseur passe à la ligne suivante");

    where("abcdefghijk", 10, 11, &n, &r, &c);
    CHECK(n == 2 && r == 1 && c == 1, "un caractère de plus que la largeur : retour à la ligne");

    where("abcdefghij\n", 10, 10, &n, &r, &c);
    CHECK(r == 0 && c == 10, "ligne pleine + Entrée : curseur dans la colonne réservée");
    where("abcdefghij\n", 10, 11, &n, &r, &c);
    CHECK(n == 2 && r == 1 && c == 0, "après l'Entrée : début de la ligne suivante");

    where("", 10, 0, &n, &r, &c);
    CHECK(n == 1 && r == 0 && c == 0, "texte vide : une ligne, curseur en 0,0");

    const char *cjk = "日本語";   /* 3 caractères larges, 6 colonnes */
    where(cjk, 5, 9, &n, &r, &c);
    CHECK(n == 2 && r == 1 && c == 2, "caractères larges : jamais coupés au milieu");

    const char *txt = "abcdefghijklmnopqrstuvwxyz";
    ed_layout L;
    ed_layout_build(&L, txt, strlen(txt), 10);
    size_t p = ed_move_rows(&L, txt, 13, -1);
    CHECK(p == 3, "flèche haut : même colonne sur la ligne du dessus");
    p = ed_move_rows(&L, txt, 3, +1);
    CHECK(p == 13, "flèche bas : même colonne sur la ligne du dessous");
    CHECK(ed_move_rows(&L, txt, 2, -1) == 2, "flèche haut sur la première ligne : inchangé");
    CHECK(ed_move_rows(&L, txt, 25, +1) == 25, "flèche bas sur la dernière ligne : inchangé");
    ed_layout_free(&L);

    char buf[64] = "";
    size_t len = 0, pos = 0;
    CHECK(ed_insert(buf, sizeof buf, &len, &pos, "é", 2) == 2 && len == 2, "insertion d'un caractère UTF-8");
    CHECK(ed_delete_back(buf, &len, &pos) == 2 && len == 0 && pos == 0, "Retour arrière supprime le caractère entier");

    char small[8] = "";
    len = pos = 0;
    int accepted = 0;
    for (int i = 0; i < 20; i++) accepted += ed_insert(small, sizeof small, &len, &pos, "x", 1) ? 1 : 0;
    CHECK(accepted == 7 && len == 7 && small[7] == '\0', "tampon plein : refus propre, pas de débordement");

    CHECK(ed_first_visible(7, 8, 3, 0) == 5, "fenêtre verticale : suit le curseur vers le bas");
    CHECK(ed_first_visible(1, 8, 3, 5) == 1, "fenêtre verticale : suit le curseur vers le haut");
    CHECK(ed_first_visible(0, 2, 3, 4) == 0, "fenêtre verticale : tout tient, début à 0");

    /* Saisie caractère par caractère à travers la limite de ligne, plusieurs largeurs. */
    int widths[] = {1, 2, 5, 17, 40, 79};
    int ok = 1;
    for (size_t wi = 0; wi < sizeof widths / sizeof widths[0]; wi++) {
        char b[4096] = "";
        size_t l = 0, ps = 0;
        for (int i = 0; i < 300 && ok; i++) {
            const char *piece = (i % 23 == 22) ? "\n" : (i % 11 == 10 && widths[wi] >= 2) ? "日" : (i % 7 == 6) ? "é" : "a";
            ed_insert(b, sizeof b, &l, &ps, piece, strlen(piece));
            if (!check_invariants(b, l, ps, widths[wi])) ok = 0;
        }
    }
    CHECK(ok, "300 frappes mixtes (ASCII, accents, large, Entrée) traversent la limite sans incohérence");

    /* Fuzz : insertions, suppressions et déplacements aléatoires. */
    srand(12345);
    ok = 1;
    for (int round = 0; round < 40 && ok; round++) {
        int width = 2 + rand() % 59;   /* un caractère large exige au moins 2 colonnes */
        char b[2048] = "";
        size_t l = 0, ps = 0;
        for (int step = 0; step < 400 && ok; step++) {
            int op = rand() % 8;
            if (op < 4) {
                static const char *pieces[] = {"a", "bc", " ", "é", "日", "\n", "xyz"};
                const char *pc = pieces[rand() % 7];
                ed_insert(b, sizeof b, &l, &ps, pc, strlen(pc));
            } else if (op == 4) ed_delete_back(b, &l, &ps);
            else if (op == 5) ed_delete_fwd(b, &l, ps);
            else {
                ed_layout La;
                ed_layout_build(&La, b, l, width);
                ps = ed_move_rows(&La, b, ps, op == 6 ? -1 : 1);
                ed_layout_free(&La);
            }
            if (ps > l || !check_invariants(b, l, ps, width)) ok = 0;
        }
    }
    CHECK(ok, "fuzz 40x400 opérations : invariants respectés");

    {
        const char *t = "Dites-moi ce que vous voulez faire maintenant";
        ed_layout W;
        ed_layout_build_words(&W, t, strlen(t), 20);
        int good = W.n >= 2;
        for (size_t i = 0; good && i < W.n; i++) {
            size_t w = ed_text_width(t + W.rows[i].start, W.rows[i].end - W.rows[i].start);
            if (w > 20) good = 0;
            if (W.rows[i].end > W.rows[i].start && t[W.rows[i].end - 1] == ' ') good = 0;   /* pas d'espace final */
            if (i > 0 && (t[W.rows[i].start] == ' ')) good = 0;                              /* pas d'espace initial */
        }
        CHECK(good, "retour à la ligne par mots : largeur respectée, pas d'espace en bout de ligne");
        /* aucun mot coupé : chaque ligne finit juste avant un espace ou à la fin du texte */
        int whole = 1;
        for (size_t i = 0; i + 1 < W.n; i++) {
            char nx = t[W.rows[i].end];
            if (nx != ' ') whole = 0;
        }
        CHECK(whole, "aucun mot coupé quand il tient sur une ligne");
        ed_layout_free(&W);
        const char *longw = "abcdefghijklmnopqrstuvwxyz";
        ed_layout_build_words(&W, longw, strlen(longw), 10);
        CHECK(W.n == 3, "un mot plus long que la ligne est coupé au caractère");
        ed_layout_free(&W);
        ed_layout_build_words(&W, "a\n\nb", 4, 10);
        CHECK(W.n == 3, "lignes vides conservées");
        ed_layout_free(&W);
    }

    printf("\n%s\n", fails ? "ÉCHEC DES TESTS ÉDITEUR" : "Tests éditeur : tout passe.");
    return fails ? 1 : 0;
}
