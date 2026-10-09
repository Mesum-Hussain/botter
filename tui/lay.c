/* Text layout for the botter TUI: wrapping, markdown, code highlighting, tables, logo. */
#define _GNU_SOURCE
#include "tui.h"
#include "logo.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const ttheme_t THEME_DARK = {
    .bg = TC_NONE, .surface = 0x5C7057, .muted = 0x89A482, .text = 0xE4EEE2,
    .accent = 0xACC5A6, .err = 0xD98C80, .ok = 0xACC5A6, .info = 0xB9D3C9, .purple = 0xC2B8A3,
    .code_bg = 0x222A21, .user_bg = 0x283126,
    .kw = 0xD9A57E, .str = 0xACC5A6, .num = 0x9DBBCF, .com = 0x72836D, .fn = 0xD1EDD3,
    .in_label = 0xACC5A6, .out_label = 0xD9A57E,
};
static const ttheme_t THEME_LIGHT = {
    .bg = TC_NONE, .surface = 0x89A482, .muted = 0x5C7057, .text = 0x27311F,
    .accent = 0x4A5E45, .err = 0xA8483C, .ok = 0x4E7A47, .info = 0x3F6A5E, .purple = 0x7A6A4F,
    .code_bg = 0xEEF4EC, .user_bg = 0xE6F2E5,
    .kw = 0x9A5A30, .str = 0x4E7A47, .num = 0x3D6787, .com = 0x8A9786, .fn = 0x2E4A2A,
    .in_label = 0x4A5E45, .out_label = 0x9A5A30,
};

ttheme_t T = THEME_DARK;

void theme_init(int light)
{
    T = light ? THEME_LIGHT : THEME_DARK;
}

/* ---------------- string builder ---------------- */

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        abort();
    }
    return q;
}

void tsb_add(tsb_t *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->p = xrealloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

void tsb_str(tsb_t *b, const char *s) { tsb_add(b, s, strlen(s)); }

void tsb_fmt(tsb_t *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        tsb_add(b, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
    }
}

void tsb_free(tsb_t *b)
{
    free(b->p);
    memset(b, 0, sizeof(*b));
}

/* ---------------- UTF-8 ---------------- */

uint32_t tu_decode(const char *s, size_t n, int *len)
{
    const unsigned char *u = (const unsigned char *)s;
    uint32_t c = u[0];
    int l = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (l == 0 || (size_t)l > n) {
        *len = 1;
        return c < 0x80 ? c : 0xFFFD;
    }
    if (l == 1) {
        *len = 1;
        return c;
    }
    c &= 0x7F >> l;
    for (int i = 1; i < l; i++) {
        if ((u[i] & 0xC0) != 0x80) {
            *len = 1;
            return 0xFFFD;
        }
        c = (c << 6) | (u[i] & 0x3F);
    }
    *len = l;
    return c;
}

int tu_width(uint32_t c)
{
    if (c == 0 || c < 0x20 || (c >= 0x7F && c < 0xA0)) {
        return 0;
    }
    if ((c >= 0x300 && c <= 0x36F) || (c >= 0x200B && c <= 0x200F) || (c >= 0xFE00 && c <= 0xFE0F) ||
        (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x20D0 && c <= 0x20FF) || c == 0x200D) {
        return 0;
    }
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) ||
        (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) ||
        (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
        (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
        (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x1F680 && c <= 0x1F6FF) || (c >= 0x20000 && c <= 0x3FFFD)) {
        return 2;
    }
    return 1;
}

int tu_strwidth(const char *s, size_t n)
{
    int w = 0, l;
    for (size_t i = 0; i < n; i += (size_t)l) {
        w += tu_width(tu_decode(s + i, n - i, &l));
    }
    return w;
}

/* ---------------- styles and lines ---------------- */

tsty_t sty(uint32_t fg, uint8_t at) { return (tsty_t){fg, TC_NONE, at}; }
tsty_t sty_bg(uint32_t fg, uint32_t bg, uint8_t at) { return (tsty_t){fg, bg, at}; }
int sty_eq(tsty_t a, tsty_t b) { return a.fg == b.fg && a.bg == b.bg && a.at == b.at; }

void tl_free(tlines_t *L)
{
    for (int i = 0; i < L->n; i++) {
        for (int j = 0; j < L->l[i].n; j++) {
            free(L->l[i].r[j].t);
        }
        free(L->l[i].r);
    }
    free(L->l);
    memset(L, 0, sizeof(*L));
}

tline_t *tl_new(tlines_t *L, uint32_t fill)
{
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 16;
        L->l = xrealloc(L->l, sizeof(*L->l) * (size_t)L->cap);
    }
    tline_t *l = &L->l[L->n++];
    memset(l, 0, sizeof(*l));
    l->fill = fill;
    return l;
}

static void run_push(tline_t *l, tsty_t s, const char *t, size_t n, int w)
{
    if (n == 0) {
        return;
    }
    if (l->n > 0 && sty_eq(l->r[l->n - 1].s, s)) {
        trun_t *r = &l->r[l->n - 1];
        size_t ol = strlen(r->t);
        r->t = xrealloc(r->t, ol + n + 1);
        memcpy(r->t + ol, t, n);
        r->t[ol + n] = '\0';
        r->w += w;
    } else {
        if (l->n == l->cap) {
            l->cap = l->cap ? l->cap * 2 : 4;
            l->r = xrealloc(l->r, sizeof(*l->r) * (size_t)l->cap);
        }
        trun_t *r = &l->r[l->n++];
        r->s = s;
        r->t = strndup(t, n);
        if (!r->t) {
            abort();
        }
        r->w = w;
    }
    l->w += w;
}

void tl_put(tline_t *l, tsty_t s, const char *t, size_t n)
{
    run_push(l, s, t, n, tu_strwidth(t, n));
}

void tl_put_trunc(tline_t *l, tsty_t s, const char *t, int maxw)
{
    size_t n = strlen(t);
    if (maxw <= 0) {
        return;
    }
    if (tu_strwidth(t, n) <= maxw) {
        tl_put(l, s, t, n);
        return;
    }
    int w = 0, cl;
    size_t i = 0;
    while (i < n) {
        int cw = tu_width(tu_decode(t + i, n - i, &cl));
        if (w + cw > maxw - 1) {
            break;
        }
        w += cw;
        i += (size_t)cl;
    }
    tl_put(l, s, t, i);
    tl_put(l, s, "…", strlen("…"));
}

