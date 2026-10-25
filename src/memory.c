#include "bund.h"

#include <ctype.h>
#include <dirent.h>
#include <time.h>

#define MEMORY_CONTEXT_LIMIT 10000

static int valid_slug(const char *slug) {
    if (!slug || !*slug || strlen(slug) > 64 || !strcmp(slug, ".") || !strcmp(slug, ".."))
        return 0;
    for (const unsigned char *p = (const unsigned char *)slug; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return 0;
    return 1;
}

static char *memory_path(const char *slug) {
    sbuf p;
    sb_init(&p);
    sb_addf(&p, "memory/%s.md", slug);
    char *path = bund_path(p.data);
    sb_free(&p);
    return path;
}

static void memory_seed(void) {
    static const char *names[] = {"project", "decisions", "lessons"};
    static const char *titles[] = {"# Project memory\n", "# Decisions\n", "# Lessons\n"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char *path = memory_path(names[i]);
        if (!file_exists(path)) write_text_file(path, titles[i], 0);
        free(path);
    }
}

static int contains_ci(const char *text, const char *needle) {
    size_t n = strlen(needle);
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

static int contains_token_ci(const char *text, const char *needle) {
    size_t n = strlen(needle);
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n && (p == text || isspace((unsigned char)p[-1]) ||
                       p[-1] == '"' || p[-1] == '\'' || p[-1] == ':' || p[-1] == '='))
            return 1;
    }
    return 0;
}

static int looks_like_secret(const char *text) {
    static const char *assignments[] = {
        "api_key=", "apikey=", "password=", "secret=", "token=",
        "bearer ", "-----begin private key-----", NULL
    };
    static const char *tokens[] = {
        "sk-", "ghp_", "github_pat_", "xoxb-", "xoxp-", "akia", NULL
    };
    for (int i = 0; assignments[i]; i++)
        if (contains_ci(text, assignments[i])) return 1;
    for (int i = 0; tokens[i]; i++)
        if (contains_token_ci(text, tokens[i])) return 1;
    return 0;
}

int memory_add_note(const char *slug, const char *text, int require_confirm) {
    if (!valid_slug(slug) || !text || !*text || strlen(text) > 8000) return -1;
    if (looks_like_secret(text)) return -1;
    if (require_confirm) {
        sbuf q;
        sb_init(&q);
        sb_addf(&q, "Enregistrer cette note dans la mémoire '%s' ?", slug);
        int approved = confirm(q.data);
        sb_free(&q);
        if (!approved) return 1;
    }

    char *path = memory_path(slug);
    FILE *f = fopen(path, "a");
    if (!f) {
        free(path);
        return -1;
    }
    fprintf(f, "\n- (%ld) ", (long)time(NULL));
    fputs(text, f);
    fputc('\n', f);
    fclose(f);
    free(path);
    return 0;
}

static int memory_link(const char *from, const char *to) {
    if (!valid_slug(from) || !valid_slug(to)) return -1;
    char *src = memory_path(from);
    char *dst = memory_path(to);
    if (!file_exists(src) || !file_exists(dst)) {
        free(src);
        free(dst);
        return -1;
    }
    char *existing = read_text_file(src);
    sbuf marker;
    sb_init(&marker);
    sb_addf(&marker, "[[%s]]", to);
    if (existing && strstr(existing, marker.data)) {
        free(existing);
        free(src);
        free(dst);
        sb_free(&marker);
        return 0;
    }
    FILE *f = fopen(src, "a");
    if (!f) {
        free(existing);
        free(src);
        free(dst);
        sb_free(&marker);
        return -1;
    }
    fprintf(f, "\n\nRelated: %s\n", marker.data);
    fclose(f);
    free(existing);
    free(src);
    free(dst);
    sb_free(&marker);
    return 0;
}

static void memory_list(void) {
    char *root = bund_path("memory");
    DIR *d = opendir(root);
    if (!d) {
        puts("Aucune mémoire.");
        free(root);
        return;
    }
    puts("Nœuds mémoire :");
    struct dirent *e;
    int count = 0;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 4 || strcmp(e->d_name + n - 3, ".md") != 0) continue;
        e->d_name[n - 3] = '\0';
        printf("  %s\n", e->d_name);
        count++;
    }
    if (!count) puts("  (vide)");
    closedir(d);
    free(root);
}

static void memory_show(const char *slug) {
    if (!valid_slug(slug)) {
        puts("Identifiant de mémoire invalide.");
        return;
    }
    char *path = memory_path(slug);
    char *text = read_text_file(path);
    if (!text) puts("Nœud introuvable (/memory list).");
    else {
        fputs(text, stdout);
        if (*text && text[strlen(text) - 1] != '\n') putchar('\n');
    }
    free(text);
    free(path);
}

