/* _DEFAULT_SOURCE for mkdtemp(), setenv()/unsetenv(), strdup() (used by
 * tests/test_fs_helpers.h). */
#define _DEFAULT_SOURCE

#include "test.h"
#include "test_fs_helpers.h"
#include "../include/tfm_common.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- path_join ------------------------------------------------------- */

TEST(path_join_basic)
{
    char out[64];
    ASSERT_EQ(path_join(out, sizeof(out), "a", "b"), 1);
    ASSERT_STR_EQ(out, "a/b");
}

TEST(path_join_rejects_null_args)
{
    char out[64];
    ASSERT_EQ(path_join(NULL, sizeof(out), "a", "b"), 0);
    ASSERT_EQ(path_join(out, sizeof(out), NULL, "b"), 0);
    ASSERT_EQ(path_join(out, sizeof(out), "a", NULL), 0);
}

TEST(path_join_reports_truncation)
{
    /* "a/bbbb" needs 7 bytes including the NUL; a 4-byte buffer can't
     * hold it - path_join() must say so via its return value instead of
     * silently handing back a truncated (and possibly still valid-
     * looking, wrong) path. */
    char out[4];
    ASSERT_EQ(path_join(out, sizeof(out), "a", "bbbb"), 0);
}

/* --- is_safe_path_component -------------------------------------------
 * Guards Rename/New-Folder prompts against escaping the current directory
 * (see tfm_common.h's comment) - every case here is a concrete input a
 * user could type into one of those prompts. */

TEST(is_safe_path_component_accepts_ordinary_names)
{
    ASSERT_TRUE(is_safe_path_component("notes.txt"));
    ASSERT_TRUE(is_safe_path_component("a"));
    /* A leading dot (hidden file) is fine - only the exact strings "."
     * and ".." are rejected, not anything merely starting with a dot. */
    ASSERT_TRUE(is_safe_path_component(".hidden"));
    /* Same for a name that merely starts with "..", as opposed to being
     * exactly "..". */
    ASSERT_TRUE(is_safe_path_component("..backup"));
}

TEST(is_safe_path_component_rejects_dangerous_names)
{
    ASSERT_FALSE(is_safe_path_component(NULL));
    ASSERT_FALSE(is_safe_path_component(""));
    ASSERT_FALSE(is_safe_path_component("."));
    ASSERT_FALSE(is_safe_path_component(".."));
    /* Any embedded '/' turns a component into a path, which is exactly
     * the "existingsub/newname" / "../../etc/passwd" escape the function
     * exists to block. */
    ASSERT_FALSE(is_safe_path_component("sub/name"));
    ASSERT_FALSE(is_safe_path_component("../escape"));
    ASSERT_FALSE(is_safe_path_component("/absolute"));
}

/* --- builtin_cd ---------------------------------------------------------
 * current_dir must be a PATH_MAX-sized buffer per tfm_common.h. */

TEST(builtin_cd_rejects_non_cd_command)
{
    char current_dir[PATH_MAX] = "/tmp";
    char error_msg[256] = "";
    ASSERT_EQ(builtin_cd(current_dir, "ls", error_msg, sizeof(error_msg)), 0);
    ASSERT_STR_EQ(error_msg, "Not a cd command");
    /* current_dir must be left untouched on rejection. */
    ASSERT_STR_EQ(current_dir, "/tmp");
}

TEST(builtin_cd_rejects_too_short_command)
{
    /* strlen("c") < 2 - must not read command[2] out of bounds (the
     * exact bug tfm_common.c's own comment calls out). */
    char current_dir[PATH_MAX] = "/tmp";
    char error_msg[256] = "";
    ASSERT_EQ(builtin_cd(current_dir, "c", error_msg, sizeof(error_msg)), 0);
    ASSERT_STR_EQ(error_msg, "Not a cd command");
}

TEST(builtin_cd_absolute_path)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char target[PATH_MAX], command[PATH_MAX];
    join_path(target, sizeof(target), base, "/sub");
    ASSERT_EQ(mkdir(target, 0755), 0);
    join_path(command, sizeof(command), "cd ", target);

    char current_dir[PATH_MAX] = "/tmp";
    char error_msg[256] = "";
    ASSERT_EQ(builtin_cd(current_dir, command, error_msg, sizeof(error_msg)), 1);
    ASSERT_STR_EQ(current_dir, target);

    force_remove_tree(base);
}

