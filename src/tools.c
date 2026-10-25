#include "bund.h"

#include <unistd.h>
#include <sys/stat.h>

static const char *BLOCKED[] = {"sudo ", "rm -rf /", "mkfs", "shutdown", "reboot", ":(){", NULL};

/* Chemins relatifs uniquement, sans '..' ni dossier .git. */
static int safe_rel(const char *p) {
    if (!p || !*p || p[0] == '/' || strstr(p, "..")) return 0;
    if (strcmp(p, ".git") == 0 || strncmp(p, ".git/", 5) == 0) return 0;
    return 1;
}

static char *cap(char *s, size_t max) {
    if (strlen(s) <= max) return s;
    s[max] = '\0';
    sbuf b;
    sb_init(&b);
    sb_add(&b, s);
    sb_add(&b, "\n[... tronqué]");
    free(s);
    return sb_steal(&b);
}

static const char *detect_test_cmd(void) {
    if (file_exists("Makefile")) return "make test";
    if (file_exists("package.json")) return "npm test --silent";
    if (file_exists("go.mod")) return "go test ./...";
    if (file_exists("Cargo.toml")) return "cargo test";
    if (file_exists("pyproject.toml") || file_exists("setup.py") || file_exists("pytest.ini"))
        return "python3 -m pytest -q";
    return NULL;
}

static void ensure_parent(const char *path) {
    char *d = str_dup(path);
    char *s = strrchr(d, '/');
    if (s) {
        *s = '\0';
        mkdir_p(d);
    }
    free(d);
}

