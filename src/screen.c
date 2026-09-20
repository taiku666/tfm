#include "screen.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "input.h"
#include "tfm_common.h"

#define ANSI_RESET "\x1b[0m"

#define BOX_TOP_LEFT "\xe2\x94\x8f"     /* ┏ */
#define BOX_TOP_RIGHT "\xe2\x94\x93"    /* ┓ */
#define BOX_BOTTOM_LEFT "\xe2\x94\x97"  /* ┗ */
#define BOX_BOTTOM_RIGHT "\xe2\x94\x9b" /* ┛ */
#define BOX_HORIZONTAL "\xe2\x94\x81"   /* ━ */
#define BOX_VERTICAL "\xe2\x94\x83"     /* ┃ */

/* Thin border glyphs for popups, distinct from the thick outer border. */
#define POPUP_TOP_LEFT "\xe2\x94\x8c"     /* ┌ */
#define POPUP_TOP_RIGHT "\xe2\x94\x90"    /* ┐ */
#define POPUP_BOTTOM_LEFT "\xe2\x94\x94"  /* └ */
#define POPUP_BOTTOM_RIGHT "\xe2\x94\x98" /* ┘ */
#define POPUP_HORIZONTAL "\xe2\x94\x80"   /* ─ */
#define POPUP_VERTICAL "\xe2\x94\x82"     /* │ */

typedef struct {
    const char *name;
    const char *ansi_code;
} ColorEntry;

static const ColorEntry COLOR_TABLE[] = {
    {"black", "\x1b[30m"},
    {"red", "\x1b[31m"},
    {"green", "\x1b[32m"},
    {"yellow", "\x1b[33m"},
    {"blue", "\x1b[34m"},
    {"magenta", "\x1b[35m"},
    {"cyan", "\x1b[36m"},
    {"white", "\x1b[37m"},
    {"bright_black", "\x1b[90m"},
    {"bright_red", "\x1b[91m"},
    {"bright_green", "\x1b[92m"},
    {"bright_yellow", "\x1b[93m"},
    {"bright_blue", "\x1b[94m"},
    {"bright_magenta", "\x1b[95m"},
    {"bright_cyan", "\x1b[96m"},
    {"bright_white", "\x1b[97m"},
};

static const ColorEntry BG_COLOR_TABLE[] = {
    {"black", "\x1b[40m"},
    {"red", "\x1b[41m"},
    {"green", "\x1b[42m"},
    {"yellow", "\x1b[43m"},
    {"blue", "\x1b[44m"},
    {"magenta", "\x1b[45m"},
    {"cyan", "\x1b[46m"},
    {"white", "\x1b[47m"},
    {"bright_black", "\x1b[100m"},
    {"bright_red", "\x1b[101m"},
    {"bright_green", "\x1b[102m"},
    {"bright_yellow", "\x1b[103m"},
    {"bright_blue", "\x1b[104m"},
    {"bright_magenta", "\x1b[105m"},
    {"bright_cyan", "\x1b[106m"},
    {"bright_white", "\x1b[107m"},
};

static const char *color_lookup(const char *name)
{
    if (name == NULL || strcasecmp(name, "system") == 0) {
        return "";
    }

    size_t n = sizeof(COLOR_TABLE) / sizeof(COLOR_TABLE[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcasecmp(COLOR_TABLE[i].name, name) == 0) {
            return COLOR_TABLE[i].ansi_code;
        }
    }

    return "";
}

int screen_color_name_is_valid(const char *name)
{
    if (name == NULL || strcasecmp(name, "system") == 0) {
        return 1;
    }

    size_t n = sizeof(COLOR_TABLE) / sizeof(COLOR_TABLE[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcasecmp(COLOR_TABLE[i].name, name) == 0) {
            return 1;
        }
    }

    return 0;
}

