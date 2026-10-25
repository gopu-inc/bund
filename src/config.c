#include "bund.h"

#include <ctype.h>
#include <strings.h>

typedef struct {
    const char *id, *provider, *url, *model, *key_env, *mode, *desc;
} preset_t;

static const preset_t PRESETS[] = {
    {"openai", "OPENAI", "https://api.openai.com/v1", "gpt-4.1-mini",
     "OPENAI_API_KEY", "cloud", "OpenAI (clé payante)"},
    {"claude", "CLAUDE", "https://api.anthropic.com/v1", "claude-sonnet-4-5",
     "ANTHROPIC_API_KEY", "cloud", "Anthropic Claude (adaptateur natif)"},
    {"google", "GOOGLE", "https://generativelanguage.googleapis.com/v1beta/openai",
     "gemini-2.0-flash", "GEMINI_API_KEY", "cloud", "Google Gemini (endpoint OpenAI)"},
    {"deepseek", "DEEPSEEK", "https://api.deepseek.com/v1", "deepseek-chat",
     "DEEPSEEK_API_KEY", "cloud", "DeepSeek"},
    {"groq", "GROQ", "https://api.groq.com/openai/v1", "openai/gpt-oss-120b",
     "GROQ_API_KEY", "cloud", "Groq (modèle actuel recommandé)"},
    {"together", "TOGETHER", "https://api.together.xyz/v1", "Qwen/Qwen2.5-Coder-32B-Instruct",
     "TOGETHER_API_KEY", "cloud", "Together AI"},
    {"openrouter", "OPENROUTER", "https://openrouter.ai/api/v1", "openai/gpt-4.1-mini",
     "OPENROUTER_API_KEY", "cloud", "OpenRouter (multi-modèles)"},
    {"mistral", "MISTRAL", "https://api.mistral.ai/v1", "codestral-latest",
     "MISTRAL_API_KEY", "cloud", "Mistral AI"},
    {"ollama", "OLLAMA", "http://127.0.0.1:11434/v1", "qwen2.5-coder:1.5b",
     "", "local", "Ollama local (127.0.0.1 = cet appareil)"},
    {"compatible", "OPENAI_COMPATIBLE", "http://127.0.0.1:8000/v1", "mon-modele",
     "BUND_API_KEY", "cloud", "Tout serveur compatible OpenAI"},
};
#define NPRESETS (sizeof PRESETS / sizeof PRESETS[0])

static void apply_preset(bund_config *c, const preset_t *p) {
    snprintf(c->provider, sizeof c->provider, "%s", p->provider);
    snprintf(c->base_url, sizeof c->base_url, "%s", p->url);
    snprintf(c->model, sizeof c->model, "%s", p->model);
    snprintf(c->api_key_env, sizeof c->api_key_env, "%s", p->key_env);
    snprintf(c->mode, sizeof c->mode, "%s", p->mode);
}

void config_defaults(bund_config *c) {
    memset(c, 0, sizeof *c);
    apply_preset(c, &PRESETS[0]);
    snprintf(c->agent, sizeof c->agent, "%s", "bund");
    /* Public OAuth Client ID for the BUND GitHub Device Flow app. */
    snprintf(c->github_client_id, sizeof c->github_client_id, "%s", "Ov23ligJJGB452O8ITsP");
    snprintf(c->git_name, sizeof c->git_name, "%s", "Bund");
    c->max_steps = 100;
    c->max_retries = 2;
    c->timeout = 120;
    c->auto_test = 1;
}

static int is_true(const char *v) {
    return str_ieq(v, "true") || str_ieq(v, "1") || str_ieq(v, "yes") ||
           str_ieq(v, "on") || str_ieq(v, "oui") || str_ieq(v, "o");
}

