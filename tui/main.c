/*
 * botter TUI: full-screen chat front end for the botter agent.
 *
 * The agent itself (botcore + botter pack, a normal .bot) runs as a child in
 * frontend mode (BOTCORE_FRONTEND=1, see botcore src/front.h); this program
 * owns the terminal. The .bot is appended to this executable by
 * `botter-tui --bundle <agent.bot> <out>`, or given as argv[1] while developing.
 * It is executed from a memfd, so agents that botter builds get the plain
 * botcore runtime (and its REPL), never this UI.
 *
 * Not a tty, or BOTTER_TUI=0: the agent is executed directly (plain REPL).
 */
#define _GNU_SOURCE
#include "tui.h"
#include "cJSON.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static const char TRAILER_MAGIC[8] = {'B', 'O', 'T', 'T', 'U', 'I', '0', '1'};
#define TRAILER_LEN 24 /* u64 payload offset, u64 payload length, magic */

/* ================= terminal ================= */

static int            g_tty = -1;     /* output fd (the terminal) */
static struct termios g_saved;
static int            g_raw;
static int            g_W = 80, g_H = 24;
static int            g_truecolor;
static int            g_sigpipe[2] = {-1, -1};
static volatile sig_atomic_t g_winch, g_quit;

static const char ENTER_SEQ[] = "\033[?1049h\033[?1000h\033[?1006h\033[?2004h\033[5 q\033[?25l\033[2J";
static const char LEAVE_SEQ[] = "\033[0m\033[?2004l\033[?1006l\033[?1000l\033[0 q\033[?25h\033[?1049l";

static void tty_restore(void)
{
    if (g_raw) {
        (void)!write(g_tty, LEAVE_SEQ, sizeof(LEAVE_SEQ) - 1);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
        g_raw = 0;
    }
}

