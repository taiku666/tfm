#define _DEFAULT_SOURCE

#include "panel.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

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
    panel->marks = NULL;
    panel->mark_sizes = NULL;
    panel->mark_count = 0;
    panel->marked_dirs = 0;
    panel->marked_bytes = 0;
    panel_reload(panel);
}

static int is_markable(const DirEntryInfo *entry)
{
    return strcmp(entry->name, "..") != 0;
}

/* lstat size of a marked file: a symlink counts as the link itself,
 * matching what a copy would write. 0 if it vanished meanwhile. */
static long long entry_size(const Panel *panel, const DirEntryInfo *entry)
{
    char path[PATH_MAX];
    struct stat st;
    if (entry->is_dir || !path_join(path, sizeof(path), panel->path, entry->name) || lstat(path, &st) != 0) {
        return 0;
    }
    return (long long)st.st_size;
}

/* Unmarking subtracts the size recorded at marking time, not a fresh
 * lstat, so a file that changed size meanwhile can't skew the total. */
static void add_mark_totals(Panel *panel, size_t index, int sign)
{
    const DirEntryInfo *entry = &panel->entries[index];
    if (sign > 0) {
        panel->mark_sizes[index] = entry_size(panel, entry);
        panel->mark_count++;
        panel->marked_dirs += entry->is_dir ? 1 : 0;
        panel->marked_bytes += panel->mark_sizes[index];
    } else {
        panel->mark_count--;
        panel->marked_dirs -= entry->is_dir ? 1 : 0;
        panel->marked_bytes -= panel->mark_sizes[index];
    }
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Replaces panel's listing with entries/count (taking ownership). With
 * keep_marks, entries whose name was marked before stay marked - looked
 * up in a sorted copy of the old marked names, so re-marking stays
 * O(n log m) even with thousands of marks. */
static void panel_set_listing(Panel *panel, DirEntryInfo *entries, size_t count, int keep_marks)
{
    const char **old_marked = NULL;
    size_t old_marked_count = 0;
    if (keep_marks && panel->marks != NULL && panel->mark_count > 0) {
        old_marked = malloc(panel->mark_count * sizeof(*old_marked));
        if (old_marked != NULL) {
            for (size_t i = 0; i < panel->count; i++) {
                if (panel->marks[i]) {
                    old_marked[old_marked_count++] = panel->entries[i].name;
                }
            }
            qsort(old_marked, old_marked_count, sizeof(*old_marked), compare_names);
        }
    }

    /* calloc(0) may return NULL, which would read as "allocation
     * failed" - size them for at least one entry. */
    unsigned char *marks = calloc(count > 0 ? count : 1, 1);
    long long *mark_sizes = calloc(count > 0 ? count : 1, sizeof(*mark_sizes));
    if (marks == NULL || mark_sizes == NULL) {
        free(marks);
        free(mark_sizes);
        marks = NULL;
        mark_sizes = NULL;
    }

    panel->mark_count = 0;
    panel->marked_dirs = 0;
    panel->marked_bytes = 0;
    DirEntryInfo *old_entries = panel->entries;
    panel->entries = entries;
    panel->count = count;
    if (marks != NULL && old_marked_count > 0) {
        for (size_t i = 0; i < count; i++) {
            const char *name = entries[i].name;
            if (bsearch(&name, old_marked, old_marked_count, sizeof(*old_marked), compare_names) != NULL) {
                marks[i] = 1;
            }
        }
    }
    free(panel->marks);
    free(panel->mark_sizes);
    panel->marks = marks;
    panel->mark_sizes = mark_sizes;
    if (marks != NULL) {
        for (size_t i = 0; i < count; i++) {
            if (marks[i]) {
                add_mark_totals(panel, i, 1);
            }
        }
    }

    /* old_marked points into the old entries, so they go last. */
    free(old_marked);
    if (old_entries != NULL) {
        dir_list_free(old_entries);
    }
}

int panel_reload(Panel *panel)
{
    DirEntryInfo *new_entries = NULL;
    size_t new_count = 0;

    if (dir_list(panel->path, &new_entries, &new_count) != 0) {
        /* A failed reload (dir deleted/unmounted, permissions revoked...)
         * must not destroy a still-valid existing listing, and keeps
         * scroll_offset/selected_index too - they still index that same
         * listing, and a transient error shouldn't throw the user back to
         * the top. With no entries at all yet (e.g. an invalid path saved
         * in tfm.ini), synthesize a ".." entry so the panel isn't a dead
         * end. */
        int saved_errno = errno;
        if (panel->entries == NULL) {
            DirEntryInfo *fallback = malloc(sizeof(DirEntryInfo));
            if (fallback != NULL) {
                snprintf(fallback[0].name, sizeof(fallback[0].name), "..");
                fallback[0].is_dir = 1;
                panel_set_listing(panel, fallback, 1, 0);
            }
            panel->scroll_offset = 0;
            panel->selected_index = 0;
        }
        errno = saved_errno;
        return 0;
    }

    panel_set_listing(panel, new_entries, new_count, 1);
    panel->scroll_offset = 0;
    panel->selected_index = 0;
    return 1;
}

int panel_change_dir(Panel *panel, const char *command, char *error_msg, size_t error_msg_size)
{
    /* Resolved into a scratch copy, not panel->path itself: builtin_cd()
     * rewrites its buffer on success, and a dir_list() failure right
     * afterwards (directory vanished between builtin_cd()'s probe and the
     * listing) would leave the new path shown above the old listing - and
     * persisted into tfm.ini on quit. Path and listing are only swapped
     * together, after both have succeeded. */
    char new_path[PATH_MAX];
    snprintf(new_path, sizeof(new_path), "%s", unsized(panel->path));
    if (!builtin_cd(new_path, command, error_msg, error_msg_size)) {
        return 0;
    }

    DirEntryInfo *new_entries = NULL;
    size_t new_count = 0;
    if (dir_list(new_path, &new_entries, &new_count) != 0) {
        if (error_msg != NULL) {
            snprintf(error_msg, error_msg_size, "Cannot read directory: %s", strerror(errno));
        }
        return 0;
    }

    snprintf(panel->path, sizeof(panel->path), "%s", new_path);
    panel_set_listing(panel, new_entries, new_count, 0);
    panel->scroll_offset = 0;
    panel->selected_index = 0;
    return 1;
}

void panel_free(Panel *panel)
{
    if (panel->entries != NULL) {
        dir_list_free(panel->entries);
        panel->entries = NULL;
        panel->count = 0;
    }
    free(panel->marks);
    free(panel->mark_sizes);
    panel->marks = NULL;
    panel->mark_sizes = NULL;
    panel->mark_count = 0;
    panel->marked_dirs = 0;
    panel->marked_bytes = 0;
}

void panel_toggle_mark(Panel *panel, size_t index)
{
    if (panel->marks == NULL || index >= panel->count || !is_markable(&panel->entries[index])) {
        return;
    }
    panel->marks[index] = !panel->marks[index];
    add_mark_totals(panel, index, panel->marks[index] ? 1 : -1);
}

void panel_toggle_mark_all(Panel *panel)
{
    if (panel->marks == NULL) {
        return;
    }
    size_t markable = 0;
    for (size_t i = 0; i < panel->count; i++) {
        markable += is_markable(&panel->entries[i]) ? 1 : 0;
    }
    int mark = panel->mark_count < markable;
    for (size_t i = 0; i < panel->count; i++) {
        if (is_markable(&panel->entries[i]) && panel->marks[i] != mark) {
            panel->marks[i] = (unsigned char)mark;
            add_mark_totals(panel, i, mark ? 1 : -1);
        }
    }
}

void panel_clear_marks(Panel *panel)
{
    if (panel->marks != NULL) {
        memset(panel->marks, 0, panel->count > 0 ? panel->count : 1);
    }
    panel->mark_count = 0;
    panel->marked_dirs = 0;
    panel->marked_bytes = 0;
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

            int marked = panel->marks != NULL && panel->marks[idx];
            char text[512];
            const char *icon = icon_for_entry(entry->name, entry->is_dir, theme->icons_enabled);
            snprintf(text, sizeof(text), "%s %s%s%s", icon, marked ? "*" : "", entry->name,
                     entry->is_dir ? "/" : "");

            const char *entry_color =
                marked ? theme->mark_color : (entry->is_dir ? theme->dir_color : theme->text_color);

            if (is_active && (int)idx == panel->selected_index) {
                screen_print_at_selected(line_row, col + 1, width - 2, text, theme->cursor_color);
            } else {
                screen_print_at_colored_bold(line_row, col + 1, width - 2, text, entry_color);
            }
        } else {
            screen_print_at_colored(line_row, col + 1, width - 2, "", theme->text_color);
        }
    }

    if (panel->mark_count > 0 && width > 6) {
        /* Drawn into the bottom border, like a title, so marking costs no
         * entry row; padded with a space each side and clipped to fit. */
        char summary[96], label[100];
        format_mark_summary(panel->mark_count, panel->marked_dirs, panel->marked_bytes, summary,
                            sizeof(summary));
        snprintf(label, sizeof(label), " %s ", summary);
        int label_width = (int)strlen(label);
        if (label_width > width - 4) {
            label_width = width - 4;
        }
        screen_print_at_colored_bold(row + height - 1, col + 2, label_width, label, theme->mark_color);
    }
}
