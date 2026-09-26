#define _DEFAULT_SOURCE

#include "opener.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int opener_is_executable(const char *path)
{
    struct stat st;
    if (path == NULL || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return 0;
    }
    return access(path, X_OK) == 0;
}

int opener_open_default(const char *path, char *error_msg, size_t error_msg_size)
{
    if (path == NULL || path[0] == '\0') {
        if (error_msg != NULL) {
            snprintf(error_msg, error_msg_size, "No file to open");
        }
        return 0;
    }

    const char *slash = strrchr(path, '/');
    const char *name = slash != NULL ? slash + 1 : path;

    pid_t pid = fork();
    if (pid < 0) {
        if (error_msg != NULL) {
            snprintf(error_msg, error_msg_size, "Cannot open \"%s\": %s", name, strerror(errno));
        }
        return 0;
    }

    if (pid == 0) {
        /* The launched app inherits this session and these fds. Its own
         * session keeps it alive when tfm's terminal closes (no SIGHUP),
         * and /dev/null keeps its output from drawing over the TUI. */
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }
        execlp("gio", "gio", "open", "--", path, (char *)NULL);
        _exit(127); /* only reached if gio isn't installed */
    }

    int status = 0;
    pid_t result;
    do {
        result = waitpid(pid, &status, 0);
    } while (result == -1 && errno == EINTR);

    if (result == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 1;
    }

    if (error_msg != NULL) {
        if (result == pid && WIFEXITED(status) && WEXITSTATUS(status) == 127) {
            snprintf(error_msg, error_msg_size, "Cannot open files: 'gio' (GLib) is not installed");
        } else {
            /* gio's own reason went to /dev/null with the app's output;
             * a missing default application is by far the common case. */
            snprintf(error_msg, error_msg_size, "No application found to open \"%s\"", name);
        }
    }
    return 0;
}
