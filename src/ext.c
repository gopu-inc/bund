#include "bund.h"

#include <ctype.h>
#include <dirent.h>
#include <strings.h>
#include <unistd.h>

/* ---------- validation ---------- */

static int safe_name(const char *s) {
    if (!s || !*s || strlen(s) > 48 || s[0] == '.') return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.')) return 0;
    return !strstr(s, "..");
}

static int safe_url(const char *s) {
    if (!s || strlen(s) > 300) return 0;
    if (strncmp(s, "https://", 8) != 0 && strncmp(s, "http://127.0.0.1", 16) != 0 &&
        strncmp(s, "http://localhost", 16) != 0) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (!(isalnum(*p) || strchr(":/._-~%?=&@+#", *p))) return 0;
    return 1;
}


/* « owner/repo » et « github.com/owner/repo » deviennent des URL https complètes. */
static void norm_repo_url(const char *in, char *out, size_t n) {
    if (!strncmp(in, "http://", 7) || !strncmp(in, "https://", 8)) snprintf(out, n, "%s", in);
    else if (!strncmp(in, "git@github.com:", 15)) snprintf(out, n, "https://github.com/%s", in + 15);
    else if (strchr(in, '.') && strchr(in, '/') && strchr(in, '.') < strchr(in, '/'))
        snprintf(out, n, "https://%s", in);                    /* github.com/x/y */
    else if (strchr(in, '/')) snprintf(out, n, "https://github.com/%s", in);   /* x/y */
    else snprintf(out, n, "%s", in);
    size_t l = strlen(out);
    while (l > 0 && out[l - 1] == '/') out[--l] = '\0';
}

static const char *git_hint(const char *err) {
    if (strstr(err, "Could not resolve host")) {
        if (strstr(err, "github.cm")) return "Hôte introuvable : « github.cm » → tu voulais sûrement github.com.";
        return "Hôte introuvable : vérifie l'adresse (faute de frappe ?) et ta connexion.";
    }
    if (strstr(err, "could not read Username") || strstr(err, "Repository not found") ||
        strstr(err, "not found"))
        return "Dépôt introuvable ou privé : vérifie le nom (owner/repo).";
    if (strstr(err, "timed out") || strstr(err, "Failed to connect"))
        return "Connexion impossible : réseau indisponible ou bloqué.";
    return NULL;
}

/* Lit "clé: valeur" dans un manifeste bund.skills (lignes # = commentaires). */
static char *manifest_get(const char *text, const char *key) {
    size_t kl = strlen(key);
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > kl && !strncmp(p, key, kl) && (p[kl] == ':' || p[kl] == '=')) {
            char *v = malloc(len - kl);
            memcpy(v, p + kl + 1, len - kl - 1);
            v[len - kl - 1] = '\0';
            char *t = str_trim(v);
            char *r = str_dup(t);
            free(v);
            return r;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return NULL;
}

/* ---------- /skills add | remove ---------- */

