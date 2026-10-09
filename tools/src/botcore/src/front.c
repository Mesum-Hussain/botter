#define _GNU_SOURCE
#include "front.h"
#include "term.h"
#include "cJSON.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_on;

/* stdin is read through our own buffer (poll + read; no stdio buffering). */
static char  *g_rb;
static size_t g_rlen, g_rcap;

int front_init(void)
{
    const char *v = getenv("BOTCORE_FRONTEND");
    if (!v || strcmp(v, "1") != 0) {
        return 0;
    }
    unsetenv("BOTCORE_FRONTEND");
    dup2(STDOUT_FILENO, STDERR_FILENO); /* one ordered stream for the UI */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    g_on = 1;
    return 1;
}

int front_active(void) { return g_on; }

static void emit(cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    if (s) {
        fputc('\x1e', stdout);
        fputs(s, stdout);
        fputc('\n', stdout);
        free(s);
    }
}

void front_event(const char *ev, ...)
{
    if (!g_on) {
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ev", ev);
    va_list ap;
    va_start(ap, ev);
    for (;;) {
        const char *k = va_arg(ap, const char *);
        if (!k) {
            break;
        }
        const char *v = va_arg(ap, const char *);
        cJSON_AddStringToObject(o, k, v ? v : "");
    }
    va_end(ap);
    emit(o);
    cJSON_Delete(o);
}

/* Next '\n'-terminated line from stdin -> malloc'd (no newline). 0 ok, 1 timeout, -1 EOF, -2 EINTR. */
static int read_line(int timeout_ms, char **line)
{
    for (;;) {
        char *nl = g_rlen ? memchr(g_rb, '\n', g_rlen) : NULL;
        if (nl) {
            size_t n = (size_t)(nl - g_rb);
            *line = strndup(g_rb, n);
            explicit_bzero(g_rb, n + 1); /* may hold a secret */
            memmove(g_rb, nl + 1, g_rlen - n - 1);
            g_rlen -= n + 1;
            return *line ? 0 : -1;
        }
        struct pollfd p = {STDIN_FILENO, POLLIN, 0};
        int r = poll(&p, 1, timeout_ms);
        if (r < 0) {
            return errno == EINTR ? -2 : -1;
        }
        if (r == 0) {
            return 1;
        }
        if (g_rcap - g_rlen < 4096) {
            size_t nc = g_rcap ? g_rcap * 2 : 8192;
            char *nb = malloc(nc);
            if (!nb) {
                return -1;
            }
            if (g_rb) {
                memcpy(nb, g_rb, g_rlen);
                explicit_bzero(g_rb, g_rcap);
                free(g_rb);
            }
            g_rb = nb;
            g_rcap = nc;
        }
        ssize_t n = read(STDIN_FILENO, g_rb + g_rlen, g_rcap - g_rlen);
        if (n < 0 && errno == EINTR) {
            return -2;
        }
        if (n <= 0) {
            return -1;
        }
        g_rlen += (size_t)n;
    }
}

int front_readline(const char *prompt, int secret, int (*idle)(void), char **out)
{
    *out = NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ev", "prompt");
    cJSON_AddStringToObject(o, "text", prompt);
    cJSON_AddBoolToObject(o, "secret", secret);
    emit(o);
    cJSON_Delete(o);

    for (;;) {
        char *line = NULL;
        int r = read_line(idle ? 1000 : -1, &line);
        if (r == 1) {
            if (idle && idle()) {
                return TERM_TICK;
            }
            continue;
        }
        if (r == -2) {
            return TERM_INTR;
        }
        if (r < 0) {
            return TERM_EOF;
        }
        cJSON *j = cJSON_Parse(line);
        explicit_bzero(line, strlen(line));
        free(line);
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "line");
        int rc = TERM_EOF;
        if (cJSON_IsString(v)) {
            *out = strdup(v->valuestring);
            explicit_bzero(v->valuestring, strlen(v->valuestring));
            rc = *out ? TERM_LINE : TERM_EOF;
        } else if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "intr"))) {
            rc = TERM_INTR;
        } else if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "eof"))) {
            cJSON_Delete(j);
            continue; /* unknown message: ignore */
        }
        cJSON_Delete(j);
        return rc;
    }
}
