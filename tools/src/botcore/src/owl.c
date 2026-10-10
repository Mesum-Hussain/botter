/*
 * Owl front end: parser (syntax tree + syntax errors) and build-time checks.
 * Shared by botcore (owl_run.c interprets the tree) and botter_pack (which
 * #includes this file and runs sq_check). See owl.h and skills/write-flow.
 */
#define _GNU_SOURCE
#include "owl.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SQ_MAXD 64

static const char *const SQ_BLOCK[] = {"", "IF", "FOR", "WHILE", "RETRY", "PARALLEL"};
enum { B_IF = 1, B_FOR, B_WHILE, B_RETRY, B_PAR };

/* ---------------------------------------------------------------- helpers */

static void sq_diag(sq_prog *p, int line, int error, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void sq_diag(sq_prog *p, int line, int error, const char *fmt, ...)
{
    char m[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    if (error) {
        p->errors++;
    } else {
        p->warnings++;
    }
    if (p->diag) {
        p->diag(p->ud, line, error, m);
    }
}

static char *sq_dup(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (!d) {
        abort();
    }
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static void *sq_alloc(size_t n)
{
    void *v = calloc(1, n);
    if (!v) {
        abort();
    }
    return v;
}

static void sq_push_node(sq_list *l, sq_node *x)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = realloc(l->v, (size_t)l->cap * sizeof(*l->v));
        if (!l->v) {
            abort();
        }
    }
    l->v[l->n++] = x;
}

static const char *sq_skip(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

/* Case-insensitive word at s; its length or 0. */
static size_t sq_word(const char *s, const char *w)
{
    size_t n = strlen(w);
    return strncasecmp(s, w, n) == 0 && !isalnum((unsigned char)s[n]) && s[n] != '_' ? n : 0;
}

/* Text ends with the UPPERCASE keyword w (as a separate word)? */
static int sq_ends(const char *s, const char *w)
{
    size_t n = strlen(s), k = strlen(w);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        n--;
    }
    return n >= k && strncmp(s + n - k, w, k) == 0 && (n == k || s[n - k - 1] == ' ' || s[n - k - 1] == '\t');
}

/* Copy of s without a trailing keyword w (and the spaces before it). */
static char *sq_strip_end(const char *s, const char *w)
{
    size_t n = strlen(s), k = strlen(w);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        n--;
    }
    if (n >= k && strncmp(s + n - k, w, k) == 0) {
        n -= k;
    }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        n--;
    }
    return sq_dup(s, n);
}

static int sq_ident(const char *s, size_t n)
{
    if (n == 0 || n > 64 || !(isalpha((unsigned char)s[0]) || s[0] == '_')) {
        return 0;
    }
    for (size_t i = 1; i < n; i++) {
        if (!isalnum((unsigned char)s[i]) && s[i] != '_') {
            return 0;
        }
    }
    return 1;
}

int sq_ref(const char *s, size_t n, char root[65])
{
    size_t i = 0;
    while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '_')) {
        i++;
    }
    if (!sq_ident(s, i)) {
        return 0;
    }
    snprintf(root, 65, "%.*s", (int)i, s);
    while (i < n) {
        if (s[i] == '.') {
            size_t j = ++i;
            while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '_')) {
                i++;
            }
            if (i == j) {
                return 0;
            }
        } else if (s[i] == '[') {
            size_t j = ++i;
            while (i < n && isdigit((unsigned char)s[i])) {
                i++;
            }
            if (i == j || i == n || s[i] != ']') {
                return 0;
            }
            i++;
        } else {
            return 0;
        }
    }
    return 1;
}

/* "name" at s -> out; position after it, or NULL. */
static const char *sq_quoted(const char *s, char *out, size_t cap)
{
    s = sq_skip(s);
    if (*s != '"') {
        return NULL;
    }
    const char *e = strchr(s + 1, '"');
    if (!e) {
        return NULL;
    }
    snprintf(out, cap, "%.*s", (int)(e - s - 1), s + 1);
    return e + 1;
}

/* `name` (one identifier) at s -> out; position after it, or NULL. */
static const char *sq_ticked(const char *s, char out[65])
{
    s = sq_skip(s);
    if (*s != '`') {
        return NULL;
    }
    const char *e = strchr(s + 1, '`');
    if (!e || !sq_ident(s + 1, (size_t)(e - s - 1))) {
        return NULL;
    }
    snprintf(out, 65, "%.*s", (int)(e - s - 1), s + 1);
    return e + 1;
}

/*
 * Syntax of an expression or prose text: strings closed, `refs` closed and
 * well-formed, and (braces) balanced { } [ ] ( ). Returns 0 if fine.
 */