void skills_add(const char *arg) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", arg);
    char *name = str_trim(buf);
    char *url = strchr(name, ' ');
    if (!url) { puts("usage : /skills add NOM URL"); return; }
    *url++ = '\0';
    url = str_trim(url);
    char nurl[320];
    norm_repo_url(url, nurl, sizeof nurl);
    url = nurl;
    if (!safe_name(name)) { puts("nom de skill invalide (lettres, chiffres, - _ .)"); return; }
    if (!safe_url(url)) { puts("URL invalide (ex : /skills add NOM owner/repo ou https://github.com/owner/repo)"); return; }

    char *tmp = bund_path("tmp/skill_clone");
    mkdir_p(tmp);
    char *qt = shell_quote(tmp), *qu = shell_quote(url);
    sbuf cmd;
    sb_init(&cmd);
    sb_addf(&cmd, "rm -rf %s && GIT_TERMINAL_PROMPT=0 git clone --depth 1 --quiet %s %s 2>&1", qt, qu, qt);
    printf("Téléchargement de %s ...\n", url);
    int st = 0;
    char *out = run_capture(cmd.data, &st);
    if (st != 0) {
        const char *h = git_hint(out);
        if (h) printf("Échec du téléchargement. %s\n", h);
        else printf("Échec du téléchargement : %.300s\n", out);
        goto done;
    }
    free(out);
    out = NULL;

    sbuf mp;
    sb_init(&mp);
    sb_addf(&mp, "%s/bund.skills", tmp);
    char *manifest = read_text_file(mp.data);
    sb_free(&mp);
    if (!manifest) {
        sbuf sk;
        sb_init(&sk);
        sb_addf(&sk, "%s/SKILL.md", tmp);
        int has = file_exists(sk.data);
        sb_free(&sk);
        if (!has) { puts("Ni bund.skills ni SKILL.md à la racine du dépôt."); goto done; }
        puts("bund.skills absent : SKILL.md trouvé, installation avec les valeurs par défaut.");
        manifest = str_dup("");
    }

    char *mname = manifest_get(manifest, "name");
    char *desc = manifest_get(manifest, "description");
    char *entry = manifest_get(manifest, "entry");
    char *ver = manifest_get(manifest, "version");
    const char *e = entry ? entry : "SKILL.md";
    if (strstr(e, "..") || e[0] == '/') { puts("chemin 'entry' refusé"); goto cleanup; }

    sbuf ep;
    sb_init(&ep);
    sb_addf(&ep, "%s/%s", tmp, e);
    if (!file_exists(ep.data)) { printf("Fichier d'entrée introuvable : %s\n", e); sb_free(&ep); goto cleanup; }
    sb_free(&ep);

    printf("Skill : %s %s\n%s\n", mname ? mname : name, ver ? ver : "", desc ? desc : "");
    if (!confirm("Installer ce skill dans ~/.bund/skills ?")) { puts("Annulé."); goto cleanup; }

    char *root = bund_path("skills");
    sbuf dest;
    sb_init(&dest);
    sb_addf(&dest, "%s/%s", root, name);
    mkdir_p(root);
    char *qd = shell_quote(dest.data);
    sbuf cp;
    sb_init(&cp);
    sb_addf(&cp, "rm -rf %s && mkdir -p %s && cd %s && tar --exclude=.git -cf - . | (cd %s && tar -xf -) 2>&1",
            qd, qd, qt, qd);
    char *o2 = run_capture(cp.data, &st);
    if (st != 0) { printf("Échec de l'installation : %s\n", o2); }
    else {
        /* la skill est lue comme SKILL.md par l'agent */
        if (strcmp(e, "SKILL.md") != 0) {
            sbuf src, dst;
            sb_init(&src); sb_init(&dst);
            sb_addf(&src, "%s/%s", dest.data, e);
            sb_addf(&dst, "%s/SKILL.md", dest.data);
            char *t = read_text_file(src.data);
            if (t) { write_text_file(dst.data, t, 0); free(t); }
            sb_free(&src); sb_free(&dst);
        }
        printf("Skill '%s' installé : %s\n", name, dest.data);
    }
    free(o2); free(qd); free(root); sb_free(&dest); sb_free(&cp);
cleanup:
    free(manifest); free(mname); free(desc); free(entry); free(ver);
done:
    free(out);
    sbuf rm;
    sb_init(&rm);
    sb_addf(&rm, "rm -rf %s", qt);
    free(run_capture(rm.data, NULL));
    sb_free(&rm); sb_free(&cmd);
    free(qt); free(qu); free(tmp);
}

