#define _GNU_SOURCE
#include "term.h"
#include "http.h"
#include "front.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static struct termios g_saved;
static volatile sig_atomic_t g_echo_off = 0;

static void restore_tty(void)
{
    if (g_echo_off) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
        g_echo_off = 0;
    }
}

static volatile sig_atomic_t g_intr = 0;
static int (*g_idle)(void);
static char *g_stash; /* text typed when an idle TICK interrupted the prompt */

int term_interrupted(void) { return g_intr; }
void term_clear_interrupt(void) { g_intr = 0; }
void term_set_idle(int (*cb)(void)) { g_idle = cb; }

static void on_int(int sig)
{
    (void)sig;
    g_intr = 1;
    http_abort(); /* no-op unless a request is in flight */
}

static void on_term(int sig)
{
    restore_tty();
    signal(sig, SIG_DFL);
    raise(sig);
}

void term_init(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_int; /* no SA_RESTART: interrupts blocking reads */
    sigaction(SIGINT, &sa, NULL);

    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    signal(SIGPIPE, SIG_IGN);
    atexit(restore_tty);
}

/* ---------------- line editor (tty, non-secret input) ---------------- */

#define HIST_MAX 200
static char *g_hist[HIST_MAX];
static int   g_nhist;

void term_history_add(const char *line)
{
    if (!*line || (g_nhist > 0 && strcmp(g_hist[g_nhist - 1], line) == 0)) {
        return;
    }
    if (g_nhist == HIST_MAX) {
        free(g_hist[0]);
        memmove(g_hist, g_hist + 1, sizeof(g_hist[0]) * (HIST_MAX - 1));
        g_nhist--;
    }
    char *d = strdup(line);
    if (d) {
        g_hist[g_nhist++] = d;
    }
}

typedef struct {
    char  *b;
    size_t len, cap, pos;
} ed_t;

static int is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

static size_t prev_ch(const ed_t *e, size_t i)
{
    while (i > 0 && is_cont((unsigned char)e->b[--i])) {
    }
    return i;
}

static size_t next_ch(const ed_t *e, size_t i)
{
    if (i < e->len) {
        i++;
    }
    while (i < e->len && is_cont((unsigned char)e->b[i])) {
        i++;
    }
    return i;
}

static size_t cols_in(const char *s, size_t n)
{
    size_t c = 0;
    for (size_t i = 0; i < n; i++) {
        if (!is_cont((unsigned char)s[i])) {
            c++;
        }
    }
    return c;
}

/* Visible width of a prompt (ANSI SGR sequences are zero-width). */
static size_t prompt_cols(const char *p)
{
    size_t c = 0;
    for (; *p; p++) {
        if (*p == '\033' && p[1] == '[') {
            p += 2;
            while (*p && !(*p >= '@' && *p <= '~')) {
                p++;
            }
            if (!*p) {
                break;
            }
        } else if (!is_cont((unsigned char)*p)) {
            c++;
        }
    }
    return c;
}

static int ed_reserve(ed_t *e, size_t extra)
{
    if (e->len + extra + 1 <= e->cap) {
        return 0;
    }
    size_t nc = (e->cap ? e->cap * 2 : 128) + extra;
    char *nb = realloc(e->b, nc);
    if (!nb) {
        return -1;
    }
    e->b = nb;
    e->cap = nc;
    return 0;
}

static void ed_insert(ed_t *e, const char *s, size_t n)
{
    if (ed_reserve(e, n) != 0) {
        return;
    }
    memmove(e->b + e->pos + n, e->b + e->pos, e->len - e->pos);
    memcpy(e->b + e->pos, s, n);
    e->pos += n;
    e->len += n;
    e->b[e->len] = '\0';
}

static void ed_delete(ed_t *e, size_t from, size_t to)
{
    memmove(e->b + from, e->b + to, e->len - to);
    e->len -= to - from;
    e->b[e->len] = '\0';
    e->pos = from;
}

static void ed_set(ed_t *e, const char *s)
{
    e->len = 0;
    e->pos = 0;
    e->b[0] = '\0';
    ed_insert(e, s, strlen(s));
}

