#include "bund.h"

#include <unistd.h>

static char *read_api_key(const bund_config *c) {
    if (!c->api_key_env[0]) return NULL;
    const char *env = getenv(c->api_key_env);
    if (env && *env) return str_dup(env);
    sbuf p;
    sb_init(&p);
    sb_addf(&p, "secrets/%s", c->api_key_env);
    char *path = bund_path(p.data);
    sb_free(&p);
    char *v = read_text_file(path);
    free(path);
    if (!v) return NULL;
    char *t = str_trim(v);
    char *out = *t ? str_dup(t) : NULL;
    free(v);
    return out;
}

/* Appelle le modèle. Adaptateur natif Anthropic si CLAUDE, sinon endpoint
   compatible OpenAI (OpenAI, Groq, DeepSeek, Gemini, Ollama, serveurs locaux). */
char *llm_complete(const bund_config *c, const char *system, const char *user,
                   char *err, size_t errsz) {
    int anth = strcmp(c->provider, "CLAUDE") == 0;
    int local = strcmp(c->mode, "local") == 0;
    char *key = read_api_key(c);

    if (!key && c->api_key_env[0] && !local) {
        snprintf(err, errsz, "clé absente : exporte %s ou place-la dans ~/.bund/secrets/%s",
                 c->api_key_env, c->api_key_env);
        return NULL;
    }

    char *es = json_escape(system);
    char *eu = json_escape(user);
    char *em = json_escape(c->model);
    sbuf body;
    sb_init(&body);
    if (anth) {
        sb_addf(&body, "{\"model\":\"%s\",\"max_tokens\":4096,\"system\":\"%s\","
                       "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}", em, es, eu);
    } else {
        sb_addf(&body, "{\"model\":\"%s\",\"temperature\":0.2,\"messages\":["
                       "{\"role\":\"system\",\"content\":\"%s\"},"
                       "{\"role\":\"user\",\"content\":\"%s\"}]}", em, es, eu);
    }
    free(es);
    free(eu);
    free(em);

    sbuf h;
    sb_init(&h);
    sb_add(&h, "Content-Type: application/json\n");
    if (anth) sb_addf(&h, "x-api-key: %s\nanthropic-version: 2023-06-01\n", key ? key : "");
    else if (key) sb_addf(&h, "Authorization: Bearer %s\n", key);

    sbuf u;
    sb_init(&u);
    sb_add(&u, c->base_url);
    sb_add(&u, anth ? "/messages" : "/chat/completions");

    char *result = NULL;
    for (int attempt = 0; attempt <= c->max_retries; attempt++) {
        char *out = http_call("POST", u.data, body.data, h.data, c->timeout);
        if (strstr(out, "\"choices\"") || strstr(out, "\"content\"")) {
            result = anth ? json_get_string(out, "text") : json_get_string(out, "content");
            if (!result) snprintf(err, errsz, "%s", "réponse vide du modèle");
            free(out);
            break;
        }
        char *msg = json_get_string(out, "message");
        snprintf(err, errsz, "%s", msg ? msg : (*out ? out : "pas de réponse"));
        int json_error = msg != NULL || strstr(out, "\"error\"") != NULL;
        free(msg);
        free(out);
        if (json_error) break;
        if (attempt < c->max_retries) sleep(2);
    }

    free(key);
    sb_free(&body);
    sb_free(&h);
    sb_free(&u);
    return result;
}
