#ifndef BC_CHAT_H
#define BC_CHAT_H

#include "cJSON.h"

/* OpenAI-compatible chat session. All state lives in RAM only. */
typedef struct {
    char  *base;  /* e.g. https://host/v1 (no trailing slash) */
    char  *key;   /* may be empty */
    char  *model;
    cJSON *hist;  /* array of {role, content}; hist[0] is the system prompt if set */
    int    dropped; /* total messages trimmed from the front of the history */
    cJSON *tools;   /* OpenAI "tools" array, or NULL */
    char *(*tool_cb)(void *ud, const char *name, const char *args_json); /* returns malloc'd result text */
    void  *tool_ud;
} chat_t;

/* Max model<->tool round trips inside one user turn. */
#define CHAT_MAX_TOOL_ROUNDS 25

/* Context budget (bytes of message text kept in RAM and sent per request). */
#ifndef CHAT_HIST_MAX_BYTES
#define CHAT_HIST_MAX_BYTES (300 * 1024)
#endif

#define CHAT_OK         0
#define CHAT_ERR_AUTH   1  /* 401/403 */
#define CHAT_ERR_OTHER  2
#define CHAT_ERR_ABORT (-2)

int  chat_init(chat_t *c, const char *base, const char *key, const char *model);
void chat_free(chat_t *c);

/* Enable tool calling. Takes ownership of `tools`. cb runs each call and returns the result text. */
void chat_set_tools(chat_t *c, cJSON *tools, char *(*cb)(void *, const char *, const char *), void *ud);

/* Set (or, with NULL/"", clear) the system prompt. Never trimmed from history. */
int  chat_set_system(chat_t *c, const char *text);

/* GET {base}/models. On failure *err is a malloc'd message. */
int  chat_validate(chat_t *c, char **err);

/*
 * Send one user turn; on success *reply is malloc'd assistant text and the
 * whole turn (user, tool calls/results, final answer) is kept in history.
 * On failure before any tool ran, history is unchanged; after tools ran the
 * turn is kept and closed with an assistant note so side effects stay visible. Oldest
 * user/assistant pairs are dropped first when over CHAT_HIST_MAX_BYTES.
 */
int  chat_send(chat_t *c, const char *user, char **reply, char **err);

#endif