TEST(builtin_cd_relative_path)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char target[PATH_MAX];
    join_path(target, sizeof(target), base, "/sub");
    ASSERT_EQ(mkdir(target, 0755), 0);

    char current_dir[PATH_MAX];
    snprintf(current_dir, sizeof(current_dir), "%s", base);
    char error_msg[256] = "";
    ASSERT_EQ(builtin_cd(current_dir, "cd sub", error_msg, sizeof(error_msg)), 1);
    ASSERT_STR_EQ(current_dir, target);

    force_remove_tree(base);
}

TEST(builtin_cd_no_arg_goes_home)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char home[PATH_MAX];
    join_path(home, sizeof(home), base, "/home");
    ASSERT_EQ(mkdir(home, 0755), 0);

    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_home;
    save_and_set_env_var(&saved_home, "HOME", home);

    char current_dir[PATH_MAX] = "/tmp";
    char error_msg[256] = "";
    ASSERT_EQ(builtin_cd(current_dir, "cd", error_msg, sizeof(error_msg)), 1);
    ASSERT_STR_EQ(current_dir, home);

    force_remove_tree(base);
}

TEST(builtin_cd_nonexistent_path_reports_reason)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char missing[PATH_MAX], command[PATH_MAX];
    join_path(missing, sizeof(missing), base, "/does-not-exist");
    join_path(command, sizeof(command), "cd ", missing);

    char current_dir[PATH_MAX] = "/tmp";
    char error_msg[256] = "";
    ASSERT_EQ(builtin_cd(current_dir, command, error_msg, sizeof(error_msg)), 0);
    /* Must not be the old generic "Directory not found" - the real
     * strerror() reason (e.g. "No such file or directory") belongs in
     * the message, and current_dir must be left untouched. */
    ASSERT_TRUE(strstr(error_msg, "Cannot cd to") != NULL);
    ASSERT_STR_EQ(current_dir, "/tmp");

    force_remove_tree(base);
}

TEST(builtin_cd_rejects_a_file)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char file[PATH_MAX], command[PATH_MAX];
    join_path(file, sizeof(file), base, "/plain.txt");
    write_file(file, "not a directory");
    join_path(command, sizeof(command), "cd ", file);

    char current_dir[PATH_MAX] = "/tmp";
    char error_msg[256] = "";
    ASSERT_EQ(builtin_cd(current_dir, command, error_msg, sizeof(error_msg)), 0);
    ASSERT_STR_EQ(error_msg, "Not a directory");
    ASSERT_STR_EQ(current_dir, "/tmp");

    force_remove_tree(base);
}

TEST(builtin_cd_rejects_unreadable_directory)
{
    if (geteuid() == 0) {
        fprintf(stderr, "    SKIP (running as root; permission-based test does not apply)\n");
        return;
    }

    char base[64];
    make_temp_dir(base, sizeof(base));
    char blocked[PATH_MAX], command[PATH_MAX];
    join_path(blocked, sizeof(blocked), base, "/blocked");
    ASSERT_EQ(mkdir(blocked, 0755), 0);
    /* 0100 (execute-only, no read): realpath()+stat() both still succeed
     * (they only need +x on the directory itself and its ancestors), but
     * opendir() needs +r too - this is exactly the "looks like a
     * directory but isn't openable" case tfm_common.c's own comment
     * describes (e.g. a root-owned systemd-private-* dir in /tmp). */
    ASSERT_EQ(chmod(blocked, 0100), 0);
    join_path(command, sizeof(command), "cd ", blocked);

    char current_dir[PATH_MAX] = "/tmp";
    char error_msg[256] = "";
    int result = builtin_cd(current_dir, command, error_msg, sizeof(error_msg));

    ASSERT_EQ(chmod(blocked, 0700), 0);
    ASSERT_EQ(result, 0);
    ASSERT_TRUE(strstr(error_msg, "Cannot open") != NULL);
    ASSERT_STR_EQ(current_dir, "/tmp");

    force_remove_tree(base);
}

