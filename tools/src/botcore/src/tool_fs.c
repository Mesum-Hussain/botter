#define _GNU_SOURCE
#include "guard.h"
#include "tools.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Resolve + gate a path. 0 = go ahead (resolved path in out); -1 = refused,
 * `result` already holds the message for the LLM.
 */
static int access_path(const char *tool, const char *path, const char *verb, char out[PATH_MAX], char *result,
                       size_t rl)
{
    int inside = 0;
    if (guard_resolve(path, out, &inside) != 0) {
        snprintf(result, rl, "invalid path '%.200s'", path);
        return -1;
    }
    if (!inside && !guard_confirm("%s wants to %s a path OUTSIDE the working dir (%s):\n  %s", tool, verb,
                                  guard_ctx(), out)) {
        snprintf(result, rl, "denied by user: %s is outside the working directory", out);
        return -1;
    }
    return 0;
}

static const char *str_arg(const cJSON *in, const char *key)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(in, key);
    return cJSON_IsString(j) ? j->valuestring : NULL;
}

static int cmp_names(const struct dirent **a, const struct dirent **b)
{
    return strcmp((*a)->d_name, (*b)->d_name);
}

static int not_dots(const struct dirent *d)
{
    return strcmp(d->d_name, ".") != 0 && strcmp(d->d_name, "..") != 0;
}

static bool fs_list(const cJSON *in, char *result, size_t rl)
{
    const char *path = str_arg(in, "path");
    char res[PATH_MAX];

    if (!path || !*path) {
        path = ".";
    }
    if (access_path("fs_list", path, "list", res, result, rl) != 0) {
        return false;
    }
    struct dirent **names = NULL;
    int n = scandir(res, &names, not_dots, cmp_names);
    if (n < 0) {
        snprintf(result, rl, "cannot list '%s': %s", path, strerror(errno));
        return false;
    }
    size_t off = 0;
    if (n == 0) {
        snprintf(result, rl, "(empty directory)");
    }
    for (int i = 0; i < n; i++) {
        char full[PATH_MAX + 256];
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", res, names[i]->d_name);
        int w;
        if (lstat(full, &st) != 0) {
            w = snprintf(result + off, rl - off, "%s\n", names[i]->d_name);
        } else if (S_ISDIR(st.st_mode)) {
            w = snprintf(result + off, rl - off, "%s/\n", names[i]->d_name);
        } else if (S_ISLNK(st.st_mode)) {
            w = snprintf(result + off, rl - off, "%s@\n", names[i]->d_name);
        } else {
            w = snprintf(result + off, rl - off, "%s (%lld bytes)\n", names[i]->d_name, (long long)st.st_size);
        }
        if (w < 0 || (size_t)w >= rl - off) {
            snprintf(result + off, rl - off > 40 ? 40 : rl - off, "...(more entries not shown)\n");
            break;
        }
        off += (size_t)w;
    }
    for (int i = 0; i < n; i++) {
        free(names[i]);
    }
    free(names);
    return true;
}

static bool fs_read(const cJSON *in, char *result, size_t rl)
{
    const char *path = str_arg(in, "path");
    char res[PATH_MAX];

    if (!path) {
        snprintf(result, rl, "'path' (string) is required");
        return false;
    }
    if (access_path("fs_read", path, "read", res, result, rl) != 0) {
        return false;
    }
    const cJSON *oj = cJSON_GetObjectItemCaseSensitive(in, "offset");
    long long off = cJSON_IsNumber(oj) && oj->valuedouble > 0 ? (long long)oj->valuedouble : 0;

    int fd = open(res, O_RDONLY | O_NOCTTY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        snprintf(result, rl, "cannot open '%s': %s", path, strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        close(fd);
        snprintf(result, rl, "'%s' is a directory; use fs_list", path);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        snprintf(result, rl, "'%s' is not a regular file", path);
        return false;
    }
    size_t cap = rl > 300 ? rl - 300 : rl / 2; /* leave room for the trailer note */
    ssize_t got = pread(fd, result, cap, (off_t)off);
    close(fd);
    if (got < 0) {
        snprintf(result, rl, "read error on '%s': %s", path, strerror(errno));
        return false;
    }
    if (memchr(result, '\0', (size_t)got)) {
        snprintf(result, rl, "'%s' looks like a binary file (%lld bytes); not shown", path, (long long)st.st_size);
        return true;
    }
    size_t len = (size_t)got;
    long long end = off + (long long)len;
    if (end < st.st_size) { /* truncated: do not cut a UTF-8 sequence in half */
        while (len > 0 && ((unsigned char)result[len] & 0xC0) == 0x80) {
            len--;
        }
        end = off + (long long)len;
        result[len] = '\0';
        snprintf(result + len, rl - len,
                 "\n[truncated: showed bytes %lld-%lld of %lld; call fs_read again with offset=%lld for more]", off,
                 end, (long long)st.st_size, end);
    } else {
        result[len] = '\0';
        if (len == 0) {
            snprintf(result, rl, off ? "(offset is at or past end of file; size %lld bytes)" : "(empty file)",
                     (long long)st.st_size);
        }
    }
    return true;
}

static int mkdir_parents(char *path) /* path is modified temporarily */
{
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(path, 0777) != 0 && errno != EEXIST) {
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }
    return 0;
}

