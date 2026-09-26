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

    char quoted[PATH_MAX * 4];
    if (!shell_quote(quoted, sizeof(quoted), path)) {
        /* Continuing with a truncated word would open a different file. */
        return -1;
    }

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
