/* _GNU_SOURCE (superset of _DEFAULT_SOURCE, same as src/fileops.c) for
 * mkdtemp(), setenv()/unsetenv(), strdup(), and unshare()/CLONE_NEWNS/
 * CLONE_NEWUSER (used by the EXDEV test below). */
#define _GNU_SOURCE

#include "test.h"
#include "test_fs_helpers.h"
#include "../include/fileops.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* --- scripted FileOpCallbacks mock -------------------------------------
 * fileops.c's public API is void-returning and reports everything through
 * FileOpCallbacks (fileops.h), so this mock scripts a queue of answers per
 * callback and logs every call, letting tests assert both "what happened
 * on disk" and "what was reported" deterministically. */

#define MOCK_MAX_QUEUE 16
#define MOCK_MAX_LOG 16
#define MOCK_LOG_MSG_LEN 512

typedef struct {
    FileOpChoice error_queue[MOCK_MAX_QUEUE];
    int error_queue_len;
    int error_queue_pos;
    int error_calls;
    char error_log[MOCK_MAX_LOG][MOCK_LOG_MSG_LEN];
    int error_log_len;

    FileOpChoice overwrite_queue[MOCK_MAX_QUEUE];
    int overwrite_queue_len;
    int overwrite_queue_pos;
    int overwrite_calls;
} MockCtx;

static void mock_reset(MockCtx *m)
{
    memset(m, 0, sizeof(*m));
}

static void mock_queue_error(MockCtx *m, FileOpChoice choice)
{
    if (m->error_queue_len < MOCK_MAX_QUEUE) {
        m->error_queue[m->error_queue_len++] = choice;
    }
}

static void mock_queue_overwrite(MockCtx *m, FileOpChoice choice)
{
    if (m->overwrite_queue_len < MOCK_MAX_QUEUE) {
        m->overwrite_queue[m->overwrite_queue_len++] = choice;
    }
}

static FileOpChoice mock_on_error(void *ctx, const char *title, const char *message)
{
    MockCtx *m = (MockCtx *)ctx;
    m->error_calls++;
    if (m->error_log_len < MOCK_MAX_LOG) {
        snprintf(m->error_log[m->error_log_len], MOCK_LOG_MSG_LEN, "%s: %s", title, message);
        m->error_log_len++;
    }
    if (m->error_queue_pos < m->error_queue_len) {
        return m->error_queue[m->error_queue_pos++];
    }
    /* Fail closed once the script runs out, same as report_error()'s own
     * NULL-cb default - keeps an under-scripted test deterministic instead
     * of hanging in a Retry loop. */
    return FILEOPS_CHOICE_ABORT;
}

static FileOpChoice mock_on_overwrite(void *ctx, const char *path)
{
    (void)path;
    MockCtx *m = (MockCtx *)ctx;
    m->overwrite_calls++;
    if (m->overwrite_queue_pos < m->overwrite_queue_len) {
        return m->overwrite_queue[m->overwrite_queue_pos++];
    }
    return FILEOPS_CHOICE_ABORT;
}

static FileOpCallbacks mock_callbacks(MockCtx *m)
{
    FileOpCallbacks cb;
    memset(&cb, 0, sizeof(cb));
    cb.on_error = mock_on_error;
    cb.on_overwrite = mock_on_overwrite;
    cb.on_progress = NULL;
    cb.ctx = m;
    return cb;
}

static int mock_log_contains(char log[][MOCK_LOG_MSG_LEN], int len, const char *needle)
{
    for (int i = 0; i < len; i++) {
        if (strstr(log[i], needle) != NULL) {
            return 1;
        }
    }
    return 0;
}

/* --- tests --------------------------------------------------------------- */

TEST(copy_file_basic)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char src[PATH_MAX], dest_dir[PATH_MAX], dest[PATH_MAX];
    join_path(src, sizeof(src), base, "/src.txt");
    join_path(dest_dir, sizeof(dest_dir), base, "/dest");
    write_file(src, "hello world");
    ASSERT_EQ(mkdir(dest_dir, 0755), 0);

    fileops_copy(src, dest_dir, NULL);

    join_path(dest, sizeof(dest), dest_dir, "/src.txt");
    char buf[64];
    ASSERT_TRUE(read_file(dest, buf, sizeof(buf)) >= 0);
    ASSERT_STR_EQ(buf, "hello world");
    ASSERT_TRUE(path_exists(src));

    force_remove_tree(base);
}

