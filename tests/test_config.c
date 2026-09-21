/* _DEFAULT_SOURCE for mkdtemp(), setenv()/unsetenv(), strdup() (used by
 * tests/test_fs_helpers.h). */
#define _DEFAULT_SOURCE

#include "test.h"
#include "test_fs_helpers.h"
#include "../include/config.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* config_*() read $HOME directly via getenv() to find ~/.tfm/tfm.ini, so
 * every test below isolates it to a fresh mkdtemp() directory - never the
 * real, ambient $HOME (see feedback_tfm_trash_undo_testing for why this
 * matters: forgetting it for even one call risks touching the real
 * ~/.tfm/tfm.ini). */

/* --- config_set_defaults ------------------------------------------------- */

TEST(config_set_defaults_uses_home)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char home[PATH_MAX];
    join_path(home, sizeof(home), base, "/home");
    ASSERT_EQ(mkdir(home, 0755), 0);

    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_home;
    save_and_set_env_var(&saved_home, "HOME", home);

    Config cfg;
    config_set_defaults(&cfg);
    ASSERT_STR_EQ(cfg.left_path, home);
    ASSERT_STR_EQ(cfg.right_path, home);
    ASSERT_STR_EQ(cfg.border_color, "system");
    ASSERT_STR_EQ(cfg.dir_color, "blue");
    ASSERT_STR_EQ(cfg.icons, "omarchy");
    ASSERT_STR_EQ(cfg.gui_theme, "omarchy");
    /* Behavior, not the literal default string (which isn't part of the
     * public API) - a fresh config must already recognize an ordinary
     * text file as an editor extension. */
    ASSERT_TRUE(config_is_editor_extension(&cfg, "notes.txt"));

    force_remove_tree(base);
}

TEST(config_set_defaults_home_unset_falls_back_to_root)
{
    const char *old_home = getenv("HOME");
    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_home;
    saved_home.name = "HOME";
    saved_home.old_value = (old_home != NULL) ? strdup(old_home) : NULL;
    unsetenv("HOME");

    Config cfg;
    config_set_defaults(&cfg);
    ASSERT_STR_EQ(cfg.left_path, "/");
    ASSERT_STR_EQ(cfg.right_path, "/");
}

/* --- config_load --------------------------------------------------------- */

TEST(config_load_missing_file_uses_defaults_silently)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char home[PATH_MAX];
    join_path(home, sizeof(home), base, "/home");
    ASSERT_EQ(mkdir(home, 0755), 0);

    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_home;
    save_and_set_env_var(&saved_home, "HOME", home);

    Config cfg;
    char error_msg[256] = "unset";
    config_load(&cfg, error_msg, sizeof(error_msg));
    /* ENOENT (no ~/.tfm/tfm.ini yet) is the ordinary first-run case, not
     * an error - error_msg must come back empty. */
    ASSERT_STR_EQ(error_msg, "");
    ASSERT_STR_EQ(cfg.left_path, home);

    force_remove_tree(base);
}

TEST(config_load_open_failure_is_reported)
{
    if (geteuid() == 0) {
        fprintf(stderr, "    SKIP (running as root; permission-based test does not apply)\n");
        return;
    }

    char base[64];
    make_temp_dir(base, sizeof(base));
    char home[PATH_MAX], tfm_dir[PATH_MAX], ini_path[PATH_MAX];
    join_path(home, sizeof(home), base, "/home");
    join_path(tfm_dir, sizeof(tfm_dir), home, "/.tfm");
    join_path(ini_path, sizeof(ini_path), tfm_dir, "/tfm.ini");
    ASSERT_EQ(mkdir(home, 0755), 0);
    ASSERT_EQ(mkdir(tfm_dir, 0755), 0);
    write_file(ini_path, "[display]\nborder_color=red\n");
    /* Unlike a missing file (ENOENT), an existing-but-unreadable one
     * (EACCES) must be surfaced, not silently swallowed into "just use
     * defaults" - see config.h's documented contract. */
    ASSERT_EQ(chmod(ini_path, 0000), 0);

    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_home;
    save_and_set_env_var(&saved_home, "HOME", home);

    Config cfg;
    char error_msg[256] = "";
    config_load(&cfg, error_msg, sizeof(error_msg));
    ASSERT_TRUE(strstr(error_msg, "Cannot open") != NULL);
    /* Defaults are still filled in even on a read failure. */
    ASSERT_STR_EQ(cfg.left_path, home);

    force_remove_tree(base);
}

