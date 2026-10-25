#include "bund.h"
#include "completion.h"
#include "editor.h"

#include <ctype.h>
#include <fcntl.h>
#include <langinfo.h>
#include <locale.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef BUND_HAVE_NOTCURSES
#include <notcurses/notcurses.h>
#endif

#define INPUT_MAX 65536
#define LOG_MAX 32768
#define LIVE_MAX 16384
#define STARTS(s, lit) (strncmp((s), (lit), strlen(lit)) == 0)

static int g_multiline;
static char g_log[LOG_MAX];     /* historique des sorties déjà terminées */

static void print_help(void) {
    puts(
        "Commandes BUND :\n"
        "  <texte>             lance l'agent sur cet objectif\n"
        "  /run <objectif>     idem, enregistré comme job\n"
        "  /multi              active/désactive la saisie multiligne\n"
        "  /conf               affiche la configuration (clé : statut seulement)\n"
        "  /conf wizard        assistant interactif\n"
        "  /conf presets       liste les fournisseurs\n"
        "  /conf preset NOM    applique un fournisseur (ex : /conf preset groq)\n"
        "  /conf set CLÉ VAL   modifie une valeur (ex : /conf set model gpt-4.1)\n"
        "  /env [show|set|reset] environnement sûr de l'agent\n"
        "  /session [list|new|use|resume|show] sessions persistantes\n"
        "  /memory [list|show|add|link|graph] mémoire et graphe local\n"
        "  /login              connexion GitHub (une fois ; les commits sont signés Bund)\n"
        "  /whoami  /logout    compte GitHub connecté / déconnexion\n"
        "  /git <args>         git ; ou directement : git add . / git commit -m \"...\" / git push\n"
        "  /agent list|use X   profils : bund, claude-code, mistral-code, codex, copilot-cli\n"
        "  /skills [show X | add NOM URL | remove NOM]  skills (manifeste bund.skills)\n"
        "  /mcp [list|browse|add|remove|tools NOM|call NOM OUTIL JSON]  serveurs MCP\n"
        "  /job [list]         historique des jobs\n"
        "  /manifest           ajoute le contributeur Bund aux manifestes du projet\n"
        "  /help  /clear  /exit\n"
        "Saisie : Entrée envoie · Ctrl+O multiligne (Ctrl+S envoie) · Tab complète\n"
        "         ↑↓ historique / lignes · Ctrl+L efface l'écran · Ctrl+C vide la saisie");
}

/* Retourne 1 si la session doit se terminer. */
static int dispatch(bund_config *c, const char *line) {
    if (line[0] != '/') {
        /* « git add . » tapé directement = /git add . */
        if (!strncmp(line, "git ", 4) && git_is_subcommand(line + 4)) {
            git_command(c, line + 4);
            return 0;
        }
        agent_run(c, line);
        return 0;
    }
    char *buf = str_dup(line + 1);
    char *cmd = buf;
    char *arg = strchr(buf, ' ');
    if (arg) {
        *arg = '\0';
        arg = str_trim(arg + 1);
    } else {
        arg = buf + strlen(buf);
    }

    int quit = 0;
    if (!strcmp(cmd, "help")) print_help();
    else if (!strcmp(cmd, "exit") || !strcmp(cmd, "quit")) quit = 1;
    else if (!strcmp(cmd, "clear")) g_log[0] = '\0';
    else if (!strcmp(cmd, "multi")) {
        g_multiline = !g_multiline;
        printf("Saisie multiligne %s.\n", g_multiline ? "activée" : "désactivée");
    }
    else if (!strcmp(cmd, "conf")) conf_cmd(c, arg);
    else if (!strcmp(cmd, "env")) env_cmd(c, arg);
    else if (!strcmp(cmd, "session")) session_cmd(arg);
    else if (!strcmp(cmd, "memory")) memory_cmd(arg);
    else if (!strcmp(cmd, "login")) login_github(c);
    else if (!strcmp(cmd, "logout")) logout_github();
    else if (!strcmp(cmd, "whoami")) whoami_github();
    else if (!strcmp(cmd, "git")) git_command(c, arg);
    else if (!strcmp(cmd, "agent")) agent_cmd(c, arg);
    else if (!strcmp(cmd, "skills")) skills_cmd(arg);
    else if (!strcmp(cmd, "mcp")) mcp_cmd(arg);
    else if (!strcmp(cmd, "job")) job_cmd(c, arg);
    else if (!strcmp(cmd, "run")) {
        if (!*arg) puts("usage : /run <objectif>");
        else agent_run(c, arg);
    } else if (!strcmp(cmd, "manifest")) manifest_ensure_dir(".");
    else printf("commande inconnue : /%s (essaie /help)\n", cmd);
    free(buf);
    return quit;
}

#ifdef BUND_HAVE_NOTCURSES
/* =====================================================================
   Interface Notcurses
   - saisie avec retour à la ligne automatique (logique dans editor.c)
   - fenêtre d'autocomplétion (logique dans completion.c)
   - panneau latéral si le terminal est large (>= 100 colonnes)
   ===================================================================== */

#define HIST_MAX 64
static char history[HIST_MAX][INPUT_MAX];
static size_t hist_count;
static struct notcurses *g_nc;
static struct ncplane *g_plane;
static int g_capture_terminal_fd = -1;
static FILE *g_cap_tmp;
static int g_busy;
static int g_utf8;
static char g_cur_cmd[256];
static char g_live[LIVE_MAX];
static char g_notice[96];
static size_t g_first_row;
static comp_item g_comp[5];
static size_t g_ncomp, g_sel, g_quick;
static const bund_config *g_cfg;

