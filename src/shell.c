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
        /* 126 for a failed chdir, 127 for a failed exec - the POSIX shell
         * convention ("cannot execute" vs "command not found"), so the
         * caller can tell a child setup failure from the command's own
         * exit code. */
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
                /* Any other failure is realistically ECHILD: the child is
                 * already gone, so there's nothing left to wait for. */
                break;
            }
            if (result != 0) {
                if (WIFSTOPPED(status)) {
                    /* A stopped child (e.g. `kill -STOP`, or a command
                     * that suspends itself) would otherwise make this loop
                     * poll forever. tfm has no job control, so there's no
                     * sensible "leave it stopped" outcome - resume it and
                     * keep waiting for a real exit. */
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
        /* 128+signal, the shell $? convention, keeps "killed by a signal"
         * distinct from -1 ("fork()/waitpid() itself failed"). */
        return 128 + WTERMSIG(status);
    }
    return -1;
}

int shell_execute(const char *command, const char *cwd)
{
    return shell_execute_cb(command, cwd, NULL, NULL);
}
