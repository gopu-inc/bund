#include "bund.h"

#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <strings.h>

static void oom(void) {
    fprintf(stderr, "bund: mémoire insuffisante\n");
    exit(1);
}

void sb_init(sbuf *b) {
    b->cap = 256;
    b->len = 0;
    b->data = malloc(b->cap);
    if (!b->data) oom();
    b->data[0] = '\0';
}

static void sb_grow(sbuf *b, size_t extra) {
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return;
    size_t nc = b->cap;
    while (nc < need) nc *= 2;
    char *p = realloc(b->data, nc);
    if (!p) oom();
    b->data = p;
    b->cap = nc;
}

void sb_addn(sbuf *b, const char *s, size_t n) {
    sb_grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void sb_add(sbuf *b, const char *s) { sb_addn(b, s, strlen(s)); }

void sb_addc(sbuf *b, char c) { sb_addn(b, &c, 1); }

void sb_addf(sbuf *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        sb_grow(b, (size_t)n);
        vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
}

char *sb_steal(sbuf *b) {
    char *p = b->data;
    b->data = NULL;
    b->len = b->cap = 0;
    return p ? p : str_dup("");
}

void sb_free(sbuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

char *str_dup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) oom();
    memcpy(p, s, n);
    return p;
}

char *str_trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *e = '\0';
    return s;
}

int str_ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

/* ---------- JSON ---------- */

char *json_escape(const char *s) {
    sbuf b;
    sb_init(&b);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  sb_add(&b, "\\\""); break;
        case '\\': sb_add(&b, "\\\\"); break;
        case '\n': sb_add(&b, "\\n"); break;
        case '\r': sb_add(&b, "\\r"); break;
        case '\t': sb_add(&b, "\\t"); break;
        default:
            if (*p < 0x20) sb_addf(&b, "\\u%04x", *p);
            else sb_addc(&b, (char)*p);
        }
    }
    return sb_steal(&b);
}

static int prefix_ci(const char *text, const char *prefix) {
    size_t n = strlen(prefix);
    return strncasecmp(text, prefix, n) == 0;
}

static int secret_boundary(char c) {
    return c == '\0' || isspace((unsigned char)c) || c == '"' || c == '\'' ||
           c == '(' || c == '[' || c == '{' || c == '=' || c == ':';
}

char *redact_secrets(const char *s) {
    static const char *assignments[] = {
        "api_key=", "apikey=", "password=", "secret=", "token=", NULL
    };
    static const char *tokens[] = {
        "github_pat_", "ghp_", "xoxb-", "xoxp-", "sk-", "akia", NULL
    };
    sbuf out;
    sb_init(&out);
    const char *p = s ? s : "";
    while (*p) {
        int handled = 0;
        for (int i = 0; assignments[i]; i++) {
            size_t n = strlen(assignments[i]);
            if (!prefix_ci(p, assignments[i])) continue;
            sb_addn(&out, p, n);
            p += n;
            while (*p == '"' || *p == '\'' || isspace((unsigned char)*p)) {
                sb_addc(&out, *p);
                p++;
            }
            sb_add(&out, "[REDACTED]");
            while (*p && !isspace((unsigned char)*p) &&
                   *p != '"' && *p != '\'' && *p != ',' && *p != ';' &&
                   *p != '&' && *p != ')') p++;
            handled = 1;
            break;
        }
        if (handled) continue;

        for (int i = 0; tokens[i]; i++) {
            size_t n = strlen(tokens[i]);
            if (!prefix_ci(p, tokens[i]) || (p != s && !secret_boundary(p[-1])))
                continue;
            sb_add(&out, "[REDACTED]");
            p += n;
            while (*p && (isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == '.'))
                p++;
            handled = 1;
            break;
        }
        if (!handled) sb_addc(&out, *p++);
    }
    return sb_steal(&out);
}

static const char *json_find_value(const char *json, const char *key) {
    size_t kl = strlen(key);
    char *pat = malloc(kl + 3);
    if (!pat) oom();
    sprintf(pat, "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + kl + 2;
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q == ':') {
            free(pat);
            q++;
            while (*q && isspace((unsigned char)*q)) q++;
            return q;
        }
        p += 1;
    }
    free(pat);
    return NULL;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

