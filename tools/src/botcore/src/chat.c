#define _GNU_SOURCE
#include "chat.h"
#include "http.h"
#include "term.h"

#include <stdio.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>

#define VALIDATE_TIMEOUT_S 20
#define CHAT_TIMEOUT_S     300

int chat_init(chat_t *c, const char *base, const char *key, const char *model)
{
    memset(c, 0, sizeof(*c));
    c->base = strdup(base);
    c->key = strdup(key ? key : "");
    if (c->key) {
        mlock(c->key, strlen(c->key) + 1); /* best effort: keep the key out of swap */
    }
    c->model = strdup(model);
    c->hist = cJSON_CreateArray();
    if (!c->base || !c->key || !c->model || !c->hist) {
        chat_free(c);
        return -1;
    }
    size_t n = strlen(c->base);
    while (n > 0 && c->base[n - 1] == '/') {
        c->base[--n] = '\0';
    }
    return 0;
}

void chat_free(chat_t *c)
{
    if (c->key) {
        size_t kl = strlen(c->key) + 1;
        explicit_bzero(c->key, kl);
        munlock(c->key, kl);
    }
    free(c->base);
    free(c->key);
    free(c->model);
    cJSON_Delete(c->hist);
    cJSON_Delete(c->tools);
    memset(c, 0, sizeof(*c));
}

/* Keep only {id, type, function{name, arguments}} in each stored tool call. */
static void strip_tool_calls(cJSON *hist)
{
    cJSON *m;
    cJSON_ArrayForEach(m, hist)
    {
        cJSON *calls = cJSON_GetObjectItemCaseSensitive(m, "tool_calls");
        cJSON *tc;
        cJSON_ArrayForEach(tc, calls)
        {
            for (cJSON *f = tc->child, *next; f; f = next) {
                next = f->next;
                if (strcmp(f->string, "id") != 0 && strcmp(f->string, "type") != 0 &&
                    strcmp(f->string, "function") != 0) {
                    cJSON_Delete(cJSON_DetachItemViaPointer(tc, f));
                }
            }
        }
    }
}

void chat_switch(chat_t *c, chat_t *from)
{
    if (strcmp(c->base, from->base) != 0) {
        strip_tool_calls(c->hist);
    }
    if (c->key) {
        size_t kl = strlen(c->key) + 1;
        explicit_bzero(c->key, kl);
        munlock(c->key, kl);
    }
    free(c->base);
    free(c->key);
    free(c->model);
    c->base = from->base;
    c->key = from->key;
    c->model = from->model;
    from->base = from->key = from->model = NULL;
    c->thoughts = c->ev_cb && strstr(c->base, "generativelanguage.googleapis.com") != NULL;
    chat_free(from);
}

static char *url_for(const chat_t *c, const char *path)
{
    size_t n = strlen(c->base) + strlen(path) + 1;
    char *u = malloc(n);
    if (u) {
        snprintf(u, n, "%s%s", c->base, path);
    }
    return u;
}

/* Pull a human-readable message out of an API error body. */
static char *error_text(int status, const char *body)
{
    char out[512];
    const char *msg = NULL;
    cJSON *root = cJSON_Parse(body);
    cJSON *e = root;

    if (cJSON_IsArray(e)) { /* Gemini wraps errors in a 1-element array */
        e = cJSON_GetArrayItem(e, 0);
    }
    e = cJSON_GetObjectItemCaseSensitive(e, "error");
    if (cJSON_IsString(e)) {
        msg = e->valuestring;
    } else if (cJSON_IsObject(e)) {
        cJSON *m = cJSON_GetObjectItemCaseSensitive(e, "message");
        if (cJSON_IsString(m)) {
            msg = m->valuestring;
        }
    }
    if (msg) {
        snprintf(out, sizeof(out), "HTTP %d: %.400s", status, msg);
    } else {
        snprintf(out, sizeof(out), "HTTP %d: %.400s", status,
                 body && *body ? body : "(empty response)");
    }
    cJSON_Delete(root);
    return strdup(out);
}

static int transport_error(int rc, const http_resp_t *r, char **err)
{
    if (rc == HTTP_ERR_ABORT) {
        return CHAT_ERR_ABORT;
    }
    if (err) {
        char m[600];
        snprintf(m, sizeof(m), "connection failed: %.500s", r->err ? r->err : "unknown error");
        *err = strdup(m);
    }
    return CHAT_ERR_OTHER;
}