static int sq_syntax(sq_prog *p, int line, const char *s, int braces)
{
    int bal[3] = {0, 0, 0};
    for (const char *c = s; *c; c++) {
        if (*c == '"') {
            const char *q = c + 1;
            while (*q && *q != '"') {
                q += *q == '\\' && q[1] ? 2 : 1;
            }
            if (!*q) {
                sq_diag(p, line, 1, "a string is not closed: add the missing \"");
                return -1;
            }
            c = q;
        } else if (*c == '`') {
            const char *q = strchr(c + 1, '`');
            char root[65];
            if (!q) {
                sq_diag(p, line, 1, "a `name` is not closed: add the missing `");
                return -1;
            }
            if (!sq_ref(c + 1, (size_t)(q - c - 1), root)) {
                sq_diag(p, line, 1, "`%.*s` is not a valid variable name (letters, digits, _ ; fields with . or [n])",
                        (int)(q - c - 1), c + 1);
                return -1;
            }
            c = q;
        } else if (braces) {
            const char *o = "{[(", *cl = "}])", *k;
            if ((k = strchr(o, *c))) {
                bal[k - o]++;
            } else if ((k = strchr(cl, *c))) {
                if (--bal[k - cl] < 0) {
                    sq_diag(p, line, 1, "unbalanced '%c'", *c);
                    return -1;
                }
            }
        }
    }
    if (braces && (bal[0] || bal[1] || bal[2])) {
        sq_diag(p, line, 1,
                "a payload's { } [ ] ( ) are not balanced; keep a payload on one line, or write 'with parameters:' "
                "followed by '- name: value' lines");
        return -1;
    }
    return 0;
}

int sq_is_literal(const char *s)
{
    int any = 0;
    for (const char *c = s; *c;) {
        if (*c == '"') {
            const char *q = c + 1;
            while (*q && *q != '"') {
                q += *q == '\\' && q[1] ? 2 : 1;
            }
            c = *q ? q + 1 : q;
            any = 1;
        } else if (*c == '`') {
            const char *q = strchr(c + 1, '`');
            c = q ? q + 1 : c + 1;
            any = 1;
        } else if (strchr(" \t+,:{}[]-.0123456789", *c)) {
            any |= isdigit((unsigned char)*c) || *c == '{' || *c == '[';
            c++;
        } else if (isalpha((unsigned char)*c) || *c == '_') {
            const char *e = c;
            while (isalnum((unsigned char)*e) || *e == '_') {
                e++;
            }
            size_t n = (size_t)(e - c);
            int kw = (n == 4 && !strncmp(c, "true", 4)) || (n == 5 && !strncmp(c, "false", 5)) ||
                     (n == 4 && !strncmp(c, "null", 4));
            const char *a = sq_skip(e);
            if (!kw && *a != ':') { /* a bare word that is not a JSON key: prose */
                return 0;
            }
            any = 1;
            c = e;
        } else {
            return 0;
        }
    }
    return any;
}

/* ----------------------------------------------------------------- parser */

typedef struct {
    int      kind;
    int      line;
    int      has_else;
    int      count; /* statements in the current body / branch */
    sq_node *node;
    sq_list *cur;
} sq_frame;

typedef struct {
    sq_prog *p;
    int      line;
    sq_frame st[SQ_MAXD];
    int      depth;
    sq_node *last; /* last statement (for "- k: v" parameter lines) */
    int      params;
    int      step, stmts, returns;
} sq_ps;

static sq_list *sq_cur(sq_ps *s)
{
    return s->depth ? s->st[s->depth - 1].cur : &s->p->top;
}

static sq_node *sq_add(sq_ps *s, int kind)
{
    sq_node *x = sq_alloc(sizeof(*x));
    x->kind = kind;
    x->line = s->line;
    sq_push_node(sq_cur(s), x);
    if (s->depth) {
        s->st[s->depth - 1].count++;
    }
    if (kind != SQ_STEP && kind != SQ_LOAD) {
        s->stmts++;
    }
    return x;
}

static void sq_open(sq_ps *s, int bkind, sq_node *x)
{
    if (s->depth == SQ_MAXD) {
        sq_diag(s->p, s->line, 1, "blocks nested too deeply");
        return;
    }
    sq_frame *f = &s->st[s->depth++];
    memset(f, 0, sizeof(*f));
    f->kind = bkind;
    f->line = s->line;
    f->node = x;
    f->cur = bkind == B_IF ? &x->br[0].body : &x->body;
}

static const char *sq_bname(int k)
{
    return k == B_FOR ? "FOR EACH" : SQ_BLOCK[k];
}

static void sq_close(sq_ps *s, int kind)
{
    if (s->depth == 0) {
        sq_diag(s->p, s->line, 1, "END %s without a matching %s", SQ_BLOCK[kind], sq_bname(kind));
        return;
    }
    sq_frame *f = &s->st[s->depth - 1];
    if (f->kind != kind) {
        int k = s->depth - 1;
        while (k >= 0 && s->st[k].kind != kind) {
            k--;
        }
        sq_diag(s->p, s->line, 1, "END %s, but the %s opened on line %d is still open (close it with END %s first)",
                SQ_BLOCK[kind], sq_bname(f->kind), f->line, SQ_BLOCK[f->kind]);
        if (k < 0) {
            return; /* stray END: ignore it */
        }
        s->depth = k + 1; /* recover: the inner blocks end here */
        f = &s->st[k];
    }
    if (!f->count) {
        sq_diag(s->p, s->line, 0, "the %s block opened on line %d is empty", SQ_BLOCK[kind], f->line);
    }
    s->depth--;
}

static int sq_lev(const char *a, const char *b)
{
    size_t n = strlen(a), m = strlen(b);
    int d[24][24];
    if (n > 22 || m > 22) {
        return 99;
    }
    for (size_t i = 0; i <= n; i++) {
        d[i][0] = (int)i;
    }
    for (size_t j = 0; j <= m; j++) {
        d[0][j] = (int)j;
    }
    for (size_t i = 1; i <= n; i++) {
        for (size_t j = 1; j <= m; j++) {
            int c = d[i - 1][j - 1] + (a[i - 1] != b[j - 1]);
            int x = d[i - 1][j] + 1, y = d[i][j - 1] + 1;
            d[i][j] = c < x ? (c < y ? c : y) : (x < y ? x : y);
        }
    }
    return d[n][m];
}

