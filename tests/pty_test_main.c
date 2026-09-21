/* Integration tests that spawn the real, compiled bin/tfm binary attached
 * to a pseudo-terminal (forkpty()) and drive it like a real user/terminal
 * would - signals, EOF, real keystrokes over a real pty - instead of
 * linking against tfm's source objects like every other tests/test_*.c
 * does. This is what lets it exercise things no unit test can: real raw-
 * mode terminal state actually getting set and restored, and the
 * shutdown-signal-during-splash race that was a real, previously-fixed
 * bug in this codebase (the main loop's shutdown-flag check used to only
 * run after the blocking read, so a signal arriving before the loop was
 * ever reached - e.g. during the ~2.5s splash animation - hung forever).
 *
 * Deliberately named pty_test_main.c, not test_pty_main.c or
 * test_main_pty.c: the Makefile's `unit-test`/`test`/`asan-test` targets
 * only pick up tests/test_*.c, and this file is intentionally NOT part of
 * that fast suite - it's slow (every spawn pays the ~2.5-3s splash cost)
 * and exercises a real compiled binary rather than instrumented source
 * objects, so it has its own `make test-pty` target instead. Best-effort:
 * skips (not fails) if pty allocation isn't available in this environment
 * (some sandboxes disallow /dev/ptmx), same spirit as `make lint` without
 * cppcheck or the EXDEV test in tests/test_fileops.c. */

#define _DEFAULT_SOURCE

#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static const char *tfm_binary_path(void)
{
    const char *p = getenv("TFM_BINARY");
    return (p != NULL && p[0] != '\0') ? p : "./bin/tfm";
}

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

/* Sleeps ms milliseconds while continuously draining master_fd (discarding
 * the bytes), instead of a plain sleep_ms(). tfm writes a lot of output
 * during splash_show()'s animation and the initial full-screen redraw
 * right after; a pty's kernel output buffer is finite (commonly 64KB),
 * so any unattended wait while tfm is running risks it blocking inside
 * write() once that fills - indistinguishable from a hang from the
 * outside, and it would never even reach the input_read_key() call a
 * test is waiting to deliver a keystroke/signal to. Every wait in this
 * file while tfm is alive uses this, never a plain sleep_ms(). master_fd
 * must already be O_NONBLOCK (spawn_tfm() sets this once). */
static void drain_for_ms(int master_fd, int ms)
{
    const int step_ms = 20;
    int elapsed = 0;
    char discard[4096];
    while (elapsed < ms) {
        while (read(master_fd, discard, sizeof(discard)) > 0) {
        }
        sleep_ms(step_ms);
        elapsed += step_ms;
    }
}

/* Spawns tfm_binary_path() attached to a fresh pty (forkpty()). Returns
 * the child pid and, via *master_fd_out, the pty master fd (write to it
 * to deliver keystrokes, tcgetattr() it to inspect the shared terminal
 * line settings) - or -1 if pty allocation itself failed (the caller
 * should treat this as "skip", not "fail", per this file's own header
 * comment. If stdin_from_dev_null is non-zero, the child's stdin is
 * replaced with /dev/null right after forkpty() (which otherwise wires
 * stdin/stdout/stderr all to the pty slave), simulating `tfm </dev/null`
 * while stdout/stderr stay on the pty so screen.c's terminal-size ioctls
 * still see a real terminal. */
static pid_t spawn_tfm(int *master_fd_out, int stdin_from_dev_null)
{
    int master_fd;
    pid_t pid = forkpty(&master_fd, NULL, NULL, NULL);
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        if (stdin_from_dev_null) {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }
        }
        execl(tfm_binary_path(), tfm_binary_path(), (char *)NULL);
        _exit(127); /* exec itself failed */
    }
    /* Non-blocking so wait_for_exit()'s drain loop never itself blocks
     * waiting for more output that may not come for a while (or ever,
     * once tfm is idling in its main loop waiting for a keystroke). */
    fcntl(master_fd, F_SETFL, O_NONBLOCK);
    *master_fd_out = master_fd;
    return pid;
}

/* Waits up to timeout_ms for pid to exit, polling with WNOHANG rather
 * than a plain blocking waitpid() so a hang-type regression fails this
 * test instead of wedging the whole suite forever. Returns 1 and fills
 * *status if it exited in time; 0 on timeout, after SIGKILLing and
 * reaping the child so no zombie is left behind either way.
 *
 * Also drains master_fd on every poll (discarding the bytes): tfm writes
 * a lot of output during splash_show()'s animation, and a pty's kernel
 * output buffer is finite (commonly 64KB) - nobody reading the master
 * side makes the child block inside write() once that buffer fills,
 * which looks exactly like a hang (this was tracked down by comparing
 * against a standalone reproduction that DID read master_fd and exited
 * fine in ~2.6s, versus one that didn't and never returned). master_fd
 * must already be O_NONBLOCK (set once by the caller after spawn_tfm()). */
