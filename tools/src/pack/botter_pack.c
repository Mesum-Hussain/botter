/*
 * botter_pack: attach an agent project to a prebuilt botcore executable.
 *
 *   botter_pack build <agent-dir> <botcore|file.bot> <out.bot>
 *   botter_pack check <agent-dir> <botcore|file.bot>        (validate only)
 *   botter_pack list  <file.bot>
 *
 * Embeds (read-only) from <agent-dir>:
 *   manifest.md, agent.md, agent.json, flow.md, skills/, tools/doc/ (recursive)   plain files
 *       (flow.md is optional: the session flow in OML, linted for structure, see oml_lint)
 *       (agent.json is optional: {"offline": true} makes the agent offline; default online)
 *   tools/bin/<name>                                 tool executables: either a
 *       native ELF x86-64 binary (any language that compiles to one) or a script
 *       starting with "#!" (interpreter must exist on the machine running the agent).
 * Never embedded: tools/src, artifacts/, hidden files, symlinks, anything else.
 * Every tools/bin/<name> needs tools/doc/<name>.json (see botcore docs).
 *
 * Output format: botcore ELF + zero padding to 4096 + BLOB + FOOTER. The format
 * is defined by botcore (src/blob.h); this file carries its own copy of it.
 * Plain C11/POSIX, no dependencies.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLOB_MAGIC       "BOTBLOB1"
#define FOOTER_MAGIC     "BOTPACK1"
#define BLOB_HEADER_SIZE 16
#define BLOB_ENTRY_SIZE  32
#define FOOTER_SIZE      32
#define BLOB_ALIGN       4096
#define MAX_FILE         (256u * 1024 * 1024)

enum { K_DATA = 0, K_ELF = 1, K_SCRIPT = 2 };

typedef struct {
    char    *path;
    char    *full;
    char    *data;
    size_t   len;
    uint32_t kind;
} item_t;

static item_t *items;
static size_t  n_items, cap_items;
static int     errors, warnings;

static void die(const char *msg)
{
    fprintf(stderr, "botter_pack: %s\n", msg);
    exit(1);
}

static void err(const char *fmt, const char *a, const char *b)
{
    fprintf(stderr, "botter_pack: error: ");
    fprintf(stderr, fmt, a, b ? b : "");
    fputc('\n', stderr);
    errors++;
}

static void warn(const char *fmt, const char *a)
{
    fprintf(stderr, "botter_pack: warning: ");
    fprintf(stderr, fmt, a);
    fputc('\n', stderr);
    warnings++;
}

static char *xstrdup(const char *s)
{
    char *d = strdup(s);
    if (!d) {
        die("out of memory");
    }
    return d;
}

static char *join(const char *a, const char *b)
{
    size_t n = strlen(a) + strlen(b) + 2;
    char  *r = malloc(n);
    if (!r) {
        die("out of memory");
    }
    snprintf(r, n, "%s/%s", a, b);
    return r;
}

static void put32(unsigned char *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (unsigned char)(v >> (8 * i));
    }
}

static void put64(unsigned char *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (unsigned char)(v >> (8 * i));
    }
}

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const unsigned char *p)
{
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

static uint32_t crc32_ieee(const unsigned char *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        err("cannot read %s: %s", path, strerror(errno));
        return NULL;
    }
    size_t cap = 4096, n = 0;
    char  *buf = malloc(cap + 1);
    if (!buf) {
        die("out of memory");
    }
    size_t r;
    while ((r = fread(buf + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n > MAX_FILE) {
            err("%s is too large", path, NULL);
            free(buf);
            fclose(f);
            return NULL;
        }
        if (n == cap) {
            cap *= 2;
            buf = realloc(buf, cap + 1);
            if (!buf) {
                die("out of memory");
            }
        }
    }
    int bad = ferror(f);
    fclose(f);
    if (bad) {
        err("read error on %s", path, NULL);
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

/* ---- ELF inspection (x86-64 only) ---- */

/* returns 0 = not ELF64 LE, 1 = ELF; sets *x86 and *dynamic */
static int elf_info(const unsigned char *d, size_t n, int *x86, int *dynamic)
{
    *x86 = 0;
    *dynamic = 0;
    if (n < 64 || memcmp(d, "\x7f" "ELF", 4) != 0 || d[4] != 2 || d[5] != 1) {
        return 0;
    }
    *x86 = (d[18] | (d[19] << 8)) == 62;
    uint64_t phoff = get64(d + 32);
    unsigned phentsize = d[54] | (d[55] << 8), phnum = d[56] | (d[57] << 8);
    for (unsigned i = 0; i < phnum; i++) {
        uint64_t o = phoff + (uint64_t)i * phentsize;
        if (phentsize < 4 || o + phentsize > n) {
            break;
        }
        if (get32(d + o) == 3) { /* PT_INTERP */
            *dynamic = 1;
        }
    }
    return 1;
}

