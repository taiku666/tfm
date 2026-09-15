#define _DEFAULT_SOURCE

#include "fileops.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tfm_common.h"

typedef struct {
    long long total_bytes;
    long long copied_bytes;
    int last_reported_percent;
} CopyProgress;

static double progress_percent(const CopyProgress *progress)
{
    if (progress->total_bytes <= 0) {
        return 100.0;
    }
    return 100.0 * (double)progress->copied_bytes / (double)progress->total_bytes;
}

/* Wrappers around cb->on_error/on_overwrite/on_progress that tolerate a
 * NULL cb or NULL callback fields (default to abort, or no progress). */
static FileOpChoice report_error(const FileOpCallbacks *cb, const char *title, const char *message)
{
    if (cb == NULL || cb->on_error == NULL) {
        return FILEOPS_CHOICE_ABORT;
    }
    return cb->on_error(cb->ctx, title, message);
}

static FileOpChoice report_overwrite(const FileOpCallbacks *cb, const char *path)
{
    if (cb == NULL || cb->on_overwrite == NULL) {
        return FILEOPS_CHOICE_ABORT;
    }
    return cb->on_overwrite(cb->ctx, path);
}

static void report_progress(const FileOpCallbacks *cb, const char *title, const char *item, double percent)
{
    if (cb == NULL || cb->on_progress == NULL) {
        return;
    }
    cb->on_progress(cb->ctx, title, item, percent);
}

/* Computes the total size of path (recursively) so copy_recursive() can
 * show a percentage; this requires a full pre-pass before the actual
 * copy pass. report_progress() is called here too (once per directory
 * entered, not per file, to avoid flooding the callback): without it, a
 * large tree (e.g. a repo with node_modules) caused a long silent pause
 * before any visible progress, and in the GUI the window appeared frozen
 * because g_main_context_iteration() is only pumped inside on_progress
 * (see gui_fileop_on_progress), which wasn't called during this pass. */
static long long compute_total_size(const char *path, const FileOpCallbacks *cb)
{
    struct stat st;
    /* lstat, not stat: a directory symlink pointing at an ancestor or
     * itself would otherwise recurse into stat() forever (stack overflow). */
    if (lstat(path, &st) != 0) {
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        return (long long)st.st_size;
    }

    long long total = 0;
    DIR *dp = opendir(path);
    if (dp == NULL) {
        return 0;
    }

    report_progress(cb, "Calculating size...", path, 0.0);

    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[PATH_MAX];
        if (path_join(child, sizeof(child), path, entry->d_name)) {
            total += compute_total_size(child, cb);
        }
    }

    closedir(dp);
    return total;
}

/* Forward declaration: copy_file() needs to remove an existing destination
 * on overwrite (including a directory-vs-file type mismatch), but
 * remove_existing_for_overwrite() is defined further below. */
static int remove_existing_for_overwrite(const char *dest, const struct stat *dest_st,
                                          const FileOpCallbacks *cb);

/* Copies a single file. Returns 1 on success or if the user skipped it,
 * 0 if the operation should abort. */
