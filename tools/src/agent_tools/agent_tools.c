/*
 * agent_tools: the botter agent's own tools, one source, one executable that
 * behaves according to the name it is started under (the botcore tool runner
 * passes the tool name as argv[0]):
 *
 *   agent_manifest  refresh the generated block of <dir>/manifest.md
 *   agent_build     validate (dry_run) or build <dir> into a <name>.bot
 *   agent_inspect   list what is embedded in a .bot
 *
 * Tool protocol: arguments = one JSON object on stdin, result = stdout,
 * exit 0 = ok. Everything (including packer warnings) goes to stdout.
 *
 * Safety: there is no confirmation channel for external tools, so every path
 * the model supplies must resolve inside the working directory (symlink-safe).
 *
 * agent_build needs a botcore runtime to put the agent on. It uses the running
 * agent itself: the botter process is "botcore + pack", and everything in
 * front of its pack is the pristine runtime (see botter_pack.c). botcore hands
 * tools a read-only fd of its own executable (BOTCORE_RUNTIME_FD, fd 3): a
 * sandboxed tool cannot open /proc/<parent>/exe itself.
 * Build:  see ../../../Makefile  (static musl, links ../pack and cJSON)
 */
#define _GNU_SOURCE
#define main pack_main
#include "../pack/botter_pack.c"
#undef main

#include <libgen.h>
#include <limits.h>
#include <stdarg.h>
#include "cJSON.h"

/* ---------- small helpers ---------- */

static char *read_stdin(void)
{
    size_t cap = 4096, n = 0;
    char  *b = malloc(cap + 1);
    if (!b) {
        die("out of memory");
    }
    ssize_t r;
    while ((r = read(0, b + n, cap - n)) > 0) {
        n += (size_t)r;
        if (n == cap) {
            if (cap >= (1u << 20)) {
                die("arguments too large");
            }
            cap *= 2;
            b = realloc(b, cap + 1);
            if (!b) {
                die("out of memory");
            }
        }
    }
    b[n] = '\0';
    return b;
}

static const char *arg_str(const cJSON *a, const char *key, const char *def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(a, key);
    return cJSON_IsString(v) && v->valuestring[0] ? v->valuestring : def;
}

static char g_cwd[PATH_MAX];

/*
 * Resolve `p` (absolute or relative to cwd) to a canonical absolute path even
 * if its last components do not exist yet. Returns NULL if it ends up outside
 * the working directory. ".." below a non-existent prefix is rejected.
 */
static char *resolve_inside(const char *p)
{
    char abs[PATH_MAX * 2];
    if (strlen(p) >= PATH_MAX || strlen(g_cwd) + strlen(p) + 2 > sizeof(abs)) {
        return NULL;
    }
    if (p[0] == '/') {
        snprintf(abs, sizeof(abs), "%s", p);
    } else {
        snprintf(abs, sizeof(abs), "%s/%s", g_cwd, p);
    }
    char head[PATH_MAX * 2], tail[PATH_MAX * 4] = "";
    snprintf(head, sizeof(head), "%s", abs);
    char real[PATH_MAX];
    for (;;) {
        if (realpath(head, real)) {
            break;
        }
        char *sl = strrchr(head, '/');
        if (!sl || sl == head) {
            return NULL;
        }
        const char *comp = sl + 1;
        if (strcmp(comp, "..") == 0 || strcmp(comp, ".") == 0 || !*comp) {
            return NULL;
        }
        char t2[PATH_MAX * 4];
        snprintf(t2, sizeof(t2), "/%s%s", comp, tail);
        snprintf(tail, sizeof(tail), "%s", t2);
        *sl = '\0';
    }
    char full[PATH_MAX * 4];
    snprintf(full, sizeof(full), "%s%s", real, tail);
    size_t cl = strlen(g_cwd);
    if (strncmp(full, g_cwd, cl) != 0 || (full[cl] != '\0' && full[cl] != '/' && cl != 1)) {
        return NULL;
    }
    return xstrdup(full);
}

/* ---------- sorted directory listing ---------- */

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static char **list_dir(const char *path, size_t *n)
{
    *n = 0;
    DIR *d = opendir(path);
    if (!d) {
        return NULL;
    }
    size_t cap = 16;
    char **v = malloc(cap * sizeof(*v));
    struct dirent *e;
    while (v && (e = readdir(d))) {
        if (e->d_name[0] == '.') {
            continue;
        }
        if (*n == cap) {
            cap *= 2;
            v = realloc(v, cap * sizeof(*v));
            if (!v) {
                break;
            }
        }
        v[(*n)++] = xstrdup(e->d_name);
    }
    closedir(d);
    if (v) {
        qsort(v, *n, sizeof(*v), cmp_str);
    }
    return v;
}

