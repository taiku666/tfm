#ifndef TFM_COMMON_H
#define TFM_COMMON_H

#include <stddef.h>

/* Shared across both frontends' --version output. */
#define TFM_VERSION "0.5.0"

/* Some libc implementations (e.g. musl) don't define PATH_MAX in <limits.h>. */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Joins dir and name into "dir/name". Returns 1 if the result fit fully
 * in out (size out_size), 0 if truncated or if out/dir/name is NULL. */
int path_join(char *out, size_t out_size, const char *dir, const char *name);

/* Checks that name is safe to use as a single path COMPONENT (a new
 * file/directory name typed into a Rename or New-Folder prompt) rather
 * than a path - rejects NULL, "", ".", "..", and anything containing a
 * '/'. Without this, a typed name like "../../etc/passwd" or
 * "existingsub/newname" silently escapes the current directory when fed
 * through path_join() + rename()/mkdir(), which do no such validation
 * themselves (they're general-purpose path builders, used plenty of
 * places where a full path is exactly what's wanted). Returns 1 if safe,
 * 0 otherwise. */
int is_safe_path_component(const char *name);

/* Resolves a "cd [path]" command against current_dir (buffer of at least
 * PATH_MAX bytes), validating with realpath()+stat()+opendir(). Returns 1
 * and updates current_dir on success; returns 0 and writes a reason into
 * error_msg (if non-NULL) on failure. */
int builtin_cd(char *current_dir, const char *command, char *error_msg, size_t error_msg_size);

/* Number of trailing bytes in buf[0..len) that make up the last UTF-8
 * codepoint (1 for a plain ASCII byte, 2-4 for a multi-byte character) -
 * for a Backspace that removes one displayed character, not one byte.
 * Removing only buf[len-1] would leave a dangling continuation byte
 * (10xxxxxx) behind when the last character is multi-byte (e.g. an
 * umlaut). Shared by every "$ " command line/text-prompt Backspace
 * handler instead of each reimplementing (or missing) this. Returns 0 if
 * len == 0 (nothing to remove), so callers can subtract the result
 * unconditionally without a separate "len > 0" guard underflowing. */
size_t utf8_prev_char_len(const char *buf, size_t len);

/* Returns str unchanged, as a plain, unsized `const char *` at the call
 * site. Exists only to suppress a GCC -Wformat-truncation false positive
 * (seen building with -O2 -D_FORTIFY_SOURCE=2, invisible at -O0): when a
 * snprintf() source is provably bounded to fit the destination by an
 * invariant the analysis can't see - e.g. two buffers deliberately the
 * same declared size, or a pointer offset bounded by a preceding loop -
 * GCC still assumes the worst case if it can trace the source back to a
 * fixed-size array. Routing the source through this function first hides
 * that array from the analysis. __attribute__((noinline)) is load-
 * bearing, not decorative: a plain static function gets inlined at -O2,
 * letting the analysis see straight through it and reintroducing the
 * exact false positive this exists to remove. Only ever call this where
 * the surrounding code already guarantees the copy fits - it does
 * nothing to actually prevent truncation, and the reasoning for why the
 * copy is safe belongs in a comment at each call site, not here. */
__attribute__((noinline)) const char *unsized(const char *str);

#endif