int chat_validate(chat_t *c, char **err)
{
    http_resp_t r;
    int ret;
    char *url = url_for(c, "/models");

    *err = NULL;
    if (!url) {
        return CHAT_ERR_OTHER;
    }
    int rc = http_request(url, c->key, NULL, VALIDATE_TIMEOUT_S, &r);
    free(url);

    if (rc != HTTP_OK) {
        ret = transport_error(rc, &r, err);
    } else if (r.status >= 200 && r.status < 300) {
        ret = CHAT_OK;
    } else if (r.status == 404 || r.status == 405 || r.status == 501) {
        ret = CHAT_OK; /* provider has no /models: cannot verify, don't block */
    } else {
        *err = error_text(r.status, r.body);
        /* Gemini answers a bad key on /models with 400, not 401 */
        ret = (r.status == 400 || r.status == 401 || r.status == 403) ? CHAT_ERR_AUTH : CHAT_ERR_OTHER;
    }
    http_resp_free(&r);
    return ret;
}

/* content may be a string, or (some providers) an array of {type,text}. */
static char *extract_content(const cJSON *content)
{
    if (cJSON_IsString(content)) {
        return strdup(content->valuestring);
    }
    if (cJSON_IsArray(content)) {
        size_t cap = 1, len = 0;
        char *s = calloc(1, 1);
        const cJSON *p;
        cJSON_ArrayForEach(p, content)
        {
            cJSON *t = cJSON_GetObjectItemCaseSensitive(p, "text");
            if (!cJSON_IsString(t) || !s) {
                continue;
            }
            size_t n = strlen(t->valuestring);
            char *ns = realloc(s, cap + n);
            if (!ns) {
                free(s);
                return NULL;
            }
            s = ns;
            memcpy(s + len, t->valuestring, n + 1);
            len += n;
            cap += n;
        }
        return s;
    }
    return NULL;
}

static void add_msg(cJSON *arr, const char *role, const char *content)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "content", content);
    cJSON_AddItemToArray(arr, m);
}

static int has_system(const chat_t *c)
{
    const cJSON *m = cJSON_GetArrayItem(c->hist, 0);
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "role");
    return cJSON_IsString(r) && strcmp(r->valuestring, "system") == 0;
}

int chat_set_system(chat_t *c, const char *text)
{
    if (has_system(c)) {
        cJSON_DeleteItemFromArray(c->hist, 0);
    }
    if (!text || !*text) {
        return 0;
    }
    cJSON *m = cJSON_CreateObject();
    if (!m) {
        return -1;
    }
    cJSON_AddStringToObject(m, "role", "system");
    cJSON_AddStringToObject(m, "content", text);
    if (!cJSON_InsertItemInArray(c->hist, 0, m)) {
        cJSON_Delete(m);
        return -1;
    }
    return 0;
}

static size_t hist_bytes(const chat_t *c)
{
    size_t n = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, c->hist)
    {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(m, "content");
        if (cJSON_IsString(t)) {
            n += strlen(t->valuestring);
        }
    }
    return n;
}

static int msg_is_user(const cJSON *m)
{
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "role");
    return cJSON_IsString(r) && strcmp(r->valuestring, "user") == 0;
}

/* Drop oldest whole turns (user .. final answer, incl. tool traffic) until `extra` more bytes fit. */
static void trim_history(chat_t *c, size_t extra)
{
    int first = has_system(c) ? 1 : 0;
    size_t bytes = hist_bytes(c);

    while (bytes + extra > CHAT_HIST_MAX_BYTES) {
        int n = cJSON_GetArraySize(c->hist);
        int next = first + 1; /* start of the 2nd turn */
        while (next < n && !msg_is_user(cJSON_GetArrayItem(c->hist, next))) {
            next++;
        }
        if (next >= n) {
            break; /* only one turn left: keep it */
        }
        for (int i = first; i < next; i++) {
            const cJSON *t = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(c->hist, first), "content");
            if (cJSON_IsString(t)) {
                bytes -= strlen(t->valuestring);
            }
            cJSON_DeleteItemFromArray(c->hist, first);
            c->dropped++;
        }
    }
}