static const char *bg_color_lookup(const char *name)
{
    if (name == NULL) {
        return NULL;
    }

    size_t n = sizeof(BG_COLOR_TABLE) / sizeof(BG_COLOR_TABLE[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcasecmp(BG_COLOR_TABLE[i].name, name) == 0) {
            return BG_COLOR_TABLE[i].ansi_code;
        }
    }

    return NULL;
}

/* "system" maps to bold blue (palette color 34), matching ls's default -
 * a theme-dependent accent rather than a fixed RGB color. */
static const char *border_color_lookup(const char *name)
{
    if (name == NULL || strcasecmp(name, "system") == 0) {
        return "\x1b[1;34m";
    }
    return color_lookup(name);
}

static int g_fancy_style = 0;
static char g_progress_bar_color[32] = "system";

void screen_set_fancy_style(int enabled)
{
    g_fancy_style = enabled;
}

void screen_set_progress_bar_color(const char *color_name)
{
    snprintf(g_progress_bar_color, sizeof(g_progress_bar_color), "%s", color_name);
}

void screen_get_size(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

void screen_clear(void)
{
    printf("\x1b[2J\x1b[H");
}

void screen_enter_alt_screen(void)
{
    printf("\x1b[?1049h");
    fflush(stdout);
}

void screen_leave_alt_screen(void)
{
    printf("\x1b[?1049l");
    fflush(stdout);
}

static void move_cursor(int row, int col)
{
    printf("\x1b[%d;%dH", row, col);
}

/* Draws one horizontal popup border edge (top or bottom) at row/col,
 * box_width columns wide - the same "corner + N horizontals + corner"
 * pattern was previously duplicated at four separate popup-drawing call
 * sites (screen_draw_popup, draw_popup_frame, screen_draw_progress_popup,
 * screen_prompt_buttons). */
static void draw_popup_border_edge(int row, int col, int box_width, int is_top)
{
    move_cursor(row, col);
    printf(is_top ? POPUP_TOP_LEFT : POPUP_BOTTOM_LEFT);
    for (int c = 0; c < box_width - 2; c++) {
        printf(POPUP_HORIZONTAL);
    }
    printf(is_top ? POPUP_TOP_RIGHT : POPUP_BOTTOM_RIGHT);
}

/* Blanks a rectangle with spaces - erases a popup box's previous
 * footprint before it's redrawn at a new size/position. */
static void clear_rect(int start_row, int start_col, int height, int width)
{
    for (int r = 0; r < height; r++) {
        move_cursor(start_row + r, start_col);
        for (int c = 0; c < width; c++) {
            putchar(' ');
        }
    }
}

/* Counts visible columns, not bytes: UTF-8 continuation bytes (10xxxxxx)
 * don't count. Assumes every codepoint is 1 column wide (fine for
 * icons/latin umlauts, not for e.g. CJK wide chars). strlen() would
 * overcount the visible width of multibyte content (icons, filenames
 * with umlauts). */
static int utf8_visual_width(const char *s)
{
    int width = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        if ((*p & 0xC0) != 0x80) {
            width++;
        }
    }
    return width;
}

/* Byte length of the longest prefix of text that is at most max_columns
 * visible columns wide - never cuts in the middle of a UTF-8 character.
 * Shared by print_utf8_padded() and any other caller that needs to
 * truncate/center text by codepoint instead of by byte (e.g.
 * screen_draw_border()'s title, which used strlen()+"%.*s" before). */
static size_t utf8_byte_len_for_width(const char *text, int max_columns)
{
    size_t byte_len = 0;
    int col = 0;
    while (text[byte_len] != '\0') {
        unsigned char c = (unsigned char)text[byte_len];
        if ((c & 0xC0) != 0x80) {
            if (col == max_columns) {
                break;
            }
            col++;
        }
        byte_len++;
    }
    return byte_len;
}

/* Prints text truncated/padded to visual_width visible columns (not
 * bytes) - never cuts in the middle of a UTF-8 character. */