static void on_crash(int sig)
{
    tty_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void on_signal(int sig)
{
    if (sig == SIGWINCH) {
        g_winch = 1;
    } else {
        g_quit = 1;
    }
    int e = errno;
    (void)!write(g_sigpipe[1], "x", 1);
    errno = e;
}

static void tty_size(void)
{
    struct winsize ws;
    if (ioctl(g_tty, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        g_W = ws.ws_col;
        g_H = ws.ws_row;
    }
}

static int tty_setup(void)
{
    if (tcgetattr(STDIN_FILENO, &g_saved) != 0) {
        return -1;
    }
    struct termios t = g_saved;
    t.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) != 0) {
        return -1;
    }
    g_raw = 1;
    (void)!write(g_tty, ENTER_SEQ, sizeof(ENTER_SEQ) - 1);
    atexit(tty_restore);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_crash;
    sa.sa_flags = SA_RESETHAND;
    int crash[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
    for (size_t i = 0; i < sizeof(crash) / sizeof(crash[0]); i++) {
        sigaction(crash[i], &sa, NULL);
    }
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    tty_size();
    return 0;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/*
 * Light or dark terminal background? BOTTER_THEME=light|dark wins; else ask the
 * terminal (OSC 11, followed by DA1 so terminals without OSC 11 answer at once);
 * else COLORFGBG; else dark. Runs in raw mode, before any input is read.
 */
static int tty_is_light(void)
{
    const char *th = getenv("BOTTER_THEME");
    if (th && (strcmp(th, "light") == 0 || strcmp(th, "dark") == 0)) {
        return th[0] == 'l';
    }
    static const char q[] = "\033]11;?\033\\\033[c";
    (void)!write(g_tty, q, sizeof(q) - 1);
    char buf[256];
    size_t n = 0;
    double end = now_s() + 0.3;
    while (n < sizeof(buf) - 1) {
        int ms = (int)((end - now_s()) * 1000);
        struct pollfd p = {STDIN_FILENO, POLLIN, 0};
        if (ms <= 0 || poll(&p, 1, ms) <= 0) {
            break;
        }
        ssize_t r = read(STDIN_FILENO, buf + n, sizeof(buf) - 1 - n);
        if (r <= 0) {
            break;
        }
        n += (size_t)r;
        buf[n] = '\0';
        char *da = strstr(buf, "\033[?");
        if (da && strchr(da, 'c')) {
            break; /* DA1 answered: any OSC 11 reply came before it */
        }
    }
    buf[n] = '\0';
    char *rgb = strstr(buf, "rgb:");
    if (rgb) {
        unsigned r, g, b;
        if (sscanf(rgb + 4, "%x/%x/%x", &r, &g, &b) == 3) {
            int big = strchr(rgb + 4, '/') - (rgb + 4) > 2; /* 16-bit channels */
            double s = big ? 65535.0 : 255.0;
            return 0.2126 * r / s + 0.7152 * g / s + 0.0722 * b / s > 0.5;
        }
    }
    const char *fb = getenv("COLORFGBG"); /* "fg;bg", bg 7 or 15 = light */
    const char *semi = fb ? strrchr(fb, ';') : NULL;
    if (semi) {
        int bg = atoi(semi + 1);
        return bg == 7 || bg == 15;
    }
    return 0;
}

/* ================= colour output ================= */

static int rgb256(uint32_t c)
{
    int r = (int)(c >> 16) & 255, g = (int)(c >> 8) & 255, b = (int)c & 255;
    static const int lv[6] = {0, 95, 135, 175, 215, 255};
#define Q(v) ((v) < 48 ? 0 : (v) < 115 ? 1 : ((v) - 35) / 40)
    int qr = Q(r), qg = Q(g), qb = Q(b);
#undef Q
    int cr = lv[qr], cg = lv[qg], cb = lv[qb];
    int avg = (r + g + b) / 3;
    int gi = avg > 238 ? 23 : (avg - 3) / 10;
    if (gi < 0) {
        gi = 0;
    }
    int gv = 8 + gi * 10;
    int dc = (r - cr) * (r - cr) + (g - cg) * (g - cg) + (b - cb) * (b - cb);
    int dg = (r - gv) * (r - gv) + (g - gv) * (g - gv) + (b - gv) * (b - gv);
    return dg < dc ? 232 + gi : 16 + 36 * qr + 6 * qg + qb;
}

static void put_color(tsb_t *o, int base, uint32_t c)
{
    if (c == TC_NONE) {
        tsb_fmt(o, ";%d", base + 1); /* 39 / 49: default */
    } else if (g_truecolor) {
        tsb_fmt(o, ";%d;2;%u;%u;%u", base, (c >> 16) & 255, (c >> 8) & 255, c & 255);
    } else {
        tsb_fmt(o, ";%d;5;%d", base, rgb256(c));
    }
}

static void sgr(tsb_t *o, tsty_t s, uint32_t fill)
{
    tsb_str(o, "\033[0");
    if (s.at & TA_BOLD) {
        tsb_str(o, ";1");
    }
    if (s.at & TA_ITALIC) {
        tsb_str(o, ";3");
    }
    if (s.at & TA_UNDER) {
        tsb_str(o, ";4");
    }
    if (s.at & TA_STRIKE) {
        tsb_str(o, ";9");
    }
    put_color(o, 38, s.fg);
    put_color(o, 48, s.bg == TC_NONE ? fill : s.bg);
    tsb_str(o, "m");
}

/* Row builder: clips at the row width and pads with a background colour. */
typedef struct {
    tsb_t *o;
    int    w, W;
} rowb_t;

static void rb_text(rowb_t *rb, tsty_t s, uint32_t fill, const char *t, int tw)
{
    if (rb->w >= rb->W || !*t) {
        return;
    }
    sgr(rb->o, s, fill);
    if (rb->w + tw <= rb->W) {
        tsb_str(rb->o, t);
        rb->w += tw;
        return;
    }
    size_t n = strlen(t);
    for (size_t i = 0; i < n;) {
        int l;
        int cw = tu_width(tu_decode(t + i, n - i, &l));
        if (rb->w + cw > rb->W) {
            break;
        }
        tsb_add(rb->o, t + i, (size_t)l);
        rb->w += cw;
        i += (size_t)l;
    }
}

static void rb_fill(rowb_t *rb, uint32_t bg, int upto)
{
    if (upto > rb->W) {
        upto = rb->W;
    }
    if (rb->w >= upto) {
        return;
    }
    sgr(rb->o, sty(T.text, 0), bg);
    while (rb->w < upto) {
        tsb_add(rb->o, " ", 1);
        rb->w++;
    }
}

static void rb_line(rowb_t *rb, const tline_t *l, uint32_t def_bg)
{
    uint32_t fill = l->fill == TC_NONE ? def_bg : l->fill;
    for (int i = 0; i < l->n; i++) {
        rb_text(rb, l->r[i].s, fill, l->r[i].t, l->r[i].w);
    }
}

/* ================= transcript ================= */

enum { B_HEADER, B_RAW, B_USER, B_ASK, B_AGENT, B_THINK, B_TOOL };
enum { TOOL_RUN, TOOL_OK, TOOL_ERR };

typedef struct {
    int      kind;
    tsb_t    text;  /* body; tool: args json; ask: label */
    char    *extra; /* tool: result; ask: answer */
    int      state; /* tool: TOOL_*; ask: secret */
    int      sep;   /* blank line before */
    int      num;   /* notebook label: In [num] (user) / Out[num] (first agent block); 0 = none */
    int      lw, lx; /* laid out for width / expand state; lw < 0 = dirty */
    tlines_t L;
} blk_t;

static blk_t *g_b;
static int    g_nb, g_capb;
static int    g_expand;        /* ctrl+o: full thinking / tool output */
static char  *g_model, *g_cwd;
static int    g_scroll;        /* lines scrolled up from the bottom */
static int    g_last_total = -1, g_last_cw = -1;
static int    g_cell;          /* notebook execution count: one per chat message */
static int    g_out_pending;   /* the next agent block opens the Out[g_cell] cell */

/* Left gutter holding the In/Out labels (0 on narrow screens: labels get their own line). */
static int gutter_w(int cw) { return cw >= 60 ? 10 : 0; }

static void gutter_label(char *out, size_t cap, int user, int num)
{
    char lab[32];
    snprintf(lab, sizeof(lab), user ? "In [%d]:" : "Out[%d]:", num);
    snprintf(out, cap, "%s", lab);
}

static blk_t *blk_add(int kind)
{
    if (g_nb == g_capb) {
        g_capb = g_capb ? g_capb * 2 : 32;
        g_b = realloc(g_b, sizeof(*g_b) * (size_t)g_capb);
        if (!g_b) {
            abort();
        }
    }
    blk_t *b = &g_b[g_nb];
    memset(b, 0, sizeof(*b));
    b->kind = kind;
    b->lw = -1;
    if (kind == B_USER) {
        b->num = ++g_cell;
        g_out_pending = 1;
    } else if (g_out_pending && kind != B_ASK && kind != B_HEADER) {
        b->num = g_cell;
        g_out_pending = 0;
    }
    if (g_nb > 0) {
        int pk = g_b[g_nb - 1].kind;
        int compact = (kind == B_RAW || kind == B_ASK) && (pk == B_RAW || pk == B_ASK);
        b->sep = !compact;
    }
    g_nb++;
    return b;
}

static void blk_text(int kind, const char *t)
{
    blk_t *b = blk_add(kind);
    tsb_str(&b->text, t ? t : "");
}

/* Text for one-line summaries: newlines -> ↵, control chars -> space, cut at maxw columns. */
static void put_flat(tsb_t *o, const char *s, int maxw)
{
    size_t n = strlen(s);
    int w = 0;
    for (size_t i = 0; i < n;) {
        int l;
        uint32_t cp = tu_decode(s + i, n - i, &l);
        const char *rep = NULL;
        if (cp == '\n') {
            rep = "↵";
        } else if (cp < 0x20 || cp == 0x7f) {
            rep = " ";
        }
        int cw = rep ? 1 : tu_width(cp);
        if (w + cw > maxw) {
            tsb_str(o, "…");
            return;
        }
        if (rep) {
            tsb_str(o, rep);
        } else {
            tsb_add(o, s + i, (size_t)l);
        }
        w += cw;
        i += (size_t)l;
    }
}

static void args_summary(tsb_t *o, const char *json)
{
    cJSON *j = cJSON_Parse(json);
    if (!cJSON_IsObject(j)) {
        put_flat(o, json, 120);
        cJSON_Delete(j);
        return;
    }
    int first = 1;
    const cJSON *it;
    cJSON_ArrayForEach(it, j)
    {
        if (!first) {
            tsb_str(o, ", ");
        }
        first = 0;
        tsb_str(o, it->string ? it->string : "?");
        tsb_str(o, ": ");
        if (cJSON_IsString(it)) {
            tsb_str(o, "\"");
            put_flat(o, it->valuestring, 60);
            tsb_str(o, "\"");
        } else {
            char *v = cJSON_PrintUnformatted(it);
            put_flat(o, v ? v : "?", 40);
            free(v);
        }
        if (o->len > 400) {
            tsb_str(o, ", …");
            break;
        }
    }
    cJSON_Delete(j);
}

/* Keep the first `keep` lines of L, then a "+N lines" hint. */
static void collapse(tlines_t *L, int from, int keep, const char *indent)
{
    int n = L->n - from;
    if (g_expand || n <= keep + 1) {
        return;
    }
    tlines_t tail = {0};
    for (int i = from + keep; i < L->n; i++) {
        *tl_new(&tail, 0) = L->l[i];
    }
    L->n = from + keep;
    tl_free(&tail);
    tline_t *h = tl_new(L, TC_NONE);
    char m[96];
    snprintf(m, sizeof(m), "%s… +%d lines (ctrl+o to expand)", indent, n - keep);
    tl_put(h, sty(T.muted, TA_ITALIC), m, strlen(m));
}

static void trunc_left(tline_t *l, tsty_t s, const char *t, int maxw)
{
    size_t n = strlen(t);
    if (tu_strwidth(t, n) <= maxw) {
        tl_put(l, s, t, n);
        return;
    }
    size_t i = 0;
    while (i < n && tu_strwidth(t + i, n - i) > maxw - 1) {
        int cl;
        tu_decode(t + i, n - i, &cl);
        i += (size_t)cl;
    }
    tl_put(l, s, "…", strlen("…"));
    tl_put(l, s, t + i, n - i);
}

static void lay_header(blk_t *b, int cw)
{
    tlines_t logo = {0};
    lay_logo(&logo);
    int lw = logo_width();
    int bw = cw < 74 ? cw : 74;
    int inner = bw - 4;
    tsty_t border = sty(T.surface, 0), title = sty(T.accent, TA_BOLD), mut = sty(T.muted, 0), txt = sty(T.text, 0);
    int side = inner >= lw + 3 + 22;
    int tw = side ? inner - lw - 3 : inner;

    tlines_t info = {0};
    tline_t *l;
    tl_blank(&info);
    l = tl_new(&info, TC_NONE);
    tl_put_trunc(l, title, "Botter", tw);
    l = tl_new(&info, TC_NONE);
    tl_put_trunc(l, mut, "the agent that builds agents", tw);
    tl_blank(&info);
    l = tl_new(&info, TC_NONE);
    tl_put(l, mut, "model ", 6);
    tl_put_trunc(l, g_model ? txt : mut, g_model ? g_model : "not connected", tw - 6);
    l = tl_new(&info, TC_NONE);
    tl_put(l, mut, "cwd   ", 6);
    if (g_cwd) {
        trunc_left(l, txt, g_cwd, tw - 6);
    } else {
        char cwd[4096];
        trunc_left(l, txt, getcwd(cwd, sizeof(cwd)) ? cwd : "?", tw - 6);
    }
    tl_blank(&info);
    l = tl_new(&info, TC_NONE);
    tl_put_trunc(l, mut, "Describe the agent you want to build.", tw);
    l = tl_new(&info, TC_NONE);
    tl_put_trunc(l, mut, "/exit quits · ctrl+o expands details", tw);

    tlines_t body = {0};
    if (side) {
        int h = logo.n > info.n ? logo.n : info.n;
        for (int y = 0; y < h; y++) {
            l = tl_new(&body, TC_NONE);
            if (y < logo.n) {
                tl_cat(l, &logo.l[y]);
            }
            tl_pad(l, lw + 3, txt);
            if (y < info.n) {
                tl_cat(l, &info.l[y]);
            }
        }
    } else {
        int pad = (inner - lw) / 2;
        for (int y = 0; y < logo.n; y++) {
            l = tl_new(&body, TC_NONE);
            tl_pad(l, pad, txt);
            tl_cat(l, &logo.l[y]);
        }
        for (int y = 0; y < info.n; y++) {
            l = tl_new(&body, TC_NONE);
            tl_cat(l, &info.l[y]);
        }
    }
    tl_free(&logo);
    tl_free(&info);

    l = tl_new(&b->L, TC_NONE);
    tl_put(l, border, "╭", strlen("╭"));
    for (int x = 0; x < bw - 2; x++) {
        tl_put(l, border, "─", strlen("─"));
    }
    tl_put(l, border, "╮", strlen("╮"));
    for (int y = -1; y <= body.n; y++) {
        l = tl_new(&b->L, TC_NONE);
        tl_put(l, border, "│ ", strlen("│ "));
        if (y >= 0 && y < body.n) {
            tl_cat(l, &body.l[y]);
        }
        tl_pad(l, bw - 1, txt);
        tl_put(l, border, "│", strlen("│"));
    }
    l = tl_new(&b->L, TC_NONE);
    tl_put(l, border, "╰", strlen("╰"));
    for (int x = 0; x < bw - 2; x++) {
        tl_put(l, border, "─", strlen("─"));
    }
    tl_put(l, border, "╯", strlen("╯"));
    tl_free(&body);
}

/* A user message as a notebook input cell: a tinted, bordered box. */
static void lay_user_cell(tlines_t *L, const char *t, size_t n, int w)
{
    tsty_t bd = sty(T.surface, 0), in = sty_bg(T.text, T.user_bg, 0);
    int inner = w - 4 > 1 ? w - 4 : 1;
    tlines_t body = {0};
    lay_text(&body, t, n, in, inner, T.user_bg);
    tline_t *l = tl_new(L, TC_NONE);
    tl_put(l, bd, "╭", strlen("╭"));
    for (int x = 0; x < w - 2; x++) {
        tl_put(l, bd, "─", strlen("─"));
    }
    tl_put(l, bd, "╮", strlen("╮"));
    for (int y = 0; y < body.n; y++) {
        l = tl_new(L, TC_NONE);
        tl_put(l, bd, "│", strlen("│"));
        tl_put(l, in, " ", 1);
        tl_cat(l, &body.l[y]);
        tl_pad(l, w - 1, in);
        tl_put(l, bd, "│", strlen("│"));
    }
    l = tl_new(L, TC_NONE);
    tl_put(l, bd, "╰", strlen("╰"));
    for (int x = 0; x < w - 2; x++) {
        tl_put(l, bd, "─", strlen("─"));
    }
    tl_put(l, bd, "╯", strlen("╯"));
    tl_free(&body);
}

/* Append C (laid out for width w) to L behind a gutter of width g; the label goes on line `at`
 * of C. A line's fill colour (code blocks) is kept inside the content column, not the gutter. */
static void put_gutter(tlines_t *L, tlines_t *C, int g, int w, const char *label, uint32_t lc, int at)
{
    if (g == 0 && label) {
        tline_t *l = tl_new(L, TC_NONE);
        tl_put(l, sty(lc, TA_BOLD), label, strlen(label));
    }
    for (int y = 0; y < C->n; y++) {
        uint32_t fill = C->l[y].fill;
        tline_t *l = tl_new(L, g > 0 ? TC_NONE : fill);
        if (g > 0) {
            if (label && y == at) {
                int lw = tu_strwidth(label, strlen(label));
                tl_pad(l, g - 2 - lw, sty(T.text, 0));
                tl_put(l, sty(lc, TA_BOLD), label, strlen(label));
            }
            tl_pad(l, g, sty(T.text, 0));
        }
        int r0 = l->n;
        tl_cat(l, &C->l[y]);
        if (g > 0 && fill != TC_NONE) {
            for (int k = r0; k < l->n; k++) {
                if (l->r[k].s.bg == TC_NONE) {
                    l->r[k].s.bg = fill;
                }
            }
            tl_pad(l, g + w, sty_bg(T.text, fill, 0));
        }
    }
    tl_free(C);
}

static void lay_block(blk_t *b, int cw)
{
    tl_free(&b->L);
    if (b->sep) {
        tl_blank(&b->L);
    }
    const char *t = b->text.p ? b->text.p : "";
    if (b->kind == B_HEADER) {
        lay_header(b, cw);
        b->lw = cw;
        b->lx = g_expand;
        return;
    }
    int g = gutter_w(cw), w = cw - g;
    tlines_t C = {0};
    int at = 0;
    switch (b->kind) {
    case B_RAW:
        lay_ansi(&C, t, b->text.len, w);
        break;
    case B_USER:
        lay_user_cell(&C, t, b->text.len, w);
        at = 1;
        break;
    case B_ASK: {
        tcells_t *c = tc_new();
        tc_add(c, t, b->text.len, sty(T.muted, 0));
        tc_add(c, " ", 1, sty(T.text, 0));
        const char *ans = b->state ? "••••••••" : (b->extra ? b->extra : "");
        tc_add(c, ans, strlen(ans), sty(T.text, TA_BOLD));
        tc_flow(c, &C, w, TC_NONE, 1);
        tc_free(c);
        break;
    }
    case B_AGENT:
        if (b->text.len == 0 || !t[strspn(t, " \n\t")]) {
            tline_t *l = tl_new(&C, TC_NONE);
            tl_put(l, sty(T.muted, TA_ITALIC), "(empty response)", strlen("(empty response)"));
        } else {
            lay_markdown(&C, t, w);
        }
        break;
    case B_THINK: {
        tline_t *h = tl_new(&C, TC_NONE);
        tl_put(h, sty(T.purple, 0), "✻ ", strlen("✻ "));
        tl_put(h, sty(T.muted, TA_ITALIC), "Thinking", 8);
        int bf = C.n;
        lay_text(&C, t, b->text.len, sty(T.muted, TA_ITALIC), w - 2, TC_NONE);
        collapse(&C, bf, 4, "");
        tl_prefix(&C, bf, sty(T.muted, 0), "  ", sty(T.muted, 0), "  ");
        break;
    }
    case B_TOOL: {
        /* text = name NUL args-json (see on_record) */
        const char *nm = t;
        const char *args = b->text.p && strlen(t) + 1 < b->text.len ? t + strlen(t) + 1 : "{}";
        tsb_t a = {0};
        args_summary(&a, args);
        uint32_t dot = b->state == TOOL_RUN ? T.muted : b->state == TOOL_OK ? T.ok : T.err;
        tcells_t *c = tc_new();
        tc_add(c, nm, strlen(nm), sty(T.text, TA_BOLD));
        tc_add(c, "(", 1, sty(T.muted, 0));
        if (a.len) {
            tc_add(c, a.p, a.len, sty(T.muted, 0));
        }
        tc_add(c, ")", 1, sty(T.muted, 0));
        tc_flow(c, &C, w - 2, TC_NONE, 1);
        tc_free(c);
        tsb_free(&a);
        collapse(&C, 0, 3, "");
        tl_prefix(&C, 0, sty(dot, 0), "⏺ ", sty(T.text, 0), "  ");
        if (b->state != TOOL_RUN) {
            const char *r = b->extra && *b->extra ? b->extra : "(no output)";
            int rf = C.n;
            lay_text(&C, r, strlen(r), sty(b->state == TOOL_ERR ? T.err : T.muted, 0), w - 5, TC_NONE);
            collapse(&C, rf, 3, "");
            tl_prefix(&C, rf, sty(T.muted, 0), "  ⎿  ", sty(T.muted, 0), "     ");
        }
        break;
    }
    }
    char lab[32];
    if (b->num) {
        gutter_label(lab, sizeof(lab), b->kind == B_USER, b->num);
    }
    put_gutter(&b->L, &C, g, w, b->num ? lab : NULL, b->kind == B_USER ? T.in_label : T.out_label, at);
    b->lw = cw;
    b->lx = g_expand;
}

/* Lay out what changed; returns the total number of transcript lines. */
static int layout_all(int cw)
{
    int total = 0;
    for (int i = 0; i < g_nb; i++) {
        blk_t *b = &g_b[i];
        if (b->lw != cw || ((b->kind == B_THINK || b->kind == B_TOOL) && b->lx != g_expand)) {
            lay_block(b, cw);
        }
        total += b->L.n;
    }
    return total;
}

/* ================= child (the agent) ================= */

static pid_t g_child = -1;
static int   g_cin = -1, g_cout = -1;
static tsb_t g_rx;          /* bytes from the child not yet processed */
static int   g_raw_open;    /* last block is RAW and may receive more text */

static int    g_await;      /* child is waiting for a line */
static int    g_secret;
static int    g_chat;       /* the prompt is the chat prompt (">> ") */
static char   g_label[256];
static int    g_busy;
static char   g_busy_label[96];
static double g_busy_t0;
static char   g_notice[160];
static double g_notice_until;

static void notice(const char *m)
{
    snprintf(g_notice, sizeof(g_notice), "%s", m);
    g_notice_until = now_s() + 3;
}

static void send_json(cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    s[n] = '\n';
    for (size_t off = 0; off < n + 1;) {
        ssize_t w = write(g_cin, s + off, n + 1 - off);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            break;
        }
        off += (size_t)w;
    }
    explicit_bzero(s, n + 1);
    free(s);
}

static void send_flag(const char *k)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddTrueToObject(o, k);
    send_json(o);
    cJSON_Delete(o);
    g_await = 0;
}

