#ifndef BC_SEMVER_H
#define BC_SEMVER_H

/*
 * Semantic versions (MAJOR.MINOR.PATCH[-pre][+build]) and ranges as in
 * agent.json "engines": "1.2.3", ">=0.1.0", ">0.1", "<2.0.0", "<=1.4", "^1.2.0"
 * (same major), "~1.2.0" (same major.minor), "*"; several separated by spaces
 * must all hold. Header-only (shared by botcore and botter_pack).
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Parse "1.2.3" (missing minor/patch = 0 when partial is set). Returns chars used, 0 on error. */
static inline int sv_parse(const char *s, long v[3], int partial)
{
    const char *p = s;
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*p) || (p[0] == '0' && isdigit((unsigned char)p[1]))) {
            if (partial && i > 0) {
                for (; i < 3; i++) {
                    v[i] = 0;
                }
                return (int)(p - s - 1);
            }
            return 0;
        }
        v[i] = strtol(p, (char **)&p, 10);
        if (i < 2) {
            if (*p != '.') {
                if (!partial) {
                    return 0;
                }
                for (int k = i + 1; k < 3; k++) {
                    v[k] = 0;
                }
                break;
            }
            p++;
        }
    }
    if (*p == '-' || *p == '+') { /* pre-release / build: accepted, ignored when comparing */
        p++;
        while (isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == '+') {
            p++;
        }
    }
    return (int)(p - s);
}

static inline int sv_valid(const char *s)
{
    long v[3];
    int n = sv_parse(s, v, 0);
    return n > 0 && s[n] == '\0';
}

static inline int sv_cmp(const long a[3], const long b[3])
{
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}

/* 1 if version satisfies range, 0 if not, -1 if the range is malformed. */
static inline int sv_satisfies(const char *version, const char *range)
{
    long have[3];
    int ok = 1;
    if (sv_parse(version, have, 0) <= 0) {
        return -1;
    }
    const char *p = range;
    while (*p) {
        while (*p == ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        if (*p == '*') {
            p++;
            continue;
        }
        char op[3] = "";
        int k = 0;
        while (k < 2 && strchr("<>=^~", *p)) {
            op[k++] = *p++;
        }
        op[k] = '\0';
        long want[3];
        int n = sv_parse(p, want, 1);
        if (n <= 0) {
            return -1;
        }
        p += n;
        int c = sv_cmp(have, want);
        if (!op[0] || !strcmp(op, "=")) {
            ok &= c == 0;
        } else if (!strcmp(op, ">=")) {
            ok &= c >= 0;
        } else if (!strcmp(op, ">")) {
            ok &= c > 0;
        } else if (!strcmp(op, "<=")) {
            ok &= c <= 0;
        } else if (!strcmp(op, "<")) {
            ok &= c < 0;
        } else if (!strcmp(op, "^")) {
            ok &= c >= 0 && have[0] == want[0] && (want[0] != 0 || have[1] == want[1]);
        } else if (!strcmp(op, "~")) {
            ok &= c >= 0 && have[0] == want[0] && have[1] == want[1];
        } else {
            return -1;
        }
        if (*p && *p != ' ') {
            return -1;
        }
    }
    return ok;
}

#endif