static void print_utf8_padded(const char *text, int visual_width)
{
    if (visual_width < 0) {
        visual_width = 0;
    }

    size_t byte_len = utf8_byte_len_for_width(text, visual_width);
    int col = utf8_visual_width(text);
    if (col > visual_width) {
        col = visual_width;
    }

    printf("%.*s", (int)byte_len, text);
    for (int i = col; i < visual_width; i++) {
        putchar(' ');
    }
}

void screen_hide_cursor(void)
{
    printf("\x1b[?25l");
    fflush(stdout);
}

void screen_show_cursor(void)
{
    printf("\x1b[?25h");
    fflush(stdout);
}

void screen_draw_menu_bar(const char *text)
{
    int rows, cols;
    screen_get_size(&rows, &cols);
    (void)rows;

    move_cursor(2, 2);
    print_utf8_padded(text, cols - 2);
    fflush(stdout);
}

void screen_draw_function_bar(const FunctionKey *keys, int count, const char *color_name)
{
    int rows, cols;
    screen_get_size(&rows, &cols);

    const char *bg_code = bg_color_lookup(color_name);
    int reversed = (bg_code == NULL); /* "system" or unknown -> inverted */

    move_cursor(rows - 1, 2);

    int max_width = cols - 2;
    if (max_width < 0) {
        max_width = 0;
    }

    /* Unlike every other bar in this file, this one had no width limit -
     * long/many key labels could wrap past the line. Stop at an entry
     * boundary rather than mid-label. */
    int used = 0;
    for (int i = 0; i < count; i++) {
        int entry_width = utf8_visual_width(keys[i].key) + 1 + utf8_visual_width(keys[i].label) + 2;
        if (used + entry_width > max_width) {
            break;
        }

        if (reversed) {
            printf("\x1b[7m%s" ANSI_RESET, keys[i].key);
        } else {
            printf("%s%s" ANSI_RESET, bg_code, keys[i].key);
        }
        printf(" %s  ", keys[i].label);

        used += entry_width;
    }

    int remaining = max_width - used;
    if (remaining > 0) {
        printf("%*s", remaining, "");
    }

    fflush(stdout);
}

void screen_draw_command_line(const char *prompt, const char *text)
{
    int rows, cols;
    screen_get_size(&rows, &cols);

    int row = rows - 2;
    char line[512];
    snprintf(line, sizeof(line), "%s%s", prompt, text);

    move_cursor(row, 2);
    int max_width = cols - 2;
    if (max_width < 0) {
        max_width = 0;
    }
    print_utf8_padded(line, max_width);
    /* Use utf8_visual_width(), not strlen(), and clamp to max_width so the
     * cursor lands on the actually displayed (possibly truncated) column
     * instead of past the terminal edge or misplaced by multibyte bytes. */
    int cursor_offset = utf8_visual_width(line);
    if (cursor_offset > max_width) {
        cursor_offset = max_width;
    }
    move_cursor(row, 2 + cursor_offset);
    fflush(stdout);
}

/* Draws a centered 3-line popup (frame + 3 content lines) and returns the
 * start row/col and available inner width. Shared base for the info,
 * progress, and choice popups. */
static void draw_popup_frame(const char *lines[3], int *out_start_row, int *out_start_col, int *out_inner_width)
{
    int rows, cols;
    screen_get_size(&rows, &cols);

    int max_content = 0;
    for (int i = 0; i < 3; i++) {
        int len = utf8_visual_width(lines[i]);
        if (len > max_content) {
            max_content = len;
        }
    }

    int inner_width = max_content;
    int max_inner_width = cols - 6;
    if (max_inner_width < 10) {
        max_inner_width = 10;
    }
    if (inner_width > max_inner_width) {
        inner_width = max_inner_width;
    }

    int box_width = inner_width + 4;
    int box_height = 5;

    int start_row = (rows - box_height) / 2;
    if (start_row < 2) {
        start_row = 2;
    }
    int start_col = (cols - box_width) / 2;
    if (start_col < 2) {
        start_col = 2;
    }

    draw_popup_border_edge(start_row, start_col, box_width, 1);

    for (int i = 0; i < 3; i++) {
        move_cursor(start_row + 1 + i, start_col);
        printf(POPUP_VERTICAL " ");
        print_utf8_padded(lines[i], inner_width);
        printf(" " POPUP_VERTICAL);
    }

    draw_popup_border_edge(start_row + 4, start_col, box_width, 0);

    *out_start_row = start_row;
    *out_start_col = start_col;
    *out_inner_width = inner_width;
}

