#include "bund.h"

#include <ctype.h>
#include <unistd.h>

/* ---------- tokenisation ---------- */

static int tokenize(const char *s, char **out, int max) {
    int n = 0;
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        sbuf cur;
        sb_init(&cur);
        int inq = 0;
        char q = 0;
        while (*p && (inq || (*p != ' ' && *p != '\t'))) {
            if (inq) {
                if (*p == q) inq = 0;
                else sb_addc(&cur, *p);
            } else if (*p == '"' || *p == '\'') {
                inq = 1;
                q = *p;
            } else {
                sb_addc(&cur, *p);
            }
            p++;
        }
        if (n < max) out[n++] = sb_steal(&cur);
        else sb_free(&cur);
    }
    return n;
}

static void free_tokens(char **t, int n) {
    for (int i = 0; i < n; i++) free(t[i]);
}

/* ---------- GitHub : jeton ---------- */

static int save_token(const char *tok) {
    char *p = bund_path("secrets/github_token");
    int r = write_text_file(p, tok, 1);
    free(p);
    setenv("BUND_GH_TOKEN", tok, 1);
    return r;
}

int load_github_token(void) {
    char *p = bund_path("secrets/github_token");
    char *t = read_text_file(p);
    free(p);
    if (!t) return 0;
    char *s = str_trim(t);
    if (!*s) { free(t); return 0; }
    setenv("BUND_GH_TOKEN", s, 1);
    free(t);
    return 1;
}

static void cache_user(const char *login, long id) {
    char *p = bund_path("github_user");
    sbuf b;
    sb_init(&b);
    sb_addf(&b, "%s\n%ld\n", login, id);
    write_text_file(p, b.data, 0);
    sb_free(&b);
    free(p);
}

/* Lit le compte GitHub mémorisé lors de la dernière connexion (sans réseau). */
int github_cached_user(char *login, size_t lsz, long *id) {
    char *p = bund_path("github_user");
    char *t = read_text_file(p);
    free(p);
    if (!t) return 0;
    char *nl = strchr(t, '\n');
    int ok = 0;
    if (nl) {
        *nl = '\0';
        if (*t) {
            snprintf(login, lsz, "%s", t);
            if (id) *id = strtol(nl + 1, NULL, 10);
            ok = 1;
        }
    }
    free(t);
    return ok;
}

int whoami_github(void) {
    if (!load_github_token()) {
        puts("Non connecté à GitHub. Lance /login.");
        return 1;
    }
    sbuf h;
    sb_init(&h);
    sb_addf(&h, "Authorization: Bearer %s\nAccept: application/vnd.github+json\nUser-Agent: bund\n",
            getenv("BUND_GH_TOKEN"));
    char *r = http_call("GET", "https://api.github.com/user", NULL, h.data, 30);
    char *login = json_get_string(r, "login");
    long id = json_get_int(r, "id", 0);
    if (login) {
        printf("Connecté en tant que %s\n", login);
        cache_user(login, id);
    } else {
        puts("Token invalide ou réseau indisponible. Relance /login.");
    }
    free(login);
    free(r);
    sb_free(&h);
    return login ? 0 : 1;
}

int logout_github(void) {
    char *p = bund_path("secrets/github_token");
    unlink(p);
    free(p);
    p = bund_path("github_user");
    unlink(p);
    free(p);
    unsetenv("BUND_GH_TOKEN");
    puts("Déconnecté de GitHub.");
    return 0;
}

