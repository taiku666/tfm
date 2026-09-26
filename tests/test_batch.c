/* _DEFAULT_SOURCE for mkdtemp(), setenv()/unsetenv(), strdup() (used by
 * tests/test_fs_helpers.h). */
#define _DEFAULT_SOURCE

#include "test.h"
#include "test_fs_helpers.h"
#include "../include/batch.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- scripted UI callbacks -----------------------------------------------
 * The front-end side of a batch: answers queued per callback, every call
 * logged, so tests can assert both the file tree and what the user was
 * asked. */

#define QUEUE_MAX 8
#define LOG_MAX 32

typedef struct {
    FileOpChoice overwrite_answers[QUEUE_MAX];
    int overwrite_len, overwrite_pos, overwrite_calls;
    FileOpChoice error_answers[QUEUE_MAX];
    int error_len, error_pos, error_calls;
    char progress_titles[LOG_MAX][128];
    double progress_percent[LOG_MAX];
    int progress_len;
} Ui;

static FileOpChoice ui_on_overwrite(void *ctx, const char *path)
{
    (void)path;
    Ui *ui = ctx;
    ui->overwrite_calls++;
    return ui->overwrite_pos < ui->overwrite_len ? ui->overwrite_answers[ui->overwrite_pos++]
                                                 : FILEOPS_CHOICE_ABORT;
}

static FileOpChoice ui_on_error(void *ctx, const char *title, const char *message)
{
    (void)title;
    (void)message;
    Ui *ui = ctx;
    ui->error_calls++;
    return ui->error_pos < ui->error_len ? ui->error_answers[ui->error_pos++] : FILEOPS_CHOICE_ABORT;
}

static void ui_on_progress(void *ctx, const char *title, const char *item, double percent)
{
    (void)item;
    Ui *ui = ctx;
    if (ui->progress_len < LOG_MAX) {
        snprintf(ui->progress_titles[ui->progress_len], sizeof(ui->progress_titles[0]), "%s", title);
        ui->progress_percent[ui->progress_len] = percent;
        ui->progress_len++;
    }
}

static FileOpCallbacks ui_callbacks(Ui *ui)
{
    memset(ui, 0, sizeof(*ui));
    FileOpCallbacks cb = {ui_on_error, ui_on_overwrite, ui_on_progress, ui};
    return cb;
}

/* A /tmp sandbox with src/ and dst/ and its own trash via XDG_DATA_HOME,
 * so trash tests never touch the real one. */
typedef struct {
    char base[64], src[PATH_MAX], dst[PATH_MAX], data[PATH_MAX];
    char paths[4][PATH_MAX];
    const char *path_ptrs[4];
} Sandbox;

static void sandbox_init(Sandbox *sb, int file_count)
{
    make_temp_dir(sb->base, sizeof(sb->base));
    join_path(sb->src, sizeof(sb->src), sb->base, "/src");
    join_path(sb->dst, sizeof(sb->dst), sb->base, "/dst");
    join_path(sb->data, sizeof(sb->data), sb->base, "/data");
    ASSERT_EQ(mkdir(sb->src, 0755), 0);
    ASSERT_EQ(mkdir(sb->dst, 0755), 0);
    ASSERT_EQ(mkdir(sb->data, 0755), 0);
    for (int i = 0; i < file_count; i++) {
        char name[16];
        snprintf(name, sizeof(name), "/f%d.txt", i);
        join_path(sb->paths[i], sizeof(sb->paths[i]), sb->src, name);
        write_file(sb->paths[i], "new\n");
        sb->path_ptrs[i] = sb->paths[i];
    }
}

static int dst_has(const Sandbox *sb, const char *name, const char *content)
{
    char path[PATH_MAX], buf[64] = "";
    join_path(path, sizeof(path), sb->dst, name);
    if (!path_exists(path)) {
        return 0;
    }
    read_file(path, buf, sizeof(buf));
    return content == NULL || strcmp(buf, content) == 0;
}

static void preplace_in_dst(const Sandbox *sb, const char *name)
{
    char path[PATH_MAX];
    join_path(path, sizeof(path), sb->dst, name);
    write_file(path, "old\n");
}

/* --- copy / move / delete ------------------------------------------------ */

