#define _GNU_SOURCE
#include "guard.h"
#include "tools.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/*
 * RAM-only, session-lifetime scheduler. Nothing is persisted. Entries are
 * checked only while the REPL is idle at its prompt (single-threaded), so a
 * due task may run a little late if the agent is busy.
 */

#define CRON_MAX        16
#define CRON_ACTION_MAX 256

enum { C_PERIODIC = 1, C_DAILY, C_ONCE };

typedef struct {
    int    id; /* 0 = free slot */
    int    type;
    int    interval_min; /* periodic: period; once: original delay */
    int    hour, minute; /* daily */
    char   action[CRON_ACTION_MAX];
    time_t next;
} entry_t;

static entry_t g_e[CRON_MAX];
static int     g_next_id = 1;

static time_t next_daily(int hour, int minute, time_t after)
{
    struct tm tm;
    localtime_r(&after, &tm);
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t <= after) {
        tm.tm_mday++;
        tm.tm_isdst = -1;
        t = mktime(&tm);
    }
    return t;
}

int cron_due(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < CRON_MAX; i++) {
        if (g_e[i].id && g_e[i].next <= now) {
            return 1;
        }
    }
    return 0;
}

int cron_take_due(char *action, size_t action_len)
{
    time_t now = time(NULL);
    for (int i = 0; i < CRON_MAX; i++) {
        entry_t *e = &g_e[i];
        if (!e->id || e->next > now) {
            continue;
        }
        int id = e->id;
        snprintf(action, action_len, "%s", e->action);
        if (e->type == C_ONCE) {
            memset(e, 0, sizeof(*e));
        } else if (e->type == C_PERIODIC) {
            e->next = now + (time_t)e->interval_min * 60;
        } else {
            e->next = next_daily(e->hour, e->minute, now);
        }
        return id;
    }
    return 0;
}

static int get_int(const cJSON *in, const char *k, int *out)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(in, k);
    if (!cJSON_IsNumber(j)) {
        return 0;
    }
    *out = (int)j->valuedouble;
    return 1;
}

static bool cron_set(const cJSON *in, char *result, size_t rl)
{
    const cJSON *tj = cJSON_GetObjectItemCaseSensitive(in, "type");
    const cJSON *aj = cJSON_GetObjectItemCaseSensitive(in, "action");
    int v = 0, minute = 0;

    if (guard_plan()) {
        snprintf(result, rl, "%s", GUARD_PLAN_REFUSAL);
        return false;
    }

    if (!cJSON_IsString(tj)) {
        snprintf(result, rl, "'type' required: periodic, daily or once");
        return false;
    }
    if (!cJSON_IsString(aj) || !*aj->valuestring) {
        snprintf(result, rl, "'action' required: the instruction to run when the schedule fires");
        return false;
    }
    if (strlen(aj->valuestring) >= CRON_ACTION_MAX) {
        snprintf(result, rl, "'action' too long (max %d chars)", CRON_ACTION_MAX - 1);
        return false;
    }
    entry_t ne;
    memset(&ne, 0, sizeof(ne));
    time_t now = time(NULL);
    const char *ty = tj->valuestring;

    if (strcmp(ty, "periodic") == 0) {
        if (!get_int(in, "interval_minutes", &v) || v < 1 || v > 1440) {
            snprintf(result, rl, "periodic needs 'interval_minutes' 1-1440");
            return false;
        }
        ne.type = C_PERIODIC;
        ne.interval_min = v;
        ne.next = now + (time_t)v * 60;
    } else if (strcmp(ty, "once") == 0) {
        if (!get_int(in, "delay_minutes", &v) || v < 1 || v > 1440) {
            snprintf(result, rl, "once needs 'delay_minutes' 1-1440");
            return false;
        }
        ne.type = C_ONCE;
        ne.interval_min = v;
        ne.next = now + (time_t)v * 60;
    } else if (strcmp(ty, "daily") == 0) {
        get_int(in, "minute", &minute);
        if (!get_int(in, "hour", &v) || v < 0 || v > 23 || minute < 0 || minute > 59) {
            snprintf(result, rl, "daily needs 'hour' 0-23 and optional 'minute' 0-59 (local time)");
            return false;
        }
        ne.type = C_DAILY;
        ne.hour = v;
        ne.minute = minute;
        ne.next = next_daily(v, minute, now);
    } else {
        snprintf(result, rl, "type must be periodic, daily or once");
        return false;
    }
    snprintf(ne.action, sizeof(ne.action), "%s", aj->valuestring);

    for (int i = 0; i < CRON_MAX; i++) {
        if (!g_e[i].id) {
            ne.id = g_next_id++;
            g_e[i] = ne;
            char when[32];
            struct tm tm;
            strftime(when, sizeof(when), "%a %H:%M %Z", localtime_r(&ne.next, &tm));
            snprintf(result, rl, "created schedule #%d (first run %s): %s. Runs only during this session.", ne.id,
                     when, ne.action);
            return true;
        }
    }
    snprintf(result, rl, "no free schedule slots (max %d); delete one first", CRON_MAX);
    return false;
}

