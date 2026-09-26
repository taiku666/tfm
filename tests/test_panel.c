/* _DEFAULT_SOURCE for mkdtemp() (used by tests/test_fs_helpers.h), matching
 * src/dir.c's own feature-test macro. */
#define _DEFAULT_SOURCE

#include "test.h"
#include "test_fs_helpers.h"
#include "../include/dir.h"
#include "../include/panel.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- dir_list() fault injection ---------------------------------------------
 *
 * This binary is linked with -Wl,--wrap=dir_list (see the Makefile), so
 * panel.o's calls to dir_list() land here instead. The failure the
 * CR4-M4 fix guards against - builtin_cd()'s own opendir() probe passes,
 * then the directory vanishes/loses permissions/hits EIO before
 * dir_list() runs - is a real race with no reliable way to trigger it
 * from outside, so this lets a test fail exactly that one listing call
 * on demand while every other call still goes to the real dir.c. */
int __real_dir_list(const char *path, DirEntryInfo **out_entries, size_t *out_count);

static int g_fail_next_dir_list_errno;

int __wrap_dir_list(const char *path, DirEntryInfo **out_entries, size_t *out_count);
int __wrap_dir_list(const char *path, DirEntryInfo **out_entries, size_t *out_count)
{
    if (g_fail_next_dir_list_errno != 0) {
        *out_entries = NULL;
        *out_count = 0;
        errno = g_fail_next_dir_list_errno;
        g_fail_next_dir_list_errno = 0;
        return -1;
    }
    return __real_dir_list(path, out_entries, out_count);
}

/* Builds base/{a,b,c,d,e} (plain files) plus base/sub/ - enough entries
 * that a non-zero cursor/scroll position is meaningful. */
static void make_listing_fixture(char *base, size_t base_size)
{
    make_temp_dir(base, base_size);
    const char *names[] = {"/a", "/b", "/c", "/d", "/e"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[PATH_MAX];
        join_path(path, sizeof(path), base, names[i]);
        write_file(path, "x");
    }
    char sub[PATH_MAX];
    join_path(sub, sizeof(sub), base, "/sub");
    mkdir(sub, 0755);
}

/* --- panel_reload ---------------------------------------------------------- */

TEST(panel_reload_success_returns_1)
{
    char base[64];
    make_listing_fixture(base, sizeof(base));

    Panel panel;
    panel_init(&panel, base);
    /* "..", "sub", a..e */
    ASSERT_EQ(panel.count, (size_t)7);
    ASSERT_EQ(panel_reload(&panel), 1);
    ASSERT_EQ(panel.count, (size_t)7);

    panel_free(&panel);
    force_remove_tree(base);
}

TEST(panel_reload_failure_keeps_listing_and_cursor)
{
    char base[64];
    make_listing_fixture(base, sizeof(base));

    Panel panel;
    panel_init(&panel, base);
    panel.selected_index = 4;
    panel.scroll_offset = 2;
    const DirEntryInfo *old_entries = panel.entries;

    /* A real failure, not an injected one: the directory is gone. */
    force_remove_tree(base);
    ASSERT_EQ(panel_reload(&panel), 0);

    ASSERT_TRUE(panel.entries == old_entries);
    ASSERT_EQ(panel.count, (size_t)7);
    ASSERT_EQ(panel.selected_index, 4);
    ASSERT_EQ(panel.scroll_offset, 2);
    ASSERT_STR_EQ(panel.path, base);

    panel_free(&panel);
}

TEST(panel_reload_failure_with_no_listing_synthesizes_parent_entry)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char missing[PATH_MAX];
    join_path(missing, sizeof(missing), base, "/does-not-exist");

    Panel panel;
    panel_init(&panel, missing);

    ASSERT_EQ(panel.count, (size_t)1);
    ASSERT_STR_EQ(panel.entries[0].name, "..");
    ASSERT_TRUE(panel.entries[0].is_dir);
    ASSERT_EQ(panel.selected_index, 0);
    ASSERT_EQ(panel.scroll_offset, 0);

    panel_free(&panel);
    force_remove_tree(base);
}

/* --- panel_change_dir ------------------------------------------------------ */