static const char *const SQ_KW[] = {"STEP", "LOAD", "CONNECT", "EXECUTE", "INVOKE", "SAVE", "SET",    "ASK", "IF",
                                    "ELSE", "END",     "FOR",     "WHILE",  "RETRY", "IN",   "RETURN", NULL};

/* An UPPERCASE first word that is no keyword: a typo of one (error), or a shouted word (prose). */
static int sq_unknown(sq_ps *s, const char *w)
{
    static const char *const alias[][2] = {{"INVOCATE", "INVOKE"}, {"INVOKES", "INVOKE"}, {"EXEC", "EXECUTE"},
                                           {"CALL", "EXECUTE"},    {"RUN", "EXECUTE"},    {"STORE", "SAVE"},
                                           {"FOREACH", "FOR EACH"}, {"ELIF", "ELSE IF"},  {"ELSEIF", "ELSE IF"},
                                           {"ENDIF", "END IF"},    {"ENDFOR", "END FOR"}, {"THEN", "IF ... THEN"},
                                           {"PRINT", "RETURN"},    {"REPEAT", "RETRY UP TO N TIMES DO"}};
    for (size_t i = 0; i < sizeof(alias) / sizeof(alias[0]); i++) {
        if (strcmp(w, alias[i][0]) == 0) {
            sq_diag(s->p, s->line, 1, "unknown keyword %s; did you mean %s?", w, alias[i][1]);
            return 1;
        }
    }
    if (strlen(w) < 4) {
        return 0; /* API, URL, PDF ...: plain English */
    }
    for (int i = 0; SQ_KW[i]; i++) {
        if (strlen(SQ_KW[i]) >= 4 && sq_lev(w, SQ_KW[i]) <= 2) {
            sq_diag(s->p, s->line, 1, "unknown keyword %s; did you mean %s?", w, SQ_KW[i]);
            return 1;
        }
    }
    return 0;
}

static void sq_instr(sq_ps *s, const char *t)
{
    static const char *const prose[] = {"if", "for each", "while", "else", "end", "return", "retry", NULL};
    static const char *const fix[] = {"IF ... THEN / END IF", "FOR EACH `x` IN ... DO / END FOR",
                                      "WHILE ... AT MOST N TIMES DO / END WHILE", "ELSE", "END IF / END FOR ...",
                                      "RETURN", "RETRY UP TO N TIMES DO / END RETRY"};
    for (int i = 0; prose[i]; i++) {
        if (sq_word(t, prose[i]) && !isupper((unsigned char)t[1])) {
            sq_diag(s->p, s->line, 0, "write '%s' as an Owl statement in capitals (%s) so the flow can be checked",
                    prose[i], fix[i]);
            break;
        }
    }
    sq_syntax(s->p, s->line, t, 0);
    sq_node *x = sq_add(s, SQ_INSTR);
    x->a = sq_dup(t, strlen(t));
}

