#define _DEFAULT_SOURCE

#include "dir.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "tfm_common.h"

/* Sort order: ".." first, then directories alphabetically, then files alphabetically. */
static int compare_entries(const void *a, const void *b)
{
    const DirEntryInfo *ea = a;
    const DirEntryInfo *eb = b;

    int a_is_parent = strcmp(ea->name, "..") == 0;
    int b_is_parent = strcmp(eb->name, "..") == 0;
    if (a_is_parent != b_is_parent) {
        return a_is_parent ? -1 : 1;
    }

    if (ea->is_dir != eb->is_dir) {
        return eb->is_dir - ea->is_dir;
    }

    return strcasecmp(ea->name, eb->name);
}

int dir_list(const char *path, DirEntryInfo **out_entries, size_t *out_count)
{
    /* Set to a defined empty state up front, in case a caller ignores the
     * return value on failure. */
    *out_entries = NULL;
    *out_count = 0;

    DIR *dp = opendir(path);
    if (dp == NULL) {
        return -1;
    }

    size_t capacity = 32;
    size_t count = 0;
    DirEntryInfo *entries = malloc(capacity * sizeof(DirEntryInfo));
    if (entries == NULL) {
        closedir(dp);
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        /* Skip "." but keep ".." visible for navigating up. */
        if (strcmp(entry->d_name, ".") == 0) {
            continue;
        }

        if (count == capacity) {
            capacity *= 2;
            DirEntryInfo *grown = realloc(entries, capacity * sizeof(DirEntryInfo));
            if (grown == NULL) {
                free(entries);
                closedir(dp);
                return -1;
            }
            entries = grown;
        }

        snprintf(entries[count].name, sizeof(entries[count].name), "%s", entry->d_name);

        if (entry->d_type == DT_DIR) {
            entries[count].is_dir = 1;
        } else if (entry->d_type == DT_UNKNOWN || entry->d_type == DT_LNK) {
            /* Some filesystems don't report a reliable type via readdir();
             * fall back to stat() (not lstat, so a symlink to a directory
             * is treated as a directory). */
            char full_path[PATH_MAX];
            snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
            struct stat st;
            entries[count].is_dir = (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode));
        } else {
            entries[count].is_dir = 0;
        }

        count++;
    }

    closedir(dp);

    qsort(entries, count, sizeof(DirEntryInfo), compare_entries);

    *out_entries = entries;
    *out_count = count;
    return 0;
}

void dir_list_free(DirEntryInfo *entries)
{
    free(entries);
}