TEST(panel_change_dir_success_switches_path_and_listing_together)
{
    char base[64];
    make_listing_fixture(base, sizeof(base));
    char sub[PATH_MAX];
    join_path(sub, sizeof(sub), base, "/sub");
    char inner[PATH_MAX];
    join_path(inner, sizeof(inner), base, "/sub/only-file");
    write_file(inner, "x");

    Panel panel;
    panel_init(&panel, base);
    panel.selected_index = 3;
    panel.scroll_offset = 1;

    char error_msg[256] = "";
    ASSERT_EQ(panel_change_dir(&panel, "cd sub", error_msg, sizeof(error_msg)), 1);

    /* make_temp_dir() hands back an already-canonical /tmp path, so the
     * realpath()-resolved result compares equal. */
    ASSERT_STR_EQ(panel.path, sub);
    ASSERT_EQ(panel.count, (size_t)2); /* "..", "only-file" */
    ASSERT_EQ(panel.selected_index, 0);
    ASSERT_EQ(panel.scroll_offset, 0);

    panel_free(&panel);
    force_remove_tree(base);
}

TEST(panel_change_dir_listing_failure_leaves_panel_unchanged)
{
    char base[64];
    make_listing_fixture(base, sizeof(base));

    Panel panel;
    panel_init(&panel, base);
    panel.selected_index = 4;
    panel.scroll_offset = 2;
    const DirEntryInfo *old_entries = panel.entries;

    /* builtin_cd() succeeds (sub really exists and is openable), then the
     * listing itself fails - the exact window CR4-M4 was about. Before
     * the fix, panel->path had already been rewritten to .../sub at this
     * point while the listing stayed on base's entries. */
    g_fail_next_dir_list_errno = EIO;
    char error_msg[256] = "";
    int result = panel_change_dir(&panel, "cd sub", error_msg, sizeof(error_msg));
    g_fail_next_dir_list_errno = 0;

    ASSERT_EQ(result, 0);
    ASSERT_STR_EQ(panel.path, base);
    ASSERT_TRUE(panel.entries == old_entries);
    ASSERT_EQ(panel.count, (size_t)7);
    ASSERT_EQ(panel.selected_index, 4);
    ASSERT_EQ(panel.scroll_offset, 2);
    ASSERT_TRUE(strstr(error_msg, "Cannot read directory") != NULL);
    ASSERT_TRUE(strstr(error_msg, strerror(EIO)) != NULL);

    panel_free(&panel);
    force_remove_tree(base);
}

TEST(panel_change_dir_cd_failure_leaves_panel_unchanged)
{
    char base[64];
    make_listing_fixture(base, sizeof(base));

    Panel panel;
    panel_init(&panel, base);
    panel.selected_index = 2;
    const DirEntryInfo *old_entries = panel.entries;

    char error_msg[256] = "";
    ASSERT_EQ(panel_change_dir(&panel, "cd no-such-dir", error_msg, sizeof(error_msg)), 0);

    ASSERT_STR_EQ(panel.path, base);
    ASSERT_TRUE(panel.entries == old_entries);
    ASSERT_EQ(panel.selected_index, 2);
    ASSERT_TRUE(error_msg[0] != '\0');

    panel_free(&panel);
    force_remove_tree(base);
}

/* --- marks ----------------------------------------------------------------- */

