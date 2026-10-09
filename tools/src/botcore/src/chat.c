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
    c->thoughts = c->ev_cb && c->want_thoughts && strstr(c->base, "generativelanguage.googleapis.com") != NULL;
    c->no_stream = 0;
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

void chat_set_events(chat_t *c, void (*cb)(void *, int, const char *), void *ud, int thoughts)
{
    c->ev_cb = cb;
    c->ev_ud = ud;
    c->want_thoughts = thoughts;
    c->thoughts = cb && thoughts && strstr(c->base, "generativelanguage.googleapis.com") != NULL;
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

/* ---- streaming (Server-Sent Events) ------------------------------------ */

typedef struct {
    char  *p;
    size_t len, cap;
} buf_t;

static int buf_add(buf_t *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + n + 1) {
            nc *= 2;
        }
        char *np = realloc(b->p, nc);
        if (!np) {
            return -1;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 0;
}

static void buf_cut(buf_t *b, size_t n) /* drop the first n bytes */
{
    memmove(b->p, b->p + n, b->len - n + 1);
    b->len -= n;
}

typedef struct {
    chat_t *c;
    buf_t   line;    /* SSE bytes not yet split into lines */
    buf_t   text;    /* answer text without reasoning */
    buf_t   pend;    /* content not yet classified as text or reasoning */
    int     tstate;  /* 0 = before any text (a <think> block may start), 1 = inside one, 2 = text */
    char    close[16];
    cJSON  *calls;   /* tool calls assembled from deltas */
    char   *err;     /* error object sent in the stream */
    int     events;  /* data: lines seen */
} stream_t;

static void emit(stream_t *s, int kind, const char *p, size_t n)
{
    if (!n) {
        return;
    }
    if (kind == CHAT_EV_TEXT_DELTA) {
        buf_add(&s->text, p, n);
    }
    if (s->c->ev_cb) {
        char *t = strndup(p, n);
        if (t) {
            s->c->ev_cb(s->c->ev_ud, kind, t);
            free(t);
        }
    }
}

/*
 * Content pieces -> text or reasoning: leading <think>/<thinking>/<thought>
 * blocks are reasoning (as report_thoughts does for whole messages). Only with
 * an event callback; otherwise everything is text.
 */
static void feed_content(stream_t *s, const char *p, size_t n, int final)
{
    static const char *const tags[] = {"think", "thinking", "thought"};
    buf_add(&s->pend, p, n);
    while (s->pend.len) {
        if (s->tstate == 2 || !s->c->ev_cb) {
            emit(s, CHAT_EV_TEXT_DELTA, s->pend.p, s->pend.len);
            buf_cut(&s->pend, s->pend.len);
            s->tstate = 2;
        } else if (s->tstate == 1) {
            char *e = strstr(s->pend.p, s->close);
            if (e) {
                emit(s, CHAT_EV_THINK_DELTA, s->pend.p, (size_t)(e - s->pend.p));
                buf_cut(&s->pend, (size_t)(e - s->pend.p) + strlen(s->close));
                s->tstate = 0;
                continue;
            }
            size_t keep = final ? 0 : strlen(s->close) - 1; /* the close tag may be split */
            if (s->pend.len > keep) {
                emit(s, CHAT_EV_THINK_DELTA, s->pend.p, s->pend.len - keep);
                buf_cut(&s->pend, s->pend.len - keep);
            }
            return;
        } else {
            size_t ws = strspn(s->pend.p, " \n\r\t");
            const char *q = s->pend.p + ws;
            size_t ql = s->pend.len - ws;
            int prefix = 0, hit = 0;
            for (size_t i = 0; i < 3 && !hit; i++) {
                char open[16];
                size_t ol = (size_t)snprintf(open, sizeof(open), "<%s>", tags[i]);
                if (ql >= ol && strncmp(q, open, ol) == 0) {
                    snprintf(s->close, sizeof(s->close), "</%s>", tags[i]);
                    buf_cut(&s->pend, ws + ol);
                    s->tstate = 1;
                    hit = 1;
                } else if (ql < ol && strncmp(q, open, ql) == 0) {
                    prefix = 1;
                }
            }
            if (hit) {
                continue;
            }
            if ((prefix || ql == 0) && !final) {
                return; /* undecided: wait for more */
            }
            s->tstate = 2;
        }
    }
}

/* Merge one streamed tool-call piece into s->calls (by "index", else by "id"). */
static void merge_call(stream_t *s, const cJSON *d)
{
    const cJSON *ix = cJSON_GetObjectItemCaseSensitive(d, "index");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(d, "id");
    int n = cJSON_GetArraySize(s->calls), slot = -1;
    if (cJSON_IsNumber(ix)) {
        for (int i = 0; i < n; i++) {
            const cJSON *si = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(s->calls, i), "index");
            if (cJSON_IsNumber(si) && si->valueint == ix->valueint) {
                slot = i;
            }
        }
    } else if (cJSON_IsString(id) && *id->valuestring) {
        for (int i = 0; i < n; i++) {
            const cJSON *si = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(s->calls, i), "id");
            if (cJSON_IsString(si) && strcmp(si->valuestring, id->valuestring) == 0) {
                slot = i;
            }
        }
    } else if (n) {
        slot = n - 1;
    }
    if (slot < 0) {
        cJSON_AddItemToArray(s->calls, cJSON_Duplicate(d, 1));
        return;
    }
    cJSON *t = cJSON_GetArrayItem(s->calls, slot);
    for (const cJSON *f = d->child; f; f = f->next) {
        if (strcmp(f->string, "function") != 0) {
            cJSON_DeleteItemFromObjectCaseSensitive(t, f->string); /* id, type, extra_content: latest wins */
            cJSON_AddItemToObject(t, f->string, cJSON_Duplicate(f, 1));
            continue;
        }
        cJSON *tf = cJSON_GetObjectItemCaseSensitive(t, "function");
        if (!cJSON_IsObject(tf)) {
            cJSON_DeleteItemFromObjectCaseSensitive(t, "function");
            cJSON_AddItemToObject(t, "function", cJSON_Duplicate(f, 1));
            continue;
        }
        const cJSON *nm = cJSON_GetObjectItemCaseSensitive(f, "name");
        const cJSON *ar = cJSON_GetObjectItemCaseSensitive(f, "arguments");
        cJSON *tn = cJSON_GetObjectItemCaseSensitive(tf, "name");
        if (cJSON_IsString(nm) && *nm->valuestring && !(cJSON_IsString(tn) && *tn->valuestring)) {
            cJSON_DeleteItemFromObjectCaseSensitive(tf, "name");
            cJSON_AddStringToObject(tf, "name", nm->valuestring);
        }
        if (cJSON_IsString(ar) && *ar->valuestring) {
            cJSON *ta = cJSON_GetObjectItemCaseSensitive(tf, "arguments");
            const char *old = cJSON_IsString(ta) ? ta->valuestring : "";
            char *cat = malloc(strlen(old) + strlen(ar->valuestring) + 1);
            if (cat) {
                strcpy(cat, old);
                strcat(cat, ar->valuestring);
                cJSON_DeleteItemFromObjectCaseSensitive(tf, "arguments");
                cJSON_AddStringToObject(tf, "arguments", cat);
                free(cat);
            }
        }
    }
}