void chat_set_tools(chat_t *c, cJSON *tools, char *(*cb)(void *, const char *, const char *), void *ud)
{
    cJSON_Delete(c->tools);
    c->tools = tools;
    c->tool_cb = cb;
    c->tool_ud = ud;
}

void chat_set_events(chat_t *c, void (*cb)(void *, int, const char *), void *ud)
{
    c->ev_cb = cb;
    c->ev_ud = ud;
    c->thoughts = cb && strstr(c->base, "generativelanguage.googleapis.com") != NULL;
}

/*
 * Reasoning: a "reasoning_content"/"reasoning" field, or leading <think>,
 * <thinking>, <thought> blocks in the text (removed from `text` in place).
 */
static void report_thoughts(chat_t *c, const cJSON *msg, char *text)
{
    if (!c->ev_cb) {
        return;
    }
    const char *fields[] = {"reasoning_content", "reasoning"};
    for (size_t i = 0; i < 2; i++) {
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(msg, fields[i]);
        if (cJSON_IsString(r) && *r->valuestring) {
            c->ev_cb(c->ev_ud, CHAT_EV_THINKING, r->valuestring);
            break;
        }
    }
    static const char *const tags[] = {"think", "thinking", "thought"};
    for (int found = 1; text && found;) {
        found = 0;
        char *p = text;
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
            p++;
        }
        for (size_t i = 0; i < 3 && !found; i++) {
            char open[16], close[20];
            snprintf(open, sizeof(open), "<%s>", tags[i]);
            snprintf(close, sizeof(close), "</%s>", tags[i]);
            if (strncmp(p, open, strlen(open)) != 0) {
                continue;
            }
            char *body = p + strlen(open);
            char *end = strstr(body, close);
            char *rest = end ? end + strlen(close) : body + strlen(body);
            if (end) {
                *end = '\0';
            }
            if (*body) {
                c->ev_cb(c->ev_ud, CHAT_EV_THINKING, body);
            }
            memmove(text, rest, strlen(rest) + 1);
            found = 1;
        }
    }
}

/* Append a tool-role result message. */
static void add_tool_result(cJSON *arr, const char *id, const char *name, const char *content)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "tool");
    cJSON_AddStringToObject(m, "tool_call_id", id);
    cJSON_AddStringToObject(m, "name", name);
    cJSON_AddStringToObject(m, "content", content);
    cJSON_AddItemToArray(arr, m);
}

/*
 * Run every tool call in an assistant message, appending results to history.
 * Returns 1 if the user interrupted (remaining calls are answered "skipped").
 */
static int run_tool_calls(chat_t *c, const cJSON *calls)
{
    int interrupted = 0;
    const cJSON *call;

    cJSON_ArrayForEach(call, calls)
    {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(call, "id");
        const cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(fn, "name");
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
        const char *sid = cJSON_IsString(id) ? id->valuestring : "";
        const char *sname = cJSON_IsString(name) ? name->valuestring : "";
        char *res = NULL;

        if (interrupted) {
            add_tool_result(c->hist, sid, sname, "skipped: interrupted by user");
            continue;
        }
        if (c->tool_cb && *sname) {
            res = c->tool_cb(c->tool_ud, sname, cJSON_IsString(args) ? args->valuestring : "{}");
        }
        add_tool_result(c->hist, sid, sname, res && *res ? res : (res ? "(no output)" : "ERROR: tool failed"));
        free(res);
        if (term_interrupted()) {
            interrupted = 1;
        }
    }
    return interrupted;
}

