#ifndef TFM_FILEOPS_H
#define TFM_FILEOPS_H

#include <stddef.h>

/* UI-independent feedback that fileops.c needs from the caller when
 * errors/conflicts occur. No dependency on screen.h or GTK, so both the
 * TUI and the GUI share fileops.c - the caller decides *how* to ask or
 * display, fileops.c only *that* and *when*. */
typedef enum {
    FILEOPS_CHOICE_SKIP,
    FILEOPS_CHOICE_RETRY,
    FILEOPS_CHOICE_ABORT,
    FILEOPS_CHOICE_OVERWRITE,
    /* on_overwrite answers that also apply to every later conflict in the
     * same batch (see batch.h). fileops.c itself treats them as plain
     * OVERWRITE/SKIP; remembering them is batch.c's job. */
    FILEOPS_CHOICE_OVERWRITE_ALL,
    FILEOPS_CHOICE_SKIP_ALL
} FileOpChoice;

/* Callbacks provided by the caller. ctx is passed through unchanged to
 * every callback (e.g. a pointer to UI-owned state) and may be NULL. */
typedef struct {
    /* Error popup with skip/retry/abort. */
    FileOpChoice (*on_error)(void *ctx, const char *title, const char *message);
    /* Destination already exists: skip/overwrite/abort, or skip/overwrite
     * all. */
    FileOpChoice (*on_overwrite)(void *ctx, const char *path);
    /* Progress display (0-100); item is e.g. the file currently being
     * processed. May be NULL if no progress display is wanted. */
    void (*on_progress)(void *ctx, const char *title, const char *item, double percent);
    void *ctx;
} FileOpCallbacks;

/* Copies src (file or directory, recursively) into dest_dir (src's name
 * is created there). Reports progress and errors/conflicts via cb. */
void fileops_copy(const char *src, const char *dest_dir, const FileOpCallbacks *cb);

/* Moves src (file or directory) into dest_dir, via rename() on the same
 * filesystem or copy-then-delete across filesystems (reporting progress
 * via cb). If the destination exists, asks like fileops_copy. Overwrite
 * on a type mismatch (file vs. directory) recursively deletes the whole
 * existing destination first, since neither rename() nor a merge can
 * reconcile it. */
void fileops_move(const char *src, const char *dest_dir, const FileOpCallbacks *cb);

/* Deletes path (file or directory, recursively) permanently - no undo.
 * On errors (e.g. missing permissions on a subdirectory), asks
 * cb->on_error (skip/retry/abort) - the caller should show its own
 * confirmation beforehand; fileops_delete() does not ask again. */
void fileops_delete(const char *path, const FileOpCallbacks *cb);

/* Moves path (file or directory) to the freedesktop.org "home trash"
 * ($XDG_DATA_HOME/Trash, falling back to ~/.local/share/Trash) -
 * recoverable via fileops_restore_trashed()/fileops_restore_last_trashed()
 * and by other trash-spec-aware file managers. Reports errors via cb like
 * fileops_delete(); the caller shows its own confirmation. Only the home
 * trash is implemented, not per-mount $topdir/.Trash-$uid: trashing from
 * another filesystem works via copy-then-delete but isn't fully
 * spec-compliant. Returns 1 on success and writes the item's name inside
 * the trash into trash_name_out (if non-NULL) - it differs from the
 * original name when " (1)" etc. was appended to avoid a collision. */
int fileops_trash(const char *path, const FileOpCallbacks *cb, char *trash_name_out,
                  size_t trash_name_out_size);

/* Restores the single most-recently-trashed item to its original
 * location - "undo my last delete", not a trash browser. Never
 * overwrites: fails via cb->on_error if the trash is empty, its metadata
 * can't be read, or something already exists at the original location.
 * On success, writes the restored path into restored_path_out (if
 * non-NULL) and returns 1; returns 0 on any failure. */
int fileops_restore_last_trashed(const FileOpCallbacks *cb, char *restored_path_out,
                                  size_t restored_path_out_size);

/* Restores the trash item named trash_name (as reported by
 * fileops_trash()) to its original location, with the same never-
 * overwrite rules and return convention as fileops_restore_last_trashed(). */
int fileops_restore_trashed(const char *trash_name, const FileOpCallbacks *cb, char *restored_path_out,
                            size_t restored_path_out_size);

#endif