static int copy_file(const char *src_path, const char *dest_path, CopyProgress *progress,
                      const FileOpCallbacks *cb)
{
    if (strcmp(src_path, dest_path) == 0) {
        for (;;) {
            FileOpChoice choice = report_error(cb, "Error", "Source and destination are the same file");
            if (choice == FILEOPS_CHOICE_RETRY) {
                continue;
            }
            return choice == FILEOPS_CHOICE_SKIP;
        }
    }

    struct stat existing;
    if (lstat(dest_path, &existing) == 0) {
        /* src_path and dest_path can be different strings referring to the
         * same file (e.g. reachable via a symlinked directory), which the
         * strcmp() above misses. fopen(dest_path, "wb") would still
         * truncate it to 0 bytes while it's open for reading via "in" -
         * silent, irreversible data loss. Compare by inode to catch this. */
        struct stat src_st;
        if (stat(src_path, &src_st) == 0) {
            struct stat dest_real = existing;
            if (S_ISLNK(existing.st_mode)) {
                stat(dest_path, &dest_real);
            }
            if (dest_real.st_dev == src_st.st_dev && dest_real.st_ino == src_st.st_ino) {
                for (;;) {
                    FileOpChoice choice = report_error(cb, "Error", "Source and destination are the same file");
                    if (choice == FILEOPS_CHOICE_RETRY) {
                        continue;
                    }
                    return choice == FILEOPS_CHOICE_SKIP;
                }
            }
        }

        FileOpChoice choice = report_overwrite(cb, dest_path);
        if (choice != FILEOPS_CHOICE_OVERWRITE) {
            /* Fail safe: only an explicit OVERWRITE proceeds; any other
             * value (including an unexpected one from a buggy UI
             * callback, since this enum is shared with on_error) aborts
             * or skips rather than silently overwriting. */
            return choice == FILEOPS_CHOICE_SKIP;
        }
        /* Remove the existing entry. If it's a directory, unlink() would
         * fail (EISDIR) and fall through to O_EXCL failing EEXIST,
         * producing an infinite Retry loop with no indication of the real
         * cause - route through remove_existing_for_overwrite() (shared
         * with copy_recursive()'s type-mismatch handling) so a directory
         * is removed recursively instead. For a plain file/symlink this
         * still removes it by name (not following it): a subsequent
         * fopen(dest_path, "wb") would follow a symlink and
         * open/truncate its target instead - a different file than the
         * one just confirmed. That + O_EXCL below close this TOCTOU
         * window. */
        if (!remove_existing_for_overwrite(dest_path, &existing, cb)) {
            return 0;
        }
    }

    /* Tracks whether dest_path was already (re)created in this call: a
     * "Retry" after a write error should truncate/reuse the same file we
     * just created (O_TRUNC), but the very first creation uses O_EXCL so
     * open() fails if something appeared there in the race between the
     * overwrite check above and here (e.g. a newly created symlink)
     * instead of silently following it like fopen(..., "wb") would. */
    int dest_created = 0;

    for (;;) {
        FILE *in = fopen(src_path, "rb");
        if (in == NULL) {
            FileOpChoice choice = report_error(cb, "Error reading", src_path);
            if (choice == FILEOPS_CHOICE_RETRY) {
                continue;
            }
            return choice == FILEOPS_CHOICE_SKIP;
        }

        int open_flags = O_WRONLY | O_CREAT | (dest_created ? O_TRUNC : O_EXCL);
        int dest_fd = open(dest_path, open_flags, 0666);
        FILE *out = (dest_fd != -1) ? fdopen(dest_fd, "wb") : NULL;
        if (out == NULL) {
            if (dest_fd != -1) {
                close(dest_fd);
            }
            fclose(in);
            FileOpChoice choice = report_error(cb, "Error writing", dest_path);
            if (choice == FILEOPS_CHOICE_RETRY) {
                continue;
            }
            return choice == FILEOPS_CHOICE_SKIP;
        }
        dest_created = 1;

        int failed = 0;
        char buffer[65536];
        size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
            if (fwrite(buffer, 1, n, out) != n) {
                failed = 1;
                break;
            }
            progress->copied_bytes += (long long)n;

            int percent_int = (int)(progress_percent(progress) + 0.5);
            if (percent_int != progress->last_reported_percent) {
                report_progress(cb, "Copying...", dest_path, progress_percent(progress));
                progress->last_reported_percent = percent_int;
            }
        }

        if (!failed) {
            /* Preserve the source's permissions instead of the process
             * default (open() with 0666 & ~umask): otherwise a copied
             * executable loses its x-bit, or a private 0600 file (e.g. an
             * SSH key) ends up 0644 (world-readable) under a typical umask. */
            struct stat src_mode_st;
            if (stat(src_path, &src_mode_st) == 0) {
                fchmod(fileno(out), src_mode_st.st_mode & 07777);
            }
        }

        fclose(in);
        fclose(out);

        if (failed) {
            FileOpChoice choice = report_error(cb, "Error writing", dest_path);
            if (choice == FILEOPS_CHOICE_RETRY) {
                continue;
            }
            return choice == FILEOPS_CHOICE_SKIP;
        }

        return 1;
    }
}

