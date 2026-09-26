#define _DEFAULT_SOURCE

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CONFIG_DIR_NAME ".tfm"
#define CONFIG_FILE_NAME "tfm.ini"

/* Returns 1 on success, 0 if the result was truncated (e.g. $HOME sitting
 * right up against PATH_MAX, leaving no room for "/.tfm") - checked
 * explicitly rather than left to silently produce a shorter path that
 * could point at a different, existing location. */
static int config_get_dir(char *buf, size_t len)
{
    const char *home = getenv("HOME");
    if (home == NULL) {
        /* Matches config_set_defaults()'s own $HOME-unset fallback ("/")
         * - using "." (the cwd) here instead would make the config
         * file's location silently cwd-dependent: launching tfm from two
         * different directories would read/write two unrelated
         * "./.tfm/tfm.ini" files with no diagnostic either way. */
        home = "/";
    }
    int n = snprintf(buf, len, "%s/%s", home, CONFIG_DIR_NAME);
    return n > 0 && (size_t)n < len;
}

static int config_get_path(char *buf, size_t len)
{
    char dir[PATH_MAX];
    if (!config_get_dir(dir, sizeof(dir))) {
        return 0;
    }
    int n = snprintf(buf, len, "%s/%s", dir, CONFIG_FILE_NAME);
    return n > 0 && (size_t)n < len;
}

void config_set_defaults(Config *cfg)
{
    const char *home = getenv("HOME");
    if (home == NULL) {
        home = "/";
    }
    snprintf(cfg->left_path, sizeof(cfg->left_path), "%s", home);
    snprintf(cfg->right_path, sizeof(cfg->right_path), "%s", home);
    snprintf(cfg->border_color, sizeof(cfg->border_color), "system");
    snprintf(cfg->panel_border_color, sizeof(cfg->panel_border_color), "system");
    snprintf(cfg->text_color, sizeof(cfg->text_color), "system");
    snprintf(cfg->cursor_color, sizeof(cfg->cursor_color), "system");
    snprintf(cfg->dir_color, sizeof(cfg->dir_color), "blue");
    snprintf(cfg->mark_color, sizeof(cfg->mark_color), "yellow");
    snprintf(cfg->icons, sizeof(cfg->icons), "omarchy");
    snprintf(cfg->gui_theme, sizeof(cfg->gui_theme), "omarchy");
}

/* Escapes a value for one INI line: '\' -> "\\", '\n' -> "\n" (literal
 * backslash-n, two characters), '\r' -> "\r" - without this, a path
 * containing a literal newline byte would split one logical value across
 * two physical .ini lines on save (the first fragment reloads, the
 * second has no '=' and is silently dropped). Returns 1 if the escaped
 * result fit in out, 0 if truncated - config_save() treats that as a
 * save failure rather than writing a corrupt line. */
static int escape_value(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p != '\0'; p++) {
        const char *rep = NULL;
        if (*p == '\\') {
            rep = "\\\\";
        } else if (*p == '\n') {
            rep = "\\n";
        } else if (*p == '\r') {
            rep = "\\r";
        }
        if (rep != NULL) {
            size_t len = strlen(rep);
            if (o + len >= out_size) {
                return 0;
            }
            memcpy(out + o, rep, len);
            o += len;
        } else {
            if (o + 1 >= out_size) {
                return 0;
            }
            out[o++] = (char)*p;
        }
    }
    out[o] = '\0';
    return 1;
}

/* Reverses escape_value() in place, on an already-trim()'d value.
 * An unrecognized backslash sequence (e.g. a lone trailing backslash, or
 * a pre-escaping tfm.ini that never had one) is left as a literal
 * backslash rather than silently dropped, so older config files still
 * round-trip unchanged. */