/* ---- collecting files ---- */

static int bad_name(const char *name)
{
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 0x20 || *p == 0x7f || *p == '\\' || *p == '"') {
            return 1;
        }
    }
    return 0;
}

static int valid_tool_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 64) {
        return 0;
    }
    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_' ||
              *s == '-')) {
            return 0;
        }
    }
    return 1;
}

static void add_item(const char *rel, const char *full, uint32_t kind)
{
    if (n_items == cap_items) {
        cap_items = cap_items ? cap_items * 2 : 32;
        items = realloc(items, cap_items * sizeof(*items));
        if (!items) {
            die("out of memory");
        }
    }
    item_t *it = &items[n_items++];
    memset(it, 0, sizeof(*it));
    it->path = xstrdup(rel);
    it->full = xstrdup(full);
    it->kind = kind;
}

static void walk(const char *rel, const char *full, uint32_t kind, int flat)
{
    DIR *d = opendir(full);
    if (!d) {
        err("cannot open %s: %s", full, strerror(errno));
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char *r = join(rel, e->d_name), *f = join(full, e->d_name);
        struct stat st;
        if (bad_name(e->d_name)) {
            err("unsupported file name under %s (control char, quote or backslash)", rel, NULL);
        } else if (lstat(f, &st) != 0) {
            err("cannot stat %s: %s", f, strerror(errno));
        } else if (S_ISDIR(st.st_mode)) {
            if (flat) {
                warn("ignoring sub-directory %s (tools/bin must be flat)", r);
            } else {
                walk(r, f, kind, flat);
            }
        } else if (S_ISREG(st.st_mode)) {
            add_item(r, f, kind);
        } else {
            warn("skipping non-regular file %s", r);
        }
        free(r);
        free(f);
    }
    closedir(d);
}

static void add_root_file(const char *dir, const char *rel)
{
    char *f = join(dir, rel);
    struct stat st;
    if (lstat(f, &st) == 0 && S_ISREG(st.st_mode)) {
        add_item(rel, f, K_DATA);
    }
    free(f);
}

static void add_tree(const char *dir, const char *rel, uint32_t kind, int flat)
{
    char *f = join(dir, rel);
    struct stat st;
    if (lstat(f, &st) == 0 && S_ISDIR(st.st_mode)) {
        walk(rel, f, kind, flat);
    }
    free(f);
}

static int cmp_item(const void *a, const void *b)
{
    return strcmp(((const item_t *)a)->path, ((const item_t *)b)->path);
}

/*
 * OML v1 structure check for flow.md (warnings only: conditions and actions are
 * plain English, so only the shape is checked). Rules: the file starts with the
 * <!-- OML v1 ... --> description; the OML code sits in a ```oml fenced block
 * (so markdown keeps its indentation); 4-space indentation, no tabs; a line ending in
 * ':' opens a block whose body is indented exactly 4 more; if/elif/else/for/while
 * lines end in ':'; elif/else follow an unfinished if at the same depth;
 * for-lines read "for <item> in <collection>:".
 */
#define OML_MAXD 64
static void oml_warn(int line, const char *msg)
{
    char m[200];
    snprintf(m, sizeof(m), "flow.md line %d: %s", line, msg);
    warn("%s", m);
}

static void oml_lint_code(const char *s, size_t n, int line);

static void oml_lint(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) {
        i++;
    }
    if (n - i < 11 || strncmp(s + i, "<!-- OML v1", 11) != 0) {
        warn("flow.md does not start with the %s description comment", "<!-- OML v1 ... -->");
    }
    /* find the ```oml fence; lint only its body */
    int line = 0;
    for (size_t p = 0; p < n;) {
        size_t e = p;
        while (e < n && s[e] != '\n') {
            e++;
        }
        line++;
        size_t q = p;
        while (q < e && s[q] == ' ') {
            q++;
        }
        if (e - q >= 6 && (strncmp(s + q, "```oml", 6) == 0 || strncmp(s + q, "```OML", 6) == 0)) {
            size_t b = e < n ? e + 1 : n, c = b;
            while (c < n) {
                size_t ce = c;
                while (ce < n && s[ce] != '\n') {
                    ce++;
                }
                size_t cq = c;
                while (cq < ce && s[cq] == ' ') {
                    cq++;
                }
                if (ce - cq >= 3 && strncmp(s + cq, "```", 3) == 0) {
                    break;
                }
                c = ce < n ? ce + 1 : n;
            }
            if (c >= n) {
                warn("flow.md: the %s block is not closed with ```", "```oml");
            }
            oml_lint_code(s + b, c - b, line);
            return;
        }
        p = e + 1;
    }
    warn("flow.md: put the OML code inside a %s fenced block, or markdown will mangle its indentation", "```oml");
    oml_lint_code(s, n, 0);
}