/* Forward declaration: copy_recursive() needs to remove an existing
 * destination directory on a symlink/type-mismatch overwrite, but
 * delete_recursive() is defined further below. */
static int delete_recursive(const char *path, const FileOpCallbacks *cb);

/* Removes an existing entry at dest (file/symlink via unlink(), directory
 * recursively) after the user has confirmed overwrite via
 * report_overwrite(). Returns 1 to continue, 0 if aborted during a
 * recursive delete. */
static int remove_existing_for_overwrite(const char *dest, const struct stat *dest_st,
                                          const FileOpCallbacks *cb)
{
    if (S_ISDIR(dest_st->st_mode)) {
        return delete_recursive(dest, cb);
    }
    unlink(dest);
    return 1;
}

/* Copies src (file or directory) recursively to dest. Returns 1 to
 * continue, 0 if aborted. */
static int copy_recursive(const char *src, const char *dest, CopyProgress *progress,
                           const FileOpCallbacks *cb)
{
    struct stat st;
    /* lstat, not stat: handle symlinks themselves (copy as a link) rather
     * than following them, so a directory symlink pointing at an ancestor
     * or itself can't trigger infinite recursion (stack overflow). */
    for (;;) {
        if (lstat(src, &st) == 0) {
            break;
        }
        FileOpChoice choice = report_error(cb, "Error", src);
        if (choice == FILEOPS_CHOICE_RETRY) {
            continue;
        }
        return choice == FILEOPS_CHOICE_SKIP;
    }

    if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        ssize_t len;
        for (;;) {
            len = readlink(src, target, sizeof(target) - 1);
            if (len >= 0) {
                break;
            }
            FileOpChoice choice = report_error(cb, "Error reading link", src);
            if (choice == FILEOPS_CHOICE_RETRY) {
                continue;
            }
            return choice == FILEOPS_CHOICE_SKIP;
        }
        target[len] = '\0';

        struct stat dest_existing;
        if (lstat(dest, &dest_existing) == 0) {
            /* dest already exists (file, directory, or symlink) - ask like
             * for any other file instead of failing with EEXIST, so a
             * resumed copy over an already-copied symlink can be
             * skipped/overwritten/aborted rather than just erroring out. */
            FileOpChoice choice = report_overwrite(cb, dest);
            if (choice != FILEOPS_CHOICE_OVERWRITE) {
                return choice == FILEOPS_CHOICE_SKIP;
            }
            if (!remove_existing_for_overwrite(dest, &dest_existing, cb)) {
                return 0;
            }
        }

        for (;;) {
            if (symlink(target, dest) == 0) {
                return 1;
            }
            FileOpChoice choice = report_error(cb, "Error creating link", dest);
            if (choice == FILEOPS_CHOICE_RETRY) {
                continue;
            }
            return choice == FILEOPS_CHOICE_SKIP;
        }
    }

    if (!S_ISDIR(st.st_mode)) {
        return copy_file(src, dest, progress, cb);
    }

    for (;;) {
        /* mkdir()'s mode argument is masked by umask, so passing
         * st.st_mode here alone is not enough to preserve the source's
         * permissions - a 0777/0775 shared directory would silently come
         * out as 0755 under a typical umask 022. A follow-up chmod() below
         * (on the newly-created-here path only) closes that gap, mirroring
         * fchmod() in copy_file(). On EEXIST, an already-existing
         * destination directory's permissions are left untouched (no
         * downgrade of a deliberately set permission via merge). */
        if (mkdir(dest, st.st_mode & 07777) == 0) {
            chmod(dest, st.st_mode & 07777);
            break;
        }
        if (errno == EEXIST) {
            struct stat dest_existing;
            if (lstat(dest, &dest_existing) != 0) {
                /* dest vanished between the failed mkdir() and this lstat()
                 * (race) - just retry, mkdir() should succeed then. */
                continue;
            }
            if (S_ISDIR(dest_existing.st_mode)) {
                /* dest is already a directory - the normal merge case
                 * (e.g. resuming a partially copied tree), no need to ask. */
                break;
            }
            /* Type mismatch: dest exists but is a file or symlink, not a
             * directory. Tolerating this used to surface a confusing
             * ENOTDIR from the following opendir(dest) instead of asking;
             * now handled like any other conflict. */
            FileOpChoice choice = report_overwrite(cb, dest);
            if (choice != FILEOPS_CHOICE_OVERWRITE) {
                return choice == FILEOPS_CHOICE_SKIP;
            }
            unlink(dest);
            continue;
        }
        FileOpChoice choice = report_error(cb, "Cannot create directory", dest);
        if (choice == FILEOPS_CHOICE_RETRY) {
            continue;
        }
        return choice == FILEOPS_CHOICE_SKIP;
    }

    DIR *dp;
    for (;;) {
        dp = opendir(src);
        if (dp != NULL) {
            break;
        }
        FileOpChoice choice = report_error(cb, "Error reading", src);
        if (choice == FILEOPS_CHOICE_RETRY) {
            continue;
        }
        return choice == FILEOPS_CHOICE_SKIP;
    }

    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_src[PATH_MAX];
        char child_dest[PATH_MAX];
        if (!path_join(child_src, sizeof(child_src), src, entry->d_name) ||
            !path_join(child_dest, sizeof(child_dest), dest, entry->d_name)) {
            FileOpChoice choice = report_error(cb, "Path too long", entry->d_name);
            if (choice == FILEOPS_CHOICE_SKIP) {
                continue;
            }
            closedir(dp);
            return 0;
        }

        if (!copy_recursive(child_src, child_dest, progress, cb)) {
            closedir(dp);
            return 0;
        }
    }

    closedir(dp);
    return 1;
}

