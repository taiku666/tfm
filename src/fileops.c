/* _GNU_SOURCE (a superset of _DEFAULT_SOURCE) for renameat2()/
 * RENAME_NOREPLACE, used by move_to_exact_dest() below to close a TOCTOU
 * race in the trash implementation's rename() fast path. */
#define _GNU_SOURCE

#include "fileops.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "tfm_common.h"

/* Ceiling on directory-tree recursion depth for compute_total_size(),
 * copy_recursive(), and delete_recursive(). All three recurse one stack
 * frame per directory level with no depth check, so a sufficiently deep
 * tree (rare, but not impossible - a deeply nested build cache, or a
 * maliciously/accidentally constructed tree) could exhaust the stack and
 * crash instead of failing cleanly. copy_recursive()'s frame alone holds
 * three PATH_MAX (4096-byte) buffers, so even a generous limit here still
 * leaves a comfortable margin below a real overflow on the default 8MB
 * stack. */
#define MAX_RECURSION_DEPTH 200

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
static long long compute_total_size_impl(const char *path, const FileOpCallbacks *cb, int depth)
{
    /* Same "just an estimate, fail soft" treatment as an opendir()
     * failure below - a tree deep enough to hit this is already an edge
     * case for a progress-percent pre-pass, not worth surfacing a dialog
     * over. copy_recursive()/delete_recursive() (below) hit the real data
     * they operate on, so THEY report this instead of silently
     * undercounting. */
    if (depth > MAX_RECURSION_DEPTH) {
        return 0;
    }

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
            total += compute_total_size_impl(child, cb, depth + 1);
        }
    }

    closedir(dp);
    return total;
}

static long long compute_total_size(const char *path, const FileOpCallbacks *cb)
{
    return compute_total_size_impl(path, cb, 0);
}

/* Forward declaration: copy_file() needs to remove an existing destination
 * on overwrite (including a directory-vs-file type mismatch), but
 * remove_existing_for_overwrite() is defined further below. */
static int remove_existing_for_overwrite(const char *dest, const struct stat *dest_st,
                                          const FileOpCallbacks *cb);

/* Copies a single file. Returns 1 on success or Skip (setting *had_skip
 * on Skip), 0 on Abort. had_skip lets move() tell "fully copied" apart
 * from "partially skipped", so it never deletes a skipped source file. */
