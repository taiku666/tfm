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
 * copy_recursive(), and delete_recursive(), which recurse one stack frame
 * per directory level - a pathologically deep tree would otherwise
 * exhaust the stack and crash instead of failing cleanly.
 * copy_recursive()'s frame holds three PATH_MAX buffers, so 200 still
 * leaves a comfortable margin on the default 8MB stack. */
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
    FileOpChoice choice = cb->on_overwrite(cb->ctx, path);
    /* The "all" part is the caller's to remember (batch.c); for this one
     * conflict they mean the same as the plain answers. */
    if (choice == FILEOPS_CHOICE_OVERWRITE_ALL) {
        return FILEOPS_CHOICE_OVERWRITE;
    }
    if (choice == FILEOPS_CHOICE_SKIP_ALL) {
        return FILEOPS_CHOICE_SKIP;
    }
    return choice;
}

static void report_progress(const FileOpCallbacks *cb, const char *title, const char *item, double percent)
{
    if (cb == NULL || cb->on_progress == NULL) {
        return;
    }
    cb->on_progress(cb->ctx, title, item, percent);
}

/* Computes the total size of path (recursively) so copy_recursive() can
 * show a percentage. report_progress() is called once per directory
 * entered: on a large tree this pre-pass is otherwise a long silent
 * pause, and the GUI only pumps its main loop inside on_progress, so its
 * window would appear frozen. */