TEST(copy_directory_recursive)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char src_dir[PATH_MAX], nested[PATH_MAX], nested_file[PATH_MAX], dest_dir[PATH_MAX];
    join_path(src_dir, sizeof(src_dir), base, "/srcdir");
    join_path(nested, sizeof(nested), src_dir, "/nested");
    join_path(nested_file, sizeof(nested_file), nested, "/inner.txt");
    join_path(dest_dir, sizeof(dest_dir), base, "/dest");
    ASSERT_EQ(mkdir(src_dir, 0755), 0);
    ASSERT_EQ(mkdir(nested, 0755), 0);
    write_file(nested_file, "nested content");
    ASSERT_EQ(mkdir(dest_dir, 0755), 0);

    fileops_copy(src_dir, dest_dir, NULL);

    char copied_file[PATH_MAX];
    join_path(copied_file, sizeof(copied_file), dest_dir, "/srcdir/nested/inner.txt");
    char buf[64];
    ASSERT_TRUE(read_file(copied_file, buf, sizeof(buf)) >= 0);
    ASSERT_STR_EQ(buf, "nested content");
    ASSERT_TRUE(path_exists(nested_file));

    force_remove_tree(base);
}

TEST(move_same_filesystem)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char src[PATH_MAX], dest_dir[PATH_MAX], dest[PATH_MAX];
    join_path(src, sizeof(src), base, "/src.txt");
    join_path(dest_dir, sizeof(dest_dir), base, "/dest");
    write_file(src, "move me");
    ASSERT_EQ(mkdir(dest_dir, 0755), 0);

    fileops_move(src, dest_dir, NULL);

    join_path(dest, sizeof(dest), dest_dir, "/src.txt");
    char buf[64];
    ASSERT_TRUE(read_file(dest, buf, sizeof(buf)) >= 0);
    ASSERT_STR_EQ(buf, "move me");
    ASSERT_FALSE(path_exists(src));

    force_remove_tree(base);
}

TEST(overwrite_skip_leaves_both_untouched)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char src[PATH_MAX], dest_dir[PATH_MAX], dest[PATH_MAX];
    join_path(src, sizeof(src), base, "/file.txt");
    join_path(dest_dir, sizeof(dest_dir), base, "/dest");
    join_path(dest, sizeof(dest), dest_dir, "/file.txt");
    ASSERT_EQ(mkdir(dest_dir, 0755), 0);
    write_file(src, "NEW");
    write_file(dest, "OLD");

    MockCtx m;
    mock_reset(&m);
    mock_queue_overwrite(&m, FILEOPS_CHOICE_SKIP);
    FileOpCallbacks cb = mock_callbacks(&m);

    fileops_copy(src, dest_dir, &cb);

    char buf[64];
    ASSERT_TRUE(read_file(dest, buf, sizeof(buf)) >= 0);
    ASSERT_STR_EQ(buf, "OLD");
    ASSERT_TRUE(read_file(src, buf, sizeof(buf)) >= 0);
    ASSERT_STR_EQ(buf, "NEW");
    ASSERT_EQ(m.overwrite_calls, 1);

    force_remove_tree(base);
}

TEST(overwrite_replaces_dest)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char src[PATH_MAX], dest_dir[PATH_MAX], dest[PATH_MAX];
    join_path(src, sizeof(src), base, "/file.txt");
    join_path(dest_dir, sizeof(dest_dir), base, "/dest");
    join_path(dest, sizeof(dest), dest_dir, "/file.txt");
    ASSERT_EQ(mkdir(dest_dir, 0755), 0);
    write_file(src, "NEW");
    write_file(dest, "OLD");

    MockCtx m;
    mock_reset(&m);
    mock_queue_overwrite(&m, FILEOPS_CHOICE_OVERWRITE);
    FileOpCallbacks cb = mock_callbacks(&m);

    fileops_copy(src, dest_dir, &cb);

    char buf[64];
    ASSERT_TRUE(read_file(dest, buf, sizeof(buf)) >= 0);
    ASSERT_STR_EQ(buf, "NEW");

    force_remove_tree(base);
}

