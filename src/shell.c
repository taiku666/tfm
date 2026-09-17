#define _POSIX_C_SOURCE 200809L

#include "shell.h"

#include <errno.h>
#include <signal.h>
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
            result = waitpid(pid, &status, WNOHANG | WUNTRACED);
            if (result == -1) {
                if (errno == EINTR) {
                    /* Interrupted by a signal (e.g. SIGWINCH, installed
                     * without SA_RESTART by input.c on purpose). Retry
                     * instead of treating this as the child exiting,
                     * which would leak a zombie. */
                    continue;
                }
                /* Any other waitpid() failure on a pid we just fork()ed
                 * (realistically only ECHILD, meaning the child no
                 * longer exists to wait for - there is no signal handler
                 * anywhere in this process that could have reaped it
                 * behind our back, so this means it's already gone, not
                 * that a zombie was left behind). Nothing left to wait
                 * for either way. */
                break;
            }
            if (result != 0) {
                if (WIFSTOPPED(status)) {
                    /* WUNTRACED above makes a stopped child (e.g. an
                     * external `kill -STOP <pid>`, or a command that
                     * suspends itself) visible instead of invisible to
                     * WNOHANG - without it, this loop would just keep
                     * polling and pumping forever with no indication
                     * anything unusual happened, since waitpid() never
                     * reports a state change for a merely-stopped child.
                     * tfm has no job-control model (no fg/bg for the
                     * shell bar), so there is no sensible "leave it
                     * stopped" outcome here - resume it and keep waiting
                     * for a real exit, same as a shell resuming a
                     * background job that gets suspended by the
                     * terminal driver. */
                    kill(pid, SIGCONT);
                    pump(pump_ctx);
                    struct timespec ts = {0, 20000000L};
                    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
                        /* Retry with the remaining time instead of
                         * dropping it and looping back to waitpid()
                         * immediately - a frequent signal source (e.g.
                         * repeated resizes) would otherwise turn this
                         * into a tight spin instead of the intended
                         * ~50Hz poll rate. */
                    }
                    continue;
                }
                break;
            }
            pump(pump_ctx);
            struct timespec ts = {0, 20000000L};
            while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
                /* See the matching comment in the WIFSTOPPED branch above. */
            }
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
