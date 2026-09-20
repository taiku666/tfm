#define _DEFAULT_SOURCE

#include "tfm_common.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int path_join(char *out, size_t out_size, const char *dir, const char *name)
{
    /* Public shared helper, called from several places with a
     * caller-controlled dir/name - a NULL here would reach snprintf()'s
     * "%s" and either crash or (on a libc that tolerates it) print
     * "(null)" into a path used for a real filesystem operation. */
    if (out == NULL || dir == NULL || name == NULL) {
        return 0;
    }
    int n = snprintf(out, out_size, "%s/%s", dir, name);
    return n > 0 && (size_t)n < out_size;
}

int is_safe_path_component(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }
    return strchr(name, '/') == NULL;
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
        /* Every realpath() failure used to collapse into the same
         * generic "Directory not found", regardless of the real reason
         * (ENOENT: doesn't exist; EACCES: a parent component isn't
         * searchable; ENAMETOOLONG: a component too long; ELOOP: a
         * symlink cycle) - strerror() distinguishes them for the user
         * instead of always implying "just doesn't exist" for cases
         * that are actually a permissions or path problem. Captured
         * immediately after the failing call, before any other libc
         * call (even snprintf() below) could clobber errno. */
        int saved_errno = errno;
        if (error_msg != NULL) {
            snprintf(error_msg, error_msg_size, "Cannot cd to \"%s\": %s", raw_path,
                     strerror(saved_errno));
        }
        return 0;
    }

    struct stat st;
    if (stat(resolved, &st) != 0) {
        int saved_errno = errno;
        if (error_msg != NULL) {
            snprintf(error_msg, error_msg_size, "Cannot cd to \"%s\": %s", resolved,
                     strerror(saved_errno));
        }
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        /* stat() itself succeeded here - there is no errno to report,
         * "Not a directory" is already the precise, accurate reason. */
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
        /* Same reasoning as the realpath() case above - opendir() can
         * fail for reasons other than a plain permission denial (e.g.
         * ENOENT on a race, EMFILE if the process is out of file
         * descriptors), so report the real one instead of always saying
         * "Permission denied" regardless of the actual cause. */
        int saved_errno = errno;
        if (error_msg != NULL) {
            snprintf(error_msg, error_msg_size, "Cannot open \"%s\": %s", resolved,
                     strerror(saved_errno));
        }
        return 0;
    }
    closedir(dp);

    snprintf(current_dir, PATH_MAX, "%s", resolved);
    return 1;
}

size_t utf8_prev_char_len(const char *buf, size_t len)
{
    if (len == 0) {
        return 0;
    }
    size_t new_len = len - 1;
    size_t continuation_bytes = 0;
    /* UTF-8 continuation bytes are 10xxxxxx (0x80-0xBF); skip back over
     * any of those to reach the lead byte of the last codepoint. Capped
     * at 3 continuation bytes (a well-formed codepoint is at most 4 bytes
     * total) - without this cap, a buffer containing malformed UTF-8 (a
     * long run of orphaned continuation bytes with no valid lead byte
     * before them - not reachable via normal typing, only via a broken
     * terminal/IME or already-corrupt input) would walk all the way back
     * to index 0, and a single Backspace would delete the ENTIRE buffer
     * instead of one character. */
    while (new_len > 0 && continuation_bytes < 3 && ((unsigned char)buf[new_len] & 0xC0) == 0x80) {
        new_len--;
        continuation_bytes++;
    }
    /* Also verify the byte actually landed on is a valid UTF-8 lead byte
     * (0xxxxxxx, 110xxxxx, 1110xxxx, or 11110xxx) - malformed input could
     * still stop mid-sequence within the capped walk above. If not,
     * there's no well-formed codepoint to remove here at all; fall back
     * to deleting exactly the last byte rather than guessing further. */
    unsigned char lead = (unsigned char)buf[new_len];
    int is_valid_lead = (lead & 0x80) == 0x00 || (lead & 0xE0) == 0xC0 || (lead & 0xF0) == 0xE0 ||
                         (lead & 0xF8) == 0xF0;
    if (!is_valid_lead) {
        return 1;
    }
    return len - new_len;
}