typedef struct {
    char login[64];
    int github;
    char branch[64];
    int dirty;
    char tree[20][48];
    int ntree;
    char project[64];
    int key_ok;
} side_info;
static side_info g_side;

typedef struct { unsigned char r, g, b; } rgb;
static const rgb C_ACC = {0, 230, 200}, C_BLUE = {80, 140, 255}, C_DIM = {125, 135, 155},
                 C_TXT = {222, 228, 238}, C_OK = {90, 225, 130}, C_WARN = {255, 195, 80},
                 C_ERR = {255, 105, 105}, C_BORDER = {55, 95, 175}, C_SEL = {30, 70, 165},
                 C_BLACK = {0, 0, 0};

int tui_is_active(void) { return g_nc != NULL && g_plane != NULL; }

static void plane_dims(int *rows, int *cols) {
    *rows = 24;
    *cols = 80;
    /* (void *) : compatible avec Notcurses 2.x (int *) et 3.x (unsigned *). */
    ncplane_dim_yx(g_plane, (void *)rows, (void *)cols);
}

/* Largeur de la zone principale et largeur de texte de la saisie (+1 colonne
   réservée au curseur). Utilisées par le dessin ET par les flèches haut/bas. */
static int main_width(int cols) { return cols >= 100 ? cols - 36 : cols; }
static int input_width(int cols) {
    int w = main_width(cols) - 5;
    return w < 4 ? 4 : w;
}

static void fgc(rgb c) { ncplane_set_fg_rgb8(g_plane, c.r, c.g, c.b); }
static void bgc(rgb c) { ncplane_set_bg_rgb8(g_plane, c.r, c.g, c.b); }
static void plain(void) {
    ncplane_set_fg_default(g_plane);
    ncplane_set_bg_default(g_plane);
    ncplane_set_styles(g_plane, NCSTYLE_NONE);
}

/* Nombre d'octets de `s` (au plus `maxbytes`) qui tiennent dans `maxw` colonnes. */
static size_t fit(const char *s, size_t maxbytes, int maxw) {
    size_t i = 0;
    int w = 0;
    while (i < maxbytes && s[i]) {
        size_t b;
        int cw = ed_cwidth(s + i, maxbytes - i, &b);
        if (!b) b = 1;
        if (w + cw > maxw) break;
        w += cw;
        i += b;
    }
    return i;
}

/* Écrit `s` à (y,x) en tronquant à `maxw` colonnes. */
static void put(int y, int x, int maxw, const char *s, rgb c) {
    if (y < 0 || x < 0 || maxw <= 0 || !*s) return;
    char tmp[1024];
    size_t lim = strlen(s);
    if (lim > sizeof tmp - 1) lim = sizeof tmp - 1;
    size_t n = fit(s, lim, maxw);
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)tmp[i] < 0x20 || tmp[i] == 0x7f) tmp[i] = ' ';
    fgc(c);
    ncplane_putstr_yx(g_plane, y, x, tmp);
}

static void putr(int y, int xright, const char *s, rgb c) {   /* aligné à droite */
    int w = (int)ed_text_width(s, strlen(s));
    put(y, xright - w, w, s, c);
}

static void hline(int y, int x, int w, rgb c) {
    if (w <= 0) return;
    char line[1024];
    const char *h = g_utf8 ? "─" : "-";
    size_t hl = strlen(h), n = 0;
    for (int i = 0; i < w && n + hl < sizeof line; i++) {
        memcpy(line + n, h, hl);
        n += hl;
    }
    line[n] = '\0';
    fgc(c);
    ncplane_putstr_yx(g_plane, y, x, line);
}

static void box(int y, int x, int h, int w, rgb c, const char *title) {
    if (h < 2 || w < 2) return;
    const char *tl = g_utf8 ? "┌" : "+", *tr = g_utf8 ? "┐" : "+";
    const char *bl = g_utf8 ? "└" : "+", *br = g_utf8 ? "┘" : "+";
    const char *v = g_utf8 ? "│" : "|";
    fgc(c);
    ncplane_putstr_yx(g_plane, y, x, tl);
    hline(y, x + 1, w - 2, c);
    fgc(c);
    ncplane_putstr_yx(g_plane, y, x + w - 1, tr);
    for (int r = 1; r < h - 1; r++) {
        ncplane_putstr_yx(g_plane, y + r, x, v);
        ncplane_putstr_yx(g_plane, y + r, x + w - 1, v);
    }
    ncplane_putstr_yx(g_plane, y + h - 1, x, bl);
    hline(y + h - 1, x + 1, w - 2, c);
    fgc(c);
    ncplane_putstr_yx(g_plane, y + h - 1, x + w - 1, br);
    if (title && *title && w > 6) {
        char t[96];
        snprintf(t, sizeof t, " %s ", title);
        put(y, x + 2, w - 4, t, C_TXT);
    }
}

/* Affichage : pendant une commande, la sortie standard pointe vers un fichier
   de capture ; on la redirige temporairement vers le vrai terminal. */
static void nc_render(void) {
    int saved = -1;
    if (g_capture_terminal_fd >= 0) {
        fflush(stdout);
        saved = dup(STDOUT_FILENO);
        if (saved >= 0) dup2(g_capture_terminal_fd, STDOUT_FILENO);
    }
    notcurses_render(g_nc);
    if (saved >= 0) {
        dup2(saved, STDOUT_FILENO);
        close(saved);
    }
}

/* ---------- informations du panneau latéral (sans réseau) ---------- */

static int key_ready(const bund_config *c) {
    if (!c->api_key_env[0]) return 1;
    const char *e = getenv(c->api_key_env);
    if (e && *e) return 1;
    sbuf p;
    sb_init(&p);
    sb_addf(&p, "secrets/%s", c->api_key_env);
    char *path = bund_path(p.data);
    sb_free(&p);
    int ok = file_exists(path);
    free(path);
    return ok;
}

