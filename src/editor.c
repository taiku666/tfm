#include "editor.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"
#include "tfm_common.h"

/* $EDITOR may itself contain arguments (e.g. Omarchy's
 * "omarchy-launch-editor --inline"), so it is passed through unparsed
 * to shell_execute() rather than split and exec'd directly. */
int editor_open_cb(const char *path, void (*pump)(void *ctx), void *pump_ctx)
{
    if (path == NULL || path[0] == '\0') {
        return -1;
    }

    const char *editor = getenv("EDITOR");
    if (editor == NULL || editor[0] == '\0') {
        editor = "vi";
    }

    /* Single-quote path so spaces/special chars are safe; embedded '
     * is escaped with the standard '\'' POSIX trick. */
    char quoted[PATH_MAX * 4];
    size_t qi = 0;
    quoted[qi++] = '\'';
    const char *p = path;
    for (; *p != '\0' && qi < sizeof(quoted) - 5; p++) {
        if (*p == '\'') {
            quoted[qi++] = '\'';
            quoted[qi++] = '\\';
            quoted[qi++] = '\'';
            quoted[qi++] = '\'';
        } else {
            quoted[qi++] = *p;
        }
    }
    if (*p != '\0') {
        /* Buffer ran out before path did - continuing would silently
         * open a truncated, different path. */
        return -1;
    }
    quoted[qi++] = '\'';
    quoted[qi] = '\0';

    char command[sizeof(quoted) + 256];
    if ((size_t)snprintf(command, sizeof(command), "%s %s", editor, quoted) >= sizeof(command)) {
        return -1;
    }

    return shell_execute_cb(command, ".", pump, pump_ctx);
}

int editor_open(const char *path)
{
    return editor_open_cb(path, NULL, NULL);
}
