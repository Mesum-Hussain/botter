#ifndef BC_SQNC_H
#define BC_SQNC_H

#include <stddef.h>

/*
 * Sqnc ("sequence"): the language of an agent's SQNC.md. The whole file is
 * one ```sqnc block: a frontmatter (spec-version: "sqnc-1"), then UPPERCASE
 * statements; every other line is a plain-English instruction (no Markdown). This file is the shared front end:
 * sq_parse() builds the syntax tree (syntax errors), sq_check() checks names
 * and variables (botter_pack, at build time), sqnc_run.c interprets the tree
 * (botcore, at run time). No dependencies beyond libc.
 */

enum {
    SQ_INSTR = 1, /* plain-English instruction            a = text */
    SQ_STEP,      /* STEP n: NAME label                   a = NAME, n = n */
    SQ_LOAD,      /* LOAD SKILL "a"                       */
    SQ_EXEC,      /* EXECUTE tool `a` <b>                 b = rest (payload / prose), pk/pv = "- k: v" lines */
    SQ_INVOKE,    /* INVOKE SKILL "a" [USING <b>]          b = rest after the name (may start with USING) */
    SQ_ASK,       /* ASK USER <a>                         */
    SQ_SAVE_VAR,  /* SAVE <a> INTO VARIABLE `b`           */
    SQ_SAVE_FILE, /* SAVE <a> TO FILE "b"                 */
    SQ_SET,       /* SET `a` TO <b>                       */
    SQ_IF,        /* IF/ELSE IF/ELSE ... END IF           br[] (cond NULL = ELSE) */
    SQ_FOR,       /* FOR EACH `a` IN <b> DO ... END FOR   */
    SQ_WHILE,     /* WHILE <a> [AT MOST n TIMES] DO ...   n = bound (0 = none) */
    SQ_RETRY,     /* RETRY UP TO n TIMES DO ... END RETRY */
    SQ_PAR,       /* IN PARALLEL DO ... END PARALLEL      */
    SQ_RETURN,    /* RETURN <a>                           */
};

typedef struct sq_node sq_node;

typedef struct {
    sq_node **v;
    int       n, cap;
} sq_list;

typedef struct {
    char   *cond; /* NULL = ELSE */
    int     line;
    sq_list body;
} sq_branch;

struct sq_node {
    int        kind, line;
    char      *a, *b;
    int        n;
    char     **pk, **pv; /* "with parameters:" lines */
    int       *pl;       /* their line numbers */
    int        np;
    sq_list    body;
    sq_branch *br;
    int        nbr;
};

typedef struct {
    sq_list top;
    char    title[200];
    int     errors, warnings;
    /* diagnostics: line, 1 = error / 0 = warning, message */
    void (*diag)(void *ud, int line, int error, const char *msg);
    void *ud;
} sq_prog;

/* Parse SQNC.md text. Syntax errors go to p->diag; returns the error count. */
int  sq_parse(const char *src, size_t n, sq_prog *p);
void sq_free(sq_prog *p);

/* Build-time checks on a parsed program: tools and skills exist, LOAD before INVOKE,
 * variables set before use, result/skill_output/answer after their producers. */
void sq_check(sq_prog *p, int (*has_tool)(const char *), int (*has_skill)(const char *));

/* `ref` syntax: identifier then .field / [n] parts; root name into root[65]. */
int  sq_ref(const char *s, size_t n, char root[65]);
/* Expression contains only "strings", `refs`, numbers, true/false/null, + and {..}/[..] JSON (no prose). */
int  sq_is_literal(const char *expr);

#endif
