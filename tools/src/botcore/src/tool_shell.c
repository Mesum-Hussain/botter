#define _GNU_SOURCE
#include "guard.h"
#include "term.h"
#include "tools.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define DEFAULT_TIMEOUT_S 30
#define MAX_TIMEOUT_S     600

static int g_isolation; /* NET_ISOLATION_*: how children are cut off from the network */

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

    /* botcore is not dumpable (key in RAM), which would make /proc/self/uid_map root-owned;
     * this child is about to exec, which resets dumpability anyway. */
    prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
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

/*
 * L5 fallback when user namespaces are blocked (e.g. Ubuntu's AppArmor userns
 * restriction): a seccomp filter that makes socket(AF_INET/AF_INET6/AF_PACKET)
 * fail with EACCES and disables io_uring (it can create sockets too). Unix
 * sockets keep working. Needs no_new_privs, so setuid programs (sudo) cannot
 * elevate in these children. Kernel ABI constants are spelled out because
 * musl ships no linux/ kernel headers.
 */
#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#endif
#ifndef PR_SET_SECCOMP
#define PR_SET_SECCOMP 22
#endif
#define SECCOMP_MODE_FILTER_     2
#define SECCOMP_RET_KILL_PROCESS 0x80000000u
#define SECCOMP_RET_ERRNO_       0x00050000u
#define SECCOMP_RET_ALLOW_       0x7fff0000u
#define AUDIT_ARCH_X86_64_       0xc000003eu
#define X32_SYSCALL_BIT          0x40000000u
#define NR_IO_URING_SETUP        425

struct sock_filter_ {
    uint16_t code;
    uint8_t  jt, jf;
    uint32_t k;
};
struct sock_fprog_ {
    unsigned short       len;
    struct sock_filter_ *filter;
};
struct seccomp_data_ {
    int      nr;
    uint32_t arch;
    uint64_t ip;
    uint64_t args[6];
};

#define BPF_LD_W_ABS 0x20 /* BPF_LD | BPF_W | BPF_ABS */
#define BPF_JEQ_K    0x15 /* BPF_JMP | BPF_JEQ | BPF_K */
#define BPF_JGE_K    0x35 /* BPF_JMP | BPF_JGE | BPF_K */
#define BPF_RET_K    0x06 /* BPF_RET | BPF_K */
#define STMT(c, k)       {(c), 0, 0, (k)}
#define JUMP(c, k, t, f) {(c), (t), (f), (k)}

static int enter_seccomp_offline(void)
{
    struct sock_filter_ f[] = {
        STMT(BPF_LD_W_ABS, offsetof(struct seccomp_data_, arch)),
        JUMP(BPF_JEQ_K, AUDIT_ARCH_X86_64_, 1, 0),
        STMT(BPF_RET_K, SECCOMP_RET_KILL_PROCESS), /* 32-bit ABI: would bypass the nr checks */
        STMT(BPF_LD_W_ABS, offsetof(struct seccomp_data_, nr)),
        JUMP(BPF_JGE_K, X32_SYSCALL_BIT, 0, 1),
        STMT(BPF_RET_K, SECCOMP_RET_KILL_PROCESS), /* x32 ABI */
        JUMP(BPF_JEQ_K, NR_IO_URING_SETUP, 0, 1),
        STMT(BPF_RET_K, SECCOMP_RET_ERRNO_ | ENOSYS),
        JUMP(BPF_JEQ_K, SYS_socket, 0, 6),
        STMT(BPF_LD_W_ABS, offsetof(struct seccomp_data_, args[0])), /* domain (low 32 bits) */
        JUMP(BPF_JEQ_K, AF_INET, 3, 0),
        JUMP(BPF_JEQ_K, AF_INET6, 2, 0),
        JUMP(BPF_JEQ_K, AF_PACKET, 1, 0),
        STMT(BPF_RET_K, SECCOMP_RET_ALLOW_),
        STMT(BPF_RET_K, SECCOMP_RET_ERRNO_ | EACCES),
        STMT(BPF_RET_K, SECCOMP_RET_ALLOW_),
    };
    struct sock_fprog_ prog = {(unsigned short)(sizeof(f) / sizeof(f[0])), f};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return -1;
    }
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER_, &prog, 0, 0) == 0 ? 0 : -1;
}

