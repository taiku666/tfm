#include "omarchy_theme.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tfm_common.h"

/* Trims leading/trailing whitespace (including the line's own "\n"/"\r")
 * in place. */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    *end = '\0';
    return s;
}

/* Parses the value half of a "key = value" line in place (colors.toml
 * writes values like accent = "#f38d70"), dropping a trailing TOML
 * "# comment". A quoted value is everything up to its closing quote, so
 * the '#' of a hex color inside the quotes is never mistaken for a
 * comment, and whatever follows the closing quote (a comment, or junk)
 * is ignored. An unquoted value - not valid TOML, but tolerated for a
 * hand-edited file - ends at the first '#' preceded by whitespace, so
 * a bare "accent = #f38d70" still keeps its leading '#'.
 *
 * The previous version trimmed trailing whitespace and only then checked
 * "ends with a quote", so accent = "#f38d70" # comment kept the quotes
 * AND the comment - and that whole string went verbatim into the
 * @define-color CSS, which GTK then rejected wholesale, silently
 * dropping the entire theme (CR4-M5). An unterminated quote returns "",
 * i.e. "no usable value". */
static char *parse_value(char *s)
{
    s = trim(s);
    if (*s == '"') {
        s++;
        char *close = strchr(s, '"');
        if (close == NULL) {
            return s + strlen(s);
        }
        *close = '\0';
        return s;
    }

    /* p > s guards the p[-1] lookbehind. */
    for (char *p = s; *p != '\0'; p++) {
        if (p > s && *p == '#' && (p[-1] == ' ' || p[-1] == '\t')) {
            *p = '\0';
            break;
        }
    }
    return trim(s);
}

/* Colors from colors.toml are spliced verbatim into CSS (see
 * gui_main.c's apply_omarchy_theme()), so anything other than a plain
 * "#rgb"/"#rrggbb" hex color - the same two forms contrasting_fg_for()
 * understands - is rejected here instead of being handed to GTK. A value
 * that isn't a color (a typo, a stray ";", a CSS fragment) would
 * otherwise make gtk_css_provider_load_from_string() discard the whole
 * theme, not just that one color. */
static int is_hex_color(const char *s)
{
    size_t len = strlen(s);
    if (s[0] != '#' || (len != 4 && len != 7)) {
        return 0;
    }
    for (size_t i = 1; i < len; i++) {
        if (!isxdigit((unsigned char)s[i])) {
            return 0;
        }
    }
    return 1;
}

/* Copies value into a color field only if it's a valid hex color;
 * otherwise leaves the field empty so the caller's libadwaita-default
 * fallback for that color applies. Returns 1 if the value was stored. */
static int store_color(char *field, size_t field_size, const char *value)
{
    if (!is_hex_color(value)) {
        field[0] = '\0';
        return 0;
    }
    snprintf(field, field_size, "%s", value);
    return 1;
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
        char *key = trim(key_buf);
        char *value = parse_value(eq + 1);

        if (strcmp(key, "mode") == 0) {
            out->is_dark = (strcmp(value, "dark") == 0);
        } else if (strcmp(key, "accent") == 0) {
            /* An invalid accent counts as "not found": it's the one color
             * the whole theme hinges on (see the header), so the caller
             * falls back to standard system theming instead. */
            found_accent = store_color(out->accent, sizeof(out->accent), value);
        } else if (strcmp(key, "background") == 0) {
            store_color(out->background, sizeof(out->background), value);
        } else if (strcmp(key, "foreground") == 0) {
            store_color(out->foreground, sizeof(out->foreground), value);
        } else if (strcmp(key, "dark_background") == 0) {
            store_color(out->dark_background, sizeof(out->dark_background), value);
        } else if (strcmp(key, "selection") == 0) {
            store_color(out->selection, sizeof(out->selection), value);
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