TEST(config_load_ignores_malformed_lines)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char home[PATH_MAX], tfm_dir[PATH_MAX], ini_path[PATH_MAX];
    join_path(home, sizeof(home), base, "/home");
    join_path(tfm_dir, sizeof(tfm_dir), home, "/.tfm");
    join_path(ini_path, sizeof(ini_path), tfm_dir, "/tfm.ini");
    ASSERT_EQ(mkdir(home, 0755), 0);
    ASSERT_EQ(mkdir(tfm_dir, 0755), 0);
    write_file(ini_path,
               "; a comment\n"
               "# another comment style\n"
               "\n"
               "this line has no equals sign\n"
               "[unknown_section]\n"
               "border_color=should_be_ignored\n"
               "[display]\n"
               "border_color=green\n");

    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_home;
    save_and_set_env_var(&saved_home, "HOME", home);

    Config cfg;
    char error_msg[256] = "unset";
    config_load(&cfg, error_msg, sizeof(error_msg));
    ASSERT_STR_EQ(error_msg, "");
    ASSERT_STR_EQ(cfg.border_color, "green");

    force_remove_tree(base);
}

TEST(config_load_empty_panel_value_keeps_default)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char home[PATH_MAX], tfm_dir[PATH_MAX], ini_path[PATH_MAX];
    join_path(home, sizeof(home), base, "/home");
    join_path(tfm_dir, sizeof(tfm_dir), home, "/.tfm");
    join_path(ini_path, sizeof(ini_path), tfm_dir, "/tfm.ini");
    ASSERT_EQ(mkdir(home, 0755), 0);
    ASSERT_EQ(mkdir(tfm_dir, 0755), 0);
    /* An empty value must not overwrite the $HOME default with "" - that
     * would feed path_join() as "/name" for every later operation in
     * that panel (see config.c's own comment). */
    write_file(ini_path, "[panels]\nleft_path=\n");

    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_home;
    save_and_set_env_var(&saved_home, "HOME", home);

    Config cfg;
    char error_msg[256] = "";
    config_load(&cfg, error_msg, sizeof(error_msg));
    ASSERT_STR_EQ(cfg.left_path, home);

    force_remove_tree(base);
}

/* --- config_save ----------------------------------------------------------
 * Both config_load()'s and config_save()'s error_msg buffers are sized
 * larger than the messages below actually need. */

TEST(config_save_creates_tfm_dir_and_file_with_0600)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char home[PATH_MAX], tfm_dir[PATH_MAX], ini_path[PATH_MAX];
    join_path(home, sizeof(home), base, "/home");
    join_path(tfm_dir, sizeof(tfm_dir), home, "/.tfm");
    join_path(ini_path, sizeof(ini_path), tfm_dir, "/tfm.ini");
    ASSERT_EQ(mkdir(home, 0755), 0);

    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_home;
    save_and_set_env_var(&saved_home, "HOME", home);

    Config cfg;
    config_set_defaults(&cfg);
    char error_msg[256] = "unset";
    config_save(&cfg, error_msg, sizeof(error_msg));
    ASSERT_STR_EQ(error_msg, "");
    ASSERT_TRUE(is_dir(tfm_dir));
    ASSERT_TRUE(path_exists(ini_path));

    struct stat st;
    ASSERT_EQ(stat(ini_path, &st), 0);
    /* Saved via mkstemp()+fchmod(0600)+rename() - the final file keeps
     * that mode regardless of umask, unlike a plain fopen(path, "w"). */
    ASSERT_EQ(st.st_mode & 0777, 0600);

    force_remove_tree(base);
}

TEST(config_save_and_load_round_trip)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char home[PATH_MAX];
    join_path(home, sizeof(home), base, "/home");
    ASSERT_EQ(mkdir(home, 0755), 0);

    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_home;
    save_and_set_env_var(&saved_home, "HOME", home);

    Config cfg;
    config_set_defaults(&cfg);
    snprintf(cfg.border_color, sizeof(cfg.border_color), "red");
    snprintf(cfg.dir_color, sizeof(cfg.dir_color), "cyan");
    snprintf(cfg.icons, sizeof(cfg.icons), "off");
    snprintf(cfg.gui_theme, sizeof(cfg.gui_theme), "system");
    snprintf(cfg.editor_extensions, sizeof(cfg.editor_extensions), "foo,bar");

    char error_msg[256] = "unset";
    config_save(&cfg, error_msg, sizeof(error_msg));
    ASSERT_STR_EQ(error_msg, "");

    Config loaded;
    config_load(&loaded, error_msg, sizeof(error_msg));
    ASSERT_STR_EQ(error_msg, "");
    ASSERT_STR_EQ(loaded.border_color, "red");
    ASSERT_STR_EQ(loaded.dir_color, "cyan");
    ASSERT_STR_EQ(loaded.icons, "off");
    ASSERT_STR_EQ(loaded.gui_theme, "system");
    ASSERT_STR_EQ(loaded.editor_extensions, "foo,bar");
    ASSERT_STR_EQ(loaded.left_path, home);
    ASSERT_STR_EQ(loaded.right_path, home);

    force_remove_tree(base);
}