static int copy_file(const char *src_path, const char *dest_path, CopyProgress *progress,
                      const FileOpCallbacks *cb, int *had_skip)
{
    if (strcmp(src_path, dest_path) == 0) {
        for (;;) {
            FileOpChoice choice = report_error(cb, "Error", "Source and destination are the same file");
            if (choice == FILEOPS_CHOICE_RETRY) {
                continue;
            }
            if (choice == FILEOPS_CHOICE_SKIP) {
                *had_skip = 1;
                return 1;
            }
            return 0;
        }
    }

    /* Stat src_path once up front and reuse it below both for the
     * same-inode check and for the post-copy fchmod() - avoids re-stat()ing
     * a path that hasn't changed between the two uses. */
    struct stat src_st;
    int have_src_st = (stat(src_path, &src_st) == 0);

    struct stat existing;
    if (lstat(dest_path, &existing) == 0) {
        /* src_path and dest_path can be different strings referring to the
         * same file (e.g. reachable via a symlinked directory), which the
         * strcmp() above misses. fopen(dest_path, "wb") would still
         * truncate it to 0 bytes while it's open for reading via "in" -
         * silent, irreversible data loss. Compare by inode to catch this. */
        if (have_src_st) {
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
                    if (choice == FILEOPS_CHOICE_SKIP) {
                        *had_skip = 1;
                        return 1;
                    }
                    return 0;
                }
            }
        }

        FileOpChoice choice = report_overwrite(cb, dest_path);
        if (choice != FILEOPS_CHOICE_OVERWRITE) {
            /* Fail safe: only an explicit OVERWRITE proceeds; any other
             * value (including an unexpected one from a buggy UI
             * callback, since this enum is shared with on_error) aborts
             * or skips rather than silently overwriting. */
            if (choice == FILEOPS_CHOICE_SKIP) {
                *had_skip = 1;
                return 1;
            }
            return 0;
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
            int saved_errno = errno;
            char msg[PATH_MAX + 128];
            snprintf(msg, sizeof(msg), "%s: %s", src_path, strerror(saved_errno));
            FileOpChoice choice = report_error(cb, "Error reading", msg);
            if (choice == FILEOPS_CHOICE_RETRY) {
                continue;
            }
            if (choice == FILEOPS_CHOICE_SKIP) {
                *had_skip = 1;
                return 1;
            }
            return 0;
        }

        /* O_NOFOLLOW on every attempt, not just the first O_EXCL create:
         * a symlink planted at dest_path during the error/overwrite
         * prompt (TOCTOU) would otherwise be followed and its target
         * truncated by a Retry's O_TRUNC open. */
        int open_flags = O_WRONLY | O_CREAT | O_NOFOLLOW | (dest_created ? O_TRUNC : O_EXCL);
        /* 0600, not 0666: the file's permissions are only finalized to
         * the source's real mode by fchmod() below AFTER the copy
         * completes - creating it at the permissive default in the
         * meantime would leave a private source file (e.g. a 0600 SSH
         * key) briefly world-readable-minus-umask while its content is
         * still being written, and permanently so if the process is
         * killed mid-copy before the fchmod() runs. Narrow-then-widen is
         * the safe direction: a source file that's actually MORE
         * permissive than 0600 still ends up correctly widened by the
         * fchmod() at the end, it just isn't briefly too-open first. */
        int dest_fd = open(dest_path, open_flags, 0600);
        FILE *out = (dest_fd != -1) ? fdopen(dest_fd, "wb") : NULL;
        if (out == NULL) {
            /* Captured immediately after the failing open()/fdopen() -
             * before close()/unlink()/fclose() below can clobber it. */
            int saved_errno = errno;
            if (dest_fd != -1) {
                /* open() succeeded but fdopen() failed - the file exists
                 * (freshly created or truncated) but is now an orphaned
                 * empty file. Remove it and reset dest_created so a Retry
                 * uses O_EXCL again instead of hitting EEXIST forever. */
                close(dest_fd);
                unlink(dest_path);
                dest_created = 0;
            }
            fclose(in);
            char msg[PATH_MAX + 128];
            snprintf(msg, sizeof(msg), "%s: %s", dest_path, strerror(saved_errno));
            FileOpChoice choice = report_error(cb, "Error writing", msg);
            if (choice == FILEOPS_CHOICE_RETRY) {
                continue;
            }
            if (choice == FILEOPS_CHOICE_SKIP) {
                *had_skip = 1;
                return 1;
            }
            return 0;
        }
        dest_created = 1;

        /* Snapshot copied_bytes so a Retry after a mid-file failure
         * restores it instead of double-counting this file's bytes
         * (the retry starts the file over from byte 0). */
        long long bytes_before_attempt = progress->copied_bytes;

        int failed = 0;
        /* Captured at the point of the specific failure below (fwrite,
         * ferror, or fclose further down) so the eventual error message
         * names the real cause instead of always saying the same generic
         * "Error writing" with no detail. */
        int write_errno = 0;
        char buffer[65536];
        size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
            if (fwrite(buffer, 1, n, out) != n) {
                write_errno = errno;
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
        if (!failed && ferror(in)) {
            /* fread() returning 0 means either clean EOF or a read
             * error (e.g. EIO on a flaky mount) - without this check a
             * mid-copy I/O error looks identical to a successful,
             * complete copy. */
            write_errno = errno;
            failed = 1;
        }

        if (!failed) {
            /* Preserve the source's permissions instead of the process
             * default (open() with 0666 & ~umask): otherwise a copied
             * executable loses its x-bit, or a private 0600 file (e.g. an
             * SSH key) ends up 0644 (world-readable) under a typical
             * umask. Masked to 0777 (not 07777): setuid/setgid on a
             * regular file must never be carried over to a copy made by
             * a different, possibly unprivileged, owner. Reuses the
             * src_st already stat()'d at the top of this function instead
             * of re-stat()ing the same unchanged path. */
            if (have_src_st) {
                /* fwrite() above is buffered in userspace - without this
                 * flush, the buffered bytes are still unwritten at the
                 * kernel level when futimens() runs below, and the
                 * eventual real write() (triggered by the fclose() calls
                 * further down) bumps mtime back to "now" as an ordinary
                 * side effect of writing data, silently undoing the
                 * timestamp restore. Caught by testing the actual copied
                 * file's timestamp, not just futimens()'s return value
                 * (which reports success either way). */
                fflush(out);

                fchmod(fileno(out), src_st.st_mode & 0777);

                /* Best-effort: fchown() to the source's owner/group only
                 * succeeds for root (CAP_CHOWN) or when the caller is
                 * already a member of the target group - for a normal,
                 * unprivileged user copying their own files this is a
                 * harmless no-op (they already own the new file), but
                 * for a root-run backup/restore it preserves ownership
                 * instead of silently reassigning everything to root.
                 * Failure (EPERM) is expected and ignored - there is no
                 * Retry/Skip/Abort question to ask the user here, this is
                 * metadata preservation, not the operation itself. (void)
                 * does NOT silence glibc's warn_unused_result on this
                 * function - an empty if-body is the actual idiom. */
                if (fchown(fileno(out), src_st.st_uid, src_st.st_gid) != 0) {
                }

                /* Best-effort: preserve mtime/atime so a copy doesn't
                 * look "just modified" (breaks incremental-backup tools,
                 * build-cache freshness checks, and just plain misleads
                 * the user about when a file was actually last changed). */
                struct timespec times[2];
                times[0] = src_st.st_atim;
                times[1] = src_st.st_mtim;
                futimens(fileno(out), times);
            }
        }

        int in_close_failed = fclose(in) != 0;
        int in_close_errno = errno;
        int out_close_failed = fclose(out) != 0;
        int out_close_errno = errno;
        if (!failed && (in_close_failed || out_close_failed)) {
            /* A buffered write error (e.g. ENOSPC) can surface only at
             * fclose(), after every fwrite() appeared to succeed. Prefer
             * the write side's errno since that's the fclose whose
             * failure actually corrupted the copy; the read side closing
             * badly is comparatively harmless (the data was already
             * fully read). */
            write_errno = out_close_failed ? out_close_errno : in_close_errno;
            failed = 1;
        }

        if (failed) {
            progress->copied_bytes = bytes_before_attempt;
            char msg[PATH_MAX + 128];
            snprintf(msg, sizeof(msg), "%s: %s", dest_path, strerror(write_errno));
            FileOpChoice choice = report_error(cb, "Error writing", msg);
            if (choice == FILEOPS_CHOICE_RETRY) {
                continue;
            }
            if (choice == FILEOPS_CHOICE_SKIP) {
                *had_skip = 1;
                return 1;
            }
            return 0;
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
    for (;;) {
        if (unlink(dest) == 0) {
            return 1;
        }
        int saved_errno = errno;
        /* An ignored unlink() failure (e.g. EPERM on an immutable file)
         * used to fall through silently: the caller's subsequent
         * O_EXCL/mkdir create would then fail EEXIST against the entry
         * that was never actually removed, and Retry would just hit the
         * same silent unlink() failure again - an infinite dialog loop
         * with no indication of the real cause. Skip has no well-defined
         * meaning at this layer (the caller already committed to
         * overwriting), so it's treated the same as Abort: stop this
         * entry rather than silently proceeding as if it were removed. */
        char msg[PATH_MAX + 128];
        snprintf(msg, sizeof(msg), "%s: %s", dest, strerror(saved_errno));
        FileOpChoice choice = report_error(cb, "Cannot remove", msg);
        if (choice == FILEOPS_CHOICE_RETRY) {
            continue;
        }
        return 0;
    }
}

/* Copies src (file or directory) recursively to dest. Returns 1 to
 * continue, 0 if aborted. See copy_file()'s comment for what had_skip is
 * for and why every SKIP exit below sets it instead of just returning 1. */
static int copy_recursive_impl(const char *src, const char *dest, CopyProgress *progress,
                                const FileOpCallbacks *cb, int *had_skip, int depth)
{
    if (depth > MAX_RECURSION_DEPTH) {
        /* Unlike compute_total_size_impl()'s "just an estimate" case,
         * this function is about to actually copy real data - fail
         * loudly through the normal Retry/Skip/Abort machinery instead
         * of silently stopping partway (Retry can't help here - the
         * tree's depth won't change - but the choice is still routed
         * through so the caller sees a consistent contract). */
        FileOpChoice choice = report_error(cb, "Directory tree too deep", src);
        if (choice == FILEOPS_CHOICE_SKIP) {
            *had_skip = 1;
            return 1;
        }
        return 0;
    }

    struct stat st;
    /* lstat, not stat: handle symlinks themselves (copy as a link) rather
     * than following them, so a directory symlink pointing at an ancestor
     * or itself can't trigger infinite recursion (stack overflow). */
    for (;;) {
        if (lstat(src, &st) == 0) {
            break;
        }
        int saved_errno = errno;
        char msg[PATH_MAX + 128];
        snprintf(msg, sizeof(msg), "%s: %s", src, strerror(saved_errno));
        FileOpChoice choice = report_error(cb, "Error", msg);
        if (choice == FILEOPS_CHOICE_RETRY) {
            continue;
        }
        if (choice == FILEOPS_CHOICE_SKIP) {
            *had_skip = 1;
            return 1;
        }
        return 0;
    }

    if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        ssize_t len;
        for (;;) {
            len = readlink(src, target, sizeof(target) - 1);
            if (len < 0) {
                int saved_errno = errno;
                char msg[PATH_MAX + 128];
                snprintf(msg, sizeof(msg), "%s: %s", src, strerror(saved_errno));
                FileOpChoice choice = report_error(cb, "Error reading link", msg);
                if (choice == FILEOPS_CHOICE_RETRY) {
                    continue;
                }
                if (choice == FILEOPS_CHOICE_SKIP) {
                    *had_skip = 1;
                    return 1;
                }
                return 0;
            }
            if ((size_t)len == sizeof(target) - 1) {
                /* readlink() truncates silently on overflow: no NUL
                 * termination, no truncation indicator in the return
                 * value, just a full buffer. Continuing would symlink()
                 * a truncated, wrong target with no error at all. */
                FileOpChoice choice = report_error(cb, "Error reading link", "Link target too long");
                if (choice == FILEOPS_CHOICE_RETRY) {
                    continue;
                }
                if (choice == FILEOPS_CHOICE_SKIP) {
                    *had_skip = 1;
                    return 1;
                }
                return 0;
            }
            break;
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
                if (choice == FILEOPS_CHOICE_SKIP) {
                    *had_skip = 1;
                    return 1;
                }
                return 0;
            }
            if (!remove_existing_for_overwrite(dest, &dest_existing, cb)) {
                return 0;
            }
        }

        for (;;) {
            if (symlink(target, dest) == 0) {
                return 1;
            }
            int saved_errno = errno;
            char msg[PATH_MAX + 128];
            snprintf(msg, sizeof(msg), "%s: %s", dest, strerror(saved_errno));
            FileOpChoice choice = report_error(cb, "Error creating link", msg);
            if (choice == FILEOPS_CHOICE_RETRY) {
                continue;
            }
            if (choice == FILEOPS_CHOICE_SKIP) {
                *had_skip = 1;
                return 1;
            }
            return 0;
        }
    }

    if (!S_ISDIR(st.st_mode)) {
        if (!S_ISREG(st.st_mode)) {
            /* FIFO/socket/device: fopen()ing one of these blocks or
             * copies forever instead of erroring, since copy_file()
             * assumes an ordinary file. */
            FileOpChoice choice = report_error(cb, "Cannot copy", "Not a regular file or directory");
            if (choice == FILEOPS_CHOICE_SKIP) {
                *had_skip = 1;
                return 1;
            }
            return 0;
        }
        return copy_file(src, dest, progress, cb, had_skip);
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
            /* Best-effort, same reasoning as copy_file()'s fchown() -
             * harmless no-op for a normal user, preserves ownership for
             * a root-run backup/restore. Directory mtime is deliberately
             * NOT restored here: it will be repeatedly overwritten as
             * this directory's own entries are copied into it below, so
             * setting it now would just be discarded. (void) does NOT
             * silence glibc's warn_unused_result on this function - an
             * empty if-body is the actual idiom. */
            if (chown(dest, st.st_uid, st.st_gid) != 0) {
            }
            break;
        }
        int mkdir_errno = errno;
        if (mkdir_errno == EEXIST) {
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
                if (choice == FILEOPS_CHOICE_SKIP) {
                    *had_skip = 1;
                    return 1;
                }
                return 0;
            }
            if (unlink(dest) != 0) {
                /* Without checking this, a persistent removal failure
                 * (e.g. EPERM) would just re-hit EEXIST and re-ask
                 * "overwrite?" for an entry that already answered
                 * OVERWRITE and still can't actually be removed -
                 * looping the same prompt instead of surfacing the real
                 * cause. */
                int unlink_errno = errno;
                char unlink_msg[PATH_MAX + 128];
                snprintf(unlink_msg, sizeof(unlink_msg), "%s: %s", dest, strerror(unlink_errno));
                FileOpChoice remove_choice = report_error(cb, "Cannot remove", unlink_msg);
                if (remove_choice == FILEOPS_CHOICE_RETRY) {
                    continue;
                }
                if (remove_choice == FILEOPS_CHOICE_SKIP) {
                    *had_skip = 1;
                    return 1;
                }
                return 0;
            }
            continue;
        }
        char mkdir_msg[PATH_MAX + 128];
        snprintf(mkdir_msg, sizeof(mkdir_msg), "%s: %s", dest, strerror(mkdir_errno));
        FileOpChoice choice = report_error(cb, "Cannot create directory", mkdir_msg);
        if (choice == FILEOPS_CHOICE_RETRY) {
            continue;
        }
        if (choice == FILEOPS_CHOICE_SKIP) {
            *had_skip = 1;
            return 1;
        }
        return 0;
    }

    DIR *dp;
    for (;;) {
        dp = opendir(src);
        if (dp != NULL) {
            break;
        }
        int saved_errno = errno;
        char msg[PATH_MAX + 128];
        snprintf(msg, sizeof(msg), "%s: %s", src, strerror(saved_errno));
        FileOpChoice choice = report_error(cb, "Error reading", msg);
        if (choice == FILEOPS_CHOICE_RETRY) {
            continue;
        }
        if (choice == FILEOPS_CHOICE_SKIP) {
            *had_skip = 1;
            return 1;
        }
        return 0;
    }

    struct dirent *entry;
    for (;;) {
        /* errno reset right before every readdir() call: the loop body
         * recurses into copy_recursive() for subdirectories, which runs
         * plenty of its own syscalls that can leave errno set without
         * that being a real failure of THIS readdir() - resetting only
         * once before the loop would misattribute that stale errno to
         * this loop's own, successful EOF return. */
        while ((errno = 0, entry = readdir(dp)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            char child_src[PATH_MAX];
            char child_dest[PATH_MAX];
            if (!path_join(child_src, sizeof(child_src), src, entry->d_name) ||
                !path_join(child_dest, sizeof(child_dest), dest, entry->d_name)) {
                FileOpChoice choice = report_error(cb, "Path too long", entry->d_name);
                if (choice == FILEOPS_CHOICE_SKIP) {
                    *had_skip = 1;
                    continue;
                }
                closedir(dp);
                return 0;
            }

            if (!copy_recursive_impl(child_src, child_dest, progress, cb, had_skip, depth + 1)) {
                closedir(dp);
                return 0;
            }
        }

        if (errno == 0) {
            break;
        }
        int saved_errno = errno;
        char msg[PATH_MAX + 128];
        snprintf(msg, sizeof(msg), "%s: %s", src, strerror(saved_errno));
        /* readdir() returning NULL means EOF or error alike - without
         * this check, a mid-read failure (EIO on a flaky mount) silently
         * looks like "done copying this directory", leaving entries
         * added to the source after the failure point uncopied with no
         * indication anything went wrong. */
        FileOpChoice choice = report_error(cb, "Error reading", msg);
        if (choice == FILEOPS_CHOICE_RETRY) {
            continue;
        }
        if (choice == FILEOPS_CHOICE_SKIP) {
            *had_skip = 1;
            break;
        }
        closedir(dp);
        return 0;
    }

    closedir(dp);
    return 1;
}