static void interrupt_child(void)
{
    if (g_child > 0) {
        kill(g_child, SIGINT);
        notice("interrupting…");
    }
}

/* ================= editor ================= */

static tsb_t  E;        /* input text */
static size_t Epos;     /* cursor (byte offset) */
static int    Etop;     /* first visible visual line */
static tsb_t  g_stash;  /* chat draft kept while a question is asked */
static int    g_stashed;
static char  *g_hist[300];
static int    g_nh, g_hidx = -1;
static char  *g_draft;

static void ed_clear(void)
{
    if (E.p) {
        explicit_bzero(E.p, E.cap);
    }
    E.len = 0;
    if (E.p) {
        E.p[0] = '\0';
    }
    Epos = 0;
    Etop = 0;
}

static void ed_set(const char *s)
{
    ed_clear();
    tsb_str(&E, s);
    Epos = E.len;
}

static void ed_insert(const char *s, size_t n)
{
    if (!E.p) {
        tsb_add(&E, "", 0);
    }
    if (E.len + n + 1 > E.cap) {
        size_t nc = (E.len + n + 1) * 2;
        char *nb = malloc(nc);
        if (!nb) {
            abort();
        }
        memcpy(nb, E.p, E.len + 1);
        explicit_bzero(E.p, E.cap); /* may hold a secret */
        free(E.p);
        E.p = nb;
        E.cap = nc;
    }
    memmove(E.p + Epos + n, E.p + Epos, E.len - Epos + 1);
    memcpy(E.p + Epos, s, n);
    E.len += n;
    Epos += n;
    g_hidx = -1;
}

