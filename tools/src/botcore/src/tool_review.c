/*
 * sqnc_review: the logic check of an agent project's SQNC.md, by the connected
 * LLM (a fresh request, no conversation). The syntax and names are checked by
 * the build (sqnc.c); this asks whether the steps make sense: can each step be
 * done with the named tool or skill and the data available then, do payloads
 * fit the tools' parameters, are loops bounded, is anything outward-facing done
 * without asking, does the flow fit what agent.md says the agent is for.
 * Registered only for builder agents (config.json "builder": true, i.e. Botter).
 */
#define _GNU_SOURCE
#include "chat.h"
#include "guard.h"
#include "sqnc.h"
#include "tools.h"

#include <dirent.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static chat_t *g_chat;

void review_set_chat(chat_t *c)
{
    g_chat = c;
}

/* Whole file (at most cap bytes), or NULL. */
static char *slurp(const char *path, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    char *b = malloc(cap + 1);
    size_t n = b ? fread(b, 1, cap, f) : 0;
    fclose(f);
    if (b) {
        b[n] = '\0';
    }
    return b;
}

typedef struct {
    char  *p;
    size_t len, cap;
} sb_t;

static void sb_add(sb_t *b, const char *s)
{
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        size_t nc = (b->len + n + 1) * 2;
        char *np = realloc(b->p, nc);
        if (!np) {
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n + 1);
    b->len += n;
}

static void sb_fmt(sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_fmt(sb_t *b, const char *fmt, ...)
{
    char *t = NULL;
    va_list ap;
    va_start(ap, fmt);
    if (vasprintf(&t, fmt, ap) >= 0) {
        sb_add(b, t);
        free(t);
    }
    va_end(ap);
}

static void diag_collect(void *ud, int line, int error, const char *msg)
{
    if (error) {
        sb_fmt(ud, "SQNC.md:%d: %s\n", line, msg);
    }
}

/* skills/<n>/SKILL.md: name + description lines; tools/doc/<n>.json: whole (capped). */
static void add_capabilities(sb_t *b, const char *dir)
{
    char p[PATH_MAX + 64];
    snprintf(p, sizeof(p), "%s/skills", dir);
    DIR *d = opendir(p);
    struct dirent *e;
    sb_add(b, "\n## Skills (INVOKE SKILL)\n");
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char f[PATH_MAX + 300];
        snprintf(f, sizeof(f), "%s/skills/%s/SKILL.md", dir, e->d_name);
        char *t = slurp(f, 4096);
        const char *desc = t ? strstr(t, "description:") : NULL;
        sb_fmt(b, "- %s: %.*s\n", e->d_name, desc ? (int)strcspn(desc + 12, "\n") : 0, desc ? desc + 12 : "");
        free(t);
    }
    if (d) {
        closedir(d);
    }
    snprintf(p, sizeof(p), "%s/tools/doc", dir);
    d = opendir(p);
    sb_add(b, "\n## Agent tools (EXECUTE tool), with their JSON Schema parameters\n");
    while (d && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 6 || strcmp(e->d_name + n - 5, ".json") != 0) {
            continue;
        }
        char f[PATH_MAX + 300];
        snprintf(f, sizeof(f), "%s/tools/doc/%s", dir, e->d_name);
        char *t = slurp(f, 6000);
        sb_fmt(b, "- %.*s: %s\n", (int)(n - 5), e->d_name, t ? t : "(unreadable)");
        free(t);
    }
    if (d) {
        closedir(d);
    }
    sb_add(b, "Built-in tools: fs_list, fs_read {path}, fs_write {path, content}, shell_exec {command}, "
              "vfs_list, vfs_read, get_time, cron_set, cron_list, cron_delete.\n");
}

static const char REVIEWER[] =
    "You review SQNC.md, the step-by-step flow of an AI agent, written in Sqnc. Its syntax and names are already "
    "checked; judge only whether the LOGIC makes sense. How Sqnc runs: botcore executes the structure itself "
    "(EXECUTE tool calls the tool with the payload or parameters, its output is `result`; ASK USER shows the "
    "question and waits, the reply is `answer`; SAVE/SET store values; IF, FOR EACH, WHILE ... AT MOST N TIMES, "
    "RETRY UP TO N TIMES (repeats its body while a step in it fails) and IN PARALLEL control the order; comparisons "
    "such as IS EQUAL TO, CONTAINS, IS EMPTY are evaluated exactly). Plain-English lines, INVOKE SKILL, and "
    "conditions or values written in prose are handed to an LLM, one step at a time, which can use the agent's "
    "tools but cannot ask the user anything during that step. RETURN ends the flow.\n"
    "Find real problems only: a step that cannot be done with the named tool or skill or the data available at "
    "that point; payload keys that do not match the tool's parameters, or required parameters missing; a value "
    "of the wrong kind (FOR EACH over a single text, a comparison against something never set to that form); a "
    "condition that nothing earlier lets anyone decide; a RETRY around steps that cannot fail or a loop that "
    "cannot end; something outward-facing or irreversible (sending, posting, paying, deleting) with no ASK USER "
    "approval before it; a plain-English step that needs the user's input (it cannot ask: use ASK USER first); "
    "unreachable or pointless steps; a flow that does not do what agent.md says the agent is for; a RETURN that "
    "tells the user nothing useful. Ignore style and wording.\n"
    "Reply with only JSON: {\"makes_sense\": true or false, \"summary\": \"one sentence\", \"problems\": "
    "[{\"line\": N, \"statement\": \"the line\", \"why\": \"why it does not make sense\", \"suggestion\": \"the "
    "corrected Sqnc line(s), or what to do instead\"}]}. makes_sense is false only if there is at least one real "
    "problem; then list each one.";