static void on_sse_event(stream_t *s, const char *data)
{
    if (strcmp(data, "[DONE]") == 0) {
        return;
    }
    cJSON *j = cJSON_Parse(data);
    if (!j) {
        return;
    }
    s->events++;
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "error");
    if (e && !s->err) {
        char *t = cJSON_PrintUnformatted(e);
        s->err = t ? t : strdup("error in stream");
    }
    const cJSON *ch = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "choices"), 0);
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(ch, "delta");
    const char *rf[] = {"reasoning_content", "reasoning"};
    for (size_t i = 0; i < 2; i++) {
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(d, rf[i]);
        if (cJSON_IsString(r) && *r->valuestring) {
            emit(s, CHAT_EV_THINK_DELTA, r->valuestring, strlen(r->valuestring));
            break;
        }
    }
    const cJSON *ct = cJSON_GetObjectItemCaseSensitive(d, "content");
    if (cJSON_IsString(ct) && *ct->valuestring) {
        feed_content(s, ct->valuestring, strlen(ct->valuestring), 0);
    }
    const cJSON *tc;
    cJSON_ArrayForEach(tc, cJSON_GetObjectItemCaseSensitive(d, "tool_calls"))
    {
        merge_call(s, tc);
    }
    cJSON_Delete(j);
}