TEST(overwrite_null_callback_aborts)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char src[PATH_MAX], dest_dir[PATH_MAX], dest[PATH_MAX];
    join_path(src, sizeof(src), base, "/file.txt");
    join_path(dest_dir, sizeof(dest_dir), base, "/dest");
    join_path(dest, sizeof(dest), dest_dir, "/file.txt");
    ASSERT_EQ(mkdir(dest_dir, 0755), 0);
    write_file(src, "NEW");
    write_file(dest, "OLD");

    fileops_copy(src, dest_dir, NULL);

    char buf[64];
    ASSERT_TRUE(read_file(dest, buf, sizeof(buf)) >= 0);
    ASSERT_STR_EQ(buf, "OLD");

    force_remove_tree(base);
}

TEST(delete_skip_past_permission_error)
{
    if (geteuid() == 0) {
        fprintf(stderr, "    SKIP (running as root; permission-based test does not apply)\n");
        return;
    }

    char base[64];
    make_temp_dir(base, sizeof(base));
    char target[PATH_MAX], sibling[PATH_MAX], blocked[PATH_MAX], inner[PATH_MAX];
    join_path(target, sizeof(target), base, "/target");
    join_path(sibling, sizeof(sibling), target, "/sibling.txt");
    join_path(blocked, sizeof(blocked), target, "/blocked");
    join_path(inner, sizeof(inner), blocked, "/inner.txt");
    ASSERT_EQ(mkdir(target, 0755), 0);
    write_file(sibling, "keep me");
    ASSERT_EQ(mkdir(blocked, 0755), 0);
    write_file(inner, "trapped");
    ASSERT_EQ(chmod(blocked, 0000), 0);

    MockCtx m;
    mock_reset(&m);
    /* 1st on_error: opendir(blocked) fails (EACCES) -> SKIP, leaving
     * blocked's contents untouched. 2nd: the final rmdir(target) then
     * fails too, since target is non-empty (blocked is still inside it)
     * -> SKIP again, so the call reports success-with-skips for the rest
     * of the tree instead of hanging or aborting outright. */
    mock_queue_error(&m, FILEOPS_CHOICE_SKIP);
    mock_queue_error(&m, FILEOPS_CHOICE_SKIP);
    FileOpCallbacks cb = mock_callbacks(&m);

    fileops_delete(target, &cb);

    ASSERT_FALSE(path_exists(sibling));
    ASSERT_TRUE(path_exists(target));
    ASSERT_EQ(m.error_calls, 2);
    /* Restore +x on blocked before checking inner: lstat()/access() need
     * to traverse blocked to reach it at all, so leaving it at 0000 would
     * make inner look "gone" (EACCES) whether or not it actually is. */
    ASSERT_EQ(chmod(blocked, 0700), 0);
    ASSERT_TRUE(path_exists(inner));

    force_remove_tree(base);
}

TEST(delete_retry_then_abort_terminates)
{
    if (geteuid() == 0) {
        fprintf(stderr, "    SKIP (running as root; permission-based test does not apply)\n");
        return;
    }

    char base[64];
    make_temp_dir(base, sizeof(base));
    char blocked[PATH_MAX], inner[PATH_MAX];
    join_path(blocked, sizeof(blocked), base, "/blocked");
    join_path(inner, sizeof(inner), blocked, "/inner.txt");
    ASSERT_EQ(mkdir(blocked, 0755), 0);
    write_file(inner, "trapped");
    ASSERT_EQ(chmod(blocked, 0000), 0);

    MockCtx m;
    mock_reset(&m);
    mock_queue_error(&m, FILEOPS_CHOICE_RETRY);
    mock_queue_error(&m, FILEOPS_CHOICE_RETRY);
    mock_queue_error(&m, FILEOPS_CHOICE_RETRY);
    mock_queue_error(&m, FILEOPS_CHOICE_ABORT);
    FileOpCallbacks cb = mock_callbacks(&m);

    fileops_delete(blocked, &cb);

    /* Exactly 4 calls (3 retries + the final abort) proves the retry loop
     * stops as soon as the caller stops asking for another one, instead of
     * looping forever against a permission error that will never resolve
     * on its own. */
    ASSERT_EQ(m.error_calls, 4);
    ASSERT_TRUE(path_exists(blocked));
    /* Same reasoning as delete_skip_past_permission_error: restore +x on
     * blocked before checking inner, or EACCES on the traversal itself
     * would look identical to "inner was deleted". */
    ASSERT_EQ(chmod(blocked, 0700), 0);
    ASSERT_TRUE(path_exists(inner));

    force_remove_tree(base);
}