/* ---------- dynamic string ---------- */

typedef struct {
    char  *p;
    size_t n, cap;
} sb_t;

static void sb_add(sb_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_add(sb_t *s, const char *fmt, ...)
{
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int w = vsnprintf(s->p ? s->p + s->n : NULL, s->cap - s->n, fmt, ap);
        va_end(ap);
        if (w < 0) {
            die("format error");
        }
        if (s->p && (size_t)w < s->cap - s->n) {
            s->n += (size_t)w;
            return;
        }
        s->cap = s->cap ? s->cap * 2 + (size_t)w : 1024 + (size_t)w;
        s->p = realloc(s->p, s->cap);
        if (!s->p) {
            die("out of memory");
        }
    }
}

/* ---------- agent_manifest ---------- */

#define GEN_BEGIN "<!-- botter:generated:begin (managed by the agent_manifest tool; edit outside these markers) -->"
#define GEN_END   "<!-- botter:generated:end -->"

static void one_line(char *s, size_t max)
{
    for (char *p = s; *p; p++) {
        if (*p == '\n' || *p == '\r' || *p == '\t') {
            *p = ' ';
        }
    }
    if (strlen(s) > max) {
        s[max] = '\0';
        if (max > 3) {
            memcpy(s + max - 3, "...", 3);
        }
    }
}

static char *trim_in_place(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        s[n - 1] = '\0';
        s++;
    }
    return s;
}

/* SKILL.md summary: frontmatter "description:", else first prose/heading line. */
static char *skill_summary(const char *path)
{
    size_t n = 0;
    int    saved = errors;
    char  *txt = slurp(path, &n);
    errors = saved;
    if (!txt) {
        return xstrdup("(unreadable)");
    }
    char *res = NULL;
    char *save = NULL;
    char *copy = xstrdup(txt);
    int   in_fm = 0, first = 1;
    for (char *line = strtok_r(copy, "\n", &save); line && !res; line = strtok_r(NULL, "\n", &save)) {
        if (first && strncmp(line, "---", 3) == 0) {
            in_fm = 1;
            first = 0;
            continue;
        }
        first = 0;
        if (in_fm) {
            if (strncmp(line, "---", 3) == 0) {
                in_fm = 0;
            } else if (strncmp(line, "description:", 12) == 0) {
                res = xstrdup(trim_in_place(line + 12));
            }
            continue;
        }
    }
    if (!res) { /* no frontmatter description: first non-empty line, '#' stripped */
        strcpy(copy, txt);
        save = NULL;
        for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char *t = line;
            while (*t == '#' || *t == ' ') {
                t++;
            }
            if (*t && strncmp(line, "---", 3) != 0) {
                res = xstrdup(trim_in_place(t));
                break;
            }
        }
    }
    free(copy);
    free(txt);
    if (!res) {
        res = xstrdup("(empty)");
    }
    one_line(res, 240);
    return res;
}

static int file_exists(const char *dir, const char *rel)
{
    char *f = join(dir, rel);
    struct stat st;
    int ok = stat(f, &st) == 0 && S_ISREG(st.st_mode);
    free(f);
    return ok;
}

