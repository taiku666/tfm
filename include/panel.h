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
    int icons_enabled; /* 1 = Nerd Font icons (omarchy), 0 = name only */
} PanelTheme;

/* Initializes panel with path and loads its contents. */
void panel_init(Panel *panel, const char *path);

/* Reloads the current directory (panel->path). */
void panel_reload(Panel *panel);

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