static void ed_delete(size_t a, size_t b)
{
    if (b <= a || b > E.len) {
        return;
    }
    memmove(E.p + a, E.p + b, E.len - b + 1);
    E.len -= b - a;
    Epos = a;
}

static int is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

static size_t ed_prev(size_t i)
{
    while (i > 0 && is_cont((unsigned char)E.p[--i])) {
    }
    return i;
}

static size_t ed_next(size_t i)
{
    if (i < E.len) {
        i++;
    }
    while (i < E.len && is_cont((unsigned char)E.p[i])) {
        i++;
    }
    return i;
}

static size_t ed_word_left(size_t i)
{
    while (i > 0 && isspace((unsigned char)E.p[i - 1])) {
        i--;
    }
    while (i > 0 && !isspace((unsigned char)E.p[i - 1])) {
        i--;
    }
    return i;
}

static size_t ed_word_right(size_t i)
{
    while (i < E.len && isspace((unsigned char)E.p[i])) {
        i++;
    }
    while (i < E.len && !isspace((unsigned char)E.p[i])) {
        i++;
    }
    return i;
}

typedef struct {
    size_t a, b;
} vl_t;

static int chw(size_t i, int *len)
{
    uint32_t cp = tu_decode(E.p + i, E.len - i, len);
    return g_secret ? 1 : tu_width(cp);
}

static int cols_between(size_t a, size_t b);

/* Visual lines of the input at text width tw (word wrap + hard newlines). */
static int ed_lines(vl_t **out, int tw)
{
    static vl_t *v;
    static int   cap;
    int n = 0;
    size_t a = 0, i = 0, brk = 0; /* brk: just after the last space on this line (0 = none) */
    int col = 0;
#define PUSH(x, y)                                                                                                \
    do {                                                                                                          \
        if (n == cap) {                                                                                           \
            cap = cap ? cap * 2 : 16;                                                                             \
            v = realloc(v, sizeof(*v) * (size_t)cap);                                                             \
            if (!v) {                                                                                             \
                abort();                                                                                          \
            }                                                                                                     \
        }                                                                                                         \
        v[n].a = (x);                                                                                             \
        v[n].b = (y);                                                                                             \
        n++;                                                                                                      \
    } while (0)
    while (i < E.len) {
        if (E.p[i] == '\n') {
            PUSH(a, i);
            a = ++i;
            col = 0;
            brk = 0;
            continue;
        }
        int l, w = chw(i, &l);
        if (col + w > tw && col > 0) {
            if (brk > a && !g_secret && E.p[i] != ' ') { /* word wrap: move the partial word down */
                PUSH(a, brk);
                col = cols_between(brk, i);
                a = brk;
            } else {
                PUSH(a, i);
                a = i;
                col = 0;
            }
            brk = 0;
            continue;
        }
        col += w;
        if (E.p[i] == ' ') {
            brk = i + 1;
        }
        i += (size_t)l;
    }
    PUSH(a, E.len);
#undef PUSH
    *out = v;
    return n;
}

static int cols_between(size_t a, size_t b)
{
    int c = 0;
    for (size_t i = a; i < b;) {
        int l;
        c += chw(i, &l);
        i += (size_t)l;
    }
    return c;
}

static void ed_cursor(const vl_t *v, int n, int *row, int *col)
{
    int r = 0;
    for (int i = 0; i < n; i++) {
        if (Epos >= v[i].a && Epos <= v[i].b) {
            r = i;
            if (!(Epos == v[i].b && i + 1 < n && v[i + 1].a == v[i].b)) {
                break; /* at a soft wrap the cursor belongs to the next line */
            }
        }
    }
    *row = r;
    *col = cols_between(v[r].a, Epos);
}

static size_t pos_at_col(const vl_t *v, int r, int col)
{
    size_t i = v[r].a;
    int c = 0;
    while (i < v[r].b) {
        int l, w = chw(i, &l);
        if (c + w > col) {
            break;
        }
        c += w;
        i += (size_t)l;
    }
    return i;
}

static void hist_add(const char *s)
{
    if (!*s || (g_nh && strcmp(g_hist[g_nh - 1], s) == 0)) {
        return;
    }
    if (g_nh == (int)(sizeof(g_hist) / sizeof(g_hist[0]))) {
        free(g_hist[0]);
        memmove(g_hist, g_hist + 1, sizeof(g_hist[0]) * (size_t)(g_nh - 1));
        g_nh--;
    }
    g_hist[g_nh++] = strdup(s);
}

static void hist_move(int dir)
{
    if (!g_chat || g_secret || g_nh == 0) {
        return;
    }
    int idx = g_hidx < 0 ? g_nh : g_hidx;
    int ni = idx + dir;
    if (ni < 0 || ni > g_nh) {
        return;
    }
    if (g_hidx < 0) {
        free(g_draft);
        g_draft = strdup(E.p ? E.p : "");
    }
    ed_set(ni == g_nh ? (g_draft ? g_draft : "") : g_hist[ni]);
    g_hidx = ni == g_nh ? -1 : ni;
}

/* ================= screen ================= */

static tsb_t *g_prev;  /* last frame, one string per row */
static int    g_prevH;
static int    g_full = 1;
static int    g_dirty = 1;
static int    g_tH = 1;   /* transcript rows (for paging) */

static void row_reset(tsb_t *rows, int n)
{
    for (int i = 0; i < n; i++) {
        tsb_free(&rows[i]);
    }
}

static const char *const SPIN[] = {"✢", "✳", "✶", "✻", "✽", "✻", "✶", "✳"};

/* Chat box: starts after the margin and gutter, ends where transcript cells end. */
static int box_x0(void) { return 1 + gutter_w(g_W - 2); }
static int box_w(void) { return g_W - 1 - box_x0(); }
static int input_tw(void) { return box_w() - 6 > 1 ? box_w() - 6 : 1; }

/* Gutter of a chat-box row: margin, right-aligned label (if any), up to the box. */
static void rb_gutter(rowb_t *rb, const char *label, uint32_t lc)
{
    int x0 = box_x0();
    if (label && x0 > 1) {
        int lw = tu_strwidth(label, strlen(label));
        rb_fill(rb, T.bg, x0 - 2 - lw);
        rb_text(rb, sty(lc, TA_BOLD), T.bg, label, lw);
    }
    rb_fill(rb, T.bg, x0);
}