static int cmd_manifest(const cJSON *args)
{
    const char *dirarg = arg_str(args, "dir", ".");
    char       *dir = resolve_inside(dirarg);
    struct stat st;
    if (!dir || stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "error: '%s' is not a directory inside the working directory\n", dirarg);
        return 1;
    }
    char *base = xstrdup(dir);
    const char *name = basename(base);
    if (!*name || strcmp(name, "/") == 0) {
        name = "agent";
    }

    sb_t g = {0};
    sb_add(&g, "%s\n", GEN_BEGIN);

    /* files */
    sb_add(&g, "## Files\n");
    sb_add(&g, "- agent.md: persona and behaviour (system prompt)%s\n", file_exists(dir, "agent.md") ? "" : "  [MISSING]");

    /* skills */
    sb_add(&g, "\n## Skills (read with vfs_read before the task they cover)\n");
    char *sd = join(dir, "skills");
    size_t ns = 0, shown = 0;
    char **sv = list_dir(sd, &ns);
    for (size_t i = 0; sv && i < ns; i++) {
        char rel[PATH_MAX];
        snprintf(rel, sizeof(rel), "skills/%s/SKILL.md", sv[i]);
        if (!file_exists(dir, rel)) {
            continue;
        }
        char *f = join(dir, rel), *sum = skill_summary(f);
        sb_add(&g, "- %s: %s\n", rel, sum);
        free(f);
        free(sum);
        shown++;
    }
    if (!shown) {
        sb_add(&g, "- (none)\n");
    }

    /* tools */
    sb_add(&g, "\n## Tools (callable; arguments are described in the tool schema)\n");
    char *td = join(dir, "tools/doc"), *tb = join(dir, "tools/bin");
    size_t nb = 0, nd = 0, ntools = 0;
    char **bv = list_dir(tb, &nb);
    char **dv = list_dir(td, &nd);
    for (size_t i = 0; bv && i < nb; i++) {
        char dp[PATH_MAX], mp[PATH_MAX];
        snprintf(dp, sizeof(dp), "tools/doc/%s.json", bv[i]);
        snprintf(mp, sizeof(mp), "tools/doc/%s.md", bv[i]);
        char *bf = join(tb, bv[i]);
        const char *kind = "?";
        FILE *f = fopen(bf, "rb");
        if (f) {
            unsigned char m[4] = {0};
            size_t        r = fread(m, 1, 4, f);
            kind = (r >= 4 && memcmp(m, "\x7f" "ELF", 4) == 0) ? "native" : (r >= 2 && m[0] == '#' && m[1] == '!') ? "script" : "INVALID";
            fclose(f);
        }
        free(bf);
        char *desc = NULL;
        int   net = 0;
        if (file_exists(dir, dp)) {
            char *jf = join(dir, dp);
            int   saved = errors;
            size_t jn = 0;
            char *jt = slurp(jf, &jn);
            errors = saved;
            cJSON *j = jt ? cJSON_Parse(jt) : NULL;
            const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "description");
            desc = xstrdup(cJSON_IsString(d) ? d->valuestring : "(descriptor has no description)");
            net = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "network"));
            cJSON_Delete(j);
            free(jt);
            free(jf);
        } else {
            desc = xstrdup("(no descriptor: this tool will NOT load)");
        }
        one_line(desc, 240);
        sb_add(&g, "- %s [%s%s]: %s", bv[i], kind, net ? ", internet" : "", desc);
        if (file_exists(dir, mp)) {
            sb_add(&g, "  (docs: %s)", mp);
        }
        sb_add(&g, "\n");
        free(desc);
        ntools++;
    }
    for (size_t i = 0; dv && i < nd; i++) {
        size_t l = strlen(dv[i]);
        if (l > 5 && strcmp(dv[i] + l - 5, ".json") == 0) {
            char bp[PATH_MAX];
            snprintf(bp, sizeof(bp), "tools/bin/%.*s", (int)(l - 5), dv[i]);
            if (!file_exists(dir, bp)) {
                sb_add(&g, "- %.*s [NO EXECUTABLE]: descriptor exists but tools/bin/%.*s does not\n", (int)(l - 5), dv[i],
                       (int)(l - 5), dv[i]);
            }
        }
    }
    if (!ntools) {
        sb_add(&g, "- (none beyond the built-in file, shell and scheduling tools)\n");
    }
    sb_add(&g, "%s\n", GEN_END);

    /* merge with existing manifest.md */
    char *mf = join(dir, "manifest.md");
    size_t on = 0;
    int    saved = errors;
    char  *old = file_exists(dir, "manifest.md") ? slurp(mf, &on) : NULL;
    errors = saved;
    sb_t out = {0};
    if (old) {
        char *b = strstr(old, GEN_BEGIN), *e = b ? strstr(b, GEN_END) : NULL;
        if (b && e) {
            e += strlen(GEN_END);
            if (*e == '\n') {
                e++;
            }
            sb_add(&out, "%.*s%s%s", (int)(b - old), old, g.p, e);
        } else {
            sb_add(&out, "%s%s\n%s", old, on && old[on - 1] != '\n' ? "\n" : "", g.p);
        }
    } else {
        sb_add(&out, "# %s\n\nMap of this agent for the LLM: what it is, and where its skills and tools are. Text "
                     "outside the generated block below is hand-editable and is kept.\n\n%s\n## Notes\n"
                     "(Relations between skills and tools, conventions, anything the agent should know.)\n",
               name, g.p);
    }

    char tmp[PATH_MAX + 32];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", mf, (int)getpid());
    FILE *o = fopen(tmp, "wb");
    int   ok = o && fwrite(out.p, 1, out.n, o) == out.n;
    if (o) {
        ok = (fclose(o) == 0) && ok;
    }
    if (!ok || chmod(tmp, 0644) != 0 || rename(tmp, mf) != 0) {
        remove(tmp);
        fprintf(stderr, "error: cannot write %s: %s\n", mf, strerror(errno));
        return 1;
    }
    printf("manifest.md %s: %zu skill(s), %zu tool(s)\n", old ? "updated" : "created", shown, ntools);
    return 0;
}

/* ---------- agent_build ---------- */

