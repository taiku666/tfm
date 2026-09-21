/* _DEFAULT_SOURCE for mkdtemp() (used by tests/test_fs_helpers.h), matching
 * src/dir.c's own feature-test macro. */
#define _DEFAULT_SOURCE

#include "test.h"
#include "test_fs_helpers.h"
#include "../include/dir.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- small helpers for inspecting a DirEntryInfo[] result --------------- */

static const DirEntryInfo *find_entry(const DirEntryInfo *entries, size_t count, const char *name)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

/* Verifies the documented sort contract: ".." first (if present), then
 * every directory entry alphabetically (case-insensitive), then every
 * file entry alphabetically (case-insensitive). Uses strcasecmp(), not
 * wcscoll()/towlower() (dir.c's own locale-aware comparison) - the two
 * agree for plain ASCII names regardless of the active locale (the "C"
 * locale's wcscoll() degenerates to code-point order, which matches
 * strcasecmp()'s ordering for same-case-folded ASCII strings), so this is
 * a faithful check without depending on any locale being installed. */
static void assert_sort_contract(size_t count, const DirEntryInfo *entries)
{
    size_t i = 0;
    if (count > 0 && strcmp(entries[0].name, "..") == 0) {
        ASSERT_TRUE(entries[0].is_dir);
        i = 1;
    }

    size_t dirs_end = i;
    while (dirs_end < count && entries[dirs_end].is_dir) {
        dirs_end++;
    }
    for (size_t k = i + 1; k < dirs_end; k++) {
        ASSERT_TRUE(strcasecmp(entries[k - 1].name, entries[k].name) <= 0);
    }
    for (size_t k = dirs_end; k < count; k++) {
        ASSERT_FALSE(entries[k].is_dir);
    }
    for (size_t k = dirs_end + 1; k < count; k++) {
        ASSERT_TRUE(strcasecmp(entries[k - 1].name, entries[k].name) <= 0);
    }
}

/* --- error handling ------------------------------------------------------- */

TEST(dir_list_null_out_params_rejected)
{
    DirEntryInfo *entries = NULL;
    size_t count = 0;
    ASSERT_EQ(dir_list("/tmp", NULL, &count), -1);
    ASSERT_EQ(dir_list("/tmp", &entries, NULL), -1);
}

TEST(dir_list_null_path_rejected)
{
    DirEntryInfo *entries = (DirEntryInfo *)1; /* sentinel, must be reset to NULL */
    size_t count = 99;
    ASSERT_EQ(dir_list(NULL, &entries, &count), -1);
    ASSERT_TRUE(entries == NULL);
    ASSERT_EQ(count, 0);
}

TEST(dir_list_nonexistent_directory_fails)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char missing[PATH_MAX];
    join_path(missing, sizeof(missing), base, "/does-not-exist");

    DirEntryInfo *entries = (DirEntryInfo *)1;
    size_t count = 99;
    ASSERT_EQ(dir_list(missing, &entries, &count), -1);
    ASSERT_TRUE(entries == NULL);
    ASSERT_EQ(count, 0);

    force_remove_tree(base);
}

/* --- basic listing and the sort contract --------------------------------- */

TEST(dir_list_empty_directory_has_only_dotdot)
{
    char base[64];
    make_temp_dir(base, sizeof(base));

    DirEntryInfo *entries = NULL;
    size_t count = 0;
    ASSERT_EQ(dir_list(base, &entries, &count), 0);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(entries[0].name, "..");
    ASSERT_TRUE(entries[0].is_dir);

    dir_list_free(entries);
    force_remove_tree(base);
}

TEST(dir_list_excludes_dot_but_keeps_dotdot)
{
    char base[64];
    make_temp_dir(base, sizeof(base));

    DirEntryInfo *entries = NULL;
    size_t count = 0;
    ASSERT_EQ(dir_list(base, &entries, &count), 0);
    ASSERT_TRUE(find_entry(entries, count, "..") != NULL);
    ASSERT_TRUE(find_entry(entries, count, ".") == NULL);

    dir_list_free(entries);
    force_remove_tree(base);
}

