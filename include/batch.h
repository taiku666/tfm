#ifndef TFM_BATCH_H
#define TFM_BATCH_H

#include <stddef.h>

#include "fileops.h"

/* Runs one file operation over several items (a panel's marked entries,
 * or just the highlighted one) with the state both front-ends need
 * across items: an Overwrite all/Skip all answer covers every later
 * conflict, Abort stops the whole batch, progress spans the batch rather
 * than one item, and a trash batch is remembered for batch_undo().
 * UI-agnostic like fileops.c: the caller's callbacks do the asking. */

typedef enum {
    BATCH_COPY,
    BATCH_MOVE,
    BATCH_TRASH,
    BATCH_DELETE
} BatchOp;

/* Applies op to each of paths[0..count) - into dest_dir for COPY/MOVE,
 * which ignore it otherwise. Returns 1 if the batch ran to the end, 0 if
 * the user aborted it. */
int batch_run(BatchOp op, const char *const *paths, size_t count, const char *dest_dir,
              const FileOpCallbacks *ui);

/* Restores every item of this session's last BATCH_TRASH run. With none
 * (e.g. right after a restart - the trash itself has no notion of a
 * batch), restores the single most recently trashed item instead.
 * Returns the number of items restored and writes a summary for the user
 * into msg ("Restored: <path>" or "Restored 5 items"; "" if nothing was
 * restored - failures were already reported through ui). */
size_t batch_undo(const FileOpCallbacks *ui, char *msg, size_t msg_size);

/* Drops the remembered trash batch. */
void batch_forget_undo(void);

#endif
