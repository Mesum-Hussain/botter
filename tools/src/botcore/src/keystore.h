#ifndef BC_KEYSTORE_H
#define BC_KEYSTORE_H

#include <stddef.h>

/*
 * Session cache for API keys and the last connection, in the kernel keyring
 * (the user keyring): never written to disk, gone at reboot (or when the user's
 * last process exits, e.g. full logout). Entries are readable only by processes
 * that possess them (botcore and agents started from the same login session);
 * shell_exec / tool children are cut off from the keyring (ks_child_lock).
 * Names are prefixed "botcore:" (BOTCORE_KEYRING_NS=<ns> uses "<ns>:", for
 * tests); BOTCORE_NO_KEYRING=1 turns the cache off. Every call fails quietly when the kernel has
 * no keyring support: the connect flow then simply asks again.
 */

/* Copy the value of `name` into out (NUL-terminated). Returns its length, or -1. */
int  ks_get(const char *name, char *out, size_t cap);
/* Store (or replace) `name`. 0 on success. */
int  ks_put(const char *name, const char *val);
/* Remove every botcore entry. Returns how many were removed. */
int  ks_forget_all(void);

/* Child side, before exec: switch to a fresh anonymous session keyring, so the
 * child no longer possesses the cached entries (tool_sandbox_apply also blocks
 * the keyring syscalls with seccomp, so it cannot join the old one again). */
void ks_child_lock(void);

#endif
