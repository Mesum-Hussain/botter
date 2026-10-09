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
    void (*ev_cb)(void *ud, int kind, const char *text); /* optional: CHAT_EV_* */
    void  *ev_ud;
    int    thoughts; /* ask Gemini for its thoughts (cleared if the endpoint refuses) */
    int    want_thoughts; /* the UI shows reasoning (chat_set_events) */
    int    no_stream;     /* endpoint refused "stream": true; plain responses from now on */
} chat_t;

/* Events (only when an event callback is set; reasoning is then also kept out of the reply). */
#define CHAT_EV_THINKING    1 /* model reasoning (whole; non-streaming responses) */
#define CHAT_EV_TEXT        2 /* assistant text sent together with tool calls (whole, after its deltas) */
#define CHAT_EV_TEXT_DELTA  3 /* streamed piece of the answer / interim text */
#define CHAT_EV_THINK_DELTA 4 /* streamed piece of reasoning */

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

/*
 * Switch c to the endpoint, key and model of `from` (a freshly validated
 * chat_init session, freed here), keeping c's history, tools and callbacks.
 * Tool calls in the history are reduced to the standard fields, so another
 * provider does not reject the previous one's extensions.
 */
void chat_switch(chat_t *c, chat_t *from);

/* Enable tool calling. Takes ownership of `tools`. cb runs each call and returns the result text. */
void chat_set_tools(chat_t *c, cJSON *tools, char *(*cb)(void *, const char *, const char *), void *ud);

/*
 * Report reasoning / interim text through cb (see CHAT_EV_*); replies are
 * streamed (deltas) when the endpoint supports it. thoughts = ask Gemini for
 * its reasoning too.
 */
void chat_set_events(chat_t *c, void (*cb)(void *, int, const char *), void *ud, int thoughts);

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