void tl_pad(tline_t *l, int w, tsty_t s)
{
    static const char sp[] = "                                ";
    while (l->w < w) {
        int k = w - l->w < 32 ? w - l->w : 32;
        run_push(l, s, sp, (size_t)k, k);
    }
}

void tl_cat(tline_t *dst, const tline_t *src)
{
    for (int i = 0; i < src->n; i++) {
        run_push(dst, src->r[i].s, src->r[i].t, strlen(src->r[i].t), src->r[i].w);
    }
}

void tl_blank(tlines_t *L) { tl_new(L, TC_NONE); }

void tl_prefix(tlines_t *L, int from, tsty_t s1, const char *first, tsty_t s2, const char *rest)
{
    for (int i = from; i < L->n; i++) {
        tline_t *l = &L->l[i];
        const char *p = i == from ? first : rest;
        tsty_t s = i == from ? s1 : s2;
        size_t n = strlen(p);
        if (!n) {
            continue;
        }
        if (l->n == l->cap) {
            l->cap = l->cap ? l->cap * 2 : 4;
            l->r = xrealloc(l->r, sizeof(*l->r) * (size_t)l->cap);
        }
        memmove(l->r + 1, l->r, sizeof(*l->r) * (size_t)l->n);
        l->r[0].s = s;
        l->r[0].t = strdup(p);
        l->r[0].w = tu_strwidth(p, n);
        l->n++;
        l->w += l->r[0].w;
    }
}

void tl_move(tlines_t *dst, tlines_t *src)
{
    for (int i = 0; i < src->n; i++) {
        tline_t *l = tl_new(dst, 0);
        *l = src->l[i];
    }
    free(src->l);
    memset(src, 0, sizeof(*src));
}

/* ---------------- cells + wrapping ---------------- */

typedef struct {
    uint32_t off; /* into tcells.text */
    uint8_t  len;
    int8_t   w;   /* -1 = hard line break */
    uint8_t  space;
    tsty_t   s;
} cell_t;

struct tcells {
    cell_t *c;
    int     n, cap;
    tsb_t   text;
};

tcells_t *tc_new(void)
{
    tcells_t *c = calloc(1, sizeof(*c));
    if (!c) {
        abort();
    }
    return c;
}

void tc_free(tcells_t *c)
{
    if (c) {
        free(c->c);
        tsb_free(&c->text);
        free(c);
    }
}

static void cell_push(tcells_t *c, const char *s, int len, int w, int space, tsty_t st)
{
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 64;
        c->c = xrealloc(c->c, sizeof(*c->c) * (size_t)c->cap);
    }
    cell_t *e = &c->c[c->n++];
    e->off = (uint32_t)c->text.len;
    e->len = (uint8_t)len;
    e->w = (int8_t)w;
    e->space = (uint8_t)space;
    e->s = st;
    tsb_add(&c->text, s, (size_t)len);
}

void tc_add(tcells_t *c, const char *s, size_t n, tsty_t st)
{
    int col = 0; /* for tab stops, approximate (since last hard break in this call) */
    for (size_t i = 0; i < n;) {
        int l;
        uint32_t cp = tu_decode(s + i, n - i, &l);
        if (cp == '\n') {
            cell_push(c, "", 0, -1, 0, st);
            col = 0;
        } else if (cp == '\t') {
            int k = 4 - (col % 4);
            for (int j = 0; j < k; j++) {
                cell_push(c, " ", 1, 1, 1, st);
            }
            col += k;
        } else if (cp >= 0x20 && cp != 0x7F && !(cp >= 0x80 && cp < 0xA0)) {
            int w = tu_width(cp);
            cell_push(c, s + i, l, w, cp == ' ', st);
            col += w;
        }
        i += (size_t)l;
    }
}

int tc_width(const tcells_t *c)
{
    int best = 0, w = 0;
    for (int i = 0; i < c->n; i++) {
        if (c->c[i].w < 0) {
            w = 0;
        } else {
            w += c->c[i].w;
            if (w > best) {
                best = w;
            }
        }
    }
    return best;
}

static void emit_cells(tcells_t *c, tlines_t *L, int a, int b, uint32_t fill, int trim)
{
    tline_t *l = tl_new(L, fill);
    if (trim) {
        while (b > a && c->c[b - 1].space) {
            b--;
        }
    }
    for (int i = a; i < b; i++) {
        cell_t *e = &c->c[i];
        run_push(l, e->s, c->text.p + e->off, e->len, e->w);
    }
}

void tc_flow(tcells_t *c, tlines_t *L, int width, uint32_t fill, int word)
{
    if (width < 1) {
        width = 1;
    }
    int a = 0;
    for (;;) {
        int b = a;
        while (b < c->n && c->c[b].w >= 0) {
            b++;
        }
        /* hard line [a, b) */
        if (a == b) {
            tl_new(L, fill);
        }
        int start = a;
        while (start < b) {
            if (start != a && word) {
                while (start < b && c->c[start].space) {
                    start++;
                }
                if (start >= b) {
                    break;
                }
            }
            int w = 0, j = start, lastbrk = -1;
            while (j < b && w + c->c[j].w <= width) {
                if (c->c[j].space && j > start) {
                    lastbrk = j;
                }
                w += c->c[j].w;
                j++;
            }
            int end;
            if (j >= b) {
                end = b;
            } else if (word && c->c[j].space) {
                end = j;
            } else if (word && lastbrk > start) {
                end = lastbrk;
            } else {
                end = j > start ? j : start + 1;
            }
            emit_cells(c, L, start, end, fill, word && end < b);
            start = end;
        }
        if (b >= c->n) {
            break;
        }
        a = b + 1;
    }
}

void lay_text(tlines_t *L, const char *text, size_t n, tsty_t s, int width, uint32_t fill)
{
    tcells_t *c = tc_new();
    while (n > 0 && text[n - 1] == '\n') {
        n--;
    }
    tc_add(c, text, n, s);
    tc_flow(c, L, width, fill, 1);
    tc_free(c);
}

/* ---------------- inline markdown ---------------- */

static int is_punct(char ch) { return ch && ispunct((unsigned char)ch); }
static int is_alnum(char ch) { return ch && (isalnum((unsigned char)ch) || (unsigned char)ch >= 0x80); }