static int apply_kv(bund_config *c, const char *k, const char *v) {
    if (!strcmp(k, "BUND_PROVIDER")) {
        snprintf(c->provider, sizeof c->provider, "%s", v);
        for (char *p = c->provider; *p; p++) *p = (char)toupper((unsigned char)*p);
    } else if (!strcmp(k, "BUND_API_BASE_URL")) {
        snprintf(c->base_url, sizeof c->base_url, "%s", v);
    } else if (!strcmp(k, "BUND_MODEL")) {
        snprintf(c->model, sizeof c->model, "%s", v);
    } else if (!strcmp(k, "BUND_API_KEY_ENV")) {
        snprintf(c->api_key_env, sizeof c->api_key_env, "%s", v);
    } else if (!strcmp(k, "BUND_MODE")) {
        snprintf(c->mode, sizeof c->mode, "%s", v);
    } else if (!strcmp(k, "BUND_AGENT")) {
        snprintf(c->agent, sizeof c->agent, "%s", v);
    } else if (!strcmp(k, "BUND_GITHUB_CLIENT_ID")) {
        snprintf(c->github_client_id, sizeof c->github_client_id, "%s", v);
    } else if (!strcmp(k, "BUND_GIT_NAME")) {
        if (*v) snprintf(c->git_name, sizeof c->git_name, "%s", v);
    } else if (!strcmp(k, "BUND_GIT_EMAIL")) {
        snprintf(c->git_email, sizeof c->git_email, "%s", v);
    } else if (!strcmp(k, "BUND_MAX_STEPS")) {
        c->max_steps = atoi(v) > 0 ? atoi(v) : 1;
    } else if (!strcmp(k, "BUND_MAX_RETRIES")) {
        c->max_retries = atoi(v) >= 0 ? atoi(v) : 0;
    } else if (!strcmp(k, "BUND_TIMEOUT_SECONDS")) {
        c->timeout = atoi(v) > 0 ? atoi(v) : 120;
    } else if (!strcmp(k, "BUND_AUTO_EDIT")) {
        c->auto_edit = is_true(v);
    } else if (!strcmp(k, "BUND_AUTO_TEST")) {
        c->auto_test = is_true(v);
    } else if (!strcmp(k, "BUND_AUTO_COMMIT")) {
        c->auto_commit = is_true(v);
    } else if (!strcmp(k, "BUND_AUTO_PUSH")) {
        c->auto_push = is_true(v);
    } else {
        return -1;
    }
    return 0;
}

int config_save(const bund_config *c) {
    sbuf b;
    sb_init(&b);
    sb_add(&b, "# BUND — configuration (modifiable avec /conf)\n");
    sb_addf(&b, "BUND_PROVIDER=%s\n", c->provider);
    sb_addf(&b, "BUND_API_BASE_URL=%s\n", c->base_url);
    sb_addf(&b, "BUND_MODEL=%s\n", c->model);
    sb_addf(&b, "BUND_API_KEY_ENV=%s\n", c->api_key_env);
    sb_addf(&b, "BUND_MODE=%s\n", c->mode);
    sb_addf(&b, "BUND_AGENT=%s\n", c->agent);
    sb_addf(&b, "BUND_GITHUB_CLIENT_ID=%s\n", c->github_client_id);
    sb_addf(&b, "BUND_GIT_NAME=%s\n", c->git_name);
    sb_addf(&b, "BUND_GIT_EMAIL=%s\n", c->git_email);
    sb_addf(&b, "BUND_MAX_STEPS=%d\n", c->max_steps);
    sb_addf(&b, "BUND_MAX_RETRIES=%d\n", c->max_retries);
    sb_addf(&b, "BUND_TIMEOUT_SECONDS=%d\n", c->timeout);
    sb_addf(&b, "BUND_AUTO_EDIT=%s\n", c->auto_edit ? "true" : "false");
    sb_addf(&b, "BUND_AUTO_TEST=%s\n", c->auto_test ? "true" : "false");
    sb_addf(&b, "BUND_AUTO_COMMIT=%s\n", c->auto_commit ? "true" : "false");
    sb_addf(&b, "BUND_AUTO_PUSH=%s\n", c->auto_push ? "true" : "false");

    char *p = bund_path("bund.env.conf");
    int r = write_text_file(p, b.data, 0);
    free(p);
    sb_free(&b);
    return r;
}