/* Run fn in a throwaway child; 1 if it succeeded. */
static int probe_child(int (*fn)(void))
{
    pid_t p = fork();
    if (p < 0) {
        return 0;
    }
    if (p == 0) {
        _exit(fn() == 0 ? 0 : 1);
    }
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int seccomp_blocks_inet(void)
{
    if (enter_seccomp_offline() != 0) {
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    return fd < 0 && errno == EACCES ? 0 : -1;
}

/*
 * L3: Landlock filesystem sandbox for children (kernel-enforced, unprivileged).
 * Write: the working directory, /tmp, /var/tmp, /dev, ~/.cache, ~/.npm, plus
 * paths the user approved for this one command. Read + execute: system
 * directories, $PATH entries (and the prefix above a .../bin entry, e.g.
 * ~/.local, but never $HOME or /), ~/.gitconfig. Everything else (the rest of
 * $HOME: ~/.ssh, ~/.config tokens, ...) is invisible. ABI constants spelled out
 * (musl has no linux/ headers); rights not known to the running kernel are dropped.
 */
#define NR_LANDLOCK_CREATE_RULESET 444
#define NR_LANDLOCK_ADD_RULE       445
#define NR_LANDLOCK_RESTRICT_SELF  446
#define LL_CREATE_RULESET_VERSION  1u
#define LL_RULE_PATH_BENEATH       1
#define LL_EXECUTE     (1ull << 0)
#define LL_WRITE_FILE  (1ull << 1)
#define LL_READ_FILE   (1ull << 2)
#define LL_READ_DIR    (1ull << 3)
#define LL_REFER       (1ull << 13) /* ABI 2 */
#define LL_TRUNCATE    (1ull << 14) /* ABI 3 */
#define LL_IOCTL_DEV   (1ull << 15) /* ABI 5 */
#define LL_FILE_RIGHTS (LL_EXECUTE | LL_WRITE_FILE | LL_READ_FILE | LL_TRUNCATE | LL_IOCTL_DEV)
#define LL_RO          (LL_EXECUTE | LL_READ_FILE | LL_READ_DIR)

struct ll_ruleset_attr {
    uint64_t handled_access_fs;
};
struct ll_path_beneath {
    uint64_t allowed_access;
    int32_t  parent_fd;
} __attribute__((packed));

static int         g_ll_abi;      /* 0 = Landlock unavailable */
static uint64_t    g_ll_all;      /* every fs right this kernel knows */
static rlim_t      g_nproc, g_data;
static const char *const *g_grants; /* set by shell_exec around fork() */
static int         g_ngrants, g_no_landlock;

static void ll_add(int rs, const char *path, uint64_t access)
{
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) {
        return; /* missing paths are simply not granted */
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && !S_ISDIR(st.st_mode)) {
        access &= LL_FILE_RIGHTS;
    }
    struct ll_path_beneath pb = {access & g_ll_all, fd};
    syscall(NR_LANDLOCK_ADD_RULE, rs, LL_RULE_PATH_BENEATH, &pb, 0);
    close(fd);
}

/* An approved path: grant it, or (if it does not exist yet) its deepest existing parent. */
static void ll_add_grant(int rs, const char *path)
{
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s", path);
    for (;;) {
        if (access(p, F_OK) == 0 || strcmp(p, "/") == 0) {
            break;
        }
        char *s = strrchr(p, '/');
        if (!s) {
            return;
        }
        if (s == p) {
            s[1] = '\0';
        } else {
            *s = '\0';
        }
    }
    if (strcmp(p, "/") != 0) {
        ll_add(rs, p, g_ll_all);
    }
}

static void ll_home(int rs, const char *home, const char *rel, uint64_t access)
{
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", home, rel);
    ll_add(rs, p, access);
}

static int enter_landlock(void)
{
    struct ll_ruleset_attr ra = {g_ll_all};
    int rs = (int)syscall(NR_LANDLOCK_CREATE_RULESET, &ra, sizeof(ra), 0);
    if (rs < 0) {
        return -1;
    }
    static const char *const ro[] = {"/usr", "/bin", "/sbin", "/lib", "/lib32", "/lib64", "/libx32", "/etc",
                                     "/opt", "/proc", "/sys", "/run", "/var", "/nix", "/snap", "/srv", NULL};
    for (int i = 0; ro[i]; i++) {
        ll_add(rs, ro[i], LL_RO);
    }
    static const char *const rw[] = {"/tmp", "/var/tmp", "/dev", NULL};
    for (int i = 0; rw[i]; i++) {
        ll_add(rs, rw[i], g_ll_all);
    }
    ll_add(rs, guard_ctx(), g_ll_all);
    const char *home = getenv("HOME");
    char hr[PATH_MAX] = "";
    if (home && *home && realpath(home, hr)) {
        ll_home(rs, hr, ".cache", g_ll_all);
        ll_home(rs, hr, ".npm", g_ll_all);
        ll_home(rs, hr, ".gitconfig", LL_READ_FILE);
    }
    const char *path = getenv("PATH");
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", path ? path : "");
    for (char *save = NULL, *d = strtok_r(buf, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
        char r[PATH_MAX];
        if (*d != '/' || !realpath(d, r)) {
            continue;
        }
        ll_add(rs, r, LL_RO);
        size_t n = strlen(r);
        if (n > 4 && strcmp(r + n - 4, "/bin") == 0) {
            r[n - 4] = '\0'; /* ~/.local/bin -> ~/.local (libs, site-packages) */
            if (r[0] && strcmp(r, hr) != 0) {
                ll_add(rs, r, LL_RO);
            }
        }
    }
    for (int i = 0; i < g_ngrants; i++) {
        ll_add_grant(rs, g_grants[i]);
    }
    int ok = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 && syscall(NR_LANDLOCK_RESTRICT_SELF, rs, 0) == 0;
    close(rs);
    return ok ? 0 : -1;
}

void tool_sandbox_grants(const char *const *paths, int n, int no_landlock)
{
    g_grants = paths;
    g_ngrants = n;
    g_no_landlock = no_landlock;
}

/* Number of processes the user runs now (for a fork-bomb brake above it). */
static long user_procs(void)
{
    DIR *d = opendir("/proc");
    long n = 0;
    struct dirent *e;
    uid_t me = getuid();
    while (d && (e = readdir(d))) {
        struct stat st;
        char p[300];
        if (!isdigit((unsigned char)e->d_name[0])) {
            continue;
        }
        snprintf(p, sizeof(p), "/proc/%s", e->d_name);
        if (stat(p, &st) == 0 && st.st_uid == me) {
            n++;
        }
    }
    if (d) {
        closedir(d);
    }
    return n;
}

int tool_sandbox_probe(void)
{
    long abi = syscall(NR_LANDLOCK_CREATE_RULESET, NULL, 0, LL_CREATE_RULESET_VERSION);
    g_ll_abi = abi > 0 ? (int)abi : 0;
    g_ll_all = g_ll_abi ? (1ull << 13) - 1 : 0; /* ABI 1: bits 0..12 */
    if (g_ll_abi >= 2) {
        g_ll_all |= LL_REFER;
    }
    if (g_ll_abi >= 3) {
        g_ll_all |= LL_TRUNCATE;
    }
    if (g_ll_abi >= 5) {
        g_ll_all |= LL_IOCTL_DEV;
    }
    g_nproc = (rlim_t)user_procs() + 1024;
    long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGESIZE);
    g_data = pages > 0 && psz > 0 ? (rlim_t)pages * (rlim_t)psz : RLIM_INFINITY;
    return g_ll_abi;
}

/* Probe once at startup: which kind of network isolation children get (NET_ISOLATION_*). */
int tool_shell_probe(void)
{
    if (probe_child(enter_empty_netns)) {
        g_isolation = NET_ISOLATION_NETNS;
    } else if (probe_child(seccomp_blocks_inet)) {
        g_isolation = NET_ISOLATION_SECCOMP;
    } else {
        g_isolation = NET_ISOLATION_NONE;
    }
    return g_isolation;
}

/*
 * Child-side sandbox shared by shell_exec and external tools: no core dumps,
 * file size, CPU, process-count and heap rlimits; no network (L5: empty netns,
 * else seccomp) unless `net` (an online agent, or a "network" tool the user
 * allowed in an offline one); then the Landlock filesystem sandbox (L3) unless
 * the user approved a privileged command. Returns 0, or -1 if an isolation the
 * probes found could not be applied (fail closed).
 */
int tool_sandbox_apply(int cpu_s, int net)
{
    struct rlimit rl0 = {0, 0};
    setrlimit(RLIMIT_CORE, &rl0);
    struct rlimit fs = {1UL << 30, 1UL << 30};
    setrlimit(RLIMIT_FSIZE, &fs);
    struct rlimit cpu = {(rlim_t)cpu_s, (rlim_t)cpu_s};
    setrlimit(RLIMIT_CPU, &cpu);
    struct rlimit cur;
    if (g_nproc && getrlimit(RLIMIT_NPROC, &cur) == 0 && (cur.rlim_cur == RLIM_INFINITY || cur.rlim_cur > g_nproc)) {
        struct rlimit np = {g_nproc, cur.rlim_max == RLIM_INFINITY || cur.rlim_max > g_nproc ? g_nproc : cur.rlim_max};
        setrlimit(RLIMIT_NPROC, &np);
    }
    if (g_data != RLIM_INFINITY) {
        struct rlimit dl = {g_data, g_data};
        setrlimit(RLIMIT_DATA, &dl);
    }
    if (!net && ((g_isolation == NET_ISOLATION_NETNS && enter_empty_netns() != 0) ||
                 (g_isolation == NET_ISOLATION_SECCOMP && enter_seccomp_offline() != 0))) {
        const char m[] = "botcore: could not isolate network; refusing to run\n";
        (void)!write(2, m, sizeof(m) - 1);
        return -1;
    }
    if (g_ll_abi && !g_no_landlock && enter_landlock() != 0) {
        const char m[] = "botcore: could not apply the Landlock filesystem sandbox; refusing to run\n";
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
    /* Paths the user just approved are writable for this run; approved sudo runs without Landlock. */
    const char *const *grants = NULL;
    int ngrants = cls == SH_CONFIRM ? guard_shell_grants(&grants) : 0;
    tool_sandbox_grants(grants, ngrants, cls == SH_CONFIRM && guard_shell_priv());
    fflush(NULL);
    pid_t pid = fork();
    if (pid != 0) {
        tool_sandbox_grants(NULL, 0, 0); /* parent only: the child keeps them for its sandbox */
    }
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
        if (tool_sandbox_apply(timeout + 5, !guard_offline()) != 0) {
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
     "and exit code. Whether it has network access is stated in the system prompt. It can write only "
     "inside the working directory, /tmp and package caches, and read system directories and $PATH tools; "
     "other paths fail with 'Permission denied' unless the command names them (the user is then asked to "
     "approve that path for this command). Destructive commands (rm, overwrite, "
     "git reset --hard, sudo, ...) and anything touching paths outside the working directory ask the "
     "user for approval first. Output is capped (~32KB). Default timeout 30s.",
     "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},"
     "\"timeout_s\":{\"type\":\"integer\",\"description\":\"Kill after this many seconds (1-600), default 30\"}},"
     "\"required\":[\"command\"]}",
     shell_exec},
};
const size_t TOOLS_SHELL_N = sizeof(TOOLS_SHELL) / sizeof(TOOLS_SHELL[0]);