void screen_draw_popup(const char *title, const char *message)
{
    const char *lines[3] = {title, message, "Press any key to close"};
    int start_row, start_col, inner_width;
    draw_popup_frame(lines, &start_row, &start_col, &inner_width);
    fflush(stdout);
}

void screen_draw_box(int row, int col, int width, int height, const char *color_name)
{
    const char *color_code = border_color_lookup(color_name);

    printf("%s", color_code);

    draw_popup_border_edge(row, col, width, 1);

    for (int r = 1; r < height - 1; r++) {
        move_cursor(row + r, col);
        printf(POPUP_VERTICAL);
        move_cursor(row + r, col + width - 1);
        printf(POPUP_VERTICAL);
    }

    draw_popup_border_edge(row + height - 1, col, width, 0);

    printf(ANSI_RESET);
    fflush(stdout);
}

void screen_print_at_colored(int row, int col, int max_width, const char *text, const char *color_name)
{
    move_cursor(row, col);
    printf("%s", color_lookup(color_name));
    print_utf8_padded(text, max_width);
    printf(ANSI_RESET);
    fflush(stdout);
}

void screen_print_at_colored_bold(int row, int col, int max_width, const char *text, const char *color_name)
{
    move_cursor(row, col);
    printf("\x1b[1m%s", color_lookup(color_name));
    print_utf8_padded(text, max_width);
    printf(ANSI_RESET);
    fflush(stdout);
}

void screen_print_at_selected(int row, int col, int max_width, const char *text, const char *color_name)
{
    const char *bg_code = bg_color_lookup(color_name);

    move_cursor(row, col);
    if (bg_code != NULL) {
        printf("%s", bg_code);
        print_utf8_padded(text, max_width);
        printf(ANSI_RESET);
    } else {
        /* "system" -> terminal's default highlight (inverted) */
        printf("\x1b[7m");
        print_utf8_padded(text, max_width);
        printf(ANSI_RESET);
    }
    fflush(stdout);
}

void screen_draw_hline(int row, int col, int width, const char *color_name)
{
    const char *color_code = border_color_lookup(color_name);

    printf("%s", color_code);
    move_cursor(row, col);
    for (int c = 0; c < width; c++) {
        printf(POPUP_HORIZONTAL);
    }
    printf(ANSI_RESET);
    fflush(stdout);
}

void screen_draw_border(const char *color_name, const char *title)
{
    int rows, cols;
    screen_get_size(&rows, &cols);

    const char *color_code = border_color_lookup(color_name);

    printf("%s", color_code);

    move_cursor(1, 1);
    printf(BOX_TOP_LEFT);
    for (int c = 1; c < cols - 1; c++) {
        printf(BOX_HORIZONTAL);
    }
    printf(BOX_TOP_RIGHT);

    if (title != NULL && title[0] != '\0') {
        /* utf8_visual_width()/utf8_byte_len_for_width(), not strlen()+
         * "%.*s" on the raw byte count: a multibyte title (umlauts, box-
         * drawing/icon glyphs) would otherwise be mis-centered and could
         * be truncated mid-character, producing garbage bytes. */
        int max_title_len = cols - 4;
        if (max_title_len > 0) {
            int title_len = utf8_visual_width(title);
            if (title_len > max_title_len) {
                title_len = max_title_len;
            }
            size_t title_byte_len = utf8_byte_len_for_width(title, title_len);
            int title_col = (cols - title_len) / 2 + 1;
            move_cursor(1, title_col);
            printf(" %.*s ", (int)title_byte_len, title);
        }
    }

    for (int r = 2; r < rows; r++) {
        move_cursor(r, 1);
        printf(BOX_VERTICAL);
        move_cursor(r, cols);
        printf(BOX_VERTICAL);
    }

    move_cursor(rows, 1);
    printf(BOX_BOTTOM_LEFT);
    for (int c = 1; c < cols - 1; c++) {
        printf(BOX_HORIZONTAL);
    }
    printf(BOX_BOTTOM_RIGHT);

    printf(ANSI_RESET);
    fflush(stdout);
}