static void render(void)
{
    int W = g_W, H = g_H;
    tsb_t *cur = calloc((size_t)H, sizeof(*cur));
    if (!cur) {
        return;
    }
    int curx = -1, cury = -1;

    if (W < 24 || H < 8) {
        for (int y = 0; y < H; y++) {
            rowb_t rb = {&cur[y], 0, W};
            if (y == H / 2) {
                const char *m = "terminal too small";
                rb_fill(&rb, T.bg, (W - (int)strlen(m)) / 2);
                rb_text(&rb, sty(T.muted, 0), T.bg, m, (int)strlen(m));
            }
            rb_fill(&rb, T.bg, W);
        }
        goto flush;
    }

    {
        /* --- input box --- */
        vl_t *v;
        int tw = input_tw();
        int nvl = ed_lines(&v, tw);
        int crow, ccol;
        ed_cursor(v, nvl, &crow, &ccol);
        int footer = H >= 12 ? 1 : 0;
        int maxbox = (H - 4 - footer) / 2;
        if (maxbox < 1) {
            maxbox = 1;
        }
        int bl = nvl < maxbox ? nvl : maxbox;
        if (crow < Etop) {
            Etop = crow;
        }
        if (crow >= Etop + bl) {
            Etop = crow - bl + 1;
        }
        if (Etop > nvl - bl) {
            Etop = nvl - bl;
        }
        if (Etop < 0) {
            Etop = 0;
        }
        int box_top = H - footer - (bl + 2);
        int status_y = box_top - 1;
        int tH = status_y;
        g_tH = tH;

        /* --- transcript --- */
        int cw = W - 2;
        int total = layout_all(cw);
        if (g_last_total >= 0 && g_scroll > 0 && cw == g_last_cw && total > g_last_total) {
            g_scroll += total - g_last_total; /* keep the view still while reading back */
        }
        g_last_total = total;
        g_last_cw = cw;
        int maxs = total > tH ? total - tH : 0;
        if (g_scroll > maxs) {
            g_scroll = maxs;
        }
        if (g_scroll < 0) {
            g_scroll = 0;
        }
        int first = total > tH ? total - tH - g_scroll : 0;
        int bi = 0, li = first;
        while (bi < g_nb && li >= g_b[bi].L.n) {
            li -= g_b[bi].L.n;
            bi++;
        }
        for (int y = 0; y < tH; y++) {
            rowb_t rb = {&cur[y], 0, W};
            rb_fill(&rb, T.bg, 1);
            if (bi < g_nb) {
                const tline_t *l = &g_b[bi].L.l[li];
                uint32_t fill = l->fill == TC_NONE ? T.bg : l->fill;
                rb_line(&rb, l, T.bg);
                rb_fill(&rb, fill, W - 1);
                if (++li >= g_b[bi].L.n) {
                    li = 0;
                    bi++;
                    while (bi < g_nb && g_b[bi].L.n == 0) {
                        bi++;
                    }
                }
            }
            rb_fill(&rb, T.bg, W);
        }

        /* --- status line --- */
        {
            rowb_t rb = {&cur[status_y], 0, W};
            rb_fill(&rb, T.bg, 1);
            double t = now_s();
            if (g_notice[0] && t < g_notice_until) {
                rb_text(&rb, sty(T.muted, TA_ITALIC), T.bg, g_notice, tu_strwidth(g_notice, strlen(g_notice)));
            } else if (g_busy && !g_await) {
                int f = (int)((t - g_busy_t0) * 8) % 8;
                char m[200];
                rb_text(&rb, sty(T.accent, TA_BOLD), T.bg, SPIN[f], 1);
                snprintf(m, sizeof(m), " %s…", g_busy_label);
                rb_text(&rb, sty(T.accent, 0), T.bg, m, tu_strwidth(m, strlen(m)));
                snprintf(m, sizeof(m), " (%ds · esc to interrupt)", (int)(t - g_busy_t0));
                rb_text(&rb, sty(T.muted, 0), T.bg, m, tu_strwidth(m, strlen(m)));
            }
            if (g_scroll > 0) {
                char m[96];
                snprintf(m, sizeof(m), "↓ %d more lines · pgdn ", g_scroll);
                int mw = tu_strwidth(m, strlen(m));
                if (rb.w + mw + 2 <= W) {
                    rb_fill(&rb, T.bg, W - mw);
                    rb_text(&rb, sty(T.info, 0), T.bg, m, mw);
                }
            }
            rb_fill(&rb, T.bg, W);
        }

        /* --- box --- */
        uint32_t bc = g_await ? T.accent : T.surface;
        tsty_t bs = sty(bc, 0);
        int bx1 = box_x0() + box_w(); /* one past the box's right edge */
        char inlab[32];
        gutter_label(inlab, sizeof(inlab), 1, g_cell + 1);
        {
            rowb_t rb = {&cur[box_top], 0, W};
            rb_gutter(&rb, NULL, 0);
            rb_text(&rb, bs, T.bg, "╭─", 2);
            if (g_await && !g_chat && g_label[0]) {
                tline_t lab = {.fill = TC_NONE};
                tl_put(&lab, sty(T.text, TA_BOLD), " ", 1);
                tl_put_trunc(&lab, sty(T.text, TA_BOLD), g_label, box_w() - 6);
                tl_put(&lab, sty(T.text, TA_BOLD), " ", 1);
                rb_line(&rb, &lab, T.bg);
                for (int k = 0; k < lab.n; k++) {
                    free(lab.r[k].t);
                }
                free(lab.r);
            }
            while (rb.w < bx1 - 1) {
                rb_text(&rb, bs, T.bg, "─", 1);
            }
            rb_text(&rb, bs, T.bg, "╮", 1);
            rb_fill(&rb, T.bg, W);
        }
        for (int i = 0; i < bl; i++) {
            int r = Etop + i;
            rowb_t rb = {&cur[box_top + 1 + i], 0, W};
            rb_gutter(&rb, i == 0 && (g_chat || !g_await) ? inlab : NULL, T.in_label);
            int bx0 = rb.w;
            rb_text(&rb, bs, T.bg, "│", 1);
            rb_text(&rb, sty(T.text, 0), T.user_bg, " ", 1);
            if (r == 0) {
                rb_text(&rb, sty(g_await ? T.accent : T.muted, TA_BOLD), T.user_bg, g_secret ? "⚿ " : "❯ ", 2);
            } else {
                rb_fill(&rb, T.user_bg, bx0 + 4);
            }
            if (E.len == 0 && r == 0) {
                const char *ph = !g_await ? (g_busy ? "Botter is working… (type ahead; esc interrupts)" : "")
                               : g_secret ? "hidden input"
                               : g_chat   ? "Describe an agent to build, or ask anything"
                                          : "type your answer";
                rb_text(&rb, sty(T.muted, TA_ITALIC), T.user_bg, ph, tu_strwidth(ph, strlen(ph)));
            } else if (g_secret) {
                for (size_t k = v[r].a; k < v[r].b;) {
                    int l;
                    tu_decode(E.p + k, v[r].b - k, &l);
                    rb_text(&rb, sty(T.text, 0), T.user_bg, "•", 1);
                    k += (size_t)l;
                }
            } else {
                char *seg = strndup(E.p + v[r].a, v[r].b - v[r].a);
                if (seg) {
                    rb_text(&rb, sty(T.text, 0), T.user_bg, seg, tu_strwidth(seg, strlen(seg)));
                    free(seg);
                }
            }
            rb_fill(&rb, T.user_bg, bx1 - 1);
            rb_text(&rb, bs, T.bg, "│", 1);
            rb_fill(&rb, T.bg, W);
            if (r == crow) {
                cury = box_top + 1 + i;
                curx = bx0 + 4 + ccol;
            }
        }
        {
            rowb_t rb = {&cur[box_top + bl + 1], 0, W};
            rb_gutter(&rb, NULL, 0);
            rb_text(&rb, bs, T.bg, "╰", 1);
            if (nvl > bl) {
                char m[48];
                snprintf(m, sizeof(m), "─ %d/%d ", crow + 1, nvl);
                rb_text(&rb, bs, T.bg, m, tu_strwidth(m, strlen(m)));
            }
            while (rb.w < bx1 - 1) {
                rb_text(&rb, bs, T.bg, "─", 1);
            }
            rb_text(&rb, bs, T.bg, "╯", 1);
            rb_fill(&rb, T.bg, W);
        }

        /* --- footer --- */
        if (footer) {
            rowb_t rb = {&cur[H - 1], 0, W};
            rb_fill(&rb, T.bg, 1);
            const char *full = "enter send · alt+enter newline · pgup/pgdn/wheel scroll · ctrl+o expand · ctrl+c quit";
            const char *mid = "enter send · alt+enter newline · pgup/pgdn scroll";
            const char *small = "ctrl+c quit";
            int mw = g_model ? tu_strwidth(g_model, strlen(g_model)) + 2 : 0;
            const char *h = tu_strwidth(full, strlen(full)) + mw + 2 <= W ? full
                          : tu_strwidth(mid, strlen(mid)) + mw + 2 <= W ? mid
                                                                         : small;
            rb_text(&rb, sty(T.muted, 0), T.bg, h, tu_strwidth(h, strlen(h)));
            if (g_model && rb.w + mw + 1 <= W) {
                rb_fill(&rb, T.bg, W - mw);
                rb_text(&rb, sty(T.purple, 0), T.bg, g_model, mw - 2);
            }
            rb_fill(&rb, T.bg, W);
        }
    }

flush:;
    tsb_t o = {0};
    tsb_str(&o, "\033[?2026h\033[?25l");
    if (g_full || g_prevH != H) {
        tsb_str(&o, "\033[0m\033[2J");
    }
    for (int y = 0; y < H; y++) {
        if (!g_full && g_prevH == H && g_prev && g_prev[y].p && cur[y].p && strcmp(g_prev[y].p, cur[y].p) == 0) {
            continue;
        }
        tsb_fmt(&o, "\033[%d;1H", y + 1);
        if (cur[y].p) {
            tsb_add(&o, cur[y].p, cur[y].len);
        }
    }
    tsb_str(&o, "\033[0m");
    if (curx >= 0 && curx < W) {
        tsb_fmt(&o, "\033[%d;%dH\033[?25h", cury + 1, curx + 1);
    }
    tsb_str(&o, "\033[?2026l");
    for (size_t off = 0; off < o.len;) {
        ssize_t w = write(g_tty, o.p + off, o.len - off);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            break;
        }
        off += (size_t)w;
    }
    tsb_free(&o);
    if (g_prev) {
        row_reset(g_prev, g_prevH);
        free(g_prev);
    }
    g_prev = cur;
    g_prevH = H;
    g_full = 0;
    g_dirty = 0;
}

/* ================= events from the agent ================= */

static void raw_text(const char *s, size_t n)
{
    if (n == 0) {
        return;
    }
    if (!g_raw_open || g_nb == 0 || g_b[g_nb - 1].kind != B_RAW) {
        blk_add(B_RAW);
        g_raw_open = 1;
    }
    blk_t *b = &g_b[g_nb - 1];
    tsb_add(&b->text, s, n);
    b->lw = -1;
}

