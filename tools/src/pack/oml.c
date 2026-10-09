/*
 * OML v2 checker for flow.md (included by botter_pack.c; uses its err/warn
 * counters). OML v2 is the agent's session flow: Markdown with YAML-like
 * frontmatter and UPPERCASE statements; plain-English lines are instructions.
 * Nothing executes it: the agent's own LLM follows it. This checker is the
 * compiler's front end: it rejects a flow whose structure, names or variables
 * are wrong, so the model never follows a broken one.
 *
 *   ---
 *   spec-version: "oml-2"
 *   title: "Lead Outreach"
 *   ---
 *   LOAD SKILL "pitching"
 *   ## STEP 1: GATHER
 *   1. ASK USER "What do you sell?"
 *   2. SAVE answer INTO VARIABLE `service`
 *   3. EXECUTE tool `scrape_leads` with payload { "query": `service` }
 *   4. SAVE result INTO VARIABLE `leads`
 *   FOR EACH `lead` IN `leads` DO
 *       INVOKE SKILL "pitching" USING context `lead`
 *       ...
 *   END FOR
 *   RETURN "Done: " + `leads.length` + " leads"
 *
 * Statements: LOAD SKILL, EXECUTE tool, INVOKE SKILL, SAVE ... INTO VARIABLE /
 * TO FILE, SET ... TO, ASK USER, IF/ELSE IF/ELSE/END IF, FOR EACH/END FOR,
 * WHILE ... AT MOST N TIMES DO/END WHILE, RETRY UP TO N TIMES DO/END RETRY,
 * IN PARALLEL DO/END PARALLEL, RETURN. Full description: skills/write-flow.
 */

#define OML_MAXD    64
#define OML_MAXVARS 256

typedef struct {
    int  kind; /* OB_* */
    int  line;
    int  has_else;
    int  body; /* statements inside */
    char var[65]; /* FOR EACH loop variable */
} oml_block_t;

enum { OB_IF = 1, OB_FOR, OB_WHILE, OB_RETRY, OB_PAR };
static const char *const OB_NAME[] = {"", "IF", "FOR", "WHILE", "RETRY", "PARALLEL"};

static struct {
    int (*has_tool)(const char *);
    int (*has_skill)(const char *);
    int         line;
    oml_block_t st[OML_MAXD];
    int         depth;
    char        vars[OML_MAXVARS][65];
    int         nvars;
    char        dead[OML_MAXVARS][65]; /* loop variables whose FOR EACH has ended */
    int         ndead;
    char        loaded[OML_MAXVARS][65];
    int         nloaded;
    int         stmts, returns, step;
} O;

static void oml_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void oml_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "flow.md:%d: error: ", O.line);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    errors++;
}

static void oml_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void oml_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "flow.md:%d: warning: ", O.line);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    warnings++;
}

static int oml_in(char (*set)[65], int n, const char *v)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(set[i], v) == 0) {
            return 1;
        }
    }
    return 0;
}

static void oml_define(const char *v)
{
    if (!oml_in(O.vars, O.nvars, v) && O.nvars < OML_MAXVARS) {
        snprintf(O.vars[O.nvars++], 65, "%s", v);
    }
}

static int oml_ident(const char *s, size_t n)
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

/* Case-insensitive "word" at s (followed by a non-word char); returns the length or 0. */
static size_t oml_word(const char *s, const char *w)
{
    size_t n = strlen(w);
    return strncasecmp(s, w, n) == 0 && !isalnum((unsigned char)s[n]) && s[n] != '_' ? n : 0;
}

static const char *oml_skip(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

/* Does the text end with the word w (case-sensitive, uppercase keyword)? */
static int oml_ends(const char *s, const char *w)
{
    size_t n = strlen(s), k = strlen(w);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        n--;
    }
    return n >= k && strncmp(s + n - k, w, k) == 0 && (n == k || s[n - k - 1] == ' ' || s[n - k - 1] == '\t');
}

/* A `reference`: identifier, then .field / [n] parts. Root name into root. */
static int oml_ref(const char *s, size_t n, char root[65])
{
    size_t i = 0;
    while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '_')) {
        i++;
    }
    if (!oml_ident(s, i)) {
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
            while (i < n && s[i] != ']') {
                i++;
            }
            if (i == n) {
                return 0;
            }
            i++;
        } else {
            return 0;
        }
    }
    return 1;
}

