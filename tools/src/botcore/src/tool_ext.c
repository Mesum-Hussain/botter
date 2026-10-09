#define _GNU_SOURCE
/*
 * External tools: executables embedded in the agent pack (tools/bin/<name>),
 * described by tools/doc/<name>.json. They run in a forked, sandboxed child
 * (same sandbox as shell_exec) straight from an anonymous in-memory file
 * (memfd_create + fexecve): nothing is written to disk.
 *
 * Protocol: the call arguments are written to the tool's stdin as one JSON
 * object; stdout is the result; exit code 0 = success, anything else = error
 * (stderr, else stdout, is returned as the error text).
 *
 * Two kinds, detected by the packer:
 *   ELF    native executable; the memfd is close-on-exec.
 *   SCRIPT starts with "#!"; the memfd must stay open across exec because the
 *          kernel hands the interpreter /proc/self/fd/N. The interpreter has to
 *          exist on the machine running the agent (checked before every run).
 */
#include "guard.h"
#include "term.h"
#include "tools.h"
#include "vfs.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define EXT_DEFAULT_TIMEOUT_S 60
#define EXT_MAX_TIMEOUT_S     600
#define EXT_ERR_CAP           2048

#define BIN_PREFIX "tools/bin/"
#define DOC_PREFIX "tools/doc/"

struct ext_tool {
    char              *name;
    char              *description;
    cJSON             *parameters;
    int                timeout_s;
    int                network; /* descriptor "network": true: runs outside the empty netns if allowed */
    int                readonly; /* descriptor "readonly": true: changes nothing, runs in Plan mode without asking */
    const vfs_entry_t *exe;
};

static ext_tool_t *g_ext;
static size_t      g_ext_n;
static int         g_net_allowed; /* user granted internet to "network" tools this session */

static int valid_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 64) {
        return 0;
    }
    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_' ||
              *s == '-')) {
            return 0;
        }
    }
    return 1;
}

static int builtin_name(const char *name)
{
    return tools_find_builtin(name) != NULL;
}

static void warn(const char *name, const char *msg)
{
    fprintf(stderr, ANSI_DIM "warning: tool '%s' skipped: %s" ANSI_RESET "\n", name, msg);
}

void ext_init(void)
{
    signal(SIGPIPE, SIG_IGN); /* a tool that exits without reading stdin must not kill us */

    size_t n = 0;
    const vfs_entry_t *t = vfs_table(&n);
    size_t plen = strlen(BIN_PREFIX);
    for (size_t i = 0; i < n; i++) {
        if (t[i].kind == VFS_DATA || strncmp(t[i].path, BIN_PREFIX, plen) != 0) {
            continue;
        }
        const char *name = t[i].path + plen;
        if (!valid_name(name)) {
            warn(name, "name must be 1-64 chars of [A-Za-z0-9_-]");
            continue;
        }
        if (builtin_name(name)) {
            warn(name, "name clashes with a built-in tool");
            continue;
        }
        char dp[128];
        snprintf(dp, sizeof(dp), DOC_PREFIX "%s.json", name);
        const vfs_entry_t *d = vfs_find(dp);
        if (!d || d->kind != VFS_DATA) {
            warn(name, "missing descriptor " DOC_PREFIX "<name>.json");
            continue;
        }
        cJSON *j = cJSON_Parse(d->data);
        const cJSON *desc = cJSON_GetObjectItemCaseSensitive(j, "description");
        const cJSON *par = cJSON_GetObjectItemCaseSensitive(j, "parameters");
        const cJSON *to = cJSON_GetObjectItemCaseSensitive(j, "timeout_s");
        const cJSON *net = cJSON_GetObjectItemCaseSensitive(j, "network");
        const cJSON *ro = cJSON_GetObjectItemCaseSensitive(j, "readonly");
        if (!cJSON_IsObject(j) || !cJSON_IsString(desc) || !desc->valuestring[0]) {
            warn(name, "descriptor must be a JSON object with a non-empty \"description\"");
            cJSON_Delete(j);
            continue;
        }
        if (par && !cJSON_IsObject(par)) {
            warn(name, "\"parameters\" must be a JSON Schema object");
            cJSON_Delete(j);
            continue;
        }
        ext_tool_t *nt = realloc(g_ext, (g_ext_n + 1) * sizeof(*g_ext));
        if (!nt) {
            cJSON_Delete(j);
            break;
        }
        g_ext = nt;
        ext_tool_t *e = &g_ext[g_ext_n++];
        e->name = strdup(name);
        e->description = strdup(desc->valuestring);
        e->parameters = par ? cJSON_Duplicate(par, 1) : cJSON_Parse("{\"type\":\"object\",\"properties\":{}}");
        e->timeout_s = EXT_DEFAULT_TIMEOUT_S;
        if (cJSON_IsNumber(to) && to->valuedouble >= 1) {
            e->timeout_s = to->valuedouble > EXT_MAX_TIMEOUT_S ? EXT_MAX_TIMEOUT_S : (int)to->valuedouble;
        }
        e->network = cJSON_IsTrue(net);
        e->readonly = cJSON_IsTrue(ro);
        e->exe = &t[i];
        cJSON_Delete(j);
    }
}

