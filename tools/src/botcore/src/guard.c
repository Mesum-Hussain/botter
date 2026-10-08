#define _GNU_SOURCE
#include "guard.h"
#include "term.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_ctx[PATH_MAX];
static int  g_always[GUARD_NCAT];

void guard_init(void)
{
    if (!getcwd(g_ctx, sizeof(g_ctx)) || !realpath(".", g_ctx)) {
        strcpy(g_ctx, "/");
    }
}

const char *guard_ctx(void) { return g_ctx; }

static int is_inside(const char *p)
{
    size_t n = strlen(g_ctx);
    if (n == 1) { /* ctx is "/" */
        return 1;
    }
    return strncmp(p, g_ctx, n) == 0 && (p[n] == '\0' || p[n] == '/');
}

int guard_resolve(const char *path, char out[PATH_MAX], int *inside)
{
    char abs[PATH_MAX], tail[PATH_MAX] = "", real[PATH_MAX];

    if (!path || !*path || strlen(path) >= PATH_MAX - 2 || strlen(g_ctx) + strlen(path) + 2 >= PATH_MAX) {
        return -1;
    }
    if ((path[0] == '/' ? snprintf(abs, sizeof(abs), "%s", path)
                        : snprintf(abs, sizeof(abs), "%s/%s", g_ctx, path)) >= (int)sizeof(abs)) {
        return -1;
    }
    /* drop trailing slashes (keep a lone "/") */
    size_t n = strlen(abs);
    while (n > 1 && abs[n - 1] == '/') {
        abs[--n] = '\0';
    }

    /* Peel components off the end until a prefix exists, then re-append them. */
    for (;;) {
        if (realpath(abs, real)) {
            break;
        }
        if (errno != ENOENT && errno != ENOTDIR) {
            return -1;
        }
        char *slash = strrchr(abs, '/');
        if (!slash || slash == abs) {
            return -1; /* nothing resolvable (cannot happen: "/" always exists) */
        }
        const char *comp = slash + 1;
        if (strcmp(comp, "..") == 0 || strcmp(comp, ".") == 0 || !*comp) {
            return -1; /* ".." under a non-existent prefix cannot be verified */
        }
        char t2[PATH_MAX];
        if (snprintf(t2, sizeof(t2), "/%s%s", comp, tail) >= (int)sizeof(t2)) {
            return -1;
        }
        strcpy(tail, t2);
        *slash = '\0';
    }
    if (strlen(real) + strlen(tail) >= PATH_MAX) {
        return -1;
    }
    if ((strcmp(real, "/") == 0 && tail[0] ? snprintf(out, PATH_MAX, "%s", tail)
                                           : snprintf(out, PATH_MAX, "%s%s", real, tail)) >= PATH_MAX) {
        return -1;
    }
    *inside = is_inside(out);
    return 0;
}

/* ---------------- confirmation ---------------- */

static void put_clean(const char *s, size_t max)
{
    size_t n = 0;
    for (; *s && n < max; s++, n++) {
        unsigned char c = (unsigned char)*s;
        putchar(c == '\n' || (c >= 0x20 && c != 0x7f) || c >= 0x80 ? c : '?');
    }
    if (*s) {
        fputs("...", stdout);
    }
}

static int ask(int cat, const char *msg)
{
    char *in = NULL;
    int yes = 0;

    if (cat > 0 && g_always[cat]) {
        return 1;
    }
    printf(ANSI_BOLD_BLUE "Permission needed:" ANSI_RESET " ");
    put_clean(msg, 600);
    putchar('\n');
    fflush(stdout);
    int rc = term_readline(cat > 0 ? ANSI_BLUE "Allow? [y/N/a=always]: " ANSI_RESET
                                   : ANSI_BLUE "Allow? [y/N]: " ANSI_RESET,
                           0, &in);
    if (rc == TERM_LINE && in) {
        char *s = in;
        while (isspace((unsigned char)*s)) {
            s++;
        }
        if (*s == 'y' || *s == 'Y') {
            yes = 1;
        } else if ((*s == 'a' || *s == 'A') && cat > 0) {
            g_always[cat] = 1;
            yes = 1;
        }
    }
    free(in);
    return yes;
}