void skills_remove(const char *arg) {
    char buf[128];
    snprintf(buf, sizeof buf, "%s", arg);
    char *name = str_trim(buf);
    if (!safe_name(name)) { puts("nom de skill invalide"); return; }
    char *root = bund_path("skills");
    sbuf d;
    sb_init(&d);
    sb_addf(&d, "%s/%s", root, name);
    if (!file_exists(d.data)) puts("skill introuvable dans ~/.bund/skills");
    else if (confirm("Supprimer ce skill ?")) {
        char *q = shell_quote(d.data);
        sbuf c;
        sb_init(&c);
        sb_addf(&c, "rm -rf %s", q);
        free(run_capture(c.data, NULL));
        sb_free(&c); free(q);
        printf("Skill '%s' supprimé.\n", name);
    }
    sb_free(&d); free(root);
}

/* ---------- /mcp : registre local de serveurs MCP ---------- */

typedef struct { const char *name, *url, *desc; } mcp_entry;

static const mcp_entry CATALOG[] = {
    {"github", "https://api.githubcopilot.com/mcp/", "Dépôts, issues, PR GitHub"},
    {"context7", "https://mcp.context7.com/mcp", "Documentation de bibliothèques à jour"},
    {"deepwiki", "https://mcp.deepwiki.com/mcp", "Wiki de dépôts GitHub"},
};

static char *mcp_file(void) { return bund_path("mcp.list"); }

static void mcp_list(void) {
    char *path = mcp_file();
    char *t = read_text_file(path);
    free(path);
    if (!t || !*t) { puts("Aucun serveur MCP. Essaie /mcp browse puis /mcp add NOM URL"); free(t); return; }
    puts("Serveurs MCP enregistrés :");
    for (char *l = strtok(t, "\n"); l; l = strtok(NULL, "\n")) {
        char *tab = strchr(l, '\t');
        if (tab) { *tab = '\0'; printf("  %-14s %s\n", l, tab + 1); }
    }
    free(t);
}

static void mcp_add(const char *name, const char *url) {
    if (!safe_name(name)) { puts("nom invalide (lettres, chiffres, - _ .)"); return; }
    if (!safe_url(url)) { puts("URL invalide : https:// requis"); return; }
    char *path = mcp_file();
    char *old = read_text_file(path);
    sbuf n;
    sb_init(&n);
    if (old) {
        for (char *l = strtok(old, "\n"); l; l = strtok(NULL, "\n")) {
            size_t nl = strlen(name);
            if (!strncmp(l, name, nl) && l[nl] == '\t') continue;
            sb_addf(&n, "%s\n", l);
        }
        free(old);
    }
    sb_addf(&n, "%s\t%s\n", name, url);
    write_text_file(path, n.data, 0);
    sb_free(&n); free(path);
    printf("Serveur MCP '%s' enregistré.\n", name);
}

static void mcp_remove(const char *name) {
    char *path = mcp_file();
    char *old = read_text_file(path);
    if (!old) { puts("Aucun serveur MCP."); free(path); return; }
    sbuf n;
    sb_init(&n);
    int found = 0;
    for (char *l = strtok(old, "\n"); l; l = strtok(NULL, "\n")) {
        size_t nl = strlen(name);
        if (!strncmp(l, name, nl) && l[nl] == '\t') { found = 1; continue; }
        sb_addf(&n, "%s\n", l);
    }
    if (found) { write_text_file(path, n.data, 0); printf("'%s' retiré.\n", name); }
    else puts("serveur introuvable (/mcp list)");
    sb_free(&n); free(old); free(path);
}


/* ---------- MCP : appels JSON-RPC (HTTP « streamable », auth + session) ---------- */

static char g_sid[256];

static char *mcp_url_of(const char *name) {
    char *path = mcp_file();
    char *t = read_text_file(path);
    free(path);
    if (!t) return NULL;
    char *res = NULL;
    size_t nl = strlen(name);
    for (char *l = strtok(t, "\n"); l; l = strtok(NULL, "\n"))
        if (!strncmp(l, name, nl) && l[nl] == '\t') { res = str_dup(l + nl + 1); break; }
    free(t);
    return res;
}