/* Connexion : gh CLI si disponible, sinon device flow OAuth de GitHub. */
int login_github(const bund_config *c) {
    int st = 0;
    char *gt = run_capture("command -v gh >/dev/null 2>&1 && gh auth token 2>/dev/null", &st);
    char *tk = str_trim(gt);
    if (st == 0 && *tk) {
        save_token(tk);
        free(gt);
        puts("Connecté via la CLI gh.");
        return whoami_github();
    }
    free(gt);

    const char *cid = c->github_client_id[0] ? c->github_client_id : getenv("BUND_GITHUB_CLIENT_ID");
    if (!cid || !*cid) {
        puts("Connexion GitHub : il faut un OAuth App avec Device Flow activé.\n"
             "  1. Crée-la sur https://github.com/settings/developers\n"
             "  2. Enregistre son Client ID : /conf set GITHUB_CLIENT_ID <id>\n"
             "  (ou installe la CLI gh et lance `gh auth login`)");
        return 1;
    }

    const char *hdr = "Accept: application/json\nContent-Type: application/x-www-form-urlencoded\n";
    sbuf body;
    sb_init(&body);
    sb_addf(&body, "client_id=%s&scope=repo", cid);
    char *resp = http_call("POST", "https://github.com/login/device/code", body.data, hdr, 30);
    char *dc = json_get_string(resp, "device_code");
    char *uc = json_get_string(resp, "user_code");
    char *vu = json_get_string(resp, "verification_uri");
    long interval = json_get_int(resp, "interval", 5);
    if (!dc || !uc) {
        printf("Échec du démarrage du login : %s\n", resp);
        goto fail;
    }
    printf("\n1. Ouvre cette URL dans ton navigateur : %s\n", vu ? vu : "https://github.com/login/device");
    printf("2. Entre ce code : %s\n\nEn attente de l'autorisation GitHub...\n", uc);
    fflush(stdout);
    tui_refresh();   /* dans la TUI, affiche l'URL et le code pendant l'attente */

    for (int i = 0; i < 120; i++) {
        sleep((unsigned)interval);
        sbuf pb;
        sb_init(&pb);
        sb_addf(&pb, "client_id=%s&device_code=%s&grant_type=urn:ietf:params:oauth:grant-type:device_code",
                cid, dc);
        char *pr = http_call("POST", "https://github.com/login/oauth/access_token", pb.data, hdr, 30);
        sb_free(&pb);
        char *at = json_get_string(pr, "access_token");
        if (at) {
            save_token(at);
            free(at);
            free(pr);
            puts("Connecté à GitHub.");
            free(dc); free(uc); free(vu); free(resp); sb_free(&body);
            return whoami_github();
        }
        char *er = json_get_string(pr, "error");
        int pending = er && !strcmp(er, "authorization_pending");
        int slow = er && !strcmp(er, "slow_down");
        free(pr);
        if (slow) interval += 5;
        else if (!pending) {
            printf("Connexion refusée ou expirée (%s).\n", er ? er : "réponse inattendue");
            free(er);
            goto fail;
        }
        free(er);
    }
    puts("Délai dépassé. Relance /login.");
fail:
    free(dc); free(uc); free(vu); free(resp); sb_free(&body);
    return 1;
}

/* ---------- git ---------- */

static void stage_manifests(const char *dir);

static const char *ALLOWED[] = {"status", "diff", "add", "commit", "push", "pull", "fetch",
                                "log", "branch", "checkout", "switch", "remote", "init",
                                "clone", "show", "stash", "restore", "tag", NULL};

int git_is_subcommand(const char *name) {
    char first[32];
    size_t i = 0;
    while (name[i] && name[i] != ' ' && i < sizeof first - 1) { first[i] = name[i]; i++; }
    first[i] = '\0';
    for (int k = 0; ALLOWED[k]; k++)
        if (!strcmp(first, ALLOWED[k])) return 1;
    return 0;
}

/* Identité des commits faits par BUND : « Bund » + e-mail noreply du compte
   connecté, pour que GitHub rattache le commit au bon compte. */
