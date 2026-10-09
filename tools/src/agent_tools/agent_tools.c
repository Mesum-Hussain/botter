/*
 * agent_tools: the botter agent's own tools, one source, one executable that
 * behaves according to the name it is started under (the botcore tool runner
 * passes the tool name as argv[0]):
 *
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
    if (strcmp(name, "agent_build") == 0) {
        return cmd_build_tool(args);
    }
    if (strcmp(name, "agent_inspect") == 0) {
        return cmd_inspect(args);
    }
    fprintf(stderr, "error: unknown tool name '%s' (expected agent_build or agent_inspect)\n", name);
    return 2;
}