/*
 * Check the variable references and quoting in an expression / plain-English
 * text: strings "..." are data (not scanned), `refs` must be set earlier.
 * braces = also check {} [] () balance (payloads).
 */
static void oml_refs(const char *s, int braces)
{
    int bal[3] = {0, 0, 0};
    for (const char *p = s; *p; p++) {
        if (*p == '"') {
            const char *q = p + 1;
            while (*q && *q != '"') {
                q += *q == '\\' && q[1] ? 2 : 1;
            }
            if (!*q) {
                oml_err("a string is not closed: add the missing \"");
                return;
            }
            p = q;
            continue;
        }
        if (*p == '`') {
            const char *q = strchr(p + 1, '`');
            char root[65];
            if (!q) {
                oml_err("a `name` is not closed: add the missing `");
                return;
            }
            if (!oml_ref(p + 1, (size_t)(q - p - 1), root)) {
                oml_err("`%.*s` is not a valid variable name (letters, digits, _ ; fields with . )", (int)(q - p - 1),
                        p + 1);
            } else if (oml_in(O.dead, O.ndead, root) && !oml_in(O.vars, O.nvars, root)) {
                oml_err("`%s` only exists inside its FOR EACH loop", root);
            } else if (!oml_in(O.vars, O.nvars, root)) {
                oml_err("`%s` is used before it is set (SAVE ... INTO VARIABLE `%s`, SET `%s` TO ..., or FOR EACH "
                        "`%s` IN ...)",
                        root, root, root, root);
            }
            p = q;
            continue;
        }
        if (braces) {
            const char *o = "{[(", *c = "}])";
            const char *k;
            if ((k = strchr(o, *p)) && *p) {
                bal[k - o]++;
            } else if ((k = strchr(c, *p)) && *p) {
                if (--bal[k - c] < 0) {
                    oml_err("unbalanced '%c'", *p);
                    return;
                }
            }
        }
    }
    if (braces && (bal[0] || bal[1] || bal[2])) {
        oml_err("a payload's { } [ ] ( ) are not balanced; keep a payload on one line, or write 'with parameters:' "
                "followed by '- name: value' lines");
    }
}