/* Single-row redraw; scrolls horizontally when the line exceeds the terminal. */
static void ed_refresh(const ed_t *e, const char *prompt, size_t plen)
{
    struct winsize ws;
    size_t width = (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) ? ws.ws_col : 80;
    size_t avail = width > plen + 2 ? width - plen - 1 : 1;
    size_t ccol = cols_in(e->b, e->pos);
    size_t total = cols_in(e->b, e->len);
    size_t start = ccol >= avail ? ccol - avail + 1 : 0;
    size_t end = start + avail < total ? start + avail : total;

    /* char index -> byte offset */
    size_t bs = 0, be = 0, ci = 0;
    for (size_t i = 0; i <= e->len; i++) {
        if (i == e->len || !is_cont((unsigned char)e->b[i])) {
            if (ci == start) {
                bs = i;
            }
            if (ci == end) {
                be = i;
                break;
            }
            ci++;
        }
    }
    if (end >= total) {
        be = e->len;
    }

    char head[64];
    int hn = snprintf(head, sizeof(head), "\r");
    fflush(stdout);
    (void)!write(STDOUT_FILENO, head, (size_t)hn);
    (void)!write(STDOUT_FILENO, prompt, strlen(prompt));
    (void)!write(STDOUT_FILENO, ANSI_LGREEN, sizeof(ANSI_LGREEN) - 1);
    (void)!write(STDOUT_FILENO, e->b + bs, be - bs);
    (void)!write(STDOUT_FILENO, ANSI_RESET, sizeof(ANSI_RESET) - 1);
    char tail[64];
    size_t fwd = plen + ccol - start;
    int tn = fwd ? snprintf(tail, sizeof(tail), "\033[0K\r\033[%zuC", fwd)
                 : snprintf(tail, sizeof(tail), "\033[0K\r");
    (void)!write(STDOUT_FILENO, tail, (size_t)tn);
}

/* Read one byte, optionally with a short timeout (for lone ESC). 1 ok, 2 timeout, 0 EOF, -1 EINTR/error. */
static int rd_byte(unsigned char *c, int timeout_ms)
{
    if (timeout_ms >= 0) {
        struct pollfd pf = {STDIN_FILENO, POLLIN, 0};
        int p = poll(&pf, 1, timeout_ms);
        if (p < 0) {
            return -1;
        }
        if (p == 0) {
            return 2; /* timeout */
        }
    }
    ssize_t n = read(STDIN_FILENO, c, 1);
    if (n == 1) {
        return 1;
    }
    return (n < 0) ? -1 : 0;
}