static bool cron_list(const cJSON *in, char *result, size_t rl)
{
    (void)in;
    size_t off = 0;
    time_t now = time(NULL);
    for (int i = 0; i < CRON_MAX; i++) {
        const entry_t *e = &g_e[i];
        if (!e->id) {
            continue;
        }
        char kind[48];
        if (e->type == C_PERIODIC) {
            snprintf(kind, sizeof(kind), "every %d min", e->interval_min);
        } else if (e->type == C_ONCE) {
            snprintf(kind, sizeof(kind), "once");
        } else {
            snprintf(kind, sizeof(kind), "daily %02d:%02d", e->hour, e->minute);
        }
        long mins = (long)((e->next - now + 59) / 60);
        int w = snprintf(result + off, rl - off, "#%d %s (next in %ld min): %s\n", e->id, kind, mins < 0 ? 0 : mins,
                         e->action);
        if (w < 0 || (size_t)w >= rl - off) {
            break;
        }
        off += (size_t)w;
    }
    if (!off) {
        snprintf(result, rl, "no schedules");
    }
    return true;
}

static bool cron_delete(const cJSON *in, char *result, size_t rl)
{
    int id;
    if (!get_int(in, "id", &id)) {
        snprintf(result, rl, "'id' (number) required");
        return false;
    }
    for (int i = 0; i < CRON_MAX; i++) {
        if (g_e[i].id == id) {
            memset(&g_e[i], 0, sizeof(g_e[i]));
            snprintf(result, rl, "deleted schedule #%d", id);
            return true;
        }
    }
    snprintf(result, rl, "schedule #%d not found", id);
    return true;
}

static bool get_time(const cJSON *in, char *result, size_t rl)
{
    (void)in;
    time_t now = time(NULL);
    struct tm tm;
    char buf[64];
    strftime(buf, sizeof(buf), "%A %Y-%m-%d %H:%M:%S %Z (UTC%z)", localtime_r(&now, &tm));
    snprintf(result, rl, "%s", buf);
    return true;
}

const tool_t TOOLS_CRON[] = {
    {"get_time", "Get the current local date, time and timezone.", "{\"type\":\"object\",\"properties\":{}}",
     get_time},
    {"cron_set",
     "Schedule an instruction for yourself. When it fires you receive the 'action' text as a new task. "
     "Schedules live in memory for this session only and fire only while the agent is idle at the prompt. "
     "type=periodic needs interval_minutes (1-1440); type=daily needs hour (0-23, local time) and optional "
     "minute; type=once needs delay_minutes (1-1440).",
     "{\"type\":\"object\",\"properties\":{\"type\":{\"type\":\"string\",\"enum\":[\"periodic\",\"daily\",\"once\"]},"
     "\"action\":{\"type\":\"string\",\"description\":\"Instruction to carry out when it fires\"},"
     "\"interval_minutes\":{\"type\":\"integer\"},\"hour\":{\"type\":\"integer\"},\"minute\":{\"type\":\"integer\"},"
     "\"delay_minutes\":{\"type\":\"integer\"}},\"required\":[\"type\",\"action\"]}",
     cron_set},
    {"cron_list", "List active schedules.", "{\"type\":\"object\",\"properties\":{}}", cron_list},
    {"cron_delete", "Delete a schedule by id.",
     "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"}},\"required\":[\"id\"]}", cron_delete},
};
const size_t TOOLS_CRON_N = sizeof(TOOLS_CRON) / sizeof(TOOLS_CRON[0]);