static void utf8_put(sbuf *b, unsigned cp) {
    if (cp < 0x80) {
        sb_addc(b, (char)cp);
    } else if (cp < 0x800) {
        sb_addc(b, (char)(0xC0 | (cp >> 6)));
        sb_addc(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        sb_addc(b, (char)(0xE0 | (cp >> 12)));
        sb_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_addc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_addc(b, (char)(0xF0 | (cp >> 18)));
        sb_addc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_addc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

char *json_get_string(const char *json, const char *key) {
    const char *v = json_find_value(json, key);
    if (!v || *v != '"') return NULL;
    v++;
    sbuf b;
    sb_init(&b);
    while (*v && *v != '"') {
        if (*v == '\\') {
            v++;
            if (!*v) break;
            switch (*v) {
            case 'n': sb_addc(&b, '\n'); break;
            case 't': sb_addc(&b, '\t'); break;
            case 'r': sb_addc(&b, '\r'); break;
            case 'b': sb_addc(&b, '\b'); break;
            case 'f': sb_addc(&b, '\f'); break;
            case 'u': {
                unsigned cp = 0;
                for (int i = 0; i < 4 && v[1]; i++) {
                    v++;
                    cp = cp * 16 + (unsigned)hexval((unsigned char)*v);
                }
                if (cp >= 0xD800 && cp < 0xDC00 && v[1] == '\\' && v[2] == 'u') {
                    unsigned lo = 0;
                    v += 2;
                    for (int i = 0; i < 4 && v[1]; i++) {
                        v++;
                        lo = lo * 16 + (unsigned)hexval((unsigned char)*v);
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8_put(&b, cp);
                break;
            }
            default: sb_addc(&b, *v); break;
            }
            v++;
        } else {
            sb_addc(&b, *v);
            v++;
        }
    }
    return sb_steal(&b);
}

long json_get_int(const char *json, const char *key, long def) {
    const char *v = json_find_value(json, key);
    if (!v) return def;
    char *end;
    long n = strtol(v, &end, 10);
    return end == v ? def : n;
}

/* ---------- fichiers, shell, HTTP ---------- */

char *bund_path(const char *sub) {
    const char *base = getenv("BUND_HOME");
    sbuf b;
    sb_init(&b);
    if (base && *base) {
        sb_add(&b, base);
    } else {
        const char *h = getenv("HOME");
        sb_add(&b, h ? h : "/tmp");
        sb_add(&b, "/.bund");
    }
    if (sub && *sub) {
        sb_addc(&b, '/');
        sb_add(&b, sub);
    }
    return sb_steal(&b);
}

int mkdir_p(const char *path) {
    char *tmp = str_dup(path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    int r = mkdir(tmp, 0700);
    free(tmp);
    if (r != 0 && errno != EEXIST) return -1;
    return 0;
}

int ensure_bund_dirs(void) {
    static const char *subs[] = {"", "secrets", "skills", "jobs", "sessions",
                                 "tmp", "memory", "reports"};
    for (size_t i = 0; i < sizeof subs / sizeof subs[0]; i++) {
        char *p = bund_path(subs[i]);
        int r = mkdir_p(p);
        free(p);
        if (r != 0) return -1;
    }
    return 0;
}

char *read_text_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    if (!buf) oom();
    size_t r = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[r] = '\0';
    return buf;
}

int write_text_file(const char *path, const char *data, int secret) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = strlen(data);
    size_t w = fwrite(data, 1, n, f);
    fclose(f);
    if (secret) chmod(path, 0600);
    return w == n ? 0 : -1;
}

int file_exists(const char *path) { return access(path, F_OK) == 0; }

char *shell_quote(const char *s) {
    sbuf b;
    sb_init(&b);
    sb_addc(&b, '\'');
    for (; *s; s++) {
        if (*s == '\'') sb_add(&b, "'\\''");
        else sb_addc(&b, *s);
    }
    sb_addc(&b, '\'');
    return sb_steal(&b);
}

char *run_capture(const char *cmd, int *status) {
    sbuf b;
    sb_init(&b);
    FILE *f = popen(cmd, "r");
    if (!f) {
        if (status) *status = -1;
        return sb_steal(&b);
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sb_addn(&b, buf, n);
    int rc = pclose(f);
    if (status) *status = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    return sb_steal(&b);
}

/* Appel HTTP via curl. En-têtes et corps passent par des fichiers temporaires
   (jamais sur la ligne de commande), supprimés après usage. */
char *http_call(const char *method, const char *url, const char *body,
                const char *headers, int timeout) {
    char *tmpdir = bund_path("tmp");
    mkdir_p(tmpdir);
    free(tmpdir);

    char *hpath = bund_path("tmp/headers.txt");
    char *bpath = bund_path("tmp/body.json");
    write_text_file(hpath, headers ? headers : "", 1);

    sbuf hopt;
    sb_init(&hopt);
    sb_addc(&hopt, '@');
    sb_add(&hopt, hpath);
    char *qh = shell_quote(hopt.data);
    char *qm = shell_quote(method);
    char *qu = shell_quote(url);

    sbuf cmd;
    sb_init(&cmd);
    sb_addf(&cmd, "curl -sS --max-time %d -X %s -H %s",
            timeout > 0 ? timeout : 120, qm, qh);
    if (body) {
        write_text_file(bpath, body, 0);
        sbuf bopt;
        sb_init(&bopt);
        sb_addc(&bopt, '@');
        sb_add(&bopt, bpath);
        char *qb = shell_quote(bopt.data);
        sb_addf(&cmd, " --data-binary %s", qb);
        free(qb);
        sb_free(&bopt);
    }
    sb_addf(&cmd, " %s 2>&1", qu);

    int st = 0;
    char *out = run_capture(cmd.data, &st);
    unlink(hpath);
    if (body) unlink(bpath);

    free(hpath);
    free(bpath);
    free(qh);
    free(qm);
    free(qu);
    sb_free(&hopt);
    sb_free(&cmd);
    return out;
}

int confirm(const char *question) {
    /* TUI owns keyboard input: never consume pasted prompt text with fgets. */
    if (tui_is_active()) return tui_confirm(question);
    if (!isatty(0)) return 0;
    printf("%s [o/N] ", question);
    fflush(stdout);
    char line[16];
    if (!fgets(line, sizeof line, stdin)) return 0;
    return line[0] == 'o' || line[0] == 'O' || line[0] == 'y' || line[0] == 'Y';
}