/* Checks whether dir equals src or (resolved via realpath, so symlink
 * detours are caught too) lies underneath src. Without this,
 * fileops_copy("/a/proj", "/a/proj/sub", cb) would keep copying the
 * freshly created destination into itself, a self-deepening copy that
 * only stops at PATH_MAX. Returns 0 on realpath() failure (e.g. dest
 * doesn't exist yet) rather than falsely blocking; the existing inode-
 * comparison data-loss guard covers the rest. */
static int dir_is_or_contains(const char *dir, const char *src)
{
    char real_dir[PATH_MAX];
    char real_src[PATH_MAX];
    if (realpath(dir, real_dir) == NULL || realpath(src, real_src) == NULL) {
        return 0;
    }
    size_t src_len = strlen(real_src);
    if (strncmp(real_dir, real_src, src_len) != 0) {
        return 0;
    }
    return real_dir[src_len] == '\0' || real_dir[src_len] == '/';
}

/* Strips trailing "/" (but keeps "/" itself). Without this, for
 * src == "/home/user/dir/", strrchr(src, '/') would hit the trailing
 * slash and leave base empty - dest would collapse to dest_dir itself,
 * merging dir's contents directly into dest_dir (instead of into a new
 * dest_dir/dir subfolder), silently overwriting same-named entries. */
static void strip_trailing_slashes(char *path)
{
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[--len] = '\0';
    }
}