/* "name" right after the cursor -> copied into out; returns the position after it, or NULL. */
static const char *oml_quoted(const char *s, char *out, size_t cap)
{
    s = oml_skip(s);
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

/* `name` right after the cursor (one identifier) -> out; returns the position after it, or NULL. */
static const char *oml_ticked(const char *s, char out[65])
{
    s = oml_skip(s);
    if (*s != '`') {
        return NULL;
    }
    const char *e = strchr(s + 1, '`');
    if (!e || !oml_ident(s + 1, (size_t)(e - s - 1))) {
        return NULL;
    }
    snprintf(out, 65, "%.*s", (int)(e - s - 1), s + 1);
    return e + 1;
}

static void oml_push(int kind, const char *var)
{
    if (O.depth > 0) {
        O.st[O.depth - 1].body++;
    }
    if (O.depth == OML_MAXD) {
        oml_err("blocks nested too deeply");
        return;
    }
    oml_block_t *b = &O.st[O.depth++];
    memset(b, 0, sizeof(*b));
    b->kind = kind;
    b->line = O.line;
    if (var) {
        snprintf(b->var, sizeof(b->var), "%s", var);
    }
}

static void oml_pop(int kind)
{
    if (O.depth == 0) {
        oml_err("END %s without a matching %s", OB_NAME[kind], kind == OB_FOR ? "FOR EACH" : OB_NAME[kind]);
        return;
    }
    oml_block_t *b = &O.st[O.depth - 1];
    if (b->kind != kind) {
        int k = O.depth - 1;
        while (k >= 0 && O.st[k].kind != kind) {
            k--;
        }
        oml_err("END %s, but the %s opened on line %d is still open (close it with END %s first)", OB_NAME[kind],
                b->kind == OB_FOR ? "FOR EACH" : OB_NAME[b->kind], b->line, OB_NAME[b->kind]);
        if (k < 0) {
            return; /* stray END: ignore it */
        }
        while (O.depth - 1 > k) { /* recover: treat the inner blocks as closed here */
            O.st[O.depth - 1].body = 1;
            oml_pop(O.st[O.depth - 1].kind);
        }
        b = &O.st[O.depth - 1];
    }
    if (!b->body) {
        oml_warn("the %s block opened on line %d is empty", OB_NAME[kind], b->line);
    }
    if (kind == OB_FOR && b->var[0]) { /* the loop variable ends with its loop */
        for (int i = 0; i < O.nvars; i++) {
            if (strcmp(O.vars[i], b->var) == 0) {
                memmove(O.vars[i], O.vars[i + 1], (size_t)(O.nvars - i - 1) * 65);
                O.nvars--;
                break;
            }
        }
        if (O.ndead < OML_MAXVARS) {
            memcpy(O.dead[O.ndead++], b->var, sizeof(b->var));
        }
    }
    O.depth--;
}

static int oml_lev(const char *a, const char *b)
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

static const char *const OML_KW[] = {"LOAD", "CONNECT", "EXECUTE", "INVOKE", "SAVE",   "SET",
                                     "ASK",  "IF",      "ELSE",    "END",    "FOR",    "WHILE",
                                     "RETRY", "IN",     "RETURN",  NULL};

/* An UPPERCASE first word that is not a keyword: a typo of one, or just a shouted word? */
static void oml_unknown(const char *w)
{
    static const char *const alias[][2] = {{"INVOCATE", "INVOKE"}, {"INVOKES", "INVOKE"}, {"EXEC", "EXECUTE"},
                                           {"CALL", "EXECUTE"},    {"RUN", "EXECUTE"},    {"STORE", "SAVE"},
                                           {"FOREACH", "FOR EACH"}, {"ELIF", "ELSE IF"},  {"ELSEIF", "ELSE IF"},
                                           {"ENDIF", "END IF"},    {"ENDFOR", "END FOR"}, {"THEN", "IF ... THEN"},
                                           {"PRINT", "RETURN"},    {"REPEAT", "RETRY UP TO N TIMES DO"}};
    for (size_t i = 0; i < sizeof(alias) / sizeof(alias[0]); i++) {
        if (strcmp(w, alias[i][0]) == 0) {
            oml_err("unknown keyword %s; did you mean %s?", w, alias[i][1]);
            return;
        }
    }
    if (strlen(w) < 4) {
        return; /* API, URL, PDF ...: plain English */
    }
    for (int i = 0; OML_KW[i]; i++) {
        if (strlen(OML_KW[i]) >= 4 && oml_lev(w, OML_KW[i]) <= 2) {
            oml_err("unknown keyword %s; did you mean %s?", w, OML_KW[i]);
            return;
        }
    }
}

/* One statement (list marker and indentation already removed). Returns 1 if it ends with ':' (parameters follow). */
static int oml_stmt(char *s)
{
    char w[32] = "", name[200], var[65];
    size_t wl = 0;
    while (s[wl] && isalpha((unsigned char)s[wl]) && wl < sizeof(w) - 1) {
        w[wl] = s[wl];
        wl++;
    }
    w[wl] = '\0';
    int upper = wl > 1 && !isalnum((unsigned char)s[wl]) && s[wl] != '_';
    for (size_t i = 0; i < wl && upper; i++) {
        upper = isupper((unsigned char)w[i]);
    }
    const char *r = oml_skip(s + wl);
    size_t len = strlen(s);
    int colon = len && s[len - 1] == ':';

    if (!upper) {
        /* plain English: an instruction. Decisions and loops written in prose cannot be checked. */
        static const char *const prose[] = {"if", "for each", "while", "else", "end", "return", "retry", NULL};
        for (int i = 0; prose[i]; i++) {
            if (oml_word(s, prose[i])) {
                oml_warn("write '%s' as an OML statement in capitals (%s) so the flow can be checked", prose[i],
                         i == 0 ? "IF ... THEN / END IF" : i == 1 ? "FOR EACH `x` IN ... DO / END FOR"
                                                       : i == 2 ? "WHILE ... AT MOST N TIMES DO / END WHILE"
                                                       : i == 3 ? "ELSE" : i == 4 ? "END IF / END FOR ..."
                                                       : i == 5 ? "RETURN" : "RETRY UP TO N TIMES DO / END RETRY");
                break;
            }
        }
        oml_refs(s, 0);
        if (O.depth > 0) {
            O.st[O.depth - 1].body++;
        }
        O.stmts++;
        return 0;
    }
    if (O.depth > 0 && strcmp(w, "END") != 0 && strcmp(w, "ELSE") != 0) {
        O.st[O.depth - 1].body++;
    }
    O.stmts++;

    if (!strcmp(w, "LOAD")) {
        size_t k = oml_word(r, "SKILL");
        const char *e = k ? oml_quoted(r + k, name, sizeof(name)) : NULL;
        if (!e) {
            oml_err("LOAD reads: LOAD SKILL \"name\"  (optionally FROM \"./skills/name/SKILL.md\")");
            return 0;
        }
        if (O.depth) {
            oml_err("LOAD SKILL belongs at the top of the flow, not inside a block");
        }
        if (!O.has_skill(name)) {
            oml_err("LOAD SKILL \"%s\": there is no skills/%s/SKILL.md in this agent", name, name);
        }
        e = oml_skip(e);
        if ((k = oml_word(e, "FROM"))) {
            char path[300], want[300];
            const char *f = oml_quoted(e + k, path, sizeof(path));
            snprintf(want, sizeof(want), "skills/%s/SKILL.md", name);
            const char *pp = strncmp(path, "./", 2) == 0 ? path + 2 : path;
            if (!f || strcmp(pp, want) != 0) {
                oml_err("LOAD SKILL \"%s\" FROM must be \"./%s\"", name, want);
            }
        } else if (*e) {
            oml_err("unexpected text after LOAD SKILL \"%s\": %s", name, e);
        }
        if (oml_in(O.loaded, O.nloaded, name)) {
            oml_warn("skill \"%s\" is loaded twice", name);
        } else if (O.nloaded < OML_MAXVARS) {
            snprintf(O.loaded[O.nloaded++], 65, "%.64s", name);
        }
        O.stmts--;
        return 0;
    }
    if (!strcmp(w, "CONNECT")) {
        oml_err("CONNECT (MCP servers) is not supported by botcore yet; use the agent's own tools (tools/bin)");
        return 0;
    }
    if (!strcmp(w, "EXECUTE")) {
        size_t k = oml_word(r, "tool");
        const char *e = k ? oml_ticked(r + k, var) : NULL;
        if (!e) {
            oml_err("EXECUTE reads: EXECUTE tool `name` [with payload { ... } | with parameters:]");
            return 0;
        }
        if (!O.has_tool(var)) {
            oml_err("EXECUTE tool `%s`: no such tool (built-in, or tools/bin/%s with tools/doc/%s.json)", var, var, var);
        }
        oml_refs(e, 1);
        oml_define("result");
        return colon;
    }
    if (!strcmp(w, "INVOKE")) {
        size_t k = oml_word(r, "SKILL");
        const char *e = k ? oml_quoted(r + k, name, sizeof(name)) : NULL;
        if (!e) {
            oml_err("INVOKE reads: INVOKE SKILL \"name\" [USING context `x`]");
            return 0;
        }
        if (!oml_in(O.loaded, O.nloaded, name)) {
            oml_err("INVOKE SKILL \"%s\": add LOAD SKILL \"%s\" at the top of the flow first", name, name);
        }
        oml_refs(e, 1);
        oml_define("skill_output");
        return colon;
    }
    if (!strcmp(w, "SAVE")) {
        /* SAVE <what> INTO VARIABLE `v`   |   SAVE <what> TO FILE "path" */
        const char *into = NULL, *tofile = NULL;
        for (const char *p = r; *p; p++) {
            if ((p == r || p[-1] == ' ') && oml_word(p, "INTO") && oml_word(oml_skip(p + 4), "VARIABLE")) {
                into = p;
            }
            if ((p == r || p[-1] == ' ') && oml_word(p, "TO") && oml_word(oml_skip(p + 2), "FILE")) {
                tofile = p;
            }
        }
        const char *at = into ? into : tofile;
        if (!at || at == r) {
            oml_err("SAVE reads: SAVE result|skill_output|answer|`x` INTO VARIABLE `name`, or SAVE ... TO FILE \"path\"");
            return 0;
        }
        char what[300];
        snprintf(what, sizeof(what), "%.*s", (int)(at - r), r);
        static const char *const implicit[] = {"result", "skill_output", "answer"};
        for (int i = 0; i < 3; i++) {
            size_t n = strlen(implicit[i]);
            if (strncmp(what, implicit[i], n) == 0 && (what[n] == ' ' || what[n] == '\0' || what[n] == '.') &&
                !oml_in(O.vars, O.nvars, implicit[i])) {
                oml_err("SAVE %s: there is no %s yet (it comes from %s)", implicit[i], implicit[i],
                        i == 0 ? "EXECUTE tool" : i == 1 ? "INVOKE SKILL" : "ASK USER");
            }
        }
        oml_refs(what, 1);
        if (into) {
            const char *v = oml_skip(oml_skip(into + 4) + 8);
            const char *e = oml_ticked(v, var);
            if (!e || *oml_skip(e)) {
                oml_err("SAVE ... INTO VARIABLE needs one `name` (letters, digits, _)");
                return 0;
            }
            oml_define(var);
        } else {
            char path[300];
            const char *e = oml_quoted(oml_skip(tofile + 2) + 4, path, sizeof(path));
            if (!e || !path[0] || path[0] == '/' || strstr(path, "..")) {
                oml_err("SAVE ... TO FILE needs a \"relative/path\" inside the working directory");
            }
        }
        return 0;
    }
    if (!strcmp(w, "SET")) {
        const char *e = oml_ticked(r, var);
        size_t k = e ? oml_word(oml_skip(e), "TO") : 0;
        if (!e || !k || !*oml_skip(oml_skip(e) + k)) {
            oml_err("SET reads: SET `name` TO <value>");
            return 0;
        }
        oml_refs(oml_skip(e) + k, 1);
        oml_define(var);
        return 0;
    }
    if (!strcmp(w, "ASK")) {
        size_t k = oml_word(r, "USER");
        if (!k || !*oml_skip(r + k)) {
            oml_err("ASK reads: ASK USER \"question\"  (the reply is `answer`)");
            return 0;
        }
        oml_refs(r + k, 1);
        oml_define("answer");
        return 0;
    }
    if (!strcmp(w, "IF")) {
        if (!oml_ends(r, "THEN") || strlen(oml_skip(r)) <= 4) {
            oml_err("IF reads: IF <condition> THEN");
        }
        oml_refs(r, 0);
        oml_push(OB_IF, NULL);
        return 0;
    }
    if (!strcmp(w, "ELSE")) {
        int elif = oml_word(r, "IF") > 0;
        oml_block_t *b = O.depth ? &O.st[O.depth - 1] : NULL;
        if (!b || b->kind != OB_IF) {
            oml_err("%s without an open IF", elif ? "ELSE IF" : "ELSE");
        } else if (b->has_else) {
            oml_err("%s after ELSE: ELSE must be the last branch of an IF", elif ? "ELSE IF" : "ELSE");
        } else if (!b->body) {
            oml_warn("empty branch before %s", elif ? "ELSE IF" : "ELSE");
        }
        if (elif) {
            if (!oml_ends(r, "THEN")) {
                oml_err("ELSE IF reads: ELSE IF <condition> THEN");
            }
            oml_refs(r + 2, 0);
        } else if (*r) {
            oml_err("ELSE stands alone on its line (for another condition write ELSE IF <condition> THEN)");
        }
        if (b && b->kind == OB_IF) {
            b->has_else |= !elif;
            b->body = 0;
        }
        return 0;
    }
    if (!strcmp(w, "END")) {
        static const struct {
            const char *w;
            int         k;
        } ends[] = {{"IF", OB_IF}, {"FOR", OB_FOR}, {"WHILE", OB_WHILE}, {"RETRY", OB_RETRY}, {"PARALLEL", OB_PAR}};
        for (size_t i = 0; i < 5; i++) {
            size_t k = oml_word(r, ends[i].w);
            if (k && strncmp(r, ends[i].w, k) == 0) {
                if (*oml_skip(r + k)) {
                    oml_err("END %s stands alone on its line", ends[i].w);
                }
                oml_pop(ends[i].k);
                return 0;
            }
        }
        oml_err("END must be followed by IF, FOR, WHILE, RETRY or PARALLEL");
        return 0;
    }
    if (!strcmp(w, "FOR")) {
        size_t k = oml_word(r, "EACH");
        const char *e = k ? oml_ticked(r + k, var) : NULL;
        size_t in = e ? oml_word(oml_skip(e), "IN") : 0;
        if (!e || !in || !oml_ends(r, "DO")) {
            oml_err("FOR reads: FOR EACH `item` IN `collection` DO");
            oml_push(OB_FOR, NULL);
            return 0;
        }
        char coll[400];
        const char *c = oml_skip(oml_skip(e) + in);
        snprintf(coll, sizeof(coll), "%s", c);
        coll[strlen(coll) - 2] = '\0'; /* drop DO */
        oml_refs(coll, 1);
        oml_push(OB_FOR, var);
        oml_define(var);
        return 0;
    }
    if (!strcmp(w, "WHILE")) {
        if (!oml_ends(r, "DO")) {
            oml_err("WHILE reads: WHILE <condition> AT MOST N TIMES DO");
        }
        if (!strstr(r, "AT MOST")) {
            oml_warn("give the WHILE loop a bound: WHILE <condition> AT MOST N TIMES DO");
        }
        oml_refs(r, 0);
        oml_push(OB_WHILE, NULL);
        return 0;
    }
    if (!strcmp(w, "RETRY")) {
        size_t k = oml_word(r, "UP");
        const char *p = k ? oml_skip(r + k) : r;
        size_t t = k ? oml_word(p, "TO") : 0;
        if (k && t) {
            p = oml_skip(p + t);
        }
        char *end;
        long n = strtol(p, &end, 10);
        if (end == p || !oml_word(oml_skip(end), "TIMES") || !oml_ends(r, "DO")) {
            oml_err("RETRY reads: RETRY UP TO N TIMES DO");
        } else if (n < 1 || n > 20) {
            oml_err("RETRY UP TO %ld TIMES: N must be 1..20", n);
        }
        oml_push(OB_RETRY, NULL);
        return 0;
    }
    if (!strcmp(w, "IN")) {
        size_t k = oml_word(r, "PARALLEL");
        if (!k || strcmp(oml_skip(r + k), "DO") != 0) {
            oml_err("IN PARALLEL reads: IN PARALLEL DO");
        }
        oml_push(OB_PAR, NULL);
        return 0;
    }
    if (!strcmp(w, "RETURN")) {
        if (!*r) {
            oml_err("RETURN needs a value: RETURN \"text\" or RETURN `x` (or a plain-English summary)");
        }
        oml_refs(r, 1);
        O.returns++;
        return 0;
    }
    O.stmts--;
    oml_unknown(w);
    if (O.depth > 0) {
        O.st[O.depth - 1].body--;
    }
    /* a shouted plain-English line ("PDF files go to out/"): an instruction */
    oml_refs(s, 0);
    if (O.depth > 0) {
        O.st[O.depth - 1].body++;
    }
    O.stmts++;
    return 0;
}

/* The frontmatter: spec-version "oml-2" required, title recommended. Returns the offset after it. */
static size_t oml_front(const char *s, size_t n)
{
    O.line = 1;
    if (n < 4 || strncmp(s, "---", 3) != 0 || (s[3] != '\n' && s[3] != '\r')) {
        oml_err("flow.md must start with frontmatter: ---, spec-version: \"oml-2\", title: \"...\", ---");
        return 0;
    }
    int ver = 0, title = 0;
    size_t i = (size_t)(strchr(s, '\n') - s) + 1;
    while (i < n) {
        O.line++;
        const char *l = s + i;
        const char *nl = memchr(l, '\n', n - i);
        size_t ll = nl ? (size_t)(nl - l) : n - i;
        i += ll + (nl != NULL);
        if (ll >= 3 && strncmp(l, "---", 3) == 0) {
            if (!ver) {
                oml_err("the frontmatter needs spec-version: \"oml-2\"");
            }
            if (!title) {
                oml_warn("the frontmatter should have a title: \"...\"");
            }
            return i;
        }
        char line[400];
        snprintf(line, sizeof(line), "%.*s", (int)(ll < 399 ? ll : 399), l);
        line[strcspn(line, "\r")] = '\0';
        char *c = strchr(line, ':');
        if (!c) {
            if (*oml_skip(line)) {
                oml_err("frontmatter lines read key: value");
            }
            continue;
        }
        *c = '\0';
        const char *v = oml_skip(c + 1);
        char val[300];
        size_t vl = strlen(v);
        if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) {
            snprintf(val, sizeof(val), "%.*s", (int)(vl - 2), v + 1);
        } else {
            snprintf(val, sizeof(val), "%s", v);
        }
        if (!strcmp(line, "spec-version")) {
            ver = 1;
            if (strcmp(val, "oml-2") != 0) {
                oml_err("spec-version is \"%s\"; this botter understands \"oml-2\"", val);
            }
        } else if (!strcmp(line, "title")) {
            title = val[0] != '\0';
        } else if (strcmp(line, "author") != 0 && strcmp(line, "description") != 0) {
            oml_warn("unknown frontmatter key '%s' (known: spec-version, title, author, description)", line);
        }
    }
    oml_err("the frontmatter is not closed with ---");
    return n;
}

