#ifndef TFM_TEST_FS_HELPERS_H
#define TFM_TEST_FS_HELPERS_H

/* Shared filesystem test helpers, used by every tests/test_*.c that needs a
 * real, disposable /tmp tree - deliberately independent of the tfm code
 * under test (no reuse of fileops.c's own recursive-delete for cleanup, no
 * reuse of tfm_common.c's path_join for path building, etc.) so a bug in
 * the code being tested can't also corrupt the harness that's supposed to
 * catch it. Include this in one .c file per test binary, alongside test.h.
 *
 * Requires _DEFAULT_SOURCE (or _GNU_SOURCE) defined before any system
 * header, for mkdtemp()/setenv()/unsetenv()/strdup() - the including file
 * is responsible for that, same as it is for test.h. */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static __attribute__((unused)) void make_temp_dir(char *out, size_t size)
{
    snprintf(out, size, "/tmp/tfm-test-XXXXXX");
    if (mkdtemp(out) == NULL) {
        fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
        abort();
    }
}

/* out = dir + suffix. Every path built by these tests is short (a handful
 * of components under a fresh mkdtemp() directory), but destination
 * buffers are generously sized PATH_MAX like the rest of the codebase's
 * convention - which gcc's -Wformat-truncation can't see once dir has
 * itself been built up through more than one prior concatenation, since
 * its local flow analysis only tracks a fixed-size source array's real
 * content within the same function, not across a chain of them. Routing
 * through this function (dir/suffix become plain, unsized `const char *`
 * at the call site) avoids a wall of bogus warnings for concatenations
 * that are actually always well within bounds. Returns what snprintf()
 * returned, so a caller that wants to check for truncation still can. */
static __attribute__((unused)) int join_path(char *out, size_t out_size, const char *dir, const char *suffix)
{
    return snprintf(out, out_size, "%s%s", dir, suffix);
}

static __attribute__((unused)) void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "write_file(%s) failed: %s\n", path, strerror(errno));
        abort();
    }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static __attribute__((unused)) long read_file(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);
    return (long)n;
}

static __attribute__((unused)) int path_exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static __attribute__((unused)) int is_dir(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static __attribute__((unused)) void force_remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        /* chmod first: a permission test may have left this (or a
         * descendant) at 0000, which would otherwise block cleanup too. */
        chmod(path, 0700);
        DIR *dp = opendir(path);
        if (dp != NULL) {
            struct dirent *entry;
            while ((entry = readdir(dp)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                char child[PATH_MAX];
                snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
                force_remove_tree(child);
            }
            closedir(dp);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

/* --- single-variable environment save/restore --------------------------
 * For any test that needs to override an env var the code under test reads
 * directly via getenv() (e.g. $HOME, $XDG_DATA_HOME). Restored via
 * __attribute__((cleanup(...))) rather than plain code-at-the-end so a
 * failing ASSERT_*'s early `return;` still restores the real value instead
 * of leaking a deleted temp path into every later test in this same
 * process (see feedback_tfm_trash_undo_testing: forgetting this for even
 * one call risks e.g. trashing into the real, ambient $HOME/Trash). Use
 * one SavedEnvVar per variable overridden. */

typedef struct {
    const char *name;
    char *old_value;
} SavedEnvVar;

static __attribute__((unused)) void restore_env_var(SavedEnvVar *v)
{
    if (v->old_value != NULL) {
        setenv(v->name, v->old_value, 1);
        free(v->old_value);
    } else {
        unsetenv(v->name);
    }
}

static __attribute__((unused)) void save_and_set_env_var(SavedEnvVar *out, const char *name, const char *new_value)
{
    const char *old = getenv(name);
    out->name = name;
    out->old_value = (old != NULL) ? strdup(old) : NULL;
    setenv(name, new_value, 1);
}

#endif
