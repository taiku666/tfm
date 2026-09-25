/* _DEFAULT_SOURCE for mkdtemp()/setenv() (used by tests/test_fs_helpers.h). */
#define _DEFAULT_SOURCE

#include "test.h"
#include "test_fs_helpers.h"
#include "../src_gui/omarchy_theme.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* omarchy_theme.c is plain libc (no GTK), so this binary compiles it
 * straight from src_gui/ (see the Makefile) - tfm-gui's own parsing is
 * tested without needing GTK installed.
 *
 * Each test points $HOME at a fresh temp dir holding only the given
 * colors.toml text, at the exact path omarchy_theme_load() reads. */
typedef struct {
    char home[64];
    SavedEnvVar saved_home;
} ThemeFixture;

static void theme_fixture_setup(ThemeFixture *fx, const char *colors_toml)
{
    make_temp_dir(fx->home, sizeof(fx->home));
    const char *dirs[] = {"/.local", "/.local/state", "/.local/state/omarchy",
                          "/.local/state/omarchy/current", "/.local/state/omarchy/current/theme"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char dir[PATH_MAX];
        join_path(dir, sizeof(dir), fx->home, dirs[i]);
        mkdir(dir, 0755);
    }
    char path[PATH_MAX];
    join_path(path, sizeof(path), fx->home, "/.local/state/omarchy/current/theme/colors.toml");
    write_file(path, colors_toml);
    save_and_set_env_var(&fx->saved_home, "HOME", fx->home);
}

static void theme_fixture_teardown(ThemeFixture *fx)
{
    restore_env_var(&fx->saved_home);
    force_remove_tree(fx->home);
}

/* --- parsing ---------------------------------------------------------------- */

TEST(loads_real_omarchy_format)
{
    ThemeFixture fx;
    theme_fixture_setup(&fx, "mode = \"dark\"\n"
                             "\n"
                             "accent = \"#509475\"\n"
                             "selection = \"#32473B\"\n"
                             "background = \"#111c18\"\n"
                             "dark_background = \"#0c1512\"\n"
                             "foreground = \"#C1C497\"\n"
                             "red = \"#FF5345\"\n");

    OmarchyThemeColors colors;
    int result = omarchy_theme_load(&colors);
    theme_fixture_teardown(&fx);

    ASSERT_EQ(result, 1);
    ASSERT_EQ(colors.is_dark, 1);
    ASSERT_STR_EQ(colors.accent, "#509475");
    ASSERT_STR_EQ(colors.selection, "#32473B");
    ASSERT_STR_EQ(colors.background, "#111c18");
    ASSERT_STR_EQ(colors.dark_background, "#0c1512");
    ASSERT_STR_EQ(colors.foreground, "#C1C497");
}

TEST(inline_comment_after_quoted_value_is_stripped)
{
    /* The CR4-M5 case: previously the value stayed as the literal
     * `"#f38d70" # comment`, quotes included. */
    ThemeFixture fx;
    theme_fixture_setup(&fx, "mode = \"dark\" # set by theme switcher\n"
                             "accent = \"#f38d70\" # comment\n"
                             "background = \"#111111\"\t# tab before comment\n");

    OmarchyThemeColors colors;
    int result = omarchy_theme_load(&colors);
    theme_fixture_teardown(&fx);

    ASSERT_EQ(result, 1);
    ASSERT_EQ(colors.is_dark, 1);
    ASSERT_STR_EQ(colors.accent, "#f38d70");
    ASSERT_STR_EQ(colors.background, "#111111");
}

TEST(unquoted_value_keeps_its_hash_and_drops_comment)
{
    ThemeFixture fx;
    theme_fixture_setup(&fx, "accent = #f38d70\n"
                             "background = #222222 # comment\n"
                             "mode = dark # comment\n");

    OmarchyThemeColors colors;
    int result = omarchy_theme_load(&colors);
    theme_fixture_teardown(&fx);

    ASSERT_EQ(result, 1);
    ASSERT_STR_EQ(colors.accent, "#f38d70");
    ASSERT_STR_EQ(colors.background, "#222222");
    ASSERT_EQ(colors.is_dark, 1);
}