static void oml_check(const char *s, size_t n, int (*has_tool)(const char *), int (*has_skill)(const char *))
{
    memset(&O, 0, sizeof(O));
    O.has_tool = has_tool;
    O.has_skill = has_skill;
    if (n >= 11 && strncmp(s, "<!-- OML v1", 11) == 0) {
        O.line = 1;
        oml_err("this flow.md is OML v1; botter now uses OML v2 (frontmatter + UPPERCASE statements, see the "
                "write-flow skill): rewrite it");
        return;
    }
    size_t i = oml_front(s, n);
    if (!i) {
        return;
    }
    int params = 0, comment = 0, fence_warned = 0;
    while (i < n) {
        O.line++;
        const char *l = s + i;
        const char *nl = memchr(l, '\n', n - i);
        size_t ll = nl ? (size_t)(nl - l) : n - i;
        i += ll + (nl != NULL);
        char buf[2048];
        if (ll >= sizeof(buf)) {
            oml_err("line too long (max %zu characters)", sizeof(buf) - 1);
            continue;
        }
        memcpy(buf, l, ll);
        buf[ll] = '\0';
        buf[strcspn(buf, "\r")] = '\0';
        char *t = (char *)oml_skip(buf);
        for (size_t k = strlen(t); k && (t[k - 1] == ' ' || t[k - 1] == '\t'); k--) {
            t[k - 1] = '\0';
        }
        /* <!-- comments --> */
        if (comment || strncmp(t, "<!--", 4) == 0) {
            comment = strstr(t, "-->") == NULL;
            continue;
        }
        if (!*t) {
            continue;
        }
        if (strncmp(t, "```", 3) == 0) {
            if (!fence_warned++) {
                oml_err("OML v2 is written directly in flow.md, not inside a ``` code block");
            }
            continue;
        }
        if (*t == '#') { /* headings are labels; "## STEP n: NAME" is numbered in order */
            const char *h = t;
            while (*h == '#') {
                h++;
            }
            h = oml_skip(h);
            if (strncmp(h, "STEP ", 5) == 0) {
                long k = strtol(h + 5, NULL, 10);
                if (k > 0 && k != O.step + 1 && strchr(h + 5, ':') && h[5] >= '0' && h[5] <= '9') {
                    oml_warn("STEP %ld follows STEP %d", k, O.step);
                }
                if (k > 0 && h[5] >= '0' && h[5] <= '9') {
                    O.step = (int)k;
                }
                if (O.depth) {
                    oml_warn("a STEP heading inside an open %s block (opened on line %d)",
                             OB_NAME[O.st[O.depth - 1].kind], O.st[O.depth - 1].line);
                }
            }
            params = 0;
            continue;
        }
        /* list markers: "1. " / "- " / "* " */
        int dash = (t[0] == '-' || t[0] == '*') && t[1] == ' ';
        if (dash) {
            t = (char *)oml_skip(t + 2);
        } else if (isdigit((unsigned char)t[0])) {
            char *e = t;
            while (isdigit((unsigned char)*e)) {
                e++;
            }
            if (*e == '.' && (e[1] == ' ' || e[1] == '\0')) {
                t = (char *)oml_skip(e + 1);
            }
        }
        if (dash && params) { /* "- name: value" under "with parameters:" */
            char *c = strchr(t, ':');
            if (!c || !oml_ident(t, (size_t)(c - t))) {
                oml_err("a parameter line reads: - name: value");
            } else {
                oml_refs(c + 1, 1);
            }
            continue;
        }
        if (!*t) {
            continue;
        }
        params = oml_stmt(t);
    }
    while (O.depth > 0) {
        oml_block_t *b = &O.st[--O.depth];
        O.line = b->line;
        oml_err("this %s is never closed (add END %s)", b->kind == OB_FOR ? "FOR EACH" : OB_NAME[b->kind],
                OB_NAME[b->kind]);
    }
    if (!O.stmts) {
        oml_err("the flow has no statements");
    } else if (!O.returns) {
        oml_warn("the flow never RETURNs: end it with RETURN and what the user gets");
    }
}
