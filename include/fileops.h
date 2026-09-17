#ifndef TFM_FILEOPS_H
#define TFM_FILEOPS_H

#include <stddef.h>

/* UI-independent feedback that fileops.c needs from the caller when
 * errors/conflicts occur. Deliberately has no dependency on screen.h or
 * similar, so fileops.c can be reused by any frontend (terminal UI, later
 * e.g. a GTK GUI) - the caller decides *how* to ask/display, fileops.c
 * only *that* and *when*. */
typedef enum {
    FILEOPS_CHOICE_SKIP,
    FILEOPS_CHOICE_RETRY,
    FILEOPS_CHOICE_ABORT,
    FILEOPS_CHOICE_OVERWRITE
} FileOpChoice;

/* Callbacks provided by the caller. ctx is passed through unchanged to
 * every callback (e.g. a pointer to UI-owned state) and may be NULL. */
typedef struct {
    /* Error popup with skip/retry/abort. */
    FileOpChoice (*on_error)(void *ctx, const char *title, const char *message);
    /* Destination already exists: skip/overwrite/abort. */
    FileOpChoice (*on_overwrite)(void *ctx, const char *path);
    /* Progress display (0-100); item is e.g. the file currently being
     * processed. May be NULL if no progress display is wanted. */
    void (*on_progress)(void *ctx, const char *title, const char *item, double percent);
    void *ctx;
} FileOpCallbacks;

/* Copies src (file or directory, recursively) into dest_dir (src's name
 * is created there). Reports progress and errors/conflicts via cb. */
void fileops_copy(const char *src, const char *dest_dir, const FileOpCallbacks *cb);

/* Moves src (file or directory) into dest_dir. Uses rename() as a fast
 * path (same filesystem only); on failure due to different filesystems,
 * copies instead (reporting progress via cb) and then deletes the
 * original. If the destination already exists, asks like fileops_copy
 * (skip/overwrite/abort) - note that choosing Overwrite when src and the
 * existing destination are of different types (file vs. directory)
 * recursively deletes the whole existing destination tree first, since
 * rename()/a merge can't reconcile a type mismatch. */
void fileops_move(const char *src, const char *dest_dir, const FileOpCallbacks *cb);

/* Deletes path (file or directory, recursively) permanently - no undo.
 * On errors (e.g. missing permissions on a subdirectory), asks
 * cb->on_error (skip/retry/abort) - the caller should show its own
 * confirmation beforehand; fileops_delete() does not ask again. */
void fileops_delete(const char *path, const FileOpCallbacks *cb);

/* Moves path (file or directory) to the freedesktop.org "home trash"
 * ($XDG_DATA_HOME/Trash, falling back to ~/.local/share/Trash) instead
 * of deleting it - recoverable via fileops_restore_last_trashed(), and
 * interoperable with any other trash-spec-aware file manager (GNOME
 * Files, Dolphin, ...). Reports errors via cb like fileops_delete(); the
 * caller should still show its own confirmation beforehand. Only
 * implements the home-trash case, not a per-mount $topdir/.Trash-$uid
 * for other filesystems - a cross-device trash still works (via the
 * same copy-then-delete fallback fileops_move() uses), it just isn't
 * fully spec-compliant for that case. */
void fileops_trash(const char *path, const FileOpCallbacks *cb);

/* Restores the single most-recently-trashed item (by fileops_trash())
 * back to its original location. This is "undo my last delete", not a
 * trash browser - it only ever knows about the most recent item. Fails
 * cleanly via cb->on_error (reporting the reason) if the trash is empty,
 * its metadata can't be read, or something already exists at the
 * original location - never overwrites. On success, writes the restored
 * absolute path into restored_path_out (if non-NULL, capacity
 * restored_path_out_size) and returns 1; returns 0 on any failure. */
int fileops_restore_last_trashed(const FileOpCallbacks *cb, char *restored_path_out,
                                  size_t restored_path_out_size);

#endif