/* Closing run of `ch` (exactly k long, or the tail of a longer '*' run) in s[i..n); not after a space. */
static size_t find_closer(const char *s, size_t i, size_t n, char ch, size_t k)
{
    for (size_t j = i; j < n; j++) {
        if (s[j] == '\\') {
            j++;
            continue;
        }
        if (s[j] == '`') { /* skip code spans */
            const char *e = memchr(s + j + 1, '`', n - j - 1);
            if (e) {
                j = (size_t)(e - s);
            }
            continue;
        }
        if (s[j] != ch) {
            continue;
        }
        size_t r = 0;
        while (j + r < n && s[j + r] == ch) {
            r++;
        }
        if (j > i && s[j - 1] != ' ' && (r == k || (r > k && ch == '*'))) {
            size_t at = j + r - k;
            if (ch == '_' && at + k < n && is_alnum(s[at + k])) {
                j += r - 1;
                continue;
            }
            return at;
        }
        j += r - 1;
    }
    return (size_t)-1;
}

static void md_inline(tcells_t *c, const char *s, size_t n, tsty_t base)
{
    size_t i = 0, lit = 0; /* lit = start of pending literal text */
#define FLUSH()                                                                                                   \
    do {                                                                                                          \
        if (i > lit) {                                                                                            \
            tc_add(c, s + lit, i - lit, base);                                                                    \
        }                                                                                                         \
    } while (0)
    while (i < n) {
        char ch = s[i];
        if (ch == '\\' && i + 1 < n && is_punct(s[i + 1])) {
            FLUSH();
            tc_add(c, s + i + 1, 1, base);
            i += 2;
            lit = i;
            continue;
        }
        if (ch == '`') {
            size_t k = 0;
            while (i + k < n && s[i + k] == '`') {
                k++;
            }
            size_t j = i + k, end = (size_t)-1;
            while (j + k <= n) {
                if (s[j] == '`') {
                    size_t r = 0;
                    while (j + r < n && s[j + r] == '`') {
                        r++;
                    }
                    if (r == k) {
                        end = j;
                        break;
                    }
                    j += r;
                } else {
                    j++;
                }
            }
            if (end != (size_t)-1) {
                FLUSH();
                size_t a = i + k, b = end;
                if (b - a >= 2 && s[a] == ' ' && s[b - 1] == ' ') {
                    a++;
                    b--;
                }
                tsty_t cs = sty_bg(T.info, T.code_bg, base.at & TA_BOLD);
                tc_add(c, " ", 1, cs);
                tc_add(c, s + a, b - a, cs);
                tc_add(c, " ", 1, cs);
                i = end + k;
                lit = i;
                continue;
            }
            i += k;
            continue;
        }
        if ((ch == '*' || ch == '_' || ch == '~') && i + 1 < n) {
            size_t k = 0;
            while (i + k < n && s[i + k] == ch && k < 3) {
                k++;
            }
            int ok = i + k < n && s[i + k] != ' ' && s[i + k] != '\n';
            if (ch == '_' && i > 0 && is_alnum(s[i - 1])) {
                ok = 0;
            }
            if (ch == '~' && k != 2) {
                ok = 0;
            }
            if (ok) {
                size_t j = find_closer(s, i + k, n, ch, k);
                if (j != (size_t)-1) {
                    FLUSH();
                    tsty_t in = base;
                    if (ch == '~') {
                        in.at |= TA_STRIKE;
                    } else {
                        in.at |= k == 1 ? TA_ITALIC : k == 2 ? TA_BOLD : (TA_BOLD | TA_ITALIC);
                    }
                    md_inline(c, s + i + k, j - i - k, in);
                    i = j + k;
                    lit = i;
                    continue;
                }
            }
            i += k;
            continue;
        }
        if (ch == '[') {
            const char *rb = memchr(s + i, ']', n - i);
            if (rb && (size_t)(rb - s) + 1 < n && rb[1] == '(') {
                const char *rp = memchr(rb, ')', n - (size_t)(rb - s));
                if (rp) {
                    FLUSH();
                    size_t ta = i + 1, tb = (size_t)(rb - s);
                    size_t ua = tb + 2, ub = (size_t)(rp - s);
                    tsty_t ls = base;
                    ls.fg = T.info;
                    ls.at |= TA_UNDER;
                    md_inline(c, s + ta, tb - ta, ls);
                    if (ub > ua && (ub - ua != tb - ta || memcmp(s + ua, s + ta, ub - ua) != 0)) {
                        tc_add(c, " (", 2, sty(T.muted, 0));
                        tc_add(c, s + ua, ub - ua, sty(T.muted, 0));
                        tc_add(c, ")", 1, sty(T.muted, 0));
                    }
                    i = ub + 1;
                    lit = i;
                    continue;
                }
            }
        }
        if (ch == '<' && (n - i > 8) && (strncmp(s + i + 1, "http://", 7) == 0 || strncmp(s + i + 1, "https://", 8) == 0)) {
            const char *gt = memchr(s + i, '>', n - i);
            if (gt) {
                FLUSH();
                tsty_t ls = base;
                ls.fg = T.info;
                ls.at |= TA_UNDER;
                tc_add(c, s + i + 1, (size_t)(gt - s) - i - 1, ls);
                i = (size_t)(gt - s) + 1;
                lit = i;
                continue;
            }
        }
        i++;
    }
    FLUSH();
#undef FLUSH
}

static void lay_inline(tlines_t *L, const char *s, size_t n, tsty_t base, int width, uint32_t fill)
{
    tcells_t *c = tc_new();
    md_inline(c, s, n, base);
    tc_flow(c, L, width, fill, 1);
    tc_free(c);
}

/* ---------------- code highlighting ---------------- */

enum { LG_NONE, LG_C, LG_HASH, LG_DASH, LG_DIFF, LG_JSON, LG_MD, LG_OML };