void screen_draw_progress_popup(const char *title, const char *item, double percent)
{
    /* isnan() first: NaN compares false against everything, so both
     * range checks below would silently pass it through to the (int)
     * cast further down - undefined behavior. */
    if (isnan(percent)) {
        percent = 0;
    }
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }

    int rows, cols;
    screen_get_size(&rows, &cols);
    (void)rows;

    int bar_width = 30;
    int max_bar = cols - 8;
    if (bar_width > max_bar) {
        bar_width = max_bar;
    }
    if (bar_width < 5) {
        bar_width = 5;
    }

    int filled = (int)(percent / 100.0 * bar_width + 0.5);

    /* Same bytes as the regular folder icon (see panel.c) - already known
     * to render correctly on this system, rather than a newly guessed
     * glyph. */
    static const char *const INDICATOR_ICON = "\xee\x97\xbf";

    char bar[256];
    int pos = 0;
    bar[pos++] = '[';
    if (g_fancy_style) {
        for (int i = 0; i < filled; i++) {
            memcpy(bar + pos, "\xe2\x96\x88", 3); /* █ */
            pos += 3;
        }
        if (filled < bar_width) {
            size_t icon_len = strlen(INDICATOR_ICON);
            memcpy(bar + pos, INDICATOR_ICON, icon_len);
            pos += (int)icon_len;
            for (int i = filled + 1; i < bar_width; i++) {
                memcpy(bar + pos, "\xe2\x96\x91", 3); /* ░ */
                pos += 3;
            }
        }
    } else {
        for (int i = 0; i < bar_width; i++) {
            bar[pos++] = (i < filled) ? '#' : '-';
        }
    }
    bar[pos++] = ']';
    bar[pos] = '\0';

    char percent_suffix[32];
    snprintf(percent_suffix, sizeof(percent_suffix), " %3d%%", (int)(percent + 0.5));

    char percent_line[320];
    snprintf(percent_line, sizeof(percent_line), "%s%s", bar, percent_suffix);

    /* Compute visible width by hand instead of strlen(): in fancy mode the
     * bar bytes are multibyte, so strlen() would overshoot and the box
     * would end up too wide. */
    int percent_line_visual_width = bar_width + 2 + (int)strlen(percent_suffix);

    int max_inner_width = cols - 6;
    if (max_inner_width < 10) {
        max_inner_width = 10;
    }

    int inner_width = utf8_visual_width(title);
    if (percent_line_visual_width > inner_width) {
        inner_width = percent_line_visual_width;
    }
    if (inner_width < 20) {
        inner_width = 20;
    }
    if (inner_width > max_inner_width) {
        inner_width = max_inner_width;
    }

    /* PATH_MAX, not a smaller fixed size - item can be a full path up to
     * PATH_MAX bytes; a smaller buffer would silently truncate it here,
     * before the visual-width clipping below even runs (which correctly
     * adds a "..." indicator, but only for its OWN clipping - a
     * truncation at this snprintf() would have already lost bytes with
     * no indicator at all). */
    char item_display[PATH_MAX + 4];
    snprintf(item_display, sizeof(item_display), "%s", item);
    if (utf8_visual_width(item_display) > inner_width && inner_width > 3) {
        /* Keep as many trailing visible columns as needed, snapping to the
         * nearest UTF-8 character boundary rather than a fixed byte offset
         * so a multibyte character isn't split at the cut point. */
        int keep = inner_width - 3;
        size_t cut = strlen(item_display);
        int col = 0;
        while (cut > 0 && col < keep) {
            cut--;
            while (cut > 0 && ((unsigned char)item_display[cut] & 0xC0) == 0x80) {
                cut--;
            }
            col++;
        }
        char truncated[PATH_MAX + 4];
        snprintf(truncated, sizeof(truncated), "...%s", item_display + cut);
        snprintf(item_display, sizeof(item_display), "%s", truncated);
    }

    int start_row = (rows - 5) / 2;
    if (start_row < 2) {
        start_row = 2;
    }
    int box_width = inner_width + 4;
    int start_col = (cols - box_width) / 2;
    if (start_col < 2) {
        start_col = 2;
    }

    draw_popup_border_edge(start_row, start_col, box_width, 1);

    move_cursor(start_row + 1, start_col);
    printf(POPUP_VERTICAL " ");
    print_utf8_padded(title, inner_width);
    printf(" " POPUP_VERTICAL);

    move_cursor(start_row + 2, start_col);
    printf(POPUP_VERTICAL " ");
    print_utf8_padded(item_display, inner_width);
    printf(" " POPUP_VERTICAL);

    move_cursor(start_row + 3, start_col);
    printf(POPUP_VERTICAL " %s", border_color_lookup(g_progress_bar_color));
    print_utf8_padded(percent_line, inner_width);
    printf(ANSI_RESET " " POPUP_VERTICAL);

    draw_popup_border_edge(start_row + 4, start_col, box_width, 0);

    fflush(stdout);
}

