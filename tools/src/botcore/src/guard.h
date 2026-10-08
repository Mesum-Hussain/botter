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

#endif
