#ifndef TFM_FILEOPS_H
#define TFM_FILEOPS_H

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
 * (skip/overwrite/abort). */
void fileops_move(const char *src, const char *dest_dir, const FileOpCallbacks *cb);

/* Deletes path (file or directory, recursively). On errors (e.g. missing
 * permissions on a subdirectory), asks cb->on_error (skip/retry/abort) -
 * the caller should show its own confirmation beforehand; fileops_delete()
 * does not ask again. */
void fileops_delete(const char *path, const FileOpCallbacks *cb);

#endif
