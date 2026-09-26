#define _DEFAULT_SOURCE

#include "dir.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <wchar.h>
#include <wctype.h>

#include "tfm_common.h"

/* Case-insensitive, locale-aware comparison of two UTF-8 filenames.
 * strcasecmp() only folds ASCII bytes, so "Übung" and "übung" would
 * compare as unrelated; this folds wide characters with towlower() and
 * collates with wcscoll(). Falls back to strcasecmp() if the multibyte
 * conversion fails (invalid UTF-8, no locale support). */
static int compare_names_locale_aware(const char *a, const char *b)
{
    wchar_t wa[300];
    wchar_t wb[300];
    size_t na = mbstowcs(wa, a, sizeof(wa) / sizeof(wa[0]) - 1);
    size_t nb = mbstowcs(wb, b, sizeof(wb) / sizeof(wb[0]) - 1);
    if (na == (size_t)-1 || nb == (size_t)-1) {
        return strcasecmp(a, b);
    }
    wa[na] = L'\0';
    wb[nb] = L'\0';
    for (size_t i = 0; i < na; i++) {
        wa[i] = (wchar_t)towlower((wint_t)wa[i]);
    }
    for (size_t i = 0; i < nb; i++) {
        wb[i] = (wchar_t)towlower((wint_t)wb[i]);
    }
    return wcscoll(wa, wb);
}

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

    return compare_names_locale_aware(ea->name, eb->name);
}

int dir_list(const char *path, DirEntryInfo **out_entries, size_t *out_count)
{
    /* Public API contract: out_entries/out_count are mandatory (every
     * caller passes real, non-NULL out-params, but the contract wasn't
     * ever actually enforced), and opendir(NULL) is undefined behavior,
     * not a clean ENOENT-style failure - guard explicitly instead of
     * relying on every future caller to check first. */
    if (out_entries == NULL || out_count == NULL) {
        return -1;
    }

    /* Set to a defined empty state up front, in case a caller ignores the
     * return value on failure. */
    *out_entries = NULL;
    *out_count = 0;

    if (path == NULL) {
        return -1;
    }

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
    /* errno is reset before every readdir(), not once before the loop:
     * the body's stat() can fail harmlessly (e.g. a dangling symlink),
     * and its stale errno would be mistaken for a readdir() error at
     * EOF. */
    while ((errno = 0, entry = readdir(dp)) != NULL) {
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
            struct stat st;
            entries[count].is_dir = path_join(full_path, sizeof(full_path), path, entry->d_name) &&
                                     stat(full_path, &st) == 0 && S_ISDIR(st.st_mode);
        } else {
            entries[count].is_dir = 0;
        }

        count++;
    }

    if (errno != 0) {
        /* readdir() returns NULL both at genuine EOF and on a mid-read
         * error (EIO on a flaky NFS/FUSE mount, directory removed mid-
         * iteration) - without the errno check, a real failure silently
         * looks like "done", returning success with a truncated listing. */
        free(entries);
        closedir(dp);
        *out_entries = NULL;
        *out_count = 0;
        return -1;
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
