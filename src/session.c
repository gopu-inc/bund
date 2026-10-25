#include "bund.h"

#include <ctype.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

#define SESSION_CONTEXT_LIMIT 12000

static unsigned session_serial;

static int safe_id(const char *id) {
    if (!id || strncmp(id, "s_", 2) != 0 || strlen(id) > 80) return 0;
    for (const unsigned char *p = (const unsigned char *)id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return 0;
    return 1;
}

static char *session_dir(const char *id) {
    sbuf p;
    sb_init(&p);
    sb_addf(&p, "sessions/%s", id);
    char *dir = bund_path(p.data);
    sb_free(&p);
    return dir;
}

static char *active_id_read(void) {
    char *path = bund_path("session.active");
    char *id = read_text_file(path);
    free(path);
    if (!id) return NULL;
    char *trimmed = str_trim(id);
    if (!safe_id(trimmed)) {
        free(id);
        return NULL;
    }
    char *result = str_dup(trimmed);
    free(id);
    return result;
}

static char *session_create(const char *label) {
    char id[96];
    snprintf(id, sizeof id, "s_%ld_%d_%u", (long)time(NULL), (int)getpid(),
             ++session_serial);
    char *dir = session_dir(id);
    if (mkdir_p(dir) != 0) {
        free(dir);
        return NULL;
    }

    char title[160] = "Nouvelle session";
    if (label && *label) {
        size_t n = 0;
        while (label[n] && label[n] != '\n' && label[n] != '\r' && n < sizeof title - 1) {
            title[n] = label[n];
            n++;
        }
        title[n] = '\0';
    }
    sbuf p;
    sb_init(&p);
    sb_addf(&p, "%s/title.txt", dir);
    write_text_file(p.data, title, 0);
    sb_free(&p);
    free(dir);

    char *active = bund_path("session.active");
    write_text_file(active, id, 0);
    free(active);
    return str_dup(id);
}

char *session_current_id(void) {
    char *id = active_id_read();
    if (id) {
        char *dir = session_dir(id);
        int exists = file_exists(dir);
        free(dir);
        if (exists) return id;
        free(id);
    }
    return session_create("Session par défaut");
}

void session_record(const char *role, const char *text) {
    char *id = session_current_id();
    if (!id) return;
    char *dir = session_dir(id);
    sbuf p;
    sb_init(&p);
    sb_addf(&p, "%s/transcript.md", dir);
    FILE *f = fopen(p.data, "a");
    if (f) {
        char *safe_text = redact_secrets(text);
        fprintf(f, "\n### %s — %ld\n\n", role ? role : "note", (long)time(NULL));
        if (safe_text) fputs(safe_text, f);
        fputc('\n', f);
        fclose(f);
        free(safe_text);
    }
    sb_free(&p);
    free(dir);
    free(id);
}

char *session_context(void) {
    char *id = session_current_id();
    if (!id) return str_dup("");
    char *dir = session_dir(id);
    sbuf p;
    sb_init(&p);
    sb_addf(&p, "%s/transcript.md", dir);
    char *text = read_text_file(p.data);
    sb_free(&p);
    free(dir);
    free(id);
    if (!text) return str_dup("");

    char *safe_text = redact_secrets(text);
    free(text);
    text = safe_text;
    size_t n = strlen(text);
    size_t start = n > SESSION_CONTEXT_LIMIT ? n - SESSION_CONTEXT_LIMIT : 0;
    char *result = str_dup(text + start);
    free(text);
    return result;
}

static void session_list(void) {
    char *root = bund_path("sessions");
    DIR *d = opendir(root);
    if (!d) {
        puts("Aucune session.");
        free(root);
        return;
    }
    char *active = active_id_read();
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !safe_id(e->d_name)) continue;
        sbuf p;
        sb_init(&p);
        sb_addf(&p, "%s/%s/title.txt", root, e->d_name);
        char *title = read_text_file(p.data);
        sb_free(&p);
        if (!title) continue;
        printf("%s %-30s %s\n",
               active && !strcmp(active, e->d_name) ? "*" : " ",
               e->d_name, str_trim(title));
        free(title);
        count++;
    }
    closedir(d);
    if (!count) puts("Aucune session.");
    free(active);
    free(root);
}

static void session_show(const char *id) {
    char *dir = session_dir(id);
    sbuf p;
    sb_init(&p);
    sb_addf(&p, "%s/transcript.md", dir);
    char *text = read_text_file(p.data);
    if (!text) puts("Cette session n'a pas encore d'historique.");
    else {
        fputs(text, stdout);
        if (*text && text[strlen(text) - 1] != '\n') putchar('\n');
    }
    free(text);
    sb_free(&p);
    free(dir);
}

void session_cmd(const char *arg) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", arg ? arg : "");
    char *a = str_trim(buf);
    if (!*a || !strcmp(a, "current")) {
        char *id = session_current_id();
        printf("Session active : %s\n", id ? id : "(indisponible)");
        free(id);
        return;
    }
    if (!strcmp(a, "list")) {
        session_list();
        return;
    }
    if (!strncmp(a, "new", 3) && (a[3] == '\0' || isspace((unsigned char)a[3]))) {
        const char *label = str_trim(a + 3);
        char *id = session_create(label);
        if (id) printf("Nouvelle session active : %s\n", id);
        else puts("Impossible de créer la session.");
        free(id);
        return;
    }
    if (!strncmp(a, "use ", 4) || !strncmp(a, "resume ", 7)) {
        const char *id = !strncmp(a, "use ", 4) ? str_trim(a + 4) : str_trim(a + 7);
        if (!safe_id(id)) {
            puts("Identifiant de session invalide.");
            return;
        }
        char *dir = session_dir(id);
        if (!file_exists(dir)) {
            puts("Session introuvable (/session list).");
            free(dir);
            return;
        }
        char *active = bund_path("session.active");
        int ok = write_text_file(active, id, 0) == 0;
        printf(ok ? "Session active : %s\n" : "Impossible d'activer cette session.\n", id);
        free(active);
        free(dir);
        return;
    }
    if (!strncmp(a, "show", 4) && (a[4] == '\0' || isspace((unsigned char)a[4]))) {
        const char *id = str_trim(a + 4);
        char *current = NULL;
        if (!*id) {
            current = session_current_id();
            id = current;
        }
        if (!safe_id(id)) puts("Identifiant de session invalide.");
        else session_show(id);
        free(current);
        return;
    }
    if (!strncmp(a, "rename ", 7)) {
        char *id = session_current_id();
        const char *title = str_trim(a + 7);
        if (!id || !*title) { puts("usage : /session rename TITRE"); free(id); return; }
        char *dir = session_dir(id);
        sbuf p;
        sb_init(&p);
        sb_addf(&p, "%s/title.txt", dir);
        char t[160];
        snprintf(t, sizeof t, "%s", title);
        for (char *q = t; *q; q++) if (*q == '\n' || *q == '\r') *q = ' ';
        write_text_file(p.data, t, 0);
        printf("Session renommée : %s\n", t);
        sb_free(&p); free(dir); free(id);
        return;
    }
    puts("usage : /session [rename TITRE | current | list | new [nom] | use ID | resume ID | show [ID]]");
}