/* One statement (list marker and indentation removed). */
static void sq_stmt(sq_ps *s, char *t)
{
    sq_prog *p = s->p;
    char w[32] = "", name[200], var[65];
    size_t wl = 0;
    while (t[wl] && isalpha((unsigned char)t[wl]) && wl < sizeof(w) - 1) {
        w[wl] = t[wl];
        wl++;
    }
    w[wl] = '\0';
    int upper = wl > 1 && !isalnum((unsigned char)t[wl]) && t[wl] != '_';
    for (size_t i = 0; i < wl && upper; i++) {
        upper = isupper((unsigned char)w[i]);
    }
    const char *r = sq_skip(t + wl);
    size_t len = strlen(t);
    int colon = len && t[len - 1] == ':';
    s->last = NULL;

    if (!upper) {
        sq_instr(s, t);
        return;
    }
    if (!strcmp(w, "STEP")) { /* STEP n: NAME, a label numbered in order */
        char *e;
        long k = strtol(r, &e, 10);
        if (e == r || k < 1 || *e != ':' || !*sq_skip(e + 1)) {
            sq_diag(p, s->line, 1, "STEP reads: STEP n: NAME (numbered 1, 2, 3 ...)");
            return;
        }
        if (k != s->step + 1) {
            sq_diag(p, s->line, 0, "STEP %ld follows STEP %d", k, s->step);
        }
        if (s->depth) {
            sq_diag(p, s->line, 0, "a STEP inside an open %s block (opened on line %d)",
                    SQ_BLOCK[s->st[s->depth - 1].kind], s->st[s->depth - 1].line);
        }
        s->step = (int)k;
        const char *nm = sq_skip(e + 1);
        sq_node *x = sq_add(s, SQ_STEP);
        x->a = sq_dup(nm, strlen(nm));
        x->n = (int)k;
        return;
    }
    if (!strcmp(w, "LOAD")) {
        size_t k = sq_word(r, "SKILL");
        const char *e = k ? sq_quoted(r + k, name, sizeof(name)) : NULL;
        if (!e || !name[0]) {
            sq_diag(p, s->line, 1, "LOAD reads: LOAD SKILL \"name\"  (optionally FROM \"./skills/name/SKILL.md\")");
            return;
        }
        if (s->depth) {
            sq_diag(p, s->line, 1, "LOAD SKILL belongs at the top of the flow, not inside a block");
        }
        e = sq_skip(e);
        if ((k = sq_word(e, "FROM"))) {
            char path[300], want[300];
            const char *f = sq_quoted(e + k, path, sizeof(path));
            snprintf(want, sizeof(want), "skills/%s/SKILL.md", name);
            const char *pp = strncmp(path, "./", 2) == 0 ? path + 2 : path;
            if (!f || strcmp(pp, want) != 0) {
                sq_diag(p, s->line, 1, "LOAD SKILL \"%s\" FROM must be \"./%s\"", name, want);
            }
        } else if (*e) {
            sq_diag(p, s->line, 1, "unexpected text after LOAD SKILL \"%s\": %s", name, e);
        }
        sq_node *x = sq_add(s, SQ_LOAD);
        x->a = sq_dup(name, strlen(name));
        return;
    }
    if (!strcmp(w, "CONNECT")) {
        sq_diag(p, s->line, 1, "CONNECT (MCP servers) is not supported by botcore yet; use the agent's own tools (tools/bin)");
        return;
    }
    if (!strcmp(w, "EXECUTE")) {
        size_t k = sq_word(r, "tool");
        const char *e = k ? sq_ticked(r + k, var) : NULL;
        if (!e) {
            sq_diag(p, s->line, 1, "EXECUTE reads: EXECUTE tool `name` [with payload { ... } | with parameters:]");
            return;
        }
        sq_syntax(p, s->line, e, 1);
        sq_node *x = sq_add(s, SQ_EXEC);
        x->a = sq_dup(var, strlen(var));
        e = sq_skip(e);
        x->b = sq_dup(e, strlen(e));
        s->last = x;
        s->params = colon;
        return;
    }
    if (!strcmp(w, "INVOKE")) {
        size_t k = sq_word(r, "SKILL");
        const char *e = k ? sq_quoted(r + k, name, sizeof(name)) : NULL;
        if (!e || !name[0]) {
            sq_diag(p, s->line, 1, "INVOKE reads: INVOKE SKILL \"name\" [USING context `x`]");
            return;
        }
        sq_syntax(p, s->line, e, 1);
        sq_node *x = sq_add(s, SQ_INVOKE);
        x->a = sq_dup(name, strlen(name));
        e = sq_skip(e);
        size_t u = sq_word(e, "USING");
        if (u) {
            e = sq_skip(e + u);
            size_t c = sq_word(e, "context");
            e = c ? sq_skip(e + c) : e;
        }
        x->b = sq_dup(e, strlen(e));
        s->last = x;
        s->params = colon;
        return;
    }
    if (!strcmp(w, "SAVE")) {
        const char *into = NULL, *tofile = NULL;
        for (const char *c = r; *c; c++) {
            if ((c == r || c[-1] == ' ') && sq_word(c, "INTO") && sq_word(sq_skip(c + 4), "VARIABLE")) {
                into = c;
            }
            if ((c == r || c[-1] == ' ') && sq_word(c, "TO") && sq_word(sq_skip(c + 2), "FILE")) {
                tofile = c;
            }
        }
        const char *at = into ? into : tofile;
        if (!at || at == r) {
            sq_diag(p, s->line, 1,
                    "SAVE reads: SAVE result|skill_output|answer|`x` INTO VARIABLE `name`, or SAVE ... TO FILE \"path\"");
            return;
        }
        size_t wn = (size_t)(at - r);
        while (wn && r[wn - 1] == ' ') {
            wn--;
        }
        char *what = sq_dup(r, wn);
        sq_syntax(p, s->line, what, 1);
        if (into) {
            const char *e = sq_ticked(sq_skip(sq_skip(into + 4) + 8), var);
            if (!e || *sq_skip(e)) {
                sq_diag(p, s->line, 1, "SAVE ... INTO VARIABLE needs one `name` (letters, digits, _)");
                free(what);
                return;
            }
            sq_node *x = sq_add(s, SQ_SAVE_VAR);
            x->a = what;
            x->b = sq_dup(var, strlen(var));
        } else {
            char path[300] = "";
            const char *e = sq_quoted(sq_skip(tofile + 2) + 4, path, sizeof(path));
            if (!e || !path[0] || path[0] == '/' || strstr(path, "..")) {
                sq_diag(p, s->line, 1, "SAVE ... TO FILE needs a \"relative/path\" inside the working directory");
            }
            sq_node *x = sq_add(s, SQ_SAVE_FILE);
            x->a = what;
            x->b = sq_dup(path, strlen(path));
        }
        return;
    }
    if (!strcmp(w, "SET")) {
        const char *e = sq_ticked(r, var);
        size_t k = e ? sq_word(sq_skip(e), "TO") : 0;
        const char *v = k ? sq_skip(sq_skip(e) + k) : NULL;
        if (!v || !*v) {
            sq_diag(p, s->line, 1, "SET reads: SET `name` TO <value>");
            return;
        }
        sq_syntax(p, s->line, v, 1);
        sq_node *x = sq_add(s, SQ_SET);
        x->a = sq_dup(var, strlen(var));
        x->b = sq_dup(v, strlen(v));
        return;
    }
    if (!strcmp(w, "ASK")) {
        size_t k = sq_word(r, "USER");
        const char *q = k ? sq_skip(r + k) : NULL;
        if (!q || !*q) {
            sq_diag(p, s->line, 1, "ASK reads: ASK USER \"question\"  (the reply is `answer`)");
            return;
        }
        sq_syntax(p, s->line, q, 1);
        sq_node *x = sq_add(s, SQ_ASK);
        x->a = sq_dup(q, strlen(q));
        return;
    }
    if (!strcmp(w, "IF")) {
        if (!sq_ends(r, "THEN") || strlen(sq_skip(r)) <= 4) {
            sq_diag(p, s->line, 1, "IF reads: IF <condition> THEN");
        }
        sq_syntax(p, s->line, r, 0);
        sq_node *x = sq_add(s, SQ_IF);
        x->br = sq_alloc(sizeof(*x->br));
        x->nbr = 1;
        x->br[0].cond = sq_strip_end(r, "THEN");
        x->br[0].line = s->line;
        sq_open(s, B_IF, x);
        return;
    }
    if (!strcmp(w, "ELSE")) {
        int elif = sq_word(r, "IF") > 0;
        sq_frame *f = s->depth ? &s->st[s->depth - 1] : NULL;
        if (!f || f->kind != B_IF) {
            sq_diag(p, s->line, 1, "%s without an open IF", elif ? "ELSE IF" : "ELSE");
            return;
        }
        if (f->has_else) {
            sq_diag(p, s->line, 1, "%s after ELSE: ELSE must be the last branch of an IF", elif ? "ELSE IF" : "ELSE");
        } else if (!f->count) {
            sq_diag(p, s->line, 0, "empty branch before %s", elif ? "ELSE IF" : "ELSE");
        }
        if (elif) {
            if (!sq_ends(r, "THEN")) {
                sq_diag(p, s->line, 1, "ELSE IF reads: ELSE IF <condition> THEN");
            }
            sq_syntax(p, s->line, r + 2, 0);
        } else if (*r) {
            sq_diag(p, s->line, 1, "ELSE stands alone on its line (for another condition write ELSE IF <condition> THEN)");
        }
        sq_node *x = f->node;
        x->br = realloc(x->br, (size_t)(x->nbr + 1) * sizeof(*x->br));
        if (!x->br) {
            abort();
        }
        sq_branch *b = &x->br[x->nbr++];
        memset(b, 0, sizeof(*b));
        b->cond = elif ? sq_strip_end(sq_skip(r + 2), "THEN") : NULL;
        b->line = s->line;
        f->cur = &b->body;
        f->has_else |= !elif;
        f->count = 0;
        return;
    }
    if (!strcmp(w, "END")) {
        static const struct {
            const char *w;
            int         k;
        } ends[] = {{"IF", B_IF}, {"FOR", B_FOR}, {"WHILE", B_WHILE}, {"RETRY", B_RETRY}, {"PARALLEL", B_PAR}};
        for (size_t i = 0; i < 5; i++) {
            size_t k = sq_word(r, ends[i].w);
            if (k && strncmp(r, ends[i].w, k) == 0) {
                if (*sq_skip(r + k)) {
                    sq_diag(p, s->line, 1, "END %s stands alone on its line", ends[i].w);
                }
                sq_close(s, ends[i].k);
                return;
            }
        }
        sq_diag(p, s->line, 1, "END must be followed by IF, FOR, WHILE, RETRY or PARALLEL");
        return;
    }
    if (!strcmp(w, "FOR")) {
        size_t k = sq_word(r, "EACH");
        const char *e = k ? sq_ticked(r + k, var) : NULL;
        size_t in = e ? sq_word(sq_skip(e), "IN") : 0;
        sq_node *x = sq_add(s, SQ_FOR);
        if (!e || !in || !sq_ends(r, "DO")) {
            sq_diag(p, s->line, 1, "FOR reads: FOR EACH `item` IN `collection` DO");
        } else {
            x->a = sq_dup(var, strlen(var));
            x->b = sq_strip_end(sq_skip(sq_skip(e) + in), "DO");
            sq_syntax(p, s->line, x->b, 1);
        }
        sq_open(s, B_FOR, x);
        return;
    }
    if (!strcmp(w, "WHILE")) {
        if (!sq_ends(r, "DO")) {
            sq_diag(p, s->line, 1, "WHILE reads: WHILE <condition> AT MOST N TIMES DO");
        }
        sq_syntax(p, s->line, r, 0);
        sq_node *x = sq_add(s, SQ_WHILE);
        char *c = sq_strip_end(r, "DO");
        char *am = strstr(c, "AT MOST");
        if (am) {
            char *end;
            long n = strtol(am + 7, &end, 10);
            if (n < 1 || n > 1000 || !sq_word(sq_skip(end), "TIMES")) {
                sq_diag(p, s->line, 1, "WHILE ... AT MOST N TIMES: N must be 1..1000");
            }
            x->n = n > 0 ? (int)n : 0;
            while (am > c && am[-1] == ' ') {
                am--;
            }
            *am = '\0';
        } else {
            sq_diag(p, s->line, 0, "give the WHILE loop a bound: WHILE <condition> AT MOST N TIMES DO");
        }
        x->a = c;
        sq_open(s, B_WHILE, x);
        return;
    }
    if (!strcmp(w, "RETRY")) {
        size_t k = sq_word(r, "UP");
        const char *q = k ? sq_skip(r + k) : r;
        size_t t2 = k ? sq_word(q, "TO") : 0;
        if (k && t2) {
            q = sq_skip(q + t2);
        }
        char *end;
        long n = strtol(q, &end, 10);
        if (end == q || !sq_word(sq_skip(end), "TIMES") || !sq_ends(r, "DO")) {
            sq_diag(p, s->line, 1, "RETRY reads: RETRY UP TO N TIMES DO");
        } else if (n < 1 || n > 20) {
            sq_diag(p, s->line, 1, "RETRY UP TO %ld TIMES: N must be 1..20", n);
        }
        sq_node *x = sq_add(s, SQ_RETRY);
        x->n = n >= 1 && n <= 20 ? (int)n : 1;
        sq_open(s, B_RETRY, x);
        return;
    }
    if (!strcmp(w, "IN")) {
        size_t k = sq_word(r, "PARALLEL");
        if (!k || strcmp(sq_skip(r + k), "DO") != 0) {
            sq_diag(p, s->line, 1, "IN PARALLEL reads: IN PARALLEL DO");
        }
        sq_node *x = sq_add(s, SQ_PAR);
        sq_open(s, B_PAR, x);
        return;
    }
    if (!strcmp(w, "RETURN")) {
        if (!*r) {
            sq_diag(p, s->line, 1, "RETURN needs a value: RETURN \"text\" or RETURN `x` (or a plain-English summary)");
            return;
        }
        sq_syntax(p, s->line, r, 1);
        sq_node *x = sq_add(s, SQ_RETURN);
        x->a = sq_dup(r, strlen(r));
        s->returns++;
        return;
    }
    if (!sq_unknown(s, w)) {
        sq_instr(s, t); /* a shouted plain-English line ("PDF files go to out/") */
    }
}

