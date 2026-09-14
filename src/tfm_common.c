#include "tfm_common.h"

#include <stdio.h>

int path_join(char *out, size_t out_size, const char *dir, const char *name)
{
    int n = snprintf(out, out_size, "%s/%s", dir, name);
    return n > 0 && (size_t)n < out_size;
}