static void strip_sgr(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; *s && o + 1 < cap; s++) {
        if (*s == '\033') {
            if (s[1] == '[') {
                s += 2;
                while (*s && !(*s >= '@' && *s <= '~')) {
                    s++;
                }
                if (!*s) {
                    break;
                }
            }
            continue;
        }
        if ((unsigned char)*s >= 0x20 || *s == '\t') {
            out[o++] = *s;
        }
    }
    out[o] = '\0';
    while (o > 0 && out[o - 1] == ' ') {
        out[--o] = '\0';
    }
    size_t lead = strspn(out, " ");
    memmove(out, out + lead, strlen(out + lead) + 1);
}

static const char *jstr(const cJSON *j, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static void on_record(const char *json)
{
    cJSON *j = cJSON_Parse(json);
    const char *ev = jstr(j, "ev");
    g_raw_open = 0;
    if (!strcmp(ev, "prompt")) {
        char lab[256];
        strip_sgr(jstr(j, "text"), lab, sizeof(lab));
        g_await = 1;
        g_secret = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "secret"));
        g_chat = !g_secret && strcmp(lab, ">>") == 0;
        snprintf(g_label, sizeof(g_label), "%s", g_chat ? "" : lab);
        if (g_chat) {
            if (g_stashed) {
                ed_set(g_stash.p ? g_stash.p : "");
                tsb_free(&g_stash);
                g_stashed = 0;
            }
        } else {
            if (!g_stashed) {
                tsb_free(&g_stash);
                tsb_str(&g_stash, E.p ? E.p : "");
                g_stashed = 1;
            }
            ed_clear();
        }
    } else if (!strcmp(ev, "busy")) {
        if (!g_busy) {
            g_busy_t0 = now_s();
        }
        g_busy = 1;
        snprintf(g_busy_label, sizeof(g_busy_label), "%s", jstr(j, "label"));
    } else if (!strcmp(ev, "idle")) {
        g_busy = 0;
    } else if (!strcmp(ev, "info")) {
        free(g_model);
        free(g_cwd);
        g_model = strdup(jstr(j, "model"));
        g_cwd = strdup(jstr(j, "cwd"));
        for (int i = 0; i < g_nb; i++) {
            if (g_b[i].kind == B_HEADER) {
                g_b[i].lw = -1;
            }
        }
    } else if (!strcmp(ev, "thinking")) {
        blk_text(B_THINK, jstr(j, "text"));
    } else if (!strcmp(ev, "text") || !strcmp(ev, "reply")) {
        blk_text(B_AGENT, jstr(j, "text"));
    } else if (!strcmp(ev, "tool")) {
        blk_t *b = blk_add(B_TOOL);
        const char *nm = jstr(j, "name");
        tsb_add(&b->text, nm, strlen(nm) + 1); /* name NUL args */
        tsb_str(&b->text, jstr(j, "args"));
        b->state = TOOL_RUN;
        snprintf(g_busy_label, sizeof(g_busy_label), "Running %s", nm);
    } else if (!strcmp(ev, "tool_result")) {
        for (int i = g_nb - 1; i >= 0; i--) {
            if (g_b[i].kind == B_TOOL && g_b[i].state == TOOL_RUN) {
                const char *r = jstr(j, "text");
                free(g_b[i].extra);
                g_b[i].extra = strdup(r);
                g_b[i].state = strncmp(r, "ERROR", 5) == 0 ? TOOL_ERR : TOOL_OK;
                g_b[i].lw = -1;
                break;
            }
        }
        snprintf(g_busy_label, sizeof(g_busy_label), "Thinking");
    }
    cJSON_Delete(j);
    g_dirty = 1;
}

/* Split the child's output into ordinary text and "\x1e{json}\n" records. */
static void process_rx(void)
{
    size_t i = 0;
    while (i < g_rx.len) {
        char *rs = memchr(g_rx.p + i, '\x1e', g_rx.len - i);
        if (!rs) {
            /* keep an incomplete UTF-8 tail for the next read */
            size_t end = g_rx.len, k = end;
            while (k > i && end - k < 4 && is_cont((unsigned char)g_rx.p[k - 1])) {
                k--;
            }
            if (k > i && end - k < 4 && (unsigned char)g_rx.p[k - 1] >= 0xC0) {
                int need = ((unsigned char)g_rx.p[k - 1] >> 5) == 6 ? 2 : ((unsigned char)g_rx.p[k - 1] >> 4) == 14 ? 3 : 4;
                if ((int)(end - k + 1) < need) {
                    end = k - 1;
                }
            }
            raw_text(g_rx.p + i, end - i);
            i = end;
            break;
        }
        size_t at = (size_t)(rs - g_rx.p);
        raw_text(g_rx.p + i, at - i);
        char *nl = memchr(rs, '\n', g_rx.len - at);
        if (!nl) {
            i = at; /* incomplete record */
            break;
        }
        *nl = '\0';
        on_record(rs + 1);
        i = (size_t)(nl - g_rx.p) + 1;
    }
    memmove(g_rx.p, g_rx.p + i, g_rx.len - i);
    g_rx.len -= i;
    if (g_rx.p) {
        g_rx.p[g_rx.len] = '\0';
    }
    g_dirty = 1;
}

/* ================= keys ================= */

enum {
    K_NONE, K_ENTER, K_NEWLINE, K_BS, K_DEL, K_LEFT, K_RIGHT, K_UP, K_DOWN, K_HOME, K_END, K_WLEFT, K_WRIGHT,
    K_WBS, K_WDEL, K_KILLEND, K_KILLSTART, K_PGUP, K_PGDN, K_SUP, K_SDOWN, K_TOP, K_BOTTOM, K_ESC, K_CTRLC,
    K_CTRLD, K_CTRLL, K_CTRLO, K_WHEELUP, K_WHEELDN, K_TAB
};

static void submit(void)
{
    if (!g_await) {
        notice("Botter is working · esc to interrupt");
        return;
    }
    const char *text = E.p ? E.p : "";
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "line", text);
    send_json(o);
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "line");
    if (cJSON_IsString(v)) {
        explicit_bzero(v->valuestring, strlen(v->valuestring));
    }
    cJSON_Delete(o);

    if (g_chat) {
        if (text[strspn(text, " \n\t")]) {
            blk_text(B_USER, text);
            hist_add(text);
        }
    } else {
        blk_t *b = blk_add(B_ASK);
        tsb_str(&b->text, g_label);
        b->state = g_secret;
        b->extra = g_secret ? NULL : strdup(text);
    }
    g_raw_open = 0;
    free(g_draft);
    g_draft = NULL;
    g_hidx = -1;
    ed_clear();
    g_await = 0;
    g_scroll = 0;
}

static void scroll_by(int d)
{
    g_scroll += d;
    if (g_scroll < 0) {
        g_scroll = 0;
    }
}

static void do_key(int k)
{
    vl_t *v;
    int n = ed_lines(&v, input_tw()), r, c;
    ed_cursor(v, n, &r, &c);
    switch (k) {
    case K_ENTER:
        if (Epos == E.len && E.len > 0 && E.p[E.len - 1] == '\\' && !g_secret) {
            E.p[--E.len] = '\0'; /* "\" + enter = newline */
            Epos = E.len;
            ed_insert("\n", 1);
        } else {
            submit();
        }
        break;
    case K_NEWLINE:
        if (!g_secret) {
            ed_insert("\n", 1);
        }
        break;
    case K_TAB:
        ed_insert("    ", 4);
        break;
    case K_BS:
        if (Epos > 0) {
            ed_delete(ed_prev(Epos), Epos);
        }
        break;
    case K_DEL:
        if (Epos < E.len) {
            ed_delete(Epos, ed_next(Epos));
        }
        break;
    case K_LEFT:
        Epos = ed_prev(Epos);
        break;
    case K_RIGHT:
        Epos = ed_next(Epos);
        break;
    case K_WLEFT:
        Epos = ed_word_left(Epos);
        break;
    case K_WRIGHT:
        Epos = ed_word_right(Epos);
        break;
    case K_WBS:
        ed_delete(ed_word_left(Epos), Epos);
        break;
    case K_WDEL:
        ed_delete(Epos, ed_word_right(Epos));
        break;
    case K_HOME:
        Epos = v[r].a;
        break;
    case K_END:
        Epos = v[r].b;
        break;
    case K_KILLEND: {
        size_t e = v[r].b;
        ed_delete(Epos, e == Epos && e < E.len ? e + 1 : e);
        break;
    }
    case K_KILLSTART:
        ed_delete(v[r].a, Epos);
        break;
    case K_UP:
        if (r == 0) {
            hist_move(-1);
        } else {
            Epos = pos_at_col(v, r - 1, c);
        }
        break;
    case K_DOWN:
        if (r == n - 1) {
            hist_move(1);
        } else {
            Epos = pos_at_col(v, r + 1, c);
        }
        break;
    case K_PGUP:
        scroll_by(g_tH > 3 ? g_tH - 2 : 1);
        break;
    case K_PGDN:
        scroll_by(-(g_tH > 3 ? g_tH - 2 : 1));
        break;
    case K_SUP:
        scroll_by(1);
        break;
    case K_SDOWN:
        scroll_by(-1);
        break;
    case K_WHEELUP:
        scroll_by(3);
        break;
    case K_WHEELDN:
        scroll_by(-3);
        break;
    case K_TOP:
        g_scroll = 1 << 30;
        break;
    case K_BOTTOM:
        g_scroll = 0;
        break;
    case K_ESC:
        if (g_busy && !g_await) {
            interrupt_child();
        } else if (g_scroll) {
            g_scroll = 0;
        }
        break;
    case K_CTRLC:
        if (g_await) {
            if (g_chat && E.len) {
                ed_clear();
            } else {
                send_flag("intr");
                ed_clear();
            }
        } else if (g_busy) {
            interrupt_child();
        }
        break;
    case K_CTRLD:
        if (g_await && E.len == 0) {
            send_flag("eof");
        } else if (Epos < E.len) {
            ed_delete(Epos, ed_next(Epos));
        }
        break;
    case K_CTRLL:
        g_full = 1;
        break;
    case K_CTRLO:
        g_expand = !g_expand;
        notice(g_expand ? "showing full thinking and tool output" : "details collapsed");
        break;
    }
    g_dirty = 1;
}