const ext_tool_t *ext_find(const char *name)
{
    for (size_t i = 0; i < g_ext_n; i++) {
        if (strcmp(g_ext[i].name, name) == 0) {
            return &g_ext[i];
        }
    }
    return NULL;
}

void ext_schema_append(cJSON *arr)
{
    for (size_t i = 0; i < g_ext_n; i++) {
        cJSON *fn = cJSON_CreateObject();
        cJSON_AddStringToObject(fn, "name", g_ext[i].name);
        cJSON_AddStringToObject(fn, "description", g_ext[i].description);
        cJSON_AddItemToObject(fn, "parameters", cJSON_Duplicate(g_ext[i].parameters, 1));
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "type", "function");
        cJSON_AddItemToObject(e, "function", fn);
        cJSON_AddItemToArray(arr, e);
    }
}

size_t ext_network_tools(char *names, size_t cap)
{
    size_t n = 0, len = 0;
    if (cap) {
        names[0] = '\0';
    }
    for (size_t i = 0; i < g_ext_n; i++) {
        if (g_ext[i].network) {
            int w = snprintf(names + len, cap > len ? cap - len : 0, "%s%s", n ? ", " : "", g_ext[i].name);
            if (w > 0 && len + (size_t)w < cap) {
                len += (size_t)w;
            }
            n++;
        }
    }
    return n;
}

void ext_allow_network(int allow)
{
    g_net_allowed = allow;
}

/* Is a command with this name/path runnable? ("env" style lookups use PATH.) */
static int runnable(const char *cmd)
{
    if (strchr(cmd, '/')) {
        return access(cmd, X_OK) == 0;
    }
    const char *path = getenv("PATH");
    if (!path) {
        path = "/usr/local/bin:/usr/bin:/bin";
    }
    char buf[4096];
    while (*path) {
        const char *end = strchr(path, ':');
        size_t l = end ? (size_t)(end - path) : strlen(path);
        if (l + strlen(cmd) + 2 < sizeof(buf)) {
            snprintf(buf, sizeof(buf), "%.*s/%s", (int)(l ? l : 1), l ? path : ".", cmd);
            if (access(buf, X_OK) == 0) {
                return 1;
            }
        }
        path += l + (end ? 1 : 0);
    }
    return 0;
}

/* Parse "#!interp [arg]"; on failure to find the interpreter write why into `why`. */
static int check_interpreter(const vfs_entry_t *x, char *why, size_t wl)
{
    char line[256];
    size_t n = x->len < sizeof(line) - 1 ? x->len : sizeof(line) - 1;
    memcpy(line, x->data, n);
    line[n] = '\0';
    char *nl = strchr(line, '\n');
    if (nl) {
        *nl = '\0';
    }
    char *p = line + 2; /* skip "#!" */
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    char *interp = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r') {
        p++;
    }
    char *rest = p;
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    *p = '\0';
    if (!*interp) {
        snprintf(why, wl, "script has an empty #! line");
        return 0;
    }
    const char *check = interp;
    const char *base = strrchr(interp, '/');
    if (base && strcmp(base + 1, "env") == 0 && *rest) {
        /* "#!/usr/bin/env python3": the real interpreter is the next word */
        char *e = rest;
        while (*e && *e != ' ' && *e != '\t' && *e != '\r') {
            e++;
        }
        *e = '\0';
        check = rest;
    }
    if (!runnable(check)) {
        snprintf(why, wl, "this tool needs '%.200s', which is not installed on this machine", check);
        return 0;
    }
    return 1;
}

static void append(char *buf, size_t cap, size_t *len, size_t *total, const char *data, size_t n)
{
    *total += n;
    size_t take = n < cap - *len ? n : cap - *len;
    memcpy(buf + *len, data, take);
    *len += take;
}