static int wait_for_exit(pid_t pid, int master_fd, int timeout_ms, int *status)
{
    const int step_ms = 20;
    int elapsed = 0;
    char discard[4096];
    for (;;) {
        while (read(master_fd, discard, sizeof(discard)) > 0) {
        }

        pid_t r = waitpid(pid, status, WNOHANG);
        if (r == pid) {
            return 1;
        }
        if (r < 0) {
            return 0;
        }
        if (elapsed >= timeout_ms) {
            kill(pid, SIGKILL);
            waitpid(pid, status, 0);
            return 0;
        }
        sleep_ms(step_ms);
        elapsed += step_ms;
    }
}

/* Generous timeout for every wait below: splash_show() alone takes
 * ~2.5-3s (a fixed animation with no way to skip it), plus normal
 * process-teardown time - 6s leaves ample margin without letting a real
 * hang regression block the suite for long. */
#define EXIT_TIMEOUT_MS 6000

TEST(eof_on_stdin_quits_promptly_instead_of_spinning)
{
    int master_fd;
    pid_t pid = spawn_tfm(&master_fd, 1 /* stdin from /dev/null */);
    if (pid < 0) {
        fprintf(stderr, "    SKIP (pty allocation unavailable: %s)\n", strerror(errno));
        return;
    }

    int status = 0;
    int exited = wait_for_exit(pid, master_fd, EXIT_TIMEOUT_MS, &status);
    ASSERT_TRUE(exited);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    close(master_fd);
}

TEST(sigterm_during_splash_exits_promptly_and_restores_terminal)
{
    int master_fd;
    pid_t pid = spawn_tfm(&master_fd, 0 /* real interactive pty stdin */);
    if (pid < 0) {
        fprintf(stderr, "    SKIP (pty allocation unavailable: %s)\n", strerror(errno));
        return;
    }

    /* A pty's line-discipline settings are shared by the master and slave
     * ends (both describe the same underlying terminal) - reading them
     * from the master side, before tfm ever touches them, gives the
     * "cooked mode" baseline tfm's own raw-mode setup/teardown must
     * exactly restore afterward. */
    struct termios before;
    ASSERT_EQ(tcgetattr(master_fd, &before), 0);

    /* Deliberately early: fires well inside the ~2.5-3s splash window,
     * before the main loop (and its input_read_key() call) is ever
     * reached - exactly the previously-fixed race described in this
     * file's header comment. */
    drain_for_ms(master_fd, 300);
    ASSERT_EQ(kill(pid, SIGTERM), 0);

    int status = 0;
    int exited = wait_for_exit(pid, master_fd, EXIT_TIMEOUT_MS, &status);
    ASSERT_TRUE(exited);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    struct termios after;
    ASSERT_EQ(tcgetattr(master_fd, &after), 0);
    ASSERT_EQ(after.c_lflag & (tcflag_t)(ECHO | ICANON | ISIG),
              before.c_lflag & (tcflag_t)(ECHO | ICANON | ISIG));

    close(master_fd);
}

TEST(f10_keystroke_quits_cleanly_and_restores_terminal)
{
    int master_fd;
    pid_t pid = spawn_tfm(&master_fd, 0);
    if (pid < 0) {
        fprintf(stderr, "    SKIP (pty allocation unavailable: %s)\n", strerror(errno));
        return;
    }

    struct termios before;
    ASSERT_EQ(tcgetattr(master_fd, &before), 0);

    /* Past the splash animation and into the interactive main loop
     * before sending the real F10 (Quit) keystroke - VT-style "[21~",
     * same encoding tests/test_input.c verifies against a pipe; here
     * it travels through a real pty end to end. */
    drain_for_ms(master_fd, 3500);
    const char f10[] = "\x1b[21~";
    ssize_t written = write(master_fd, f10, sizeof(f10) - 1);
    ASSERT_EQ(written, (ssize_t)(sizeof(f10) - 1));

    int status = 0;
    int exited = wait_for_exit(pid, master_fd, EXIT_TIMEOUT_MS, &status);
    ASSERT_TRUE(exited);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    struct termios after;
    ASSERT_EQ(tcgetattr(master_fd, &after), 0);
    ASSERT_EQ(after.c_lflag & (tcflag_t)(ECHO | ICANON | ISIG),
              before.c_lflag & (tcflag_t)(ECHO | ICANON | ISIG));

    close(master_fd);
}

int main(void)
{
    TFM_RUN(eof_on_stdin_quits_promptly_instead_of_spinning);
    TFM_RUN(sigterm_during_splash_exits_promptly_and_restores_terminal);
    TFM_RUN(f10_keystroke_quits_cleanly_and_restores_terminal);
    return TFM_SUMMARY();
}