static int copy_recursive(const char *src, const char *dest, CopyProgress *progress,
                           const FileOpCallbacks *cb, int *had_skip)
{
    return copy_recursive_impl(src, dest, progress, cb, had_skip, 0);
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
    char real_src[PATH_MAX];
    if (realpath(src, real_src) == NULL) {
        return 0;
    }

    /* dir (the destination) may not exist yet - e.g. copying src into a
     * not-yet-created subdirectory of itself - so realpath(dir) failing
     * used to be treated as "not contained", bypassing this guard for
     * exactly the case it exists to catch. Walk up to the nearest
     * existing ancestor of dir instead: if that ancestor already lies
     * under src, every not-yet-created descendant mkdir() would create
     * under it does too. */
    char probe[PATH_MAX];
    if ((size_t)snprintf(probe, sizeof(probe), "%s", dir) >= sizeof(probe)) {
        return 0;
    }

    char real_dir[PATH_MAX];
    for (;;) {
        if (realpath(probe, real_dir) != NULL) {
            break;
        }
        char *slash = strrchr(probe, '/');
        if (slash == NULL || slash == probe) {
            /* Ran out of ancestors to try and still couldn't resolve -
             * fail closed (treat as contained, blocking the copy)
             * rather than silently bypassing the guard on uncertainty. */
            return 1;
        }
        *slash = '\0';
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

/* True for a path with no safe basename to operate on: NULL, empty, "/",
 * ".", or "..". Without this, fileops_copy("/", dest, cb) computes an
 * empty base and the EEXIST merge path silently copies the entire
 * filesystem into dest; fileops_delete("/", cb) had no guard at all. */
static int is_unsafe_root_path(const char *path)
{
    return path == NULL || path[0] == '\0' || strcmp(path, "/") == 0 ||
           strcmp(path, ".") == 0 || strcmp(path, "..") == 0;
}

void fileops_copy(const char *src, const char *dest_dir, const FileOpCallbacks *cb)
{
    if (is_unsafe_root_path(src)) {
        report_error(cb, "Error", "Refusing to copy this path");
        return;
    }

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

    if (base[0] == '\0') {
        /* Catches a src that only becomes "/" after normalization
         * (e.g. "//"), which slips past is_unsafe_root_path() above. */
        report_error(cb, "Error", "Refusing to copy this path");
        return;
    }

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

    int had_skip = 0;
    copy_recursive(src, dest, &progress, cb, &had_skip);
}

/* Deletes path recursively, with error handling (skip/retry/abort)
 * instead of silently ignoring failures - important for trees with
 * read-only or inaccessible sub-entries. Returns 1 to continue, 0 if
 * aborted. */
static int delete_recursive_impl(const char *path, const FileOpCallbacks *cb, int depth)
{
    if (depth > MAX_RECURSION_DEPTH) {
        /* See the matching check/comment in copy_recursive_impl(). */
        FileOpChoice choice = report_error(cb, "Directory tree too deep", path);
        return choice == FILEOPS_CHOICE_SKIP;
    }

    struct stat st;
    for (;;) {
        if (lstat(path, &st) == 0) {
            break;
        }
        int saved_errno = errno;
        if (saved_errno == ENOENT) {
            /* Already gone (e.g. removed by another process between the
             * caller listing it and this call) - the goal of "path no
             * longer exists" is already met, so treat this as success
             * instead of looping Retry forever against an entry that will
             * never come back. */
            return 1;
        }
        char msg[PATH_MAX + 128];
        snprintf(msg, sizeof(msg), "%s: %s", path, strerror(saved_errno));
        FileOpChoice choice = report_error(cb, "Error", msg);
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
            int saved_errno = errno;
            char msg[PATH_MAX + 128];
            snprintf(msg, sizeof(msg), "%s: %s", path, strerror(saved_errno));
            FileOpChoice choice = report_error(cb, "Cannot access directory", msg);
            if (choice == FILEOPS_CHOICE_RETRY) {
                continue;
            }
            return choice == FILEOPS_CHOICE_SKIP;
        }

        struct dirent *entry;
        for (;;) {
            /* See the matching comment in copy_recursive(): reset right
             * before every readdir() call, not just once, since the
             * loop body recurses into delete_recursive(). */
            while ((errno = 0, entry = readdir(dp)) != NULL) {
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
                if (!delete_recursive_impl(child, cb, depth + 1)) {
                    closedir(dp);
                    return 0;
                }
            }

            if (errno == 0) {
                break;
            }
            int saved_errno = errno;
            char msg[PATH_MAX + 128];
            snprintf(msg, sizeof(msg), "%s: %s", path, strerror(saved_errno));
            /* Without this check, a mid-read readdir() failure (EIO on a
             * flaky mount) silently looks like "directory fully
             * enumerated", leaving unprocessed entries behind - the
             * subsequent rmdir() below would then just fail ENOTEMPTY
             * with no indication why. */
            FileOpChoice choice = report_error(cb, "Error reading", msg);
            if (choice == FILEOPS_CHOICE_RETRY) {
                continue;
            }
            if (choice == FILEOPS_CHOICE_SKIP) {
                break;
            }
            closedir(dp);
            return 0;
        }
        closedir(dp);

        for (;;) {
            if (rmdir(path) == 0) {
                return 1;
            }
            int rmdir_errno = errno;
            if (rmdir_errno == ENOENT) {
                /* Same race as above: already gone is success. */
                return 1;
            }
            char msg[PATH_MAX + 128];
            snprintf(msg, sizeof(msg), "%s: %s", path, strerror(rmdir_errno));
            FileOpChoice choice = report_error(cb, "Cannot remove directory", msg);
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
        int unlink_errno = errno;
        if (unlink_errno == ENOENT) {
            /* Same race as the lstat() above: already gone is success. */
            return 1;
        }
        char msg[PATH_MAX + 128];
        snprintf(msg, sizeof(msg), "%s: %s", path, strerror(unlink_errno));
        FileOpChoice choice = report_error(cb, "Cannot delete file", msg);
        if (choice == FILEOPS_CHOICE_RETRY) {
            continue;
        }
        return choice == FILEOPS_CHOICE_SKIP;
    }
}

static int delete_recursive(const char *path, const FileOpCallbacks *cb)
{
    return delete_recursive_impl(path, cb, 0);
}

void fileops_move(const char *src, const char *dest_dir, const FileOpCallbacks *cb)
{
    if (is_unsafe_root_path(src)) {
        report_error(cb, "Error", "Refusing to move this path");
        return;
    }

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

    if (base[0] == '\0') {
        /* See the matching check in fileops_copy(). */
        report_error(cb, "Error", "Refusing to move this path");
        return;
    }

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

            int had_skip = 0;
            if (copy_recursive(src, dest, &progress, cb, &had_skip)) {
                if (had_skip) {
                    /* At least one entry was Skipped rather than actually
                     * copied - deleting the source here would destroy
                     * exactly the files the user chose to keep. Leave the
                     * whole source tree in place instead of guessing which
                     * parts are now safe to remove. */
                    report_error(cb, "Move incomplete",
                                 "Some files were skipped and were not moved; source left in place.");
                } else if (!delete_recursive(src, cb)) {
                    /* copy_recursive() succeeded fully (had_skip == 0) but
                     * the source-side delete itself failed or was
                     * aborted (e.g. a read-only source entry) - without
                     * this check the function returned as if the move
                     * had fully succeeded, leaving both the copy and the
                     * un-deleted source on disk with no indication
                     * anything was left behind. */
                    report_error(cb, "Move incomplete",
                                 "Copied, but could not remove the original; source left in place.");
                }
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

    int had_skip = 0;
    if (copy_recursive(src, dest, &progress, cb, &had_skip)) {
        if (had_skip) {
            /* See the same guard in the dir-merge branch above: don't
             * delete source entries that were never actually copied. */
            report_error(cb, "Move incomplete",
                         "Some files were skipped and were not moved; source left in place.");
        } else if (!delete_recursive(src, cb)) {
            /* Same gap as the dir-merge branch above: the cross-fs copy
             * fully succeeded but deleting the now-redundant source
             * failed/was aborted - report it instead of returning
             * silently as if the move had fully completed. */
            report_error(cb, "Move incomplete",
                         "Copied, but could not remove the original; source left in place.");
        }
    }
}

/* Moves src to the exact path dest (not "into" a directory - dest is
 * the full destination path), via rename() first and a copy+delete
 * fallback across filesystems. Shared by fileops_trash() and
 * fileops_restore_last_trashed() so both reuse the same cross-device-
 * safe move semantics fileops_move() already has, without going
 * through its dest_dir/basename-joining logic - trash needs an exact,
 * collision-resolved destination name, not src's own basename. Returns
 * 1 on success, 0 if aborted/failed (reported via cb). */
static int move_to_exact_dest(const char *src, const char *dest, const FileOpCallbacks *cb)
{
    /* renameat2(..., RENAME_NOREPLACE) instead of a plain rename(): a bare
     * rename() atomically REPLACES dest if it already exists, with no way
     * to ask it not to - both callers of this function rely on dest being
     * a just-verified-free name/path (fileops_trash()'s
     * unique_trash_name(), fileops_restore_last_trashed()'s lstat()
     * check), but a plain rename() re-opens a TOCTOU window between that
     * check and the actual rename (e.g. two trash operations racing on
     * the same source basename, or a file created at the restore target
     * in that narrow window) that would otherwise silently destroy
     * whatever was already at dest. RENAME_NOREPLACE makes the kernel
     * enforce "only if dest doesn't exist" atomically, closing the race
     * outright instead of just narrowing it. */
    int renamed = renameat2(AT_FDCWD, src, AT_FDCWD, dest, RENAME_NOREPLACE) == 0;
    if (!renamed && (errno == EINVAL || errno == ENOSYS)) {
        /* RENAME_NOREPLACE isn't supported by every kernel/filesystem
         * (needs Linux >=3.15, and some FUSE/network filesystems still
         * don't implement it) - fall back to a plain rename() rather than
         * refusing the move outright on an otherwise-working system. This
         * reopens the race above, but only on filesystems where the
         * atomic check was never available to begin with. */
        renamed = rename(src, dest) == 0;
    }
    if (renamed) {
        return 1;
    }
    if (errno == EEXIST) {
        report_error(cb, "Error moving", "Something already exists at the destination");
        return 0;
    }
    if (errno != EXDEV) {
        report_error(cb, "Error moving", strerror(errno));
        return 0;
    }

    CopyProgress progress;
    progress.total_bytes = compute_total_size(src, cb);
    progress.copied_bytes = 0;
    progress.last_reported_percent = -1;

    report_progress(cb, "Moving...", src, 0.0);

    int had_skip = 0;
    if (!copy_recursive(src, dest, &progress, cb, &had_skip)) {
        return 0;
    }
    if (had_skip) {
        report_error(cb, "Move incomplete",
                     "Some files were skipped and were not moved; source left in place.");
        return 0;
    }
    if (!delete_recursive(src, cb)) {
        report_error(cb, "Move incomplete",
                     "Copied, but could not remove the original; source left in place.");
        return 0;
    }
    return 1;
}

/* Recursively creates every missing directory component of path (like
 * "mkdir -p"), including path itself. Tolerates EEXIST at every level -
 * expected on the common case where most of the chain already exists. */
static int mkdir_parents(const char *path, mode_t mode)
{
    char buf[PATH_MAX];
    if ((size_t)snprintf(buf, sizeof(buf), "%s", path) >= sizeof(buf)) {
        return 0;
    }
    for (char *p = buf + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, mode) != 0 && errno != EEXIST) {
                return 0;
            }
            *p = '/';
        }
    }
    return mkdir(buf, mode) == 0 || errno == EEXIST;
}

