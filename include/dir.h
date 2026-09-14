#ifndef TFM_DIR_H
#define TFM_DIR_H

#include <stddef.h>

typedef struct {
    char name[256];
    int is_dir;
} DirEntryInfo;

/* Reads the contents of path into a malloc'd array of DirEntryInfo.
 * *out_entries must later be freed with dir_list_free(). Returns 0 on
 * success, -1 on error. */
int dir_list(const char *path, DirEntryInfo **out_entries, size_t *out_count);

/* Frees an array produced by dir_list(). */
void dir_list_free(DirEntryInfo *entries);

#endif
