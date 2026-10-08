#define _GNU_SOURCE
#include "guard.h"
#include "term.h"
#include "tools.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define DEFAULT_TIMEOUT_S 30
#define MAX_TIMEOUT_S     600

static int g_netns_ok; /* children can be put in an empty network namespace */

static int write_file(const char *path, const char *s)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    ssize_t n = write(fd, s, strlen(s));
    close(fd);
    return n == (ssize_t)strlen(s) ? 0 : -1;
}

/*
 * L5: empty network namespace (loopback only), kernel-enforced, unprivileged.
 * Runs in a forked child just before exec. Maps our uid/gid to themselves so
 * file ownership and `id` stay sane.
 */
static int enter_empty_netns(void)
{
    char buf[64];
    uid_t u = geteuid();
    gid_t g = getegid();

    if (unshare(CLONE_NEWUSER | CLONE_NEWNET) != 0) {
        return -1;
    }
    write_file("/proc/self/setgroups", "deny");
    snprintf(buf, sizeof(buf), "%u %u 1", (unsigned)u, (unsigned)u);
    if (write_file("/proc/self/uid_map", buf) != 0) {
        return -1;
    }
    snprintf(buf, sizeof(buf), "%u %u 1", (unsigned)g, (unsigned)g);
    if (write_file("/proc/self/gid_map", buf) != 0) {
        return -1;
    }
    return 0;
}

/* Probe once at startup. Returns 1 if shell commands run network-isolated. */
int tool_shell_probe(void)
{
    pid_t p = fork();
    if (p < 0) {
        return 0;
    }
    if (p == 0) {
        _exit(enter_empty_netns() == 0 ? 0 : 1);
    }
    int st = 0;
    waitpid(p, &st, 0);
    g_netns_ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    return g_netns_ok;
}

/*
 * Child-side sandbox shared by shell_exec and external tools: no core dumps,
 * file size and CPU rlimits, empty network namespace (L5). Returns 0, or -1 if
 * the network namespace was expected to work but could not be entered (fail closed).
 */
int tool_sandbox_apply(int cpu_s)
{
    struct rlimit rl0 = {0, 0};
    setrlimit(RLIMIT_CORE, &rl0);
    struct rlimit fs = {1UL << 30, 1UL << 30};
    setrlimit(RLIMIT_FSIZE, &fs);
    struct rlimit cpu = {(rlim_t)cpu_s, (rlim_t)cpu_s};
    setrlimit(RLIMIT_CPU, &cpu);
    if (g_netns_ok && enter_empty_netns() != 0) {
        const char m[] = "botcore: could not isolate network; refusing to run\n";
        (void)!write(2, m, sizeof(m) - 1);
        return -1;
    }
    return 0;
}