TEST(delete_recursion_depth_guard)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", base);
    /* > MAX_RECURSION_DEPTH (200, fileops.c) levels deep. */
    for (int i = 0; i < 210; i++) {
        char next[PATH_MAX];
        int n = join_path(next, sizeof(next), path, "/d");
        ASSERT_TRUE(n > 0 && (size_t)n < sizeof(next));
        ASSERT_EQ(mkdir(next, 0755), 0);
        snprintf(path, sizeof(path), "%s", next);
    }

    MockCtx m;
    mock_reset(&m);
    /* No queued responses: every on_error call defaults to ABORT, so the
     * first "Directory tree too deep" report aborts immediately instead of
     * cascading into ~200 further prompts as the abort unwinds back up
     * through every already-descended level. */
    FileOpCallbacks cb = mock_callbacks(&m);

    fileops_delete(base, &cb);

    ASSERT_EQ(m.error_calls, 1);
    ASSERT_TRUE(mock_log_contains(m.error_log, m.error_log_len, "too deep"));
    ASSERT_TRUE(path_exists(base));

    force_remove_tree(base);
}

TEST(copy_recursion_depth_guard)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char src_root[PATH_MAX], dest_dir[PATH_MAX], path[PATH_MAX];
    join_path(src_root, sizeof(src_root), base, "/deep");
    ASSERT_EQ(mkdir(src_root, 0755), 0);
    snprintf(path, sizeof(path), "%s", src_root);
    for (int i = 0; i < 210; i++) {
        char next[PATH_MAX];
        int n = join_path(next, sizeof(next), path, "/d");
        ASSERT_TRUE(n > 0 && (size_t)n < sizeof(next));
        ASSERT_EQ(mkdir(next, 0755), 0);
        snprintf(path, sizeof(path), "%s", next);
    }
    join_path(dest_dir, sizeof(dest_dir), base, "/dest");
    ASSERT_EQ(mkdir(dest_dir, 0755), 0);

    MockCtx m;
    mock_reset(&m);
    FileOpCallbacks cb = mock_callbacks(&m);

    fileops_copy(src_root, dest_dir, &cb);

    ASSERT_EQ(m.error_calls, 1);
    ASSERT_TRUE(mock_log_contains(m.error_log, m.error_log_len, "too deep"));

    force_remove_tree(base);
}

TEST(copy_symlink_target_truncation_detected)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char link[PATH_MAX], dest_dir[PATH_MAX];
    join_path(link, sizeof(link), base, "/biglink");
    join_path(dest_dir, sizeof(dest_dir), base, "/dest");
    ASSERT_EQ(mkdir(dest_dir, 0755), 0);

    /* The symlink target need not exist - readlink() just returns whatever
     * string symlink() stored. copy_recursive_impl() must detect the case
     * where that string exactly fills the PATH_MAX-1 read buffer
     * (readlink()'s undocumented truncation signal) instead of silently
     * copying a truncated target. */
    char long_target[PATH_MAX];
    memset(long_target, 'a', sizeof(long_target) - 1);
    long_target[sizeof(long_target) - 1] = '\0';
    ASSERT_EQ(symlink(long_target, link), 0);

    MockCtx m;
    mock_reset(&m);
    FileOpCallbacks cb = mock_callbacks(&m);

    fileops_copy(link, dest_dir, &cb);

    ASSERT_EQ(m.error_calls, 1);
    ASSERT_TRUE(mock_log_contains(m.error_log, m.error_log_len, "too long"));

    force_remove_tree(base);
}

