/* _POSIX_C_SOURCE for termios/tcgetattr, matching src/input.c's own
 * feature-test macro. */
#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "../include/input.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* input_read_key() always reads from the real STDIN_FILENO (0) - there's
 * no injection seam in input.c, and adding one would mean changing
 * production code just to make it testable. Instead, each test replaces
 * fd 0 with a pipe it controls: write the test's bytes, close the write
 * end (so anything past those bytes is a deterministic, immediate EOF
 * instead of a real wait), then dup2() the read end onto STDIN_FILENO.
 * EOF-after-known-bytes is a fine substitute for input.c's real 30ms
 * escape-sequence timeout here: read_seq_byte() treats "timed out" and
 * "hit EOF" identically (both just mean "no more bytes right now"), so
 * every test in this file resolves instantly instead of needing to wait
 * out a real timeout.
 *
 * input.c keeps its own escape-sequence "replay" state
 * (g_pending/g_pending_len/g_pending_pos) in file-static variables with
 * no reset hook exposed - so any test that provokes an unmatched/replayed
 * sequence must fully drain it via enough input_read_key() calls before
 * the test ends, or its leftover bytes would corrupt the NEXT test's
 * first read. Every such test below does this explicitly. */

static void set_stdin_bytes(const char *bytes, size_t len)
{
    int fds[2];
    if (pipe(fds) != 0) {
        fprintf(stderr, "pipe() failed: %s\n", strerror(errno));
        abort();
    }
    if (len > 0) {
        ssize_t written = write(fds[1], bytes, len);
        if (written < 0 || (size_t)written != len) {
            fprintf(stderr, "write() to test pipe failed or was short\n");
            abort();
        }
    }
    close(fds[1]); /* EOF right after these bytes - see comment above. */
    if (dup2(fds[0], STDIN_FILENO) < 0) {
        fprintf(stderr, "dup2() failed: %s\n", strerror(errno));
        abort();
    }
    close(fds[0]);
}

TEST(eof_immediately_returns_key_eof)
{
    set_stdin_bytes("", 0);
    KeyEvent ev = input_read_key();
    ASSERT_EQ(ev.type, KEY_EOF);
}

TEST(plain_ascii_char_is_key_char)
{
    set_stdin_bytes("x", 1);
    KeyEvent ev = input_read_key();
    ASSERT_EQ(ev.type, KEY_CHAR);
    ASSERT_EQ(ev.ch, 'x');
}

TEST(bare_escape_with_no_more_bytes_is_key_esc)
{
    set_stdin_bytes("\x1b", 1);
    KeyEvent ev = input_read_key();
    ASSERT_EQ(ev.type, KEY_ESC);
}

TEST(arrow_keys_up_down_left_right)
{
    set_stdin_bytes("\x1b[A\x1b[B\x1b[C\x1b[D", sizeof("\x1b[A\x1b[B\x1b[C\x1b[D") - 1);
    ASSERT_EQ(input_read_key().type, KEY_UP);
    ASSERT_EQ(input_read_key().type, KEY_DOWN);
    ASSERT_EQ(input_read_key().type, KEY_RIGHT);
    ASSERT_EQ(input_read_key().type, KEY_LEFT);
}

TEST(function_keys_vt_style_and_alternate_op_style)
{
    /* F1 has two real-world encodings (xterm's "OP" vs. the VT/linux-
     * console "[11~") - both must map to the same KeyType. */
    set_stdin_bytes("\x1b[11~\x1bOP\x1b[19~\x1b[24~", sizeof("\x1b[11~\x1bOP\x1b[19~\x1b[24~") - 1);
    ASSERT_EQ(input_read_key().type, KEY_F1);
    ASSERT_EQ(input_read_key().type, KEY_F1);
    ASSERT_EQ(input_read_key().type, KEY_F8);
    ASSERT_EQ(input_read_key().type, KEY_F12);
}

TEST(home_end_dual_encodings)
{
    /* Three real-world Home encodings, three real-world End encodings -
     * see ESC_SEQUENCES's own comment on why both families are listed. */
    set_stdin_bytes("\x1b[H\x1b[1~\x1bOH\x1b[F\x1b[4~\x1bOF",
                    sizeof("\x1b[H\x1b[1~\x1bOH\x1b[F\x1b[4~\x1bOF") - 1);
    ASSERT_EQ(input_read_key().type, KEY_HOME);
    ASSERT_EQ(input_read_key().type, KEY_HOME);
    ASSERT_EQ(input_read_key().type, KEY_HOME);
    ASSERT_EQ(input_read_key().type, KEY_END);
    ASSERT_EQ(input_read_key().type, KEY_END);
    ASSERT_EQ(input_read_key().type, KEY_END);
}

TEST(pgup_pgdn_insert_delete_shift_tab)
{
    set_stdin_bytes("\x1b[5~\x1b[6~\x1b[2~\x1b[3~\x1b[Z", sizeof("\x1b[5~\x1b[6~\x1b[2~\x1b[3~\x1b[Z") - 1);
    ASSERT_EQ(input_read_key().type, KEY_PGUP);
    ASSERT_EQ(input_read_key().type, KEY_PGDN);
    ASSERT_EQ(input_read_key().type, KEY_INSERT);
    ASSERT_EQ(input_read_key().type, KEY_DELETE);
    ASSERT_EQ(input_read_key().type, KEY_SHIFT_TAB);
}