TEST(crlf_line_endings_are_handled)
{
    ThemeFixture fx;
    theme_fixture_setup(&fx, "mode = \"dark\"\r\naccent = \"#abcdef\"\r\n");

    OmarchyThemeColors colors;
    int result = omarchy_theme_load(&colors);
    theme_fixture_teardown(&fx);

    ASSERT_EQ(result, 1);
    ASSERT_EQ(colors.is_dark, 1);
    ASSERT_STR_EQ(colors.accent, "#abcdef");
}

/* --- validation ------------------------------------------------------------- */

TEST(shorthand_hex_is_accepted)
{
    ThemeFixture fx;
    theme_fixture_setup(&fx, "accent = \"#f80\"\n");

    OmarchyThemeColors colors;
    int result = omarchy_theme_load(&colors);
    theme_fixture_teardown(&fx);

    ASSERT_EQ(result, 1);
    ASSERT_STR_EQ(colors.accent, "#f80");
}

TEST(invalid_accent_counts_as_not_found)
{
    /* Anything that isn't #rgb/#rrggbb must never reach the CSS - a CSS
     * fragment here would otherwise be spliced into @define-color. */
    const char *bad_accents[] = {
        "accent = \"red\"\n",
        "accent = \"#12345\"\n",
        "accent = \"#1234567\"\n",
        "accent = \"#ggghhh\"\n",
        "accent = \"#fff; } * { color: red\"\n",
        "accent = \"#f38d70\n", /* unterminated quote */
        "accent = \"\"\n",
        "accent =\n",
    };
    for (size_t i = 0; i < sizeof(bad_accents) / sizeof(bad_accents[0]); i++) {
        ThemeFixture fx;
        theme_fixture_setup(&fx, bad_accents[i]);

        OmarchyThemeColors colors;
        int result = omarchy_theme_load(&colors);
        theme_fixture_teardown(&fx);

        if (result != 0 || colors.accent[0] != '\0') {
            fprintf(stderr, "    accepted bad input: %s", bad_accents[i]);
        }
        ASSERT_EQ(result, 0);
        ASSERT_STR_EQ(colors.accent, "");
    }
}

TEST(invalid_secondary_color_is_dropped_but_theme_still_loads)
{
    ThemeFixture fx;
    theme_fixture_setup(&fx, "accent = \"#509475\"\n"
                             "background = \"not-a-color\"\n"
                             "foreground = \"#C1C497\"\n");

    OmarchyThemeColors colors;
    int result = omarchy_theme_load(&colors);
    theme_fixture_teardown(&fx);

    ASSERT_EQ(result, 1);
    ASSERT_STR_EQ(colors.accent, "#509475");
    ASSERT_STR_EQ(colors.background, "");
    ASSERT_STR_EQ(colors.foreground, "#C1C497");
}

TEST(later_invalid_accent_clears_an_earlier_valid_one)
{
    /* Last assignment wins, as for every other key - a later bad value
     * must not leave the earlier good one half-applied with
     * found_accent claiming otherwise. */
    ThemeFixture fx;
    theme_fixture_setup(&fx, "accent = \"#509475\"\naccent = \"oops\"\n");

    OmarchyThemeColors colors;
    int result = omarchy_theme_load(&colors);
    theme_fixture_teardown(&fx);

    ASSERT_EQ(result, 0);
    ASSERT_STR_EQ(colors.accent, "");
}

TEST(missing_file_returns_0)
{
    ThemeFixture fx;
    make_temp_dir(fx.home, sizeof(fx.home));
    save_and_set_env_var(&fx.saved_home, "HOME", fx.home);

    OmarchyThemeColors colors;
    int result = omarchy_theme_load(&colors);
    theme_fixture_teardown(&fx);

    ASSERT_EQ(result, 0);
}

int main(void)
{
    TFM_RUN(loads_real_omarchy_format);
    TFM_RUN(inline_comment_after_quoted_value_is_stripped);
    TFM_RUN(unquoted_value_keeps_its_hash_and_drops_comment);
    TFM_RUN(crlf_line_endings_are_handled);
    TFM_RUN(shorthand_hex_is_accepted);
    TFM_RUN(invalid_accent_counts_as_not_found);
    TFM_RUN(invalid_secondary_color_is_dropped_but_theme_still_loads);
    TFM_RUN(later_invalid_accent_clears_an_earlier_valid_one);
    TFM_RUN(missing_file_returns_0);
    return TFM_SUMMARY();
}
