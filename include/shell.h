#ifndef TFM_SHELL_H
#define TFM_SHELL_H

/* Runs command via /bin/sh -c with cwd as working directory. Returns
 * the command's exit code, or -1 on error (e.g. fork() failed). */
int shell_execute(const char *command, const char *cwd);

/* Like shell_execute(), but polls with WNOHANG and calls pump(pump_ctx)
 * repeatedly instead of blocking in waitpid() - for callers with their
 * own event loop (GTK) that would otherwise freeze while an external
 * command/editor runs. pump may be NULL, in which case this behaves
 * like shell_execute(). */
int shell_execute_cb(const char *command, const char *cwd, void (*pump)(void *ctx), void *pump_ctx);

#endif
