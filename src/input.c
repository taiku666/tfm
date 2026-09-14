#define _POSIX_C_SOURCE 200809L

#include "input.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static struct termios g_orig_termios;
static int g_raw_mode_active = 0;
static int g_atexit_registered = 0;
static volatile sig_atomic_t g_resized = 0;

static void handle_sigwinch(int signum)
{
    (void)signum;
    g_resized = 1;
}

int input_consume_resize_flag(void)
{
    if (g_resized) {
        g_resized = 0;
        return 1;
    }
    return 0;
}

void input_wait_any_key(void)
{
    for (;;) {
        KeyEvent key = input_read_key();
        if (input_consume_resize_flag()) {
            continue;
        }
        if (key.type == KEY_NONE) {
            continue;
        }
        return;
    }
}

void input_disable_raw_mode(void)
{
    if (g_raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw_mode_active = 0;
    }
}

void input_enable_raw_mode(void)
{
    tcgetattr(STDIN_FILENO, &g_orig_termios);

    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(unsigned int)(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_raw_mode_active = 1;

    if (!g_atexit_registered) {
        atexit(input_disable_raw_mode);
        g_atexit_registered = 1;

        struct sigaction winch_action;
        memset(&winch_action, 0, sizeof(winch_action));
        winch_action.sa_handler = handle_sigwinch;
        sigemptyset(&winch_action.sa_mask);
        winch_action.sa_flags = 0; /* no SA_RESTART: read() must be interrupted */
        sigaction(SIGWINCH, &winch_action, NULL);
    }
}

typedef struct {
    const char *seq;
    KeyType type;
} EscSequence;

static const EscSequence ESC_SEQUENCES[] = {
    {"[11~", KEY_F1},
    {"[12~", KEY_F2},
    {"[13~", KEY_F3},
    {"[14~", KEY_F4},
    {"[15~", KEY_F5},
    {"[17~", KEY_F6},
    {"[18~", KEY_F7},
    {"[19~", KEY_F8},
    {"[20~", KEY_F9},
    {"[21~", KEY_F10},
    {"[23~", KEY_F11},
    {"[24~", KEY_F12},
    {"OP", KEY_F1},
    {"OQ", KEY_F2},
    {"OR", KEY_F3},
    {"OS", KEY_F4},
    {"[A", KEY_UP},
    {"[B", KEY_DOWN},
    {"[C", KEY_RIGHT},
    {"[D", KEY_LEFT},
};

static KeyType lookup_esc_sequence(const char *buf, int len)
{
    size_t n = sizeof(ESC_SEQUENCES) / sizeof(ESC_SEQUENCES[0]);
    for (size_t i = 0; i < n; i++) {
        if ((int)strlen(ESC_SEQUENCES[i].seq) == len &&
            strncmp(ESC_SEQUENCES[i].seq, buf, (size_t)len) == 0) {
            return ESC_SEQUENCES[i].type;
        }
    }
    return KEY_UNKNOWN;
}

KeyEvent input_read_key(void)
{
    KeyEvent event = {KEY_NONE, 0};
    char c;

    if (read(STDIN_FILENO, &c, 1) != 1) {
        /* stdin at EOF (e.g. `tfm < /dev/null`) makes read() return 0/-1
         * forever; without this pause the main loop would spin at 100%
         * CPU. */
        struct timespec ts = {0, 20000000L};
        nanosleep(&ts, NULL);
        return event;
    }

    if (c != '\x1b') {
        event.type = KEY_CHAR;
        event.ch = c;
        return event;
    }

    /* Escape sequence: remaining bytes are usually already buffered;
     * switch to non-blocking briefly to collect them. */
    char seq[8];
    int seq_len = 0;

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    while (seq_len < (int)sizeof(seq) - 1) {
        char next;
        ssize_t n = read(STDIN_FILENO, &next, 1);
        if (n != 1) {
            break;
        }
        seq[seq_len++] = next;
        seq[seq_len] = '\0';

        /* Stop as soon as the collected bytes exactly match a known
         * sequence, so fast typing doesn't merge in the next keypress's
         * bytes and lose both keys. */
        if (lookup_esc_sequence(seq, seq_len) != KEY_UNKNOWN) {
            break;
        }
    }

    fcntl(STDIN_FILENO, F_SETFL, flags);

    if (seq_len == 0) {
        event.type = KEY_ESC;
        return event;
    }

    event.type = lookup_esc_sequence(seq, seq_len);
    return event;
}
