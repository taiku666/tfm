#ifndef TFM_SHELL_H
#define TFM_SHELL_H

/* Runs command via /bin/sh -c with cwd as working directory. Return
 * value is the child's normal exit code (0-255) on a clean exit; 126 if
 * chdir(cwd) failed before exec; 127 if execl() itself failed (e.g.
 * /bin/sh missing); 128+signal if the child was killed by a signal; -1
 * only for a failure in this function itself (fork()/waitpid() failed),
 * never for anything that happened inside the child.
 *
 * A NULL or empty command is deliberately treated as a successful no-op
 * (returns 0) rather than an error: it's the ordinary result of the user
 * pressing Enter on an empty shell bar, not a caller mistake, so no
 * dialog should appear. This differs on purpose from editor_open_cb()'s
 * NULL/empty-path contract (-1, a real error) - "run nothing" and "open
 * nothing" aren't the same kind of input: an empty command has an
 * obviously correct outcome (do nothing), while an empty file path has
 * no sensible file to fall back to opening. */
int shell_execute(const char *command, const char *cwd);

/* Like shell_execute(), but polls with WNOHANG and calls pump(pump_ctx)
 * repeatedly instead of blocking in waitpid() - for callers with their
 * own event loop (GTK) that would otherwise freeze while an external
 * command/editor runs. pump may be NULL, in which case this behaves
 * like shell_execute(). Same return-value contract as shell_execute(). */
int shell_execute_cb(const char *command, const char *cwd, void (*pump)(void *ctx), void *pump_ctx);

#endif