/* A button in a choice popup. The shortcut is always the label's first
 * letter (e.g. "Cancel" -> C); Skip/Retry/Abort/Overwrite/Yes/No all have
 * distinct initials. */
typedef struct {
    const char *label;
} DialogButton;

static int button_row_width(const DialogButton *buttons, int count)
{
    int width = 0;
    for (int i = 0; i < count; i++) {
        if (i > 0) {
            width += 1;
        }
        width += (int)strlen(buttons[i].label) + 2; /* "[" + label + "]" */
    }
    return width;
}

/* Draws the buttons centered within exactly inner_width visible columns.
 * The active button is inverted (terminal highlight); its shortcut letter
 * is always bold+underlined - both via SGR codes only, no fixed colors. */
static void draw_button_row(int inner_width, const DialogButton *buttons, int count, int active_index)
{
    int content_width = button_row_width(buttons, count);
    int pad_left = (inner_width - content_width) / 2;
    if (pad_left < 0) {
        pad_left = 0;
    }
    int pad_right = inner_width - content_width - pad_left;
    if (pad_right < 0) {
        pad_right = 0;
    }

    for (int i = 0; i < pad_left; i++) {
        putchar(' ');
    }

    for (int i = 0; i < count; i++) {
        if (i > 0) {
            putchar(' ');
        }
        int active = (i == active_index);
        if (active) {
            printf("\x1b[7m");
        }
        putchar('[');
        printf("\x1b[1;4m%c" ANSI_RESET, buttons[i].label[0]);
        if (active) {
            printf("\x1b[7m");
        }
        printf("%s", buttons[i].label + 1);
        putchar(']');
        if (active) {
            printf(ANSI_RESET);
        }
    }

    for (int i = 0; i < pad_right; i++) {
        putchar(' ');
    }
}

/* Shows a popup with title, message, and a row of buttons. Left/right
 * arrows move the highlight, Enter confirms, a button's initial letter
 * selects it directly. Esc returns -1 (caller decides what that means).
 * Redraws fully on every keypress, which makes it resize-safe for free
 * (SIGWINCH while the dialog is open). */