int chat_send(chat_t *c, const char *user, char **reply, char **err)
{
    http_resp_t r;
    int ret = CHAT_ERR_OTHER;
    int tools_ran = 0;
    char *url = url_for(c, "/chat/completions");
    *reply = NULL;
    *err = NULL;

    trim_history(c, strlen(user));
    add_msg(c->hist, "user", user);
    int user_idx = cJSON_GetArraySize(c->hist) - 1;

    if (!url) {
        *err = strdup("out of memory");
        goto fail;
    }

    for (int round = 0; round <= CHAT_MAX_TOOL_ROUNDS; round++) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "model", c->model);
        cJSON_AddItemReferenceToObject(root, "messages", c->hist);
        if (c->tools && cJSON_GetArraySize(c->tools) > 0) {
            cJSON_AddItemReferenceToObject(root, "tools", c->tools);
        }
        if (c->thoughts) {
            cJSON *eb = cJSON_AddObjectToObject(root, "extra_body");
            cJSON *tc = cJSON_AddObjectToObject(cJSON_AddObjectToObject(eb, "google"), "thinking_config");
            cJSON_AddTrueToObject(tc, "include_thoughts");
        }
        char *body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root); /* only drops the references (and extra_body) */
        if (!body) {
            *err = strdup("out of memory");
            goto fail;
        }

        int rc = http_request(url, c->key, body, CHAT_TIMEOUT_S, &r);
        free(body);
        if (rc != HTTP_OK) {
            ret = transport_error(rc, &r, err);
            http_resp_free(&r);
            goto fail;
        }
        if (r.status == 400 && c->thoughts) {
            c->thoughts = 0; /* endpoint refused extra_body: retry without it */
            http_resp_free(&r);
            round--;
            continue;
        }
        if (r.status < 200 || r.status >= 300) {
            *err = error_text(r.status, r.body);
            ret = (r.status == 401 || r.status == 403) ? CHAT_ERR_AUTH : CHAT_ERR_OTHER;
            http_resp_free(&r);
            goto fail;
        }

        cJSON *j = cJSON_Parse(r.body);
        http_resp_free(&r);
        cJSON *ch = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "choices"), 0);
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(ch, "message");
        cJSON *calls = cJSON_GetObjectItemCaseSensitive(msg, "tool_calls");

        if (cJSON_IsArray(calls) && cJSON_GetArraySize(calls) > 0 && c->tool_cb) {
            /* Keep the assistant message minimal but verbatim in tool_calls
             * (Gemini needs extra_content.google.thought_signature echoed back). */
            cJSON *am = cJSON_CreateObject();
            cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
            cJSON_AddStringToObject(am, "role", "assistant");
            if (cJSON_IsString(content)) {
                report_thoughts(c, msg, content->valuestring);
                if (c->ev_cb && content->valuestring[strspn(content->valuestring, " \n\r\t")]) {
                    c->ev_cb(c->ev_ud, CHAT_EV_TEXT, content->valuestring);
                }
                cJSON_AddStringToObject(am, "content", content->valuestring);
            } else {
                report_thoughts(c, msg, NULL);
                cJSON_AddNullToObject(am, "content");
            }
            cJSON_AddItemToObject(am, "tool_calls", cJSON_Duplicate(calls, 1));
            cJSON_AddItemToArray(c->hist, am);
            cJSON_Delete(j);

            tools_ran = 1;
            int intr = run_tool_calls(c, cJSON_GetObjectItemCaseSensitive(am, "tool_calls"));
            if (intr || term_interrupted()) {
                ret = CHAT_ERR_ABORT;
                goto fail;
            }
            continue;
        }

        char *text = extract_content(cJSON_GetObjectItemCaseSensitive(msg, "content"));
        if (!text && cJSON_IsObject(msg)) {
            text = strdup(""); /* null content, no tool calls: an empty answer */
        }
        report_thoughts(c, msg, text);
        cJSON_Delete(j);
        if (!text) {
            *err = strdup("unexpected response format (no message content)");
            goto fail;
        }
        add_msg(c->hist, "assistant", text);
        *reply = text;
        free(url);
        return CHAT_OK;
    }

    /* Too many rounds: close the turn so the model sees why. */
    {
        const char *note = "[stopped: too many tool calls in one turn. Tell the user what was done and what is left.]";
        add_msg(c->hist, "assistant", note);
        *reply = strdup(note);
        free(url);
        return CHAT_OK;
    }

fail:
    free(url);
    if (tools_ran) {
        /* Side effects already happened: keep the record and close the turn. */
        char note[700];
        snprintf(note, sizeof(note), "[turn %s after tool calls ran%s%.400s]",
                 ret == CHAT_ERR_ABORT ? "interrupted by the user" : "failed", *err ? ": " : "", *err ? *err : "");
        add_msg(c->hist, "assistant", note);
    } else {
        while (cJSON_GetArraySize(c->hist) > user_idx) { /* roll back the turn */
            cJSON_DeleteItemFromArray(c->hist, user_idx);
        }
    }
    return ret;
}