TEST(builtin_cd_path_too_long_is_rejected_not_truncated)
{
    /* A silently truncated raw_path could still resolve to a real,
     * completely unrelated existing directory (e.g. if some prefix of
     * the intended path happens to exist) - builtin_cd() must refuse
     * outright instead of ever calling realpath() on a truncated path. */
    char long_arg[PATH_MAX + 64];
    memset(long_arg, 'a', sizeof(long_arg) - 1);
    long_arg[sizeof(long_arg) - 1] = '\0';
    char command[PATH_MAX + 128];
    join_path(command, sizeof(command), "cd /", long_arg);

    char current_dir[PATH_MAX] = "/tmp";
    char error_msg[256] = "";
    ASSERT_EQ(builtin_cd(current_dir, command, error_msg, sizeof(error_msg)), 0);
    ASSERT_STR_EQ(error_msg, "Path too long");
    ASSERT_STR_EQ(current_dir, "/tmp");
}

/* --- utf8_prev_char_len -------------------------------------------------
 * Drives tfm's Backspace handling - must remove exactly one displayed
 * character (1-4 bytes), never split a multi-byte codepoint. */

TEST(utf8_prev_char_len_empty_buffer)
{
    ASSERT_EQ(utf8_prev_char_len("x", 0), 0);
}

TEST(utf8_prev_char_len_ascii)
{
    ASSERT_EQ(utf8_prev_char_len("abc", 3), 1);
}

TEST(utf8_prev_char_len_two_byte)
{
    /* U+00E9 (e acute), UTF-8: 0xC3 0xA9. */
    const char buf[] = {'a', (char)0xC3, (char)0xA9};
    ASSERT_EQ(utf8_prev_char_len(buf, sizeof(buf)), 2);
}

TEST(utf8_prev_char_len_three_byte)
{
    /* U+20AC (euro sign), UTF-8: 0xE2 0x82 0xAC. */
    const char buf[] = {'a', (char)0xE2, (char)0x82, (char)0xAC};
    ASSERT_EQ(utf8_prev_char_len(buf, sizeof(buf)), 3);
}

TEST(utf8_prev_char_len_four_byte)
{
    /* U+1F600 (grinning face emoji), UTF-8: 0xF0 0x9F 0x98 0x80. */
    const char buf[] = {'a', (char)0xF0, (char)0x9F, (char)0x98, (char)0x80};
    ASSERT_EQ(utf8_prev_char_len(buf, sizeof(buf)), 4);
}

TEST(utf8_prev_char_len_caps_malformed_continuation_run)
{
    /* A valid single-byte (ASCII) lead followed by 10 orphaned
     * continuation bytes - not reachable via normal typing, only via
     * malformed/corrupt input (a broken terminal/IME, or already-corrupt
     * input). The leading 'a' matters: without the 3-continuation-byte
     * cap in tfm_common.c, the backward walk would reach it (a genuinely
     * valid lead byte) and conclude the *entire* 11-byte buffer is "one
     * codepoint", returning 11 and deleting everything on a single
     * Backspace - a buffer of *all* continuation bytes with no valid lead
     * anywhere wouldn't actually distinguish capped from uncapped here,
     * since the final validity check catches that case regardless of
     * where the walk stopped. With the cap, the walk stops after 3 steps
     * on another continuation byte (still not a valid lead), so it falls
     * back to deleting just 1 byte instead. */
    const char buf[] = {'a', (char)0x80, (char)0x80, (char)0x80, (char)0x80, (char)0x80,
                         (char)0x80,      (char)0x80, (char)0x80, (char)0x80, (char)0x80};
    ASSERT_EQ(utf8_prev_char_len(buf, sizeof(buf)), 1);
}

TEST(utf8_prev_char_len_lone_lead_byte_without_continuation)
{
    /* A 2-byte lead with no continuation byte following it (e.g. the
     * buffer was cut short) - falls back to deleting just that 1 byte
     * rather than guessing further. */
    const char buf[] = {(char)0xC3};
    ASSERT_EQ(utf8_prev_char_len(buf, sizeof(buf)), 1);
}