static int lang_of(const char *l, size_t n)
{
    char b[24];
    if (n >= sizeof(b)) {
        n = sizeof(b) - 1;
    }
    for (size_t i = 0; i < n; i++) {
        b[i] = (char)tolower((unsigned char)l[i]);
    }
    b[n] = '\0';
    static const char *const cl[] = {"c", "h", "cpp", "c++", "cc", "hpp", "js", "javascript", "ts", "typescript",
                                     "jsx", "tsx", "java", "go", "rust", "rs", "swift", "kotlin", "kt", "cs",
                                     "csharp", "php", "css", "scss", "scala", "dart", "zig", "proto", NULL};
    static const char *const hl[] = {"py", "python", "sh", "bash", "zsh", "shell", "console", "rb", "ruby", "yaml",
                                     "yml", "toml", "make", "makefile", "dockerfile", "r", "perl", "pl", "ini",
                                     "conf", "nix", "elixir", "ex", "fish", "ps1", "powershell", NULL};
    static const char *const dl[] = {"sql", "lua", "haskell", "hs", "elm", NULL};
    for (int i = 0; cl[i]; i++) {
        if (!strcmp(b, cl[i])) {
            return LG_C;
        }
    }
    for (int i = 0; hl[i]; i++) {
        if (!strcmp(b, hl[i])) {
            return LG_HASH;
        }
    }
    for (int i = 0; dl[i]; i++) {
        if (!strcmp(b, dl[i])) {
            return LG_DASH;
        }
    }
    if (!strcmp(b, "diff") || !strcmp(b, "patch")) {
        return LG_DIFF;
    }
    if (!strcmp(b, "json") || !strcmp(b, "jsonc") || !strcmp(b, "json5")) {
        return LG_JSON;
    }
    if (!strcmp(b, "md") || !strcmp(b, "markdown")) {
        return LG_MD;
    }
    if (!strcmp(b, "oml")) {
        return LG_OML;
    }
    return LG_NONE;
}

static const char *const KEYWORDS[] = {
    "if", "else", "for", "while", "do", "return", "break", "continue", "switch", "case", "default", "goto",
    "struct", "union", "enum", "typedef", "static", "const", "extern", "void", "int", "char", "long", "short",
    "unsigned", "signed", "float", "double", "bool", "true", "false", "null", "NULL", "nullptr", "sizeof",
    "def", "class", "import", "from", "as", "with", "try", "except", "finally", "raise", "lambda", "yield",
    "pass", "in", "is", "not", "and", "or", "None", "True", "False", "self", "async", "await", "let", "var",
    "function", "new", "this", "typeof", "instanceof", "export", "package", "func", "go", "chan", "map",
    "range", "defer", "type", "interface", "fn", "impl", "pub", "mod", "use", "match", "mut", "ref", "trait",
    "where", "loop", "echo", "fi", "then", "elif", "esac", "done", "local", "readonly", "catch", "throw",
    "throws", "public", "private", "protected", "final", "abstract", "extends", "implements", "select",
    "insert", "update", "delete", "create", "table", "values", "into", "SELECT", "FROM", "WHERE", "INSERT",
    "UPDATE", "DELETE", "CREATE", "TABLE", "VALUES", "INTO", "JOIN", "ON", "AND", "OR", "NOT", "inline",
    "volatile", "register", "auto", "of", "end", "nil", "elseif", "unless", "until", "begin", "module",
    "require", "include", "#include", "#define", "#if", "#ifdef", "#ifndef", "#endif", "#else", "#pragma",
    "string", "str", "u8", "u16", "u32", "u64", "i32", "i64", "usize", "size_t", "uint32_t", "uint64_t",
    "int32_t", "int64_t", "uint8_t", "global", "nonlocal", "assert", "del", "print", "export", "source", NULL};

static int is_kw(const char *s, size_t n)
{
    for (int i = 0; KEYWORDS[i]; i++) {
        if (strlen(KEYWORDS[i]) == n && memcmp(KEYWORDS[i], s, n) == 0) {
            return 1;
        }
    }
    return 0;
}

static int id_char(char ch) { return isalnum((unsigned char)ch) || ch == '_' || (unsigned char)ch >= 0x80; }

/*
 * OML v2 (botter flow.md): UPPERCASE keywords (EXECUTE, IF ... THEN, END FOR,
 * IS EQUAL TO ...), `variables`, "strings", numbers, list markers, STEP
 * headings and frontmatter. Everything else is plain English.
 */
static int oml_kw(const char *s, size_t n)
{
    static const char *const KW[] = {"LOAD", "SKILL", "FROM", "EXECUTE", "INVOKE", "USING", "SAVE", "INTO", "VARIABLE",
                                     "TO", "FILE", "SET", "ASK", "USER", "IF", "THEN", "ELSE", "END", "FOR", "EACH",
                                     "IN", "DO", "WHILE", "AT", "MOST", "TIMES", "RETRY", "UP", "PARALLEL", "RETURN",
                                     "IS", "EQUAL", "NOT", "GREATER", "LESS", "THAN", "EMPTY", "CONTAINS", "AND", "OR",
                                     "CONNECT", "AS", NULL};
    for (int k = 0; KW[k]; k++) {
        if (strlen(KW[k]) == n && strncmp(KW[k], s, n) == 0) {
            return 1;
        }
    }
    return 0;
}

static void oml_line(tcells_t *c, const char *s, size_t n)
{
    uint32_t B = T.code_bg;
    tsty_t plain = sty_bg(T.text, B, 0), kw = sty_bg(T.kw, B, TA_BOLD), str = sty_bg(T.str, B, 0),
           num = sty_bg(T.num, B, 0), head = sty_bg(T.fn, B, TA_BOLD), var = sty_bg(T.info, B, 0),
           mut = sty_bg(T.muted, B, 0);
    size_t i = 0;
    while (i < n && s[i] == ' ') {
        i++;
    }
    tc_add(c, s, i, plain);
    if (i < n && (s[i] == '#' || (n - i >= 3 && !strncmp(s + i, "---", 3)))) { /* heading / frontmatter fence */
        tc_add(c, s + i, n - i, s[i] == '#' ? head : mut);
        return;
    }
    size_t j = i;
    while (j < n && isdigit((unsigned char)s[j])) {
        j++;
    }
    if (j > i && j < n && s[j] == '.') { /* "1." list marker */
        tc_add(c, s + i, j + 1 - i, mut);
        i = j + 1;
    } else if (i + 1 < n && s[i] == '-' && s[i + 1] == ' ') {
        tc_add(c, s + i, 1, mut);
        i++;
    }
    while (i < n) {
        char ch = s[i];
        if (ch == '"') {
            size_t e = i + 1;
            while (e < n && s[e] != '"') {
                e += s[e] == '\\' && e + 1 < n ? 2 : 1;
            }
            e = e < n ? e + 1 : n;
            tc_add(c, s + i, e - i, str);
            i = e;
        } else if (ch == '`') {
            size_t e = i + 1;
            while (e < n && s[e] != '`') {
                e++;
            }
            e = e < n ? e + 1 : n;
            tc_add(c, s + i, e - i, var);
            i = e;
        } else if (isdigit((unsigned char)ch) && (i == 0 || !id_char(s[i - 1]))) {
            size_t e = i;
            while (e < n && isdigit((unsigned char)s[e])) {
                e++;
            }
            tc_add(c, s + i, e - i, num);
            i = e;
        } else if (id_char(ch)) {
            size_t e = i;
            while (e < n && id_char(s[e])) {
                e++;
            }
            int k = oml_kw(s + i, e - i) || (e - i == 4 && !strncmp(s + i, "tool", 4) && i >= 8 &&
                                             !strncmp(s + i - 8, "EXECUTE ", 8));
            tc_add(c, s + i, e - i, k ? kw : plain);
            i = e;
        } else {
            int l;
            tu_decode(s + i, n - i, &l);
            tc_add(c, s + i, (size_t)l, plain);
            i += (size_t)l;
        }
    }
}

