/* _DEFAULT_SOURCE for mkdtemp(), setenv()/unsetenv(), symlink(), getsid()
 * (used here and by tests/test_fs_helpers.h). */
#define _DEFAULT_SOURCE

#include "test.h"
#include "test_fs_helpers.h"
#include "../include/opener.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- opener_is_executable -------------------------------------------- */

TEST(is_executable_regular_files_by_mode)
{
    char base[64], exe[PATH_MAX], plain[PATH_MAX];
    make_temp_dir(base, sizeof(base));
    join_path(exe, sizeof(exe), base, "/run.sh");
    join_path(plain, sizeof(plain), base, "/notes.txt");
    write_file(exe, "#!/bin/sh\n");
    write_file(plain, "hello\n");
    ASSERT_EQ(chmod(exe, 0755), 0);
    ASSERT_EQ(chmod(plain, 0644), 0);

    ASSERT_EQ(opener_is_executable(exe), 1);
    ASSERT_EQ(opener_is_executable(plain), 0);

    force_remove_tree(base);
}

TEST(is_executable_rejects_directories_and_bad_paths)
{
    char base[64], dir[PATH_MAX], missing[PATH_MAX];
    make_temp_dir(base, sizeof(base));
    join_path(dir, sizeof(dir), base, "/sub");
    join_path(missing, sizeof(missing), base, "/missing");
    ASSERT_EQ(mkdir(dir, 0755), 0);

    /* A directory's x bit means "searchable", not "runnable". */
    ASSERT_EQ(opener_is_executable(dir), 0);
    ASSERT_EQ(opener_is_executable(missing), 0);
    ASSERT_EQ(opener_is_executable(NULL), 0);

    force_remove_tree(base);
}

TEST(is_executable_follows_symlinks)
{
    char base[64], exe[PATH_MAX], link_ok[PATH_MAX], link_dangling[PATH_MAX];
    make_temp_dir(base, sizeof(base));
    join_path(exe, sizeof(exe), base, "/tool");
    join_path(link_ok, sizeof(link_ok), base, "/tool-link");
    join_path(link_dangling, sizeof(link_dangling), base, "/dangling");
    write_file(exe, "#!/bin/sh\n");
    ASSERT_EQ(chmod(exe, 0755), 0);
    ASSERT_EQ(symlink(exe, link_ok), 0);
    ASSERT_EQ(symlink("/nonexistent/tfm-test", link_dangling), 0);

    ASSERT_EQ(opener_is_executable(link_ok), 1);
    ASSERT_EQ(opener_is_executable(link_dangling), 0);

    force_remove_tree(base);
}

/* --- opener_open_default (against a fake gio on $PATH) ----------------- */

/* Installs base/bin/gio: records its arguments, where its stdout points
 * and its session ID into $FAKE_GIO_RECORD, then exits $FAKE_GIO_RC. The
 * real gio would launch an app on the developer's desktop. */
static void install_fake_gio(const char *base, char *bin_dir, size_t bin_dir_size)
{
    char gio[PATH_MAX];
    join_path(bin_dir, bin_dir_size, base, "/bin");
    ASSERT_EQ(mkdir(bin_dir, 0755), 0);
    join_path(gio, sizeof(gio), bin_dir, "/gio");
    write_file(gio, "#!/bin/sh\n"
                    "out=$(readlink /proc/$$/fd/1)\n" /* before the redirect below */
                    "{\n"
                    "  printf 'args:%s|' \"$@\"; echo\n"
                    "  echo \"stdout:$out\"\n"
                    "  echo \"sid:$(cut -d' ' -f6 /proc/$$/stat)\"\n"
                    "} > \"$FAKE_GIO_RECORD\"\n"
                    "exit \"$FAKE_GIO_RC\"\n");
    ASSERT_EQ(chmod(gio, 0755), 0);
}