static void refresh_side(const bund_config *c) {
    memset(&g_side, 0, sizeof g_side);
    char login[64];
    char *tokp = bund_path("secrets/github_token");
    int has_token = file_exists(tokp);
    free(tokp);
    if (has_token) {
        g_side.github = 1;
        if (github_cached_user(login, sizeof login, NULL))
            snprintf(g_side.login, sizeof g_side.login, "%s", login);
    }
    int st = 0;
    /* symbolic-ref fonctionne aussi sur un dépôt sans commit ; repli sur le hash en HEAD détachée */
    char *b = run_capture("git symbolic-ref --short -q HEAD 2>/dev/null || git rev-parse --short HEAD 2>/dev/null", &st);
    if (st == 0) {
        snprintf(g_side.branch, sizeof g_side.branch, "%s", str_trim(b));
        free(b);
        b = run_capture("git status --porcelain 2>/dev/null | wc -l", &st);
        g_side.dirty = atoi(b);
    }
    free(b);
    char *ls = run_capture("ls -1p 2>/dev/null | head -20", &st);
    char *save = NULL;
    for (char *l = strtok_r(ls, "\n", &save); l && g_side.ntree < 20; l = strtok_r(NULL, "\n", &save))
        snprintf(g_side.tree[g_side.ntree++], sizeof g_side.tree[0], "%s", l);
    free(ls);
    char cwd[256];
    if (getcwd(cwd, sizeof cwd)) {
        const char *base = strrchr(cwd, '/');
        snprintf(g_side.project, sizeof g_side.project, "%.60s", base && base[1] ? base + 1 : cwd);
    }
    g_side.key_ok = key_ready(c);
}

/* ---------- sortie en direct pendant une commande ---------- */

static void load_capture_tail(void) {
    if (!g_cap_tmp) return;
    fflush(stdout);
    fflush(stderr);
    int fd = fileno(g_cap_tmp);
    struct stat sb;
    if (fstat(fd, &sb) != 0) return;
    off_t size = sb.st_size;
    off_t start = size > (off_t)(sizeof g_live - 1) ? size - (off_t)(sizeof g_live - 1) : 0;
    ssize_t n = pread(fd, g_live, (size_t)(size - start), start);   /* ne déplace pas la position d'écriture */
    if (n < 0) n = 0;
    g_live[n] = '\0';
    size_t skip = 0;
    while (g_live[skip] && ((unsigned char)g_live[skip] & 0xC0) == 0x80) skip++;
    if (skip) memmove(g_live, g_live + skip, (size_t)n - skip + 1);
}

static void log_append(const char *s) {
    size_t n = strlen(s), cur = strlen(g_log);
    if (n >= sizeof g_log) {
        s += n - (sizeof g_log - 1);
        n = sizeof g_log - 1;
    }
    if (cur + n >= sizeof g_log) {
        size_t drop = cur + n - (sizeof g_log - 1);
        while (drop < cur && ((unsigned char)g_log[drop] & 0xC0) == 0x80) drop++;
        memmove(g_log, g_log + drop, cur - drop + 1);
        cur -= drop;
    }
    memcpy(g_log + cur, s, n + 1);
}

/* ---------- dessin ---------- */

static const char *LOGO[] = {
    " ____  _   _ _   _ ____  ",
    "| __ )| | | | \\ | |  _ \\ ",
    "|  _ \\| | | |  \\| | | | |",
    "| |_) | |_| | |\\  | |_| |",
    "|____/ \\___/|_| \\_|____/ ",
};

static void draw_wrapped(int *y, int ymax, int x, int w, const char *text, rgb c) {
    ed_layout L;
    ed_layout_build_words(&L, text, strlen(text), w);
    for (size_t i = 0; i < L.n && *y < ymax; i++) {
        char seg[512];
        size_t n = L.rows[i].end - L.rows[i].start;
        if (n > sizeof seg - 1) n = sizeof seg - 1;
        memcpy(seg, text + L.rows[i].start, n);
        seg[n] = '\0';
        put(*y, x, w + 1, seg, c);
        (*y)++;
    }
    ed_layout_free(&L);
}

static int wrapped_rows(const char *t, int w) {
    ed_layout L;
    ed_layout_build_words(&L, t, strlen(t), w);
    int n = (int)L.n;
    ed_layout_free(&L);
    return n;
}

static void draw_welcome(int y0, int y1, int mainw) {
    static const char *HELLO = "Je suis Bund, votre agent de développement. Dites-moi ce que vous voulez faire :";
    static const char *GHWARN = "GitHub : tape /login pour te connecter. Les commits seront signés « Bund ».";
    int y = y0, avail = y1 - y0;
    int logo_h = (int)(sizeof LOGO / sizeof LOGO[0]);
    int w = mainw - 4;
    int qc = (int)comp_quick_count();
    /* hauteur du contenu sans logo : texte, liste, aide, éventuel avertissement GitHub */
    int need = wrapped_rows(HELLO, w) + 1 + qc + 1 + (g_side.github ? 0 : 1 + wrapped_rows(GHWARN, w));
    if (avail >= need + logo_h + 4 && mainw >= 30) {
        for (int i = 0; i < logo_h; i++) put(y + i, 2, mainw - 3, LOGO[i], C_ACC);
        y += logo_h;
        put(y++, 2, mainw - 3, "AI CODING AGENT", C_ACC);
        put(y++, 2, mainw - 3, "Code. Build. Automate.", C_DIM);
        y++;
    }
    if (y >= y1) return;
    draw_wrapped(&y, y1, 2, w, HELLO, C_TXT);
    y++;
    for (int i = 0; i < qc && y < y1; i++) {
        char l[96];
        snprintf(l, sizeof l, "%s %s", g_utf8 ? "•" : "-", comp_quick_label((size_t)i));
        put(y++, 3, mainw - 5, l, C_BLUE);
    }
    if (y < y1) put(y++, 3, mainw - 5, mainw >= 56 ? "Tab : insérer une suggestion · /help : commandes" : "Tab : suggestions · /help", C_DIM);
    if (!g_side.github && y + 1 < y1) {
        y++;
        draw_wrapped(&y, y1, 2, w, GHWARN, C_WARN);
    }
}