/* One source line -> cells. *blk: inside a C block comment. */
static void code_line(tcells_t *c, const char *s, size_t n, int lang, int *blk)
{
    uint32_t B = T.code_bg;
    tsty_t plain = sty_bg(T.text, B, 0), kw = sty_bg(T.kw, B, 0), str = sty_bg(T.str, B, 0),
           num = sty_bg(T.num, B, 0), com = sty_bg(T.com, B, TA_ITALIC), fn = sty_bg(T.fn, B, 0);
    if (lang == LG_DIFF) {
        tsty_t d = plain;
        if (n && s[0] == '+') {
            d = sty_bg(T.ok, B, 0);
        } else if (n && s[0] == '-') {
            d = sty_bg(T.err, B, 0);
        } else if (n > 1 && s[0] == '@' && s[1] == '@') {
            d = sty_bg(T.info, B, 0);
        }
        tc_add(c, s, n, d);
        return;
    }
    if (lang == LG_NONE || lang == LG_MD) {
        tc_add(c, s, n, plain);
        return;
    }
    if (lang == LG_OML) {
        oml_line(c, s, n);
        return;
    }
    size_t i = 0;
    while (i < n) {
        if (*blk) {
            const char *e = NULL;
            for (size_t j = i; j + 1 < n; j++) {
                if (s[j] == '*' && s[j + 1] == '/') {
                    e = s + j;
                    break;
                }
            }
            size_t end = e ? (size_t)(e - s) + 2 : n;
            tc_add(c, s + i, end - i, com);
            i = end;
            if (e) {
                *blk = 0;
            }
            continue;
        }
        char ch = s[i];
        if ((lang == LG_C && ch == '/' && i + 1 < n && s[i + 1] == '/') ||
            (lang == LG_HASH && ch == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) ||
            (lang == LG_DASH && ch == '-' && i + 1 < n && s[i + 1] == '-')) {
            if (!(lang == LG_HASH && i + 1 < n && s[i + 1] == '!' && i == 0)) {
                tc_add(c, s + i, n - i, com);
                return;
            }
        }
        if (lang == LG_C && ch == '/' && i + 1 < n && s[i + 1] == '*') {
            *blk = 1;
            continue;
        }
        if (ch == '"' || ch == '\'' || (ch == '`' && lang == LG_C)) {
            size_t j = i + 1;
            while (j < n && s[j] != ch) {
                j += s[j] == '\\' ? 2 : 1;
            }
            if (j > n) {
                j = n;
            }
            size_t end = j < n ? j + 1 : n;
            int key = lang == LG_JSON && ch == '"';
            if (key) { /* JSON: keys in the function colour, values as strings */
                size_t k = end;
                while (k < n && s[k] == ' ') {
                    k++;
                }
                key = k < n && s[k] == ':';
            }
            tc_add(c, s + i, end - i, key ? fn : str);
            i = end;
            continue;
        }
        if (isdigit((unsigned char)ch) && (i == 0 || !id_char(s[i - 1]))) {
            size_t j = i;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '_')) {
                j++;
            }
            tc_add(c, s + i, j - i, num);
            i = j;
            continue;
        }
        if (id_char(ch) || (ch == '#' && lang == LG_C)) {
            size_t j = i + 1;
            while (j < n && id_char(s[j])) {
                j++;
            }
            size_t k = j;
            while (k < n && s[k] == ' ') {
                k++;
            }
            tsty_t st = is_kw(s + i, j - i) ? kw : (k < n && s[k] == '(') ? fn : plain;
            tc_add(c, s + i, j - i, st);
            i = j;
            continue;
        }
        int l;
        tu_decode(s + i, n - i, &l);
        tc_add(c, s + i, (size_t)l, plain);
        i += (size_t)l;
    }
}

static void lay_code(tlines_t *L, const char *lang, size_t langn, const char *const *lines, const size_t *lens,
                     int nl, int width)
{
    int lg = lang_of(lang, langn);
    tsty_t pad = sty_bg(T.text, T.code_bg, 0);
    tline_t *h = tl_new(L, T.code_bg);
    tl_put(h, pad, " ", 1);
    if (langn) {
        char lb[40];
        snprintf(lb, sizeof(lb), "%.*s", (int)(langn < 30 ? langn : 30), lang);
        tl_put(h, sty_bg(T.muted, T.code_bg, TA_ITALIC), lb, strlen(lb));
    }
    int blk = 0;
    for (int i = 0; i < nl; i++) {
        tcells_t *c = tc_new();
        code_line(c, lines[i], lens[i], lg, &blk);
        int from = L->n;
        tc_flow(c, L, width - 2, T.code_bg, 0);
        tc_free(c);
        tl_prefix(L, from, pad, " ", pad, " ");
    }
    tl_new(L, T.code_bg);
}

/* ---------------- tables ---------------- */

