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

#endif