TEST(open_default_success_runs_detached_gio)
{
    char base[64], bin_dir[PATH_MAX], record[PATH_MAX];
    make_temp_dir(base, sizeof(base));
    install_fake_gio(base, bin_dir, sizeof(bin_dir));
    join_path(record, sizeof(record), base, "/record");

    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_path;
    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_rc;
    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_record;
    char path_env[PATH_MAX + 32];
    snprintf(path_env, sizeof(path_env), "%s:/usr/bin:/bin", bin_dir);
    save_and_set_env_var(&saved_path, "PATH", path_env);
    save_and_set_env_var(&saved_rc, "FAKE_GIO_RC", "0");
    save_and_set_env_var(&saved_record, "FAKE_GIO_RECORD", record);

    char error_msg[256] = "unset";
    ASSERT_EQ(opener_open_default("/some dir/photo.png", error_msg, sizeof(error_msg)), 1);

    char recorded[1024] = "";
    read_file(record, recorded, sizeof(recorded));
    /* "--" ends gio's options, so the path is never parsed as a flag. */
    ASSERT_TRUE(strstr(recorded, "args:open|args:--|args:/some dir/photo.png|") != NULL);
    ASSERT_TRUE(strstr(recorded, "stdout:/dev/null\n") != NULL);

    /* Its own session (sid == its own pid, never ours), so the launched
     * app survives tfm's terminal closing. */
    char our_sid[32];
    snprintf(our_sid, sizeof(our_sid), "sid:%d\n", (int)getsid(0));
    ASSERT_TRUE(strstr(recorded, "sid:") != NULL);
    ASSERT_TRUE(strstr(recorded, our_sid) == NULL);

    force_remove_tree(base);
}

TEST(open_default_gio_failure_reports_no_application)
{
    char base[64], bin_dir[PATH_MAX], record[PATH_MAX];
    make_temp_dir(base, sizeof(base));
    install_fake_gio(base, bin_dir, sizeof(bin_dir));
    join_path(record, sizeof(record), base, "/record");

    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_path;
    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_rc;
    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_record;
    char path_env[PATH_MAX + 32];
    snprintf(path_env, sizeof(path_env), "%s:/usr/bin:/bin", bin_dir);
    save_and_set_env_var(&saved_path, "PATH", path_env);
    save_and_set_env_var(&saved_rc, "FAKE_GIO_RC", "2");
    save_and_set_env_var(&saved_record, "FAKE_GIO_RECORD", record);

    char error_msg[256] = "";
    ASSERT_EQ(opener_open_default("/data/blob.xyz", error_msg, sizeof(error_msg)), 0);
    ASSERT_STR_EQ(error_msg, "No application found to open \"blob.xyz\"");

    force_remove_tree(base);
}

TEST(open_default_missing_gio_is_reported)
{
    char base[64];
    make_temp_dir(base, sizeof(base));

    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_path;
    save_and_set_env_var(&saved_path, "PATH", base); /* empty: no gio */

    char error_msg[256] = "";
    ASSERT_EQ(opener_open_default("/data/photo.png", error_msg, sizeof(error_msg)), 0);
    ASSERT_STR_EQ(error_msg, "Cannot open files: 'gio' (GLib) is not installed");

    force_remove_tree(base);
}

TEST(open_default_rejects_empty_path)
{
    char error_msg[64] = "";
    ASSERT_EQ(opener_open_default(NULL, error_msg, sizeof(error_msg)), 0);
    ASSERT_STR_EQ(error_msg, "No file to open");
    error_msg[0] = '\0';
    ASSERT_EQ(opener_open_default("", error_msg, sizeof(error_msg)), 0);
    ASSERT_STR_EQ(error_msg, "No file to open");
    /* error_msg is optional. */
    ASSERT_EQ(opener_open_default("", NULL, 0), 0);
}

int main(void)
{
    TFM_RUN(is_executable_regular_files_by_mode);
    TFM_RUN(is_executable_rejects_directories_and_bad_paths);
    TFM_RUN(is_executable_follows_symlinks);
    TFM_RUN(open_default_success_runs_detached_gio);
    TFM_RUN(open_default_gio_failure_reports_no_application);
    TFM_RUN(open_default_missing_gio_is_reported);
    TFM_RUN(open_default_rejects_empty_path);
    return TFM_SUMMARY();
}