static bool sqnc_review(const cJSON *in, char *result, size_t rl)
{
    const cJSON *dj = cJSON_GetObjectItemCaseSensitive(in, "dir");
    const char *dir = cJSON_IsString(dj) && *dj->valuestring ? dj->valuestring : ".";
    char res[PATH_MAX], f[PATH_MAX + 64];
    int inside = 0;
    if (!g_chat) {
        snprintf(result, rl, "no LLM connection");
        return false;
    }
    if (guard_resolve(dir, res, &inside) != 0 || !inside) {
        snprintf(result, rl, "'%s' must be a project folder inside the working directory", dir);
        return false;
    }
    snprintf(f, sizeof(f), "%s/SQNC.md", res);
    char *src = slurp(f, 200000);
    if (!src) {
        snprintf(result, rl, "%s has no SQNC.md", dir);
        return false;
    }

    /* syntax first: a flow that does not parse is not worth an LLM call */
    sb_t errs = {0};
    sq_prog prog;
    memset(&prog, 0, sizeof(prog));
    prog.diag = diag_collect;
    prog.ud = &errs;
    int ne = sq_parse(src, strlen(src), &prog);
    sq_free(&prog);
    if (ne) {
        snprintf(result, rl,
                 "{\"makes_sense\": false, \"summary\": \"SQNC.md has syntax errors; fix them first (agent_build "
                 "dry_run shows them)\", \"problems\": [], \"report\": %s}",
                 "\"see the syntax errors below\"");
        size_t l = strlen(result);
        snprintf(result + l, rl - l, "\n%s", errs.p ? errs.p : "");
        free(errs.p);
        free(src);
        return true;
    }
    free(errs.p);

    sb_t q = {0};
    snprintf(f, sizeof(f), "%s/agent.md", res);
    char *agent = slurp(f, 8000);
    sb_fmt(&q, "## agent.md (what the agent is for)\n%s\n", agent ? agent : "(none)");
    free(agent);
    add_capabilities(&q, res);
    sb_add(&q, "\n## SQNC.md (with line numbers)\n");
    int line = 1;
    for (char *l = src, *nl; l && *l; l = nl ? nl + 1 : NULL, line++) {
        nl = strchr(l, '\n');
        sb_fmt(&q, "%4d| %.*s\n", line, nl ? (int)(nl - l) : (int)strlen(l), l);
    }
    free(src);

    char *reply = NULL, *err = NULL;
    int rc = chat_ask(g_chat, REVIEWER, q.p ? q.p : "", 0, &reply, &err);
    free(q.p);
    if (rc != CHAT_OK) {
        snprintf(result, rl, "review failed: %s", err ? err : "interrupted");
        free(err);
        free(reply);
        return false;
    }

    /* the verdict as JSON, plus a readable report */
    cJSON *j = cJSON_Parse(reply);
    if (!j) {
        char *a = strchr(reply, '{'), *b = strrchr(reply, '}');
        if (a && b > a) {
            b[1] = '\0';
            j = cJSON_Parse(a);
        }
    }
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        snprintf(result, rl, "{\"makes_sense\": null, \"summary\": \"the reviewer did not answer in JSON\", "
                             "\"problems\": [], \"report\": \"\"}\n%.2000s",
                 reply);
        free(reply);
        return true;
    }
    free(reply);
    sb_t rep = {0};
    const cJSON *p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(j, "problems"))
    {
        const cJSON *ln = cJSON_GetObjectItemCaseSensitive(p, "line");
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(p, "statement");
        const cJSON *why = cJSON_GetObjectItemCaseSensitive(p, "why");
        const cJSON *sg = cJSON_GetObjectItemCaseSensitive(p, "suggestion");
        sb_fmt(&rep, "- line %d: %s\n  why: %s\n  instead: %s\n", cJSON_IsNumber(ln) ? ln->valueint : 0,
               cJSON_IsString(st) ? st->valuestring : "", cJSON_IsString(why) ? why->valuestring : "",
               cJSON_IsString(sg) ? sg->valuestring : "");
    }
    if (!cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(j, "makes_sense"))) {
        cJSON_DeleteItemFromObjectCaseSensitive(j, "makes_sense");
        cJSON_AddBoolToObject(j, "makes_sense", rep.len == 0);
    }
    cJSON_DeleteItemFromObjectCaseSensitive(j, "report");
    cJSON_AddStringToObject(j, "report", rep.p ? rep.p : "");
    free(rep.p);
    char *out = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    snprintf(result, rl, "%s", out ? out : "{}");
    free(out);
    return true;
}

const tool_t TOOLS_REVIEW[] = {
    {"sqnc_review",
     "Check whether an agent project's SQNC.md makes sense (its logic, not its syntax) with a fresh LLM review: "
     "steps that cannot work, payloads that do not fit the tools, missing approvals before outward-facing steps, "
     "loops that cannot end, a flow that does not fit agent.md. Returns JSON {makes_sense, summary, problems: "
     "[{line, statement, why, suggestion}], report}. Run it after agent_build dry_run passes; if makes_sense is "
     "false, tell the user why and what the review suggests instead.",
     "{\"type\":\"object\",\"properties\":{\"dir\":{\"type\":\"string\",\"description\":\"Agent project folder, "
     "default the working directory\"}}}",
     sqnc_review},
};
const size_t TOOLS_REVIEW_N = sizeof(TOOLS_REVIEW) / sizeof(TOOLS_REVIEW[0]);