/* Frontmatter: spec-version "owl-1" required, title recommended. Offset after it, or 0. */
static size_t sq_front(sq_ps *s, const char *src, size_t n)
{
    sq_prog *p = s->p;
    s->line++;
    if (n < 4 || strncmp(src, "---", 3) != 0 || (src[3] != '\n' && src[3] != '\r')) {
        sq_diag(p, s->line, 1, "the frontmatter comes right after ```owl: ---, spec-version: \"owl-1\", title: \"...\", ---");
        return 0;
    }
    int ver = 0;
    size_t i = (size_t)(strchr(src, '\n') - src) + 1;
    while (i < n) {
        s->line++;
        const char *l = src + i;
        const char *nl = memchr(l, '\n', n - i);
        size_t ll = nl ? (size_t)(nl - l) : n - i;
        i += ll + (nl != NULL);
        if (ll >= 3 && strncmp(l, "---", 3) == 0) {
            if (!ver) {
                sq_diag(p, s->line, 1, "the frontmatter needs spec-version: \"owl-1\"");
            }
            if (!p->title[0]) {
                sq_diag(p, s->line, 0, "the frontmatter should have a title: \"...\"");
            }
            return i;
        }
        char line[400];
        snprintf(line, sizeof(line), "%.*s", (int)(ll < 399 ? ll : 399), l);
        line[strcspn(line, "\r")] = '\0';
        char *c = strchr(line, ':');
        if (!c) {
            if (*sq_skip(line)) {
                sq_diag(p, s->line, 1, "frontmatter lines read key: value");
            }
            continue;
        }
        *c = '\0';
        const char *v = sq_skip(c + 1);
        char val[300];
        size_t vl = strlen(v);
        if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) {
            snprintf(val, sizeof(val), "%.*s", (int)(vl - 2), v + 1);
        } else {
            snprintf(val, sizeof(val), "%s", v);
        }
        if (!strcmp(line, "spec-version")) {
            ver = 1;
            if (strcmp(val, "owl-1") != 0) {
                sq_diag(p, s->line, 1, "spec-version is \"%s\"; this botter understands \"owl-1\"", val);
            }
        } else if (!strcmp(line, "title")) {
            snprintf(p->title, sizeof(p->title), "%.199s", val);
        } else if (strcmp(line, "description") != 0) {
            sq_diag(p, s->line, 0, "unknown frontmatter key '%s' (known: spec-version, title, description)", line);
        }
    }
    sq_diag(p, s->line, 1, "the frontmatter is not closed with ---");
    return 0;
}