/* `line` = file line number just before s (for messages). */
static void oml_lint_code(const char *s, size_t n, int line)
{
    int open_if[OML_MAXD] = {0};
    int in_comment = 0, want_body = -1, prev_indent = 0, opener_line = 0;
    for (size_t p = 0; p < n;) {
        size_t e = p;
        while (e < n && s[e] != '\n') {
            e++;
        }
        line++;
        char buf[512];
        size_t len = e - p < sizeof(buf) - 1 ? e - p : sizeof(buf) - 1;
        memcpy(buf, s + p, len);
        buf[len] = '\0';
        p = e + 1;
        while (len && (buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
            buf[--len] = '\0';
        }
        char *t = buf;
        if (in_comment || strstr(t, "<!--")) {
            in_comment = strstr(t, "-->") == NULL;
            continue;
        }
        int ind = 0, tab = 0;
        while (t[ind] == ' ' || t[ind] == '\t') {
            tab |= t[ind] == '\t';
            ind++;
        }
        t += ind;
        if (!*t || (ind == 0 && *t == '#')) {
            continue; /* blank line or markdown heading */
        }
        if (tab) {
            oml_warn(line, "use spaces, not tabs, for indentation");
            continue;
        }
        if (ind % 4) {
            oml_warn(line, "indentation must be a multiple of 4 spaces");
        }
        int d = ind / 4 < OML_MAXD ? ind / 4 : OML_MAXD - 1;
        if (want_body >= 0) {
            if (ind != want_body) {
                oml_warn(opener_line, "a line ending in ':' must be followed by a body indented 4 more spaces");
            }
            want_body = -1;
        } else if (ind > prev_indent) {
            oml_warn(line, "unexpected indent (only a line ending in ':' opens a block)");
        }
        for (int k = d + 1; k < OML_MAXD; k++) {
            open_if[k] = 0;
        }
        char w[16] = {0};
        size_t wl = strcspn(t, " :");
        if (wl < sizeof(w)) {
            memcpy(w, t, wl);
        }
        int colon = len && buf[len - 1] == ':';
        if (!strcmp(w, "if") || !strcmp(w, "elif") || !strcmp(w, "else") || !strcmp(w, "for") || !strcmp(w, "while")) {
            if (!colon) {
                oml_warn(line, "if/elif/else/for/while lines must end with ':'");
            }
        }
        if (!strcmp(w, "elif") || !strcmp(w, "else")) {
            if (!open_if[d]) {
                oml_warn(line, "elif/else without an unfinished 'if' at the same indentation");
            }
            open_if[d] = !strcmp(w, "elif");
        } else {
            open_if[d] = !strcmp(w, "if");
        }
        if (!strcmp(w, "for") && !strstr(t, " in ")) {
            oml_warn(line, "a for-line reads: for <item> in <collection>:");
        }
        if (colon) {
            want_body = ind + 4;
            opener_line = line;
        }
        prev_indent = ind;
    }
    if (want_body >= 0) {
        oml_warn(opener_line, "a line ending in ':' must be followed by a body indented 4 more spaces");
    }
}

static int has_item(const char *path)
{
    for (size_t i = 0; i < n_items; i++) {
        if (strcmp(items[i].path, path) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Load every item; classify tools/bin entries as ELF or script. */
static void load_and_classify(void)
{
    const char *binp = "tools/bin/";
    size_t      bl = strlen(binp);
    for (size_t i = 0; i < n_items; i++) {
        item_t *it = &items[i];
        it->data = slurp(it->full, &it->len);
        if (!it->data) {
            continue;
        }
        if (strncmp(it->path, binp, bl) != 0) {
            continue; /* plain data */
        }
        const char *name = it->path + bl;
        if (!valid_tool_name(name)) {
            err("tools/bin/%s: tool names must be 1-64 chars of [A-Za-z0-9_-]", name, NULL);
            continue;
        }
        int x86, dyn;
        if (elf_info((unsigned char *)it->data, it->len, &x86, &dyn)) {
            it->kind = K_ELF;
            if (!x86) {
                err("tools/bin/%s is an ELF binary but not for x86-64", name, NULL);
            } else if (dyn) {
                warn("tools/bin/%s is dynamically linked; it only runs where its shared libraries exist "
                     "(link it statically for a portable agent)",
                     name);
            }
        } else if (it->len >= 3 && it->data[0] == '#' && it->data[1] == '!') {
            it->kind = K_SCRIPT;
        } else {
            err("tools/bin/%s is neither an ELF executable nor a script starting with \"#!\"", name, NULL);
        }
        char dp[160];
        snprintf(dp, sizeof(dp), "tools/doc/%s.json", name);
        if (!has_item(dp)) {
            err("tools/bin/%s has no descriptor %s", name, dp);
        }
    }
    /* descriptor without executable: harmless but almost surely a mistake */
    for (size_t i = 0; i < n_items; i++) {
        const char *p = items[i].path;
        size_t      l = strlen(p);
        if (strncmp(p, "tools/doc/", 10) == 0 && l > 15 && strcmp(p + l - 5, ".json") == 0) {
            char bp[160];
            snprintf(bp, sizeof(bp), "tools/bin/%.*s", (int)(l - 15), p + 10);
            if (!has_item(bp)) {
                warn("%s has no matching executable in tools/bin (tool will not exist)", p);
            }
        }
    }
}

/* ---- output ---- */

static int write_all(FILE *f, const void *d, size_t n)
{
    return fwrite(d, 1, n, f) == n;
}

static int cmd_build(const char *dir, const char *botcore, const char *out)
{
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        err("%s is not a directory", dir, NULL);
        return 1;
    }
    size_t blen = 0;
    char  *core = slurp(botcore, &blen);
    if (!core) {
        return 1;
    }
    int x86, dyn;
    if (!elf_info((unsigned char *)core, blen, &x86, &dyn) || !x86) {
        err("%s is not an x86-64 ELF executable", botcore, NULL);
        return 1;
    }
    if (blen >= FOOTER_SIZE && memcmp(core + blen - 8, FOOTER_MAGIC, 8) == 0) {
        /* A finished .bot (e.g. the running agent itself) is accepted as the base:
         * the runtime is everything in front of its pack. */
        const unsigned char *ft = (const unsigned char *)core + blen - FOOTER_SIZE;
        uint64_t             boff = get64(ft), plen = get64(ft + 8);
        if (boff % BLOB_ALIGN != 0 || boff < 64 || boff + plen + FOOTER_SIZE != blen) {
            err("%s has a damaged agent pack footer", botcore, NULL);
            return 1;
        }
        blen = (size_t)boff;
    }
    if (dyn) {
        warn("%s is dynamically linked; the .bot will need the same libc on the target (use the static build)",
             botcore);
    }

    add_root_file(dir, "manifest.md");
    add_root_file(dir, "agent.md");
    add_root_file(dir, "agent.json");
    add_root_file(dir, "flow.md");
    add_tree(dir, "skills", K_DATA, 0);
    add_tree(dir, "tools/doc", K_DATA, 0);
    add_tree(dir, "tools/bin", K_DATA, 1);
    if (errors) {
        return 1;
    }
    qsort(items, n_items, sizeof(*items), cmp_item);
    load_and_classify();
    if (errors) {
        return 1;
    }
    if (!has_item("agent.md")) {
        warn("no agent.md (%s: default system prompt will be used)", "agent");
    }
    if (!has_item("manifest.md")) {
        warn("no manifest.md (%s)", "the agent has no map of its files");
    }
    for (size_t i = 0; i < n_items; i++) {
        if (strcmp(items[i].path, "flow.md") == 0 && items[i].data) {
            oml_lint(items[i].data, items[i].len);
        }
    }

    if (!out) { /* dry run: everything validated, write nothing */
        size_t nx = 0;
        for (size_t i = 0; i < n_items; i++) {
            nx += items[i].kind != K_DATA;
        }
        fprintf(stderr, "botter_pack: check passed: %zu files (%zu tools), %d warning(s)\n", n_items, nx, warnings);
        return 0;
    }

    /* layout */
    uint64_t pos = BLOB_HEADER_SIZE + (uint64_t)n_items * BLOB_ENTRY_SIZE;
    uint64_t *poff = calloc(n_items + 1, sizeof(*poff)), *doff = calloc(n_items + 1, sizeof(*doff));
    if (!poff || !doff) {
        die("out of memory");
    }
    for (size_t i = 0; i < n_items; i++) {
        poff[i] = pos;
        pos += strlen(items[i].path) + 1;
        doff[i] = pos;
        pos += items[i].len + 1;
    }
    if (pos > 0xFFFFFFFFull * 4) {
        die("agent pack too large");
    }
    unsigned char *blob = calloc(1, (size_t)pos);
    if (!blob) {
        die("out of memory");
    }
    memcpy(blob, BLOB_MAGIC, 8);
    put32(blob + 8, (uint32_t)n_items);
    for (size_t i = 0; i < n_items; i++) {
        unsigned char *e = blob + BLOB_HEADER_SIZE + i * BLOB_ENTRY_SIZE;
        size_t         pl = strlen(items[i].path);
        put32(e, (uint32_t)poff[i]);
        put32(e + 4, (uint32_t)pl);
        put64(e + 8, doff[i]);
        put64(e + 16, items[i].len);
        put32(e + 24, items[i].kind);
        memcpy(blob + poff[i], items[i].path, pl);
        memcpy(blob + doff[i], items[i].data, items[i].len);
    }
    uint64_t base = ((uint64_t)blen + BLOB_ALIGN - 1) / BLOB_ALIGN * BLOB_ALIGN;
    unsigned char ft[FOOTER_SIZE] = {0};
    put64(ft, base);
    put64(ft + 8, pos);
    put32(ft + 16, crc32_ieee(blob, (size_t)pos));
    memcpy(ft + 24, FOOTER_MAGIC, 8);

    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", out, (int)getpid());
    FILE *o = fopen(tmp, "wb");
    if (!o) {
        err("cannot write %s: %s", tmp, strerror(errno));
        return 1;
    }
    static const unsigned char zeros[BLOB_ALIGN];
    int ok = write_all(o, core, blen) && write_all(o, zeros, (size_t)(base - blen)) && write_all(o, blob, (size_t)pos) &&
             write_all(o, ft, sizeof(ft));
    ok = (fclose(o) == 0) && ok;
    if (!ok || chmod(tmp, 0755) != 0 || rename(tmp, out) != 0) {
        remove(tmp);
        err("failed to write %s", out, NULL);
        return 1;
    }
    size_t nexec = 0;
    for (size_t i = 0; i < n_items; i++) {
        nexec += items[i].kind != K_DATA;
    }
    fprintf(stderr, "botter_pack: wrote %s: %zu files (%zu tools), pack %llu bytes, total %llu bytes\n", out, n_items,
            nexec, (unsigned long long)pos, (unsigned long long)(base + pos + FOOTER_SIZE));
    return 0;
}

static int cmd_list(const char *file)
{
    size_t         n = 0;
    unsigned char *d = (unsigned char *)slurp(file, &n);
    if (!d) {
        return 1;
    }
    if (n < FOOTER_SIZE || memcmp(d + n - 8, FOOTER_MAGIC, 8) != 0) {
        printf("%s: no agent pack\n", file);
        return 1;
    }
    const unsigned char *ft = d + n - FOOTER_SIZE;
    uint64_t             boff = get64(ft), blen = get64(ft + 8);
    if (boff + blen + FOOTER_SIZE != n || blen < BLOB_HEADER_SIZE) {
        printf("%s: damaged footer\n", file);
        return 1;
    }
    if (crc32_ieee(d + boff, (size_t)blen) != get32(ft + 16)) {
        printf("%s: checksum mismatch\n", file);
        return 1;
    }
    const unsigned char *b = d + boff;
    uint32_t             cnt = get32(b + 8);
    static const char   *kn[] = {"data", "elf", "script"};
    printf("%s: %u entries, pack at offset %llu (%llu bytes)\n", file, cnt, (unsigned long long)boff,
           (unsigned long long)blen);
    for (uint32_t i = 0; i < cnt; i++) {
        const unsigned char *e = b + BLOB_HEADER_SIZE + (size_t)i * BLOB_ENTRY_SIZE;
        uint32_t             k = get32(e + 24);
        printf("  %-7s %9llu  %s\n", k < 3 ? kn[k] : "?", (unsigned long long)get64(e + 16), (const char *)b + get32(e));
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "build") == 0) {
        return cmd_build(argv[2], argv[3], argv[4]);
    }
    if (argc == 4 && strcmp(argv[1], "check") == 0) {
        return cmd_build(argv[2], argv[3], NULL);
    }
    if (argc == 3 && strcmp(argv[1], "list") == 0) {
        return cmd_list(argv[2]);
    }
    fprintf(stderr,
            "usage:\n  %s build <agent-dir> <botcore|file.bot> <out.bot>\n  %s check <agent-dir> <botcore|file.bot>\n"
            "  %s list <file.bot>\n",
            argv[0], argv[0], argv[0]);
    return 2;
}