/* Jeton : ~/.bund/secrets/mcp_NOM (/mcp auth), sinon jeton GitHub pour les serveurs GitHub. */
static char *mcp_token(const char *name, const char *url) {
    sbuf sp;
    sb_init(&sp);
    sb_addf(&sp, "secrets/mcp_%s", name);
    char *path = bund_path(sp.data);
    sb_free(&sp);
    char *t = read_text_file(path);
    free(path);
    if (t) {
        char *r = *str_trim(t) ? str_dup(str_trim(t)) : NULL;
        free(t);
        if (r) return r;
    }
    if (strstr(url, "github") && load_github_token()) {
        const char *g = getenv("BUND_GH_TOKEN");
        if (g && *g) return str_dup(g);
    }
    return NULL;
}

/* Réponse JSON brute, ou dernière ligne « data: » si le serveur répond en SSE. */
static char *mcp_payload(const char *raw) {
    const char *last = NULL;
    for (const char *l = raw; l && *l; ) {
        const char *eol = strchr(l, '\n');
        if (!strncmp(l, "data:", 5)) last = l + 5;
        l = eol ? eol + 1 : NULL;
    }
    char *r = str_dup(last ? last : raw);
    char *nl = last ? strchr(r, '\n') : NULL;
    if (nl) *nl = '\0';
    return r;
}

static void mcp_read_headers(const char *dump) {
    for (const char *l = dump; l && *l; ) {
        const char *eol = strchr(l, '\n');
        if (!strncasecmp(l, "mcp-session-id:", 15)) {
            const char *v = l + 15;
            while (*v == ' ') v++;
            size_t n = eol ? (size_t)(eol - v) : strlen(v);
            while (n && (v[n - 1] == '\r' || v[n - 1] == ' ')) n--;
            if (n && n < sizeof g_sid) { memcpy(g_sid, v, n); g_sid[n] = '\0'; }
        }
        l = eol ? eol + 1 : NULL;
    }
}

static char *mcp_rpc(const char *url, const char *token, const char *method,
                     const char *params, int id) {
    sbuf b, h;
    sb_init(&b);
    sb_init(&h);
    if (id) sb_addf(&b, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}", id, method, params);
    else sb_addf(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}", method, params);
    sb_add(&h, "Content-Type: application/json\nAccept: application/json, text/event-stream\n");
    if (token) sb_addf(&h, "Authorization: Bearer %s\n", token);
    if (g_sid[0]) sb_addf(&h, "Mcp-Session-Id: %s\n", g_sid);

    char *tmpdir = bund_path("tmp");
    mkdir_p(tmpdir);
    free(tmpdir);
    char *hp = bund_path("tmp/mcp_h.txt"), *bp = bund_path("tmp/mcp_b.json"), *dp = bund_path("tmp/mcp_d.txt");
    write_text_file(hp, h.data, 1);
    write_text_file(bp, b.data, 1);
    char *qh, *qb, *qd, *qu = shell_quote(url);
    sbuf o;
    sb_init(&o); sb_addf(&o, "@%s", hp); qh = shell_quote(o.data); sb_free(&o);
    sb_init(&o); sb_addf(&o, "@%s", bp); qb = shell_quote(o.data); sb_free(&o);
    qd = shell_quote(dp);
    sbuf cmd;
    sb_init(&cmd);
    sb_addf(&cmd, "curl -sS --max-time 60 -X POST -H %s --data-binary %s -D %s %s 2>&1", qh, qb, qd, qu);
    int st = 0;
    char *raw = run_capture(cmd.data, &st);
    char *dump = read_text_file(dp);
    if (dump) mcp_read_headers(dump);
    int http = 0;
    if (dump && !strncmp(dump, "HTTP/", 5)) { const char *sp = strchr(dump, ' '); if (sp) http = atoi(sp + 1); }
    char *res;
    if (http == 401 || http == 403) {
        res = str_dup("ERREUR : authentification refusée. Utilise /mcp auth NOM JETON (ou /login pour GitHub).");
    } else {
        res = mcp_payload(raw);
        if (http >= 400 && !strstr(res, "\"error\"")) {
            sbuf e; sb_init(&e);
            sb_addf(&e, "ERREUR HTTP %d : %.300s", http, res);
            free(res); res = sb_steal(&e);
        }
    }
    unlink(hp); unlink(bp); unlink(dp);
    free(hp); free(bp); free(dp); free(qh); free(qb); free(qd); free(qu); free(raw); free(dump);
    sb_free(&cmd); sb_free(&b); sb_free(&h);
    return res;
}