int config_load(bund_config *c) {
    config_defaults(c);
    char *path = bund_path("bund.env.conf");
    char *txt = read_text_file(path);
    if (!txt) {
        config_save(c);   /* premier lancement */
        free(path);
        return 0;
    }
    char *line = txt;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *t = str_trim(line);
        if (*t && *t != '#') {
            char *eq = strchr(t, '=');
            if (eq) {
                *eq = '\0';
                apply_kv(c, str_trim(t), str_trim(eq + 1));
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(txt);
    free(path);
    /* Keep the built-in public OAuth Client ID if an older config has it blank. */
    if (!c->github_client_id[0])
        snprintf(c->github_client_id, sizeof c->github_client_id, "%s", "Ov23ligJJGB452O8ITsP");
    return 0;
}

int config_set(bund_config *c, const char *key, const char *value) {
    char k[128];
    if (strncmp(key, "BUND_", 5) == 0) snprintf(k, sizeof k, "%s", key);
    else snprintf(k, sizeof k, "BUND_%s", key);
    for (char *p = k; *p; p++) *p = (char)toupper((unsigned char)*p);
    return apply_kv(c, k, value);
}

static int normalize_agent_key(const char *key, char *out, size_t outsz) {
    if (!key || !*key) return -1;
    if (strncasecmp(key, "BUND_", 5) == 0) snprintf(out, outsz, "%s", key);
    else snprintf(out, outsz, "BUND_%s", key);
    for (char *p = out; *p; p++) *p = (char)toupper((unsigned char)*p);
    static const char *allowed[] = {
        "BUND_PROVIDER", "BUND_API_BASE_URL", "BUND_MODEL", "BUND_MODE",
        "BUND_AGENT", "BUND_MAX_STEPS", "BUND_MAX_RETRIES",
        "BUND_TIMEOUT_SECONDS", "BUND_AUTO_EDIT", "BUND_AUTO_TEST", NULL
    };
    for (int i = 0; allowed[i]; i++)
        if (!strcmp(out, allowed[i])) return 0;
    return -1;
}

static int valid_agent_value(const char *key, const char *value) {
    if (!value || !*value || strlen(value) > 500 ||
        strpbrk(value, "\r\n") != NULL) return 0;
    if (!strcmp(key, "BUND_PROVIDER")) {
        static const char *providers[] = {
            "OPENAI", "GOOGLE", "CLAUDE", "DEEPSEEK", "GROQ",
            "OLLAMA", "OPENAI_COMPATIBLE", NULL
        };
        for (int i = 0; providers[i]; i++)
            if (str_ieq(value, providers[i])) return 1;
        return 0;
    }
    if (!strcmp(key, "BUND_API_BASE_URL")) {
        return (!strncmp(value, "https://", 8) || !strncmp(value, "http://", 7)) &&
               strpbrk(value, " \t") == NULL;
    }
    if (!strcmp(key, "BUND_MODE"))
        return str_ieq(value, "cloud") || str_ieq(value, "local");
    if (!strcmp(key, "BUND_MAX_STEPS")) {
        char *end = NULL;
        long n = strtol(value, &end, 10);
        if (end == value || *end) return 0;
        return n >= 1 && n <= 100;
    }
    if (!strcmp(key, "BUND_MAX_RETRIES")) {
        char *end = NULL;
        long n = strtol(value, &end, 10);
        if (end == value || *end) return 0;
        return n >= 0 && n <= 10;
    }
    if (!strcmp(key, "BUND_TIMEOUT_SECONDS")) {
        char *end = NULL;
        long n = strtol(value, &end, 10);
        if (end == value || *end) return 0;
        return n >= 1 && n <= 600;
    }
    if (!strcmp(key, "BUND_AUTO_EDIT") || !strcmp(key, "BUND_AUTO_TEST"))
        return is_true(value) || str_ieq(value, "false") || str_ieq(value, "0") ||
               str_ieq(value, "no") || str_ieq(value, "off") || str_ieq(value, "non");
    if (!strcmp(key, "BUND_MODEL"))
        return strpbrk(value, " \t") == NULL;
    if (!strcmp(key, "BUND_AGENT")) {
        static const char *agents[] = {
            "bund", "claude-code", "mistral-code", "codex", "copilot-cli", NULL
        };
        for (int i = 0; agents[i]; i++)
            if (str_ieq(value, agents[i])) return 1;
        return 0;
    }
    return 0;
}

char *config_agent_get(const bund_config *c, const char *key) {
    char k[128];
    if (normalize_agent_key(key, k, sizeof k) != 0) return NULL;
    const char *value = NULL;
    if (!strcmp(k, "BUND_PROVIDER")) value = c->provider;
    else if (!strcmp(k, "BUND_API_BASE_URL")) value = c->base_url;
    else if (!strcmp(k, "BUND_MODEL")) value = c->model;
    else if (!strcmp(k, "BUND_MODE")) value = c->mode;
    else if (!strcmp(k, "BUND_AGENT")) value = c->agent;
    else if (!strcmp(k, "BUND_MAX_STEPS")) {
        char *v = malloc(16);
        if (!v) return NULL;
        snprintf(v, 16, "%d", c->max_steps);
        return v;
    } else if (!strcmp(k, "BUND_MAX_RETRIES")) {
        char *v = malloc(16);
        if (!v) return NULL;
        snprintf(v, 16, "%d", c->max_retries);
        return v;
    } else if (!strcmp(k, "BUND_TIMEOUT_SECONDS")) {
        char *v = malloc(16);
        if (!v) return NULL;
        snprintf(v, 16, "%d", c->timeout);
        return v;
    } else if (!strcmp(k, "BUND_AUTO_EDIT")) value = c->auto_edit ? "true" : "false";
    else if (!strcmp(k, "BUND_AUTO_TEST")) value = c->auto_test ? "true" : "false";
    return value ? str_dup(value) : NULL;
}

int config_agent_set(bund_config *c, const char *key, const char *value) {
    char k[128];
    if (normalize_agent_key(key, k, sizeof k) != 0 ||
        !valid_agent_value(k, value)) return -1;
    bund_config previous = *c;
    if (config_set(c, k, value) != 0) return -1;
    if (config_save(c) != 0) {
        *c = previous;
        return -1;
    }
    return 0;
}

void env_cmd(bund_config *c, const char *arg) {
    char buf[768];
    snprintf(buf, sizeof buf, "%s", arg ? arg : "");
    char *a = str_trim(buf);
    if (!*a || !strcmp(a, "show") || !strcmp(a, "list")) {
        puts("Environnement de l'agent (aucune clé secrète affichée) :");
        printf("  provider=%s\n  mode=%s\n  base_url=%s\n  model=%s\n"
               "  agent=%s\n  max_steps=%d\n  max_retries=%d\n"
               "  timeout_seconds=%d\n  auto_edit=%s\n  auto_test=%s\n",
               c->provider, c->mode, c->base_url, c->model, c->agent,
               c->max_steps, c->max_retries, c->timeout,
               c->auto_edit ? "true" : "false", c->auto_test ? "true" : "false");
        puts("  Les clés API et les variables système ne sont pas modifiables par l'agent.");
        return;
    }
    if (!strncmp(a, "set ", 4)) {
        char *key = str_trim(a + 4);
        char *sp = strchr(key, ' ');
        if (!sp) {
            puts("usage : /env set <paramètre> <valeur>");
            return;
        }
        *sp = '\0';
        if (config_agent_set(c, key, str_trim(sp + 1)) != 0) {
            puts("Paramètre refusé ou valeur invalide. Valeurs autorisées : /env");
            return;
        }
        printf("Environnement mis à jour : %s\n", key);
        return;
    }
    if (!strcmp(a, "reset")) {
        if (!confirm("Réinitialiser la configuration BUND aux valeurs par défaut ?")) {
            puts("Réinitialisation annulée.");
            return;
        }
        config_defaults(c);
        if (config_save(c) == 0) puts("Configuration réinitialisée.");
        else puts("Erreur pendant l'enregistrement de la configuration.");
        return;
    }
    puts("usage : /env [show | set PARAMÈTRE VALEUR | reset]");
}

int config_preset(bund_config *c, const char *name) {
    for (size_t i = 0; i < NPRESETS; i++) {
        if (str_ieq(PRESETS[i].id, name)) {
            apply_preset(c, &PRESETS[i]);
            return 0;
        }
    }
    return -1;
}

void config_presets_print(void) {
    for (size_t i = 0; i < NPRESETS; i++)
        printf("  %-11s %-18s %s\n", PRESETS[i].id, PRESETS[i].provider, PRESETS[i].desc);
}

void config_print(const bund_config *c) {
    const char *key_state = "non requise";
    if (c->api_key_env[0]) {
        const char *e = getenv(c->api_key_env);
        sbuf p;
        sb_init(&p);
        sb_addf(&p, "secrets/%s", c->api_key_env);
        char *path = bund_path(p.data);
        sb_free(&p);
        if (e && *e) key_state = "présente (variable d'environnement)";
        else if (file_exists(path)) key_state = "présente (~/.bund/secrets)";
        else key_state = "ABSENTE";
        free(path);
    }
    printf("BUND_PROVIDER=%s\nBUND_API_BASE_URL=%s\nBUND_MODEL=%s\n"
           "BUND_API_KEY_ENV=%s   (clé : %s)\nBUND_MODE=%s\nBUND_AGENT=%s\n"
           "BUND_MAX_STEPS=%d\nBUND_MAX_RETRIES=%d\nBUND_TIMEOUT_SECONDS=%d\n"
           "BUND_AUTO_EDIT=%s\nBUND_AUTO_TEST=%s\nBUND_AUTO_COMMIT=%s\n"
           "BUND_AUTO_PUSH=%s\nBUND_GITHUB_CLIENT_ID=%s\nBUND_GIT_NAME=%s\nBUND_GIT_EMAIL=%s\n",
           c->provider, c->base_url, c->model, c->api_key_env, key_state,
           c->mode, c->agent, c->max_steps, c->max_retries, c->timeout,
           c->auto_edit ? "true" : "false", c->auto_test ? "true" : "false",
           c->auto_commit ? "true" : "false", c->auto_push ? "true" : "false",
           c->github_client_id[0] ? c->github_client_id : "(non défini)",
           c->git_name, c->git_email[0] ? c->git_email : "(auto : noreply du compte GitHub)");
}

static void ask(const char *label, char *out, size_t n, const char *def) {
    char line[256];
    printf("%s [%s] : ", label, def);
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin)) line[0] = '\0';
    char *t = str_trim(line);
    snprintf(out, n, "%s", *t ? t : def);
}