static long long compute_total_size_impl(const char *path, const FileOpCallbacks *cb, int depth)
{
    /* Fail soft, like an opendir() failure below: this only feeds a
     * progress estimate. copy_recursive()/delete_recursive() touch real
     * data, so they report the same condition instead. */
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
        /* Removed via remove_existing_for_overwrite() so a directory goes
         * recursively (a plain unlink() would fail EISDIR and leave O_EXCL
         * below failing EEXIST forever). A file or symlink is removed by
         * name, not followed - together with O_EXCL below, that stops a
         * symlink's target (a different file than the one just
         * confirmed) from being truncated. */
        if (!remove_existing_for_overwrite(dest_path, &existing, cb)) {
            return 0;
        }
    }

    /* Whether dest_path was already created in this call: a Retry reuses
     * that file (O_TRUNC), but the first creation uses O_EXCL so it fails
     * if something (e.g. a symlink) appeared there since the overwrite
     * check above, instead of following it. */
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
        /* 0600, not 0666: the real mode is only applied by fchmod() after
         * the copy completes, so a private source (e.g. an SSH key) would
         * otherwise sit readable under the umask while being written - or
         * permanently, if tfm is killed mid-copy. Narrow-then-widen is the
         * safe direction. */
        int dest_fd = open(dest_path, open_flags, 0600);
        FILE *out = (dest_fd != -1) ? fdopen(dest_fd, "wb") : NULL;
        if (out == NULL) {
            /* Captured immediately after the failing open()/fdopen() -
             * before close()/unlink()/fclose() below can clobber it. */
            int saved_errno = errno;
            if (dest_fd != -1) {
                /* open() succeeded but fdopen() failed, leaving an
                 * orphaned empty file. Remove it and reset dest_created so
                 * a Retry uses O_EXCL again instead of hitting EEXIST
                 * forever. */
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
        /* Captured at the specific failure below (fwrite, ferror, or
         * fclose) so the error message names the real cause. */
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

            /* fread() only returns a short count at EOF or on a read
             * error (C11 7.21.8.1), which ferror() below tells apart, so
             * one more fread() could only return 0. Stopping here gives
             * scan-build's unix.Stream checker a loop shape it can follow
             * instead of a false "read in EOF state" report. */
            if (n < sizeof(buffer)) {
                break;
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
            /* Preserve the source's permissions: otherwise a copied
             * executable loses its x-bit, or a 0600 file ends up
             * world-readable under a typical umask. Masked to 0777:
             * setuid/setgid must never carry over to a copy made by a
             * different, possibly unprivileged, owner. */
            if (have_src_st) {
                /* Flush first: fwrite() is buffered, and the real write()
                 * at fclose() would bump mtime back to "now", silently
                 * undoing the futimens() below. */
                fflush(out);

                fchmod(fileno(out), src_st.st_mode & 0777);

                /* Best-effort: only succeeds for root (or a member of the
                 * target group), where it keeps a root-run backup/restore
                 * from reassigning everything to root; EPERM otherwise is
                 * expected. An empty if-body, because (void) doesn't
                 * silence glibc's warn_unused_result. */
                if (fchown(fileno(out), src_st.st_uid, src_st.st_gid) != 0) {
                }

                /* Best-effort: preserve mtime/atime so a copy doesn't look
                 * "just modified" to backup tools, build caches, or the
                 * user. */
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
             * fclose(), after every fwrite() appeared to succeed. The
             * write side's errno is preferred: that's the failure that
             * actually corrupted the copy. */
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
        /* Reported here (e.g. EPERM on an immutable file): ignored, the
         * caller's O_EXCL/mkdir create would fail EEXIST against the
         * still-present entry, in an endless Retry loop that never names
         * the real cause. Skip has no meaning at this layer (the caller
         * already committed to overwriting), so it's treated as Abort. */
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
        /* Unlike compute_total_size_impl()'s estimate, this copies real
         * data, so it fails loudly instead of stopping partway silently.
         * Retry can't change the tree's depth, so only Skip is honored. */
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
        /* mkdir()'s mode is masked by umask (a 0775 shared directory
         * would come out 0755), so the chmod() below reapplies it - on a
         * newly created directory only, so merging into an existing one
         * never changes its deliberately set permissions. */
        if (mkdir(dest, st.st_mode & 07777) == 0) {
            chmod(dest, st.st_mode & 07777);
            /* Best-effort, as with copy_file()'s fchown(). Directory mtime
             * is not restored: copying the entries into it below would
             * overwrite it anyway. */
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
            /* Type mismatch: dest is a file or symlink, not a directory.
             * Asked like any other conflict, rather than letting the
             * opendir(dest) below fail with a confusing ENOTDIR. */
            FileOpChoice choice = report_overwrite(cb, dest);
            if (choice != FILEOPS_CHOICE_OVERWRITE) {
                if (choice == FILEOPS_CHOICE_SKIP) {
                    *had_skip = 1;
                    return 1;
                }
                return 0;
            }
            if (unlink(dest) != 0) {
                /* Otherwise a persistent failure (e.g. EPERM) re-hits
                 * EEXIST and loops the "overwrite?" prompt without ever
                 * naming the real cause. */
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
        /* errno reset before every readdir(), not once before the loop:
         * the recursive call below can leave errno set, which would be
         * misread as a failure of this loop's successful EOF return. */
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
        /* readdir() returns NULL for EOF and error alike; without this, a
         * mid-read failure (EIO on a flaky mount) would silently leave the
         * rest of the directory uncopied. */
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
 * freshly created destination into itself until PATH_MAX. Returns 0 if
 * src itself can't be resolved. */
static int dir_is_or_contains(const char *dir, const char *src)
{
    char real_src[PATH_MAX];
    if (realpath(src, real_src) == NULL) {
        return 0;
    }

    /* dir may not exist yet (e.g. a not-yet-created subdirectory of src),
     * so realpath(dir) failing can't mean "not contained". Walk up to the
     * nearest existing ancestor instead: if it lies under src, so does
     * everything that would be created beneath it. */
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
 * filesystem into dest, and fileops_delete("/", cb) would delete it. */
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
    /* A truncated src would silently operate on a different, shorter
     * path than the one passed in, so truncation is an error. */
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
            /* Already gone (e.g. removed by another process since it was
             * listed) - the goal is met, so succeed instead of looping
             * Retry against an entry that will never come back. */
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
            /* A mid-read readdir() failure (EIO on a flaky mount) would
             * otherwise look like a complete listing, and the rmdir()
             * below would fail ENOTEMPTY with no indication why. */
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
    /* lstat, not stat, as in copy_file(): stat() fails ENOENT on a
     * dangling symlink at dest, which would let rename() replace it
     * without asking, breaking fileops.h's "asks before overwriting"
     * contract. */
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
            /* Both sides are directories: merge (asking per file
             * conflict, like fileops_copy()) instead of deleting the
             * destination wholesale, which would destroy files that exist
             * only there. Then remove the source. */
            CopyProgress progress;
            progress.total_bytes = compute_total_size(src, cb);
            progress.copied_bytes = 0;
            progress.last_reported_percent = -1;

            report_progress(cb, "Moving...", src, 0.0);

            int had_skip = 0;
            if (copy_recursive(src, dest, &progress, cb, &had_skip)) {
                if (had_skip) {
                    /* Deleting the source would destroy exactly the files
                     * the user chose to Skip, so the whole source tree is
                     * left in place. */
                    report_error(cb, "Move incomplete",
                                 "Some files were skipped and were not moved; source left in place.");
                } else if (!delete_recursive(src, cb)) {
                    /* The copy succeeded but removing the source failed or
                     * was aborted (e.g. a read-only source entry) - say so
                     * rather than report the move as complete. */
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
            /* As in the dir-merge branch above: the copy succeeded but
             * removing the source didn't. */
            report_error(cb, "Move incomplete",
                         "Copied, but could not remove the original; source left in place.");
        }
    }
}

/* Moves src to the exact path dest (the full destination path, not a
 * directory to move into), via rename() with a copy+delete fallback
 * across filesystems. Used by fileops_trash() and
 * fileops_restore_last_trashed(), which need an exact, collision-resolved
 * destination name rather than fileops_move()'s dest_dir/basename
 * joining. Returns 1 on success, 0 if aborted/failed (reported via cb). */
static int move_to_exact_dest(const char *src, const char *dest, const FileOpCallbacks *cb)
{
    /* RENAME_NOREPLACE: both callers have just verified dest is free, but
     * a plain rename() atomically replaces whatever appears there in the
     * meantime (e.g. two trash operations racing on the same basename),
     * silently destroying it. The kernel flag closes that TOCTOU window. */
    int renamed = renameat2(AT_FDCWD, src, AT_FDCWD, dest, RENAME_NOREPLACE) == 0;
    if (!renamed && (errno == EINVAL || errno == ENOSYS)) {
        /* Not supported by every kernel/filesystem (Linux < 3.15, some
         * FUSE/network filesystems) - fall back to plain rename() there
         * rather than refuse the move. */
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

/* Finds a name for basename, appending " (1)", " (2)", ... on collision,
 * that is free as both files_dir/<name> and info_dir/<name>.trashinfo
 * (lstat, so a dangling symlink counts as taken), and writes it (not a
 * full path) into out_name. fileops_restore_last_trashed() relies on that
 * exact files/info pairing. info_dir is checked too because an orphaned
 * .trashinfo (e.g. from a restore killed before its cleanup) would
 * otherwise be silently overwritten by a later trash of the same name. */
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

/* Percent-encodes path for a .trashinfo "Path=" line, as the
 * freedesktop.org trash spec requires - a raw space or non-ASCII byte
 * would make the key file ambiguous to other trash tools and to the
 * decoder below. Stops early (out stays valid) if out is too small. */
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

int fileops_trash(const char *path, const FileOpCallbacks *cb, char *trash_name_out,
                  size_t trash_name_out_size)
{
    if (is_unsafe_root_path(path)) {
        report_error(cb, "Error", "Refusing to trash this path");
        return 0;
    }

    char normalized[PATH_MAX];
    if ((size_t)snprintf(normalized, sizeof(normalized), "%s", path) >= sizeof(normalized)) {
        report_error(cb, "Path too long", path);
        return 0;
    }
    strip_trailing_slashes(normalized);
    path = normalized;

    const char *base = strrchr(path, '/');
    base = (base != NULL) ? base + 1 : path;
    if (base[0] == '\0') {
        report_error(cb, "Error", "Refusing to trash this path");
        return 0;
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
            return 0;
        }
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == NULL || !path_join(abs_path, sizeof(abs_path), cwd, path)) {
            report_error(cb, "Error", "Could not resolve the absolute path to trash");
            return 0;
        }
    }

    char files_dir[PATH_MAX], info_dir[PATH_MAX];
    if (!get_trash_dirs(files_dir, sizeof(files_dir), info_dir, sizeof(info_dir))) {
        report_error(cb, "Error", "Could not access or create the trash directory");
        return 0;
    }

    char trash_name[PATH_MAX];
    if (!unique_trash_name(files_dir, info_dir, base, trash_name, sizeof(trash_name))) {
        report_error(cb, "Error", "Could not find a free name in the trash");
        return 0;
    }

    char dest[PATH_MAX], info_path[PATH_MAX];
    if (!path_join(dest, sizeof(dest), files_dir, trash_name) ||
        (size_t)snprintf(info_path, sizeof(info_path), "%s/%s.trashinfo", info_dir, trash_name) >=
            sizeof(info_path)) {
        report_error(cb, "Path too long", trash_name);
        return 0;
    }

    if (!move_to_exact_dest(path, dest, cb)) {
        return 0;
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

    if (trash_name_out != NULL) {
        snprintf(trash_name_out, trash_name_out_size, "%s", trash_name);
    }
    return 1;
}

/* Moves files_dir/item_name back to the original path recorded in
 * info_dir/item_name.trashinfo, then removes that metadata. Shared by
 * fileops_restore_last_trashed() and fileops_restore_trashed(). */
static int restore_trash_item(const char *files_dir, const char *info_dir, const char *item_name,
                              const FileOpCallbacks *cb, char *restored_path_out,
                              size_t restored_path_out_size)
{
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

    /* Never overwrite: whatever occupies the original spot now is
     * unrelated data created after the delete, not a conflicting copy,
     * so fail cleanly instead of offering an Overwrite prompt. */
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

    /* Finds the *.trashinfo file with the newest mtime - fileops_trash()
     * writes it at deletion time, so no DeletionDate parsing is needed. */
    static const char trashinfo_suffix[] = ".trashinfo";
    char newest_name[PATH_MAX] = "";
    struct timespec newest_mtime = {0, 0};
    struct dirent *entry;
    /* errno reset before every readdir(), as in copy_recursive_impl(), so
     * a mid-scan error (e.g. EIO) is told apart from EOF instead of
     * silently reporting "trash is empty". */
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
        /* Nanoseconds, not just st_mtime: a batch trashes several items
         * within the same second, and the newest must still win. */
        if (newest_name[0] == '\0' || st.st_mtim.tv_sec > newest_mtime.tv_sec ||
            (st.st_mtim.tv_sec == newest_mtime.tv_sec && st.st_mtim.tv_nsec > newest_mtime.tv_nsec)) {
            newest_mtime = st.st_mtim;
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

    return restore_trash_item(files_dir, info_dir, item_name, cb, restored_path_out, restored_path_out_size);
}

int fileops_restore_trashed(const char *trash_name, const FileOpCallbacks *cb, char *restored_path_out,
                            size_t restored_path_out_size)
{
    /* A trash name is a single component; anything else would reach
     * outside files/ and info/. */
    if (!is_safe_path_component(trash_name)) {
        report_error(cb, "Error", "Invalid trash item name");
        return 0;
    }

    char files_dir[PATH_MAX], info_dir[PATH_MAX];
    if (!get_trash_dirs(files_dir, sizeof(files_dir), info_dir, sizeof(info_dir))) {
        report_error(cb, "Error", "Could not access the trash directory");
        return 0;
    }
    return restore_trash_item(files_dir, info_dir, trash_name, cb, restored_path_out, restored_path_out_size);
}

void fileops_delete(const char *path, const FileOpCallbacks *cb)
{
    if (is_unsafe_root_path(path)) {
        report_error(cb, "Error", "Refusing to delete this path");
        return;
    }
    delete_recursive(path, cb);
}