static int edit_line(const char *prompt, char **out)
{
    struct termios t;
    ed_t e = {0};
    size_t plen = prompt_cols(prompt);
    int hidx = g_nhist; /* g_nhist == the live (draft) line */
    char *draft = NULL;
    int result = TERM_EOF;

    if (tcgetattr(STDIN_FILENO, &g_saved) != 0) {
        return -2; /* not usable; caller falls back to cooked */
    }
    t = g_saved;
    t.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN); /* ISIG stays: Ctrl-C still raises SIGINT */
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    g_echo_off = 1;

    if (ed_reserve(&e, 0) != 0) {
        goto done;
    }
    e.b[0] = '\0';
    if (g_stash) {
        ed_set(&e, g_stash);
        free(g_stash);
        g_stash = NULL;
    }
    ed_refresh(&e, prompt, plen);

    for (;;) {
        unsigned char c;
        int r;
        if (g_idle) {
            r = rd_byte(&c, 1000);
            if (r == 2) {
                if (g_idle()) { /* the caller has work to do now */
                    g_stash = strdup(e.b);
                    result = TERM_TICK;
                    break;
                }
                continue;
            }
        } else {
            r = rd_byte(&c, -1);
        }
        if (r < 0) {
            result = (errno == EINTR) ? TERM_INTR : TERM_EOF;
            break;
        }
        if (r == 0) {
            result = TERM_EOF;
            break;
        }

        if (c == '\r' || c == '\n') {
            result = TERM_LINE;
            break;
        }
        if (c == 4) { /* Ctrl-D */
            if (e.len == 0) {
                result = TERM_EOF;
                break;
            }
            if (e.pos < e.len) {
                ed_delete(&e, e.pos, next_ch(&e, e.pos));
            }
        } else if (c == 127 || c == 8) {
            if (e.pos > 0) {
                ed_delete(&e, prev_ch(&e, e.pos), e.pos);
            }
        } else if (c == 1) {
            e.pos = 0;
        } else if (c == 5) {
            e.pos = e.len;
        } else if (c == 2) {
            e.pos = prev_ch(&e, e.pos);
        } else if (c == 6) {
            e.pos = next_ch(&e, e.pos);
        } else if (c == 11) {
            ed_delete(&e, e.pos, e.len);
        } else if (c == 21) {
            ed_delete(&e, 0, e.pos);
        } else if (c == 23) { /* Ctrl-W: previous word */
            size_t i = e.pos;
            while (i > 0 && e.b[i - 1] == ' ') {
                i--;
            }
            while (i > 0 && e.b[i - 1] != ' ') {
                i--;
            }
            ed_delete(&e, i, e.pos);
        } else if (c == 12) { /* Ctrl-L */
            (void)!write(STDOUT_FILENO, "\033[H\033[2J", 7);
        } else if (c == 16 || c == 14 || c == 27) {
            int dir = 0, key = 0; /* dir: -1 up, +1 down */
            if (c == 16) {
                dir = -1;
            } else if (c == 14) {
                dir = 1;
            } else {
                unsigned char s1, s2;
                if (rd_byte(&s1, 40) != 1) {
                    continue; /* lone ESC */
                }
                if (s1 == '[' || s1 == 'O') {
                    if (rd_byte(&s2, 40) != 1) {
                        continue;
                    }
                    if (s2 >= '0' && s2 <= '9') {
                        unsigned char s3 = 0;
                        if (rd_byte(&s3, 40) == 1 && s3 == '~') {
                            key = s2; /* ESC [ n ~ */
                        } else {
                            while (s3 < '@' && rd_byte(&s3, 40) == 1) { /* swallow ;mods etc. */
                            }
                        }
                        if (key == '3') {
                            if (e.pos < e.len) {
                                ed_delete(&e, e.pos, next_ch(&e, e.pos));
                            }
                        } else if (key == '1' || key == '7') {
                            e.pos = 0;
                        } else if (key == '4' || key == '8') {
                            e.pos = e.len;
                        }
                    } else if (s2 == 'A') {
                        dir = -1;
                    } else if (s2 == 'B') {
                        dir = 1;
                    } else if (s2 == 'C') {
                        e.pos = next_ch(&e, e.pos);
                    } else if (s2 == 'D') {
                        e.pos = prev_ch(&e, e.pos);
                    } else if (s2 == 'H') {
                        e.pos = 0;
                    } else if (s2 == 'F') {
                        e.pos = e.len;
                    }
                }
            }
            if (dir != 0) {
                int ni = hidx + dir;
                if (ni >= 0 && ni <= g_nhist) {
                    if (hidx == g_nhist) { /* leaving the live line: keep it */
                        free(draft);
                        draft = strdup(e.b);
                    }
                    hidx = ni;
                    ed_set(&e, hidx == g_nhist ? (draft ? draft : "") : g_hist[hidx]);
                }
            }
        } else if (c >= 32) {
            /* printable ASCII or a UTF-8 byte (controls were handled above) */
            ed_insert(&e, (const char *)&c, 1);
        }
        ed_refresh(&e, prompt, plen);
    }

    if (result == TERM_LINE) {
        e.pos = e.len;
        ed_refresh(&e, prompt, plen);
    }
done:
    restore_tty();
    if (result == TERM_LINE || result == TERM_INTR) {
        fputs("\n", stdout);
        fflush(stdout);
    } else if (result == TERM_TICK) {
        fputs("\r\033[0K", stdout); /* wipe the prompt line; the caller reprints it */
        fflush(stdout);
    }
    free(draft);
    if (result == TERM_LINE) {
        *out = e.b;
    } else {
        free(e.b);
    }
    return result;
}

int term_readline(const char *prompt, int secret, char **out)
{
    char *line = NULL;
    size_t cap = 0;
    int tty = secret && isatty(STDIN_FILENO);

    *out = NULL;
    if (front_active()) {
        return front_readline(prompt, secret, g_idle, out);
    }
    if (!secret && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        int er = edit_line(prompt, out);
        if (er != -2) {
            return er;
        }
    }
    fputs(prompt, stdout);
    fflush(stdout);

    if (tty && tcgetattr(STDIN_FILENO, &g_saved) == 0) {
        struct termios t = g_saved;
        t.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
        g_echo_off = 1;
    }

    errno = 0;
    ssize_t n = getline(&line, &cap, stdin);
    int err = errno;

    if (g_echo_off) {
        restore_tty();
        fputs("\n", stdout); /* the user's Enter was not echoed */
    }

    if (n < 0) {
        free(line);
        if (err == EINTR) {
            clearerr(stdin);
            fputs("\n", stdout);
            return TERM_INTR;
        }
        return TERM_EOF;
    }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        line[--n] = '\0';
    }
    *out = line;
    return TERM_LINE;
}

void term_print_clean(const char *s)
{
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '\n' || ch == '\t' || (ch >= 0x20 && ch != 0x7f)) {
            putchar(ch);
        }
    }
}