double tool_now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static bool shell_exec(const cJSON *in, char *result, size_t rl)
{
    const cJSON *cj = cJSON_GetObjectItemCaseSensitive(in, "command");
    const cJSON *tj = cJSON_GetObjectItemCaseSensitive(in, "timeout_s");
    char why[600];

    if (!cJSON_IsString(cj) || !*cj->valuestring) {
        snprintf(result, rl, "'command' (non-empty string) is required");
        return false;
    }
    const char *cmd = cj->valuestring;
    int timeout = DEFAULT_TIMEOUT_S;
    if (cJSON_IsNumber(tj)) {
        timeout = (int)tj->valuedouble;
        if (timeout < 1) {
            timeout = 1;
        }
        if (timeout > MAX_TIMEOUT_S) {
            timeout = MAX_TIMEOUT_S;
        }
    }

    int cls = guard_shell_classify(cmd, why, sizeof(why));
    if (cls == SH_DENY) {
        snprintf(result, rl, "%s", why);
        return false;
    }
    if (cls == SH_CONFIRM &&
        !guard_confirm("shell_exec wants to run:\n  %s\nReason: %s", cmd, why)) {
        snprintf(result, rl, "denied by user: command was not run (%s)", why);
        return false;
    }

    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) != 0) {
        snprintf(result, rl, "pipe failed: %s", strerror(errno));
        return false;
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        snprintf(result, rl, "fork failed: %s", strerror(errno));
        return false;
    }
    if (pid == 0) {
        setsid();
        int dn = open("/dev/null", O_RDONLY);
        if (dn >= 0) {
            dup2(dn, 0);
        }
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        if (chdir(guard_ctx()) != 0) {
            _exit(126);
        }
        if (tool_sandbox_apply(timeout + 5) != 0) {
            _exit(126);
        }
        char *argv[] = {"sh", "-c", (char *)cmd, NULL};
        execve("/bin/sh", argv, environ);
        _exit(127);
    }
    close(pfd[1]);

    size_t cap = rl > 200 ? rl - 200 : rl / 2, len = 0;
    size_t total = 0;
    double deadline = tool_now_s() + timeout;
    int timed_out = 0, interrupted = 0;
    char chunk[4096];

    int st = 0, exited = 0;
    for (;;) {
        struct pollfd p = {pfd[0], POLLIN, 0};
        int r = poll(&p, 1, 100);
        if (r > 0) {
            ssize_t n = read(pfd[0], chunk, sizeof(chunk));
            if (n > 0) {
                total += (size_t)n;
                size_t take = (size_t)n < cap - len ? (size_t)n : cap - len;
                memcpy(result + len, chunk, take);
                len += take;
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                break; /* EOF: every writer has exited */
            }
        }
        /* The shell itself finished: take what is already buffered and stop, so
         * a background job (`cmd &`) cannot hold the call open. */
        if (waitpid(pid, &st, WNOHANG) == pid) {
            exited = 1;
            for (int i = 0; i < 64; i++) {
                struct pollfd q = {pfd[0], POLLIN, 0};
                if (poll(&q, 1, 0) <= 0) {
                    break;
                }
                ssize_t n = read(pfd[0], chunk, sizeof(chunk));
                if (n <= 0) {
                    break;
                }
                total += (size_t)n;
                size_t take = (size_t)n < cap - len ? (size_t)n : cap - len;
                memcpy(result + len, chunk, take);
                len += take;
            }
            break;
        }
        /* checked after every read too, so an output flood cannot dodge them */
        if (term_interrupted()) {
            interrupted = 1;
            break;
        }
        if (tool_now_s() > deadline) {
            timed_out = 1;
            break;
        }
    }
    close(pfd[0]);
    if (timed_out || interrupted) {
        kill(-pid, SIGKILL);
    }
    while (!exited && waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    kill(-pid, SIGKILL); /* stragglers (background jobs) die with the call */

    result[len] = '\0';
    char *tail = result + len;
    size_t room = rl - len;
    if (len > 0 && result[len - 1] != '\n' && room > 1) {
        *tail++ = '\n';
        room--;
    }
    if (total > len) {
        int w = snprintf(tail, room, "[output truncated: showed %zu of %zu bytes]\n", len, total);
        tail += w > 0 && (size_t)w < room ? (size_t)w : 0;
        room = rl - (size_t)(tail - result);
    }
    if (interrupted) {
        snprintf(tail, room, "[interrupted by user; command killed]");
    } else if (timed_out) {
        snprintf(tail, room, "[timed out after %ds; command killed. Raise timeout_s (max %d) if needed]", timeout,
                 MAX_TIMEOUT_S);
    } else if (WIFEXITED(st)) {
        snprintf(tail, room, "[exit code %d]", WEXITSTATUS(st));
    } else if (WIFSIGNALED(st)) {
        snprintf(tail, room, "[killed by signal %d]", WTERMSIG(st));
    }
    return true;
}

const tool_t TOOLS_SHELL[] = {
    {"shell_exec",
     "Run a shell command (/bin/sh -c) in the working directory and return its combined stdout+stderr "
     "and exit code. The environment is OFFLINE: network access is impossible and network tools (curl, "
     "git clone/pull/push, pip, npm, ssh, ...) are blocked. Destructive commands (rm, overwrite, "
     "git reset --hard, sudo, ...) and anything touching paths outside the working directory ask the "
     "user for approval first. Output is capped (~32KB). Default timeout 30s.",
     "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},"
     "\"timeout_s\":{\"type\":\"integer\",\"description\":\"Kill after this many seconds (1-600), default 30\"}},"
     "\"required\":[\"command\"]}",
     shell_exec},
};
const size_t TOOLS_SHELL_N = sizeof(TOOLS_SHELL) / sizeof(TOOLS_SHELL[0]);
