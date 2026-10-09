#ifndef BC_GUARD_H
#define BC_GUARD_H

#include <limits.h>
#include <stddef.h>

/*
 * Safety policy (progress.md §6): free inside the context dir (cwd at launch);
 * ask the user (tty y/N) before touching anything outside it or doing
 * something destructive. LLM output is untrusted; these gates are the soft
 * layers (L1/L2). Kernel enforcement (Landlock, netns) is separate.
 */

void        guard_init(void);   /* records the context dir; call once at startup */
const char *guard_ctx(void);

/*
 * Network mode of this agent. Online (the default) leaves shell_exec and tools
 * on the host network; offline (agent.json {"offline": true}) blocks network
 * commands here and cuts children off in the kernel (tool_sandbox_apply).
 * The LLM connection itself is made by botcore and is never affected.
 */
void        guard_set_offline(int offline);
int         guard_offline(void);

/*
 * Plan / Build mode (like OpenCode). Build (the default) works as before. Plan
 * is read-only: fs_write and cron_set refuse, shell_exec and tools run with the
 * working directory read-only in Landlock (only /tmp-style scratch dirs stay
 * writable), privileged commands are refused, and agent tools not marked
 * "readonly": true in their descriptor ask the user first.
 */
void        guard_set_plan(int plan);
int         guard_plan(void);
#define GUARD_PLAN_REFUSAL "PLAN MODE: this would change things, which is not allowed while planning. " \
                           "Finish the plan and ask the user to switch to Build mode (Tab in the UI, or /build)."

/*
 * Resolve `path` (relative paths are relative to the context dir) to an
 * absolute, symlink-free path. The final components need not exist.
 * Returns 0 on success (*inside = 1 if within the context dir), -1 if the
 * path is unusable (empty, has ".." under a non-existent prefix, too long).
 */
int  guard_resolve(const char *path, char out[PATH_MAX], int *inside);

/* Ask a y/N question on the tty. Returns 1 only on an explicit yes; EOF/Ctrl-C/non-tty => 0. */
int  guard_confirm(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/*
 * Like guard_confirm but also offers "a" = always allow this category for the
 * rest of the session (RAM only). cat is 1..GUARD_NCAT-1.
 */
#define GUARD_CAT_OVERWRITE 1
#define GUARD_NCAT          2
int  guard_confirm_cat(int cat, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Shell command classification (soft gate). */
#define SH_OK       0
#define SH_CONFIRM  1  /* ask the user; `why` says what is risky */
#define SH_DENY     2  /* never run; `why` explains */
int  guard_shell_classify(const char *cmd, char *why, size_t why_len);

/*
 * Set by the last guard_shell_classify: outside-ctx paths the command names
 * (absolute, ~ expanded) and whether it escalates privileges (sudo, ...). Once
 * the user approves the command, the Landlock sandbox grants those paths for
 * that one run; an approved privileged command runs without Landlock.
 */
#define GUARD_MAX_GRANTS 16
int  guard_shell_grants(const char *const **paths); /* count */
int  guard_shell_priv(void);

#endif
