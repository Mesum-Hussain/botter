/*
 * botter_pack: attach an agent project to a prebuilt botcore executable.
 *
 *   botter_pack build <agent-dir> <botcore|file.bot> <out.bot>
 *   botter_pack check <agent-dir> <botcore|file.bot>        (validate only)
 *   botter_pack list  <file.bot>
 *
 * Embeds (read-only) from <agent-dir>:
 *   agent.md, config.json, FLOW.md, skills/, tools/doc/ (recursive)   plain files
 *       (FLOW.md is optional: the session flow in Owl, checked by botcore/src/owl.c; errors stop the build)
 *       (config.json: how to build and run the agent, read by load_config/check_config:
 *        name, display_name, version, description, internet, skills, tools, requires, builder)
 *   tools/bin/<name>                                 tool executables: either a
 *       native ELF x86-64 binary (any language that compiles to one) or a script
 *       starting with "#!" (interpreter must exist on the machine running the agent).
 * Never embedded: tools/src, artifacts/, hidden files, symlinks, anything else.
 * Every tools/bin/<name> needs tools/doc/<name>.json (see botcore docs).
 *
 * Output format: botcore ELF + zero padding to 4096 + BLOB + FOOTER, with the
 * ELF's 32-byte pack reference ("BOTPKREF", u64 blob_off, u64 file_size, u64 0)
 * patched so botcore can detect a truncated .bot. The format
 * is defined by botcore (src/blob.h); this file carries its own copy of it.
 * Plain C11/POSIX, no dependencies.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"

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
static cJSON  *g_cfg; /* the project's config.json (NULL if absent or invalid) */

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

static int has_item(const char *path)
{
    for (size_t i = 0; i < n_items; i++) {
        if (strcmp(items[i].path, path) == 0) {
            return 1;
        }
    }
    return 0;
}

#include "../botcore/src/owl.c"

/* Owl diagnostics in compiler form: FLOW.md:LINE: error|warning: message */
static void owl_diag(void *ud, int line, int error, const char *msg)
{
    (void)ud;
    fprintf(stderr, "FLOW.md:%d: %s: %s\n", line, error ? "error" : "warning", msg);
    if (error) {
        errors++;
    } else {
        warnings++;
    }
}

/* Names Owl's EXECUTE tool may use: botcore's built-ins and this agent's tools/bin. */
static int owl_has_tool(const char *name)
{
    static const char *const builtin[] = {"fs_list",  "fs_read",   "fs_write",  "shell_exec", "vfs_list", "vfs_read",
                                          "get_time", "cron_set",  "cron_list", "cron_delete", NULL};
    for (int i = 0; builtin[i]; i++) {
        if (strcmp(builtin[i], name) == 0) {
            return 1;
        }
    }
    if (strcmp(name, "owl_review") == 0) { /* only for agents that build agents: config.json "builder" */
        return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g_cfg, "builder"));
    }
    char p[300];
    snprintf(p, sizeof(p), "tools/bin/%s", name);
    return has_item(p);
}

static int owl_has_skill(const char *name)
{
    char p[300];
    snprintf(p, sizeof(p), "skills/%s/SKILL.md", name);
    return has_item(p);
}

static char g_core_version[32]; /* the runtime's BOTCORE_VERSION tag, "" if unknown */

static int name_ok(const char *n)
{
    size_t l = strlen(n);
    if (l == 0 || l > 64 || !(islower((unsigned char)n[0]) || isdigit((unsigned char)n[0]))) {
        return 0;
    }
    for (size_t i = 0; i < l; i++) {
        if (!islower((unsigned char)n[i]) && !isdigit((unsigned char)n[i]) && !strchr("-_.", n[i])) {
            return 0;
        }
    }
    return 1;
}