/* Ouvre une connexion : renvoie l'URL (à libérer) ou NULL avec `err` rempli. */
static char *mcp_open(const char *name, char **token, char *err, size_t errsz) {
    g_sid[0] = '\0';
    char *url = mcp_url_of(name);
    if (!url) { snprintf(err, errsz, "serveur MCP inconnu : %s (/mcp list)", name); return NULL; }
    *token = mcp_token(name, url);
    char *r = mcp_rpc(url, *token, "initialize",
        "{\"protocolVersion\":\"2025-03-26\",\"capabilities\":{},\"clientInfo\":{\"name\":\"bund\",\"version\":\"" BUND_VERSION "\"}}", 1);
    if (!strstr(r, "\"result\"")) {
        char *safe = redact_secrets(r);
        snprintf(err, errsz, "échec de l'initialisation MCP : %.300s%s", safe,
                 (!*token && strstr(r, "uthorization")) ? "\n→ ce serveur demande un jeton : /mcp auth NOM JETON" : "");
        free(safe); free(r); free(url);
        free(*token); *token = NULL;
        return NULL;
    }
    free(r);
    free(mcp_rpc(url, *token, "notifications/initialized", "{}", 0));
    return url;
}

char *mcp_list_str(void) {
    char *path = mcp_file();
    char *t = read_text_file(path);
    free(path);
    if (!t || !*t) { free(t); return str_dup("aucun serveur MCP enregistré"); }
    sbuf o;
    sb_init(&o);
    for (char *l = strtok(t, "\n"); l; l = strtok(NULL, "\n")) {
        char *tab = strchr(l, '\t');
        if (tab) { *tab = '\0'; sb_addf(&o, "%s %s\n", l, tab + 1); }
    }
    free(t);
    return sb_steal(&o);
}

char *mcp_tools_str(const char *name) {
    char err[600] = {0}, *token = NULL;
    char *url = mcp_open(name, &token, err, sizeof err);
    if (!url) return str_dup(err);
    char *r = mcp_rpc(url, token, "tools/list", "{}", 2);
    sbuf o;
    sb_init(&o);
    const char *p = strstr(r, "\"tools\"");
    if (!p) sb_addf(&o, "aucun outil renvoyé : %.300s", r);
    while (p && (p = strstr(p, "\"name\"")) != NULL) {
        p += 6;
        while (*p == ' ' || *p == ':') p++;
        if (*p != '"') continue;
        p++;
        const char *e = strchr(p, '"');
        if (!e) break;
        sb_addf(&o, "- %.*s", (int)(e - p), p);
        const char *d = strstr(e, "\"description\"");
        const char *nx = strstr(e, "\"name\"");
        if (d && (!nx || d < nx)) {
            d += 13;
            while (*d == ' ' || *d == ':') d++;
            if (*d == '"') {
                d++;
                const char *de = d;
                while (*de && *de != '"' && (de - d) < 100) { if (*de == '\\') de++; if (*de) de++; }
                sb_addf(&o, " : %.*s", (int)(de - d), d);
            }
        }
        sb_addc(&o, '\n');
        p = e;
    }
    free(r); free(url); free(token);
    return sb_steal(&o);
}

