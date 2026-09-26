#define _POSIX_C_SOURCE 200809L

#include "input.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
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

void input_disable_raw_mode(void)
{
    if (g_raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw_mode_active = 0;
    }
}

void input_enable_raw_mode(void)
{
    if (g_raw_mode_active) {
        /* A second call would overwrite the already-raw g_orig_termios
         * below, so a later input_disable_raw_mode() would "restore"
         * raw mode instead of the real original settings. */
        return;
    }

    if (tcgetattr(STDIN_FILENO, &g_orig_termios) != 0) {
        /* Not a tty (e.g. piped/redirected stdin) - stay in cooked mode
         * rather than proceeding with a garbage g_orig_termios. */
        return;
    }

    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(unsigned int)(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        return;
    }
    g_raw_mode_active = 1;

    if (!g_atexit_registered) {
        atexit(input_disable_raw_mode);
        g_atexit_registered = 1;

        struct sigaction winch_action;
        memset(&winch_action, 0, sizeof(winch_action));
        winch_action.sa_handler = handle_sigwinch;
        sigemptyset(&winch_action.sa_mask);
        winch_action.sa_flags = 0; /* no SA_RESTART: read() must be interrupted */
        /* Failure deliberately not reported: this runs after
         * screen_enter_alt_screen(), so an stderr write would corrupt the
         * display. The (practically impossible) failure only means resizes
         * are ignored, which is survivable. */
        (void)sigaction(SIGWINCH, &winch_action, NULL);
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
    /* Home/End/PgUp/PgDn/Insert/Delete/Shift-Tab: two encodings each are
     * common across terminals (e.g. xterm sends "[H"/"[F" for Home/End,
     * many others send the VT-style "[1~"/"[4~"), so both are listed
     * rather than picking one and silently dropping the key on the
     * other family of terminal. */
    {"[H", KEY_HOME},
    {"[1~", KEY_HOME},
    {"OH", KEY_HOME},
    {"[F", KEY_END},
    {"[4~", KEY_END},
    {"OF", KEY_END},
    {"[5~", KEY_PGUP},
    {"[6~", KEY_PGDN},
    {"[2~", KEY_INSERT},
    {"[3~", KEY_DELETE},
    {"[Z", KEY_SHIFT_TAB},
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

/* Bytes consumed while collecting an escape sequence that turned out not
 * to match anything in ESC_SEQUENCES - replayed one at a time as plain
 * KEY_CHAR (or, if one happens to be another ESC, as the start of a new
 * sequence) by later input_read_key() calls, instead of being silently
 * discarded. Sized to match the seq[] collection buffer below. */
static char g_pending[32];
static int g_pending_len = 0;
static int g_pending_pos = 0;

/* Reads one byte, preferring an already-buffered pending byte (see
 * above) over a real read() - same ssize_t-style contract as read(): 1
 * on success, 0 on EOF, -1 on error (errno set, including EINTR). */
static ssize_t read_one_byte(char *out)
{
    if (g_pending_pos < g_pending_len) {
        *out = g_pending[g_pending_pos++];
        if (g_pending_pos == g_pending_len) {
            g_pending_pos = 0;
            g_pending_len = 0;
        }
        return 1;
    }
    return read(STDIN_FILENO, out, 1);
}

/* How long to wait for the next byte of a possible escape sequence
 * before concluding no more are coming (a bare ESC, or the sequence is
 * already complete). Long enough that a real terminal's own multi-byte
 * sequence - sent as one burst, but occasionally split across two reads
 * by a slow pty/ssh link - doesn't get cut short; short enough that a
 * human pressing plain ESC doesn't feel a delay before it registers. */
#define ESC_SEQ_TIMEOUT_MS 30

/* Waits up to ESC_SEQ_TIMEOUT_MS for one more sequence byte. Returns 1 if
 * a byte was read into *out, 0 if the wait timed out or hit real EOF
 * (either way: no more bytes are coming right now), -1 on a genuine
 * error other than EINTR (which is retried - a SIGWINCH firing mid-
 * sequence must not be misread as "sequence complete"). Pending bytes
 * (see g_pending above) are returned immediately with no wait, since
 * they're already in memory. */
static int read_seq_byte(char *out)
{
    for (;;) {
        if (g_pending_pos < g_pending_len) {
            return (int)read_one_byte(out);
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = ESC_SEQ_TIMEOUT_MS * 1000;

        int sel = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (sel == 0) {
            return 0; /* timeout: nothing else arrived in time */
        }

        ssize_t n = read_one_byte(out);
        if (n == 1) {
            return 1;
        }
        if (n == 0) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
}

KeyEvent input_read_key(void)
{
    KeyEvent event = {KEY_NONE, 0, 0};
    char c;

    ssize_t n = read_one_byte(&c);
    if (n != 1) {
        if (n == 0) {
            /* Real EOF (stdin closed, e.g. `tfm < /dev/null`) is
             * permanent - it will never produce a keystroke, so report it
             * distinctly instead of returning KEY_NONE forever, which used
             * to spin the main loop at 50Hz (20ms sleep/iteration) with no
             * way to ever exit. */
            event.type = KEY_EOF;
            return event;
        }
        /* n == -1: EINTR (e.g. a SIGWINCH during this read()) is
         * transient, not "no key yet" - the caller's next loop iteration
         * needs to see the resize flag and redraw immediately, so don't
         * add an artificial 20ms delay here. A genuine transient error
         * still gets one, to avoid a tight spin against a stuck fd. */
        if (errno != EINTR) {
            struct timespec ts = {0, 20000000L};
            nanosleep(&ts, NULL);
        }
        return event;
    }

    if (c != '\x1b') {
        event.type = KEY_CHAR;
        event.ch = c;
        return event;
    }

    /* Escape sequence: collect bytes one at a time with a short timeout
     * (see read_seq_byte()) instead of a single non-blocking drain, which
     * tells a bare ESC apart from a sequence whose later bytes haven't
     * arrived yet. 32 bytes so a longer unrecognized sequence (e.g.
     * ESC[27;5;65~) gets fully drained. */
    char seq[32];
    int seq_len = 0;

    while (seq_len < (int)sizeof(seq) - 1) {
        char next;
        if (read_seq_byte(&next) != 1) {
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

    if (seq_len == 0) {
        event.type = KEY_ESC;
        return event;
    }

    KeyType matched = lookup_esc_sequence(seq, seq_len);
    if (matched != KEY_UNKNOWN) {
        event.type = matched;
        return event;
    }

    /* Shift+F8 (permanent delete), xterm's "<code>;<modifier>~" encoding
     * with modifier 2 = Shift. Matched directly rather than via a general
     * modifier parser, since it's the only modified key tfm binds. */
    if (seq_len == 6 && memcmp(seq, "[19;2~", 6) == 0) {
        event.type = KEY_F8;
        event.shift = 1;
        return event;
    }

    /* Unrecognized sequence: report KEY_UNKNOWN, but queue the collected
     * bytes to be replayed as plain characters by the next
     * input_read_key() call(s) - discarding them would make e.g. an
     * unmatched Alt-chord's letter vanish. */
    int queued = seq_len;
    if (queued > (int)sizeof(g_pending)) {
        queued = (int)sizeof(g_pending);
    }
    memcpy(g_pending, seq, (size_t)queued);
    g_pending_len = queued;
    g_pending_pos = 0;

    event.type = KEY_UNKNOWN;
    return event;
}
