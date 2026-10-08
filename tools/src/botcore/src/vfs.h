#ifndef BC_VFS_H
#define BC_VFS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Read-only embedded file table = the agent pack appended to this executable
 * (manifest.md, agent.md, skills/, tools/doc/, tools/bin/). See blob.h for the
 * on-disk format. Entries are sorted by path (strcmp order) and every blob is
 * followed by a NUL byte that is not counted in `len`.
 */
enum {
    VFS_DATA        = 0, /* plain file (markdown, json) */
    VFS_EXEC_ELF    = 1, /* tool: native executable */
    VFS_EXEC_SCRIPT = 2, /* tool: script with a #! line */
};

typedef struct {
    const char *path; /* relative, '/'-separated, no leading '/' */
    const char *data;
    size_t      len;
    int         kind;
} vfs_entry_t;

/*
 * Locate and map the pack appended to /proc/self/exe. Call once at startup.
 * Returns 0 when there is no pack (empty VFS) or it loaded fine; -1 when a
 * pack is present but corrupt (vfs_error() says why).
 */
int                vfs_init(void);
const char        *vfs_error(void);

const vfs_entry_t *vfs_table(size_t *count);
size_t             vfs_count(void);
const vfs_entry_t *vfs_find(const char *path); /* exact match, bsearch */

#endif