/* http_request_stream callback: split into lines, handle "data:" lines. */
static void on_sse_bytes(void *ud, const char *p, size_t n)
{
    stream_t *s = ud;
    buf_add(&s->line, p, n);
    char *nl;
    while (s->line.p && (nl = memchr(s->line.p, '\n', s->line.len))) {
        *nl = '\0';
        if (nl > s->line.p && nl[-1] == '\r') {
            nl[-1] = '\0';
        }
        if (strncmp(s->line.p, "data:", 5) == 0) {
            on_sse_event(s, s->line.p + 5 + (s->line.p[5] == ' '));
        }
        buf_cut(&s->line, (size_t)(nl - s->line.p) + 1);
    }
}

/* After the stream: the assembled message as a non-streaming response {"choices":[{"message":...}]}. */
static cJSON *stream_result(stream_t *s)
{
    if (s->line.len && strncmp(s->line.p, "data:", 5) == 0) { /* last line without '\n' */
        on_sse_event(s, s->line.p + 5 + (s->line.p[5] == ' '));
    }
    feed_content(s, "", 0, 1);
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "assistant");
    if (s->text.len || !cJSON_GetArraySize(s->calls)) {
        cJSON_AddStringToObject(msg, "content", s->text.p ? s->text.p : "");
    } else {
        cJSON_AddNullToObject(msg, "content");
    }
    if (cJSON_GetArraySize(s->calls)) {
        cJSON *tc;
        cJSON_ArrayForEach(tc, s->calls)
        {
            cJSON_DeleteItemFromObjectCaseSensitive(tc, "index");
        }
        cJSON_AddItemToObject(msg, "tool_calls", s->calls);
        s->calls = NULL;
    }
    cJSON *j = cJSON_CreateObject();
    cJSON *ch = cJSON_CreateObject();
    cJSON_AddItemToObject(ch, "message", msg);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(j, "choices"), ch);
    return j;
}

static void stream_free(stream_t *s)
{
    free(s->line.p);
    free(s->text.p);
    free(s->pend.p);
    free(s->err);
    cJSON_Delete(s->calls);
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
        int streaming = c->ev_cb && !c->no_stream;
        if (streaming) {
            cJSON_AddTrueToObject(root, "stream");
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

        stream_t st = {.c = c, .calls = cJSON_CreateArray()};
        int rc = streaming ? http_request_stream(url, c->key, body, CHAT_TIMEOUT_S, on_sse_bytes, &st, &r)
                           : http_request(url, c->key, body, CHAT_TIMEOUT_S, &r);
        free(body);
        if (rc != HTTP_OK) {
            ret = transport_error(rc, &r, err);
            http_resp_free(&r);
            stream_free(&st);
            goto fail;
        }
        if (r.status == 400 && (c->thoughts || streaming)) {
            /* endpoint refused extra_body, then "stream": retry without it */
            if (c->thoughts) {
                c->thoughts = 0;
            } else {
                c->no_stream = 1;
            }
            http_resp_free(&r);
            stream_free(&st);
            round--;
            continue;
        }
        if (st.err) {
            *err = st.err;
            st.err = NULL;
            http_resp_free(&r);
            stream_free(&st);
            goto fail;
        }
        if (r.status < 200 || r.status >= 300) {
            *err = error_text(r.status, r.body);
            ret = (r.status == 401 || r.status == 403) ? CHAT_ERR_AUTH : CHAT_ERR_OTHER;
            http_resp_free(&r);
            stream_free(&st);
            goto fail;
        }

        /* A streamed answer is assembled into the shape of a plain one; an endpoint
         * that ignored "stream" sent plain JSON, parsed as before. */
        cJSON *j = streaming && st.events ? stream_result(&st) : cJSON_Parse(r.body);
        stream_free(&st);
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