/* MAJOR.MINOR.PATCH, numbers without leading zeros, optional -pre / +build. */
static int version_ok(const char *v)
{
    for (int part = 0; part < 3; part++) {
        if (!isdigit((unsigned char)*v) || (v[0] == '0' && isdigit((unsigned char)v[1]))) {
            return 0;
        }
        while (isdigit((unsigned char)*v)) {
            v++;
        }
        if (part < 2 && *v++ != '.') {
            return 0;
        }
    }
    if (*v == '-' || *v == '+') {
        for (v++; *v; v++) {
            if (!isalnum((unsigned char)*v) && !strchr(".-+", *v)) {
                return 0;
            }
        }
    }
    return *v == '\0';
}

static void cfg_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void cfg_err(const char *fmt, ...)
{
    char m[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    err("config.json: %s%s", m, NULL);
}

static void cfg_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void cfg_warn(const char *fmt, ...)
{
    char m[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    warn("config.json: %s", m);
}

/* A list of names ("skills", "tools", "requires"): 1 if valid (or absent). */
static int cfg_list_ok(const char *key, int (*valid)(const char *))
{
    const cJSON *l = cJSON_GetObjectItemCaseSensitive(g_cfg, key), *e;
    if (!l) {
        return 1;
    }
    int ok = cJSON_IsArray(l);
    cJSON_ArrayForEach(e, (ok ? l : NULL))
    {
        ok &= cJSON_IsString(e) && valid(e->valuestring);
    }
    if (!ok) {
        cfg_err("\"%s\" must be a list of names", key);
    }
    return ok;
}

static int cmd_name_ok(const char *c)
{
    return *c && !strpbrk(c, " /;|&$`'\"\t");
}

static int tool_name_ok(const char *c)
{
    return *c && strlen(c) <= 64 && strspn(c, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == strlen(c);
}

static int in_cfg_list(const char *key, const char *name)
{
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(g_cfg, key))
    {
        if (cJSON_IsString(e) && !strcmp(e->valuestring, name)) {
            return 1;
        }
    }
    return 0;
}

/*
 * config.json: how to build and run the agent.
 *   {"name": "lead-outreach", "display_name": "Lead Outreach", "version": "0.1.0",
 *    "description": "...", "internet": true, "skills": [...], "tools": [...],
 *    "requires": ["python3"], "builder": false}
 * Read before the files are collected: "skills" / "tools" choose what is compiled in.
 */
static void load_config(const char *dir)
{
    char *old = join(dir, "agent.json");
    if (access(old, F_OK) == 0) {
        err("%s: agent.json is now config.json; rename it (see the project-layout skill for its fields)%s", old, NULL);
    }
    free(old);
    char *f = join(dir, "config.json");
    size_t n = 0;
    char *d = access(f, F_OK) == 0 ? slurp(f, &n) : NULL;
    free(f);
    if (!d) {
        warn("no config.json: add one with the agent's %s (see the project-layout skill)", "name, version and description");
        return;
    }
    g_cfg = cJSON_ParseWithLength(d, n);
    free(d);
    if (!cJSON_IsObject(g_cfg)) {
        err("config.json is not a valid JSON object%s%s", g_cfg ? "" : " (syntax error)", NULL);
        cJSON_Delete(g_cfg);
        g_cfg = NULL;
        return;
    }
    static const char *const strs[] = {"name", "display_name", "version", "description", NULL};
    static const char *const bools[] = {"internet", "builder", NULL};
    static const char *const lists[] = {"skills", "tools", "requires", NULL};
    for (const cJSON *x = g_cfg->child; x; x = x->next) {
        int known = 0;
        for (int k = 0; strs[k]; k++) {
            if (!strcmp(x->string, strs[k])) {
                known = 1;
                if (!cJSON_IsString(x)) {
                    cfg_err("\"%s\" must be a string", x->string);
                }
            }
        }
        for (int k = 0; bools[k]; k++) {
            if (!strcmp(x->string, bools[k])) {
                known = 1;
                if (!cJSON_IsBool(x)) {
                    cfg_err("\"%s\" must be true or false", x->string);
                }
            }
        }
        for (int k = 0; lists[k]; k++) {
            known |= !strcmp(x->string, lists[k]);
        }
        if (!known) {
            cfg_warn("unknown key \"%.60s\" (known: name, display_name, version, description, internet, skills, tools, "
                     "requires, builder)", x->string);
        }
    }
    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(g_cfg, "name");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(g_cfg, "version");
    const cJSON *ds = cJSON_GetObjectItemCaseSensitive(g_cfg, "description");
    if (!nm) {
        cfg_warn("no \"name\" (%s)", "lowercase letters, digits, - _ .; usually the folder name");
    } else if (cJSON_IsString(nm) && !name_ok(nm->valuestring)) {
        cfg_err("name \"%s\" must be 1-64 lowercase letters, digits, - _ . (starting with a letter or digit); put the "
                "spelling people see in \"display_name\"", nm->valuestring);
    }
    if (!v) {
        cfg_warn("no \"version\" (%s)", "MAJOR.MINOR.PATCH, e.g. \"0.1.0\"");
    } else if (cJSON_IsString(v) && !version_ok(v->valuestring)) {
        cfg_err("version \"%s\" is not MAJOR.MINOR.PATCH (e.g. 1.0.0)", v->valuestring);
    }
    if (!ds) {
        cfg_warn("no \"description\" (%s)", "one sentence: what the agent does and for whom");
    } else if (cJSON_IsString(ds) && strlen(ds->valuestring) > 300) {
        cfg_warn("keep the description to one or two sentences (%s)", "300 characters");
    }
    cfg_list_ok("skills", tool_name_ok);
    cfg_list_ok("tools", tool_name_ok);
    cfg_list_ok("requires", cmd_name_ok);
}

/*
 * After the files are collected: "skills" / "tools" (when given) decide which are
 * compiled in; listed ones must exist, unlisted ones are left out with a warning.
 */
static void select_from_config(void)
{
    const cJSON *sk = cJSON_GetObjectItemCaseSensitive(g_cfg, "skills");
    const cJSON *tl = cJSON_GetObjectItemCaseSensitive(g_cfg, "tools");
    char seen[1024] = "";
    size_t w = 0;
    for (size_t i = 0; i < n_items; i++) {
        const char *p = items[i].path;
        char nm[200] = "";
        const char *key = NULL;
        if (cJSON_IsArray(sk) && !strncmp(p, "skills/", 7)) {
            snprintf(nm, sizeof(nm), "%.*s", (int)strcspn(p + 7, "/"), p + 7);
            key = "skills";
        } else if (cJSON_IsArray(tl) && !strncmp(p, "tools/bin/", 10)) {
            snprintf(nm, sizeof(nm), "%s", p + 10);
            key = "tools";
        } else if (cJSON_IsArray(tl) && !strncmp(p, "tools/doc/", 10)) {
            snprintf(nm, sizeof(nm), "%.*s", (int)strcspn(p + 10, "."), p + 10);
            key = "tools";
        }
        if (key && !in_cfg_list(key, nm)) {
            char tag[260];
            snprintf(tag, sizeof(tag), "|%s/%s|", key, nm);
            if (!strstr(seen, tag) && strlen(seen) + strlen(tag) < sizeof(seen)) {
                strcat(seen, tag);
                char m[300];
                snprintf(m, sizeof(m), "%s \"%s\" is not listed in config.json \"%s\"; it is left out", key, nm, key);
                warn("%s", m);
            }
            free(items[i].path);
            free(items[i].full);
            continue;
        }
        items[w++] = items[i];
    }
    n_items = w;
    const cJSON *e;
    cJSON_ArrayForEach(e, (cJSON_IsArray(sk) ? sk : NULL))
    {
        char p[300];
        snprintf(p, sizeof(p), "skills/%s/SKILL.md", cJSON_IsString(e) ? e->valuestring : "");
        if (!has_item(p)) {
            cfg_err("skill \"%s\" is listed but there is no %s", cJSON_IsString(e) ? e->valuestring : "?", p);
        }
    }
    cJSON_ArrayForEach(e, (cJSON_IsArray(tl) ? tl : NULL))
    {
        char p[300];
        snprintf(p, sizeof(p), "tools/bin/%s", cJSON_IsString(e) ? e->valuestring : "");
        if (!has_item(p)) {
            cfg_err("tool \"%s\" is listed but there is no %s", cJSON_IsString(e) ? e->valuestring : "?", p);
        }
    }
}

/* Script tools need their interpreter on the machine: it belongs in "requires". */
static void check_requires(void)
{
    for (size_t i = 0; i < n_items; i++) {
        if (items[i].kind != K_SCRIPT || !items[i].data) {
            continue;
        }
        const char *l = items[i].data + 2, *nl = strchr(l, '\n');
        char line[300];
        snprintf(line, sizeof(line), "%.*s", (int)(nl ? nl - l : (long)strlen(l)), l);
        char *tok = strtok(line, " \t\r");
        if (tok && !strcmp(tok, "/usr/bin/env")) {
            tok = strtok(NULL, " \t\r");
            if (tok && !strcmp(tok, "-S")) {
                tok = strtok(NULL, " \t\r");
            }
        } else if (tok && strrchr(tok, '/')) {
            tok = strrchr(tok, '/') + 1;
        }
        if (tok && strcmp(tok, "sh") != 0 && strcmp(tok, "bash") != 0 && !in_cfg_list("requires", tok)) {
            char m[300];
            snprintf(m, sizeof(m), "%s runs with %s: add \"%s\" to \"requires\"", items[i].path, tok, tok);
            cfg_warn("%s", m);
        }
    }
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

/* ---- contracts the build can see: descriptors, skill frontmatter, tool scripts ---- */

static void diagf(int error, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void diagf(int error, const char *fmt, ...)
{
    char    m[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    if (error) {
        err("%s%s", m, NULL);
    } else {
        warn("%s", m);
    }
}

/* tools/doc/<name>.json: the model only learns a tool's inputs from "parameters". */
static void check_descriptor(const item_t *it)
{
    static const char *const known[] = {"description", "parameters", "timeout_s", "network", "readonly", "status", NULL};
    cJSON *j = cJSON_ParseWithLength(it->data, it->len);
    if (!cJSON_IsObject(j)) {
        diagf(1, "%s is not a JSON object", it->path);
        cJSON_Delete(j);
        return;
    }
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "description");
    const cJSON *par = cJSON_GetObjectItemCaseSensitive(j, "parameters");
    if (!cJSON_IsString(d) || !d->valuestring[0]) {
        diagf(1, "%s: no \"description\" (one sentence: what the tool does)", it->path);
    }
    if (!par) {
        diagf(1, "%s: no \"parameters\": the model would call the tool without arguments. Add a JSON Schema, e.g. "
                 "{\"type\": \"object\", \"properties\": {\"query\": {\"type\": \"string\", \"description\": \"...\"}}, "
                 "\"required\": [\"query\"]} ({\"type\": \"object\", \"properties\": {}} for a tool without input)",
              it->path);
    } else {
        const cJSON *ty = cJSON_GetObjectItemCaseSensitive(par, "type");
        const cJSON *pr = cJSON_GetObjectItemCaseSensitive(par, "properties");
        const cJSON *rq = cJSON_GetObjectItemCaseSensitive(par, "required"), *e;
        if (!cJSON_IsObject(par) || !cJSON_IsString(ty) || strcmp(ty->valuestring, "object") != 0 || !cJSON_IsObject(pr)) {
            diagf(1, "%s: \"parameters\" must be {\"type\": \"object\", \"properties\": {...}}", it->path);
        } else {
            cJSON_ArrayForEach(e, pr)
            {
                const cJSON *pd = cJSON_GetObjectItemCaseSensitive(e, "description");
                if (!cJSON_IsObject(e) || !cJSON_GetObjectItemCaseSensitive(e, "type")) {
                    diagf(1, "%s: parameter \"%s\" needs a \"type\"", it->path, e->string);
                } else if (!cJSON_IsString(pd) || !pd->valuestring[0]) {
                    diagf(0, "%s: parameter \"%s\" has no \"description\" (the model guesses what to pass)", it->path,
                          e->string);
                }
            }
            cJSON_ArrayForEach(e, rq)
            {
                if (!cJSON_IsString(e) || !cJSON_GetObjectItemCaseSensitive(pr, e->valuestring)) {
                    diagf(1, "%s: \"required\" names \"%s\", which is not in \"properties\"", it->path,
                          cJSON_IsString(e) ? e->valuestring : "?");
                }
            }
        }
    }
    const cJSON *k;
    cJSON_ArrayForEach(k, j)
    {
        int ok = 0;
        for (int i = 0; known[i]; i++) {
            ok |= strcmp(k->string, known[i]) == 0;
        }
        if (!ok) {
            diagf(0, "%s: unknown key \"%.60s\" is ignored (known: description, parameters, timeout_s, network, "
                     "readonly, status; prose docs go in tools/doc/<name>.md)",
                  it->path, k->string);
        }
    }
    cJSON_Delete(j);
}

/* skills/<name>/SKILL.md: the agent picks skills by the frontmatter description. */
static void check_skill(const item_t *it)
{
    const char *d = it->data, *end = it->data + it->len;
    const char *close = it->len > 4 && strncmp(d, "---\n", 4) == 0 ? memmem(d + 4, end - d - 4, "\n---", 4) : NULL;
    if (!close) {
        diagf(1, "%s has no frontmatter; start it with:\n---\nname: <folder name>\ndescription: <when to read this "
                 "skill, one sentence>\n---",
              it->path);
        return;
    }
    size_t fl = close - d;
    if (!memmem(d, fl, "\nname:", 6)) {
        diagf(1, "%s: frontmatter has no \"name:\"", it->path);
    }
    if (!memmem(d, fl, "\ndescription:", 13)) {
        diagf(1, "%s: frontmatter has no \"description:\" (the agent decides from it when to read the skill)", it->path);
    }
}

/* tools/bin scripts: arguments arrive as JSON on stdin; output must be real. */
static void check_script(const item_t *it)
{
    char *low = malloc(it->len + 1);
    if (!low) {
        return;
    }
    for (size_t i = 0; i < it->len; i++) {
        low[i] = (char)tolower((unsigned char)it->data[i]);
    }
    low[it->len] = '\0';
    const char *nl = memchr(low, '\n', it->len);
    int py = memmem(low, nl ? (size_t)(nl - low) : it->len, "python", 6) != NULL; /* the #! line */
    int reads = py ? strstr(low, "sys.stdin") || strstr(low, "input(") || strstr(low, "/dev/stdin")
                   : strstr(low, "read ") || strstr(low, "cat") || strstr(low, "jq") || strstr(low, "/dev/stdin") ||
                         strstr(low, "stdin") || strstr(low, "python");
    if (!reads) {
        diagf(0, "%s does not seem to read stdin: its arguments arrive as one JSON object on stdin (see write-tool)",
              it->path);
    }
    if (py ? strstr(low, "sys.argv[1") != NULL : strstr(it->data, "$1") != NULL) {
        diagf(0, "%s reads command-line arguments; botcore passes none (arguments arrive as JSON on stdin)", it->path);
    }
    int uses_art = 0; /* outside comments: a credit line naming the source is fine */
    for (const char *l = it->data, *end = it->data + it->len; l < end && !uses_art;) {
        const char *nl = memchr(l, '\n', (size_t)(end - l));
        size_t      n = nl ? (size_t)(nl - l) : (size_t)(end - l);
        size_t      k = strspn(l, " \t");
        if (k < n && l[k] != '#' && strncmp(l + k, "//", 2) != 0) {
            uses_art = memmem(l, n, "artifacts/", 10) != NULL;
        }
        l += n + 1;
    }
    if (uses_art) {
        diagf(1, "%s uses files under artifacts/, which are not part of the .bot (the agent must run in any "
                 "folder): copy the code it needs into the tool itself, keeping the licence notice",
              it->path);
    }
    if (strstr(low, "sleep")) { /* polls a job: the default 60 s timeout kills it mid-wait */
        char dp[200];
        snprintf(dp, sizeof(dp), "tools/doc/%s.json", it->path + 10);
        const cJSON *to = NULL;
        cJSON *d = NULL;
        for (size_t i = 0; i < n_items; i++) {
            if (strcmp(items[i].path, dp) == 0 && items[i].data) {
                d = cJSON_ParseWithLength(items[i].data, items[i].len);
                to = cJSON_GetObjectItemCaseSensitive(d, "timeout_s");
            }
        }
        if (!cJSON_IsNumber(to) || to->valuedouble <= 60) {
            diagf(0, "%s waits (sleep) but %s has no \"timeout_s\" above the default 60 s: set it to the "
                     "longest run it needs (max 600)",
                  it->path, dp);
        }
        cJSON_Delete(d);
    }
    static const char *const fake[] = {"mock", "placeholder", "dummy", "lorem ipsum", "hard-coded", "hardcoded",
                                       "simulated", "fake ", NULL};
    for (int i = 0; fake[i]; i++) {
        if (strstr(low, fake[i])) {
            diagf(0, "%s mentions \"%s\": tools must do the real job or fail with a clear error, never return "
                     "mock or placeholder output",
                  it->path, fake[i]);
            break;
        }
    }
    free(low);
}

static void check_contracts(void)
{
    for (size_t i = 0; i < n_items; i++) {
        const item_t *it = &items[i];
        size_t        l = strlen(it->path);
        if (!it->data) {
            continue;
        }
        if (strncmp(it->path, "tools/doc/", 10) == 0 && l > 15 && strcmp(it->path + l - 5, ".json") == 0) {
            check_descriptor(it);
        } else if (strncmp(it->path, "skills/", 7) == 0 && l > 9 && strcmp(it->path + l - 9, "/SKILL.md") == 0) {
            check_skill(it);
        } else if (it->kind == K_SCRIPT) {
            check_script(it);
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
    if (core) { /* the runtime's version (main.c BC_VERSION_TAG) */
        const char *tag = memmem(core, blen, "BOTCORE_VERSION=", 16);
        if (tag) {
            snprintf(g_core_version, sizeof(g_core_version), "%.31s", tag + 16);
        }
    }
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

    load_config(dir);
    add_root_file(dir, "agent.md");
    add_root_file(dir, "config.json");
    add_root_file(dir, "FLOW.md");
    add_tree(dir, "skills", K_DATA, 0);
    add_tree(dir, "tools/doc", K_DATA, 0);
    add_tree(dir, "tools/bin", K_DATA, 1);
    select_from_config();
    if (errors) {
        return 1;
    }
    qsort(items, n_items, sizeof(*items), cmp_item);
    load_and_classify();
    check_contracts();
    if (errors) {
        return 1;
    }
    check_requires();
    if (!has_item("agent.md")) {
        warn("no agent.md (%s: default system prompt will be used)", "agent");
    }
    char *mf = join(dir, "manifest.md");
    if (access(mf, F_OK) == 0) {
        warn("%s is no longer used (botcore lists skills and tools itself); it is not embedded, delete it", mf);
    }
    free(mf);
    char *oldflow = join(dir, "SQNC.md");
    if (access(oldflow, F_OK) == 0) {
        err("%s: SQNC.md is now FLOW.md and its fence is ```owl (see the write-flow skill)%s", oldflow, NULL);
    }
    free(oldflow);
    for (size_t i = 0; i < n_items; i++) {
        if (strcmp(items[i].path, "FLOW.md") == 0 && items[i].data) {
            sq_prog prog;
            memset(&prog, 0, sizeof(prog));
            prog.diag = owl_diag;
            if (sq_parse(items[i].data, items[i].len, &prog) == 0) {
                sq_check(&prog, owl_has_tool, owl_has_skill);
            }
            sq_free(&prog);
        }
    }
    if (errors) { /* a FLOW.md that does not compile is not built */
        fprintf(stderr, "botter_pack: FLOW.md has %d error(s); nothing was built\n", errors);
        return 1;
    }

    if (!out) { /* dry run: everything validated, write nothing */
        size_t nx = 0;
        for (size_t i = 0; i < n_items; i++) {
            nx += items[i].kind != K_DATA;
        }
        fprintf(stderr, "botter_pack: check passed (runtime botcore %s): %zu files (%zu tools), %d warning(s)\n",
                g_core_version[0] ? g_core_version : "?", n_items, nx, warnings);
        return 0;
    }

    /* layout */
    uint64_t pos = BLOB_HEADER_SIZE + (uint64_t)n_items * BLOB_ENTRY_SIZE;
    uint64_t *poff = calloc(n_items + 1, sizeof(*poff)), *doff = calloc(n_items + 1, sizeof(*doff));
    if (!poff || !doff) {
        die("out of memory");
    }
    size_t saved = 0;
    for (size_t i = 0; i < n_items; i++) {
        poff[i] = pos;
        pos += strlen(items[i].path) + 1;
        /* identical content (e.g. one multi-call tool binary under several names) is stored once */
        size_t j = 0;
        while (j < i && !(items[j].len == items[i].len && memcmp(items[j].data, items[i].data, items[i].len) == 0)) {
            j++;
        }
        if (j < i && items[i].len > 0) {
            doff[i] = doff[j];
            saved += items[i].len;
            continue;
        }
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

    /* pack reference: exactly one "BOTPKREF" in the runtime */
    unsigned char *ref = NULL;
    int nref = 0;
    for (size_t i = 0; i + 32 <= blen; i++) {
        if (core[i] == 'B' && memcmp(core + i, "BOTPKREF", 8) == 0) {
            ref = (unsigned char *)core + i;
            nref++;
        }
    }
    if (nref == 1) {
        put64(ref + 8, base);
        put64(ref + 16, base + pos + FOOTER_SIZE);
        put64(ref + 24, 0);
    } else {
        warn("%s has no unique pack reference (older botcore?): a truncated .bot will not be detected", botcore);
    }

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
    fprintf(stderr, "botter_pack: wrote %s (runtime botcore %s): %zu files (%zu tools), pack %llu bytes, total %llu bytes", out,
            g_core_version[0] ? g_core_version : "?", n_items,
            nexec, (unsigned long long)pos, (unsigned long long)(base + pos + FOOTER_SIZE));
    if (saved) {
        fprintf(stderr, " (%zu duplicate bytes stored once)", saved);
    }
    fputc('\n', stderr);
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
    for (uint32_t i = 0; i < cnt; i++) { /* config.json first */
        const unsigned char *e = b + BLOB_HEADER_SIZE + (size_t)i * BLOB_ENTRY_SIZE;
        if (strcmp((const char *)b + get32(e), "config.json") != 0) {
            continue;
        }
        cJSON *j = cJSON_ParseWithLength((const char *)b + get64(e + 8), (size_t)get64(e + 16));
        for (const cJSON *f = cJSON_IsObject(j) ? j->child : NULL; f; f = f->next) {
            char *t = cJSON_IsString(f) ? strdup(f->valuestring) : cJSON_PrintUnformatted(f);
            if (t) {
                printf("%-13s %s\n", f->string, t);
            }
            free(t);
        }
        cJSON_Delete(j);
    }
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