static void unescape_value(char *s)
{
    char *w = s;
    for (char *r = s; *r != '\0'; r++) {
        if (*r == '\\' && r[1] != '\0') {
            r++;
            if (*r == 'n') {
                *w++ = '\n';
            } else if (*r == 'r') {
                *w++ = '\r';
            } else if (*r == '\\') {
                *w++ = '\\';
            } else {
                *w++ = '\\';
                *w++ = *r;
            }
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == '\0') {
        return s;
    }
    char *end = s + strlen(s) - 1;
    while (end >= s && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) {
        *end = '\0';
        end--;
    }
    return s;
}

void config_load(Config *cfg, char *error_msg, size_t error_msg_size)
{
    config_set_defaults(cfg);

    if (error_msg != NULL && error_msg_size > 0) {
        error_msg[0] = '\0';
    }

    char path[PATH_MAX];
    if (!config_get_path(path, sizeof(path))) {
        if (error_msg != NULL) {
            snprintf(error_msg, error_msg_size, "Path too long");
        }
        return;
    }

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        /* ENOENT is the ordinary first-run case: keep the defaults silently.
         * Any other failure (e.g. EACCES) is real and reported, so it
         * doesn't look identical to a first run. */
        if (errno != ENOENT && error_msg != NULL) {
            snprintf(error_msg, error_msg_size, "Cannot open \"%s\": %s", path, strerror(errno));
        }
        return;
    }

    /* Must fit a "key=value" line whose value can be up to PATH_MAX long
     * (left_path/right_path); a smaller buffer would truncate long lines
     * mid-value via fgets(). */
    char line[PATH_MAX + 64];
    char section[64] = "";

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *trimmed = trim(line);

        if (trimmed[0] == '\0' || trimmed[0] == ';' || trimmed[0] == '#') {
            continue;
        }

        if (trimmed[0] == '[') {
            char *end = strchr(trimmed, ']');
            if (end != NULL) {
                *end = '\0';
                snprintf(section, sizeof(section), "%s", trimmed + 1);
            }
            continue;
        }

        char *eq = strchr(trimmed, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        char *key = trim(trimmed);
        char *value = trim(eq + 1);
        /* Reverse escape_value()'s encoding before storing - see its
         * comment for why a value can contain an escaped '\n'/'\r'/'\\'. */
        unescape_value(value);

        if (strcmp(section, "panels") == 0) {
            /* An empty/whitespace-only value (value[0] == '\0' after
             * trim()) is left as the config_set_defaults() default
             * ($HOME) instead of overwriting it with "" - an empty path
             * would otherwise feed path_join() as "/name" for every
             * subsequent operation in that panel. */
            if (strcmp(key, "left_path") == 0 && value[0] != '\0') {
                snprintf(cfg->left_path, sizeof(cfg->left_path), "%s", value);
            } else if (strcmp(key, "right_path") == 0 && value[0] != '\0') {
                snprintf(cfg->right_path, sizeof(cfg->right_path), "%s", value);
            }
        } else if (strcmp(section, "display") == 0) {
            if (strcmp(key, "border_color") == 0) {
                snprintf(cfg->border_color, sizeof(cfg->border_color), "%s", value);
            } else if (strcmp(key, "panel_border_color") == 0) {
                snprintf(cfg->panel_border_color, sizeof(cfg->panel_border_color), "%s", value);
            } else if (strcmp(key, "text_color") == 0) {
                snprintf(cfg->text_color, sizeof(cfg->text_color), "%s", value);
            } else if (strcmp(key, "cursor_color") == 0) {
                snprintf(cfg->cursor_color, sizeof(cfg->cursor_color), "%s", value);
            } else if (strcmp(key, "dir_color") == 0) {
                snprintf(cfg->dir_color, sizeof(cfg->dir_color), "%s", value);
            } else if (strcmp(key, "mark_color") == 0) {
                snprintf(cfg->mark_color, sizeof(cfg->mark_color), "%s", value);
            } else if (strcmp(key, "icons") == 0) {
                snprintf(cfg->icons, sizeof(cfg->icons), "%s", value);
            } else if (strcmp(key, "gui_theme") == 0) {
                snprintf(cfg->gui_theme, sizeof(cfg->gui_theme), "%s", value);
            }
        }
        /* Unknown sections are skipped, e.g. the [editor] extensions list
         * from tfm.ini files written before F3 could edit any file. */
    }

    fclose(fp);
}

