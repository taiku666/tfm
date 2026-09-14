#ifndef TFM_EDITOR_H
#define TFM_EDITOR_H

/* Opens path (absolute or relative) blockingly in $EDITOR (falls back
 * to "vi" if unset/empty). Returns the editor's exit code, or -1 on
 * error (e.g. fork() failed). */
int editor_open(const char *path);

/* Like editor_open(), but pumps the caller's event loop while waiting
 * instead of blocking - see shell_execute_cb(). pump may be NULL, in
 * which case this behaves like editor_open(). */
int editor_open_cb(const char *path, void (*pump)(void *ctx), void *pump_ctx);

#endif