static int cmd_build_tool(const cJSON *args)
{
    const char *dirarg = arg_str(args, "dir", ".");
    const cJSON *dry = cJSON_GetObjectItemCaseSensitive(args, "dry_run");
    int         dry_run = cJSON_IsTrue(dry);
    char       *dir = resolve_inside(dirarg);
    struct stat st;
    if (!dir || stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "error: '%s' is not a directory inside the working directory\n", dirarg);
        return 1;
    }

    /* botcore passes a read-only handle to its own executable as BOTCORE_RUNTIME_FD */
    const char *rfd = getenv("BOTCORE_RUNTIME_FD");
    char        runtime[64];
    if (!rfd || atoi(rfd) < 3) {
        fprintf(stderr, "error: cannot find the botcore runtime: agent_build must be run by a botcore agent binary\n");
        return 1;
    }
    snprintf(runtime, sizeof(runtime), "/proc/self/fd/%d", atoi(rfd));

    char *out = NULL;
    if (!dry_run) {
        const char *oarg = arg_str(args, "output", NULL);
        char        def[PATH_MAX];
        if (!oarg) {
            char *b = xstrdup(dir);
            const char *n = basename(b);
            snprintf(def, sizeof(def), "%s.bot", (*n && strcmp(n, "/") != 0) ? n : "agent");
            free(b);
            oarg = def;
        }
        out = resolve_inside(oarg);
        if (!out) {
            fprintf(stderr, "error: output '%s' is outside the working directory\n", oarg);
            return 1;
        }
        struct stat os;
        if (stat(out, &os) == 0) {
            if (!S_ISREG(os.st_mode)) {
                fprintf(stderr, "error: output '%s' exists and is not a regular file\n", oarg);
                return 1;
            }
            unsigned char m[4] = {0};
            FILE *f = fopen(out, "rb");
            size_t r = f ? fread(m, 1, 4, f) : 0;
            if (f) {
                fclose(f);
            }
            if (r < 4 || memcmp(m, "\x7f" "ELF", 4) != 0) {
                fprintf(stderr, "error: refusing to overwrite '%s': it exists and is not an executable (.bot) file\n", oarg);
                return 1;
            }
        }
        char *oc = xstrdup(out);
        char *od = dirname(oc);
        if (stat(od, &os) != 0 || !S_ISDIR(os.st_mode)) {
            fprintf(stderr, "error: directory of output '%s' does not exist\n", oarg);
            return 1;
        }
        free(oc);
    }

    /* the runtime must be a real agent binary (botcore + pack) */
    size_t         rn = 0;
    int            saved = errors;
    unsigned char *rt = (unsigned char *)slurp(runtime, &rn);
    errors = saved;
    if (!rt || rn < FOOTER_SIZE || memcmp(rt + rn - 8, FOOTER_MAGIC, 8) != 0) {
        fprintf(stderr, "error: cannot find the botcore runtime: agent_build must be run by a botcore agent binary\n");
        return 1;
    }
    free(rt);

    return cmd_build(dir, runtime, out);
}

/* ---------- agent_inspect ---------- */

static int cmd_inspect(const cJSON *args)
{
    const char *f = arg_str(args, "file", NULL);
    if (!f) {
        fprintf(stderr, "error: missing required argument 'file'\n");
        return 1;
    }
    char *p = resolve_inside(f);
    if (!p) {
        fprintf(stderr, "error: '%s' is outside the working directory\n", f);
        return 1;
    }
    return cmd_list(p);
}

/* ---------- dispatch ---------- */

int main(int argc, char **argv)
{
    (void)argc;
    /* packer warnings/errors are part of the result */
    dup2(1, 2);
    setvbuf(stdout, NULL, _IOLBF, 0);

    char *b = xstrdup(argv[0] ? argv[0] : "");
    const char *name = basename(b);

    if (!getcwd(g_cwd, sizeof(g_cwd))) {
        fprintf(stderr, "error: cannot determine working directory\n");
        return 1;
    }
    char *in = read_stdin();
    cJSON *args = cJSON_Parse(in[0] ? in : "{}");
    if (!cJSON_IsObject(args)) {
        fprintf(stderr, "error: arguments must be a JSON object\n");
        return 1;
    }
    if (strcmp(name, "agent_manifest") == 0) {
        return cmd_manifest(args);
    }
    if (strcmp(name, "agent_build") == 0) {
        return cmd_build_tool(args);
    }
    if (strcmp(name, "agent_inspect") == 0) {
        return cmd_inspect(args);
    }
    fprintf(stderr, "error: unknown tool name '%s' (expected agent_manifest, agent_build or agent_inspect)\n", name);
    return 2;
}