void config_save(const Config *cfg, char *error_msg, size_t error_msg_size)
{
    if (error_msg != NULL && error_msg_size > 0) {
        error_msg[0] = '\0';
    }

    char dir[PATH_MAX];
    if (!config_get_dir(dir, sizeof(dir))) {
        if (error_msg != NULL) {
            snprintf(error_msg, error_msg_size, "Path too long");
        }
        return;
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        if (error_msg != NULL) {
            snprintf(error_msg, error_msg_size, "Cannot create \"%s\": %s", dir, strerror(errno));
        }
        return;
    }

    char path[PATH_MAX];
    if (!config_get_path(path, sizeof(path))) {
        if (error_msg != NULL) {
            snprintf(error_msg, error_msg_size, "Path too long");
        }
        return;
    }

    /* Write to a temp file and rename() over the real one atomically on
     * success; writing directly to tfm.ini could leave it truncated or
     * half-written on a disk-full/write error.
     *
     * mkstemp(), not a fixed "%s.tmp" + fopen("w"): a predictable tmp
     * path lets an attacker pre-plant a symlink there and have it
     * silently followed and truncated. mkstemp() picks a random name and
     * creates it atomically (O_CREAT|O_EXCL); fchmod(0600) keeps the
     * saved paths private regardless of umask. */
    char tmp_path[PATH_MAX];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.XXXXXX", path);
    if (n <= 0 || (size_t)n >= sizeof(tmp_path)) {
        if (error_msg != NULL) {
            snprintf(error_msg, error_msg_size, "Path too long");
        }
        return;
    }

    int fd = mkstemp(tmp_path);
    if (fd == -1) {
        if (error_msg != NULL) {
            snprintf(error_msg, error_msg_size, "Cannot create \"%s\": %s", tmp_path, strerror(errno));
        }
        return;
    }
    fchmod(fd, 0600);

    FILE *fp = fdopen(fd, "w");
    if (fp == NULL) {
        int saved_errno = errno;
        close(fd);
        remove(tmp_path);
        if (error_msg != NULL) {
            snprintf(error_msg, error_msg_size, "Cannot write \"%s\": %s", tmp_path, strerror(saved_errno));
        }
        return;
    }

    /* Sized for the largest field (a PATH_MAX path) fully escaped, and
     * reused for every key. A value that still doesn't fit fails the
     * whole save rather than writing a truncated line. */
    char esc[PATH_MAX * 2 + 16];
    int ok = 1;

#define WRITE_KV(key, value) \
    (ok = ok && escape_value((value), esc, sizeof(esc)) && fprintf(fp, key "=%s\n", esc) >= 0)

    ok = ok && fprintf(fp, "[panels]\n") >= 0;
    WRITE_KV("left_path", cfg->left_path);
    WRITE_KV("right_path", cfg->right_path);
    ok = ok && fprintf(fp, "\n[display]\n") >= 0;
    WRITE_KV("border_color", cfg->border_color);
    WRITE_KV("panel_border_color", cfg->panel_border_color);
    WRITE_KV("text_color", cfg->text_color);
    WRITE_KV("cursor_color", cfg->cursor_color);
    WRITE_KV("dir_color", cfg->dir_color);
    WRITE_KV("mark_color", cfg->mark_color);
    WRITE_KV("icons", cfg->icons);
    WRITE_KV("gui_theme", cfg->gui_theme);

#undef WRITE_KV

    if (!ok && error_msg != NULL) {
        /* Either escape_value() truncation or an fprintf() failure; errno
         * isn't reliably set for the latter before a flush, so the two
         * can't always be told apart. */
        snprintf(error_msg, error_msg_size, "Could not write config data (value too long or a write error)");
    }

    if (fclose(fp) != 0) {
        if (ok && error_msg != NULL) {
            /* A buffered write error (e.g. ENOSPC) can surface only here,
             * after every fprintf() above appeared to succeed. */
            snprintf(error_msg, error_msg_size, "Cannot write \"%s\": %s", tmp_path, strerror(errno));
        }
        ok = 0;
    }

    if (ok && rename(tmp_path, path) == 0) {
        return;
    }
    if (ok && error_msg != NULL) {
        snprintf(error_msg, error_msg_size, "Cannot save to \"%s\": %s", path, strerror(errno));
    }
    remove(tmp_path);
}