static rgb line_color(const char *l) {
    if (STARTS(l, "✔")) return C_OK;
    if (STARTS(l, "› ") || STARTS(l, "> ")) return C_ACC;
    if (STARTS(l, "  →")) return C_BLUE;
    if (STARTS(l, "  ←") || STARTS(l, "· ")) return C_DIM;
    if (strstr(l, "erreur") || strstr(l, "refus") || STARTS(l, "[git") || STARTS(l, "bund:")) return C_ERR;
    return C_TXT;
}

/* Copie `src` dans `dst` en retirant ce qu'un terminal ne doit pas recevoir tel quel :
   séquences ANSI, retours chariot et contrôles ; les tabulations deviennent 4 espaces. */
static size_t append_clean(char *dst, size_t cap, size_t n, const char *src) {
    for (size_t i = 0; src[i] && n + 5 < cap; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == 0x1b) {                                   /* ESC [ ... lettre */
            if (src[i + 1] == '[') {
                i += 2;
                while (src[i] && !((unsigned char)src[i] >= 0x40 && (unsigned char)src[i] <= 0x7e)) i++;
            }
            continue;
        }
        if (ch == '\t') { memcpy(dst + n, "    ", 4); n += 4; continue; }
        if (ch == '\n' || ch >= 0x20) {
            if (ch != 0x7f) dst[n++] = (char)ch;
        }
    }
    dst[n] = '\0';
    return n;
}

static void draw_output(int y0, int y1, int mainw) {
    static char disp[LOG_MAX + LIVE_MAX + 512];
    size_t dn = append_clean(disp, sizeof disp, 0, g_log);
    if (g_busy) {
        dn = append_clean(disp, sizeof disp, dn, g_utf8 ? "› " : "> ");
        dn = append_clean(disp, sizeof disp, dn, g_cur_cmd);
        dn = append_clean(disp, sizeof disp, dn, "\n");
        append_clean(disp, sizeof disp, dn, g_live);
    }
    int w = mainw - 4;
    if (w < 4) w = 4;
    int avail = y1 - y0;
    if (avail <= 0) return;
    ed_layout L;
    ed_layout_build_words(&L, disp, strlen(disp), w);
    size_t total = L.n;
    if (total > 1 && L.rows[total - 1].start == L.rows[total - 1].end) total--;   /* \n final */
    size_t first = total > (size_t)avail ? total - (size_t)avail : 0;
    int y = y0;
    for (size_t i = first; i < total; i++, y++) {
        char seg[512];
        size_t n = L.rows[i].end - L.rows[i].start;
        if (n > sizeof seg - 1) n = sizeof seg - 1;
        memcpy(seg, disp + L.rows[i].start, n);
        seg[n] = '\0';
        put(y, 2, w + 1, seg, line_color(seg));
    }
    ed_layout_free(&L);
}

static void draw_popup(int box_top, int mainw, int y_min) {
    size_t n = g_ncomp;
    while (n > 0 && box_top - (int)n - 2 < y_min) n--;
    if (n == 0) return;
    int w = mainw - 2 < 64 ? mainw - 2 : 64;
    if (w < 16) return;
    int top = box_top - (int)n - 2;
    plain();
    char blank[160];
    int bw = w - 2 < (int)sizeof blank - 1 ? w - 2 : (int)sizeof blank - 1;
    memset(blank, ' ', (size_t)bw);
    blank[bw] = '\0';
    for (size_t i = 0; i < n + 2; i++) ncplane_putstr_yx(g_plane, top + (int)i, 2, blank);   /* efface le fond */
    box(top, 1, (int)n + 2, w, C_BORDER, NULL);
    int maxlw = 0;   /* colonne des descriptions alignée sur le plus long libellé */
    for (size_t i = 0; i < n; i++) {
        int lw = (int)ed_text_width(g_comp[i].label, strlen(g_comp[i].label));
        if (lw > maxlw) maxlw = lw;
    }
    for (size_t i = 0; i < n; i++) {
        int y = top + 1 + (int)i;
        int sel = (i == g_sel);
        if (sel) {
            bgc(C_SEL);
            fgc(C_TXT);
            ncplane_putstr_yx(g_plane, y, 2, blank);
        }
        put(y, 3, w - 4, g_comp[i].label, sel ? C_TXT : C_ACC);
        if (w - 6 - maxlw > 6) put(y, 3 + maxlw + 2, w - 6 - maxlw - 1, g_comp[i].desc, sel ? C_TXT : C_DIM);
        if (sel) ncplane_set_bg_default(g_plane);
    }
}