static int screen_prompt_buttons(const char *title, const char *message, const DialogButton *buttons,
                                  int count, int default_index)
{
    int selected = (default_index >= 0 && default_index < count) ? default_index : 0;

    /* Geometry is recomputed every iteration and only actually changes on
     * a real resize, but nothing clears the old box first - track it so
     * it can be blanked before the next draw. */
    int prev_start_row = -1, prev_start_col = -1, prev_box_width = 0;

    for (;;) {
        if (prev_start_row >= 0) {
            clear_rect(prev_start_row, prev_start_col, 5, prev_box_width);
        }

        int rows, cols;
        screen_get_size(&rows, &cols);

        int buttons_width = button_row_width(buttons, count);

        int max_content = utf8_visual_width(title);
        if (utf8_visual_width(message) > max_content) {
            max_content = utf8_visual_width(message);
        }
        if (buttons_width > max_content) {
            max_content = buttons_width;
        }

        int max_inner_width = cols - 6;
        if (max_inner_width < 10) {
            max_inner_width = 10;
        }
        int inner_width = max_content;
        if (inner_width > max_inner_width) {
            inner_width = max_inner_width;
        }

        int box_width = inner_width + 4;
        int box_height = 5;

        int start_row = (rows - box_height) / 2;
        if (start_row < 2) {
            start_row = 2;
        }
        int start_col = (cols - box_width) / 2;
        if (start_col < 2) {
            start_col = 2;
        }

        prev_start_row = start_row;
        prev_start_col = start_col;
        prev_box_width = box_width;

        draw_popup_border_edge(start_row, start_col, box_width, 1);

        move_cursor(start_row + 1, start_col);
        printf(POPUP_VERTICAL " ");
        print_utf8_padded(title, inner_width);
        printf(" " POPUP_VERTICAL);

        move_cursor(start_row + 2, start_col);
        printf(POPUP_VERTICAL " ");
        print_utf8_padded(message, inner_width);
        printf(" " POPUP_VERTICAL);

        move_cursor(start_row + 3, start_col);
        printf(POPUP_VERTICAL " ");
        draw_button_row(inner_width, buttons, count, selected);
        printf(" " POPUP_VERTICAL);

        draw_popup_border_edge(start_row + 4, start_col, box_width, 0);

        fflush(stdout);

        KeyEvent key = input_read_key();

        if (input_consume_resize_flag()) {
            continue;
        }

        if (key.type == KEY_LEFT) {
            selected = (selected - 1 + count) % count;
        } else if (key.type == KEY_RIGHT) {
            selected = (selected + 1) % count;
        } else if (key.type == KEY_ESC) {
            return -1;
        } else if (key.type == KEY_CHAR) {
            if (key.ch == '\r' || key.ch == '\n') {
                return selected;
            }
            char c = (char)toupper((unsigned char)key.ch);
            for (int i = 0; i < count; i++) {
                if (toupper((unsigned char)buttons[i].label[0]) == c) {
                    return i;
                }
            }
        }
    }
}

ScreenChoice screen_prompt_choice(const char *title, const char *message)
{
    static const DialogButton buttons[] = {{"Skip"}, {"Retry"}, {"Abort"}};
    int idx = screen_prompt_buttons(title, message, buttons, 3, 0);
    if (idx < 0) {
        idx = 2; /* Esc -> Abort */
    }
    if (idx == 0) {
        return SCREEN_CHOICE_SKIP;
    }
    if (idx == 1) {
        return SCREEN_CHOICE_RETRY;
    }
    return SCREEN_CHOICE_ABORT;
}

