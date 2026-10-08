#define _GNU_SOURCE
#include "tools.h"
#include "vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const tool_t *t;
    size_t        n;
} table_t;

static table_t tables(int i)
{
    switch (i) {
    case 0: return (table_t){TOOLS_FS, TOOLS_FS_N};
    case 1: return (table_t){TOOLS_SHELL, TOOLS_SHELL_N};
    case 2: return (table_t){TOOLS_CRON, TOOLS_CRON_N};
    case 3: return (table_t){TOOLS_VFS, vfs_count() ? TOOLS_VFS_N : 0};
    default: return (table_t){NULL, 0};
    }
}

static const tool_t *find_tool(const char *name)
{
    for (int i = 0; tables(i).t; i++) {
        table_t tb = tables(i);
        for (size_t j = 0; j < tb.n; j++) {
            if (strcmp(tb.t[j].name, name) == 0) {
                return &tb.t[j];
            }
        }
    }
    return NULL;
}

const tool_t *tools_find_builtin(const char *name)
{
    return find_tool(name);
}

cJSON *tools_schema(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; tables(i).t; i++) {
        table_t tb = tables(i);
        for (size_t j = 0; j < tb.n; j++) {
            cJSON *params = cJSON_Parse(tb.t[j].schema);
            if (!params) {
                fprintf(stderr, "internal: bad schema for tool %s\n", tb.t[j].name);
                continue;
            }
            cJSON *fn = cJSON_CreateObject();
            cJSON_AddStringToObject(fn, "name", tb.t[j].name);
            cJSON_AddStringToObject(fn, "description", tb.t[j].description);
            cJSON_AddItemToObject(fn, "parameters", params);
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "type", "function");
            cJSON_AddItemToObject(e, "function", fn);
            cJSON_AddItemToArray(arr, e);
        }
    }
    ext_schema_append(arr);
    return arr;
}

void tools_sanitize_utf8(char *s)
{
    unsigned char *p = (unsigned char *)s;
    while (*p) {
        unsigned char c = *p;
        int len = 0;
        if (c < 0x80) {
            if (c < 0x20 && c != '\n' && c != '\t') {
                *p = '?';
            } else if (c == 0x7f) {
                *p = '?';
            }
            p++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
        }
        int ok = len > 0;
        for (int i = 1; ok && i < len; i++) {
            ok = (p[i] & 0xC0) == 0x80;
        }
        if (ok && len == 3 && ((c == 0xE0 && p[1] < 0xA0) || (c == 0xED && p[1] >= 0xA0))) {
            ok = 0; /* overlong / surrogate */
        }
        if (ok && len == 4 && ((c == 0xF0 && p[1] < 0x90) || (c == 0xF4 && p[1] >= 0x90))) {
            ok = 0;
        }
        if (ok) {
            p += len;
        } else {
            *p++ = '?';
        }
    }
}

char *tools_call(const char *name, const char *args_json)
{
    char *res = calloc(1, TOOL_RESULT_MAX);
    if (!res) {
        return NULL;
    }
    const tool_t *t = find_tool(name);
    const ext_tool_t *x = t ? NULL : ext_find(name);
    if (!t && !x) {
        snprintf(res, TOOL_RESULT_MAX, "ERROR: unknown tool '%.100s'", name);
        goto out;
    }
    cJSON *in = cJSON_Parse(args_json && *args_json ? args_json : "{}");
    if (!cJSON_IsObject(in)) {
        cJSON_Delete(in);
        snprintf(res, TOOL_RESULT_MAX, "ERROR: arguments must be a JSON object");
        goto out;
    }
    /* Handlers write at res + 7 so a failure can be marked without a copy. */
    static const char ERR[] = "ERROR: ";
    bool ok = t ? t->fn(in, res + sizeof(ERR) - 1, TOOL_RESULT_MAX - (sizeof(ERR) - 1))
                : ext_run(x, in, res + sizeof(ERR) - 1, TOOL_RESULT_MAX - (sizeof(ERR) - 1));
    cJSON_Delete(in);
    if (ok) {
        memmove(res, res + sizeof(ERR) - 1, strlen(res + sizeof(ERR) - 1) + 1);
    } else {
        memcpy(res, ERR, sizeof(ERR) - 1);
    }
out:
    res[TOOL_RESULT_MAX - 1] = '\0';
    tools_sanitize_utf8(res);
    return res;
}