TEST(config_save_load_escapes_special_characters)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char home[PATH_MAX];
    join_path(home, sizeof(home), base, "/home");
    ASSERT_EQ(mkdir(home, 0755), 0);

    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_home;
    save_and_set_env_var(&saved_home, "HOME", home);

    Config cfg;
    config_set_defaults(&cfg);
    /* A literal backslash, newline, and carriage return, plus an
     * embedded '=' - each is a case escape_value()/unescape_value() (and
     * the "split on first '=' only" parsing) exist specifically to round-
     * trip correctly, since a raw newline would otherwise split one
     * logical .ini value across two physical lines. */
    snprintf(cfg.border_color, sizeof(cfg.border_color), "a=b\\n\nc\r");

    char error_msg[256] = "unset";
    config_save(&cfg, error_msg, sizeof(error_msg));
    ASSERT_STR_EQ(error_msg, "");

    Config loaded;
    config_load(&loaded, error_msg, sizeof(error_msg));
    ASSERT_STR_EQ(error_msg, "");
    ASSERT_STR_EQ(loaded.border_color, "a=b\\n\nc\r");

    force_remove_tree(base);
}

TEST(config_save_blocked_by_file_at_config_dir_path)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char home[PATH_MAX], tfm_dir[PATH_MAX];
    join_path(home, sizeof(home), base, "/home");
    join_path(tfm_dir, sizeof(tfm_dir), home, "/.tfm");
    ASSERT_EQ(mkdir(home, 0755), 0);
    /* A plain file already occupies ~/.tfm - mkdir() tolerates EEXIST
     * (the common case: the directory already exists from a previous
     * run), but a file there instead means every subsequent path under
     * it is unusable; config_save() must fail loudly via the downstream
     * mkstemp() ENOTDIR, not silently succeed or crash. */
    write_file(tfm_dir, "not a directory");

    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_home;
    save_and_set_env_var(&saved_home, "HOME", home);

    Config cfg;
    config_set_defaults(&cfg);
    char error_msg[256] = "";
    config_save(&cfg, error_msg, sizeof(error_msg));
    ASSERT_TRUE(error_msg[0] != '\0');
    ASSERT_TRUE(strstr(error_msg, "Cannot create") != NULL);

    force_remove_tree(base);
}

TEST(config_save_reports_permission_failure)
{
    if (geteuid() == 0) {
        fprintf(stderr, "    SKIP (running as root; permission-based test does not apply)\n");
        return;
    }

    char base[64];
    make_temp_dir(base, sizeof(base));
    char home[PATH_MAX], tfm_dir[PATH_MAX];
    join_path(home, sizeof(home), base, "/home");
    join_path(tfm_dir, sizeof(tfm_dir), home, "/.tfm");
    ASSERT_EQ(mkdir(home, 0755), 0);
    ASSERT_EQ(mkdir(tfm_dir, 0755), 0);
    /* Read+execute but no write: mkdir() itself succeeds (dir already
     * exists), but mkstemp() inside it must fail with EACCES rather than
     * silently doing nothing or crashing. */
    ASSERT_EQ(chmod(tfm_dir, 0500), 0);

    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_home;
    save_and_set_env_var(&saved_home, "HOME", home);

    Config cfg;
    config_set_defaults(&cfg);
    char error_msg[256] = "";
    config_save(&cfg, error_msg, sizeof(error_msg));
    ASSERT_TRUE(error_msg[0] != '\0');
    ASSERT_TRUE(strstr(error_msg, "Cannot create") != NULL);

    /* force_remove_tree() restores +w on every directory it descends
     * into on its own, so no explicit chmod-back is needed here. */
    force_remove_tree(base);
}