static void draw_side(int x0, int w, int rows) {
    int y = 1;
    char l[160];
    box(y, x0, 4, w, C_BORDER, "GitHub");
    put(y + 1, x0 + 2, w - 4, g_side.github ? "● Authentifié" : "○ Non connecté (/login)",
        g_side.github ? C_OK : C_WARN);
    put(y + 2, x0 + 2, w - 4, g_side.login[0] ? g_side.login : "commits signés Bund", C_DIM);
    y += 4;
    if (y + 4 < rows) {
        box(y, x0, 4, w, C_BORDER, "IA");
        snprintf(l, sizeof l, "%s %s", g_side.key_ok ? "●" : "○", g_cfg->provider);
        put(y + 1, x0 + 2, w - 4, l, g_side.key_ok ? C_OK : C_WARN);
        put(y + 2, x0 + 2, w - 4, g_side.key_ok ? g_cfg->model : "clé API absente",
            g_side.key_ok ? C_DIM : C_WARN);
        y += 4;
    }
    if (y + 4 < rows) {
        box(y, x0, 4, w, C_BORDER, "Git");
        if (g_side.branch[0]) {
            snprintf(l, sizeof l, "%s %s", g_utf8 ? "⎇" : "br:", g_side.branch);
            put(y + 1, x0 + 2, w - 4, l, C_ACC);
            snprintf(l, sizeof l, "%d fichier(s) modifié(s)", g_side.dirty);
            put(y + 2, x0 + 2, w - 4, l, g_side.dirty ? C_WARN : C_DIM);
        } else {
            put(y + 1, x0 + 2, w - 4, "pas un dépôt git", C_DIM);
        }
        y += 4;
    }
    if (y + 3 < rows) {
        int h = rows - 3 - y;
        if (h > g_side.ntree + 2) h = g_side.ntree + 2;
        if (h >= 3) {
            box(y, x0, h, w, C_BORDER, g_side.project[0] ? g_side.project : "Fichiers");
            for (int i = 0; i < h - 2 && i < g_side.ntree; i++) {
                size_t tl = strlen(g_side.tree[i]);
                int is_dir = tl > 0 && g_side.tree[i][tl - 1] == '/';
                put(y + 1 + i, x0 + 2, w - 4, g_side.tree[i], is_dir ? C_BLUE : C_TXT);
            }
        }
    }
}

static void draw_ui(const char *buf, size_t pos) {
    int rows, cols;
    if (!g_plane) return;
    plane_dims(&rows, &cols);
    plain();
    ncplane_erase(g_plane);
    if (rows < 6 || cols < 24) {      /* terminal minuscule : juste la saisie */
        put(rows - 1, 0, cols, buf, C_TXT);
        nc_render();
        return;
    }
    int wide = cols >= 100;
    int mainw = main_width(cols);

    /* En-tête */
    ncplane_set_styles(g_plane, NCSTYLE_BOLD);
    put(0, 1, mainw - 2, "BUND", C_ACC);
    ncplane_set_styles(g_plane, NCSTYLE_NONE);
    if (mainw > 44 && (g_log[0] || g_busy)) put(0, 7, mainw - 8, "AI CODING AGENT", C_DIM);
    {
        char r[128];
        const char *who = g_side.login[0] ? g_side.login : "GitHub";
        if (g_utf8) snprintf(r, sizeof r, "%s %s  v%s", g_side.github ? "●" : "○", who, BUND_VERSION);
        else snprintf(r, sizeof r, "%s  v%s", who, BUND_VERSION);
        putr(0, mainw - 1, r, g_side.github ? C_OK : C_DIM);
    }
    hline(1, 0, mainw, C_BORDER);

    /* Saisie : retour à la ligne automatique */
    int W = input_width(cols);
    size_t len = strlen(buf);
    ed_layout L;
    ed_layout_build(&L, buf, len, W);
    int maxer = rows >= 26 ? 6 : (rows >= 16 ? 4 : 2);
    int er = (int)L.n < maxer ? (int)L.n : maxer;
    if (er < 1) er = 1;
    int box_top = rows - 3 - er;
    if (box_top < 3) {
        er = 1;
        box_top = rows - 4;
    }
    int status_y = box_top - 1;

    size_t crow, ccol;
    ed_locate(&L, buf, pos, &crow, &ccol);
    g_first_row = ed_first_visible(crow, L.n, (size_t)er, g_first_row);

    /* Zone principale */
    if (g_log[0] == '\0' && !g_busy) draw_welcome(2, status_y, mainw);
    else draw_output(2, status_y, mainw);

    /* Ligne d'état */
    if (g_notice[0]) {
        put(status_y, 1, mainw - 2, g_notice, C_WARN);
    } else if (g_busy) {
        put(status_y, 1, mainw - 2, "En cours… (la sortie s'affiche au fil des étapes)", C_WARN);
    } else {
        char s[640];
        snprintf(s, sizeof s, "%s%s · %s/%s%s%s", g_multiline ? "MULTILIGNE · " : "", g_cfg->agent,
                 g_cfg->provider, g_cfg->model, g_side.branch[0] ? " · " : "", g_side.branch);
        put(status_y, 1, mainw - 2, s, g_multiline ? C_WARN : C_DIM);
    }

    /* Boîte de saisie */
    box(box_top, 0, er + 2, mainw, C_BLUE, g_busy ? "Agent" : "Saisie");
    if (g_first_row > 0) {
        char up[32];
        snprintf(up, sizeof up, "%s %zu", g_utf8 ? "↑" : "^", g_first_row);
        putr(box_top, mainw - 3, up, C_DIM);
    }
    for (int r = 0; r < er; r++) {
        size_t li = g_first_row + (size_t)r;
        if (li >= L.n) break;
        int y = box_top + 1 + r;
        if (li == 0) put(y, 2, 2, g_utf8 ? "›" : ">", C_ACC);
        size_t n = L.rows[li].end - L.rows[li].start;
        char seg[1024];
        if (n > sizeof seg - 1) n = sizeof seg - 1;
        memcpy(seg, buf + L.rows[li].start, n);
        seg[n] = '\0';
        put(y, 4, W + 1, seg, C_TXT);
    }
    /* Curseur : cellule en vidéo inversée (ne décale jamais le texte) */
    if (!g_busy && crow >= g_first_row && crow < g_first_row + (size_t)er) {
        int cy = box_top + 1 + (int)(crow - g_first_row), cx = 4 + (int)ccol;
        char cell[8] = " ";
        if (pos < len && buf[pos] != '\n') {
            size_t b;
            ed_cwidth(buf + pos, len - pos, &b);
            if (b > 0 && b < sizeof cell) {
                memcpy(cell, buf + pos, b);
                cell[b] = '\0';
            }
        }
        bgc(C_ACC);
        fgc(C_BLACK);
        ncplane_putstr_yx(g_plane, cy, cx, cell);
        ncplane_set_bg_default(g_plane);
    }

    /* Fenêtre d'autocomplétion, au-dessus de la saisie */
    if (g_ncomp > 0 && !g_busy) draw_popup(box_top, mainw, 2);

    /* Pied de page */
    {
        const char *hint;
        if (g_busy) hint = "Patiente : l'agent travaille";
        else if (g_multiline)
            hint = mainw >= 70 ? "Entrée : nouvelle ligne · Ctrl+S : envoyer · Ctrl+O : fin du mode multiligne"
                               : "Ctrl+S envoyer · Ctrl+O fin multiligne";
        else if (g_ncomp > 0)
            hint = mainw >= 52 ? "↑↓ choisir · Tab accepter · Entrée envoyer tel quel" : "↑↓ choisir · Tab accepter";
        else
            hint = mainw >= 62 ? "Entrée envoyer · Ctrl+O multiligne · Tab suggestions · /help"
                               : "Entrée · Tab · Ctrl+O multi · /help";
        put(rows - 1, 1, mainw - 2, hint, C_DIM);
    }

    if (wide) draw_side(mainw + 1, cols - mainw - 2, rows);
    ed_layout_free(&L);
    plain();
    nc_render();
}