char *tools_execute(bund_config *c, const char *name, const char *arg,
                    const char *body) {
    int st = 0;

    if (!strcmp(name, "list_files")) {
        const char *dir = (arg && *arg) ? arg : ".";
        if (!safe_rel(dir)) return str_dup("refusé : dossier non autorisé");
        char *q = shell_quote(dir);
        sbuf cmd;
        sb_init(&cmd);
        sb_addf(&cmd, "find %s -path '*/.git' -prune -o -type f -print 2>/dev/null | head -300", q);
        free(q);
        char *r = run_capture(cmd.data, &st);
        sb_free(&cmd);
        return cap(r, 6000);
    }

    if (!strcmp(name, "read_file")) {
        if (!safe_rel(arg)) return str_dup("refusé : chemin non autorisé (absolu, '..' ou .git)");
        char *t = read_text_file(arg);
        if (!t) return str_dup("erreur : fichier introuvable");
        return cap(t, 8000);
    }

    if (!strcmp(name, "search")) {
        if (!arg || !*arg) return str_dup("erreur : texte de recherche manquant");
        char *q = shell_quote(arg);
        sbuf cmd;
        sb_init(&cmd);
        sb_addf(&cmd, "grep -rnI -F -- %s . --exclude-dir=.git 2>/dev/null | head -100", q);
        free(q);
        char *r = run_capture(cmd.data, &st);
        sb_free(&cmd);
        if (!*r) { free(r); return str_dup("aucun résultat"); }
        return cap(r, 6000);
    }

    if (!strcmp(name, "run_tests")) {
        const char *tc = detect_test_cmd();
        if (!tc) return str_dup("aucune commande de test détectée");
        sbuf cmd;
        sb_init(&cmd);
        sb_addf(&cmd, "if command -v timeout >/dev/null 2>&1; then timeout 180 %s; "
                      "else %s; fi 2>&1; echo \"[exit $?]\"", tc, tc);
        char *r = run_capture(cmd.data, &st);
        sb_free(&cmd);
        return cap(r, 6000);
    }

    if (!strcmp(name, "git_diff")) {
        char *r = run_capture("git --no-pager diff 2>&1 | head -400", &st);
        if (!*r) { free(r); return str_dup("aucune différence"); }
        return cap(r, 6000);
    }

    if (!strcmp(name, "env_get")) {
        char *value = config_agent_get(c, arg);
        if (!value) return str_dup("paramètre non autorisé ou inconnu");
        sbuf r;
        sb_init(&r);
        sb_addf(&r, "%s=%s", arg, value);
        free(value);
        return sb_steal(&r);
    }

    if (!strcmp(name, "env_set")) {
        if (!arg || !*arg) return str_dup("erreur : paramètre et valeur requis");
        char *kv = str_dup(arg);
        char *sp = strchr(kv, ' ');
        if (!sp) {
            free(kv);
            return str_dup("erreur : format attendu PARAMÈTRE VALEUR");
        }
        *sp = '\0';
        char *value = str_trim(sp + 1);
        sbuf q;
        sb_init(&q);
        if (str_ieq(kv, "AUTO_EDIT") || str_ieq(kv, "BUND_AUTO_EDIT")) {
            sb_addf(&q, "Modifier BUND_AUTO_EDIT = %s ? (true autorise les prochaines "
                         "modifications de fichiers sans confirmation)", value);
        } else {
            sb_addf(&q, "Modifier l'environnement BUND : %s = %s ?", kv, value);
        }
        int approved = confirm(q.data);
        sb_free(&q);
        if (!approved) {
            free(kv);
            return str_dup("modification refusée ou confirmation indisponible");
        }
        int saved = config_agent_set(c, kv, value);
        sbuf r;
        sb_init(&r);
        if (saved == 0) sb_addf(&r, "environnement mis à jour : %s", kv);
        else sb_add(&r, "modification refusée : paramètre non autorisé ou valeur invalide");
        free(kv);
        return sb_steal(&r);
    }

    if (!strcmp(name, "remember")) {
        if (!body) return str_dup("erreur : note manquante (texte puis END_NOTE)");
        int saved = memory_add_note(arg, body, 1);
        if (saved == 0) return str_dup("note enregistrée dans la mémoire locale");
        if (saved == 1) return str_dup("note refusée par l'utilisateur");
        return str_dup("note refusée : nom invalide, contenu trop long ou secret détecté");
    }

    if (!strcmp(name, "git_commit")) {
        if (!arg || !*arg) return str_dup("erreur : message de commit requis");
        char msg[220];
        snprintf(msg, sizeof msg, "%s", arg);
        for (char *p = msg; *p; p++)
            if (*p == '"' || *p == '\'' || *p == '\n' || *p == '\r' || *p == '`' || *p == '$') *p = ' ';
        char *status = run_capture("git status --short 2>&1 | head -30", &st);
        if (!*status) { free(status); return str_dup("rien à commiter"); }
        sbuf q;
        sb_init(&q);
        sb_addf(&q, "Commiter en tant que %s (git add -A) ?\nMessage : %s\n\n%s",
                c->git_name[0] ? c->git_name : "Bund", msg, status);
        free(status);
        int ok = c->auto_commit || confirm(q.data);
        sb_free(&q);
        if (!ok) return str_dup("refusé par l'utilisateur");
        char *o1 = NULL;
        git_run(c, "add -A", &o1);
        free(o1);
        /* Garde-fou : jamais de fichier secret dans un commit lancé par l'agent. */
        char *staged = run_capture("git diff --cached --name-only 2>/dev/null", &st);
        if (strstr(staged, ".env") || strstr(staged, "id_rsa") || strstr(staged, ".pem") ||
            strstr(staged, ".key") || strstr(staged, ".p12") || strstr(staged, "secrets/")) {
            char *o2 = NULL;
            git_run(c, "restore --staged .", &o2);
            free(o2);
            free(staged);
            return str_dup("refusé : un fichier secret (.env, clé, certificat) est dans l'index ; "
                           "l'index a été vidé. Ajoute-le au .gitignore.");
        }
        free(staged);
        sbuf a;
        sb_init(&a);
        sb_addf(&a, "commit -m \"%s\"", msg);
        char *o3 = NULL;
        int rc = git_run(c, a.data, &o3);
        sb_free(&a);
        sbuf r;
        sb_init(&r);
        sb_addf(&r, "%s[exit %d]", o3 ? o3 : "", rc);
        free(o3);
        return cap(sb_steal(&r), 3000);
    }

    if (!strcmp(name, "git_push")) {
        sbuf q;
        sb_init(&q);
        sb_add(&q, "Pousser vers GitHub (git push) avec ton compte connecté ?");
        int ok = c->auto_push || confirm(q.data);
        sb_free(&q);
        if (!ok) return str_dup("refusé par l'utilisateur");
        char *o = NULL;
        int rc = git_run(c, "push", &o);
        sbuf r;
        sb_init(&r);
        sb_addf(&r, "%s[exit %d]", o ? o : "", rc);
        free(o);
        return cap(sb_steal(&r), 3000);
    }

    if (!strcmp(name, "write_file")) {
        if (!safe_rel(arg)) return str_dup("refusé : chemin non autorisé");
        if (!body) return str_dup("erreur : contenu manquant (contenu puis END_FILE)");
        printf("\n--- aperçu de %s (%zu octets) ---\n%.*s\n---\n",
               arg, strlen(body), 600, body);
        sbuf q;
        sb_init(&q);
        sb_addf(&q, "Écrire %s ?", arg);
        int ok = c->auto_edit || confirm(q.data);
        sb_free(&q);
        if (!ok) return str_dup("refusé par l'utilisateur");
        ensure_parent(arg);
        if (write_text_file(arg, body, 0) != 0) return str_dup("erreur : écriture impossible");
        if (is_manifest(arg)) manifest_ensure_file(arg);
        sbuf r;
        sb_init(&r);
        sb_addf(&r, "écrit : %s (%zu octets)", arg, strlen(body));
        return sb_steal(&r);
    }

    if (!strcmp(name, "mcp_list")) return mcp_list_str();
    if (!strcmp(name, "mcp_tools")) {
        if (!arg || !*arg) return str_dup("usage : mcp_tools <serveur>");
        return cap(mcp_tools_str(arg), 6000);
    }
    if (!strcmp(name, "mcp_call")) {
        char buf[512];
        snprintf(buf, sizeof buf, "%s", arg ? arg : "");
        char *srv = str_trim(buf);
        char *tool = strchr(srv, ' ');
        if (!tool) return str_dup("usage : mcp_call <serveur> <outil> <json>");
        *tool++ = '\0';
        tool = str_trim(tool);
        char *args = strchr(tool, ' ');
        if (args) *args++ = '\0';
        const char *auto_mcp = getenv("BUND_AUTO_MCP");
        if (!(auto_mcp && (!strcmp(auto_mcp, "true") || !strcmp(auto_mcp, "1")))) {
            sbuf q;
            sb_init(&q);
            sb_addf(&q, "Appeler l'outil MCP %s/%s ?", srv, tool);
            int ok = confirm(q.data);
            sb_free(&q);
            if (!ok) return str_dup("refusé par l'utilisateur");
        }
        return cap(mcp_call_str(srv, tool, args ? str_trim(args) : ""), 6000);
    }
    if (!strcmp(name, "run")) {
        if (!arg || !*arg) return str_dup("erreur : commande vide");
        for (int i = 0; BLOCKED[i]; i++)
            if (strstr(arg, BLOCKED[i])) return str_dup("refusé : commande bloquée par la politique de sécurité");
        sbuf q;
        sb_init(&q);
        sb_addf(&q, "Exécuter la commande : %s", arg);
        int ok = confirm(q.data);
        sb_free(&q);
        if (!ok) return str_dup("refusé (confirmation impossible ou refusée)");
        sbuf cmd;
        sb_init(&cmd);
        sb_addf(&cmd, "( %s ) 2>&1; echo \"[exit $?]\"", arg);
        char *r = run_capture(cmd.data, &st);
        sb_free(&cmd);
        return cap(r, 6000);
    }

    sbuf r;
    sb_init(&r);
    sb_addf(&r, "action inconnue : %s", name);
    return sb_steal(&r);
}
