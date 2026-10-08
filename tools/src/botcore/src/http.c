#define _GNU_SOURCE
#include "http.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_OUT (32u * 1024 * 1024)

static volatile pid_t g_child = 0;
static volatile sig_atomic_t g_aborted = 0;

void http_abort(void)
{
    pid_t p = g_child;
    g_aborted = 1;
    if (p > 0) {
        kill(p, SIGTERM);
    }
}

/* ---- growable buffer -------------------------------------------------- */

typedef struct {
    char  *p;
    size_t len, cap;
} sb_t;

static int sb_add(sb_t *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) {
            cap *= 2;
        }
        char *np = realloc(b->p, cap);
        if (!np) {
            return -1;
        }
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 0;
}

static int sb_str(sb_t *b, const char *s)
{
    return sb_add(b, s, strlen(s));
}

/* Append s as a curl-config quoted string: "..." with \\ \" \n \r \t \v. */
static int sb_quoted(sb_t *b, const char *s)
{
    if (sb_add(b, "\"", 1)) {
        return -1;
    }
    for (; *s; s++) {
        const char *esc = NULL;
        switch (*s) {
        case '\\': esc = "\\\\"; break;
        case '"':  esc = "\\\""; break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        case '\v': esc = "\\v";  break;
        default: break;
        }
        if (esc ? sb_str(b, esc) : sb_add(b, s, 1)) {
            return -1;
        }
    }
    return sb_add(b, "\"", 1);
}

static int has_ctl(const char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20 || *s == 0x7f) {
            return 1;
        }
    }
    return 0;
}

/* ---- request ---------------------------------------------------------- */

void http_resp_free(http_resp_t *r)
{
    if (!r) {
        return;
    }
    free(r->body);
    free(r->err);
    r->body = r->err = NULL;
    r->status = 0;
}

static char *trim_dup(const char *s, size_t n)
{
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) {
        n--;
    }
    char *d = malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

int http_request(const char *url, const char *bearer, const char *body,
                 int timeout_s, http_resp_t *out)
{
    sb_t cfg = {0}, so = {0}, se = {0};
    int in_p[2] = {-1, -1}, out_p[2] = {-1, -1}, err_p[2] = {-1, -1};
    int rc = HTTP_ERR_XPORT;
    char tmo[16];

    memset(out, 0, sizeof(*out));
    out->body = strdup("");

    /* Anything with control chars could inject extra curl-config lines. */
    if (!url || has_ctl(url) || (bearer && has_ctl(bearer))) {
        out->err = strdup("invalid characters in URL or credentials");
        return HTTP_ERR_XPORT;
    }

    int ok = 0;
    ok |= sb_str(&cfg, "url = ");
    ok |= sb_quoted(&cfg, url);
    ok |= sb_str(&cfg, "\n");
    if (bearer && *bearer) {
        sb_t h = {0};
        ok |= sb_str(&h, "Authorization: Bearer ");
        ok |= sb_str(&h, bearer);
        ok |= sb_str(&cfg, "header = ");
        ok |= h.p ? sb_quoted(&cfg, h.p) : -1;
        ok |= sb_str(&cfg, "\n");
        if (h.p) {
            explicit_bzero(h.p, h.len);
            free(h.p);
        }
    }
    if (body) {
        ok |= sb_str(&cfg, "header = \"Content-Type: application/json\"\n");
        ok |= sb_str(&cfg, "data-raw = ");
        ok |= sb_quoted(&cfg, body);
        ok |= sb_str(&cfg, "\n");
    }
    if (ok) {
        out->err = strdup("out of memory");
        goto done;
    }

    if (pipe(in_p) || pipe(out_p) || pipe(err_p)) {
        out->err = strdup("pipe() failed");
        goto done;
    }

    snprintf(tmo, sizeof(tmo), "%d", timeout_s > 0 ? timeout_s : 60);
    g_aborted = 0;

    pid_t pid = fork();
    if (pid < 0) {
        out->err = strdup("fork() failed");
        goto done;
    }
    if (pid == 0) {
        dup2(in_p[0], 0);
        dup2(out_p[1], 1);
        dup2(err_p[1], 2);
        for (int fd = 3; fd < 64; fd++) {
            close(fd);
        }
        /* -q: ignore ~/.curlrc. Config (with secrets) arrives on stdin. */
        execlp("curl", "curl", "-q", "-sS", "-K", "-",
               "--proto", "=http,https",
               "--connect-timeout", "15", "--max-time", tmo,
               "-w", "\n%{http_code}", (char *)NULL);
        static const char m[] = "curl not found (required for now)";
        (void)!write(2, m, sizeof(m) - 1);
        _exit(127);
    }
    g_child = pid;
    close_fd(&in_p[0]);
    close_fd(&out_p[1]);
    close_fd(&err_p[1]);

    /* curl consumes the whole config before producing output: no deadlock. */
    for (size_t off = 0; off < cfg.len;) {
        ssize_t n = write(in_p[1], cfg.p + off, cfg.len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break; /* EPIPE: curl died early; stderr will say why */
        }
        off += (size_t)n;
    }
    close_fd(&in_p[1]);

    struct pollfd fds[2] = {{out_p[0], POLLIN, 0}, {err_p[0], POLLIN, 0}};
    sb_t *dst[2] = {&so, &se};
    char buf[8192];
    while (fds[0].fd >= 0 || fds[1].fd >= 0) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
                continue;
            }
            ssize_t n = read(fds[i].fd, buf, sizeof(buf));
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0 || dst[i]->len + (size_t)n > MAX_OUT) {
                close(fds[i].fd);
                fds[i].fd = -1;
                if (n > 0) {
                    kill(pid, SIGTERM);
                }
            } else if (sb_add(dst[i], buf, (size_t)n)) {
                kill(pid, SIGTERM);
                close(fds[i].fd);
                fds[i].fd = -1;
            }
        }
    }
    close_fd(&fds[0].fd);
    close_fd(&fds[1].fd);
    out_p[0] = err_p[0] = -1; /* owned by fds[] above */

    int wst = 0;
    while (waitpid(pid, &wst, 0) < 0 && errno == EINTR) {
    }
    g_child = 0;

    if (g_aborted) {
        rc = HTTP_ERR_ABORT;
        goto done;
    }

    /* stdout = body "\n" http_code */
    int status = 0;
    if (so.p) {
        char *nl = memrchr(so.p, '\n', so.len);
        if (nl) {
            status = atoi(nl + 1);
            *nl = '\0';
            so.len = (size_t)(nl - so.p);
        }
        free(out->body);
        out->body = so.p;
        so.p = NULL;
    }
    out->status = status;

    if (status == 0) {
        out->err = se.len ? trim_dup(se.p, se.len) : NULL;
        if (!out->err) {
            char m[48];
            snprintf(m, sizeof(m), "curl exited with status %d",
                     WIFEXITED(wst) ? WEXITSTATUS(wst) : -1);
            out->err = strdup(m);
        }
        rc = HTTP_ERR_XPORT;
    } else {
        rc = HTTP_OK;
    }

done:
    close_fd(&in_p[0]);
    close_fd(&in_p[1]);
    close_fd(&out_p[0]);
    close_fd(&out_p[1]);
    close_fd(&err_p[0]);
    close_fd(&err_p[1]);
    if (cfg.p) {
        explicit_bzero(cfg.p, cfg.len); /* contains the API key */
        free(cfg.p);
    }
    free(so.p);
    free(se.p);
    return rc;
}