/* Paste: CR/CRLF -> LF, tabs -> spaces, other control chars dropped. */
static int    g_pasting;
static void paste_text(const char *s, size_t n)
{
    tsb_t t = {0};
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\r') {
            tsb_add(&t, "\n", 1);
            if (i + 1 < n && s[i + 1] == '\n') {
                i++;
            }
        } else if (ch == '\t') {
            tsb_add(&t, "    ", 4);
        } else if (ch == '\n' || ch >= 0x20) {
            if (ch != 0x7f && !(g_secret && ch == '\n')) {
                tsb_add(&t, (const char *)&s[i], 1);
            }
        }
    }
    if (t.len) {
        ed_insert(t.p, t.len);
    }
    if (t.p) {
        explicit_bzero(t.p, t.len);
    }
    tsb_free(&t);
    g_dirty = 1;
}

static int csi_mod(const int *p, int np)
{
    return np >= 2 ? p[1] : 1; /* 2 shift, 3 alt, 5 ctrl */
}

/* Decode one key from s[0..n). Returns bytes consumed, 0 = incomplete (wait for more). */
static size_t parse_input(const unsigned char *s, size_t n, int final)
{
    if (g_pasting) {
        static const char END[] = "\033[201~";
        const unsigned char *e = memmem(s, n, END, 6);
        if (e) {
            paste_text((const char *)s, (size_t)(e - s));
            g_pasting = 0;
            return (size_t)(e - s) + 6;
        }
        if (n > 6) {
            paste_text((const char *)s, n - 5);
            return n - 5;
        }
        return final ? n : 0;
    }
    unsigned char ch = s[0];
    if (ch == 0x1b) {
        if (n == 1) {
            if (final) {
                do_key(K_ESC);
                return 1;
            }
            return 0;
        }
        if (s[1] == '[') {
            size_t i = 2;
            while (i < n && s[i] >= 0x20 && s[i] <= 0x3F) {
                i++;
            }
            while (i < n && s[i] >= 0x20 && s[i] <= 0x2F) {
                i++;
            }
            if (i >= n) {
                if (final) {
                    do_key(K_ESC);
                    return 1;
                }
                return 0;
            }
            unsigned char fin = s[i];
            size_t len = i + 1;
            int mouse = s[2] == '<';
            int p[8] = {0}, np = 0;
            for (size_t k = mouse ? 3 : 2; k < i && np < 8;) {
                int val = 0, any = 0;
                while (k < i && isdigit(s[k])) {
                    val = val * 10 + (s[k++] - '0');
                    any = 1;
                }
                p[np++] = any ? val : 0;
                if (k < i && (s[k] == ';' || s[k] == ':')) {
                    k++;
                } else {
                    break;
                }
            }
            if (mouse) {
                if (fin == 'M' && np >= 1) {
                    if (p[0] == 64) {
                        do_key(K_WHEELUP);
                    } else if (p[0] == 65) {
                        do_key(K_WHEELDN);
                    }
                }
                return len;
            }
            int m = csi_mod(p, np);
            switch (fin) {
            case 'A':
                do_key(m == 2 ? K_SUP : K_UP);
                break;
            case 'B':
                do_key(m == 2 ? K_SDOWN : K_DOWN);
                break;
            case 'C':
                do_key(m == 5 || m == 3 ? K_WRIGHT : K_RIGHT);
                break;
            case 'D':
                do_key(m == 5 || m == 3 ? K_WLEFT : K_LEFT);
                break;
            case 'H':
                do_key(m == 5 ? K_TOP : K_HOME);
                break;
            case 'F':
                do_key(m == 5 ? K_BOTTOM : K_END);
                break;
            case 'u': /* kitty keyboard protocol: code;mods */
                if (p[0] == 13 && m > 1) {
                    do_key(K_NEWLINE);
                } else if (p[0] == 13) {
                    do_key(K_ENTER);
                } else if (p[0] == 27) {
                    do_key(K_ESC);
                }
                break;
            case '~':
                if (p[0] == 200) {
                    g_pasting = 1;
                } else if (p[0] == 27 && np >= 3 && p[2] == 13) { /* xterm modifyOtherKeys */
                    do_key(p[1] > 1 ? K_NEWLINE : K_ENTER);
                } else if (p[0] == 1 || p[0] == 7) {
                    do_key(m == 5 ? K_TOP : K_HOME);
                } else if (p[0] == 4 || p[0] == 8) {
                    do_key(m == 5 ? K_BOTTOM : K_END);
                } else if (p[0] == 3) {
                    do_key(m == 5 ? K_WDEL : K_DEL);
                } else if (p[0] == 5) {
                    do_key(K_PGUP);
                } else if (p[0] == 6) {
                    do_key(K_PGDN);
                }
                break;
            }
            return len;
        }
        if (s[1] == 'O') {
            if (n < 3) {
                return final ? n : 0;
            }
            static const char keys[] = "ABCDHF";
            static const int map[] = {K_UP, K_DOWN, K_RIGHT, K_LEFT, K_HOME, K_END};
            const char *f = strchr(keys, s[2]);
            if (f && s[2]) {
                do_key(map[f - keys]);
            }
            return 3;
        }
        /* Alt + key */
        if (s[1] == '\r' || s[1] == '\n') {
            do_key(K_NEWLINE);
        } else if (s[1] == 0x7f || s[1] == 8) {
            do_key(K_WBS);
        } else if (s[1] == 'b') {
            do_key(K_WLEFT);
        } else if (s[1] == 'f') {
            do_key(K_WRIGHT);
        } else if (s[1] == 'd') {
            do_key(K_WDEL);
        } else if (s[1] == 0x1b) {
            do_key(K_ESC);
            return 1;
        }
        return 2;
    }
    if (ch < 0x20 || ch == 0x7f) {
        static const int ctl[32] = {
            [1] = K_HOME,    [2] = K_LEFT,   [3] = K_CTRLC, [4] = K_CTRLD,  [5] = K_END,      [6] = K_RIGHT,
            [8] = K_BS,      [9] = K_TAB,    [10] = K_NEWLINE, [11] = K_KILLEND, [12] = K_CTRLL, [13] = K_ENTER,
            [14] = K_DOWN,   [15] = K_CTRLO, [16] = K_UP,   [21] = K_KILLSTART, [23] = K_WBS,
        };
        do_key(ch == 0x7f ? K_BS : ctl[ch]);
        return 1;
    }
    if (ch >= 0x80) {
        size_t need = (ch >> 5) == 6 ? 2 : (ch >> 4) == 14 ? 3 : (ch >> 3) == 30 ? 4 : 1;
        if (n < need) {
            return final ? n : 0;
        }
        if (need > 1) {
            ed_insert((const char *)s, need);
        }
        g_dirty = 1;
        return need;
    }
    size_t k = 0; /* run of printable ASCII */
    while (k < n && s[k] >= 0x20 && s[k] < 0x7f) {
        k++;
    }
    ed_insert((const char *)s, k);
    g_dirty = 1;
    return k;
}

/* ================= payload / child process ================= */

static int copy_range(int dst, int src, off_t off, uint64_t len)
{
    char buf[1 << 16];
    while (len > 0) {
        size_t want = len < sizeof(buf) ? (size_t)len : sizeof(buf);
        ssize_t r = pread(src, buf, want, off);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return -1;
        }
        for (ssize_t o = 0; o < r;) {
            ssize_t w = write(dst, buf + o, (size_t)(r - o));
            if (w < 0 && errno == EINTR) {
                continue;
            }
            if (w <= 0) {
                return -1;
            }
            o += w;
        }
        off += r;
        len -= (uint64_t)r;
    }
    return 0;
}

static uint64_t get64(const unsigned char *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

static void put64(unsigned char *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (unsigned char)(v >> (8 * i));
    }
}

