#define _POSIX_C_SOURCE 200809L

#include "shell.h"

#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

int shell_execute_cb(const char *command, const char *cwd, void (*pump)(void *ctx), void *pump_ctx)
{
    if (command == NULL || command[0] == '\0') {
        return 0;
    }

    pid_t pid = fork();

    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        /* Distinct exit codes so the caller (and shell_execute_cb()'s own
         * WIFEXITED/WEXITSTATUS translation below) can tell "couldn't set
         * up the child" apart from "the invoked command genuinely exited
         * 127" - both used to _exit(127), making them indistinguishable.
         * 126/127 follow the same convention POSIX shells use
         * ("cannot execute" vs "command not found"). */
        if (chdir(cwd) != 0) {
            _exit(126);
        }
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127); /* only reached if execl failed */
    }

    int status = 0;
    pid_t result;
    if (pump == NULL) {
        do {
            result = waitpid(pid, &status, 0);
        } while (result == -1 && errno == EINTR);
    } else {
        for (;;) {
            result = waitpid(pid, &status, WNOHANG);
            if (result == -1) {
                if (errno == EINTR) {
                    /* Interrupted by a signal (e.g. SIGWINCH, installed
                     * without SA_RESTART by input.c on purpose). Retry
                     * instead of treating this as the child exiting,
                     * which would leak a zombie. */
                    continue;
                }
                break;
            }
            if (result != 0) {
                break;
            }
            pump(pump_ctx);
            struct timespec ts = {0, 20000000L};
            nanosleep(&ts, NULL);
        }
    }

    if (result == -1) {
        return -1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        /* 128+signal (the same convention shells use for $?) instead of
         * -1: -1 previously meant both "killed by a signal" and "fork()/
         * waitpid() itself failed", two very different situations a
         * caller might want to react to differently. */
        return 128 + WTERMSIG(status);
    }
    return -1;
}

int shell_execute(const char *command, const char *cwd)
{
    return shell_execute_cb(command, cwd, NULL, NULL);
}