/* Resolves the freedesktop.org "home trash" files/ and info/ directories
 * ($XDG_DATA_HOME/Trash, or ~/.local/share/Trash if unset), creating
 * them (mode 0700 - trash contents are as sensitive as the files in it)
 * if they don't exist yet. Returns 1 on success. Deliberately only
 * implements the home-trash case, not a per-mount $topdir/.Trash-$uid
 * for other filesystems - see fileops.h. */
static int get_trash_dirs(char *files_dir, size_t files_size, char *info_dir, size_t info_size)
{
    char base[PATH_MAX];
    const char *data_home = getenv("XDG_DATA_HOME");
    int n;
    if (data_home != NULL && data_home[0] != '\0') {
        n = snprintf(base, sizeof(base), "%s/Trash", data_home);
    } else {
        const char *home = getenv("HOME");
        if (home == NULL || home[0] == '\0') {
            return 0;
        }
        n = snprintf(base, sizeof(base), "%s/.local/share/Trash", home);
    }
    if (n <= 0 || (size_t)n >= sizeof(base)) {
        return 0;
    }

    if ((size_t)snprintf(files_dir, files_size, "%s/files", base) >= files_size ||
        (size_t)snprintf(info_dir, info_size, "%s/info", base) >= info_size) {
        return 0;
    }

    return mkdir_parents(files_dir, 0700) && mkdir_parents(info_dir, 0700);
}