/* Where the embedded agent lives in our own executable: 0 ok. */
static int find_payload(int self, uint64_t *off, uint64_t *len, uint64_t *self_len)
{
    struct stat st;
    unsigned char t[TRAILER_LEN];
    if (fstat(self, &st) != 0) {
        return -1;
    }
    *self_len = (uint64_t)st.st_size;
    if (st.st_size < TRAILER_LEN || pread(self, t, TRAILER_LEN, st.st_size - TRAILER_LEN) != TRAILER_LEN ||
        memcmp(t + 16, TRAILER_MAGIC, 8) != 0) {
        return -1;
    }
    *off = get64(t);
    *len = get64(t + 8);
    if (*off + *len + TRAILER_LEN != (uint64_t)st.st_size) {
        return -1;
    }
    *self_len = *off;
    return 0;
}

/* `botter-tui --bundle <agent.bot> <out>`: this program + the agent + trailer. */
static int bundle(const char *agent, const char *out)
{
    int self = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    int ag = open(agent, O_RDONLY | O_CLOEXEC);
    uint64_t off, len, self_len;
    struct stat st;
    if (self < 0 || ag < 0 || fstat(ag, &st) != 0) {
        fprintf(stderr, "bundle: cannot open %s\n", self < 0 ? "/proc/self/exe" : agent);
        return 1;
    }
    find_payload(self, &off, &len, &self_len); /* self_len excludes any payload already attached */
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp%d", out, (int)getpid());
    int o = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    unsigned char t[TRAILER_LEN];
    put64(t, self_len);
    put64(t + 8, (uint64_t)st.st_size);
    memcpy(t + 16, TRAILER_MAGIC, 8);
    if (o < 0 || copy_range(o, self, 0, self_len) != 0 || copy_range(o, ag, 0, (uint64_t)st.st_size) != 0 ||
        write(o, t, TRAILER_LEN) != TRAILER_LEN || fchmod(o, 0755) != 0 || close(o) != 0 || rename(tmp, out) != 0) {
        fprintf(stderr, "bundle: writing %s failed: %s\n", out, strerror(errno));
        unlink(tmp);
        return 1;
    }
    return 0;
}

/* The agent as an executable memfd. */
static int load_agent(int argc, char **argv)
{
    int src;
    uint64_t off = 0, len = 0, self_len;
    if (argc > 1) {
        src = open(argv[1], O_RDONLY | O_CLOEXEC);
        struct stat st;
        if (src < 0 || fstat(src, &st) != 0) {
            fprintf(stderr, "botter: cannot open %s: %s\n", argv[1], strerror(errno));
            return -1;
        }
        len = (uint64_t)st.st_size;
    } else {
        src = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
        if (src < 0 || find_payload(src, &off, &len, &self_len) != 0) {
            fprintf(stderr, "botter: no agent embedded (build with `make`, or run: %s <agent.bot>)\n", argv[0]);
            return -1;
        }
    }
    int fd = memfd_create("botter", MFD_CLOEXEC);
    if (fd < 0 || copy_range(fd, src, (off_t)off, len) != 0) {
        fprintf(stderr, "botter: cannot load the agent: %s\n", strerror(errno));
        return -1;
    }
    close(src);
    fchmod(fd, 0700);
    return fd;
}

static int spawn_agent(int fd)
{
    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC) != 0 || pipe2(out, O_CLOEXEC) != 0) {
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        setenv("BOTCORE_FRONTEND", "1", 1);
        char *av[] = {"botter", NULL};
        fexecve(fd, av, environ);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    g_child = pid;
    g_cin = in[1];
    g_cout = out[0];
    fcntl(g_cout, F_SETFL, fcntl(g_cout, F_GETFL) | O_NONBLOCK);
    return 0;
}

/* After leaving the alternate screen: the conversation goes to normal scrollback. */
static void dump_transcript(void)
{
    int cw = g_W - 2 > 20 ? g_W - 2 : 20;
    layout_all(cw);
    tsb_t o = {0};
    for (int i = 0; i < g_nb; i++) {
        for (int y = 0; y < g_b[i].L.n; y++) {
            const tline_t *l = &g_b[i].L.l[y];
            tsb_str(&o, " ");
            for (int k = 0; k < l->n; k++) {
                tsty_t s = l->r[k].s;
                sgr(&o, s, l->fill == TC_NONE ? TC_NONE : l->fill);
                tsb_str(&o, l->r[k].t);
            }
            tsb_str(&o, "\033[0m\n");
        }
    }
    if (o.len) {
        (void)!write(g_tty, o.p, o.len);
    }
    tsb_free(&o);
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--bundle") == 0) {
        return bundle(argv[2], argv[3]);
    }
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("usage: %s [agent.bot]        run (default: the embedded agent)\n"
               "       %s --bundle <agent.bot> <out>\n"
               "BOTTER_TUI=0 runs the plain REPL.\n",
               argv[0], argv[0]);
        return 0;
    }
    int fd = load_agent(argc, argv);
    if (fd < 0) {
        return 1;
    }
    const char *term = getenv("TERM"), *want = getenv("BOTTER_TUI");
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || (term && strcmp(term, "dumb") == 0) ||
        (want && strcmp(want, "0") == 0)) {
        char *av[] = {"botter", NULL};
        fexecve(fd, av, environ);
        fprintf(stderr, "botter: cannot start the agent: %s\n", strerror(errno));
        return 1;
    }
    const char *ct = getenv("COLORTERM");
    g_truecolor = ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"));
    if (!g_truecolor && term && (strstr(term, "kitty") || strstr(term, "ghostty") || strstr(term, "wezterm") ||
                                 strstr(term, "alacritty") || strstr(term, "foot") || strstr(term, "direct"))) {
        g_truecolor = 1;
    }
    g_tty = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    if (pipe2(g_sigpipe, O_CLOEXEC | O_NONBLOCK) != 0 || spawn_agent(fd) != 0) {
        fprintf(stderr, "botter: cannot start the agent: %s\n", strerror(errno));
        return 1;
    }
    close(fd);
    if (tty_setup() != 0) {
        fprintf(stderr, "botter: cannot set up the terminal\n");
        kill(g_child, SIGTERM);
        return 1;
    }
    theme_init(tty_is_light());
    blk_add(B_HEADER);

    unsigned char ib[8192];
    size_t ibn = 0;
    double esc_deadline = 0;
    int child_open = 1;
    while (child_open && !g_quit) {
        if (g_winch) {
            g_winch = 0;
            tty_size();
            g_full = 1;
            g_dirty = 1;
        }
        double t = now_s();
        if (g_notice[0] && t >= g_notice_until) {
            g_notice[0] = '\0';
            g_dirty = 1;
        }
        if (g_busy && !g_await) {
            g_dirty = 1; /* spinner */
        }
        if (g_dirty || g_full) {
            render();
        }
        int timeout = -1;
        if (g_busy && !g_await) {
            timeout = 100;
        }
        if (g_notice[0]) {
            int ms = (int)((g_notice_until - t) * 1000) + 10;
            timeout = timeout < 0 || ms < timeout ? ms : timeout;
        }
        if (ibn > 0) {
            int ms = (int)((esc_deadline - t) * 1000);
            timeout = ms < 0 ? 0 : (timeout < 0 || ms < timeout ? ms : timeout);
        }
        struct pollfd p[3] = {{STDIN_FILENO, POLLIN, 0}, {g_cout, POLLIN, 0}, {g_sigpipe[0], POLLIN, 0}};
        int pr = poll(p, 3, timeout);
        if (pr < 0 && errno != EINTR) {
            break;
        }
        if (pr > 0 && (p[2].revents & POLLIN)) {
            char junk[64];
            while (read(g_sigpipe[0], junk, sizeof(junk)) > 0) {
            }
        }
        if (pr > 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[65536];
            for (;;) {
                ssize_t r = read(g_cout, buf, sizeof(buf));
                if (r > 0) {
                    tsb_add(&g_rx, buf, (size_t)r);
                    continue;
                }
                if (r == 0) {
                    child_open = 0;
                } else if (errno == EINTR) {
                    continue;
                }
                break;
            }
            process_rx();
        }
        if (pr > 0 && (p[0].revents & POLLIN)) {
            ssize_t r = read(STDIN_FILENO, ib + ibn, sizeof(ib) - ibn);
            if (r == 0) {
                break; /* terminal gone */
            }
            if (r > 0) {
                ibn += (size_t)r;
                esc_deadline = now_s() + 0.03;
            }
        } else if (pr > 0 && (p[0].revents & (POLLHUP | POLLERR))) {
            break;
        }
        int final = ibn > 0 && now_s() >= esc_deadline;
        size_t used = 0;
        while (used < ibn) {
            size_t k = parse_input(ib + used, ibn - used, final);
            if (k == 0) {
                break;
            }
            used += k;
        }
        memmove(ib, ib + used, ibn - used);
        ibn -= used;
        if (ibn == sizeof(ib)) {
            ibn = 0; /* garbage: drop */
        }
    }

    int status = 0;
    if (child_open || g_quit) {
        kill(g_child, SIGTERM);
    }
    waitpid(g_child, &status, 0);
    tty_restore();
    dump_transcript();
    if (E.p) {
        explicit_bzero(E.p, E.cap);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
