#include "omarchy_theme.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tfm_common.h"

/* Trims whitespace and strips one pair of surrounding quotes in-place
 * (colors.toml writes values like accent = "#f38d70"). */
static char *trim_and_unquote(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    *end = '\0';

    size_t len = strlen(s);
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
        s[len - 1] = '\0';
        s++;
    }
    return s;
}

int omarchy_theme_load(OmarchyThemeColors *out)
{
    memset(out, 0, sizeof(*out));

    const char *home = getenv("HOME");
    if (home == NULL) {
        return 0;
    }

    char path[PATH_MAX];
    /* Checked explicitly: an unchecked snprintf() truncating a very long
     * $HOME would silently build a shorter, unrelated path instead of
     * failing outright - fopen() on that path would most likely just
     * fail anyway (ENOENT), but "definitely refuse" is more honest than
     * "probably happens to fail". */
    if ((size_t)snprintf(path, sizeof(path), "%s/.local/state/omarchy/current/theme/colors.toml",
                          home) >= sizeof(path)) {
        return 0;
    }

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return 0;
    }

    int found_accent = 0;

    /* Sized generously (see config.c for the same fix) - too small a
     * buffer would make fgets() truncate an unusually long line, and the
     * remainder would then be misread as its own broken "line". */
    char line[1024];
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';

        char key_buf[sizeof(line)];
        snprintf(key_buf, sizeof(key_buf), "%s", line);
        char *key = trim_and_unquote(key_buf);
        char *value = trim_and_unquote(eq + 1);

        if (strcmp(key, "mode") == 0) {
            out->is_dark = (strcmp(value, "dark") == 0);
        } else if (strcmp(key, "accent") == 0) {
            snprintf(out->accent, sizeof(out->accent), "%s", value);
            found_accent = 1;
        } else if (strcmp(key, "background") == 0) {
            snprintf(out->background, sizeof(out->background), "%s", value);
        } else if (strcmp(key, "foreground") == 0) {
            snprintf(out->foreground, sizeof(out->foreground), "%s", value);
        } else if (strcmp(key, "dark_background") == 0) {
            snprintf(out->dark_background, sizeof(out->dark_background), "%s", value);
        } else if (strcmp(key, "selection") == 0) {
            snprintf(out->selection, sizeof(out->selection), "%s", value);
        }
    }

    fclose(fp);
    /* "mode" is optional; if absent, is_dark just stays 0 (light). */
    return found_accent;
}

/* Finds the first active (not "--"-commented) "rounding = N" line in a
 * Hyprland Lua config. The prefix check on "rounding" doubles as a word
 * boundary since other keys like "gradient_rounding" won't start with
 * "rounding" after trimming leading whitespace. */
static int scan_rounding_in_file(const char *path, int *out_value)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return 0;
    }

    char line[1024];
    int found = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (p[0] == '-' && p[1] == '-') {
            continue;
        }
        if (strncmp(p, "rounding", 8) != 0) {
            continue;
        }
        char *after = p + 8;
        while (*after == ' ' || *after == '\t') {
            after++;
        }
        if (*after != '=') {
            continue;
        }
        after++;
        while (*after == ' ' || *after == '\t') {
            after++;
        }
        /* strtol(), not sscanf("%d", ...): sscanf's %d on a pathological
         * number of digits (this file is user-editable Hyprland Lua
         * config) is undefined behavior on overflow, whereas strtol()
         * defines the outcome (clamped to LONG_MIN/LONG_MAX, errno set)
         * - clamped below to a sane corner-radius range regardless. */
        errno = 0;
        char *end = NULL;
        long parsed = strtol(after, &end, 10);
        if (end != after && errno == 0 && parsed >= 0 && parsed <= 1000) {
            *out_value = (int)parsed;
            found = 1;
            break;
        }
    }

    fclose(fp);
    return found;
}

int omarchy_hypr_corner_rounding(void)
{
    const char *home = getenv("HOME");
    char path[PATH_MAX];
    int value = 0;

    if (home != NULL &&
        (size_t)snprintf(path, sizeof(path), "%s/.config/hypr/looknfeel.lua", home) < sizeof(path)) {
        if (scan_rounding_in_file(path, &value)) {
            return value;
        }
    }

    if (scan_rounding_in_file("/usr/share/omarchy/default/hypr/looknfeel.lua", &value)) {
        return value;
    }

    return 0;
}
