#include "completion.h"

#include <stdio.h>
#include <string.h>

typedef struct { const char *name, *desc; } entry;

static const entry SLASH[] = {
    {"/help", "Aide et commandes"},       {"/git", "Commandes git (add, commit, push...)"},
    {"/login", "Connexion GitHub"},       {"/whoami", "Compte GitHub connecté"},
    {"/logout", "Déconnexion GitHub"},    {"/conf", "Configuration de BUND"},
    {"/agent", "Profil d'agent"},         {"/run", "Lancer l'agent sur un objectif"},
    {"/multi", "Saisie multiligne"},      {"/session", "Sessions persistantes"},
    {"/memory", "Mémoire du projet"},     {"/skills", "Skills disponibles"},   {"/mcp", "Serveurs MCP"},
    {"/job", "Historique des tâches"},    {"/env", "Environnement de l'agent"},
    {"/manifest", "Ajouter le contributeur Bund"},
    {"/clear", "Effacer la sortie"},      {"/exit", "Quitter"},
};

static const entry GIT[] = {
    {"add .", "Ajouter tous les fichiers"},
    {"commit -m ", "Message de commit"},
    {"push", "Pousser vers le remote"},
    {"status", "Voir le statut"},
    {"log --oneline", "Historique des commits"},
    {"diff", "Voir les modifications"},
    {"pull", "Récupérer depuis le remote"},
    {"branch", "Lister les branches"},
    {"fetch", "Mettre à jour les références"},
    {"checkout ", "Changer de branche"},
};

static const entry PRESETS[] = {
    {"openai", "OpenAI"}, {"together", "Together AI"}, {"openrouter", "OpenRouter"}, {"mistral", "Mistral AI"}, {"claude", "Anthropic Claude"}, {"google", "Google Gemini"},
    {"deepseek", "DeepSeek"}, {"groq", "Groq (quota gratuit)"}, {"ollama", "Modèle local"},
    {"compatible", "API compatible OpenAI"},
};

static const entry SKILLS_SUB[] = {
    {"add ", "Installer depuis une URL (bund.skills)"}, {"remove ", "Supprimer un skill"},
    {"show ", "Afficher un skill"},
};

static const entry MCP_SUB[] = {
    {"list", "Serveurs enregistrés"}, {"browse", "Catalogue de serveurs connus"},
    {"add ", "Ajouter NOM [URL]"}, {"remove ", "Retirer un serveur"},
    {"auth ", "Enregistrer un jeton (NOM JETON)"}, {"tools ", "Lister les outils d'un serveur"}, {"call ", "Appeler un outil"},
};

static const entry AGENTS[] = {
    {"bund", "Agent par défaut"}, {"claude-code", "Style prudent"}, {"mistral-code", "Style rapide"},
    {"codex", "Boucle étape par étape"}, {"copilot-cli", "Style terminal"},
};

static const char *QUICK_LABEL[] = {"Analyser le projet", "Exécuter les tests", "Corriger un bug",
                                    "Préparer un commit"};
static const char *QUICK_TEXT[] = {
    "Analyse ce projet : structure, langages, points fragiles, et propose des améliorations.",
    "Lance les tests du projet et explique les échecs éventuels.",
    "Trouve la cause du bug suivant et corrige-le : ",
    "Résume mes modifications et prépare un message de commit.",
};

static size_t add(comp_item *out, size_t n, size_t max, const char *prefix, const char *name,
                  const char *desc) {
    if (n >= max) return n;
    snprintf(out[n].insert, sizeof out[n].insert, "%s%s", prefix, name);
    snprintf(out[n].label, sizeof out[n].label, "%s%s", prefix, name);
    out[n].desc = desc;
    return n + 1;
}

static size_t from_table(comp_item *out, size_t max, const entry *t, size_t cnt,
                         const char *prefix, const char *typed) {
    size_t n = 0, tl = strlen(typed);
    for (size_t i = 0; i < cnt; i++)
        if (strncmp(t[i].name, typed, tl) == 0) n = add(out, n, max, prefix, t[i].name, t[i].desc);
    return n;
}

size_t comp_suggest(const char *buf, comp_item *out, size_t max) {
    if (!buf || !*buf || max == 0) return 0;
    if (strchr(buf, '\n') || strlen(buf) > 60) return 0;

    /* /conf preset <x> et /agent use <x> */
    if (!strncmp(buf, "/conf preset ", 13))
        return from_table(out, max, PRESETS, sizeof PRESETS / sizeof PRESETS[0], "/conf preset ", buf + 13);
    if (!strncmp(buf, "/agent use ", 11))
        return from_table(out, max, AGENTS, sizeof AGENTS / sizeof AGENTS[0], "/agent use ", buf + 11);

    if (!strncmp(buf, "/skills ", 8))
        return from_table(out, max, SKILLS_SUB, sizeof SKILLS_SUB / sizeof SKILLS_SUB[0], "/skills ", buf + 8);
    if (!strncmp(buf, "/mcp ", 5))
        return from_table(out, max, MCP_SUB, sizeof MCP_SUB / sizeof MCP_SUB[0], "/mcp ", buf + 5);

    /* git ... ou /git ... */
    const char *prefix = NULL, *rest = NULL;
    if (!strncmp(buf, "/git ", 5)) { prefix = "/git "; rest = buf + 5; }
    else if (!strncmp(buf, "git ", 4)) { prefix = "git "; rest = buf + 4; }
    else if (!strcmp(buf, "git")) { prefix = "git "; rest = ""; }
    if (prefix) {
        size_t n = from_table(out, max, GIT, sizeof GIT / sizeof GIT[0], prefix, rest);
        if (n == 1 && !strcmp(out[0].insert, buf)) return 0;   /* déjà complet */
        return n;
    }

    /* /commande (sans espace) */
    if (buf[0] == '/' && !strchr(buf, ' ')) {
        size_t n = from_table(out, max, SLASH, sizeof SLASH / sizeof SLASH[0], "", buf);
        if (n == 1 && !strcmp(out[0].insert, buf)) return 0;
        return n;
    }
    return 0;
}

size_t comp_quick_count(void) { return sizeof QUICK_LABEL / sizeof QUICK_LABEL[0]; }
const char *comp_quick_label(size_t i) { return QUICK_LABEL[i % comp_quick_count()]; }
const char *comp_quick_text(size_t i) { return QUICK_TEXT[i % comp_quick_count()]; }