static int split_row(const char *s, size_t n, const char **cell, size_t *clen, int max)
{
    size_t a = 0, b = n;
    while (a < b && (s[a] == ' ' || s[a] == '\t')) {
        a++;
    }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) {
        b--;
    }
    if (a < b && s[a] == '|') {
        a++;
    }
    if (b > a && s[b - 1] == '|' && !(b >= 2 && s[b - 2] == '\\')) {
        b--;
    }
    int k = 0;
    size_t st = a;
    for (size_t i = a; i <= b && k < max; i++) {
        if (i == b || (s[i] == '|' && !(i > a && s[i - 1] == '\\'))) {
            size_t x = st, y = i;
            while (x < y && s[x] == ' ') {
                x++;
            }
            while (y > x && s[y - 1] == ' ') {
                y--;
            }
            cell[k] = s + x;
            clen[k] = y - x;
            k++;
            st = i + 1;
        }
    }
    return k;
}

static int is_sep_row(const char *s, size_t n)
{
    int dash = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (ch == '-') {
            dash = 1;
        } else if (ch != '|' && ch != ':' && ch != ' ' && ch != '\t') {
            return 0;
        }
    }
    return dash;
}

#define TMAXC 16

static void lay_table(tlines_t *L, const char *const *rows, const size_t *lens, int nr, int width)
{
    const char *cell[TMAXC];
    size_t clen[TMAXC];
    int ncol = split_row(rows[0], lens[0], cell, clen, TMAXC);
    int align[TMAXC] = {0}; /* 0 left, 1 center, 2 right */
    {
        int k = split_row(rows[1], lens[1], cell, clen, TMAXC);
        for (int j = 0; j < k && j < ncol; j++) {
            int l = clen[j] && cell[j][0] == ':', r = clen[j] && cell[j][clen[j] - 1] == ':';
            align[j] = l && r ? 1 : r ? 2 : 0;
        }
    }
    int nrow = nr - 1; /* header + body rows (separator excluded) */
    tcells_t **cs = calloc((size_t)(nrow * ncol), sizeof(*cs));
    int nat[TMAXC] = {0};
    for (int r = 0, src = 0; r < nrow; r++, src++) {
        if (src == 1) {
            src++;
        }
        int k = split_row(rows[src], lens[src], cell, clen, TMAXC);
        for (int j = 0; j < ncol; j++) {
            tcells_t *c = tc_new();
            if (j < k) {
                md_inline(c, cell[j], clen[j], sty(T.text, r == 0 ? TA_BOLD : 0));
            }
            cs[r * ncol + j] = c;
            int w = tc_width(c);
            if (w > nat[j]) {
                nat[j] = w;
            }
        }
    }
    int cw[TMAXC], total = 3 * (ncol - 1);
    for (int j = 0; j < ncol; j++) {
        cw[j] = nat[j] < 1 ? 1 : nat[j];
        total += cw[j];
    }
    while (total > width) { /* shrink the widest column */
        int wi = 0;
        for (int j = 1; j < ncol; j++) {
            if (cw[j] > cw[wi]) {
                wi = j;
            }
        }
        if (cw[wi] <= 4) {
            break;
        }
        cw[wi]--;
        total--;
    }
    tsty_t bar = sty(T.muted, 0);
    for (int r = 0; r < nrow; r++) {
        tlines_t cl[TMAXC];
        int h = 1;
        for (int j = 0; j < ncol; j++) {
            memset(&cl[j], 0, sizeof(cl[j]));
            tc_flow(cs[r * ncol + j], &cl[j], cw[j], TC_NONE, 1);
            if (cl[j].n > h) {
                h = cl[j].n;
            }
        }
        for (int y = 0; y < h; y++) {
            tline_t *l = tl_new(L, TC_NONE);
            for (int j = 0; j < ncol; j++) {
                if (j) {
                    tl_put(l, bar, " │ ", strlen(" │ "));
                }
                int base = l->w;
                if (y < cl[j].n) {
                    int gap = cw[j] - cl[j].l[y].w;
                    int lead = align[j] == 2 ? gap : align[j] == 1 ? gap / 2 : 0;
                    tl_pad(l, base + lead, sty(T.text, 0));
                    tl_cat(l, &cl[j].l[y]);
                }
                tl_pad(l, base + cw[j], sty(T.text, 0));
            }
        }
        for (int j = 0; j < ncol; j++) {
            tl_free(&cl[j]);
        }
        if (r == 0) {
            tline_t *l = tl_new(L, TC_NONE);
            for (int j = 0; j < ncol; j++) {
                if (j) {
                    tl_put(l, bar, "─┼─", strlen("─┼─"));
                }
                for (int x = 0; x < cw[j]; x++) {
                    tl_put(l, bar, "─", strlen("─"));
                }
            }
        }
    }
    for (int i = 0; i < nrow * ncol; i++) {
        tc_free(cs[i]);
    }
    free(cs);
}

/* ---------------- block markdown ---------------- */

static size_t lead_spaces(const char *s, size_t n)
{
    size_t i = 0, col = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) {
        col += s[i] == '\t' ? 4 : 1;
        i++;
    }
    return col;
}

static const char *skip_ws(const char *s, const char *e)
{
    while (s < e && (*s == ' ' || *s == '\t')) {
        s++;
    }
    return s;
}

/* List marker at s: returns its length (incl. following space) and fills the bullet text. */
static size_t list_marker(const char *s, size_t n, char *bullet, size_t bcap, int level)
{
    if (n >= 2 && (s[0] == '-' || s[0] == '*' || s[0] == '+') && s[1] == ' ') {
        static const char *const b[] = {"•", "◦", "▪"};
        snprintf(bullet, bcap, "%s", b[level % 3]);
        return 2;
    }
    size_t i = 0;
    while (i < n && i < 9 && isdigit((unsigned char)s[i])) {
        i++;
    }
    if (i > 0 && i + 1 < n && (s[i] == '.' || s[i] == ')') && s[i + 1] == ' ') {
        snprintf(bullet, bcap, "%.*s.", (int)i, s);
        return i + 2;
    }
    return 0;
}