TEST(shift_f8_modifier_sets_shift_flag_distinct_from_plain_f8)
{
    set_stdin_bytes("\x1b[19~\x1b[19;2~", sizeof("\x1b[19~\x1b[19;2~") - 1);
    KeyEvent plain = input_read_key();
    ASSERT_EQ(plain.type, KEY_F8);
    ASSERT_EQ(plain.shift, 0);

    KeyEvent shifted = input_read_key();
    ASSERT_EQ(shifted.type, KEY_F8);
    ASSERT_EQ(shifted.shift, 1);
}

TEST(unknown_sequence_is_replayed_byte_by_byte)
{
    /* "[9~" is not in ESC_SEQUENCES - a genuinely unrecognized sequence.
     * It must be reported as KEY_UNKNOWN once, then its raw bytes
     * replayed as plain characters one at a time on later calls, instead
     * of being silently dropped (which used to eat the start of the
     * user's next real keystroke too). */
    set_stdin_bytes("\x1b[9~", sizeof("\x1b[9~") - 1);
    KeyEvent unknown = input_read_key();
    ASSERT_EQ(unknown.type, KEY_UNKNOWN);

    KeyEvent b1 = input_read_key();
    ASSERT_EQ(b1.type, KEY_CHAR);
    ASSERT_EQ(b1.ch, '[');
    KeyEvent b2 = input_read_key();
    ASSERT_EQ(b2.type, KEY_CHAR);
    ASSERT_EQ(b2.ch, '9');
    KeyEvent b3 = input_read_key();
    ASSERT_EQ(b3.type, KEY_CHAR);
    ASSERT_EQ(b3.ch, '~');
    /* Pending queue is now fully drained - safe for the next test. */
}

TEST(sequence_stops_as_soon_as_matched_not_merging_next_key)
{
    /* Up arrow immediately followed by a real 'x' keystroke, sent as one
     * burst (as fast typing would deliver it) - the 'x' must surface as
     * its own KEY_CHAR, not get pulled into the arrow sequence or lost. */
    set_stdin_bytes("\x1b[Ax", sizeof("\x1b[Ax") - 1);
    ASSERT_EQ(input_read_key().type, KEY_UP);
    KeyEvent next = input_read_key();
    ASSERT_EQ(next.type, KEY_CHAR);
    ASSERT_EQ(next.ch, 'x');
}

TEST(overlong_unmatched_sequence_is_bounded_and_nothing_lost)
{
    /* ESC + 40 non-matching bytes: the collection buffer (seq[32], see
     * input.c) stops appending at 31 bytes regardless of how much more
     * keeps arriving, to guarantee termination against a hostile/garbled
     * stream. Every byte must still be accounted for afterward: the
     * first 31 come back as replayed KEY_CHARs from the pending queue,
     * and the remaining 9 are still sitting untouched on the real pipe,
     * read normally once the pending queue is empty, ending in a real
     * EOF - nothing silently discarded at the boundary. */
    char input[41];
    input[0] = '\x1b';
    memset(input + 1, '9', 40);
    set_stdin_bytes(input, sizeof(input));

    ASSERT_EQ(input_read_key().type, KEY_UNKNOWN);

    for (int i = 0; i < 31; i++) {
        KeyEvent ev = input_read_key();
        ASSERT_EQ(ev.type, KEY_CHAR);
        ASSERT_EQ(ev.ch, '9');
    }
    for (int i = 0; i < 9; i++) {
        KeyEvent ev = input_read_key();
        ASSERT_EQ(ev.type, KEY_CHAR);
        ASSERT_EQ(ev.ch, '9');
    }
    ASSERT_EQ(input_read_key().type, KEY_EOF);
}

TEST(enable_raw_mode_on_non_tty_stdin_is_a_safe_noop)
{
    /* input_enable_raw_mode()'s own comment: a non-tty stdin (a pipe,
     * here) must leave it in cooked mode rather than proceeding with
     * garbage terminal settings - confirmed by tcgetattr() on the same
     * fd still failing (ENOTTY) afterward, and that ordinary key reads
     * still work normally through the untouched pipe. */
    set_stdin_bytes("x", 1);
    input_enable_raw_mode();
    input_disable_raw_mode();

    struct termios attrs;
    ASSERT_EQ(tcgetattr(STDIN_FILENO, &attrs), -1);

    KeyEvent ev = input_read_key();
    ASSERT_EQ(ev.type, KEY_CHAR);
    ASSERT_EQ(ev.ch, 'x');
}

int main(void)
{
    TFM_RUN(eof_immediately_returns_key_eof);
    TFM_RUN(plain_ascii_char_is_key_char);
    TFM_RUN(bare_escape_with_no_more_bytes_is_key_esc);
    TFM_RUN(arrow_keys_up_down_left_right);
    TFM_RUN(function_keys_vt_style_and_alternate_op_style);
    TFM_RUN(home_end_dual_encodings);
    TFM_RUN(pgup_pgdn_insert_delete_shift_tab);
    TFM_RUN(shift_f8_modifier_sets_shift_flag_distinct_from_plain_f8);
    TFM_RUN(unknown_sequence_is_replayed_byte_by_byte);
    TFM_RUN(sequence_stops_as_soon_as_matched_not_merging_next_key);
    TFM_RUN(overlong_unmatched_sequence_is_bounded_and_nothing_lost);
    TFM_RUN(enable_raw_mode_on_non_tty_stdin_is_a_safe_noop);
    return TFM_SUMMARY();
}
