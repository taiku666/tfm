#ifndef TFM_PANEL_H
#define TFM_PANEL_H

#include <stddef.h>

#include "dir.h"
#include "tfm_common.h"

typedef struct {
    char path[PATH_MAX];
    DirEntryInfo *entries;
    size_t count;
    int scroll_offset;
    int selected_index;
} Panel;

/* Colors for drawing a panel (see screen.h for valid color names;
 * "system" means the default terminal color). */
typedef struct {
    const char *border_color;
    const char *text_color;
    const char *cursor_color;
    const char *dir_color;
    int icons_enabled; /* 1 = Nerd Font icons (omarchy), 0 = name only */
} PanelTheme;

/* Non-content rows in a panel_draw() box: top border + path line + header
 * separator + bottom border. Both panel_draw() itself (to compute how many
 * entry rows fit) and any caller that needs to know the same thing ahead of
 * time (e.g. main.c's panel_visible_rows(), for scrolling math before the
 * next draw) must agree on this number - previously two independent literal
 * `4`s that had to be kept in sync by hand. */
#define PANEL_CHROME_ROWS 4

/* Initializes panel with path and loads its contents. */
void panel_init(Panel *panel, const char *path);

/* Reloads the current directory (panel->path). Returns 1 on success. On
 * failure returns 0 (errno set by dir_list()) and keeps the previous
 * listing and cursor/scroll position intact, or - if there was no listing
 * yet - installs a single ".." entry so the panel isn't a dead end. */
int panel_reload(Panel *panel);

/* Runs a "cd ..." command (see builtin_cd()) against panel and, only if
 * both resolving the target and listing it succeed, switches panel->path
 * and its listing together. Returns 1 on success; on failure returns 0,
 * writes a reason into error_msg, and leaves the panel (path, listing,
 * cursor) completely unchanged. */
int panel_change_dir(Panel *panel, const char *command, char *error_msg, size_t error_msg_size);

/* Frees resources held by panel. */
void panel_free(Panel *panel);

/* Moves the selection by delta entries (negative = up, positive = down)
 * and adjusts the visible scroll window as needed. visible_rows is the
 * number of rows visible in the panel. */
void panel_move_selection(Panel *panel, int delta, int visible_rows);

/* Draws the panel as a box at row/col with width/height, including path
 * and scrollable directory contents, per theme. The selection bar is
 * drawn only when is_active != 0. */
void panel_draw(const Panel *panel, int row, int col, int width, int height, const PanelTheme *theme,
                 int is_active);

#endif