void lay_markdown(tlines_t *L, const char *md, int width)
{
    /* split into lines */
    size_t total = strlen(md), cap = 64, nl = 0;
    const char **ln = malloc(cap * sizeof(*ln));
    size_t *ll = malloc(cap * sizeof(*ll));
    for (size_t i = 0; i <= total;) {
        const char *e = memchr(md + i, '\n', total - i);
        size_t len = e ? (size_t)(e - md) - i : total - i;
        if (nl == cap) {
            cap *= 2;
            ln = xrealloc(ln, cap * sizeof(*ln));
            ll = xrealloc(ll, cap * sizeof(*ll));
        }
        if (len && md[i + len - 1] == '\r') {
            len--;
        }
        ln[nl] = md + i;
        ll[nl++] = len;
        if (!e) {
            break;
        }
        i = (size_t)(e - md) + 1;
    }
    while (nl > 0 && ll[nl - 1] == 0) {
        nl--;
    }
    size_t i = 0;
    while (i < nl && ll[i] == 0) {
        i++;
    }

    int prev_blank = 0;
    while (i < nl) {
        const char *s = ln[i], *e = ln[i] + ll[i];
        const char *t = skip_ws(s, e);
        size_t tn = (size_t)(e - t);
        size_t ind = lead_spaces(s, ll[i]);

        if (tn == 0) {
            if (!prev_blank) {
                tl_blank(L);
            }
            prev_blank = 1;
            i++;
            continue;
        }
        prev_blank = 0;

        /* fenced code */
        if (tn >= 3 && (strncmp(t, "```", 3) == 0 || strncmp(t, "~~~", 3) == 0)) {
            char fc = t[0];
            size_t fl = 0;
            while (fl < tn && t[fl] == fc) {
                fl++;
            }
            const char *lang = skip_ws(t + fl, e);
            size_t langn = 0;
            while (lang + langn < e && !isspace((unsigned char)lang[langn]) && lang[langn] != '{') {
                langn++;
            }
            /* "```python oml": highlighted as Python by GitHub/editors, as OML here */
            for (const char *w = lang + langn; w + 3 <= e; w++) {
                if (isspace((unsigned char)w[-1]) && !strncmp(w, "oml", 3) && (w + 3 == e || isspace((unsigned char)w[3]))) {
                    lang = "oml";
                    langn = 3;
                    break;
                }
            }
            size_t j = i + 1;
            while (j < nl) {
                const char *u = skip_ws(ln[j], ln[j] + ll[j]);
                size_t k = 0;
                while (u + k < ln[j] + ll[j] && u[k] == fc) {
                    k++;
                }
                if (k >= fl && skip_ws(u + k, ln[j] + ll[j]) == ln[j] + ll[j]) {
                    break;
                }
                j++;
            }
            /* strip the fence's indentation from the body */
            int n = (int)(j - i - 1);
            const char **cl = malloc((size_t)(n + 1) * sizeof(*cl));
            size_t *cn = malloc((size_t)(n + 1) * sizeof(*cn));
            for (int k = 0; k < n; k++) {
                const char *u = ln[i + 1 + (size_t)k];
                size_t un = ll[i + 1 + (size_t)k], d = 0;
                while (d < ind && d < un && u[d] == ' ') {
                    d++;
                }
                cl[k] = u + d;
                cn[k] = un - d;
            }
            int from = L->n;
            int pad = ind > 8 ? 8 : (int)ind;
            lay_code(L, lang, langn, cl, cn, n, width - pad);
            if (pad) {
                char sp[9] = "        ";
                sp[pad] = '\0';
                tl_prefix(L, from, sty(T.text, 0), sp, sty(T.text, 0), sp);
            }
            free(cl);
            free(cn);
            i = j < nl ? j + 1 : j;
            continue;
        }

        /* heading */
        if (t[0] == '#') {
            size_t h = 0;
            while (h < tn && t[h] == '#') {
                h++;
            }
            if (h <= 6 && (h == tn || t[h] == ' ')) {
                const char *x = skip_ws(t + h, e);
                const char *y = e;
                while (y > x && (y[-1] == '#' || y[-1] == ' ')) {
                    y--;
                }
                tsty_t hs = h == 1 ? sty(T.accent, TA_BOLD | TA_UNDER)
                          : h == 2 ? sty(T.accent, TA_BOLD)
                          : h == 3 ? sty(T.text, TA_BOLD)
                                   : sty(T.text, TA_BOLD | TA_ITALIC);
                lay_inline(L, x, (size_t)(y - x), hs, width, TC_NONE);
                i++;
                continue;
            }
        }

        /* horizontal rule */
        if (tn >= 3 && (t[0] == '-' || t[0] == '*' || t[0] == '_')) {
            size_t k = 0, cnt = 0;
            while (k < tn && (t[k] == t[0] || t[k] == ' ')) {
                cnt += t[k] == t[0];
                k++;
            }
            if (k == tn && cnt >= 3) {
                tline_t *l = tl_new(L, TC_NONE);
                for (int x = 0; x < width; x++) {
                    tl_put(l, sty(T.muted, 0), "─", strlen("─"));
                }
                i++;
                continue;
            }
        }

        /* table */
        if (memchr(t, '|', tn) && i + 1 < nl && memchr(ln[i + 1], '|', ll[i + 1]) && is_sep_row(ln[i + 1], ll[i + 1])) {
            size_t j = i + 2;
            while (j < nl && ll[j] && memchr(ln[j], '|', ll[j])) {
                j++;
            }
            lay_table(L, ln + i, ll + i, (int)(j - i), width);
            i = j;
            continue;
        }

        /* blockquote (consecutive '>' lines) */
        if (t[0] == '>') {
            int from = L->n;
            while (i < nl) {
                const char *u = skip_ws(ln[i], ln[i] + ll[i]);
                const char *ue = ln[i] + ll[i];
                if (u >= ue || *u != '>') {
                    break;
                }
                u++;
                if (u < ue && *u == ' ') {
                    u++;
                }
                lay_inline(L, u, (size_t)(ue - u), sty(T.text, TA_ITALIC), width - 2, TC_NONE);
                i++;
            }
            tl_prefix(L, from, sty(T.purple, 0), "▌ ", sty(T.purple, 0), "▌ ");
            continue;
        }

        /* list item (+ following indented continuation lines) */
        int level = (int)(ind / 2);
        if (level > 6) {
            level = 6;
        }
        char bullet[24];
        size_t ml = list_marker(t, tn, bullet, sizeof(bullet), level);
        if (ml) {
            const char *x = t + ml;
            tsty_t bs = sty(T.accent, 0);
            const char *check = NULL;
            if ((size_t)(e - x) >= 3 && x[0] == '[' && x[2] == ']' && (x[1] == ' ' || x[1] == 'x' || x[1] == 'X')) {
                check = x[1] == ' ' ? "☐" : "☑";
                x = skip_ws(x + 3, e);
            }
            int bw = tu_strwidth(bullet, strlen(bullet)) + 1;
            int indent = level * 2;
            int from = L->n;
            tsb_t body = {0};
            tsb_add(&body, x, (size_t)(e - x));
            while (i + 1 < nl && ll[i + 1] && lead_spaces(ln[i + 1], ll[i + 1]) > ind) {
                const char *u = skip_ws(ln[i + 1], ln[i + 1] + ll[i + 1]);
                char tmpb[24];
                if (list_marker(u, (size_t)(ln[i + 1] + ll[i + 1] - u), tmpb, sizeof(tmpb), 0) ||
                    strncmp(u, "```", 3) == 0 || strncmp(u, "~~~", 3) == 0) {
                    break;
                }
                tsb_add(&body, "\n", 1);
                tsb_add(&body, u, (size_t)(ln[i + 1] + ll[i + 1] - u));
                i++;
            }
            if (check) {
                tsty_t ks = sty(check[2] == '\x90' ? T.muted : T.ok, 0);
                tlines_t tmp = {0};
                lay_inline(&tmp, body.p, body.len, sty(T.text, 0), width - indent - bw - 2, TC_NONE);
                tl_prefix(&tmp, 0, ks, " ", ks, "  ");
                tl_prefix(&tmp, 0, ks, check, ks, "");
                tl_move(L, &tmp);
            } else {
                lay_inline(L, body.p, body.len, sty(T.text, 0), width - indent - bw, TC_NONE);
            }
            tsb_free(&body);
            char first[48], rest[48];
            snprintf(first, sizeof(first), "%*s%s ", indent, "", bullet);
            snprintf(rest, sizeof(rest), "%*s", indent + bw, "");
            tl_prefix(L, from, bs, first, bs, rest);
            i++;
            continue;
        }

        /* paragraph line (line breaks are kept) */
        int pad = ind > 8 ? 8 : (int)ind;
        int from = L->n;
        lay_inline(L, t, tn, sty(T.text, 0), width - pad, TC_NONE);
        if (pad) {
            char sp[9] = "        ";
            sp[pad] = '\0';
            tl_prefix(L, from, sty(T.text, 0), sp, sty(T.text, 0), sp);
        }
        i++;
    }
    free(ln);
    free(ll);
}