int guard_confirm(const char *fmt, ...)
{
    char msg[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    return ask(0, msg);
}

int guard_confirm_cat(int cat, const char *fmt, ...)
{
    char msg[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    return ask(cat, msg);
}

/* ---------------- shell classification ---------------- */

#define MAXTOK 512

typedef struct {
    char *s;
    int   op;     /* operator token (; | & && || ( ) newline < > >> >& ...) */
    int   quoted; /* contained quote characters */
} tok_t;

typedef struct {
    tok_t t[MAXTOK];
    int   n;
    int   subst;   /* backtick or $( seen outside single quotes */
    int   heredoc; /* << seen */
    int   bad;     /* unbalanced quotes / too many tokens */
} toks_t;

static void tk_free(toks_t *k)
{
    for (int i = 0; i < k->n; i++) {
        free(k->t[i].s);
    }
}

static void tk_add(toks_t *k, const char *buf, size_t len, int op, int quoted)
{
    if (k->n >= MAXTOK) {
        k->bad = 1;
        return;
    }
    k->t[k->n].s = strndup(buf, len);
    k->t[k->n].op = op;
    k->t[k->n].quoted = quoted;
    if (k->t[k->n].s) {
        k->n++;
    }
}

static void tokenize(const char *c, toks_t *k)
{
    char *buf = malloc(strlen(c) + 2);
    size_t bl = 0;
    int inword = 0, quoted = 0;

    memset(k, 0, sizeof(*k));
    if (!buf) {
        k->bad = 1;
        return;
    }
#define FLUSH()                                   \
    do {                                          \
        if (inword) {                             \
            tk_add(k, buf, bl, 0, quoted);        \
        }                                         \
        bl = 0;                                   \
        inword = 0;                               \
        quoted = 0;                               \
    } while (0)

    for (const char *p = c; *p && !k->bad; p++) {
        if (*p == '\'') {
            const char *e = strchr(p + 1, '\'');
            if (!e) {
                k->bad = 1;
                break;
            }
            memcpy(buf + bl, p + 1, (size_t)(e - p - 1));
            bl += (size_t)(e - p - 1);
            inword = quoted = 1;
            p = e;
        } else if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) {
                    p++;
                } else if (*p == '`' || (*p == '$' && p[1] == '(')) {
                    k->subst = 1;
                }
                buf[bl++] = *p++;
            }
            if (!*p) {
                k->bad = 1;
                break;
            }
            inword = quoted = 1;
        } else if (*p == '\\' && p[1]) {
            if (p[1] == '\n') { /* line continuation */
                p++;
                continue;
            }
            buf[bl++] = *(++p);
            inword = 1;
        } else if (*p == '`' || (*p == '$' && p[1] == '(')) {
            k->subst = 1;
            buf[bl++] = *p;
            inword = 1;
        } else if (*p == ' ' || *p == '\t' || *p == '\r') {
            FLUSH();
        } else if (strchr(";|&()<>\n", *p)) {
            /* a bare number before > or < is an fd, not a word: keep it as a word, harmless */
            FLUSH();
            char op[4] = {*p, 0, 0, 0};
            if ((*p == '&' || *p == '|' || *p == '>' || *p == '<') && p[1] == *p) {
                op[1] = *p++;
            } else if (*p == '>' && (p[1] == '&' || p[1] == '|')) {
                op[1] = *++p;
            }
            if (strcmp(op, "<<") == 0) {
                k->heredoc = 1;
            }
            tk_add(k, op, strlen(op), 1, 0);
        } else {
            buf[bl++] = *p;
            inword = 1;
        }
    }
    FLUSH();
#undef FLUSH
    free(buf);
}

