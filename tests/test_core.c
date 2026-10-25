#include "bund.h"

#include <unistd.h>

static int fails = 0;

#define CHECK(cond, msg)                                  \
    do {                                                  \
        if (cond) printf("ok    : %s\n", msg);            \
        else { printf("FAIL  : %s\n", msg); fails++; }    \
    } while (0)

static int count_occ(const char *hay, const char *needle) {
    int n = 0;
    size_t l = strlen(needle);
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += l) n++;
    return n;
}

int main(void) {
    if (system("rm -rf /tmp/bund_test_home && mkdir -p /tmp/bund_test_home") != 0) return 1;
    setenv("BUND_HOME", "/tmp/bund_test_home", 1);
    CHECK(ensure_bund_dirs() == 0, "les répertoires locaux de BUND sont prêts");

    char *e = json_escape("a\"b\n\\");
    CHECK(strcmp(e, "a\\\"b\\n\\\\") == 0, "json_escape protège guillemets, retours et antislash");
    free(e);
    char *redacted = redact_secrets("api_key=sk-test-secret task-based");
    CHECK(redacted && strstr(redacted, "[REDACTED]") &&
          strstr(redacted, "task-based") && !strstr(redacted, "sk-test-secret"),
          "les journaux masquent les secrets courants sans altérer le texte normal");
    free(redacted);

    char *v = json_get_string("{\"type\":\"text\",\"text\":\"Bonjour \\u00e9 \\n ok\"}", "text");
    CHECK(v && strcmp(v, "Bonjour é \n ok") == 0, "json_get_string décode unicode et \\n");
    free(v);

    char *m = json_get_string("{\"error\":{\"message\":\"boom\"}}", "message");
    CHECK(m && strcmp(m, "boom") == 0, "json_get_string lit un message d'erreur imbriqué");
    free(m);

    CHECK(json_get_int("{\"interval\": 7}", "interval", 5) == 7, "json_get_int");

    bund_config cfg;
    config_defaults(&cfg);
    CHECK(config_agent_set(&cfg, "model", "test-coder") == 0,
          "l'agent peut modifier un paramètre autorisé");
    CHECK(config_agent_set(&cfg, "API_KEY_ENV", "LEAKED_KEY") != 0,
          "l'environnement agent ne peut pas modifier les références de clés API");
    char *model = config_agent_get(&cfg, "BUND_MODEL");
    CHECK(model && strcmp(model, "test-coder") == 0,
          "les paramètres d'environnement sont relus sans ambiguïté");
    free(model);
    CHECK(config_agent_set(&cfg, "MAX_STEPS", "1000") != 0,
          "les limites de l'agent restent bornées");
    CHECK(config_agent_set(&cfg, "MAX_STEPS", "5garbage") != 0,
          "les réglages numériques refusent les suffixes invalides");

    CHECK(memory_add_note("project", "Les changements doivent rester vérifiables.", 0) == 0,
          "une note mémoire locale est enregistrée");
    CHECK(memory_add_note("lessons", "Un agent task-based vérifie les résultats.", 0) == 0,
          "les formulations normales ne sont pas prises pour des secrets");
    CHECK(memory_add_note("project", "password=do-not-store", 0) != 0,
          "les notes contenant des motifs de secret sont refusées");
    char *mem = memory_context();
    CHECK(mem && strstr(mem, "changements doivent rester vérifiables"),
          "la mémoire locale est ajoutée au contexte de l'agent");
    free(mem);

    char *sid = session_current_id();
    CHECK(sid && strncmp(sid, "s_", 2) == 0, "une session persistante est créée");
    session_record("Demande", "Conserver ce contexte entre les tâches.");
    char *sctx = session_context();
    CHECK(sctx && strstr(sctx, "Conserver ce contexte entre les tâches"),
          "l'historique de session est récupérable");
    free(sid);
    free(sctx);

    char cwd[1024];
    if (getcwd(cwd, sizeof cwd) == NULL) return 1;
    if (system("rm -rf /tmp/bund_test && mkdir -p /tmp/bund_test") != 0) return 1;
    write_text_file("/tmp/bund_test/package.json", "{\n  \"name\": \"x\"\n}\n", 0);
    write_text_file("/tmp/bund_test/pyproject.toml", "[project]\nname = \"x\"\n", 0);
    if (chdir("/tmp/bund_test") != 0) return 1;

    manifest_ensure_dir(".");
    char *pj = read_text_file("package.json");
    CHECK(pj && strstr(pj, "\"name\": \"x\"") && strstr(pj, BUND_REPO_URL),
          "package.json reçoit le contributeur Bund");
    char *pp = read_text_file("pyproject.toml");
    CHECK(pp && strstr(pp, "# contributor: Bund <" BUND_REPO_URL ">"),
          "pyproject.toml reçoit le contributeur Bund");

    manifest_ensure_dir(".");
    char *pj2 = read_text_file("package.json");
    CHECK(pj2 && count_occ(pj2, BUND_REPO_URL) == 1,
          "contributeur ajouté une seule fois (idempotent)");

    /* Identité des commits et garde-fou anti-secrets (actions de l'agent) */
    {
        int st = 0;
        if (system("rm -rf /tmp/bund_gt /tmp/bund_gt_home && mkdir -p /tmp/bund_gt /tmp/bund_gt_home") != 0) return 1;
        setenv("BUND_HOME", "/tmp/bund_gt_home", 1);
        write_text_file("/tmp/bund_gt_home/github_user", "tester\n777\n", 0);
        if (chdir("/tmp/bund_gt") != 0) return 1;
        if (system("git init -q -b main && git config user.name Humain && git config user.email h@x.org") != 0) return 1;
        bund_config cfg;
        config_defaults(&cfg);
        cfg.auto_commit = 1;
        write_text_file("a.txt", "hi\n", 0);
        char *r1 = tools_execute(&cfg, "git_commit", "premier commit", NULL);
        char *who = run_capture("git log -1 --format='%an|%ae|%cn|%ce'", &st);
        CHECK(strstr(who, "Bund|777+tester@users.noreply.github.com|Bund|777+tester@users.noreply.github.com") != NULL,
              "commit de l'agent : auteur et committer = Bund, e-mail noreply du compte GitHub");
        char *g = run_capture("git config user.name", &st);
        CHECK(strstr(g, "Humain") != NULL, "la configuration git de l'utilisateur n'est pas modifiée");
        free(g);

        write_text_file(".env", "SECRET=1\n", 0);
        char *r2 = tools_execute(&cfg, "git_commit", "avec secret", NULL);
        char *staged = run_capture("git diff --cached --name-only", &st);
        char *count = run_capture("git rev-list --count HEAD", &st);
        CHECK(strstr(r2, "secret") != NULL && *staged == '\0' && atoi(count) == 1,
              "fichier .env : commit refusé et index vidé");
        free(r1); free(r2); free(who); free(staged); free(count);

        char *e = git_identity_email(&cfg);
        CHECK(strcmp(e, "777+tester@users.noreply.github.com") == 0, "e-mail noreply dérivé du compte mémorisé");
        free(e);
        snprintf(cfg.git_email, sizeof cfg.git_email, "%s", "moi@exemple.fr");
        e = git_identity_email(&cfg);
        CHECK(strcmp(e, "moi@exemple.fr") == 0, "BUND_GIT_EMAIL prime sur l'e-mail dérivé");
        free(e);
        if (chdir("/tmp/bund_test") != 0) return 1;
    }

    free(pj); free(pp); free(pj2);
    if (chdir(cwd) != 0) return 1;
    printf("\n%s\n", fails ? "ÉCHEC DES TESTS" : "Tous les tests passent.");
    return fails ? 1 : 0;
}
