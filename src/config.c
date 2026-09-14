#define _DEFAULT_SOURCE

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* Default list used when tfm.ini has no (or an empty) [editor] section. */
#define DEFAULT_EDITOR_EXTENSIONS \
    "md,txt,ini,lua,c,h,cpp,hpp,py,sh,json,yaml,yml,conf,cfg,toml,log,rs,go,js,ts,css,html,xml"

#define CONFIG_DIR_NAME ".tfm"
#define CONFIG_FILE_NAME "tfm.ini"

static void config_get_dir(char *buf, size_t len)
{
    const char *home = getenv("HOME");
    if (home == NULL) {
        home = ".";
    }
    snprintf(buf, len, "%s/%s", home, CONFIG_DIR_NAME);
}

static void config_get_path(char *buf, size_t len)
{
    char dir[PATH_MAX];
    config_get_dir(dir, sizeof(dir));
    snprintf(buf, len, "%s/%s", dir, CONFIG_FILE_NAME);
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
    snprintf(cfg->icons, sizeof(cfg->icons), "omarchy");
    snprintf(cfg->gui_theme, sizeof(cfg->gui_theme), "omarchy");
    snprintf(cfg->editor_extensions, sizeof(cfg->editor_extensions), DEFAULT_EDITOR_EXTENSIONS);
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

void config_load(Config *cfg)
{
    config_set_defaults(cfg);

    char path[PATH_MAX];
    config_get_path(path, sizeof(path));

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
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

        if (strcmp(section, "panels") == 0) {
            if (strcmp(key, "left_path") == 0) {
                snprintf(cfg->left_path, sizeof(cfg->left_path), "%s", value);
            } else if (strcmp(key, "right_path") == 0) {
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
            } else if (strcmp(key, "icons") == 0) {
                snprintf(cfg->icons, sizeof(cfg->icons), "%s", value);
            } else if (strcmp(key, "gui_theme") == 0) {
                snprintf(cfg->gui_theme, sizeof(cfg->gui_theme), "%s", value);
            }
        } else if (strcmp(section, "editor") == 0) {
            if (strcmp(key, "extensions") == 0) {
                snprintf(cfg->editor_extensions, sizeof(cfg->editor_extensions), "%s", value);
            }
        }
    }

    fclose(fp);
}

void config_save(const Config *cfg)
{
    char dir[PATH_MAX];
    config_get_dir(dir, sizeof(dir));
    mkdir(dir, 0755);

    char path[PATH_MAX];
    config_get_path(path, sizeof(path));

    /* Write to a temp file and rename() over the real one atomically on
     * success; writing directly to tfm.ini could leave it truncated or
     * half-written on a disk-full/write error. */
    char tmp_path[PATH_MAX];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof(tmp_path)) {
        return;
    }

    FILE *fp = fopen(tmp_path, "w");
    if (fp == NULL) {
        return;
    }

    int ok = 1;
    ok = ok && fprintf(fp, "[panels]\n") >= 0;
    ok = ok && fprintf(fp, "left_path=%s\n", cfg->left_path) >= 0;
    ok = ok && fprintf(fp, "right_path=%s\n", cfg->right_path) >= 0;
    ok = ok && fprintf(fp, "\n[display]\n") >= 0;
    ok = ok && fprintf(fp, "border_color=%s\n", cfg->border_color) >= 0;
    ok = ok && fprintf(fp, "panel_border_color=%s\n", cfg->panel_border_color) >= 0;
    ok = ok && fprintf(fp, "text_color=%s\n", cfg->text_color) >= 0;
    ok = ok && fprintf(fp, "cursor_color=%s\n", cfg->cursor_color) >= 0;
    ok = ok && fprintf(fp, "icons=%s\n", cfg->icons) >= 0;
    ok = ok && fprintf(fp, "gui_theme=%s\n", cfg->gui_theme) >= 0;
    ok = ok && fprintf(fp, "\n[editor]\n") >= 0;
    ok = ok && fprintf(fp, "extensions=%s\n", cfg->editor_extensions) >= 0;

    if (fclose(fp) != 0) {
        ok = 0;
    }

    if (ok) {
        rename(tmp_path, path);
    } else {
        remove(tmp_path);
    }
}

int config_is_editor_extension(const Config *cfg, const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if (dot == NULL || dot == filename || dot[1] == '\0') {
        /* A leading dot (e.g. ".bashrc") is part of the filename, not an extension. */
        return 0;
    }
    const char *ext = dot + 1;

    char list[sizeof(cfg->editor_extensions)];
    snprintf(list, sizeof(list), "%s", cfg->editor_extensions);

    char *saveptr = NULL;
    for (char *tok = strtok_r(list, ",", &saveptr); tok != NULL; tok = strtok_r(NULL, ",", &saveptr)) {
        char *entry = trim(tok);
        if (entry[0] != '\0' && strcasecmp(entry, ext) == 0) {
            return 1;
        }
    }
    return 0;
}
