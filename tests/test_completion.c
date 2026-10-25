#include "completion.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg)                                \
    do {                                                \
        if (cond) printf("ok    : %s\n", msg);          \
        else { printf("FAIL  : %s\n", msg); fails++; }  \
    } while (0)

int main(void) {
    comp_item it[16];
    size_t n = comp_suggest("git ", it, 16);
    CHECK(n >= 5 && !strcmp(it[0].insert, "git add .") && !strcmp(it[0].desc, "Ajouter tous les fichiers"),
          "« git  » propose git add . en premier, avec sa description");
    n = comp_suggest("git co", it, 16);
    CHECK(n == 1 && !strcmp(it[0].insert, "git commit -m "), "« git co » → git commit -m");
    n = comp_suggest("/git pus", it, 16);
    CHECK(n == 1 && !strcmp(it[0].insert, "/git push"), "le préfixe /git est conservé");
    n = comp_suggest("/git pu", it, 16);
    CHECK(n == 2, "« pu » propose push et pull");
    n = comp_suggest("git push", it, 16);
    CHECK(n == 0, "commande déjà complète : pas de fenêtre");
    n = comp_suggest("/co", it, 16);
    CHECK(n == 1 && !strcmp(it[0].insert, "/conf"), "« /co » → /conf");
    n = comp_suggest("/", it, 16);
    CHECK(n >= 10, "« / » liste toutes les commandes");
    n = comp_suggest("/conf preset gr", it, 16);
    CHECK(n == 1 && !strcmp(it[0].insert, "/conf preset groq"), "/conf preset gr → groq");
    n = comp_suggest("/agent use co", it, 16);
    CHECK(n == 2, "/agent use co → codex et copilot-cli");
    n = comp_suggest("corrige le bug", it, 16);
    CHECK(n == 0, "une phrase normale ne déclenche aucune fenêtre");
    n = comp_suggest("git add .\nfoo", it, 16);
    CHECK(n == 0, "pas de fenêtre en saisie multiligne");
    n = comp_suggest("/", it, 3);
    CHECK(n == 3, "le nombre d'éléments est borné");
    CHECK(comp_quick_count() == 4 && comp_quick_text(5) == comp_quick_text(1), "prompts rapides cycliques");
    printf("\n%s\n", fails ? "ÉCHEC DES TESTS COMPLETION" : "Tests complétion : tout passe.");
    return fails ? 1 : 0;
}