/* Finds a name for basename that doesn't already exist as either
 * files_dir/<name> or info_dir/<name>.trashinfo (checked with lstat, so a
 * leftover dangling symlink still counts as "taken"), appending " (1)",
 * " (2)", ... on collision. Writes the chosen name (not a full path) into
 * out_name. The same name (with the same suffix, if any) is used for both
 * files/<name> and info/<name>.trashinfo - fileops_restore_last_trashed()
 * relies on that exact pairing to find the trashed item that matches a
 * given metadata file. Both directories are checked (not just files_dir):
 * an orphaned .trashinfo file (e.g. left behind by a process killed
 * between fileops_restore_last_trashed()'s move-back and its cleanup
 * remove() of the old metadata) would otherwise pass a files_dir-only
 * check and then get silently overwritten by fopen(info_path, "w") when
 * a later, unrelated trash operation happens to generate the same name. */
static int unique_trash_name(const char *files_dir, const char *info_dir, const char *basename,
                              char *out_name, size_t out_name_size)
{
    if ((size_t)snprintf(out_name, out_name_size, "%s", basename) >= out_name_size) {
        return 0;
    }
    for (int suffix = 1; suffix < 100000; suffix++) {
        char candidate_path[PATH_MAX];
        char candidate_info_path[PATH_MAX];
        struct stat st;
        if (!path_join(candidate_path, sizeof(candidate_path), files_dir, out_name) ||
            (size_t)snprintf(candidate_info_path, sizeof(candidate_info_path), "%s/%s.trashinfo",
                              info_dir, out_name) >= sizeof(candidate_info_path)) {
            return 0;
        }
        if (lstat(candidate_path, &st) != 0 && lstat(candidate_info_path, &st) != 0) {
            return 1;
        }
        if ((size_t)snprintf(out_name, out_name_size, "%s (%d)", basename, suffix) >= out_name_size) {
            return 0;
        }
    }
    return 0;
}