TEST(copy_batch_copies_all_with_batch_progress)
{
    Sandbox sb;
    sandbox_init(&sb, 3);
    Ui ui;
    FileOpCallbacks cb = ui_callbacks(&ui);

    ASSERT_EQ(batch_run(BATCH_COPY, sb.path_ptrs, 3, sb.dst, &cb), 1);
    ASSERT_TRUE(dst_has(&sb, "/f0.txt", "new\n"));
    ASSERT_TRUE(dst_has(&sb, "/f1.txt", "new\n"));
    ASSERT_TRUE(dst_has(&sb, "/f2.txt", "new\n"));

    /* Titles carry the position, and the overall percentage never goes
     * backwards or past 100 across items. */
    ASSERT_TRUE(ui.progress_len >= 3);
    ASSERT_STR_EQ(ui.progress_titles[0], "Copying (1/3)");
    ASSERT_TRUE(strstr(ui.progress_titles[ui.progress_len - 1], "(3/3)") != NULL);
    for (int i = 1; i < ui.progress_len; i++) {
        ASSERT_TRUE(ui.progress_percent[i] >= ui.progress_percent[i - 1]);
        ASSERT_TRUE(ui.progress_percent[i] <= 100.0);
    }

    force_remove_tree(sb.base);
}

TEST(overwrite_all_is_asked_once)
{
    Sandbox sb;
    sandbox_init(&sb, 3);
    preplace_in_dst(&sb, "/f0.txt");
    preplace_in_dst(&sb, "/f1.txt");
    preplace_in_dst(&sb, "/f2.txt");
    Ui ui;
    FileOpCallbacks cb = ui_callbacks(&ui);
    ui.overwrite_answers[ui.overwrite_len++] = FILEOPS_CHOICE_OVERWRITE_ALL;

    ASSERT_EQ(batch_run(BATCH_COPY, sb.path_ptrs, 3, sb.dst, &cb), 1);
    ASSERT_EQ(ui.overwrite_calls, 1);
    ASSERT_TRUE(dst_has(&sb, "/f0.txt", "new\n"));
    ASSERT_TRUE(dst_has(&sb, "/f1.txt", "new\n"));
    ASSERT_TRUE(dst_has(&sb, "/f2.txt", "new\n"));

    force_remove_tree(sb.base);
}

TEST(skip_all_is_asked_once_and_keeps_existing)
{
    Sandbox sb;
    sandbox_init(&sb, 3);
    preplace_in_dst(&sb, "/f0.txt");
    preplace_in_dst(&sb, "/f2.txt");
    Ui ui;
    FileOpCallbacks cb = ui_callbacks(&ui);
    ui.overwrite_answers[ui.overwrite_len++] = FILEOPS_CHOICE_SKIP_ALL;

    ASSERT_EQ(batch_run(BATCH_COPY, sb.path_ptrs, 3, sb.dst, &cb), 1);
    ASSERT_EQ(ui.overwrite_calls, 1);
    ASSERT_TRUE(dst_has(&sb, "/f0.txt", "old\n"));
    ASSERT_TRUE(dst_has(&sb, "/f1.txt", "new\n")); /* no conflict: still copied */
    ASSERT_TRUE(dst_has(&sb, "/f2.txt", "old\n"));

    force_remove_tree(sb.base);
}

TEST(plain_overwrite_answer_does_not_stick)
{
    Sandbox sb;
    sandbox_init(&sb, 2);
    preplace_in_dst(&sb, "/f0.txt");
    preplace_in_dst(&sb, "/f1.txt");
    Ui ui;
    FileOpCallbacks cb = ui_callbacks(&ui);
    ui.overwrite_answers[ui.overwrite_len++] = FILEOPS_CHOICE_OVERWRITE;
    ui.overwrite_answers[ui.overwrite_len++] = FILEOPS_CHOICE_SKIP;

    ASSERT_EQ(batch_run(BATCH_COPY, sb.path_ptrs, 2, sb.dst, &cb), 1);
    ASSERT_EQ(ui.overwrite_calls, 2);
    ASSERT_TRUE(dst_has(&sb, "/f0.txt", "new\n"));
    ASSERT_TRUE(dst_has(&sb, "/f1.txt", "old\n"));

    force_remove_tree(sb.base);
}

TEST(abort_stops_the_whole_batch)
{
    Sandbox sb;
    sandbox_init(&sb, 3);
    preplace_in_dst(&sb, "/f0.txt");
    Ui ui;
    FileOpCallbacks cb = ui_callbacks(&ui);
    ui.overwrite_answers[ui.overwrite_len++] = FILEOPS_CHOICE_ABORT;

    ASSERT_EQ(batch_run(BATCH_COPY, sb.path_ptrs, 3, sb.dst, &cb), 0);
    ASSERT_TRUE(dst_has(&sb, "/f0.txt", "old\n"));
    ASSERT_FALSE(dst_has(&sb, "/f1.txt", NULL));
    ASSERT_FALSE(dst_has(&sb, "/f2.txt", NULL));

    force_remove_tree(sb.base);
}