void config_wizard(bund_config *c) {
    char id[32], model[128], yn[8], defm[128];
    puts("Assistant de configuration BUND");
    config_presets_print();
    ask("Fournisseur (id)", id, sizeof id, "openai");
    if (config_preset(c, id) != 0) {
        puts("Fournisseur inconnu, configuration inchangée.");
        return;
    }
    snprintf(defm, sizeof defm, "%s", c->model);
    ask("Modèle", model, sizeof model, defm);
    snprintf(c->model, sizeof c->model, "%s", model);
    ask("Modifications sans confirmation (o/n)", yn, sizeof yn, c->auto_edit ? "o" : "n");
    c->auto_edit = (yn[0] == 'o' || yn[0] == 'O' || yn[0] == 'y' || yn[0] == 'Y');
    ask("Commit automatique (o/n)", yn, sizeof yn, c->auto_commit ? "o" : "n");
    c->auto_commit = (yn[0] == 'o' || yn[0] == 'O' || yn[0] == 'y' || yn[0] == 'Y');
    config_save(c);
    puts("Configuration enregistrée.");
}

void conf_cmd(bund_config *c, const char *arg) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", arg);
    char *a = str_trim(buf);
    if (!*a) { config_print(c); return; }
    if (!strcmp(a, "wizard")) { config_wizard(c); return; }
    if (!strcmp(a, "presets")) { config_presets_print(); return; }

    char *rest = a + strlen(a);
    char *sp = strchr(a, ' ');
    if (sp) {
        *sp = '\0';
        rest = str_trim(sp + 1);
    }
    if (!strcmp(a, "preset")) {
        if (config_preset(c, rest) != 0) { puts("preset inconnu (/conf presets)"); return; }
    } else if (!strcmp(a, "set")) {
        char *sp2 = strchr(rest, ' ');
        if (!sp2) { puts("usage : /conf set CLÉ VALEUR"); return; }
        *sp2 = '\0';
        if (config_set(c, str_trim(rest), str_trim(sp2 + 1)) != 0) {
            puts("clé inconnue (/conf pour la liste)");
            return;
        }
    } else {
        puts("usage : /conf [presets | preset NOM | set CLÉ VALEUR | wizard]");
        return;
    }
    config_save(c);
    puts("Configuration enregistrée.");
}