/* Percent-encodes path for a .trashinfo "Path=" line, per the
 * freedesktop.org trash spec (everything except a small unreserved set
 * must be encoded) - without this, a path containing e.g. a space or a
 * non-ASCII byte would produce a malformed/ambiguous key file that other
 * trash-spec-aware tools (and this codebase's own decoder below) could
 * misparse. Silently stops (leaving out valid so far) if out is too
 * small rather than overflowing it. */
static void percent_encode_path(const char *path, char *out, size_t out_size)
{
    static const char *unreserved =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789/-_.~";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)path; *p != '\0'; p++) {
        if (strchr(unreserved, (int)*p) != NULL) {
            if (o + 1 >= out_size) {
                break;
            }
            out[o++] = (char)*p;
        } else {
            if (o + 3 >= out_size) {
                break;
            }
            snprintf(out + o, 4, "%%%02X", *p);
            o += 3;
        }
    }
    out[o] = '\0';
}

/* Reverses percent_encode_path() - an unrecognized "%" (not followed by
 * two hex digits, e.g. truncated or hand-edited metadata) is copied
 * through literally rather than misinterpreted. */
static void percent_decode_path(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (const char *p = in; *p != '\0' && o + 1 < out_size;) {
        if (p[0] == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = {p[1], p[2], '\0'};
            out[o++] = (char)strtol(hex, NULL, 16);
            p += 3;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
}

void fileops_trash(const char *path, const FileOpCallbacks *cb)
{
    if (is_unsafe_root_path(path)) {
        report_error(cb, "Error", "Refusing to trash this path");
        return;
    }

    char normalized[PATH_MAX];
    if ((size_t)snprintf(normalized, sizeof(normalized), "%s", path) >= sizeof(normalized)) {
        report_error(cb, "Path too long", path);
        return;
    }
    strip_trailing_slashes(normalized);
    path = normalized;

    const char *base = strrchr(path, '/');
    base = (base != NULL) ? base + 1 : path;
    if (base[0] == '\0') {
        report_error(cb, "Error", "Refusing to trash this path");
        return;
    }

    /* The original location is recorded in the .trashinfo metadata so
     * fileops_restore_last_trashed() can put it back - must be absolute,
     * since a relative path would be meaningless once the process's
     * current directory changes (or a different process/session does
     * the restoring). */
    char abs_path[PATH_MAX];
    if (path[0] == '/') {
        if ((size_t)snprintf(abs_path, sizeof(abs_path), "%s", path) >= sizeof(abs_path)) {
            report_error(cb, "Path too long", path);
            return;
        }
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == NULL || !path_join(abs_path, sizeof(abs_path), cwd, path)) {
            report_error(cb, "Error", "Could not resolve the absolute path to trash");
            return;
        }
    }

    char files_dir[PATH_MAX], info_dir[PATH_MAX];
    if (!get_trash_dirs(files_dir, sizeof(files_dir), info_dir, sizeof(info_dir))) {
        report_error(cb, "Error", "Could not access or create the trash directory");
        return;
    }

    char trash_name[PATH_MAX];
    if (!unique_trash_name(files_dir, info_dir, base, trash_name, sizeof(trash_name))) {
        report_error(cb, "Error", "Could not find a free name in the trash");
        return;
    }

    char dest[PATH_MAX], info_path[PATH_MAX];
    if (!path_join(dest, sizeof(dest), files_dir, trash_name) ||
        (size_t)snprintf(info_path, sizeof(info_path), "%s/%s.trashinfo", info_dir, trash_name) >=
            sizeof(info_path)) {
        report_error(cb, "Path too long", trash_name);
        return;
    }

    if (!move_to_exact_dest(path, dest, cb)) {
        return;
    }

    /* Metadata is written AFTER the move succeeds: if this fails (e.g.
     * disk full), the item is still safely sitting in files/ - just
     * without a way to auto-restore its original location, not lost. */
    FILE *fp = fopen(info_path, "w");
    if (fp != NULL) {
        char encoded[PATH_MAX * 3];
        percent_encode_path(abs_path, encoded, sizeof(encoded));

        time_t now = time(NULL);
        struct tm tm_now;
        char timestamp[32] = "";
        if (localtime_r(&now, &tm_now) != NULL) {
            strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &tm_now);
        }

        fprintf(fp, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", encoded, timestamp);
        fclose(fp);
    }
}

int fileops_restore_last_trashed(const FileOpCallbacks *cb, char *restored_path_out,
                                  size_t restored_path_out_size)
{
    char files_dir[PATH_MAX], info_dir[PATH_MAX];
    if (!get_trash_dirs(files_dir, sizeof(files_dir), info_dir, sizeof(info_dir))) {
        report_error(cb, "Error", "Could not access the trash directory");
        return 0;
    }

    DIR *dp = opendir(info_dir);
    if (dp == NULL) {
        report_error(cb, "Nothing to undo", "The trash is empty.");
        return 0;
    }

    /* Finds the *.trashinfo file with the newest mtime - that file's own
     * mtime is effectively its deletion time, since fileops_trash() just
     * wrote it, so no separate DeletionDate parsing is needed to find
     * the most recent one. */
    static const char trashinfo_suffix[] = ".trashinfo";
    char newest_name[PATH_MAX] = "";
    time_t newest_mtime = 0;
    struct dirent *entry;
    /* errno reset right before every readdir() call, not just once before
     * the loop - matching copy_recursive_impl()/delete_recursive_impl()'s
     * pattern (see their own comments): without this, readdir() returning
     * NULL for a mid-scan error (e.g. EIO on a flaky mount) looks
     * identical to a normal, complete "directory fully enumerated" EOF,
     * silently reporting "trash is empty" even when a real trashed item
     * exists and simply wasn't seen. */
    while ((errno = 0, entry = readdir(dp)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        size_t suffix_len = sizeof(trashinfo_suffix) - 1;
        if (name_len <= suffix_len ||
            strcmp(entry->d_name + name_len - suffix_len, trashinfo_suffix) != 0) {
            continue;
        }
        char full[PATH_MAX];
        if (!path_join(full, sizeof(full), info_dir, entry->d_name)) {
            continue;
        }
        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }
        if (newest_name[0] == '\0' || st.st_mtime > newest_mtime) {
            newest_mtime = st.st_mtime;
            snprintf(newest_name, sizeof(newest_name), "%s", entry->d_name);
        }
    }
    if (errno != 0) {
        report_error(cb, "Error reading trash", strerror(errno));
        closedir(dp);
        return 0;
    }
    closedir(dp);

    if (newest_name[0] == '\0') {
        report_error(cb, "Nothing to undo", "The trash is empty.");
        return 0;
    }

    /* Strips ".trashinfo" to recover the trash item's own name - matches
     * files/<name> exactly (see unique_trash_name()'s comment). */
    size_t item_name_len = strlen(newest_name) - (sizeof(trashinfo_suffix) - 1);
    char item_name[PATH_MAX];
    snprintf(item_name, sizeof(item_name), "%.*s", (int)item_name_len, newest_name);

    char info_path[PATH_MAX];
    if ((size_t)snprintf(info_path, sizeof(info_path), "%s/%s.trashinfo", info_dir, item_name) >=
        sizeof(info_path)) {
        report_error(cb, "Error", "Path too long");
        return 0;
    }

    char original_path[PATH_MAX] = "";
    FILE *fp = fopen(info_path, "r");
    if (fp == NULL) {
        report_error(cb, "Error", "Could not read trash metadata");
        return 0;
    }
    char line[PATH_MAX + 16];
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *nl = strchr(line, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }
        if (strncmp(line, "Path=", 5) == 0) {
            percent_decode_path(line + 5, original_path, sizeof(original_path));
            break;
        }
    }
    fclose(fp);

    if (original_path[0] == '\0') {
        report_error(cb, "Error", "Trash metadata is missing the original path");
        return 0;
    }

    /* Never overwrite: if something now occupies the original spot
     * (e.g. a new file was created with that name after the trash), fail
     * cleanly instead of silently clobbering it - there's no
     * Retry/Skip/Abort question that makes sense here, unlike a normal
     * copy/move conflict, since the "conflict" is with unrelated data
     * the user created after the delete, not a stale copy of the same
     * operation. */
    struct stat existing;
    if (lstat(original_path, &existing) == 0) {
        report_error(cb, "Cannot undo", "Something already exists at the original location.");
        return 0;
    }

    char trashed_item_path[PATH_MAX];
    if (!path_join(trashed_item_path, sizeof(trashed_item_path), files_dir, item_name)) {
        report_error(cb, "Error", "Path too long");
        return 0;
    }

    if (!move_to_exact_dest(trashed_item_path, original_path, cb)) {
        return 0;
    }

    /* Only removed after the move back succeeds - if move_to_exact_dest()
     * failed (e.g. the original directory no longer exists), the item
     * stays in the trash for another attempt instead of being lost. */
    remove(info_path);

    if (restored_path_out != NULL) {
        snprintf(restored_path_out, restored_path_out_size, "%s", original_path);
    }
    return 1;
}

void fileops_delete(const char *path, const FileOpCallbacks *cb)
{
    if (is_unsafe_root_path(path)) {
        report_error(cb, "Error", "Refusing to delete this path");
        return;
    }
    delete_recursive(path, cb);
}