char *mcp_call_str(const char *name, const char *tool, const char *args) {
    if (!safe_name(tool)) return str_dup("nom d'outil invalide");
    char err[600] = {0}, *token = NULL;
    char *url = mcp_open(name, &token, err, sizeof err);
    if (!url) return str_dup(err);
    sbuf pr;
    sb_init(&pr);
    sb_addf(&pr, "{\"name\":\"%s\",\"arguments\":%s}", tool, (args && *args) ? args : "{}");
    char *r = mcp_rpc(url, token, "tools/call", pr.data, 3);
    char *safe = redact_secrets(r);
    free(r); sb_free(&pr); free(url); free(token);
    return safe;
}

static void mcp_tools(const char *name) {
    char *t = mcp_tools_str(name);
    printf("Outils de '%s' :\n%s\n", name, t);
    free(t);
}

static void mcp_call(const char *name, const char *tool, const char *args) {
    if (!confirm("Appeler cet outil MCP ?")) { puts("Annulé."); return; }
    char *t = mcp_call_str(name, tool, args);
    printf("%.4000s\n", t);
    free(t);
}

static void mcp_auth(const char *name, const char *token) {
    if (!safe_name(name) || !*token) { puts("usage : /mcp auth NOM JETON"); return; }
    sbuf sp;
    sb_init(&sp);
    sb_addf(&sp, "secrets/mcp_%s", name);
    char *path = bund_path(sp.data);
    sb_free(&sp);
    write_text_file(path, token, 1);
    free(path);
    printf("Jeton enregistré pour '%s' (~/.bund/secrets, droits restreints).\n", name);
}

void mcp_cmd(const char *arg) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", arg ? arg : "");
    char *a = str_trim(buf);
    if (!*a || !strcmp(a, "list")) { mcp_list(); return; }
    if (!strcmp(a, "browse")) {
        puts("Serveurs MCP connus (ajoute avec /mcp add NOM URL) :");
        for (size_t i = 0; i < sizeof CATALOG / sizeof CATALOG[0]; i++)
            printf("  %-10s %s\n             %s\n", CATALOG[i].name, CATALOG[i].desc, CATALOG[i].url);
        return;
    }
    if (!strncmp(a, "add ", 4)) {
        char *name = str_trim(a + 4);
        char *url = strchr(name, ' ');
        if (!url) {
            for (size_t i = 0; i < sizeof CATALOG / sizeof CATALOG[0]; i++)
                if (!strcmp(CATALOG[i].name, name)) { mcp_add(name, CATALOG[i].url); return; }
            puts("usage : /mcp add NOM URL");
            return;
        }
        *url++ = '\0';
        mcp_add(name, str_trim(url));
        return;
    }
    if (!strncmp(a, "remove ", 7)) { mcp_remove(str_trim(a + 7)); return; }
    if (!strncmp(a, "auth ", 5)) {
        char *name = str_trim(a + 5);
        char *tok = strchr(name, ' ');
        if (!tok) { puts("usage : /mcp auth NOM JETON"); return; }
        *tok++ = '\0';
        mcp_auth(name, str_trim(tok));
        return;
    }
    if (!strncmp(a, "tools ", 6)) { mcp_tools(str_trim(a + 6)); return; }
    if (!strncmp(a, "call ", 5)) {
        char *name = str_trim(a + 5);
        char *tool = strchr(name, ' ');
        if (!tool) { puts("usage : /mcp call NOM OUTIL [JSON]"); return; }
        *tool++ = '\0';
        tool = str_trim(tool);
        char *args = strchr(tool, ' ');
        if (args) *args++ = '\0';
        mcp_call(name, tool, args ? str_trim(args) : "");
        return;
    }
    puts("usage : /mcp [list | browse | add NOM [URL] | remove NOM | auth NOM JETON | tools NOM | call NOM OUTIL [JSON]]");
}
