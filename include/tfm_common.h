#ifndef TFM_COMMON_H
#define TFM_COMMON_H

#include <stddef.h>

/* Shared across both frontends' --version output. */
#define TFM_VERSION "0.7.0"

/* Some libc implementations (e.g. musl) don't define PATH_MAX in <limits.h>. */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Joins dir and name into "dir/name". Returns 1 if the result fit fully
 * in out (size out_size), 0 if truncated or if out/dir/name is NULL. */
int path_join(char *out, size_t out_size, const char *dir, const char *name);

/* Checks that name is safe as a single path COMPONENT (a name typed into
 * a Rename or New-Folder prompt): rejects NULL, "", ".", ".." and
 * anything containing '/'. path_join() does no such validation, so a
 * typed "../../etc/passwd" would otherwise escape the current directory.
 * Returns 1 if safe, 0 otherwise. */
int is_safe_path_component(const char *name);

/* Writes str into out as one single-quoted POSIX shell word, safe for
 * any filename: an embedded ' becomes '\'' (close, escaped quote,
 * reopen). Returns 1 on success, 0 if out/str is NULL or the result
 * didn't fit - a truncated word would name a different file. */
int shell_quote(char *out, size_t out_size, const char *str);

/* Resolves a "cd [path]" command against current_dir (buffer of at least
 * PATH_MAX bytes), validating with realpath()+stat()+opendir(). Returns 1
 * and updates current_dir on success; returns 0 and writes a reason into
 * error_msg (if non-NULL) on failure. */
int builtin_cd(char *current_dir, const char *command, char *error_msg, size_t error_msg_size);

/* Number of trailing bytes in buf[0..len) that make up the last UTF-8
 * codepoint (1-4), so Backspace removes one character rather than
 * leaving a dangling continuation byte of a multi-byte one. Returns 0
 * if len == 0, so callers can subtract the result unconditionally. */
size_t utf8_prev_char_len(const char *buf, size_t len);

/* Formats bytes for display, 1024-based: "512 B", "1.5 KB", "12.4 MB". */
void format_size(long long bytes, char *out, size_t out_size);

/* The marked-entries summary both front-ends show, e.g.
 * "3 marked (1 folder), 12.4 MB". bytes covers the marked files only
 * (folder sizes would need a full tree walk), so the size is left out
 * when only folders are marked. */
void format_mark_summary(size_t marked, size_t marked_dirs, long long bytes, char *out, size_t out_size);

/* Returns str unchanged, hiding its array size from GCC's
 * -Wformat-truncation analysis (at -O2 -D_FORTIFY_SOURCE=2), which
 * assumes the worst case when it can't see the invariant that keeps a
 * snprintf() copy in bounds. noinline is load-bearing: once inlined, the
 * analysis sees through it again. Use only where the copy provably fits
 * - it prevents no truncation itself. */
__attribute__((noinline)) const char *unsized(const char *str);

#endif