TEST(abort_in_an_error_dialog_stops_the_whole_batch)
{
    Sandbox sb;
    sandbox_init(&sb, 3);
    /* The first item vanished before the batch reached it. */
    ASSERT_EQ(unlink(sb.paths[0]), 0);
    Ui ui;
    FileOpCallbacks cb = ui_callbacks(&ui);
    ui.error_answers[ui.error_len++] = FILEOPS_CHOICE_ABORT;

    ASSERT_EQ(batch_run(BATCH_COPY, sb.path_ptrs, 3, sb.dst, &cb), 0);
    ASSERT_EQ(ui.error_calls, 1);
    ASSERT_FALSE(dst_has(&sb, "/f1.txt", NULL));
    ASSERT_FALSE(dst_has(&sb, "/f2.txt", NULL));

    force_remove_tree(sb.base);
}

TEST(move_batch_moves_all)
{
    Sandbox sb;
    sandbox_init(&sb, 3);
    Ui ui;
    FileOpCallbacks cb = ui_callbacks(&ui);

    ASSERT_EQ(batch_run(BATCH_MOVE, sb.path_ptrs, 3, sb.dst, &cb), 1);
    for (int i = 0; i < 3; i++) {
        ASSERT_FALSE(path_exists(sb.paths[i]));
    }
    ASSERT_TRUE(dst_has(&sb, "/f0.txt", "new\n"));
    ASSERT_TRUE(dst_has(&sb, "/f2.txt", "new\n"));

    force_remove_tree(sb.base);
}

TEST(delete_batch_deletes_all_including_directories)
{
    Sandbox sb;
    sandbox_init(&sb, 2);
    join_path(sb.paths[2], sizeof(sb.paths[2]), sb.src, "/dir");
    ASSERT_EQ(mkdir(sb.paths[2], 0755), 0);
    char inner[PATH_MAX];
    join_path(inner, sizeof(inner), sb.paths[2], "/inner.txt");
    write_file(inner, "x");
    sb.path_ptrs[2] = sb.paths[2];
    Ui ui;
    FileOpCallbacks cb = ui_callbacks(&ui);

    ASSERT_EQ(batch_run(BATCH_DELETE, sb.path_ptrs, 3, NULL, &cb), 1);
    for (int i = 0; i < 3; i++) {
        ASSERT_FALSE(path_exists(sb.paths[i]));
    }

    force_remove_tree(sb.base);
}

/* --- trash + undo -------------------------------------------------------- */

TEST(trash_batch_undo_restores_the_whole_batch)
{
    Sandbox sb;
    sandbox_init(&sb, 3);
    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_data;
    save_and_set_env_var(&saved_data, "XDG_DATA_HOME", sb.data);
    batch_forget_undo();
    Ui ui;
    FileOpCallbacks cb = ui_callbacks(&ui);

    ASSERT_EQ(batch_run(BATCH_TRASH, sb.path_ptrs, 3, NULL, &cb), 1);
    for (int i = 0; i < 3; i++) {
        ASSERT_FALSE(path_exists(sb.paths[i]));
    }

    char msg[PATH_MAX + 32];
    ASSERT_EQ(batch_undo(&cb, msg, sizeof(msg)), 3);
    ASSERT_STR_EQ(msg, "Restored 3 items");
    for (int i = 0; i < 3; i++) {
        ASSERT_TRUE(path_exists(sb.paths[i]));
    }

    /* The batch is used up; the trash is now empty, so the fallback
     * reports that instead of restoring anything. */
    ASSERT_EQ(batch_undo(&cb, msg, sizeof(msg)), 0);
    ASSERT_STR_EQ(msg, "");
    ASSERT_EQ(ui.error_calls, 1);

    force_remove_tree(sb.base);
}

TEST(single_item_undo_names_the_path)
{
    Sandbox sb;
    sandbox_init(&sb, 1);
    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_data;
    save_and_set_env_var(&saved_data, "XDG_DATA_HOME", sb.data);
    batch_forget_undo();
    Ui ui;
    FileOpCallbacks cb = ui_callbacks(&ui);

    ASSERT_EQ(batch_run(BATCH_TRASH, sb.path_ptrs, 1, NULL, &cb), 1);
    char msg[PATH_MAX + 32], expected[PATH_MAX + 32];
    ASSERT_EQ(batch_undo(&cb, msg, sizeof(msg)), 1);
    snprintf(expected, sizeof(expected), "Restored: %s", sb.paths[0]);
    ASSERT_STR_EQ(msg, expected);

    force_remove_tree(sb.base);
}

