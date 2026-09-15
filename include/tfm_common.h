#ifndef TFM_COMMON_H
#define TFM_COMMON_H

#include <stddef.h>

/* Some libc implementations (e.g. musl) don't define PATH_MAX in <limits.h>. */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Joins dir and name into "dir/name". Returns 1 if the result fit fully
 * in out (size out_size), 0 if truncated. */
int path_join(char *out, size_t out_size, const char *dir, const char *name);

/* Resolves a "cd [path]" command against current_dir (buffer of at least
 * PATH_MAX bytes), validating with realpath()+stat()+opendir(). Returns 1
 * and updates current_dir on success; returns 0 and writes a reason into
 * error_msg (if non-NULL) on failure. */
int builtin_cd(char *current_dir, const char *command, char *error_msg, size_t error_msg_size);

#endif