ScreenChoice screen_prompt_overwrite(const char *path)
{
    static const DialogButton buttons[] = {{"Skip"}, {"Overwrite"}, {"Abort"}};
    int idx = screen_prompt_buttons("Already exists", path, buttons, 3, 0);
    if (idx < 0) {
        idx = 2; /* Esc -> Abort */
    }
    if (idx == 0) {
        return SCREEN_CHOICE_SKIP;
    }
    if (idx == 1) {
        return SCREEN_CHOICE_OVERWRITE;
    }
    return SCREEN_CHOICE_ABORT;
}

int screen_prompt_text(const char *title, char *buffer, size_t buffer_size)
{
    /* Sized to match the caller's own buffer instead of a fixed 256:
     * previously any caller passing buffer_size > 256 (none currently do
     * - entry names are capped at 255 bytes - but the contract allows
     * it) would have a longer prefill silently truncated by this
     * function's own internal buffer, independent of and smaller than
     * what the caller actually asked for. */
    size_t edit_capacity = buffer_size > 0 ? buffer_size : 1;
    char *edited = malloc(edit_capacity);
    if (edited == NULL) {
        return 0;
    }
    snprintf(edited, edit_capacity, "%s", buffer);
    size_t len = strlen(edited);

    const char *hint = "Enter=OK  Esc=Cancel";

    /* Box size depends on the current text length, so it shrinks on
     * Backspace - track the last-drawn footprint so it can be blanked
     * before the next draw, otherwise the old wider border lingers. */
    int prev_start_row = -1, prev_start_col = -1, prev_box_width = 0;
    int result = 0;

    for (;;) {
        if (prev_start_row >= 0) {
            clear_rect(prev_start_row, prev_start_col, 5, prev_box_width);
        }

        const char *lines[3] = {title, edited, hint};
        int start_row, start_col, inner_width;
        draw_popup_frame(lines, &start_row, &start_col, &inner_width);

        prev_start_row = start_row;
        prev_start_col = start_col;
        prev_box_width = inner_width + 4;

        /* Position the cursor at the end of the edited text using visible
         * columns, not bytes, so umlauts in the name don't throw it off. */
        move_cursor(start_row + 2, start_col + 2 + utf8_visual_width(edited));
        printf("\x1b[?25h");
        fflush(stdout);

        KeyEvent key = input_read_key();

        if (input_consume_resize_flag()) {
            /* The popup is redrawn every loop iteration anyway - just
             * consume the flag here, otherwise it would leak to the
             * caller and main.c's main loop would wrongly treat the next
             * real keypress as "just a resize". */
            continue;
        }

        if (key.type == KEY_ESC) {
            break;
        }
        if (key.type == KEY_CHAR) {
            if (key.ch == '\r' || key.ch == '\n') {
                snprintf(buffer, buffer_size, "%s", edited);
                result = 1;
                break;
            }
            if (key.ch == 127 || key.ch == 8) {
                if (len > 0) {
                    /* Step back one UTF-8 codepoint, not one byte - see
                     * the matching fix in main.c's command line. */
                    len -= utf8_prev_char_len(edited, len);
                    edited[len] = '\0';
                }
            } else if ((unsigned char)key.ch >= 32 && key.ch != 127) {
                /* key.ch is a signed char - UTF-8 continuation bytes of an
                 * umlaut or other multibyte character (>=0x80) are negative
                 * as a signed char, but are still valid bytes to append one
                 * at a time (each arrives as its own KEY_CHAR from
                 * input_read_key()). */
                if (len < edit_capacity - 1) {
                    edited[len++] = key.ch;
                    edited[len] = '\0';
                }
            }
        }
    }

    free(edited);
    return result;
}

int screen_prompt_confirm(const char *title, const char *message)
{
    static const DialogButton buttons[] = {{"Yes"}, {"No"}};
    /* Default to "No" - for a (potentially destructive) confirmation,
     * pressing Enter without navigating should be the safe choice. */
    int idx = screen_prompt_buttons(title, message, buttons, 2, 1);
    if (idx < 0) {
        idx = 1; /* Esc -> No */
    }
    return idx == 0;
}