/* Appelé par l'agent : affiche la sortie produite jusqu'ici. */
void tui_refresh(void) {
    if (!tui_is_active() || !g_busy) return;
    load_capture_tail();
    draw_ui("", 0);
}

int tui_confirm(const char *question) {
    if (!tui_is_active()) return 0;
    static char qclean[2048];
    append_clean(qclean, sizeof qclean, 0, question ? question : "Autoriser cette action ?");
    const char *q = qclean;
    int rows, cols;
    load_capture_tail();
    draw_ui("", 0);
    plane_dims(&rows, &cols);
    int area = main_width(cols);          /* centrée sur la zone principale, pas sous le panneau latéral */
    int w = area - 6 < 70 ? area - 6 : 70;
    if (w < 20) w = area - 2;
    ed_layout L;
    ed_layout_build_words(&L, q, strlen(q), w - 4);
    int lines = (int)L.n;
    if (lines > rows - 8) lines = rows - 8;
    if (lines < 1) lines = 1;
    int h = lines + 4, top = (rows - h) / 2, left = (area - w) / 2;
    char blank[256];
    int bw = w - 2 < (int)sizeof blank - 1 ? w - 2 : (int)sizeof blank - 1;
    memset(blank, ' ', (size_t)bw);
    blank[bw] = '\0';
    plain();
    for (int r = 0; r < h; r++) ncplane_putstr_yx(g_plane, top + r, left + 1, blank);
    box(top, left, h, w, C_WARN, "Confirmation requise");
    for (int i = 0; i < lines && (size_t)i < L.n; i++) {
        char seg[400];
        size_t n = L.rows[i].end - L.rows[i].start;
        if (n > sizeof seg - 1) n = sizeof seg - 1;
        memcpy(seg, q + L.rows[i].start, n);
        seg[n] = '\0';
        put(top + 1 + i, left + 2, w - 4, seg, C_TXT);
    }
    put(top + h - 2, left + 2, w - 4, "o = oui   ·   n / Échap = non", C_ACC);
    ed_layout_free(&L);
    plain();
    nc_render();
    int approved = 0;
    for (;;) {
        struct ncinput ni;
        uint32_t ch = notcurses_getc_blocking(g_nc, &ni);
        if (ch == 'o' || ch == 'O' || ch == 'y' || ch == 'Y') {
            approved = 1;
            break;
        }
        if (ch == 'n' || ch == 'N' || ch == 27 || ch == NCKEY_ENTER || ch == 3 || ch == 4) break;
    }
    draw_ui("", 0);
    return approved;
}

static void capture_command_output(bund_config *c, const char *line, int *done) {
    FILE *tmp = tmpfile();
    snprintf(g_cur_cmd, sizeof g_cur_cmd, "%s", line);
    g_live[0] = '\0';
    g_busy = 1;
    if (!tmp) {
        *done = dispatch(c, line);
        g_busy = 0;
        return;
    }
    fflush(stdout);
    fflush(stderr);
    int saved_out = dup(STDOUT_FILENO), saved_err = dup(STDERR_FILENO);
    if (saved_out < 0 || saved_err < 0 || dup2(fileno(tmp), STDOUT_FILENO) < 0 ||
        dup2(fileno(tmp), STDERR_FILENO) < 0) {
        if (saved_out >= 0) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }
        if (saved_err >= 0) { dup2(saved_err, STDERR_FILENO); close(saved_err); }
        fclose(tmp);
        *done = dispatch(c, line);
        g_busy = 0;
        return;
    }
    g_capture_terminal_fd = saved_out;
    g_cap_tmp = tmp;
    draw_ui("", 0);
    *done = dispatch(c, line);
    load_capture_tail();
    g_cap_tmp = NULL;
    g_capture_terminal_fd = -1;
    fflush(stdout);
    fflush(stderr);
    dup2(saved_out, STDOUT_FILENO);
    dup2(saved_err, STDERR_FILENO);
    close(saved_out);
    close(saved_err);
    fclose(tmp);
    g_busy = 0;
    /* La sortie terminée rejoint l'historique (sauf /clear, qui l'a vidé). */
    if (strncmp(line, "/clear", 6) != 0) {
        char head[300];
        snprintf(head, sizeof head, "%s%s\n", g_utf8 ? "› " : "> ", line);
        log_append(head);
        log_append(g_live);
        if (g_live[0] && g_live[strlen(g_live) - 1] != '\n') log_append("\n");
    }
    g_live[0] = '\0';
}