bool ext_run(const ext_tool_t *t, const cJSON *in, char *result, size_t rl)
{
    const vfs_entry_t *x = t->exe;
    char why[300];
    if (x->kind == VFS_EXEC_SCRIPT && !check_interpreter(x, why, sizeof(why))) {
        snprintf(result, rl, "%s", why);
        return false;
    }
    if (guard_offline() && t->network && !g_net_allowed) {
        snprintf(result, rl, "this tool needs internet access, which the user did not allow for this session. "
                             "Tell the user; it cannot be used until the agent is restarted and access is allowed.");
        return false;
    }
    /* Plan mode: the working directory is read-only for the tool anyway, but it may still act
     * elsewhere (send mail, call an API), so tools not declared read-only need a yes. */
    if (guard_plan() && !t->readonly &&
        !guard_confirm("Plan mode: run the agent tool %s? It is not marked read-only and may change "
                       "things outside this folder (APIs, messages).", t->name)) {
        snprintf(result, rl, "%s", GUARD_PLAN_REFUSAL);
        return false;
    }
    char *args = cJSON_PrintUnformatted(in);
    if (!args) {
        snprintf(result, rl, "out of memory");
        return false;
    }
    size_t alen = strlen(args);

    int pin[2], pout[2], perr[2];
    if (pipe2(pin, O_CLOEXEC) != 0) {
        free(args);
        snprintf(result, rl, "pipe failed: %s", strerror(errno));
        return false;
    }
    if (pipe2(pout, O_CLOEXEC) != 0) {
        close(pin[0]);
        close(pin[1]);
        free(args);
        snprintf(result, rl, "pipe failed: %s", strerror(errno));
        return false;
    }
    if (pipe2(perr, O_CLOEXEC) != 0) {
        close(pin[0]);
        close(pin[1]);
        close(pout[0]);
        close(pout[1]);
        free(args);
        snprintf(result, rl, "pipe failed: %s", strerror(errno));
        return false;
    }

    int timeout = t->timeout_s;
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        close(pin[0]);
        close(pin[1]);
        close(pout[0]);
        close(pout[1]);
        close(perr[0]);
        close(perr[1]);
        free(args);
        snprintf(result, rl, "fork failed: %s", strerror(errno));
        return false;
    }
    if (pid == 0) {
        setsid();
        dup2(pin[0], 0);
        dup2(pout[1], 1);
        dup2(perr[1], 2);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        /* Hand the tool a read-only handle to this agent's own executable as fd 3
         * (BOTCORE_RUNTIME_FD=3): tools that build agents need the runtime, and a
         * sandboxed child cannot open /proc/<parent>/exe itself. Opened before the
         * sandbox: Landlock may hide the executable's directory. Best effort. */
        int rt = open("/proc/self/exe", O_RDONLY);
        if (rt >= 0) {
            if (rt != 3) {
                dup2(rt, 3);
                close(rt);
            }
            setenv("BOTCORE_RUNTIME_FD", "3", 1);
        }
        if (chdir(guard_ctx()) != 0 || tool_sandbox_apply(timeout + 5, !guard_offline() || (t->network && g_net_allowed)) != 0) {
            _exit(126);
        }
        int fd = memfd_create(t->name, x->kind == VFS_EXEC_SCRIPT ? 0 : MFD_CLOEXEC);
        if (fd < 0) {
            const char m[] = "botcore: memfd_create failed; cannot start tool\n";
            (void)!write(2, m, sizeof(m) - 1);
            _exit(126);
        }
        size_t off = 0;
        while (off < x->len) {
            ssize_t w = write(fd, x->data + off, x->len - off);
            if (w < 0 && errno == EINTR) {
                continue;
            }
            if (w <= 0) {
                _exit(126);
            }
            off += (size_t)w;
        }
        fchmod(fd, 0700);
        char *argv[] = {(char *)t->name, NULL};
        fexecve(fd, argv, environ);
        const char m[] = "botcore: could not execute tool\n";
        (void)!write(2, m, sizeof(m) - 1);
        _exit(127);
    }
    close(pin[0]);
    close(pout[1]);
    close(perr[1]);
    int flags = fcntl(pin[1], F_GETFL);
    fcntl(pin[1], F_SETFL, flags | O_NONBLOCK);

    size_t ocap = rl > 400 ? rl - 400 : rl / 2, olen = 0, ototal = 0;
    char   ebuf[EXT_ERR_CAP];
    size_t elen = 0, etotal = 0, woff = 0;
    int    in_open = 1, out_open = 1, err_open = 1;
    int    st = 0, exited = 0, timed_out = 0, interrupted = 0;
    double deadline = tool_now_s() + timeout;
    char   chunk[4096];

    if (alen == 0) {
        close(pin[1]);
        in_open = 0;
    }
    while (out_open || err_open) {
        struct pollfd p[3];
        int np = 0, ii = -1, io = -1, ie = -1;
        if (in_open) {
            ii = np;
            p[np++] = (struct pollfd){pin[1], POLLOUT, 0};
        }
        if (out_open) {
            io = np;
            p[np++] = (struct pollfd){pout[0], POLLIN, 0};
        }
        if (err_open) {
            ie = np;
            p[np++] = (struct pollfd){perr[0], POLLIN, 0};
        }
        poll(p, (nfds_t)np, 100);
        if (ii >= 0 && (p[ii].revents & (POLLOUT | POLLERR | POLLHUP))) {
            ssize_t w = write(pin[1], args + woff, alen - woff);
            if (w > 0) {
                woff += (size_t)w;
            }
            if (woff >= alen || (w < 0 && errno != EAGAIN && errno != EINTR)) {
                close(pin[1]);
                in_open = 0;
            }
        }
        if (io >= 0 && (p[io].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(pout[0], chunk, sizeof(chunk));
            if (n > 0) {
                append(result, ocap, &olen, &ototal, chunk, (size_t)n);
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                out_open = 0;
            }
        }
        if (ie >= 0 && (p[ie].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(perr[0], chunk, sizeof(chunk));
            if (n > 0) {
                append(ebuf, EXT_ERR_CAP - 1, &elen, &etotal, chunk, (size_t)n);
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                err_open = 0;
            }
        }
        if (waitpid(pid, &st, WNOHANG) == pid) {
            exited = 1; /* do not wait for grandchildren holding the pipes open */
            for (int k = 0; k < 64; k++) {
                struct pollfd q[2] = {{pout[0], POLLIN, 0}, {perr[0], POLLIN, 0}};
                if (poll(q, 2, 0) <= 0) {
                    break;
                }
                if (q[0].revents & POLLIN) {
                    ssize_t n = read(pout[0], chunk, sizeof(chunk));
                    if (n > 0) {
                        append(result, ocap, &olen, &ototal, chunk, (size_t)n);
                    }
                }
                if (q[1].revents & POLLIN) {
                    ssize_t n = read(perr[0], chunk, sizeof(chunk));
                    if (n > 0) {
                        append(ebuf, EXT_ERR_CAP - 1, &elen, &etotal, chunk, (size_t)n);
                    }
                }
            }
            break;
        }
        if (term_interrupted()) {
            interrupted = 1;
            break;
        }
        if (tool_now_s() > deadline) {
            timed_out = 1;
            break;
        }
    }
    if (in_open) {
        close(pin[1]);
    }
    close(pout[0]);
    close(perr[0]);
    if (timed_out || interrupted) {
        kill(-pid, SIGKILL);
    }
    while (!exited && waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    kill(-pid, SIGKILL);
    free(args);

    result[olen] = '\0';
    ebuf[elen] = '\0';
    size_t room = rl - olen;
    char  *tail = result + olen;
    if (ototal > olen && room > 1) {
        int w = snprintf(tail, room, "\n[output truncated: showed %zu of %zu bytes]", olen, ototal);
        if (w > 0 && (size_t)w < room) {
            tail += w;
            room -= (size_t)w;
        }
    }

    if (interrupted) {
        snprintf(tail, room, "%s[interrupted by user; tool killed]", olen ? "\n" : "");
        return false;
    }
    if (timed_out) {
        snprintf(tail, room, "%s[tool '%s' timed out after %ds and was killed]", olen ? "\n" : "", t->name, timeout);
        return false;
    }
    if (WIFSIGNALED(st)) {
        snprintf(tail, room, "%s[tool '%s' crashed: signal %d]%s%s", olen ? "\n" : "", t->name, WTERMSIG(st),
                 elen ? "\nstderr: " : "", ebuf);
        return false;
    }
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (code != 0) {
        /* Prefer stderr as the error text; fall back to whatever stdout held. */
        if (elen > 0) {
            snprintf(result, rl, "%s\n[tool '%s' exited with code %d]", ebuf, t->name, code);
        } else {
            snprintf(tail, room, "%s[tool '%s' exited with code %d]", olen ? "\n" : "", t->name, code);
        }
        return false;
    }
    if (olen == 0) {
        snprintf(result, rl, "(no output)");
    }
    return true;
}
