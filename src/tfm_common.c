#define _DEFAULT_SOURCE

#include "tfm_common.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int path_join(char *out, size_t out_size, const char *dir, const char *name)
{
    int n = snprintf(out, out_size, "%s/%s", dir, name);
    return n > 0 && (size_t)n < out_size;
}

int builtin_cd(char *current_dir, const char *command, char *error_msg, size_t error_msg_size)
{
    /* Public shared helper - don't rely on callers pre-checking, since a
     * short/non-"cd" command would read command+2 out of bounds. */
    if (current_dir == NULL || command == NULL || strlen(command) < 2 ||
        strncmp(command, "cd", 2) != 0) {
        if (error_msg != NULL) {
            snprintf(error_msg, error_msg_size, "Not a cd command");
        }
        return 0;
    }

    const char *arg = command + 2;
    while (*arg == ' ') {
        arg++;
    }

    /* Checked explicitly (not just left to realpath() to fail on): a
     * silently truncated raw_path can still be a valid, existing,
     * completely unrelated directory - realpath() would then happily
     * resolve and cd into the WRONG place instead of erroring. */
    char raw_path[PATH_MAX];
    int raw_path_len;
    if (*arg == '\0') {
        const char *home = getenv("HOME");
        raw_path_len = snprintf(raw_path, sizeof(raw_path), "%s", home != NULL ? home : "/");
    } else if (arg[0] == '/') {
        raw_path_len = snprintf(raw_path, sizeof(raw_path), "%s", arg);
    } else {
        raw_path_len = snprintf(raw_path, sizeof(raw_path), "%s/%s", current_dir, arg);
    }
    if (raw_path_len < 0 || (size_t)raw_path_len >= sizeof(raw_path)) {
        if (error_msg != NULL) {
            snprintf(error_msg, error_msg_size, "Path too long");
        }
        return 0;
    }

    char resolved[PATH_MAX];
    if (realpath(raw_path, resolved) == NULL) {
        if (error_msg != NULL) {
            snprintf(error_msg, error_msg_size, "Directory not found");
        }
        return 0;
    }

    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) {
        if (error_msg != NULL) {
            snprintf(error_msg, error_msg_size, "Not a directory");
        }
        return 0;
    }

    /* Also verify the directory is actually readable (e.g. root-owned
     * systemd-private-* dirs in /tmp look like directories but aren't
     * openable by normal users) to avoid landing in an empty, unusable
     * panel. */
    DIR *dp = opendir(resolved);
    if (dp == NULL) {
        if (error_msg != NULL) {
            snprintf(error_msg, error_msg_size, "Permission denied for this directory");
        }
        return 0;
    }
    closedir(dp);

    snprintf(current_dir, PATH_MAX, "%s", resolved);
    return 1;
}