/* ---------- saisie ---------- */

static void update_completion(const char *buf, int text_changed) {
    if (g_multiline) {
        g_ncomp = 0;
        return;
    }
    g_ncomp = comp_suggest(buf, g_comp, sizeof g_comp / sizeof g_comp[0]);
    if (text_changed || g_sel >= g_ncomp) g_sel = 0;
}

static size_t line_start_of(const char *buf, size_t pos) {
    while (pos > 0 && buf[pos - 1] != '\n') pos--;
    return pos;
}

static size_t line_end_of(const char *buf, size_t len, size_t pos) {
    while (pos < len && buf[pos] != '\n') pos++;
    return pos;
}

static int read_command(char *buf, size_t cap) {
    static char scratch[INPUT_MAX];
    size_t len = 0, pos = 0;
    long hp = (long)hist_count;
    buf[0] = '\0';
    g_first_row = 0;
    g_notice[0] = '\0';
    update_completion(buf, 1);
    draw_ui(buf, pos);
    for (;;) {
        struct ncinput ni;
        uint32_t ch = notcurses_getc_blocking(g_nc, &ni);
        int changed = 0;
        g_notice[0] = '\0';

        if (ch == 19) break;                                   /* Ctrl+S : envoyer */
        if (ch == 15) {                                        /* Ctrl+O : multiligne */
            g_multiline = !g_multiline;
        } else if (ch == NCKEY_ENTER || ch == '\n' || ch == '\r') {
            if (!g_multiline) break;
            if (!ed_insert(buf, cap, &len, &pos, "\n", 1))
                snprintf(g_notice, sizeof g_notice, "Limite de saisie atteinte (%zu octets).", cap - 1);
            changed = 1;
        } else if (ch == 4) {                                  /* Ctrl+D */
            if (!len) return 0;
            continue;
        } else if (ch == 3) {                                  /* Ctrl+C : vider */
            len = pos = 0;
            buf[0] = '\0';
            changed = 1;
        } else if (ch == 12) {                                 /* Ctrl+L : effacer l'écran */
            g_log[0] = '\0';
        } else if (ch == 1 || ch == NCKEY_HOME) {
            pos = line_start_of(buf, pos);
        } else if (ch == 5 || ch == NCKEY_END) {
            pos = line_end_of(buf, len, pos);
        } else if (ch == NCKEY_LEFT) {
            pos = ed_prev(buf, pos);
        } else if (ch == NCKEY_RIGHT) {
            pos = ed_next(buf, len, pos);
        } else if (ch == NCKEY_BACKSPACE || ch == 127 || ch == 8) {
            changed = ed_delete_back(buf, &len, &pos) > 0;
        } else if (ch == NCKEY_DEL) {
            changed = ed_delete_fwd(buf, &len, pos) > 0;
        } else if (ch == NCKEY_UP || ch == NCKEY_DOWN || ch == 16 || ch == 14) {
            int up = (ch == NCKEY_UP || ch == 16);
            int arrow = (ch == NCKEY_UP || ch == NCKEY_DOWN);
            if (g_ncomp > 0 && arrow) {
                g_sel = up ? (g_sel + g_ncomp - 1) % g_ncomp : (g_sel + 1) % g_ncomp;
            } else {
                size_t np = pos;
                if (arrow) {   /* déplacement par ligne visuelle, même largeur que l'affichage */
                    int rows, cols;
                    plane_dims(&rows, &cols);
                    ed_layout Lv;
                    ed_layout_build(&Lv, buf, len, input_width(cols));
                    np = ed_move_rows(&Lv, buf, pos, up ? -1 : 1);
                    ed_layout_free(&Lv);
                }
                if (np != pos) {
                    pos = np;
                } else if (!g_multiline || len == 0) {
                    if (up && hp > 0) {
                        if (hp == (long)hist_count) snprintf(scratch, sizeof scratch, "%s", buf);
                        --hp;
                        snprintf(buf, cap, "%s", history[hp]);
                        len = pos = strlen(buf);
                        changed = 1;
                    } else if (!up && hp < (long)hist_count) {
                        ++hp;
                        snprintf(buf, cap, "%s", hp == (long)hist_count ? scratch : history[hp]);
                        len = pos = strlen(buf);
                        changed = 1;
                    }
                }
            }
        } else if (ch == '\t') {
            if (g_ncomp > 0) {
                snprintf(buf, cap, "%s", g_comp[g_sel].insert);
                len = pos = strlen(buf);
                changed = 1;
            } else if (len == 0) {
                snprintf(buf, cap, "%s", comp_quick_text(g_quick++));
                len = pos = strlen(buf);
                changed = 1;
            }
        } else if (ch == NCKEY_RESIZE) {
            /* simple redessin ci-dessous */
        } else if (ch >= 32 && ch != 127 && ch <= 0x10ffff) {
            char enc[5];
            size_t n = ed_encode(ch, enc);
            if (n) {
                if (!ed_insert(buf, cap, &len, &pos, enc, n))
                    snprintf(g_notice, sizeof g_notice, "Limite de saisie atteinte (%zu octets).", cap - 1);
                else
                    changed = 1;
            }
        }
        update_completion(buf, changed);
        draw_ui(buf, pos);
    }
    if (len) {
        if (!hist_count || strcmp(history[hist_count - 1], buf)) {
            if (hist_count == HIST_MAX) {
                memmove(history, history + 1, sizeof history[0] * (HIST_MAX - 1));
                --hist_count;
            }
            snprintf(history[hist_count++], sizeof history[0], "%s", buf);
        }
    }
    if (g_multiline) g_multiline = 0;
    g_ncomp = 0;
    return 1;
}

