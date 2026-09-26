#ifndef TFM_OPENER_H
#define TFM_OPENER_H

#include <stddef.h>

/* What Enter does with a selected file, shared by both front-ends:
 * executables are run (after confirmation, by the caller), everything
 * else is handed to the desktop's default application for its type. */

/* 1 if path is a regular file (symlinks followed) the user may execute,
 * 0 otherwise - including directories, whose x bit means "searchable". */
int opener_is_executable(const char *path);

/* Opens path in its default application via `gio open`, which reads the
 * same mimeapps.list defaults as other file managers and starts
 * Terminal=true apps (e.g. nvim for text) in a new terminal window.
 * Returns 1 once gio has launched the app, 0 on failure with a
 * user-facing message in error_msg. Blocks only until gio exits, not
 * until the app does. */
int opener_open_default(const char *path, char *error_msg, size_t error_msg_size);

#endif
