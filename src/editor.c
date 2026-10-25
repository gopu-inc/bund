#include "editor.h"

#include <stdlib.h>
#include <string.h>

/* ---------- UTF-8 ---------- */

static int is_wide(uint32_t c) {
    return (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) ||
           (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60) ||
           (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
           (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x20000 && c <= 0x3FFFD);
}

static int is_zero_width(uint32_t c) {
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x200B && c <= 0x200F) ||
           (c >= 0xFE00 && c <= 0xFE0F) || c == 0x200D;
}

int ed_cwidth(const char *s, size_t remaining, size_t *bytes) {
    const unsigned char *p = (const unsigned char *)s;
    size_t need = 1;
    uint32_t cp = p[0];
    if (remaining == 0) { *bytes = 0; return 0; }
    if (p[0] >= 0xF0 && p[0] <= 0xF4) { need = 4; cp = p[0] & 0x07; }
    else if (p[0] >= 0xE0) { need = 3; cp = p[0] & 0x0F; }
    else if (p[0] >= 0xC2 && p[0] < 0xE0) { need = 2; cp = p[0] & 0x1F; }
    else if (p[0] >= 0x80) { *bytes = 1; return 1; }       /* octet invalide */
    if (need > remaining) { *bytes = 1; return 1; }
    for (size_t i = 1; i < need; i++) {
        if ((p[i] & 0xC0) != 0x80) { *bytes = 1; return 1; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *bytes = need;
    if (cp < 0x20 || cp == 0x7F) return 0;
    if (is_zero_width(cp)) return 0;
    return is_wide(cp) ? 2 : 1;
}

size_t ed_text_width(const char *s, size_t bytes) {
    size_t w = 0;
    for (size_t i = 0; i < bytes;) {
        size_t step;
        w += (size_t)ed_cwidth(s + i, bytes - i, &step);
        i += step ? step : 1;
    }
    return w;
}

size_t ed_encode(uint32_t cp, char out[5]) {
    if (cp <= 0x7F) { out[0] = (char)cp; return 1; }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF && !(cp >= 0xD800 && cp <= 0xDFFF)) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp >= 0x10000 && cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

size_t ed_prev(const char *s, size_t pos) {
    if (!pos) return 0;
    pos--;
    while (pos && (((unsigned char)s[pos] & 0xC0) == 0x80)) pos--;
    return pos;
}

size_t ed_next(const char *s, size_t len, size_t pos) {
    if (pos >= len) return len;
    pos++;
    while (pos < len && (((unsigned char)s[pos] & 0xC0) == 0x80)) pos++;
    return pos;
}

/* ---------- mise en page ---------- */

static void push_row(ed_layout *L, size_t a, size_t b) {
    if (L->n == L->cap) {
        size_t nc = L->cap ? L->cap * 2 : 16;
        ed_row *p = realloc(L->rows, nc * sizeof *p);
        if (!p) abort();
        L->rows = p;
        L->cap = nc;
    }
    L->rows[L->n].start = a;
    L->rows[L->n].end = b;
    L->n++;
}

void ed_layout_build(ed_layout *L, const char *buf, size_t len, int width) {
    L->rows = NULL;
    L->n = L->cap = 0;
    if (width < 1) width = 1;
    size_t row_start = 0, used = 0;
    for (size_t i = 0; i < len;) {
        if (buf[i] == '\n') {
            push_row(L, row_start, i);
            i++;
            row_start = i;
            used = 0;
            continue;
        }
        size_t b;
        size_t w = (size_t)ed_cwidth(buf + i, len - i, &b);
        if (!b) b = 1;
        if (w > (size_t)width) w = (size_t)width;
        if (used + w > (size_t)width && i > row_start) {
            push_row(L, row_start, i);
            row_start = i;
            used = 0;
        }
        used += w;
        i += b;
    }
    push_row(L, row_start, len);
    /* Dernière ligne exactement pleine : le curseur passe sur une ligne vide. */
    if (used == (size_t)width && len > row_start) push_row(L, len, len);
}

void ed_layout_build_words(ed_layout *L, const char *buf, size_t len, int width) {
    L->rows = NULL;
    L->n = L->cap = 0;
    if (width < 1) width = 1;
    size_t row_start = 0, used = 0;
    for (size_t i = 0; i < len;) {
        if (buf[i] == '\n') {
            push_row(L, row_start, i);
            i++;
            row_start = i;
            used = 0;
            continue;
        }
        size_t b;
        size_t w = (size_t)ed_cwidth(buf + i, len - i, &b);
        if (!b) b = 1;
        if (w > (size_t)width) w = (size_t)width;
        if (used + w > (size_t)width && i > row_start) {
            size_t brk = i;
            if (buf[i] != ' ') {                       /* le mot en cours déborde : revenir au dernier espace */
                size_t j = i;
                while (j > row_start && buf[j - 1] != ' ') j--;
                if (j > row_start) brk = j;            /* début du mot, juste après l'espace */
            }
            size_t end = brk;
            while (end > row_start && buf[end - 1] == ' ') end--;   /* l'espace de coupure n'est pas affiché */
            if (brk == i && buf[i] == ' ') {           /* l'espace lui-même tombe hors ligne */
                push_row(L, row_start, end);
                row_start = i + 1;
                used = 0;
                i++;
                continue;
            }
            push_row(L, row_start, end);
            row_start = brk;
            used = ed_text_width(buf + row_start, i - row_start);
        }
        used += w;
        i += b;
    }
    push_row(L, row_start, len);
}

void ed_layout_free(ed_layout *L) {
    free(L->rows);
    L->rows = NULL;
    L->n = L->cap = 0;
}

void ed_locate(const ed_layout *L, const char *buf, size_t pos, size_t *row, size_t *col) {
    size_t r = 0;
    for (size_t i = 0; i < L->n; i++)
        if (L->rows[i].start <= pos) r = i;
    size_t end = pos < L->rows[r].end ? pos : L->rows[r].end;
    *row = r;
    *col = ed_text_width(buf + L->rows[r].start, end - L->rows[r].start);
}

size_t ed_move_rows(const ed_layout *L, const char *buf, size_t pos, int dir) {
    size_t r, col;
    ed_locate(L, buf, pos, &r, &col);
    if (dir < 0 && r == 0) return pos;
    if (dir > 0 && r + 1 >= L->n) return pos;
    size_t t = dir < 0 ? r - 1 : r + 1;
    const ed_row *row = &L->rows[t];
    size_t i = row->start, w = 0;
    while (i < row->end) {
        size_t b;
        size_t cw = (size_t)ed_cwidth(buf + i, row->end - i, &b);
        if (!b) b = 1;
        if (w + cw > col) break;
        w += cw;
        i += b;
    }
    return i;
}

size_t ed_first_visible(size_t cursor_row, size_t total_rows, size_t max_rows, size_t prev_first) {
    if (max_rows == 0) return 0;
    size_t first = prev_first;
    if (cursor_row < first) first = cursor_row;
    if (cursor_row >= first + max_rows) first = cursor_row - max_rows + 1;
    if (total_rows > max_rows && first > total_rows - max_rows) first = total_rows - max_rows;
    if (total_rows <= max_rows) first = 0;
    return first;
}

/* ---------- modifications ---------- */

size_t ed_insert(char *buf, size_t cap, size_t *len, size_t *pos, const char *s, size_t n) {
    if (!n || *len + n + 1 > cap) return 0;
    memmove(buf + *pos + n, buf + *pos, *len - *pos + 1);
    memcpy(buf + *pos, s, n);
    *pos += n;
    *len += n;
    return n;
}

size_t ed_delete_back(char *buf, size_t *len, size_t *pos) {
    if (!*pos) return 0;
    size_t prev = ed_prev(buf, *pos);
    size_t n = *pos - prev;
    memmove(buf + prev, buf + *pos, *len - *pos + 1);
    *len -= n;
    *pos = prev;
    return n;
}

size_t ed_delete_fwd(char *buf, size_t *len, size_t pos) {
    if (pos >= *len) return 0;
    size_t next = ed_next(buf, *len, pos);
    size_t n = next - pos;
    memmove(buf + pos, buf + next, *len - next + 1);
    *len -= n;
    return n;
}