void fileops_copy(const char *src, const char *dest_dir, const FileOpCallbacks *cb)
{
    char normalized_src[PATH_MAX];
    /* Unlike path_join() elsewhere in this file, a truncated src here
     * can't just be caught by the caller re-checking dest - it would
     * silently operate on a different, shorter path than the one passed
     * in, so check the return value explicitly instead of ignoring it. */
    if ((size_t)snprintf(normalized_src, sizeof(normalized_src), "%s", src) >=
        sizeof(normalized_src)) {
        report_error(cb, "Path too long", src);
        return;
    }
    strip_trailing_slashes(normalized_src);
    src = normalized_src;

    const char *base = strrchr(src, '/');
    base = (base != NULL) ? base + 1 : src;

    struct stat src_top_st;
    if (lstat(src, &src_top_st) == 0 && S_ISDIR(src_top_st.st_mode) &&
        dir_is_or_contains(dest_dir, src)) {
        report_error(cb, "Error", "Cannot copy a directory into itself");
        return;
    }

    char dest[PATH_MAX];
    if (!path_join(dest, sizeof(dest), dest_dir, base)) {
        report_error(cb, "Path too long", base);
        return;
    }

    if (strcmp(src, dest) == 0) {
        report_error(cb, "Error", "Source and destination are the same file");
        return;
    }

    CopyProgress progress;
    progress.total_bytes = compute_total_size(src, cb);
    progress.copied_bytes = 0;
    progress.last_reported_percent = -1;

    report_progress(cb, "Copying...", src, 0.0);

    copy_recursive(src, dest, &progress, cb);
}

/* Deletes path recursively, with error handling (skip/retry/abort)
 * instead of silently ignoring failures - important for trees with
 * read-only or inaccessible sub-entries. Returns 1 to continue, 0 if
 * aborted. */