/* Supprime les messages de fermeture de Notcurses. */
static void stop_tui_quietly(void) {
    if (!g_nc) return;
    fflush(stdout);
    fflush(stderr);
    int saved_stdout = dup(STDOUT_FILENO);
    int saved_stderr = dup(STDERR_FILENO);
    int nullfd = open("/dev/null", O_WRONLY);
    if (nullfd >= 0) {
        dup2(nullfd, STDOUT_FILENO);
        dup2(nullfd, STDERR_FILENO);
    }
    notcurses_stop(g_nc);
    g_nc = NULL;
    g_plane = NULL;
    if (saved_stdout >= 0) { dup2(saved_stdout, STDOUT_FILENO); close(saved_stdout); }
    if (saved_stderr >= 0) { dup2(saved_stderr, STDERR_FILENO); close(saved_stderr); }
    if (nullfd >= 0) close(nullfd);
}
#else
int tui_is_active(void) { return 0; }
void tui_refresh(void) {}

int tui_confirm(const char *question) {
    (void)question;
    return 0;
}

static int read_command(char *buf, size_t cap) {
    printf("bund> ");
    if (g_multiline) puts("Saisis les lignes, puis une ligne seule « . » pour envoyer.");
    fflush(stdout);
    if (!fgets(buf, (int)cap, stdin)) return 0;
    size_t len = strlen(buf);
    if (len && buf[len - 1] == '\n') buf[--len] = '\0';
    if (!g_multiline) return 1;

    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        if (!strcmp(line, ".\n") || !strcmp(line, ".\r\n") || !strcmp(line, ".")) break;
        size_t n = strlen(line);
        if (len + n + 2 >= cap) {
            puts("Saisie multiligne trop longue; le texte est tronqué.");
            break;
        }
        buf[len++] = '\n';
        memcpy(buf + len, line, n);
        len += n;
        buf[len] = '\0';
        if (len && buf[len - 1] == '\n') buf[--len] = '\0';
    }
    g_multiline = 0;
    return 1;
}
#endif

#ifdef BUND_ASSUME_TTY
#define BUND_STDIN_IS_TTY() 1
#else
#define BUND_STDIN_IS_TTY() isatty(STDIN_FILENO)
#endif

int main(int argc, char **argv) {
    setlocale(LC_CTYPE, "");
    if (ensure_bund_dirs() != 0) {
        fprintf(stderr, "bund: impossible de créer ~/.bund\n");
        return 1;
    }
    bund_config c;
    config_load(&c);

    if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        printf("bund %s\n", BUND_VERSION);
        return 0;
    }
    if (argc > 1) {
        sbuf s;
        sb_init(&s);
        if (!strcmp(argv[1], "--help")) sb_add(&s, "/help");
        else
            for (int i = 1; i < argc; i++) {
                if (i > 1) sb_addc(&s, ' ');
                /* Préserve les arguments contenant des espaces (ex : -m "message"). */
                int needs_q = (argv[i][0] == '\0') || strchr(argv[i], ' ') != NULL;
                if (needs_q && s.data[0] == '/') {
                    sb_addc(&s, '"');
                    sb_add(&s, argv[i]);
                    sb_addc(&s, '"');
                } else {
                    sb_add(&s, argv[i]);
                }
            }
        int rc = (s.data[0] == '/') ? dispatch(&c, s.data) : agent_run(&c, s.data);
        sb_free(&s);
        if (argc > 1 && argv[1][0] == '/') return 0;
        return rc == 0 ? 0 : 1;
    }

#ifdef BUND_HAVE_NOTCURSES
    g_cfg = &c;
    if (BUND_STDIN_IS_TTY()) {
        struct notcurses_options opts = {0};
        opts.flags = NCOPTION_SUPPRESS_BANNERS;
        g_nc = notcurses_core_init(&opts, stdout);
        if (g_nc) g_plane = notcurses_stdplane(g_nc);
    }
    {
        const char *cs = nl_langinfo(CODESET);
        g_utf8 = cs && (strstr(cs, "UTF-8") || strstr(cs, "utf-8") || strstr(cs, "UTF8"));
#ifdef BUND_ASSUME_UTF8
        g_utf8 = 1;
#endif
    }
    if (tui_is_active()) refresh_side(&c);
#endif
    if (!tui_is_active()) {
        printf("BUND %s - agent de code\n", BUND_VERSION);
#ifdef BUND_HAVE_NOTCURSES
        puts("TUI Notcurses indisponible; mode texte activé. Tape /help pour les commandes.");
#else
        puts("Notcurses absent à la compilation; mode texte activé. Tape /help pour les commandes.");
#endif
    }
    static char line[INPUT_MAX];
    for (;;) {
        if (!read_command(line, sizeof line)) break;
        char *t = str_trim(line);
        if (!*t) continue;
        int done = 0;
#ifdef BUND_HAVE_NOTCURSES
        if (tui_is_active()) {
            capture_command_output(&c, t, &done);
            refresh_side(&c);
        } else {
            done = dispatch(&c, t);
        }
#else
        done = dispatch(&c, t);
#endif
        if (done) break;
    }
#ifdef BUND_HAVE_NOTCURSES
    if (tui_is_active()) stop_tui_quietly();
#endif
    return 0;
}