TEST(dangerous_root_paths_rejected_without_mutation)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char dest_dir[PATH_MAX];
    join_path(dest_dir, sizeof(dest_dir), base, "/dest");
    ASSERT_EQ(mkdir(dest_dir, 0755), 0);

    const char *dangerous[] = {"", "/", ".", ".."};
    for (size_t i = 0; i < sizeof(dangerous) / sizeof(dangerous[0]); i++) {
        MockCtx m;
        mock_reset(&m);
        FileOpCallbacks cb = mock_callbacks(&m);
        fileops_copy(dangerous[i], dest_dir, &cb);
        ASSERT_EQ(m.error_calls, 1);

        mock_reset(&m);
        fileops_move(dangerous[i], dest_dir, &cb);
        ASSERT_EQ(m.error_calls, 1);

        mock_reset(&m);
        fileops_delete(dangerous[i], &cb);
        ASSERT_EQ(m.error_calls, 1);

        mock_reset(&m);
        fileops_trash(dangerous[i], &cb);
        ASSERT_EQ(m.error_calls, 1);
    }

    /* dest_dir is an ordinary, unrelated directory - proof that none of
     * the calls above actually touched the filesystem despite being
     * handed "", "/", ".", or "..". */
    ASSERT_TRUE(is_dir(dest_dir));
    DIR *dp = opendir(dest_dir);
    ASSERT_TRUE(dp != NULL);
    int entries = 0;
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        entries++;
    }
    closedir(dp);
    ASSERT_EQ(entries, 0);

    force_remove_tree(base);
}

TEST(trash_round_trip)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char home[PATH_MAX], xdg[PATH_MAX], item[PATH_MAX];
    join_path(home, sizeof(home), base, "/home");
    join_path(xdg, sizeof(xdg), base, "/xdg-data");
    join_path(item, sizeof(item), base, "/note.txt");
    ASSERT_EQ(mkdir(home, 0755), 0);
    ASSERT_EQ(mkdir(xdg, 0755), 0);
    write_file(item, "keepsake");

    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_home;
    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_xdg;
    save_and_set_env_var(&saved_home, "HOME", home);
    save_and_set_env_var(&saved_xdg, "XDG_DATA_HOME", xdg);

    MockCtx m;
    mock_reset(&m);
    FileOpCallbacks cb = mock_callbacks(&m);

    fileops_trash(item, &cb);
    ASSERT_FALSE(path_exists(item));
    ASSERT_EQ(m.error_calls, 0);

    char restored[PATH_MAX];
    ASSERT_EQ(fileops_restore_last_trashed(&cb, restored, sizeof(restored)), 1);
    ASSERT_STR_EQ(restored, item);

    char buf[64];
    ASSERT_TRUE(read_file(item, buf, sizeof(buf)) >= 0);
    ASSERT_STR_EQ(buf, "keepsake");

    /* A second restore attempt must fail cleanly - the trash is empty
     * again, not stuck repeating the same item. */
    mock_reset(&m);
    ASSERT_EQ(fileops_restore_last_trashed(&cb, NULL, 0), 0);
    ASSERT_EQ(m.error_calls, 1);

    force_remove_tree(base);
}

TEST(trash_restore_never_clobbers_occupied_destination)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char home[PATH_MAX], xdg[PATH_MAX], item[PATH_MAX];
    join_path(home, sizeof(home), base, "/home");
    join_path(xdg, sizeof(xdg), base, "/xdg-data");
    join_path(item, sizeof(item), base, "/note.txt");
    ASSERT_EQ(mkdir(home, 0755), 0);
    ASSERT_EQ(mkdir(xdg, 0755), 0);
    write_file(item, "original");

    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_home;
    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_xdg;
    save_and_set_env_var(&saved_home, "HOME", home);
    save_and_set_env_var(&saved_xdg, "XDG_DATA_HOME", xdg);

    MockCtx m;
    mock_reset(&m);
    FileOpCallbacks cb = mock_callbacks(&m);

    fileops_trash(item, &cb);
    ASSERT_FALSE(path_exists(item));

    /* Something else now occupies the original spot - a real file the
     * user created after the trash, unrelated to the trashed one. */
    write_file(item, "someone else's file");

    mock_reset(&m);
    int ok = fileops_restore_last_trashed(&cb, NULL, 0);
    ASSERT_EQ(ok, 0);
    ASSERT_EQ(m.error_calls, 1);

    char buf[64];
    ASSERT_TRUE(read_file(item, buf, sizeof(buf)) >= 0);
    ASSERT_STR_EQ(buf, "someone else's file");

    force_remove_tree(base);
}