/* --- shell_quote ------------------------------------------------------ */

TEST(shell_quote_plain_and_embedded_quote)
{
    char out[64];
    ASSERT_EQ(shell_quote(out, sizeof(out), "my file"), 1);
    ASSERT_STR_EQ(out, "'my file'");
    ASSERT_EQ(shell_quote(out, sizeof(out), "it's"), 1);
    ASSERT_STR_EQ(out, "'it'\\''s'");
    ASSERT_EQ(shell_quote(out, sizeof(out), ""), 1);
    ASSERT_STR_EQ(out, "''");
}

/* The real contract: whatever the name contains, /bin/sh reads the quoted
 * word back as exactly that name - no expansion, splitting or injection. */
TEST(shell_quote_round_trips_through_real_shell)
{
    const char *nasty = "a b'c\"d$(touch x)`e`;f*g\\h\tz";
    char quoted[256];
    ASSERT_EQ(shell_quote(quoted, sizeof(quoted), nasty), 1);

    char command[512];
    snprintf(command, sizeof(command), "printf %%s %s", quoted);
    FILE *p = popen(command, "r");
    ASSERT_TRUE(p != NULL);
    char echoed[256] = "";
    size_t n = fread(echoed, 1, sizeof(echoed) - 1, p);
    echoed[n] = '\0';
    ASSERT_EQ(pclose(p), 0);
    ASSERT_STR_EQ(echoed, nasty);
}

TEST(shell_quote_exact_fit_and_truncation)
{
    /* "abc" needs 'abc' + NUL = 6 bytes. */
    char out[6];
    ASSERT_EQ(shell_quote(out, 6, "abc"), 1);
    ASSERT_STR_EQ(out, "'abc'");
    ASSERT_EQ(shell_quote(out, 5, "abc"), 0);
    /* An embedded quote becomes '\'' - "'" needs ''\''' + NUL = 7 bytes. */
    char q[7];
    ASSERT_EQ(shell_quote(q, 7, "'"), 1);
    ASSERT_EQ(shell_quote(q, 6, "'"), 0);
}

TEST(shell_quote_null_args)
{
    char out[16];
    ASSERT_EQ(shell_quote(NULL, sizeof(out), "a"), 0);
    ASSERT_EQ(shell_quote(out, sizeof(out), NULL), 0);
    ASSERT_EQ(shell_quote(out, 2, ""), 0);
}

int main(void)
{
    TFM_RUN(path_join_basic);
    TFM_RUN(path_join_rejects_null_args);
    TFM_RUN(path_join_reports_truncation);
    TFM_RUN(is_safe_path_component_accepts_ordinary_names);
    TFM_RUN(is_safe_path_component_rejects_dangerous_names);
    TFM_RUN(builtin_cd_rejects_non_cd_command);
    TFM_RUN(builtin_cd_rejects_too_short_command);
    TFM_RUN(builtin_cd_absolute_path);
    TFM_RUN(builtin_cd_relative_path);
    TFM_RUN(builtin_cd_no_arg_goes_home);
    TFM_RUN(builtin_cd_nonexistent_path_reports_reason);
    TFM_RUN(builtin_cd_rejects_a_file);
    TFM_RUN(builtin_cd_rejects_unreadable_directory);
    TFM_RUN(builtin_cd_path_too_long_is_rejected_not_truncated);
    TFM_RUN(utf8_prev_char_len_empty_buffer);
    TFM_RUN(utf8_prev_char_len_ascii);
    TFM_RUN(utf8_prev_char_len_two_byte);
    TFM_RUN(utf8_prev_char_len_three_byte);
    TFM_RUN(utf8_prev_char_len_four_byte);
    TFM_RUN(utf8_prev_char_len_caps_malformed_continuation_run);
    TFM_RUN(utf8_prev_char_len_lone_lead_byte_without_continuation);
    TFM_RUN(shell_quote_plain_and_embedded_quote);
    TFM_RUN(shell_quote_round_trips_through_real_shell);
    TFM_RUN(shell_quote_exact_fit_and_truncation);
    TFM_RUN(shell_quote_null_args);
    return TFM_SUMMARY();
}
