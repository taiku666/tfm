#ifndef TFM_SCREEN_H
#define TFM_SCREEN_H

#include <stddef.h>

/* Gets the current terminal size. */
void screen_get_size(int *rows, int *cols);

/* Enables/disables "fancy" style (Nerd Font icons/Unicode blocks instead
 * of plain ASCII) for elements like the progress bar. */
void screen_set_fancy_style(int enabled);

/* Sets the color the progress bar (see screen_draw_progress_popup) is
 * drawn in - usually the same as the outer border color. */
void screen_set_progress_bar_color(const char *color_name);

/* Checks whether name is a valid color name ("system" or one of the
 * names in screen.c, e.g. "red", "bright_cyan", ...). Used to validate
 * tfm.ini at startup. */
int screen_color_name_is_valid(const char *name);

/* Clears the screen and moves the cursor to the top left. */
void screen_clear(void);

/* Switches to the terminal's alternate screen buffer (like vim/less/htop)
 * so the previous screen contents are preserved and restored on exit,
 * without tfm's screens ending up in scrollback history. Call once at
 * startup. */
void screen_enter_alt_screen(void);

/* Leaves the alternate screen buffer. Call once on exit. */
void screen_leave_alt_screen(void);

/* Draws a border around the edge of the terminal window.
 * color_name: "system" for the terminal's default color, or one of the
 * names in screen.c.
 * title: optional text centered into the top border line (NULL for no
 * title). */
void screen_draw_border(const char *color_name, const char *title);

/* Hides/shows the terminal cursor. */
void screen_hide_cursor(void);
void screen_show_cursor(void);

/* Writes text to the line directly below the top border. */
void screen_draw_menu_bar(const char *text);

/* Writes text to the line directly above the bottom border. */
void screen_draw_keybinding_bar(const char *text);

typedef struct {
    const char *key;   /* e.g. "F5" */
    const char *label; /* e.g. "Copy" */
} FunctionKey;

/* Draws a function-key bar (like mc): each key as a highlighted "button"
 * (background color_name, "system" = inverted), followed by its label in
 * normal text color. */
void screen_draw_function_bar(const FunctionKey *keys, int count, const char *color_name);

/* Writes prompt+text to the command line (one line above the keybinding
 * bar) and positions the cursor right after it. */
void screen_draw_command_line(const char *prompt, const char *text);

/* Draws a centered popup with title and message. The popup is not
 * removed automatically - the caller must redraw the UI after a
 * keypress. */
void screen_draw_popup(const char *title, const char *message);

/* Draws a thin border at an arbitrary position (for panels). */
void screen_draw_box(int row, int col, int width, int height, const char *color_name);

/* Writes text left-aligned at row/col, truncated/padded to max_width
 * characters. */
void screen_print_at(int row, int col, int max_width, const char *text);

/* Like screen_print_at, but colored with color_name ("system" = no
 * color, terminal default). */
void screen_print_at_colored(int row, int col, int max_width, const char *text, const char *color_name);

/* Like screen_print_at_colored, but bold. */
void screen_print_at_colored_bold(int row, int col, int max_width, const char *text, const char *color_name);

/* Draws the selection cursor bar in panels. color_name == "system" uses
 * inverted display (terminal's default highlight), otherwise a
 * background in the given color. */
void screen_print_at_selected(int row, int col, int max_width, const char *text, const char *color_name);

/* Draws a thin horizontal separator line (used below the path display in
 * panels). */
void screen_draw_hline(int row, int col, int width, const char *color_name);

/* Shows a centered popup with a progress bar (0-100). item is e.g. the
 * filename currently being processed. Used for file operations with
 * progress (copy, and later move/delete). */
void screen_draw_progress_popup(const char *title, const char *item, double percent);

typedef enum {
    SCREEN_CHOICE_SKIP,
    SCREEN_CHOICE_RETRY,
    SCREEN_CHOICE_ABORT,
    SCREEN_CHOICE_OVERWRITE
} ScreenChoice;

/* Shows an error popup with three options (Skip/Retry/Abort) and blocks
 * until S/R/A is pressed. */
ScreenChoice screen_prompt_choice(const char *title, const char *message);

/* Shows a popup when a target file/folder already exists, with
 * Skip/Overwrite/Abort options (S/O/A). */
ScreenChoice screen_prompt_overwrite(const char *path);

/* Shows a text-input popup with title. buffer holds the prefilled text on
 * entry (e.g. the current filename) and is replaced with the edited text
 * on confirm (Enter). buffer_size is the size of buffer. Returns 1 on
 * confirm, 0 on cancel (Esc) - buffer is left unchanged in that case. */
int screen_prompt_text(const char *title, char *buffer, size_t buffer_size);

/* Shows a yes/no popup and blocks until Y/N (or Esc = No) is pressed.
 * Returns 1 for yes, 0 for no. */
int screen_prompt_confirm(const char *title, const char *message);

#endif