static void memory_graph(void) {
    char *root = bund_path("memory");
    DIR *d = opendir(root);
    if (!d) {
        puts("Le graphe mémoire est vide.");
        free(root);
        return;
    }
    puts("Graphe mémoire — les liens [[nom]] sont dirigés :");
    struct dirent *e;
    int nodes = 0, edges = 0;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 4 || strcmp(e->d_name + n - 3, ".md") != 0) continue;
        char slug[80];
        size_t sl = n - 3;
        if (sl >= sizeof slug) continue;
        memcpy(slug, e->d_name, sl);
        slug[sl] = '\0';
        if (!valid_slug(slug)) continue;
        nodes++;
        char *path = memory_path(slug);
        char *text = read_text_file(path);
        free(path);
        int linked = 0;
        for (char *p = text ? text : ""; (p = strstr(p, "[[")) != NULL; p += 2) {
            char *end = strstr(p + 2, "]]");
            if (!end) break;
            size_t len = (size_t)(end - (p + 2));
            if (len && len < 65) {
                char target[65];
                memcpy(target, p + 2, len);
                target[len] = '\0';
                char *sep = strchr(target, '|');
                if (sep) *sep = '\0';
                if (valid_slug(target)) {
                    char *target_path = memory_path(target);
                    int target_exists = file_exists(target_path);
                    free(target_path);
                    printf("  %s -> %s%s\n", slug, target,
                           target_exists ? "" : " (lien manquant)");
                    edges++;
                    linked = 1;
                }
            }
            p = end + 2;
        }
        if (!linked) printf("  %s (sans lien)\n", slug);
        free(text);
    }
    if (!nodes) puts("  (aucun nœud)");
    else printf("%d nœud(s), %d lien(s)\n", nodes, edges);
    closedir(d);
    free(root);
}

char *memory_context(void) {
    memory_seed();
    char *root = bund_path("memory");
    DIR *d = opendir(root);
    if (!d) {
        free(root);
        return str_dup("");
    }
    sbuf out;
    sb_init(&out);
    struct dirent *e;
    while ((e = readdir(d)) && out.len < MEMORY_CONTEXT_LIMIT) {
        size_t n = strlen(e->d_name);
        if (n < 4 || strcmp(e->d_name + n - 3, ".md") != 0) continue;
        sbuf p;
        sb_init(&p);
        sb_addf(&p, "%s/%s", root, e->d_name);
        char *text = read_text_file(p.data);
        sb_free(&p);
        if (!text || !*text) {
            free(text);
            continue;
        }
        char *safe_text = redact_secrets(text);
        free(text);
        sb_addf(&out, "\n### %s\n", e->d_name);
        size_t remaining = MEMORY_CONTEXT_LIMIT > out.len ?
                           MEMORY_CONTEXT_LIMIT - out.len : 0;
        sb_addn(&out, safe_text, strlen(safe_text) < remaining ? strlen(safe_text) : remaining);
        free(safe_text);
    }
    closedir(d);
    free(root);
    return sb_steal(&out);
}

void memory_cmd(const char *arg) {
    memory_seed();
    char buf[9000];
    snprintf(buf, sizeof buf, "%s", arg ? arg : "");
    char *a = str_trim(buf);
    if (!*a || !strcmp(a, "list")) {
        memory_list();
        return;
    }
    if (!strcmp(a, "graph")) {
        memory_graph();
        return;
    }
    if (!strncmp(a, "show ", 5)) {
        memory_show(str_trim(a + 5));
        return;
    }
    if (!strncmp(a, "add ", 4)) {
        char *slug = str_trim(a + 4);
        char *sp = strchr(slug, ' ');
        if (!sp) {
            puts("usage : /memory add <nœud> <note>");
            return;
        }
        *sp = '\0';
        int r = memory_add_note(slug, str_trim(sp + 1), 0);
        puts(r == 0 ? "Note enregistrée." : "Note refusée : vérifie le nom, le texte et qu'il ne contient pas de secret.");
        return;
    }
    if (!strncmp(a, "link ", 5)) {
        char *from = str_trim(a + 5);
        char *sp = strchr(from, ' ');
        if (!sp) {
            puts("usage : /memory link <source> <cible>");
            return;
        }
        *sp = '\0';
        const char *to = str_trim(sp + 1);
        int r = memory_link(from, to);
        puts(r == 0 ? "Lien enregistré." : "Lien refusé : les deux nœuds doivent exister.");
        return;
    }
    puts("usage : /memory [list | graph | show NŒUD | add NŒUD NOTE | link SOURCE CIBLE]");
}
