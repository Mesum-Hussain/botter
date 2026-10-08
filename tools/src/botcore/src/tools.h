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

/* Child-side sandbox (after fork, before exec). 0 ok, -1 refuse to run. */
int    tool_sandbox_apply(int cpu_s);
double tool_now_s(void);

/* External tools = executables embedded in the agent pack (tool_ext.c). */
typedef struct ext_tool ext_tool_t;
void   ext_init(void);                           /* scan the VFS, print warnings for bad tools */
const ext_tool_t *ext_find(const char *name);
void   ext_schema_append(cJSON *arr);
bool   ext_run(const ext_tool_t *t, const cJSON *input, char *result, size_t result_len);

/* Startup probe: 1 if shell_exec children run in an empty network namespace. */
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
