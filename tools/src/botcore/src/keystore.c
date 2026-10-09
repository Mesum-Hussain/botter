#define _GNU_SOURCE
#include "keystore.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Kernel keyring ABI, spelled out (musl ships no linux/ headers). */
#define KEY_SPEC_USER_KEYRING       (-4)
#define KEYCTL_JOIN_SESSION_KEYRING 1
#define KEYCTL_DESCRIBE             6
#define KEYCTL_SETPERM              5
#define KEYCTL_SEARCH               10
#define KEYCTL_READ                 11
#define KEYCTL_INVALIDATE           21
/* Possessor: everything; user (same uid, not possessing): view only. */
#define KS_PERM 0x3f010000u

#define PREFIX_DEFAULT "botcore"

/* BOTCORE_NO_KEYRING=1 turns the cache off; BOTCORE_KEYRING_NS renames the prefix (tests). */
static const char *prefix(void)
{
    static char p[64];
    if (!p[0]) {
        const char *ns = getenv("BOTCORE_KEYRING_NS");
        snprintf(p, sizeof(p), "%s:", ns && *ns && !strchr(ns, ':') ? ns : PREFIX_DEFAULT);
    }
    return p;
}

static int off(void)
{
    const char *v = getenv("BOTCORE_NO_KEYRING");
    return v && *v && strcmp(v, "0") != 0;
}

static long kc(int op, unsigned long a, unsigned long b, unsigned long c, unsigned long d)
{
    return syscall(SYS_keyctl, op, a, b, c, d);
}

static void desc_for(const char *name, char *d, size_t cap)
{
    snprintf(d, cap, "%s%s", prefix(), name);
}

int ks_get(const char *name, char *out, size_t cap)
{
    char d[512];
    if (off()) {
        return -1;
    }
    desc_for(name, d, sizeof(d));
    long id = kc(KEYCTL_SEARCH, (unsigned long)KEY_SPEC_USER_KEYRING, (unsigned long)"user", (unsigned long)d, 0);
    if (id < 0 || cap < 2) {
        return -1;
    }
    long n = kc(KEYCTL_READ, (unsigned long)id, (unsigned long)out, cap - 1, 0);
    if (n < 0 || (size_t)n > cap - 1) { /* error, or does not fit */
        explicit_bzero(out, cap);
        return -1;
    }
    out[n] = '\0';
    return (int)n;
}

int ks_put(const char *name, const char *val)
{
    char d[512];
    if (off()) {
        return -1;
    }
    desc_for(name, d, sizeof(d));
    long id = syscall(SYS_add_key, "user", d, val, strlen(val), KEY_SPEC_USER_KEYRING);
    if (id < 0) {
        return -1;
    }
    /* add_key's default for "user" keys is already possessor-only; make it explicit. */
    kc(KEYCTL_SETPERM, (unsigned long)id, KS_PERM, 0, 0);
    return 0;
}

int ks_forget_all(void)
{
    int32_t ids[512];
    long n = kc(KEYCTL_READ, (unsigned long)KEY_SPEC_USER_KEYRING, (unsigned long)ids, sizeof(ids), 0);
    if (n < 0) {
        return 0;
    }
    if ((size_t)n > sizeof(ids)) {
        n = sizeof(ids);
    }
    int gone = 0;
    for (long i = 0; i < n / (long)sizeof(ids[0]); i++) {
        char info[600];
        long l = kc(KEYCTL_DESCRIBE, (unsigned long)(long)ids[i], (unsigned long)info, sizeof(info) - 1, 0);
        if (l <= 0 || (size_t)l >= sizeof(info)) {
            continue;
        }
        info[l] = '\0';
        /* "type;uid;gid;perm;description" */
        const char *desc = info;
        for (int f = 0; f < 4 && desc; f++) {
            desc = strchr(desc, ';');
            desc = desc ? desc + 1 : NULL;
        }
        if (strncmp(info, "user;", 5) == 0 && desc && strncmp(desc, prefix(), strlen(prefix())) == 0 &&
            kc(KEYCTL_INVALIDATE, (unsigned long)(long)ids[i], 0, 0, 0) == 0) {
            gone++;
        }
    }
    return gone;
}

void ks_child_lock(void)
{
    kc(KEYCTL_JOIN_SESSION_KEYRING, 0, 0, 0, 0); /* NULL name = new anonymous session keyring */
}