/* ---------------- SGR-coloured output ---------------- */

void lay_ansi(tlines_t *L, const char *s, size_t n, int width)
{
    tcells_t *c = tc_new();
    int bold = 0, dim = 0, col = 0; /* col: 0 default, 1 red, 2 blue, 3 green */
    size_t i = 0, lit = 0;
    while (n > 0 && s[n - 1] == '\n') {
        n--;
    }
    while (i <= n) {
        if (i == n || s[i] == '\033' || s[i] == '\r') {
            if (i > lit) {
                tsty_t st = sty(T.text, 0);
                if (dim) {
                    st = sty(T.muted, 0);
                } else if (col == 1) {
                    st = sty(T.err, bold ? TA_BOLD : 0);
                } else if (col == 2) {
                    st = sty(bold ? T.accent : T.text, TA_BOLD);
                } else if (col == 3) {
                    st = sty(T.ok, 0);
                } else if (bold) {
                    st = sty(T.text, TA_BOLD);
                }
                tc_add(c, s + lit, i - lit, st);
            }
            if (i == n) {
                break;
            }
            if (s[i] == '\r') {
                i++;
                lit = i;
                continue;
            }
            /* ESC [ params final */
            size_t j = i + 1;
            if (j < n && s[j] == '[') {
                j++;
                size_t ps = j;
                while (j < n && !(s[j] >= '@' && s[j] <= '~')) {
                    j++;
                }
                if (j < n && s[j] == 'm') {
                    const char *p = s + ps;
                    if (p == s + j) {
                        bold = dim = col = 0;
                    }
                    while (p < s + j) {
                        int v = 0;
                        while (p < s + j && isdigit((unsigned char)*p)) {
                            v = v * 10 + (*p++ - '0');
                        }
                        if (p < s + j) {
                            p++;
                        }
                        if (v == 0) {
                            bold = dim = col = 0;
                        } else if (v == 1) {
                            bold = 1;
                        } else if (v == 2) {
                            dim = 1;
                        } else if (v == 22) {
                            bold = dim = 0;
                        } else if (v == 31 || v == 91) {
                            col = 1;
                        } else if (v == 34 || v == 94) {
                            col = 2;
                        } else if (v == 32 || v == 92) {
                            col = 3;
                        } else if (v == 39) {
                            col = 0;
                        }
                    }
                }
                i = j < n ? j + 1 : n;
            } else {
                i = j;
            }
            lit = i;
            continue;
        }
        i++;
    }
    tc_flow(c, L, width, TC_NONE, 1);
    tc_free(c);
}

/* ---------------- logo ---------------- */

int logo_width(void) { return (int)strlen(LOGO_ROWS[0]); }

static uint32_t logo_px(int x, int y)
{
    int rows = (int)(sizeof(LOGO_ROWS) / sizeof(LOGO_ROWS[0]));
    if (y >= rows) {
        return TC_NONE;
    }
    char k = LOGO_ROWS[y][x];
    for (size_t i = 0; i < sizeof(LOGO_COLORS) / sizeof(LOGO_COLORS[0]); i++) {
        if (LOGO_COLORS[i].key == k) {
            return LOGO_COLORS[i].rgb;
        }
    }
    return TC_NONE;
}

void lay_logo(tlines_t *L)
{
    int rows = (int)(sizeof(LOGO_ROWS) / sizeof(LOGO_ROWS[0]));
    int w = logo_width();
    for (int y = 0; y < rows; y += 2) {
        tline_t *l = tl_new(L, TC_NONE);
        for (int x = 0; x < w; x++) {
            uint32_t top = logo_px(x, y), bot = logo_px(x, y + 1);
            if (top == TC_NONE && bot == TC_NONE) {
                tl_put(l, sty(T.text, 0), " ", 1);
            } else if (bot == TC_NONE) {
                tl_put(l, sty(top, 0), "▀", strlen("▀"));
            } else if (top == TC_NONE) {
                tl_put(l, sty(bot, 0), "▄", strlen("▄"));
            } else {
                tl_put(l, sty_bg(top, bot, 0), "▀", strlen("▀"));
            }
        }
    }
}
