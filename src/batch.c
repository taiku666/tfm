#define _DEFAULT_SOURCE

#include "batch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tfm_common.h"

/* State shared by the wrapper callbacks below for one batch. */
typedef struct {
    const FileOpCallbacks *ui;
    int has_sticky_overwrite;
    FileOpChoice sticky_overwrite; /* OVERWRITE or SKIP, once "all" was chosen */
    int aborted;
    size_t index; /* item currently being processed */
    size_t count;
} BatchState;

/* Trash names of the last trash batch, for batch_undo(). In memory only:
 * the trash spec has no batch concept to persist it in. */
static char **g_undo_names = NULL;
static size_t g_undo_count = 0;

static FileOpChoice batch_on_error(void *ctx, const char *title, const char *message)
{
    BatchState *state = ctx;
    FileOpChoice choice = FILEOPS_CHOICE_ABORT;
    if (state->ui != NULL && state->ui->on_error != NULL) {
        choice = state->ui->on_error(state->ui->ctx, title, message);
    }
    if (choice == FILEOPS_CHOICE_ABORT) {
        state->aborted = 1;
    }
    return choice;
}

static FileOpChoice batch_on_overwrite(void *ctx, const char *path)
{
    BatchState *state = ctx;
    if (state->has_sticky_overwrite) {
        return state->sticky_overwrite;
    }

    FileOpChoice choice = FILEOPS_CHOICE_ABORT;
    if (state->ui != NULL && state->ui->on_overwrite != NULL) {
        choice = state->ui->on_overwrite(state->ui->ctx, path);
    }
    if (choice == FILEOPS_CHOICE_OVERWRITE_ALL || choice == FILEOPS_CHOICE_SKIP_ALL) {
        state->has_sticky_overwrite = 1;
        state->sticky_overwrite =
            (choice == FILEOPS_CHOICE_OVERWRITE_ALL) ? FILEOPS_CHOICE_OVERWRITE : FILEOPS_CHOICE_SKIP;
        return state->sticky_overwrite;
    }
    if (choice == FILEOPS_CHOICE_ABORT) {
        state->aborted = 1;
    }
    return choice;
}

/* Rescales one item's 0-100 into the batch's overall percentage and
 * tags the title with the item's position, e.g. "Copying (3/5)". */
static void batch_on_progress(void *ctx, const char *title, const char *item, double percent)
{
    BatchState *state = ctx;
    if (state->ui == NULL || state->ui->on_progress == NULL) {
        return;
    }
    if (state->count <= 1) {
        state->ui->on_progress(state->ui->ctx, title, item, percent);
        return;
    }

    char batch_title[128];
    snprintf(batch_title, sizeof(batch_title), "%s (%zu/%zu)", title, state->index + 1, state->count);
    double overall = ((double)state->index + percent / 100.0) / (double)state->count * 100.0;
    state->ui->on_progress(state->ui->ctx, batch_title, item, overall);
}

static const char *op_title(BatchOp op)
{
    switch (op) {
        case BATCH_COPY:  return "Copying";
        case BATCH_MOVE:  return "Moving";
        case BATCH_TRASH: return "Moving to trash";
        default:          return "Deleting";
    }
}

void batch_forget_undo(void)
{
    for (size_t i = 0; i < g_undo_count; i++) {
        free(g_undo_names[i]);
    }
    free(g_undo_names);
    g_undo_names = NULL;
    g_undo_count = 0;
}

int batch_run(BatchOp op, const char *const *paths, size_t count, const char *dest_dir,
              const FileOpCallbacks *ui)
{
    BatchState state = {.ui = ui, .count = count};
    FileOpCallbacks wrapped = {
        .on_error = batch_on_error,
        .on_overwrite = batch_on_overwrite,
        .on_progress = batch_on_progress,
        .ctx = &state,
    };

    char **trashed = NULL;
    size_t trashed_count = 0;
    if (op == BATCH_TRASH && count > 0) {
        trashed = calloc(count, sizeof(*trashed));
    }

    for (size_t i = 0; i < count && !state.aborted; i++) {
        state.index = i;
        /* Renames and trashing on one filesystem report no progress of
         * their own, so announce each item here; a copy's own progress
         * then refines it. */
        if (count > 1) {
            const char *slash = strrchr(paths[i], '/');
            batch_on_progress(&state, op_title(op), slash != NULL ? slash + 1 : paths[i], 0.0);
        }

        switch (op) {
            case BATCH_COPY:
                fileops_copy(paths[i], dest_dir, &wrapped);
                break;
            case BATCH_MOVE:
                fileops_move(paths[i], dest_dir, &wrapped);
                break;
            case BATCH_DELETE:
                fileops_delete(paths[i], &wrapped);
                break;
            case BATCH_TRASH: {
                char trash_name[PATH_MAX];
                if (fileops_trash(paths[i], &wrapped, trash_name, sizeof(trash_name)) && trashed != NULL) {
                    trashed[trashed_count] = strdup(trash_name);
                    if (trashed[trashed_count] != NULL) {
                        trashed_count++;
                    }
                }
                break;
            }
        }
    }

    /* A batch that trashed nothing leaves the previous undo in place. */
    if (trashed_count > 0) {
        batch_forget_undo();
        g_undo_names = trashed;
        g_undo_count = trashed_count;
    } else {
        free(trashed);
    }

    return !state.aborted;
}

size_t batch_undo(const FileOpCallbacks *ui, char *msg, size_t msg_size)
{
    BatchState state = {.ui = ui, .count = 1};
    FileOpCallbacks wrapped = {
        .on_error = batch_on_error,
        .on_overwrite = batch_on_overwrite,
        .on_progress = batch_on_progress,
        .ctx = &state,
    };
    char restored_path[PATH_MAX] = "";
    size_t restored = 0;

    if (msg != NULL && msg_size > 0) {
        msg[0] = '\0';
    }

    if (g_undo_count == 0) {
        restored = (size_t)fileops_restore_last_trashed(&wrapped, restored_path, sizeof(restored_path));
    } else {
        /* Newest first, the reverse of how they were trashed. An item
         * that can't be restored (e.g. its spot is taken again) was
         * already reported; the rest still come back unless the user
         * aborted. */
        for (size_t i = g_undo_count; i > 0 && !state.aborted; i--) {
            char path[PATH_MAX];
            if (fileops_restore_trashed(g_undo_names[i - 1], &wrapped, path, sizeof(path))) {
                snprintf(restored_path, sizeof(restored_path), "%s", path);
                restored++;
            }
        }
        batch_forget_undo();
    }

    if (msg != NULL && msg_size > 0) {
        if (restored == 1) {
            snprintf(msg, msg_size, "Restored: %s", restored_path);
        } else if (restored > 1) {
            snprintf(msg, msg_size, "Restored %zu items", restored);
        }
    }
    return restored;
}