TEST(trash_name_collision_keeps_both)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char home[PATH_MAX], xdg[PATH_MAX];
    char dir_a[PATH_MAX], dir_b[PATH_MAX], item_a[PATH_MAX], item_b[PATH_MAX];
    join_path(home, sizeof(home), base, "/home");
    join_path(xdg, sizeof(xdg), base, "/xdg-data");
    join_path(dir_a, sizeof(dir_a), base, "/a");
    join_path(dir_b, sizeof(dir_b), base, "/b");
    join_path(item_a, sizeof(item_a), dir_a, "/dup.txt");
    join_path(item_b, sizeof(item_b), dir_b, "/dup.txt");
    ASSERT_EQ(mkdir(home, 0755), 0);
    ASSERT_EQ(mkdir(xdg, 0755), 0);
    ASSERT_EQ(mkdir(dir_a, 0755), 0);
    ASSERT_EQ(mkdir(dir_b, 0755), 0);
    write_file(item_a, "first");
    write_file(item_b, "second");

    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_home;
    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_xdg;
    save_and_set_env_var(&saved_home, "HOME", home);
    save_and_set_env_var(&saved_xdg, "XDG_DATA_HOME", xdg);

    MockCtx m;
    mock_reset(&m);
    FileOpCallbacks cb = mock_callbacks(&m);

    fileops_trash(item_a, &cb);
    fileops_trash(item_b, &cb);
    ASSERT_EQ(m.error_calls, 0);

    char trash_files[PATH_MAX];
    join_path(trash_files, sizeof(trash_files), xdg, "/Trash/files");
    DIR *dp = opendir(trash_files);
    ASSERT_TRUE(dp != NULL);
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (strstr(entry->d_name, "dup.txt") != NULL) {
            count++;
        }
    }
    closedir(dp);
    /* unique_trash_name() must have given the two same-basename items
     * distinct names rather than the second silently overwriting the
     * first (CR5-1-adjacent regression coverage). */
    ASSERT_EQ(count, 2);

    /* Both independently restorable, most-recently-trashed first. */
    ASSERT_EQ(fileops_restore_last_trashed(&cb, NULL, 0), 1);
    ASSERT_EQ(fileops_restore_last_trashed(&cb, NULL, 0), 1);
    ASSERT_TRUE(path_exists(item_a));
    ASSERT_TRUE(path_exists(item_b));

    force_remove_tree(base);
}