char *git_identity_email(const bund_config *c) {
    if (c->git_email[0]) return str_dup(c->git_email);
    char login[128];
    long id = 0;
    if (github_cached_user(login, sizeof login, &id)) {
        sbuf b;
        sb_init(&b);
        if (id > 0) sb_addf(&b, "%ld+%s@users.noreply.github.com", id, login);
        else sb_addf(&b, "%s@users.noreply.github.com", login);
        return sb_steal(&b);
    }
    /* Non connecté : adresse neutre qui ne correspond à aucun compte GitHub. */
    return str_dup("bund-agent@localhost");
}

static int has_upstream(void) {
    int st = 0;
    char *o = run_capture("git rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>&1", &st);
    free(o);
    return st == 0;
}

static int has_origin(void) {
    int st = 0;
    char *o = run_capture("git remote get-url origin 2>&1", &st);
    free(o);
    return st == 0;
}

/* Exécute git et renvoie le code de sortie ; *out reçoit la sortie (à libérer). */
int git_run(const bund_config *c, const char *args, char **out_text) {
    char *tok[64];
    int n = tokenize(args, tok, 64);
    *out_text = NULL;
    if (n == 0) {
        *out_text = str_dup("usage : /git <sous-commande> [options]   (ex : /git status, /git push)\n");
        return 1;
    }
    int ok = 0;
    for (int i = 0; ALLOWED[i]; i++)
        if (!strcmp(tok[0], ALLOWED[i])) { ok = 1; break; }
    if (!ok) {
        sbuf m;
        sb_init(&m);
        sb_addf(&m, "sous-commande non autorisée : %s\n", tok[0]);
        *out_text = sb_steal(&m);
        free_tokens(tok, n);
        return 1;
    }

    /* `git push` seul sur une branche sans upstream : pousse vers origin et mémorise. */
    if (n == 1 && !strcmp(tok[0], "push") && !has_upstream()) {
        if (!has_origin()) {
            *out_text = str_dup("aucun remote 'origin'. Ajoute-le : /git remote add origin <url>\n");
            free_tokens(tok, n);
            return 1;
        }
        free_tokens(tok, n);
        n = tokenize("push -u origin HEAD", tok, 64);
    }

    int net = !strcmp(tok[0], "push") || !strcmp(tok[0], "pull") ||
              !strcmp(tok[0], "fetch") || !strcmp(tok[0], "clone");
    int is_commit = !strcmp(tok[0], "commit");
    if (is_commit) {
        manifest_ensure_dir(".");
        stage_manifests(".");
    }

    sbuf cmd;
    sb_init(&cmd);
    if (is_commit) {
        /* Auteur et committer = Bund, sans toucher à la configuration git globale. */
        char *email = git_identity_email(c);
        char *qn = shell_quote(c->git_name[0] ? c->git_name : "Bund");
        char *qe = shell_quote(email);
        sb_addf(&cmd, "GIT_AUTHOR_NAME=%s GIT_AUTHOR_EMAIL=%s GIT_COMMITTER_NAME=%s "
                      "GIT_COMMITTER_EMAIL=%s ", qn, qe, qn, qe);
        free(qn);
        free(qe);
        free(email);
    }
    if (net) sb_add(&cmd, "GIT_TERMINAL_PROMPT=0 ");   /* jamais d'invite qui bloquerait la TUI */
    sb_add(&cmd, "git");
    if (net) {
        /* Le jeton reste dans l'environnement : jamais sur la ligne de commande. */
        load_github_token();
        sb_add(&cmd, " -c credential.helper= -c ");
        char *h = shell_quote("credential.helper=!f() { echo username=x-access-token; "
                              "echo \"password=$BUND_GH_TOKEN\"; }; f");
        sb_add(&cmd, h);
        free(h);
    }
    for (int i = 0; i < n; i++) {
        char *q = shell_quote(tok[i]);
        sb_addc(&cmd, ' ');
        sb_add(&cmd, q);
        free(q);
    }
    sb_add(&cmd, " 2>&1");

    int st = 0;
    char *out = run_capture(cmd.data, &st);
    if (net && st != 0 && (strstr(out, "Authentication") || strstr(out, "could not read Username") ||
                           strstr(out, "403") || strstr(out, "401"))) {
        sbuf m;
        sb_init(&m);
        sb_add(&m, out);
        sb_add(&m, "[authentification GitHub refusée : lance /login]\n");
        free(out);
        out = sb_steal(&m);
    }
    *out_text = out;
    sb_free(&cmd);
    free_tokens(tok, n);
    return st;
}

