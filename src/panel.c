#include "panel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "screen.h"

/* Nerd Font icons (UTF-8), matched to "eza --icons=always" output so
 * they fit the familiar terminal look. Active only when icons=omarchy. */
#define ICON_FOLDER_GENERIC "\xee\x97\xbf"
#define ICON_FOLDER_INCLUDE "\xee\x97\xbc"
#define ICON_FOLDER_SRC "\xf3\xb0\xa3\x9e"
#define ICON_FOLDER_BUILD "\xf3\xb1\xa7\xbc"
#define ICON_FILE_MAKEFILE "\xee\x99\xb3"
#define ICON_FILE_C "\xee\x98\x9e"
#define ICON_FILE_MARKDOWN "\xf3\xb0\x82\xba"
#define ICON_FILE_INI "\xf3\xb1\x81\xbb"
#define ICON_FILE_JSON "\xee\x98\x8b"
#define ICON_FILE_TEXT "\xef\x85\x9c"
#define ICON_FILE_GENERIC "\xf3\xb0\xa1\xaf"

/* Plain, font-independent placeholders for icons=off. */
#define ICON_FOLDER_PLAIN "\xef\x81\xbb"
#define ICON_FILE_PLAIN "\xef\x85\x9b"

static const char *file_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name) {
        return "";
    }
    return dot + 1;
}

static const char *icon_for_entry(const char *name, int is_dir, int icons_enabled)
{
    if (!icons_enabled) {
        return is_dir ? ICON_FOLDER_PLAIN : ICON_FILE_PLAIN;
    }

    if (is_dir) {
        if (strcasecmp(name, "include") == 0) {
            return ICON_FOLDER_INCLUDE;
        }
        if (strcasecmp(name, "src") == 0) {
            return ICON_FOLDER_SRC;
        }
        if (strcasecmp(name, "build") == 0) {
            return ICON_FOLDER_BUILD;
        }
        return ICON_FOLDER_GENERIC;
    }

    if (strcasecmp(name, "Makefile") == 0) {
        return ICON_FILE_MAKEFILE;
    }

    const char *ext = file_extension(name);
    if (strcasecmp(ext, "c") == 0 || strcasecmp(ext, "h") == 0) {
        return ICON_FILE_C;
    }
    if (strcasecmp(ext, "md") == 0) {
        return ICON_FILE_MARKDOWN;
    }
    if (strcasecmp(ext, "ini") == 0) {
        return ICON_FILE_INI;
    }
    if (strcasecmp(ext, "json") == 0) {
        return ICON_FILE_JSON;
    }
    if (strcasecmp(ext, "txt") == 0) {
        return ICON_FILE_TEXT;
    }
    return ICON_FILE_GENERIC;
}

void panel_init(Panel *panel, const char *path)
{
    snprintf(panel->path, sizeof(panel->path), "%s", path);
    panel->entries = NULL;
    panel->count = 0;
    panel->scroll_offset = 0;
    panel->selected_index = 0;
    panel_reload(panel);
}

void panel_reload(Panel *panel)
{
    DirEntryInfo *new_entries = NULL;
    size_t new_count = 0;

    if (dir_list(panel->path, &new_entries, &new_count) != 0) {
        /* A failed reload (dir deleted/unmounted, permissions revoked...)
         * must not destroy a still-valid existing listing. If there are
         * no entries at all yet (e.g. an invalid path saved in tfm.ini),
         * synthesize a ".." entry so the panel isn't a dead end. */
        if (panel->entries == NULL) {
            DirEntryInfo *fallback = malloc(sizeof(DirEntryInfo));
            if (fallback != NULL) {
                snprintf(fallback[0].name, sizeof(fallback[0].name), "..");
                fallback[0].is_dir = 1;
                panel->entries = fallback;
                panel->count = 1;
            }
        }
        panel->scroll_offset = 0;
        panel->selected_index = 0;
        return;
    }

    if (panel->entries != NULL) {
        dir_list_free(panel->entries);
    }
    panel->entries = new_entries;
    panel->count = new_count;
    panel->scroll_offset = 0;
    panel->selected_index = 0;
}

void panel_free(Panel *panel)
{
    if (panel->entries != NULL) {
        dir_list_free(panel->entries);
        panel->entries = NULL;
        panel->count = 0;
    }
}

void panel_move_selection(Panel *panel, int delta, int visible_rows)
{
    if (panel->count == 0) {
        return;
    }

    int max_index = (int)panel->count - 1;

    panel->selected_index += delta;
    if (panel->selected_index < 0) {
        panel->selected_index = 0;
    }
    if (panel->selected_index > max_index) {
        panel->selected_index = max_index;
    }

    if (panel->selected_index < panel->scroll_offset) {
        panel->scroll_offset = panel->selected_index;
    }
    if (visible_rows > 0 && panel->selected_index >= panel->scroll_offset + visible_rows) {
        panel->scroll_offset = panel->selected_index - visible_rows + 1;
    }
}

void panel_draw(const Panel *panel, int row, int col, int width, int height, const PanelTheme *theme,
                 int is_active)
{
    screen_draw_box(row, col, width, height, theme->border_color);

    screen_print_at_colored(row + 1, col + 1, width - 2, panel->path, theme->text_color);

    screen_draw_hline(row + 2, col + 1, width - 2, theme->border_color);

    int visible_rows = height - PANEL_CHROME_ROWS;
    if (visible_rows < 0) {
        visible_rows = 0;
    }

    for (int i = 0; i < visible_rows; i++) {
        size_t idx = (size_t)panel->scroll_offset + (size_t)i;
        int line_row = row + 3 + i;

        if (idx < panel->count) {
            const DirEntryInfo *entry = &panel->entries[idx];

            char text[512];
            const char *icon = icon_for_entry(entry->name, entry->is_dir, theme->icons_enabled);
            snprintf(text, sizeof(text), "%s %s%s", icon, entry->name, entry->is_dir ? "/" : "");

            const char *entry_color = entry->is_dir ? theme->dir_color : theme->text_color;

            if (is_active && (int)idx == panel->selected_index) {
                screen_print_at_selected(line_row, col + 1, width - 2, text, theme->cursor_color);
            } else {
                screen_print_at_colored_bold(line_row, col + 1, width - 2, text, entry_color);
            }
        } else {
            screen_print_at_colored(line_row, col + 1, width - 2, "", theme->text_color);
        }
    }
}