TEST(config_get_path_too_long_on_load_and_save)
{
    /* No real directory needs to exist for this - config_get_dir()'s
     * truncation check runs before any filesystem access, so a bogus
     * oversized $HOME is enough to exercise it. */
    char huge_home[PATH_MAX];
    memset(huge_home, 'a', sizeof(huge_home) - 1);
    huge_home[sizeof(huge_home) - 1] = '\0';

    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_home;
    save_and_set_env_var(&saved_home, "HOME", huge_home);

    Config cfg;
    char error_msg[256] = "";
    config_load(&cfg, error_msg, sizeof(error_msg));
    ASSERT_STR_EQ(error_msg, "Path too long");

    error_msg[0] = '\0';
    config_save(&cfg, error_msg, sizeof(error_msg));
    ASSERT_STR_EQ(error_msg, "Path too long");
}

/* --- config_is_editor_extension ------------------------------------------- */

TEST(config_is_editor_extension_case_insensitive_default_list)
{
    Config cfg;
    config_set_defaults(&cfg);
    ASSERT_TRUE(config_is_editor_extension(&cfg, "notes.txt"));
    ASSERT_TRUE(config_is_editor_extension(&cfg, "notes.TXT"));
    ASSERT_TRUE(config_is_editor_extension(&cfg, "main.C"));
}

TEST(config_is_editor_extension_rejects_non_matches)
{
    Config cfg;
    config_set_defaults(&cfg);
    ASSERT_FALSE(config_is_editor_extension(&cfg, "README"));
    ASSERT_FALSE(config_is_editor_extension(&cfg, ".bashrc"));
    ASSERT_FALSE(config_is_editor_extension(&cfg, "file."));
    ASSERT_FALSE(config_is_editor_extension(&cfg, "program.exe"));
}

TEST(config_is_editor_extension_null_args)
{
    Config cfg;
    config_set_defaults(&cfg);
    ASSERT_FALSE(config_is_editor_extension(NULL, "notes.txt"));
    ASSERT_FALSE(config_is_editor_extension(&cfg, NULL));
}

TEST(config_is_editor_extension_uses_loaded_custom_list)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char home[PATH_MAX], tfm_dir[PATH_MAX], ini_path[PATH_MAX];
    join_path(home, sizeof(home), base, "/home");
    join_path(tfm_dir, sizeof(tfm_dir), home, "/.tfm");
    join_path(ini_path, sizeof(ini_path), tfm_dir, "/tfm.ini");
    ASSERT_EQ(mkdir(home, 0755), 0);
    ASSERT_EQ(mkdir(tfm_dir, 0755), 0);
    /* Deliberate surrounding whitespace around a couple of entries - the
     * per-token trim() inside config_is_editor_extension() must still
     * match them. */
    write_file(ini_path, "[editor]\nextensions=foo, BAR ,baz\n");

    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_home;
    save_and_set_env_var(&saved_home, "HOME", home);

    Config cfg;
    char error_msg[256] = "";
    config_load(&cfg, error_msg, sizeof(error_msg));
    ASSERT_STR_EQ(error_msg, "");
    ASSERT_TRUE(config_is_editor_extension(&cfg, "x.foo"));
    ASSERT_TRUE(config_is_editor_extension(&cfg, "x.bar"));
    ASSERT_TRUE(config_is_editor_extension(&cfg, "x.baz"));
    /* The default list is replaced, not merged, once [editor] is present. */
    ASSERT_FALSE(config_is_editor_extension(&cfg, "x.txt"));

    force_remove_tree(base);
}

int main(void)
{
    TFM_RUN(config_set_defaults_uses_home);
    TFM_RUN(config_set_defaults_home_unset_falls_back_to_root);
    TFM_RUN(config_load_missing_file_uses_defaults_silently);
    TFM_RUN(config_load_open_failure_is_reported);
    TFM_RUN(config_load_ignores_malformed_lines);
    TFM_RUN(config_load_empty_panel_value_keeps_default);
    TFM_RUN(config_save_creates_tfm_dir_and_file_with_0600);
    TFM_RUN(config_save_and_load_round_trip);
    TFM_RUN(config_save_load_escapes_special_characters);
    TFM_RUN(config_save_blocked_by_file_at_config_dir_path);
    TFM_RUN(config_save_reports_permission_failure);
    TFM_RUN(config_get_path_too_long_on_load_and_save);
    TFM_RUN(config_is_editor_extension_case_insensitive_default_list);
    TFM_RUN(config_is_editor_extension_rejects_non_matches);
    TFM_RUN(config_is_editor_extension_null_args);
    TFM_RUN(config_is_editor_extension_uses_loaded_custom_list);
    return TFM_SUMMARY();
}