int sq_parse(const char *src, size_t n, sq_prog *p)
{
    sq_ps s;
    memset(&s, 0, sizeof(s));
    s.p = p;
    /* The whole file is one ```owl block: frontmatter, then statements, then the closing ```. */
    size_t first = strcspn(src, "\n");
    const char *info = sq_skip(src + 3);
    if (n < 3 || strncmp(src, "```", 3) != 0 || !sq_word(info, "owl")) {
        sq_diag(p, 1, 1, strncmp(src, "---", 3) == 0
                             ? "FLOW.md starts with ```owl on its first line; the frontmatter goes inside the block"
                             : "FLOW.md starts with ```owl on its first line and ends with ``` (the whole file is "
                               "one Owl block)");
        return p->errors;
    }
    s.line = 1;
    size_t i = first < n ? first + 1 : n;
    size_t fm = sq_front(&s, src + i, n - i);
    if (!fm) {
        return p->errors;
    }
    i += fm;
    int closed = 0;
    while (i < n) {
        s.line++;
        const char *l = src + i;
        const char *nl = memchr(l, '\n', n - i);
        size_t ll = nl ? (size_t)(nl - l) : n - i;
        i += ll + (nl != NULL);
        char buf[2048];
        if (ll >= sizeof(buf)) {
            sq_diag(p, s.line, 1, "line too long (max %zu characters)", sizeof(buf) - 1);
            continue;
        }
        memcpy(buf, l, ll);
        buf[ll] = '\0';
        buf[strcspn(buf, "\r")] = '\0';
        char *t = (char *)sq_skip(buf);
        for (size_t k = strlen(t); k && (t[k - 1] == ' ' || t[k - 1] == '\t'); k--) {
            t[k - 1] = '\0';
        }
        if (closed) {
            if (*t) {
                sq_diag(p, s.line, 1, "nothing may follow the closing ``` of the Owl block");
                break;
            }
            continue;
        }
        if (!*t) {
            continue;
        }
        if (strncmp(t, "```", 3) == 0) {
            if (t[3]) {
                sq_diag(p, s.line, 1, "a code block cannot start inside the Owl block");
                continue;
            }
            closed = 1;
            s.params = 0;
            continue;
        }
        if (*t == '#' || strncmp(t, "<!--", 4) == 0) { /* Markdown is not part of Owl */
            const char *h = t + strspn(t, "# ");
            if (*t == '#' && strncmp(h, "STEP ", 5) == 0) {
                sq_diag(p, s.line, 1, "write the step label as Owl, without #: STEP n: NAME");
            } else {
                sq_diag(p, s.line, 1, "Markdown (headings, <!-- comments -->) is not part of Owl; write plain "
                                      "English or an Owl statement");
            }
            continue;
        }
        int dash = (t[0] == '-' || t[0] == '*') && t[1] == ' ';
        if (dash) {
            t = (char *)sq_skip(t + 2);
        } else if (isdigit((unsigned char)t[0])) {
            char *e = t;
            while (isdigit((unsigned char)*e)) {
                e++;
            }
            if (*e == '.' && (e[1] == ' ' || e[1] == '\0')) {
                t = (char *)sq_skip(e + 1);
            }
        }
        if (dash && s.params && s.last) { /* "- name: value" under "with parameters:" */
            char *c = strchr(t, ':');
            if (!c || !sq_ident(t, (size_t)(c - t))) {
                sq_diag(p, s.line, 1, "a parameter line reads: - name: value");
            } else {
                const char *v = sq_skip(c + 1);
                sq_syntax(p, s.line, v, 1);
                sq_node *x = s.last;
                x->pk = realloc(x->pk, (size_t)(x->np + 1) * sizeof(char *));
                x->pv = realloc(x->pv, (size_t)(x->np + 1) * sizeof(char *));
                x->pl = realloc(x->pl, (size_t)(x->np + 1) * sizeof(int));
                if (!x->pk || !x->pv || !x->pl) {
                    abort();
                }
                x->pl[x->np] = s.line;
                x->pk[x->np] = sq_dup(t, (size_t)(c - t));
                x->pv[x->np++] = sq_dup(v, strlen(v));
            }
            continue;
        }
        s.params = 0;
        if (*t) {
            sq_stmt(&s, t);
        }
    }
    if (!closed) {
        sq_diag(p, s.line, 1, "the Owl block is never closed: end the file with ```");
    }
    while (s.depth > 0) {
        sq_frame *f = &s.st[--s.depth];
        sq_diag(p, f->line, 1, "this %s is never closed (add END %s)", sq_bname(f->kind), SQ_BLOCK[f->kind]);
    }
    if (!s.stmts) {
        sq_diag(p, s.line, 1, "the flow has no statements");
    } else if (!s.returns) {
        sq_diag(p, s.line, 0, "the flow never RETURNs: end it with RETURN and what the user gets");
    }
    return p->errors;
}