TEST(move_exdev_fallback_copies_then_deletes)
{
    char base[64];
    make_temp_dir(base, sizeof(base));
    char other_fs[PATH_MAX];
    join_path(other_fs, sizeof(other_fs), base, "/otherfs");
    ASSERT_EQ(mkdir(other_fs, 0755), 0);

    /* All namespace/mount manipulation happens in a forked child so it can
     * never affect this test binary's own process (capabilities, mount
     * table) for tests that run afterward - this test is best-effort and
     * skips cleanly wherever unprivileged user/mount namespaces aren't
     * available, same spirit as `make lint` skipping without cppcheck. */
    uid_t real_uid = getuid();
    gid_t real_gid = getgid();
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        if (unshare(CLONE_NEWNS | CLONE_NEWUSER) != 0) {
            _exit(2); /* tells the parent to report this as skipped */
        }
        /* Map this (still-unprivileged) uid/gid to 0 inside the new user
         * namespace - the standard unprivileged-userns dance, required
         * before /proc/self/gid_map will accept a write at all. Skipping
         * this leaves the process's identity unmapped/overflow inside the
         * namespace, which was observed to make a freshly mounted tmpfs's
         * files fail open()/fopen() with a spurious EOVERFLOW - unrelated
         * to fileops.c, a namespace-identity quirk of this kernel/glibc,
         * but worth mapping properly anyway since that's what a real
         * unprivileged mount namespace setup is supposed to do. */
        int map_fd = open("/proc/self/setgroups", O_WRONLY);
        if (map_fd >= 0) {
            if (write(map_fd, "deny", 4) < 0) {
                /* Best-effort: some kernels don't need this file at all;
                 * a failure here doesn't necessarily doom the uid/gid_map
                 * writes below. */
            }
            close(map_fd);
        }
        char map_buf[64];
        int uid_map_fd = open("/proc/self/uid_map", O_WRONLY);
        int gid_map_fd = open("/proc/self/gid_map", O_WRONLY);
        if (uid_map_fd < 0 || gid_map_fd < 0) {
            if (uid_map_fd >= 0) {
                close(uid_map_fd);
            }
            if (gid_map_fd >= 0) {
                close(gid_map_fd);
            }
            _exit(2);
        }
        int map_len = snprintf(map_buf, sizeof(map_buf), "0 %d 1\n", (int)real_uid);
        if (write(uid_map_fd, map_buf, (size_t)map_len) < 0) {
            close(uid_map_fd);
            close(gid_map_fd);
            _exit(2);
        }
        close(uid_map_fd);
        map_len = snprintf(map_buf, sizeof(map_buf), "0 %d 1\n", (int)real_gid);
        if (write(gid_map_fd, map_buf, (size_t)map_len) < 0) {
            close(gid_map_fd);
            _exit(2);
        }
        close(gid_map_fd);

        if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0 ||
            mount("tmpfs", other_fs, "tmpfs", 0, "size=1m") != 0) {
            _exit(2);
        }

        char src[PATH_MAX], dest[PATH_MAX];
        join_path(src, sizeof(src), base, "/src.txt");
        join_path(dest, sizeof(dest), other_fs, "/src.txt");
        write_file(src, "cross device");

        fileops_move(src, other_fs, NULL);

        char buf[64];
        int ok = read_file(dest, buf, sizeof(buf)) >= 0 && strcmp(buf, "cross device") == 0 &&
                 !path_exists(src);
        _exit(ok ? 0 : 1);
    }

    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 2) {
        fprintf(stderr, "    SKIP (unprivileged user/mount namespaces unavailable here)\n");
    } else {
        ASSERT_TRUE(WIFEXITED(status));
        ASSERT_EQ(WEXITSTATUS(status), 0);
    }

    force_remove_tree(base);
}

int main(void)
{
    /* copy_recursion_depth_guard below needs to recurse past
     * MAX_RECURSION_DEPTH (200, fileops.c) real stack frames before the
     * guard ever gets a chance to fire - each copy_recursive_impl() frame
     * holds three PATH_MAX buffers already (fileops.c's own comment notes
     * this leaves "a comfortable margin below a real overflow on the
     * default 8MB stack" for a normal optimized build), but an
     * ASan+-O0 build (`make asan-test`) inflates every frame far beyond
     * that margin and was observed to genuinely stack-overflow around
     * depth ~185 - before the test ever reaches the code path it exists
     * to exercise. Raising the soft limit here (harmless for every other
     * test) fixes the test environment instead of weakening the test. */
    struct rlimit stack_limit;
    if (getrlimit(RLIMIT_STACK, &stack_limit) == 0) {
        rlim_t wanted = 64 * 1024 * 1024;
        if (stack_limit.rlim_max == RLIM_INFINITY || wanted <= stack_limit.rlim_max) {
            stack_limit.rlim_cur = wanted;
            setrlimit(RLIMIT_STACK, &stack_limit);
        }
    }

    TFM_RUN(copy_file_basic);
    TFM_RUN(copy_directory_recursive);
    TFM_RUN(move_same_filesystem);
    TFM_RUN(overwrite_skip_leaves_both_untouched);
    TFM_RUN(overwrite_replaces_dest);
    TFM_RUN(overwrite_null_callback_aborts);
    TFM_RUN(delete_skip_past_permission_error);
    TFM_RUN(delete_retry_then_abort_terminates);
    TFM_RUN(delete_recursion_depth_guard);
    TFM_RUN(copy_recursion_depth_guard);
    TFM_RUN(copy_symlink_target_truncation_detected);
    TFM_RUN(dangerous_root_paths_rejected_without_mutation);
    TFM_RUN(trash_round_trip);
    TFM_RUN(trash_restore_never_clobbers_occupied_destination);
    TFM_RUN(trash_name_collision_keeps_both);
    TFM_RUN(move_exdev_fallback_copies_then_deletes);
    return TFM_SUMMARY();
}