int git_command(const bund_config *c, const char *args) {
    char *out = NULL;
    int st = git_run(c, args, &out);
    printf("%s", out ? out : "");
    if (st != 0 && out && !strstr(out, "usage :") && !strstr(out, "non autorisée"))
        printf("[git : code de sortie %d]\n", st);
    free(out);
    const char *a = args;
    while (*a == ' ') a++;
    if (!strncmp(a, "commit", 6) && st == 0 && c->auto_push) return git_command(c, "push");
    return st;
}

/* ---------- manifestes : contributeur Bund ---------- */

static const char *MANIFESTS[] = {"pyproject.toml", "setup.py", "setup.cfg",
                                  "package.json", "Cargo.toml", "go.mod", NULL};

static const char *basename_of(const char *path) {
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

int is_manifest(const char *path) {
    const char *b = basename_of(path);
    for (int i = 0; MANIFESTS[i]; i++)
        if (!strcmp(b, MANIFESTS[i])) return 1;
    return 0;
}

int manifest_ensure_file(const char *path) {
    char *txt = read_text_file(path);
    if (!txt) return 0;
    if (strstr(txt, BUND_REPO_URL)) { free(txt); return 0; }

    sbuf out;
    sb_init(&out);
    if (!strcmp(basename_of(path), "package.json")) {
        char *last = strrchr(txt, '}');
        if (!last) { free(txt); sb_free(&out); return 0; }
        const char *prev = last - 1;
        while (prev > txt && isspace((unsigned char)*prev)) prev--;
        int empty = (*prev == '{');
        sb_addn(&out, txt, (size_t)(prev - txt) + 1);
        sb_addf(&out, "%s\n  \"contributors\": [\"%s\"]\n}", empty ? "" : ",", BUND_CONTRIB);
        sb_add(&out, last + 1);
    } else {
        sb_add(&out, txt);
        const char *pre = !strcmp(basename_of(path), "go.mod") ? "// " : "# ";
        if (out.len && out.data[out.len - 1] != '\n') sb_addc(&out, '\n');
        sb_addf(&out, "%scontributor: %s\n", pre, BUND_CONTRIB);
    }
    free(txt);
    int r = write_text_file(path, out.data, 0);
    sb_free(&out);
    if (r == 0) printf("manifeste mis à jour : %s\n", path);
    return r == 0;
}

int manifest_ensure_dir(const char *dir) {
    int n = 0;
    for (int i = 0; MANIFESTS[i]; i++) {
        sbuf p;
        sb_init(&p);
        sb_addf(&p, "%s/%s", dir, MANIFESTS[i]);
        if (file_exists(p.data)) n += manifest_ensure_file(p.data);
        sb_free(&p);
    }
    if (n == 0) puts("Aucun manifeste à mettre à jour (ou contributeur déjà présent).");
    return n;
}

static void stage_manifests(const char *dir) {
    for (int i = 0; MANIFESTS[i]; i++) {
        sbuf p;
        sb_init(&p);
        sb_addf(&p, "%s/%s", dir, MANIFESTS[i]);
        if (file_exists(p.data)) {
            sbuf cmd;
            sb_init(&cmd);
            char *q = shell_quote(p.data);
            sb_addf(&cmd, "git add -- %s 2>/dev/null", q);
            int st = 0;
            char *o = run_capture(cmd.data, &st);
            free(o);
            free(q);
            sb_free(&cmd);
        }
        sb_free(&p);
    }
}