/* After a restart there's no remembered batch: undo falls back to the
 * newest single item, which must be the last one trashed even though the
 * whole batch shares one timestamp second. */
TEST(undo_without_batch_restores_newest_single_item)
{
    Sandbox sb;
    sandbox_init(&sb, 3);
    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_data;
    save_and_set_env_var(&saved_data, "XDG_DATA_HOME", sb.data);
    batch_forget_undo();
    Ui ui;
    FileOpCallbacks cb = ui_callbacks(&ui);

    ASSERT_EQ(batch_run(BATCH_TRASH, sb.path_ptrs, 3, NULL, &cb), 1);
    batch_forget_undo(); /* simulates the restart */

    ASSERT_EQ(batch_undo(&cb, NULL, 0), 1);
    ASSERT_FALSE(path_exists(sb.paths[0]));
    ASSERT_FALSE(path_exists(sb.paths[1]));
    ASSERT_TRUE(path_exists(sb.paths[2]));

    force_remove_tree(sb.base);
}

TEST(failed_trash_batch_keeps_previous_undo)
{
    Sandbox sb;
    sandbox_init(&sb, 1);
    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_data;
    save_and_set_env_var(&saved_data, "XDG_DATA_HOME", sb.data);
    batch_forget_undo();
    Ui ui;
    FileOpCallbacks cb = ui_callbacks(&ui);

    ASSERT_EQ(batch_run(BATCH_TRASH, sb.path_ptrs, 1, NULL, &cb), 1);

    char missing[PATH_MAX];
    join_path(missing, sizeof(missing), sb.src, "/does-not-exist");
    const char *missing_ptr = missing;
    ui.error_answers[ui.error_len++] = FILEOPS_CHOICE_SKIP;
    batch_run(BATCH_TRASH, &missing_ptr, 1, NULL, &cb);

    ASSERT_EQ(batch_undo(&cb, NULL, 0), 1);
    ASSERT_TRUE(path_exists(sb.paths[0]));

    force_remove_tree(sb.base);
}

TEST(undo_skips_an_item_whose_spot_is_taken)
{
    Sandbox sb;
    sandbox_init(&sb, 2);
    SavedEnvVar __attribute__((cleanup(restore_env_var))) saved_data;
    save_and_set_env_var(&saved_data, "XDG_DATA_HOME", sb.data);
    batch_forget_undo();
    Ui ui;
    FileOpCallbacks cb = ui_callbacks(&ui);

    ASSERT_EQ(batch_run(BATCH_TRASH, sb.path_ptrs, 2, NULL, &cb), 1);
    write_file(sb.paths[0], "someone else's new file\n");
    ui.error_answers[ui.error_len++] = FILEOPS_CHOICE_SKIP;

    char msg[PATH_MAX + 32];
    ASSERT_EQ(batch_undo(&cb, msg, sizeof(msg)), 1);
    ASSERT_EQ(ui.error_calls, 1);
    char buf[64] = "";
    read_file(sb.paths[0], buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "someone else's new file\n"); /* never overwritten */
    ASSERT_TRUE(path_exists(sb.paths[1]));

    force_remove_tree(sb.base);
}

int main(void)
{
    TFM_RUN(copy_batch_copies_all_with_batch_progress);
    TFM_RUN(overwrite_all_is_asked_once);
    TFM_RUN(skip_all_is_asked_once_and_keeps_existing);
    TFM_RUN(plain_overwrite_answer_does_not_stick);
    TFM_RUN(abort_stops_the_whole_batch);
    TFM_RUN(abort_in_an_error_dialog_stops_the_whole_batch);
    TFM_RUN(move_batch_moves_all);
    TFM_RUN(delete_batch_deletes_all_including_directories);
    TFM_RUN(trash_batch_undo_restores_the_whole_batch);
    TFM_RUN(single_item_undo_names_the_path);
    TFM_RUN(undo_without_batch_restores_newest_single_item);
    TFM_RUN(failed_trash_batch_keeps_previous_undo);
    TFM_RUN(undo_skips_an_item_whose_spot_is_taken);
    batch_forget_undo();
    return TFM_SUMMARY();
}
