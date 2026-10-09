#ifndef BC_TOOLS_H
#define BC_TOOLS_H

#include "cJSON.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * Tool ABI (also the planned user-tool SDK ABI): the handler fills `result` (NUL-terminated, <= result_len)
 * and returns true on success, false on error. The text goes back to the LLM.
 */
typedef bool (*tool_fn)(const cJSON *input, char *result, size_t result_len);

typedef struct {
    const char *name;
    const char *description;
    const char *schema; /* JSON Schema for the arguments (object) */
    tool_fn     fn;
} tool_t;

#define TOOL_RESULT_MAX (32 * 1024)

/* Built-in tool tables (defined in tool_*.c / cron.c). */
extern const tool_t TOOLS_FS[];
extern const size_t TOOLS_FS_N;
extern const tool_t TOOLS_SHELL[];
extern const size_t TOOLS_SHELL_N;
extern const tool_t TOOLS_CRON[];
extern const size_t TOOLS_CRON_N;
extern const tool_t TOOLS_VFS[]; /* registered only when an agent pack is linked */
extern const size_t TOOLS_VFS_N;
extern const tool_t TOOLS_REVIEW[]; /* sqnc_review: builder agents only (tools_enable_review) */
extern const size_t TOOLS_REVIEW_N;
void tools_enable_review(int on);

/* Child-side sandbox (after fork, before exec). net = keep host network. 0 ok, -1 refuse to run. */
int    tool_sandbox_apply(int cpu_s, int net);
/* Startup: Landlock ABI (0 = unavailable) and rlimit values for children. */
int    tool_sandbox_probe(void);
/* Parent, around fork(): approved paths for the next child; no_landlock for approved sudo. */
void   tool_sandbox_grants(const char *const *paths, int n, int no_landlock);
double tool_now_s(void);

/* External tools = executables embedded in the agent pack (tool_ext.c). */
typedef struct ext_tool ext_tool_t;
void   ext_init(void);                           /* scan the VFS, print warnings for bad tools */
const ext_tool_t *ext_find(const char *name);
void   ext_schema_append(cJSON *arr);
bool   ext_run(const ext_tool_t *t, const cJSON *input, char *result, size_t result_len);
const char *ext_status(const ext_tool_t *t); /* descriptor "status" (e.g. "Searching for leads") or NULL */
/* Tools whose descriptor says "network": true -> count; names comma-joined into `names`. */
size_t ext_network_tools(char *names, size_t cap);
void   ext_allow_network(int allow); /* user's per-session answer (default: denied) */

/* Startup probe: how shell_exec / tool children are cut off from the network. */
enum { NET_ISOLATION_NONE = 0, NET_ISOLATION_NETNS = 1, NET_ISOLATION_SECCOMP = 2 };
int    tool_shell_probe(void);

/* RAM-only scheduler (cron.c). Entries live for the session only. */
int    cron_due(void);                               /* 1 if any entry is due now */
int    cron_take_due(char *action, size_t action_len); /* pops one due entry -> id, or 0 */

const tool_t *tools_find_builtin(const char *name);

/* OpenAI "tools" array for every registered tool (caller owns the result). */
cJSON *tools_schema(void);

/*
 * Run a tool by name with its JSON argument string. Always returns a
 * malloc'd, valid-UTF-8 text result (errors are reported as text).
 */
char *tools_call(const char *name, const char *args_json);

/* Replace invalid UTF-8 and control bytes (except \n \t) in place. */
void  tools_sanitize_utf8(char *s);

#endif