static void sq_free_list(sq_list *l);

static void sq_free_node(sq_node *x)
{
    free(x->a);
    free(x->b);
    for (int i = 0; i < x->np; i++) {
        free(x->pk[i]);
        free(x->pv[i]);
    }
    free(x->pk);
    free(x->pv);
    free(x->pl);
    sq_free_list(&x->body);
    for (int i = 0; i < x->nbr; i++) {
        free(x->br[i].cond);
        sq_free_list(&x->br[i].body);
    }
    free(x->br);
    free(x);
}

static void sq_free_list(sq_list *l)
{
    for (int i = 0; i < l->n; i++) {
        sq_free_node(l->v[i]);
    }
    free(l->v);
    memset(l, 0, sizeof(*l));
}

void sq_free(sq_prog *p)
{
    sq_free_list(&p->top);
}

/* ------------------------------------------------------------ build checks */

#define SQ_MAXV 256

typedef struct {
    sq_prog *p;
    int (*has_tool)(const char *);
    int (*has_skill)(const char *);
    char vars[SQ_MAXV][65];
    int  nvars;
    char dead[SQ_MAXV][65]; /* loop variables whose FOR EACH ended */
    int  ndead;
    char loaded[SQ_MAXV][65];
    int  nloaded;
} sq_cs;

static int sq_in(char (*set)[65], int n, const char *v)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(set[i], v) == 0) {
            return 1;
        }
    }
    return 0;
}

static void sq_def(sq_cs *c, const char *v)
{
    if (!sq_in(c->vars, c->nvars, v) && c->nvars < SQ_MAXV) {
        snprintf(c->vars[c->nvars++], 65, "%s", v);
    }
}