TEST(dir_list_sorts_dirs_before_files_case_insensitively)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    /* Deliberately mixed casing and an interleaved creation order, so a
     * correct result can only come from real sorting, not creation or
     * readdir() order. */
    const char *dirs[] = {"Zebra", "apple_dir", "Mango"};
    const char *files[] = {"banana.txt", "Cherry.txt", "apricot.txt"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char name[64], p[PATH_MAX];
        snprintf(name, sizeof(name), "/%s", dirs[i]);
        join_path(p, sizeof(p), base, name);
        ASSERT_EQ(mkdir(p, 0755), 0);
    }
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char name[64], p[PATH_MAX];
        snprintf(name, sizeof(name), "/%s", files[i]);
        join_path(p, sizeof(p), base, name);
        write_file(p, "x");
    }

    DirEntryInfo *entries = NULL;
    size_t count = 0;
    ASSERT_EQ(dir_list(base, &entries, &count), 0);
    ASSERT_EQ(count, 7); /* ".." + 3 dirs + 3 files */

    assert_sort_contract(count, entries);
    /* Pin down the exact expected order too, not just "some valid sort":
     * ".." , Mango, Zebra, apple_dir (dirs, case-insensitive alpha), then
     * apricot.txt, banana.txt, Cherry.txt (files, case-insensitive alpha). */
    ASSERT_STR_EQ(entries[0].name, "..");
    ASSERT_STR_EQ(entries[1].name, "apple_dir");
    ASSERT_STR_EQ(entries[2].name, "Mango");
    ASSERT_STR_EQ(entries[3].name, "Zebra");
    ASSERT_STR_EQ(entries[4].name, "apricot.txt");
    ASSERT_STR_EQ(entries[5].name, "banana.txt");
    ASSERT_STR_EQ(entries[6].name, "Cherry.txt");

    dir_list_free(entries);
    force_remove_tree(base);
}

TEST(dir_list_hidden_files_are_not_filtered)
{
    /* dir_list() only ever special-cases "." itself - filtering dotfiles
     * out of view (if any front-end wants that) is a caller/UI concern,
     * not this function's. */
    char base[64];
    make_temp_dir(base, sizeof(base));
    char hidden[PATH_MAX];
    join_path(hidden, sizeof(hidden), base, "/.hidden");
    write_file(hidden, "secret");

    DirEntryInfo *entries = NULL;
    size_t count = 0;
    ASSERT_EQ(dir_list(base, &entries, &count), 0);
    const DirEntryInfo *e = find_entry(entries, count, ".hidden");
    ASSERT_TRUE(e != NULL);
    ASSERT_FALSE(e->is_dir);

    dir_list_free(entries);
    force_remove_tree(base);
}

/* --- symlink handling (DT_LNK / DT_UNKNOWN -> stat() fallback) ------------ */

TEST(dir_list_symlink_to_directory_counts_as_directory)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char real_dir[PATH_MAX], link[PATH_MAX];
    join_path(real_dir, sizeof(real_dir), base, "/realdir");
    join_path(link, sizeof(link), base, "/linkdir");
    ASSERT_EQ(mkdir(real_dir, 0755), 0);
    ASSERT_EQ(symlink(real_dir, link), 0);

    DirEntryInfo *entries = NULL;
    size_t count = 0;
    ASSERT_EQ(dir_list(base, &entries, &count), 0);
    const DirEntryInfo *e = find_entry(entries, count, "linkdir");
    ASSERT_TRUE(e != NULL);
    ASSERT_TRUE(e->is_dir);

    dir_list_free(entries);
    force_remove_tree(base);
}

TEST(dir_list_symlink_to_file_counts_as_file)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char real_file[PATH_MAX], link[PATH_MAX];
    join_path(real_file, sizeof(real_file), base, "/realfile.txt");
    join_path(link, sizeof(link), base, "/linkfile.txt");
    write_file(real_file, "content");
    ASSERT_EQ(symlink(real_file, link), 0);

    DirEntryInfo *entries = NULL;
    size_t count = 0;
    ASSERT_EQ(dir_list(base, &entries, &count), 0);
    const DirEntryInfo *e = find_entry(entries, count, "linkfile.txt");
    ASSERT_TRUE(e != NULL);
    ASSERT_FALSE(e->is_dir);

    dir_list_free(entries);
    force_remove_tree(base);
}

TEST(dir_list_dangling_symlink_counts_as_file)
{
    /* stat() (not lstat()) on the dangling target fails - the fallback
     * expression's short-circuit must land on is_dir = 0, not leave it
     * uninitialized or crash. */
    char base[64];
    make_temp_dir(base, sizeof(base));
    char missing_target[PATH_MAX], link[PATH_MAX];
    join_path(missing_target, sizeof(missing_target), base, "/does-not-exist");
    join_path(link, sizeof(link), base, "/dangling");
    ASSERT_EQ(symlink(missing_target, link), 0);

    DirEntryInfo *entries = NULL;
    size_t count = 0;
    ASSERT_EQ(dir_list(base, &entries, &count), 0);
    const DirEntryInfo *e = find_entry(entries, count, "dangling");
    ASSERT_TRUE(e != NULL);
    ASSERT_FALSE(e->is_dir);

    dir_list_free(entries);
    force_remove_tree(base);
}