static bool fs_write(const cJSON *in, char *result, size_t rl)
{
    const char *path = str_arg(in, "path");
    const char *content = str_arg(in, "content");
    const cJSON *aj = cJSON_GetObjectItemCaseSensitive(in, "append");
    int append = cJSON_IsTrue(aj);
    char res[PATH_MAX];

    if (guard_plan()) {
        snprintf(result, rl, "%s", GUARD_PLAN_REFUSAL);
        return false;
    }
    if (!path || !content) {
        snprintf(result, rl, "'path' and 'content' (strings) are required");
        return false;
    }
    if (access_path("fs_write", path, "write", res, result, rl) != 0) {
        return false;
    }
    struct stat st;
    int exists = stat(res, &st) == 0;
    if (exists && !S_ISREG(st.st_mode)) {
        snprintf(result, rl, "'%s' exists and is not a regular file", path);
        return false;
    }
    if (exists && !append && st.st_size > 0 &&
        !guard_confirm_cat(GUARD_CAT_OVERWRITE, "fs_write would OVERWRITE existing file (%lld bytes):\n  %s",
                           (long long)st.st_size, res)) {
        snprintf(result, rl, "denied by user: not overwriting %s", res);
        return false;
    }
    if (mkdir_parents(res) != 0) {
        snprintf(result, rl, "cannot create parent directories of '%s': %s", path, strerror(errno));
        return false;
    }
    int fd = open(res, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC | (append ? O_APPEND : O_TRUNC), 0644);
    if (fd < 0) {
        snprintf(result, rl, "cannot open '%s' for writing: %s", path, strerror(errno));
        return false;
    }
    size_t n = strlen(content), done = 0;
    while (done < n) {
        ssize_t w = write(fd, content + done, n - done);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            snprintf(result, rl, "write error on '%s': %s", path, strerror(errno));
            close(fd);
            return false;
        }
        done += (size_t)w;
    }
    close(fd);
    snprintf(result, rl, "%s %zu bytes to %s%s", append ? "appended" : "wrote", n, res,
             exists && !append ? " (replaced existing file)" : "");
    return true;
}

const tool_t TOOLS_FS[] = {
    {"fs_list",
     "List the entries of a directory (default: the working directory). Directories end with '/'. "
     "Paths are relative to the working directory; paths outside it require user approval.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Directory path, default '.'\"}}}",
     fs_list},
    {"fs_read",
     "Read a text file. Output is capped (~32KB); for larger files use 'offset' to continue where the "
     "previous call stopped. Binary files are not shown.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
     "\"offset\":{\"type\":\"integer\",\"description\":\"Byte offset to start from, default 0\"}},"
     "\"required\":[\"path\"]}",
     fs_read},
    {"fs_write",
     "Create or overwrite a text file (parent directories are created). Overwriting an existing "
     "non-empty file asks the user for approval. Set append=true to add to the end instead.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"},"
     "\"append\":{\"type\":\"boolean\",\"description\":\"Append instead of overwrite, default false\"}},"
     "\"required\":[\"path\",\"content\"]}",
     fs_write},
};
const size_t TOOLS_FS_N = sizeof(TOOLS_FS) / sizeof(TOOLS_FS[0]);