/* Every `ref` in text (outside strings) must be set earlier. */
static void sq_refs(sq_cs *c, int line, const char *s)
{
    for (const char *q = s; q && *q; q++) {
        if (*q == '"') {
            const char *e = q + 1;
            while (*e && *e != '"') {
                e += *e == '\\' && e[1] ? 2 : 1;
            }
            q = *e ? e : e - 1;
            continue;
        }
        if (*q != '`') {
            continue;
        }
        const char *e = strchr(q + 1, '`');
        char root[65];
        if (!e) {
            return;
        }
        if (sq_ref(q + 1, (size_t)(e - q - 1), root)) {
            if (sq_in(c->dead, c->ndead, root) && !sq_in(c->vars, c->nvars, root)) {
                sq_diag(c->p, line, 1, "`%s` only exists inside its FOR EACH loop", root);
            } else if (!sq_in(c->vars, c->nvars, root)) {
                sq_diag(c->p, line, 1,
                        "`%s` is used before it is set (SAVE ... INTO VARIABLE `%s`, SET `%s` TO ..., or FOR EACH "
                        "`%s` IN ...)",
                        root, root, root, root);
            }
        }
        q = e;
    }
}

static void sq_check_list(sq_cs *c, sq_list *l)
{
    static const char *const implicit[] = {"result", "skill_output", "answer"};
    for (int i = 0; i < l->n; i++) {
        sq_node *x = l->v[i];
        switch (x->kind) {
        case SQ_INSTR:
            sq_refs(c, x->line, x->a);
            break;
        case SQ_LOAD:
            if (!c->has_skill(x->a)) {
                sq_diag(c->p, x->line, 1, "LOAD SKILL \"%s\": there is no skills/%s/SKILL.md in this agent", x->a, x->a);
            }
            if (sq_in(c->loaded, c->nloaded, x->a)) {
                sq_diag(c->p, x->line, 0, "skill \"%s\" is loaded twice", x->a);
            } else if (c->nloaded < SQ_MAXV) {
                snprintf(c->loaded[c->nloaded++], 65, "%.64s", x->a);
            }
            break;
        case SQ_EXEC:
            if (!c->has_tool(x->a)) {
                sq_diag(c->p, x->line, 1, "EXECUTE tool `%s`: no such tool (built-in, or tools/bin/%s with tools/doc/%s.json)",
                        x->a, x->a, x->a);
            }
            sq_refs(c, x->line, x->b);
            for (int k = 0; k < x->np; k++) {
                sq_refs(c, x->pl[k], x->pv[k]);
            }
            sq_def(c, "result");
            break;
        case SQ_INVOKE:
            if (!sq_in(c->loaded, c->nloaded, x->a)) {
                sq_diag(c->p, x->line, 1, "INVOKE SKILL \"%s\": add LOAD SKILL \"%s\" at the top of the flow first", x->a, x->a);
            }
            sq_refs(c, x->line, x->b);
            for (int k = 0; k < x->np; k++) {
                sq_refs(c, x->pl[k], x->pv[k]);
            }
            sq_def(c, "skill_output");
            break;
        case SQ_ASK:
            sq_refs(c, x->line, x->a);
            sq_def(c, "answer");
            break;
        case SQ_SAVE_VAR:
        case SQ_SAVE_FILE:
            for (int k = 0; k < 3; k++) {
                size_t n = strlen(implicit[k]);
                if (strncmp(x->a, implicit[k], n) == 0 && (x->a[n] == ' ' || x->a[n] == '\0' || x->a[n] == '.') &&
                    !sq_in(c->vars, c->nvars, implicit[k])) {
                    sq_diag(c->p, x->line, 1, "SAVE %s: there is no %s yet (it comes from %s)", implicit[k], implicit[k],
                            k == 0 ? "EXECUTE tool" : k == 1 ? "INVOKE SKILL" : "ASK USER");
                }
            }
            sq_refs(c, x->line, x->a);
            if (x->kind == SQ_SAVE_VAR) {
                sq_def(c, x->b);
            }
            break;
        case SQ_SET:
            sq_refs(c, x->line, x->b);
            sq_def(c, x->a);
            break;
        case SQ_IF:
            for (int k = 0; k < x->nbr; k++) {
                if (x->br[k].cond) {
                    sq_refs(c, x->br[k].line, x->br[k].cond);
                }
                sq_check_list(c, &x->br[k].body);
            }
            break;
        case SQ_FOR:
            sq_refs(c, x->line, x->b);
            if (x->a) {
                int had = sq_in(c->vars, c->nvars, x->a);
                sq_def(c, x->a);
                sq_check_list(c, &x->body);
                if (!had) { /* the loop variable ends with its loop */
                    for (int k = 0; k < c->nvars; k++) {
                        if (strcmp(c->vars[k], x->a) == 0) {
                            memmove(c->vars[k], c->vars[k + 1], (size_t)(c->nvars - k - 1) * 65);
                            c->nvars--;
                            break;
                        }
                    }
                    if (c->ndead < SQ_MAXV) {
                        snprintf(c->dead[c->ndead++], 65, "%s", x->a);
                    }
                }
            } else {
                sq_check_list(c, &x->body);
            }
            break;
        case SQ_WHILE:
            sq_refs(c, x->line, x->a);
            sq_check_list(c, &x->body);
            break;
        case SQ_RETRY:
        case SQ_PAR:
            sq_check_list(c, &x->body);
            break;
        case SQ_RETURN:
            sq_refs(c, x->line, x->a);
            break;
        }
    }
}

void sq_check(sq_prog *p, int (*has_tool)(const char *), int (*has_skill)(const char *))
{
    sq_cs *c = sq_alloc(sizeof(*c));
    c->p = p;
    c->has_tool = has_tool;
    c->has_skill = has_skill;
    sq_check_list(c, &p->top);
    free(c);
}
