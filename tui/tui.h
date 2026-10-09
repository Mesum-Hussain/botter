#ifndef BOTTER_TUI_H
#define BOTTER_TUI_H

/* Shared by main.c (process, input, screen) and lay.c (text layout). */

#include <stddef.h>
#include <stdint.h>

#define TA_BOLD   1
#define TA_ITALIC 2
#define TA_UNDER  4
#define TA_STRIKE 8

#define TC_NONE 0xFF000000u /* no colour of its own: line fill / screen background */

typedef struct {
    uint32_t fg, bg; /* 0xRRGGBB; bg may be TC_NONE */
    uint8_t  at;
} tsty_t;

typedef struct {
    tsty_t s;
    char  *t; /* UTF-8, no control chars */
    int    w; /* display columns */
} trun_t;

typedef struct {
    trun_t  *r;
    int      n, cap;
    int      w;    /* columns used */
    uint32_t fill; /* bg of the rest of the row (TC_NONE = screen bg) */
} tline_t;

typedef struct {
    tline_t *l;
    int      n, cap;
} tlines_t;

/* Sage palette (colorhunt 5C7057 89A482 ACC5A6 D1EDD3) on the terminal's own background:
 * bg is TC_NONE, so nothing is painted behind text; theme_init picks the light or dark set. */
typedef struct {
    uint32_t bg, surface, muted, text;
    uint32_t accent, err, ok, info, purple; /* purple: secondary accent (thinking, model name) */
    uint32_t code_bg, user_bg, kw, str, num, com, fn;
    uint32_t in_label, out_label;           /* notebook "In [n]:" / "Out[n]:" */
} ttheme_t;

extern ttheme_t T;
void theme_init(int light);

typedef struct {
    char  *p;
    size_t len, cap;
} tsb_t;

void tsb_add(tsb_t *b, const char *s, size_t n);
void tsb_str(tsb_t *b, const char *s);
void tsb_fmt(tsb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void tsb_free(tsb_t *b);

/* UTF-8 */
uint32_t tu_decode(const char *s, size_t n, int *len); /* invalid -> U+FFFD, 1 byte */
int      tu_width(uint32_t cp);                         /* 0, 1 or 2 columns */
int      tu_strwidth(const char *s, size_t n);

tsty_t sty(uint32_t fg, uint8_t at);
tsty_t sty_bg(uint32_t fg, uint32_t bg, uint8_t at);
int    sty_eq(tsty_t a, tsty_t b);

/* Lines */
void     tl_free(tlines_t *L);
tline_t *tl_new(tlines_t *L, uint32_t fill);
void     tl_put(tline_t *l, tsty_t s, const char *t, size_t n);   /* text without newlines */
void     tl_put_trunc(tline_t *l, tsty_t s, const char *t, int maxw); /* cut with an ellipsis */
void     tl_pad(tline_t *l, int w, tsty_t s);                     /* spaces up to w columns */
void     tl_cat(tline_t *dst, const tline_t *src);                 /* copy runs */
void     tl_blank(tlines_t *L);
/* Prepend `first` to line `from`, `rest` to lines after it. */
void     tl_prefix(tlines_t *L, int from, tsty_t s1, const char *first, tsty_t s2, const char *rest);
void     tl_move(tlines_t *dst, tlines_t *src); /* append src's lines to dst, empties src */

/* Styled text -> wrapped lines */
typedef struct tcells tcells_t;
tcells_t *tc_new(void);
void      tc_add(tcells_t *c, const char *s, size_t n, tsty_t st); /* '\n' = hard break */
void      tc_flow(tcells_t *c, tlines_t *L, int width, uint32_t fill, int word);
int       tc_width(const tcells_t *c);                             /* widest hard line */
void      tc_free(tcells_t *c);

void lay_text(tlines_t *L, const char *text, size_t n, tsty_t s, int width, uint32_t fill);
void lay_markdown(tlines_t *L, const char *md, int width);
void lay_ansi(tlines_t *L, const char *text, size_t n, int width); /* SGR-coloured output */
void lay_logo(tlines_t *L);                                         /* tui logo, half blocks */
int  logo_width(void);

#endif