/* Index of name in panel's listing, or -1. */
static int index_of(const Panel *panel, const char *name)
{
    for (size_t i = 0; i < panel->count; i++) {
        if (strcmp(panel->entries[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void mark(Panel *panel, const char *name)
{
    int index = index_of(panel, name);
    ASSERT_TRUE(index >= 0);
    panel_toggle_mark(panel, (size_t)index);
}

TEST(toggle_mark_tracks_counts_and_sizes)
{
    char base[64];
    make_listing_fixture(base, sizeof(base));
    Panel panel;
    panel_init(&panel, base);

    mark(&panel, "a");
    mark(&panel, "b");
    mark(&panel, "sub");
    ASSERT_EQ(panel.mark_count, (size_t)3);
    ASSERT_EQ(panel.marked_dirs, (size_t)1);
    ASSERT_EQ(panel.marked_bytes, 2); /* a and b hold one byte each */

    mark(&panel, "a"); /* toggles off */
    ASSERT_EQ(panel.mark_count, (size_t)2);
    ASSERT_EQ(panel.marked_bytes, 1);
    ASSERT_EQ(panel.marks[index_of(&panel, "a")], 0);

    panel_free(&panel);
    force_remove_tree(base);
}

TEST(parent_entry_is_never_marked)
{
    char base[64];
    make_listing_fixture(base, sizeof(base));
    Panel panel;
    panel_init(&panel, base);

    mark(&panel, "..");
    ASSERT_EQ(panel.mark_count, (size_t)0);
    panel_toggle_mark(&panel, panel.count + 5); /* out of range: ignored */
    ASSERT_EQ(panel.mark_count, (size_t)0);

    panel_free(&panel);
    force_remove_tree(base);
}

TEST(toggle_mark_all_marks_everything_then_clears)
{
    char base[64];
    make_listing_fixture(base, sizeof(base));
    Panel panel;
    panel_init(&panel, base);

    mark(&panel, "c"); /* a partial selection still means "mark all" */
    panel_toggle_mark_all(&panel);
    ASSERT_EQ(panel.mark_count, (size_t)6); /* all but ".." */
    ASSERT_EQ(panel.marked_dirs, (size_t)1);
    ASSERT_EQ(panel.marked_bytes, 5);
    ASSERT_EQ(panel.marks[index_of(&panel, "..")], 0);

    panel_toggle_mark_all(&panel);
    ASSERT_EQ(panel.mark_count, (size_t)0);
    ASSERT_EQ(panel.marked_bytes, 0);

    panel_free(&panel);
    force_remove_tree(base);
}

TEST(reload_keeps_marks_by_name)
{
    char base[64];
    make_listing_fixture(base, sizeof(base));
    Panel panel;
    panel_init(&panel, base);
    mark(&panel, "b");
    mark(&panel, "d");

    /* A new entry sorts in before the marked ones and a marked one goes
     * away, so the marks must follow names, not indices. */
    char path[PATH_MAX];
    join_path(path, sizeof(path), base, "/aa");
    write_file(path, "x");
    join_path(path, sizeof(path), base, "/d");
    ASSERT_EQ(unlink(path), 0);
    ASSERT_EQ(panel_reload(&panel), 1);

    ASSERT_EQ(panel.mark_count, (size_t)1);
    ASSERT_EQ(panel.marks[index_of(&panel, "b")], 1);
    ASSERT_EQ(panel.marks[index_of(&panel, "aa")], 0);
    ASSERT_EQ(panel.marked_bytes, 1);

    panel_free(&panel);
    force_remove_tree(base);
}

TEST(change_dir_clears_marks)
{
    char base[64];
    make_listing_fixture(base, sizeof(base));
    Panel panel;
    panel_init(&panel, base);
    mark(&panel, "a");
    mark(&panel, "b");

    char error_msg[256];
    ASSERT_EQ(panel_change_dir(&panel, "cd sub", error_msg, sizeof(error_msg)), 1);
    ASSERT_EQ(panel.mark_count, (size_t)0);
    ASSERT_EQ(panel_change_dir(&panel, "cd ..", error_msg, sizeof(error_msg)), 1);
    ASSERT_EQ(panel.mark_count, (size_t)0);
    ASSERT_EQ(panel.marks[index_of(&panel, "a")], 0);

    panel_free(&panel);
    force_remove_tree(base);
}

TEST(unmark_subtracts_the_size_recorded_at_marking)
{
    char base[64];
    make_listing_fixture(base, sizeof(base));
    Panel panel;
    panel_init(&panel, base);
    mark(&panel, "a");

    char path[PATH_MAX];
    join_path(path, sizeof(path), base, "/a");
    write_file(path, "grew while marked");
    mark(&panel, "a");
    ASSERT_EQ(panel.marked_bytes, 0);

    panel_free(&panel);
    force_remove_tree(base);
}

TEST(clear_marks_resets_everything)
{
    char base[64];
    make_listing_fixture(base, sizeof(base));
    Panel panel;
    panel_init(&panel, base);
    panel_toggle_mark_all(&panel);
    panel_clear_marks(&panel);

    ASSERT_EQ(panel.mark_count, (size_t)0);
    ASSERT_EQ(panel.marked_dirs, (size_t)0);
    ASSERT_EQ(panel.marked_bytes, 0);
    for (size_t i = 0; i < panel.count; i++) {
        ASSERT_EQ(panel.marks[i], 0);
    }

    panel_free(&panel);
    force_remove_tree(base);
}

int main(void)
{
    TFM_RUN(panel_reload_success_returns_1);
    TFM_RUN(panel_reload_failure_keeps_listing_and_cursor);
    TFM_RUN(panel_reload_failure_with_no_listing_synthesizes_parent_entry);
    TFM_RUN(panel_change_dir_success_switches_path_and_listing_together);
    TFM_RUN(panel_change_dir_listing_failure_leaves_panel_unchanged);
    TFM_RUN(panel_change_dir_cd_failure_leaves_panel_unchanged);
    TFM_RUN(toggle_mark_tracks_counts_and_sizes);
    TFM_RUN(parent_entry_is_never_marked);
    TFM_RUN(toggle_mark_all_marks_everything_then_clears);
    TFM_RUN(reload_keeps_marks_by_name);
    TFM_RUN(change_dir_clears_marks);
    TFM_RUN(unmark_subtracts_the_size_recorded_at_marking);
    TFM_RUN(clear_marks_resets_everything);
    return TFM_SUMMARY();
}