static int in_list(const char *w, const char *const *list)
{
    for (; *list; list++) {
        if (strcmp(w, *list) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char *const NET_CMDS[] = {
    "curl", "wget", "ssh", "scp", "sftp", "rsync", "nc", "ncat", "netcat", "socat", "telnet", "ftp", "ping",
    "ping6", "traceroute", "tracepath", "dig", "nslookup", "host", "pip", "pip3", "npm", "npx", "yarn", "pnpm",
    "cargo", "dnf", "yum", "apt", "apt-get", "zypper", "brew", "gem", "docker", "podman", NULL};
static const char *const WRAPPERS[] = {"sudo", "doas", "env", "nohup", "time", "nice", "ionice", "timeout",
                                       "exec", "command", "xargs", "stdbuf", "setsid", "builtin", NULL};
static const char *const PRIV[] = {"sudo", "doas", "su", "pkexec", NULL};
static const char *const DESTRUCT[] = {"rm", "rmdir", "shred", "truncate", "dd", "fdisk", "parted", "wipefs",
                                       "kill", "killall", "pkill", "shutdown", "reboot", "poweroff", "halt",
                                       "systemctl", "mount", "umount", "eval", "source", ".", "unlink", NULL};
static const char *const SHELLS[] = {"sh", "bash", "zsh", "dash", "ksh", "fish", NULL};
static const char *const GIT_NET[] = {"clone", "fetch", "pull", "push", "remote", "submodule", "ls-remote", NULL};
static const char *const GIT_DESTRUCT[] = {"clean", "reset", "restore", "rebase", "filter-branch", "update-ref", NULL};

#define MAXWHY 3
typedef struct {
    char why[MAXWHY][200];
    int  n;
} reasons_t;

static void add_why(reasons_t *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add_why(reasons_t *r, const char *fmt, ...)
{
    if (r->n >= MAXWHY) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->why[r->n], sizeof(r->why[0]), fmt, ap);
    va_end(ap);
    r->n++;
}

static int is_regular_file(const char *path)
{
    char res[PATH_MAX];
    int inside;
    struct stat st;
    return guard_resolve(path, res, &inside) == 0 && stat(res, &st) == 0 && S_ISREG(st.st_mode);
}

static int safe_special(const char *p)
{
    static const char *const ok[] = {"/dev/null", "/dev/zero", "/dev/urandom", "/dev/random", "/dev/stdin",
                                     "/dev/stdout", "/dev/stderr", NULL};
    return in_list(p, ok);
}

/* Flag a word that names a path outside the context dir. */
static void check_path_word(const char *w, reasons_t *r)
{
    char res[PATH_MAX];
    int inside = 1;

    if (w[0] == '~') {
        add_why(r, "touches home/other path: %.80s", w);
        return;
    }
    int dotdot = strcmp(w, "..") == 0 || strstr(w, "../") != NULL ||
                 (strlen(w) >= 3 && strcmp(w + strlen(w) - 3, "/..") == 0);
    if (w[0] != '/' && !dotdot) {
        return;
    }
    if (safe_special(w)) {
        return;
    }
    if (guard_resolve(w, res, &inside) != 0) {
        add_why(r, "cannot verify path: %.80s", w);
    } else if (!inside) {
        add_why(r, "path outside working dir: %.80s", res);
    }
}

int guard_shell_classify(const char *cmd, char *why, size_t why_len)
{
    toks_t k;
    reasons_t r = {.n = 0};
    int deny = 0;

    why[0] = '\0';
    tokenize(cmd, &k);
    if (k.bad) {
        snprintf(why, why_len, "command could not be parsed safely (quotes/size)");
        tk_free(&k);
        return SH_CONFIRM;
    }
    if (k.subst) {
        add_why(&r, "uses command substitution (cannot verify what runs)");
    }
    if (k.heredoc) {
        add_why(&r, "uses a here-document (cannot verify its contents)");
    }

    int cmdpos = 1;
    for (int i = 0; i < k.n && !deny; i++) {
        tok_t *t = &k.t[i];

        if (t->op) {
            if (strcmp(t->s, ">") == 0 || strcmp(t->s, ">|") == 0 || strcmp(t->s, ">>") == 0) {
                if (i + 1 < k.n && !k.t[i + 1].op) {
                    const char *tgt = k.t[i + 1].s;
                    if (!safe_special(tgt)) {
                        if (strcmp(t->s, ">>") != 0 && is_regular_file(tgt)) {
                            add_why(&r, "'>' overwrites existing file: %.80s", tgt);
                        }
                        check_path_word(tgt, &r);
                        if (tgt[0] == '$') {
                            add_why(&r, "redirect target from variable");
                        }
                    }
                    i++; /* target consumed */
                }
            } else if (strcmp(t->s, ">&") == 0 || strcmp(t->s, "<") == 0 || strcmp(t->s, "<<") == 0 ||
                       strcmp(t->s, "<&") == 0) {
                if (i + 1 < k.n && !k.t[i + 1].op) {
                    if (strcmp(t->s, "<") == 0) {
                        check_path_word(k.t[i + 1].s, &r);
                    }
                    i++;
                }
            } else {
                cmdpos = 1; /* ; | & && || ( ) newline */
            }
            continue;
        }

        if (!cmdpos) {
            check_path_word(t->s, &r);
            continue;
        }

        /* command position: skip env assignments */
        if (!t->quoted && (isalpha((unsigned char)t->s[0]) || t->s[0] == '_')) {
            const char *eq = strchr(t->s, '=');
            if (eq) {
                int ok = 1;
                for (const char *q = t->s; q < eq; q++) {
                    ok &= isalnum((unsigned char)*q) || *q == '_';
                }
                if (ok) {
                    continue;
                }
            }
        }
        if (t->s[0] == '$') {
            add_why(&r, "command name comes from a variable");
        }
        const char *base = strrchr(t->s, '/');
        base = base ? base + 1 : t->s;
        check_path_word(t->s, &r);

        /* collect this command's args */
        int a0 = i + 1, a1 = a0;
        while (a1 < k.n && !k.t[a1].op) {
            a1++;
        }
#define ARG(j) (k.t[j].s)

        if (in_list(base, WRAPPERS) || in_list(base, PRIV)) {
            if (in_list(base, PRIV)) {
                add_why(&r, "privilege escalation (%s)", base);
            }
            /* the wrapped command is the next non-option, non-numeric, non-assignment word */
            int j = a0;
            while (j < a1 && (ARG(j)[0] == '-' || isdigit((unsigned char)ARG(j)[0]) || strchr(ARG(j), '=') != NULL)) {
                j++;
            }
            if (j < a1) {
                /* re-process from the wrapped command by pretending it is at command position */
                i = j - 1;
                continue; /* cmdpos stays 1 */
            }
            cmdpos = 0;
            continue;
        }
        cmdpos = 0;

        if (in_list(base, NET_CMDS)) {
            snprintf(why, why_len, "'%s' is blocked: the agent is offline-only (no network tools)", base);
            deny = 1;
            break;
        }
        if (strcmp(base, "git") == 0) {
            int j = a0;
            while (j < a1 && ARG(j)[0] == '-') {
                j++;
            }
            if (j < a1) {
                const char *sub = ARG(j);
                if (in_list(sub, GIT_NET)) {
                    snprintf(why, why_len, "'git %s' is blocked: the agent is offline-only", sub);
                    deny = 1;
                    break;
                }
                int bad = in_list(sub, GIT_DESTRUCT);
                for (int q = j + 1; q < a1; q++) {
                    if (strcmp(sub, "checkout") == 0 && (strcmp(ARG(q), "--") == 0 || strcmp(ARG(q), "-f") == 0)) {
                        bad = 1;
                    }
                    if (strcmp(sub, "branch") == 0 && strcmp(ARG(q), "-D") == 0) {
                        bad = 1;
                    }
                    if (strcmp(sub, "stash") == 0 && (strcmp(ARG(q), "drop") == 0 || strcmp(ARG(q), "clear") == 0)) {
                        bad = 1;
                    }
                }
                if (bad) {
                    add_why(&r, "destructive git operation: git %s", sub);
                }
            }
        } else if (in_list(base, DESTRUCT) || strncmp(base, "mkfs", 4) == 0) {
            add_why(&r, "destructive/system command: %s", base);
        } else if (in_list(base, SHELLS)) {
            for (int j = a0; j < a1; j++) {
                if (strcmp(ARG(j), "-c") == 0 || (ARG(j)[0] == '-' && ARG(j)[1] != '-' && strchr(ARG(j), 'c'))) {
                    add_why(&r, "runs a nested shell (%s -c)", base);
                    break;
                }
            }
        } else if (strcmp(base, "chmod") == 0 || strcmp(base, "chown") == 0 || strcmp(base, "chgrp") == 0) {
            for (int j = a0; j < a1; j++) {
                if (strcmp(ARG(j), "-R") == 0 || strcmp(ARG(j), "-r") == 0 || strcmp(ARG(j), "--recursive") == 0) {
                    add_why(&r, "recursive %s", base);
                }
            }
        } else if (strcmp(base, "find") == 0) {
            for (int j = a0; j < a1; j++) {
                if (strcmp(ARG(j), "-delete") == 0 || strcmp(ARG(j), "-exec") == 0 ||
                    strcmp(ARG(j), "-execdir") == 0 || strcmp(ARG(j), "-ok") == 0) {
                    add_why(&r, "find %s", ARG(j));
                    break;
                }
            }
        } else if (strcmp(base, "mv") == 0 || strcmp(base, "cp") == 0 || strcmp(base, "install") == 0 ||
                   strcmp(base, "ln") == 0) {
            int last = -1;
            for (int j = a0; j < a1; j++) {
                if (ARG(j)[0] != '-') {
                    last = j;
                }
            }
            if (last >= 0 && is_regular_file(ARG(last))) {
                add_why(&r, "%s would overwrite existing file: %.80s", base, ARG(last));
            }
        } else if (strcmp(base, "tee") == 0) {
            int append = 0;
            for (int j = a0; j < a1; j++) {
                if (strcmp(ARG(j), "-a") == 0 || strcmp(ARG(j), "--append") == 0) {
                    append = 1;
                }
            }
            for (int j = a0; j < a1 && !append; j++) {
                if (ARG(j)[0] != '-' && is_regular_file(ARG(j))) {
                    add_why(&r, "tee would overwrite existing file: %.80s", ARG(j));
                }
            }
        }
        /* remaining args get the path-outside check on later iterations (cmdpos == 0) */
#undef ARG
    }

    tk_free(&k);
    if (deny) {
        return SH_DENY;
    }
    if (r.n == 0) {
        return SH_OK;
    }
    size_t off = 0;
    for (int i = 0; i < r.n && off < why_len; i++) {
        off += (size_t)snprintf(why + off, why_len - off, "%s%s", i ? "; " : "", r.why[i]);
    }
    return SH_CONFIRM;
}