static int delete_recursive(const char *path, const FileOpCallbacks *cb)
{
    struct stat st;
    for (;;) {
        if (lstat(path, &st) == 0) {
            break;
        }
        FileOpChoice choice = report_error(cb, "Error", path);
        if (choice == FILEOPS_CHOICE_RETRY) {
            continue;
        }
        return choice == FILEOPS_CHOICE_SKIP;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *dp;
        for (;;) {
            dp = opendir(path);
            if (dp != NULL) {
                break;
            }
            FileOpChoice choice = report_error(cb, "Cannot access directory", path);
            if (choice == FILEOPS_CHOICE_RETRY) {
                continue;
            }
            return choice == FILEOPS_CHOICE_SKIP;
        }

        struct dirent *entry;
        while ((entry = readdir(dp)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char child[PATH_MAX];
            if (!path_join(child, sizeof(child), path, entry->d_name)) {
                FileOpChoice choice = report_error(cb, "Path too long", entry->d_name);
                if (choice == FILEOPS_CHOICE_SKIP) {
                    continue;
                }
                closedir(dp);
                return 0;
            }
            if (!delete_recursive(child, cb)) {
                closedir(dp);
                return 0;
            }
        }
        closedir(dp);

        for (;;) {
            if (rmdir(path) == 0) {
                return 1;
            }
            FileOpChoice choice = report_error(cb, "Cannot remove directory", path);
            if (choice == FILEOPS_CHOICE_RETRY) {
                continue;
            }
            return choice == FILEOPS_CHOICE_SKIP;
        }
    }

    for (;;) {
        if (unlink(path) == 0) {
            return 1;
        }
        FileOpChoice choice = report_error(cb, "Cannot delete file", path);
        if (choice == FILEOPS_CHOICE_RETRY) {
            continue;
        }
        return choice == FILEOPS_CHOICE_SKIP;
    }
}

void fileops_move(const char *src, const char *dest_dir, const FileOpCallbacks *cb)
{
    char normalized_src[PATH_MAX];
    /* See the matching check in fileops_copy(): a truncated src here would
     * silently operate on a different, shorter path than the one passed
     * in, so check the return value explicitly instead of ignoring it. */
    if ((size_t)snprintf(normalized_src, sizeof(normalized_src), "%s", src) >=
        sizeof(normalized_src)) {
        report_error(cb, "Path too long", src);
        return;
    }
    strip_trailing_slashes(normalized_src);
    src = normalized_src;

    const char *base = strrchr(src, '/');
    base = (base != NULL) ? base + 1 : src;

    struct stat src_top_st;
    if (lstat(src, &src_top_st) == 0 && S_ISDIR(src_top_st.st_mode) &&
        dir_is_or_contains(dest_dir, src)) {
        report_error(cb, "Error", "Cannot move a directory into itself");
        return;
    }

    char dest[PATH_MAX];
    if (!path_join(dest, sizeof(dest), dest_dir, base)) {
        report_error(cb, "Path too long", base);
        return;
    }

    if (strcmp(src, dest) == 0) {
        report_error(cb, "Error", "Source and destination are the same file");
        return;
    }

    /* As in copy_file(): different path strings can reference the same
     * file (e.g. via a symlinked directory). Without this check, the
     * merge/copy path below would overwrite then delete the source -
     * total data loss. */
    struct stat src_lst, dest_lst;
    if (lstat(src, &src_lst) == 0 && lstat(dest, &dest_lst) == 0 &&
        src_lst.st_dev == dest_lst.st_dev && src_lst.st_ino == dest_lst.st_ino) {
        report_error(cb, "Error", "Source and destination are the same file");
        return;
    }

    struct stat existing;
    /* lstat, not stat, as in copy_file(): stat() would fail with ENOENT on
     * a dangling symlink at dest, skipping the overwrite branch entirely
     * and letting rename() silently replace the broken symlink without
     * asking - breaking the "asks before overwriting" contract documented
     * in fileops.h. lstat() catches the symlink itself (dangling or not)
     * and treats it as its own entry, consistent with copy_recursive(). */
    if (lstat(dest, &existing) == 0) {
        FileOpChoice choice = report_overwrite(cb, dest);
        if (choice != FILEOPS_CHOICE_OVERWRITE) {
            /* Fail safe: only an explicit OVERWRITE proceeds; anything
             * else (including an unexpected value) aborts. */
            return;
        }
        struct stat src_st;
        int src_is_dir = (lstat(src, &src_st) == 0 && S_ISDIR(src_st.st_mode));
        int dest_is_dir = S_ISDIR(existing.st_mode);

        if (src_is_dir && dest_is_dir) {
            /* Both sides are directories: don't delete the existing
             * destination wholesale - that would destroy files that exist
             * only in the destination, not the source (real data loss).
             * Merge instead, like fileops_copy() does (copy_recursive()
             * tolerates an existing destination directory and only asks
             * per file conflict), then remove the source to fulfill move
             * semantics. */
            CopyProgress progress;
            progress.total_bytes = compute_total_size(src, cb);
            progress.copied_bytes = 0;
            progress.last_reported_percent = -1;

            report_progress(cb, "Moving...", src, 0.0);

            if (copy_recursive(src, dest, &progress, cb)) {
                delete_recursive(src, cb);
            }
            return;
        }

        if (dest_is_dir || (src_is_dir && !dest_is_dir)) {
            /* Replacing a file with a directory or vice versa - rename()
             * would fail with ENOTDIR/EISDIR, and a merge makes no sense
             * for mismatched types. */
            if (!delete_recursive(dest, cb)) {
                return;
            }
        }
    }

    if (rename(src, dest) == 0) {
        return;
    }

    if (errno != EXDEV) {
        report_error(cb, "Error moving", strerror(errno));
        return;
    }

    /* Different filesystems: copy, then delete the original. */
    CopyProgress progress;
    progress.total_bytes = compute_total_size(src, cb);
    progress.copied_bytes = 0;
    progress.last_reported_percent = -1;

    report_progress(cb, "Moving...", src, 0.0);

    if (copy_recursive(src, dest, &progress, cb)) {
        delete_recursive(src, cb);
    }
}

void fileops_delete(const char *path, const FileOpCallbacks *cb)
{
    delete_recursive(path, cb);
}