/* --- dynamic array growth -------------------------------------------------- */

TEST(dir_list_grows_beyond_initial_capacity)
{
    /* Initial capacity in dir_list() is 32 - create enough files to force
     * at least one realloc() growth, and verify every single one survived
     * intact (no truncation/corruption/duplicate/lost entry across the
     * grow). */
    char base[64];
    make_temp_dir(base, sizeof(base));
    enum { N = 40 };
    for (int i = 0; i < N; i++) {
        char name[32], path[PATH_MAX];
        snprintf(name, sizeof(name), "/file%02d", i);
        join_path(path, sizeof(path), base, name);
        write_file(path, "x");
    }

    DirEntryInfo *entries = NULL;
    size_t count = 0;
    ASSERT_EQ(dir_list(base, &entries, &count), 0);
    ASSERT_EQ(count, N + 1); /* + ".." */

    for (int i = 0; i < N; i++) {
        char name[32];
        snprintf(name, sizeof(name), "file%02d", i);
        const DirEntryInfo *e = find_entry(entries, count, name);
        ASSERT_TRUE(e != NULL);
        ASSERT_FALSE(e->is_dir);
    }
    assert_sort_contract(count, entries);

    dir_list_free(entries);
    force_remove_tree(base);
}

/* --- UTF-8 names (mbstowcs() success path) -------------------------------- */

TEST(dir_list_utf8_names_do_not_crash)
{
    /* Best-effort: C.UTF-8 is a real UTF-8 locale ships with glibc by
     * default (unlike a full language locale such as de_DE.UTF-8, which
     * may not be installed), so this actually exercises dir.c's
     * mbstowcs()/towlower()/wcscoll() path instead of the plain
     * strcasecmp() fallback - but skips cleanly if even that isn't
     * available, rather than failing on an environment quirk unrelated
     * to dir.c itself. Only existence/no-crash is asserted, not a
     * specific collation order: C.UTF-8 decodes UTF-8 correctly but its
     * ctype/collation tables are still just ASCII rules, so it wouldn't
     * demonstrate genuine language-specific case-folding either way. */
    if (setlocale(LC_ALL, "C.UTF-8") == NULL) {
        fprintf(stderr, "    SKIP (C.UTF-8 locale not available)\n");
        return;
    }

    char base[64];
    make_temp_dir(base, sizeof(base));
    char p1[PATH_MAX], p2[PATH_MAX];
    /* U+00C4 U+00E4 U+00DF (Ä, ä, ß), UTF-8 encoded. */
    join_path(p1, sizeof(p1), base, "/\xc3\x84pfel");
    join_path(p2, sizeof(p2), base, "/Stra\xc3\x9f" "e");
    write_file(p1, "x");
    write_file(p2, "x");

    DirEntryInfo *entries = NULL;
    size_t count = 0;
    ASSERT_EQ(dir_list(base, &entries, &count), 0);
    ASSERT_EQ(count, 3); /* ".." + 2 files */
    ASSERT_TRUE(find_entry(entries, count, "\xc3\x84pfel") != NULL);
    ASSERT_TRUE(find_entry(entries, count, "Stra\xc3\x9f" "e") != NULL);

    dir_list_free(entries);
    force_remove_tree(base);
    setlocale(LC_ALL, "C");
}

TEST(dir_list_free_null_is_safe)
{
    dir_list_free(NULL);
}

int main(void)
{
    TFM_RUN(dir_list_null_out_params_rejected);
    TFM_RUN(dir_list_null_path_rejected);
    TFM_RUN(dir_list_nonexistent_directory_fails);
    TFM_RUN(dir_list_empty_directory_has_only_dotdot);
    TFM_RUN(dir_list_excludes_dot_but_keeps_dotdot);
    TFM_RUN(dir_list_sorts_dirs_before_files_case_insensitively);
    TFM_RUN(dir_list_hidden_files_are_not_filtered);
    TFM_RUN(dir_list_symlink_to_directory_counts_as_directory);
    TFM_RUN(dir_list_symlink_to_file_counts_as_file);
    TFM_RUN(dir_list_dangling_symlink_counts_as_file);
    TFM_RUN(dir_list_grows_beyond_initial_capacity);
    TFM_RUN(dir_list_utf8_names_do_not_crash);
    TFM_RUN(dir_list_free_null_is_safe);
    return TFM_SUMMARY();
}
