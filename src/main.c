#define _DEFAULT_SOURCE

#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "editor.h"
#include "fileops.h"
#include "input.h"
#include "opener.h"
#include "panel.h"
#include "screen.h"
#include "shell.h"
#include "splash.h"
#include "tfm_common.h"

#define CMD_BUFFER_SIZE 256
#define CMD_PROMPT "$ "

/* Shared size for the short one-line messages built in this file
 * (synthetic "cd <name>" commands, popup/confirm text). */
#define TUI_MSG_BUFFER_SIZE 300

typedef enum {
    FOCUS_LEFT,
    FOCUS_RIGHT
} Focus;

typedef struct {
    int panel_top;
    int panel_height;
    int left_col;
    int left_width;
    int right_col;
    int right_width;
} Layout;

/* Everything the key handlers below read or mutate, bundled so each one
 * takes a single App * instead of the half-dozen main()-locals the old
 * monolithic key loop shared implicitly. */
typedef struct {
    Config cfg;
    Panel left;
    Panel right;
    Focus focus;
    char cmd_buffer[CMD_BUFFER_SIZE];
    size_t cmd_len;
    /* left_path == "actual" means the left panel starts at tfm's launch
     * directory; the flag is kept so saving config doesn't overwrite
     * "actual" with a concrete path after a cd. */
    int left_path_is_actual;
} App;

static void compute_layout(Layout *layout)
{
    int rows, cols;
    screen_get_size(&rows, &cols);

    layout->panel_top = 3;
    int panel_bottom = rows - 3;
    layout->panel_height = panel_bottom - layout->panel_top + 1;
    if (layout->panel_height < 1) {
        layout->panel_height = 1;
    }

    int inner_width = cols - 2;
    layout->left_col = 2;
    layout->left_width = inner_width / 2;
    layout->right_col = layout->left_col + layout->left_width;
    layout->right_width = inner_width - layout->left_width;
}

static int panel_visible_rows(const Layout *layout)
{
    int visible = layout->panel_height - PANEL_CHROME_ROWS;
    return visible < 0 ? 0 : visible;
}


static void redraw_ui(const App *app)
{
    const Config *cfg = &app->cfg;

    Layout layout;
    compute_layout(&layout);

    PanelTheme theme;
    theme.border_color = cfg->panel_border_color;
    theme.text_color = cfg->text_color;
    theme.cursor_color = cfg->cursor_color;
    theme.dir_color = cfg->dir_color;
    theme.icons_enabled = strcasecmp(cfg->icons, "omarchy") == 0;

    static const FunctionKey FUNCTION_KEYS[] = {
        {"F3", "Edit"}, {"F5", "Copy"}, {"F6", "Move"}, {"F7", "Mkdir"},
        {"F8", "Delete"}, {"F9", "Undo"}, {"F10", "Quit"},
    };

    screen_clear();
    screen_draw_border(cfg->border_color, "TFM - Taiku File Manager");
    screen_draw_menu_bar("Menu");
    screen_draw_function_bar(FUNCTION_KEYS, sizeof(FUNCTION_KEYS) / sizeof(FUNCTION_KEYS[0]),
                              cfg->panel_border_color);

    panel_draw(&app->left, layout.panel_top, layout.left_col, layout.left_width, layout.panel_height, &theme,
               app->focus == FOCUS_LEFT);
    panel_draw(&app->right, layout.panel_top, layout.right_col, layout.right_width, layout.panel_height, &theme,
               app->focus == FOCUS_RIGHT);

    screen_draw_command_line(CMD_PROMPT, app->cmd_buffer);
}

/* Shows an info/error popup and waits for a key, redrawing on every
 * resize so the popup never stays at its old size/position. */
static void tui_show_popup(const char *title, const char *message)
{
    screen_hide_cursor();
    for (;;) {
        screen_draw_popup(title, message);

        KeyEvent key = input_read_key();
        if (key.type == KEY_EOF) {
            break;
        }
        if (input_consume_resize_flag()) {
            continue;
        }
        if (key.type == KEY_NONE) {
            continue;
        }
        break;
    }
    screen_show_cursor();
}


static Panel *app_active_panel(App *app)
{
    return (app->focus == FOCUS_RIGHT) ? &app->right : &app->left;
}

/* The F5-F8 handlers all need both the active and the other panel. */
static Panel *app_other_panel(App *app)
{
    return (app->focus == FOCUS_RIGHT) ? &app->left : &app->right;
}

/* Returns the selected entry, or NULL when there is nothing a file
 * operation may act on: an empty panel, or the ".." pseudo-entry. */
static const DirEntryInfo *selected_file_entry(const Panel *panel)
{
    if (panel->count == 0) {
        return NULL;
    }
    const DirEntryInfo *entry = &panel->entries[panel->selected_index];
    return strcmp(entry->name, "..") == 0 ? NULL : entry;
}

/* Reloads target, and also twin when both show the same directory - a
 * change made in target is then visible in twin too. */
static void reload_panel_and_twin(Panel *target, Panel *twin)
{
    panel_reload(target);
    if (strcmp(target->path, twin->path) == 0) {
        panel_reload(twin);
    }
}

/* Records panel's new path in cfg after a successful cd, so it's restored
 * on the next start. cfg.left_path/right_path and Panel.path are both
 * char[PATH_MAX], so this can never truncate - but GCC's
 * -Wformat-truncation (at -O2 with _FORTIFY_SOURCE=2) can't see that
 * through the struct-pointer access; unsized() (tfm_common.h) hides the
 * array size from that analysis. */
static void app_remember_panel_path(App *app, const Panel *panel)
{
    if (panel == &app->left) {
        if (!app->left_path_is_actual) {
            snprintf(app->cfg.left_path, sizeof(app->cfg.left_path), "%s", unsized(panel->path));
        }
    } else {
        snprintf(app->cfg.right_path, sizeof(app->cfg.right_path), "%s", unsized(panel->path));
    }
}

/* cd can't run as an external process since a child can't change its
 * parent's working directory, so it must be handled as a builtin. */
static int is_cd_command(const char *command)
{
    return strncmp(command, "cd", 2) == 0 &&
           (command[2] == '\0' || command[2] == ' ');
}

static int enter_selected_entry(Panel *panel, char *error_msg, size_t error_msg_size)
{
    if (panel->count == 0) {
        return 0;
    }

    const DirEntryInfo *entry = &panel->entries[panel->selected_index];

    /* ".." must always work regardless of is_dir, since some filesystems
     * don't reliably report entry type. */
    int is_parent = strcmp(entry->name, "..") == 0;

    if (!entry->is_dir && !is_parent) {
        return 0;
    }

    char synthetic_cmd[TUI_MSG_BUFFER_SIZE];
    snprintf(synthetic_cmd, sizeof(synthetic_cmd), "cd %s", entry->name);

    return panel_change_dir(panel, synthetic_cmd, error_msg, error_msg_size);
}

/* fileops.c is UI-agnostic (see fileops.h) and calls these callbacks for
 * errors/conflicts/progress instead, which bridge to the screen.c TUI. */
static FileOpChoice tui_fileop_on_error(void *ctx, const char *title, const char *message)
{
    (void)ctx;
    switch (screen_prompt_choice(title, message)) {
        case SCREEN_CHOICE_SKIP:
            return FILEOPS_CHOICE_SKIP;
        case SCREEN_CHOICE_RETRY:
            return FILEOPS_CHOICE_RETRY;
        default:
            return FILEOPS_CHOICE_ABORT;
    }
}

static FileOpChoice tui_fileop_on_overwrite(void *ctx, const char *path)
{
    (void)ctx;
    switch (screen_prompt_overwrite(path)) {
        case SCREEN_CHOICE_SKIP:
            return FILEOPS_CHOICE_SKIP;
        case SCREEN_CHOICE_OVERWRITE:
            return FILEOPS_CHOICE_OVERWRITE;
        default:
            return FILEOPS_CHOICE_ABORT;
    }
}

static void tui_fileop_on_progress(void *ctx, const char *title, const char *item, double percent)
{
    (void)ctx;
    screen_draw_progress_popup(title, item, percent);
}

static const FileOpCallbacks tui_fileop_callbacks = {
    .on_error = tui_fileop_on_error,
    .on_overwrite = tui_fileop_on_overwrite,
    .on_progress = tui_fileop_on_progress,
    .ctx = NULL,
};

/* Set by the terminating-signal handler and checked by the main loop, so
 * shutdown goes through the normal exit path (atexit handlers and
 * config_save()) - neither is async-signal-safe to call from the handler
 * itself. No SA_RESTART, so the blocking read() in input_read_key() is
 * interrupted (EINTR) and the loop sees the flag promptly. */
static volatile sig_atomic_t g_shutdown_requested = 0;

static void handle_terminating_signal(int signum)
{
    (void)signum;
    g_shutdown_requested = 1;
}

static void install_terminating_signal_handlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_terminating_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    /* Failures go to stderr, which is still the visible terminal here
     * (this runs before the alt screen and raw mode are entered). They
     * are practically impossible, but would otherwise silently disable
     * the graceful-shutdown path. */
    if (sigaction(SIGTERM, &action, NULL) != 0 || sigaction(SIGHUP, &action, NULL) != 0 ||
        sigaction(SIGQUIT, &action, NULL) != 0) {
        fprintf(stderr, "tfm: warning: failed to install a signal handler: %s\n", strerror(errno));
    }
    /* SIGINT: with ISIG cleared in raw mode (input.c), Ctrl-C arrives as
     * ordinary KEY_CHAR data, but `kill -INT <pid>` still sends a real
     * SIGINT, which would otherwise skip all terminal cleanup. */
    if (sigaction(SIGINT, &action, NULL) != 0) {
        fprintf(stderr, "tfm: warning: failed to install SIGINT handler: %s\n", strerror(errno));
    }
    /* SIGPIPE: default action is to kill the process; writing to a
     * closed pipe (e.g. stdout piped into a reader that exits early)
     * would otherwise terminate tfm with no cleanup at all. */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        fprintf(stderr, "tfm: warning: failed to ignore SIGPIPE: %s\n", strerror(errno));
    }
}

/* F5: copy the selected entry into the other panel's directory. */
static void handle_copy(App *app)
{
    Panel *active_panel = app_active_panel(app);
    Panel *other_panel = app_other_panel(app);
    const DirEntryInfo *entry = selected_file_entry(active_panel);

    if (entry != NULL) {
        char src_path[PATH_MAX];
        if (!path_join(src_path, sizeof(src_path), active_panel->path, entry->name)) {
            tui_show_popup("Error", "Path too long");
        } else {
            screen_hide_cursor();
            fileops_copy(src_path, other_panel->path, &tui_fileop_callbacks);
            screen_show_cursor();
            /* Discard a resize that happened during the copy; otherwise
             * the next real keypress in the main loop would be misread as
             * "just a resize" (see input_consume_resize_flag()). */
            input_consume_resize_flag();

            /* Only the target panel changed; the source panel keeps its
             * selection unless it shows the same directory. */
            reload_panel_and_twin(other_panel, active_panel);
        }
    }

    redraw_ui(app);
}

/* F6 with both panels in the same directory: moving there is meaningless,
 * so rename in place instead. */
static void rename_selected_in_place(Panel *active_panel, Panel *other_panel, const DirEntryInfo *entry)
{
    char new_name[256];
    snprintf(new_name, sizeof(new_name), "%s", entry->name);

    if (!screen_prompt_text("Rename", new_name, sizeof(new_name)) || new_name[0] == '\0' ||
        strcmp(new_name, entry->name) == 0) {
        return;
    }

    char old_path[PATH_MAX];
    char new_path[PATH_MAX];
    struct stat existing_st;
    if (!is_safe_path_component(new_name)) {
        /* A typed name containing '/' (e.g. "../../important") would
         * otherwise rename the file OUTSIDE the current directory via the
         * bare rename() below, with none of fileops_move()'s safety
         * checks. */
        tui_show_popup("Error", "Name cannot contain '/' or be '.'/'..'");
        return;
    }
    if (!path_join(old_path, sizeof(old_path), active_panel->path, entry->name) ||
        !path_join(new_path, sizeof(new_path), active_panel->path, new_name)) {
        tui_show_popup("Error", "Path too long");
        return;
    }
    /* A declined overwrite is a silent no-op, matching Skip/Abort
     * elsewhere. */
    if (lstat(new_path, &existing_st) == 0 && screen_prompt_overwrite(new_path) != SCREEN_CHOICE_OVERWRITE) {
        return;
    }

    if (rename(old_path, new_path) != 0) {
        tui_show_popup("Error", strerror(errno));
    } else {
        panel_reload(active_panel);
        panel_reload(other_panel);
    }
}

/* F6: move the selected entry into the other panel's directory. */
static void handle_move(App *app)
{
    Panel *active_panel = app_active_panel(app);
    Panel *other_panel = app_other_panel(app);
    const DirEntryInfo *entry = selected_file_entry(active_panel);

    if (entry != NULL) {
        if (strcmp(active_panel->path, other_panel->path) == 0) {
            rename_selected_in_place(active_panel, other_panel, entry);
        } else {
            char src_path[PATH_MAX];
            if (!path_join(src_path, sizeof(src_path), active_panel->path, entry->name)) {
                tui_show_popup("Error", "Path too long");
            } else {
                screen_hide_cursor();
                fileops_move(src_path, other_panel->path, &tui_fileop_callbacks);
                screen_show_cursor();
                /* See comment in handle_copy(). */
                input_consume_resize_flag();

                /* Unlike copy, the source panel also changes (entry
                 * disappears). */
                panel_reload(active_panel);
                panel_reload(other_panel);
            }
        }
    }

    redraw_ui(app);
}

/* F7: create a new directory in the active panel. */
static void handle_mkdir(App *app)
{
    Panel *active_panel = app_active_panel(app);
    Panel *other_panel = app_other_panel(app);

    char new_dir_name[256] = "";
    if (screen_prompt_text("New folder", new_dir_name, sizeof(new_dir_name)) && new_dir_name[0] != '\0') {
        char new_dir_path[PATH_MAX];
        if (!is_safe_path_component(new_dir_name)) {
            /* A name like "existingsub/newname" would otherwise create the
             * directory INSIDE an existing subdirectory instead of in the
             * current directory, as "New folder" implies. */
            tui_show_popup("Error", "Name cannot contain '/' or be '.'/'..'");
        } else if (!path_join(new_dir_path, sizeof(new_dir_path), active_panel->path, new_dir_name)) {
            tui_show_popup("Error", "Path too long");
        } else if (mkdir(new_dir_path, 0755) != 0) {
            tui_show_popup("Error", strerror(errno));
        } else {
            reload_panel_and_twin(active_panel, other_panel);
        }
    }

    redraw_ui(app);
}

/* F8: move the selected entry to the trash, or delete it for good with
 * Shift+F8 - e.g. for a large file not worth doubling disk usage for, or
 * sensitive data that shouldn't linger in ~/.local/share/Trash. */
static void handle_delete(App *app, int permanent)
{
    Panel *active_panel = app_active_panel(app);
    Panel *other_panel = app_other_panel(app);
    const DirEntryInfo *entry = selected_file_entry(active_panel);

    if (entry != NULL) {
        char confirm_msg[TUI_MSG_BUFFER_SIZE];
        snprintf(confirm_msg, sizeof(confirm_msg), "%s%s%s?", permanent ? "Permanently delete " : "Delete ",
                 entry->name, entry->is_dir ? "/" : "");

        screen_hide_cursor();
        if (screen_prompt_confirm(permanent ? "Permanently delete" : "Delete", confirm_msg)) {
            char target_path[PATH_MAX];
            if (!path_join(target_path, sizeof(target_path), active_panel->path, entry->name)) {
                tui_show_popup("Error", "Path too long");
            } else {
                if (permanent) {
                    fileops_delete(target_path, &tui_fileop_callbacks);
                } else {
                    fileops_trash(target_path, &tui_fileop_callbacks);
                }
                /* See comment in handle_copy(). */
                input_consume_resize_flag();

                reload_panel_and_twin(active_panel, other_panel);
            }
        }
        screen_show_cursor();
    }

    redraw_ui(app);
}

/* F9: restores the single most-recently-trashed item (see
 * fileops_restore_last_trashed()) - not a general undo of copy/move, and
 * not a trash browser. Reloads both panels since the restored item's
 * original directory may be either one, or neither. */
static void handle_undo(App *app)
{
    char restored_path[PATH_MAX] = "";
    if (fileops_restore_last_trashed(&tui_fileop_callbacks, restored_path, sizeof(restored_path))) {
        /* Sized for a full PATH_MAX restored_path, not
         * TUI_MSG_BUFFER_SIZE. */
        char msg[PATH_MAX + 32];
        snprintf(msg, sizeof(msg), "Restored: %s", restored_path);
        tui_show_popup("Undo", msg);
    }
    input_consume_resize_flag();
    panel_reload(&app->left);
    panel_reload(&app->right);
    redraw_ui(app);
}

/* Up/Down/Home/End/PgUp/PgDn. Home/End pass +-count, which always reaches
 * the first/last entry - panel_move_selection() clamps, so overshooting is
 * harmless. */
static void handle_navigation(App *app, KeyType key_type)
{
    Panel *active_panel = app_active_panel(app);

    Layout layout;
    compute_layout(&layout);
    int visible_rows = panel_visible_rows(&layout);

    int delta = 0;
    switch (key_type) {
        case KEY_UP:   delta = -1; break;
        case KEY_DOWN: delta = 1; break;
        case KEY_HOME: delta = -(int)active_panel->count; break;
        case KEY_END:  delta = (int)active_panel->count; break;
        case KEY_PGUP: delta = -visible_rows; break;
        case KEY_PGDN: delta = visible_rows; break;
        default:       return;
    }

    panel_move_selection(active_panel, delta, visible_rows);
    redraw_ui(app);
}

/* F3: open the selected file in $EDITOR, whatever its type. */
static void handle_edit(App *app)
{
    Panel *active_panel = app_active_panel(app);
    const DirEntryInfo *entry = selected_file_entry(active_panel);
    if (entry == NULL || entry->is_dir) {
        return;
    }

    char edit_error[TUI_MSG_BUFFER_SIZE] = "";
    char file_path[PATH_MAX];
    if (!path_join(file_path, sizeof(file_path), active_panel->path, entry->name)) {
        snprintf(edit_error, sizeof(edit_error), "Path too long");
    } else {
        input_disable_raw_mode();
        int exit_code = editor_open(file_path);
        input_enable_raw_mode();
        /* See comment in handle_copy(): input_read_key() wasn't being
         * called while the editor had control. */
        input_consume_resize_flag();

        panel_reload(&app->left);
        panel_reload(&app->right);

        if (exit_code != 0) {
            snprintf(edit_error, sizeof(edit_error), "Editor exited with code %d", exit_code);
        }
    }
    redraw_ui(app);

    if (edit_error[0] != '\0') {
        tui_show_popup("Error", edit_error);
        redraw_ui(app);
    }
}

/* Keeps a program's output on screen until the user has read it - the
 * full redraw afterwards would otherwise wipe it at once. A resize just
 * keeps waiting; EOF or a shutdown signal ends the wait. */
static void wait_for_key_after_program(void)
{
    printf("\n[Press any key to return to tfm]");
    fflush(stdout);
    while (!g_shutdown_requested) {
        KeyEvent key = input_read_key();
        if (key.type != KEY_NONE) {
            return;
        }
    }
}

/* Enter on an executable: confirm (default No, so a stray Enter never
 * runs anything), then run it in the panel's directory like a typed
 * command. */
static void run_selected_program(App *app, const DirEntryInfo *entry)
{
    Panel *active_panel = app_active_panel(app);

    char confirm_msg[TUI_MSG_BUFFER_SIZE];
    snprintf(confirm_msg, sizeof(confirm_msg), "Run %s?", entry->name);
    screen_hide_cursor();
    int confirmed = screen_prompt_confirm("Run", confirm_msg);
    screen_show_cursor();

    char run_error[TUI_MSG_BUFFER_SIZE] = "";
    if (confirmed) {
        /* "./" so the shell runs this file, not a same-named command
         * found on $PATH. entry->name is at most 255 bytes, and quoting
         * at most quadruples it. */
        char quoted[256 * 4 + 3];
        char command[sizeof(quoted) + 2];
        if (!shell_quote(quoted, sizeof(quoted), entry->name)) {
            snprintf(run_error, sizeof(run_error), "Name too long");
        } else {
            snprintf(command, sizeof(command), "./%s", quoted);
            /* Start the program's output on a blank screen, not on top
             * of the panels. */
            screen_clear();
            fflush(stdout);
            input_disable_raw_mode();
            int exit_code = shell_execute(command, active_panel->path);
            input_enable_raw_mode();
            wait_for_key_after_program();
            /* See comment in handle_copy(). */
            input_consume_resize_flag();

            if (exit_code != 0) {
                snprintf(run_error, sizeof(run_error), "Exit code %d: %s", exit_code, entry->name);
            }
            /* After the message is built: the reload frees entry. */
            panel_reload(&app->left);
            panel_reload(&app->right);
        }
    }
    redraw_ui(app);

    if (run_error[0] != '\0') {
        tui_show_popup("Error", run_error);
        redraw_ui(app);
    }
}

/* Enter with an empty command line: cd into a directory, run an
 * executable, or open any other file in its default application. */
static void open_selected_entry(App *app)
{
    Panel *active_panel = app_active_panel(app);
    const DirEntryInfo *entry = selected_file_entry(active_panel);
    char enter_error[TUI_MSG_BUFFER_SIZE] = "";

    if (entry == NULL || entry->is_dir) {
        /* Includes "..", which selected_file_entry() leaves out. */
        if (enter_selected_entry(active_panel, enter_error, sizeof(enter_error))) {
            app_remember_panel_path(app, active_panel);
            redraw_ui(app);
        }
    } else {
        char file_path[PATH_MAX];
        if (!path_join(file_path, sizeof(file_path), active_panel->path, entry->name)) {
            snprintf(enter_error, sizeof(enter_error), "Path too long");
        } else if (opener_is_executable(file_path)) {
            run_selected_program(app, entry);
        } else {
            opener_open_default(file_path, enter_error, sizeof(enter_error));
        }
    }

    if (enter_error[0] != '\0') {
        tui_show_popup("Error", enter_error);
        redraw_ui(app);
    }
}

/* Enter with a non-empty command line: run it as a builtin cd, or through
 * the shell in the active panel's directory. */
static void run_command_line(App *app)
{
    Panel *active_panel = app_active_panel(app);
    int show_error = 0;
    char error_message[CMD_BUFFER_SIZE + 32] = "";

    if (is_cd_command(app->cmd_buffer)) {
        if (!panel_change_dir(active_panel, app->cmd_buffer, error_message, sizeof(error_message))) {
            show_error = 1;
        } else {
            app_remember_panel_path(app, active_panel);
        }
    } else {
        /* Same blank screen and pause as run_selected_program(). */
        screen_clear();
        fflush(stdout);
        input_disable_raw_mode();
        int exit_code = shell_execute(app->cmd_buffer, active_panel->path);
        input_enable_raw_mode();
        wait_for_key_after_program();
        /* See comment in handle_copy(). */
        input_consume_resize_flag();

        panel_reload(&app->left);
        panel_reload(&app->right);

        if (exit_code != 0) {
            show_error = 1;
            snprintf(error_message, sizeof(error_message), "Exit code %d: %s", exit_code, app->cmd_buffer);
        }
    }

    app->cmd_len = 0;
    app->cmd_buffer[0] = '\0';

    redraw_ui(app);

    if (show_error) {
        tui_show_popup("Error", error_message);
        redraw_ui(app);
    }
}

static void handle_backspace(App *app)
{
    if (app->cmd_len > 0) {
        /* Step back one UTF-8 codepoint, not one byte - removing only the
         * last byte of a multi-byte character (e.g. an umlaut) would leave
         * a dangling continuation byte in the buffer. */
        app->cmd_len -= utf8_prev_char_len(app->cmd_buffer, app->cmd_len);
        app->cmd_buffer[app->cmd_len] = '\0';
        screen_draw_command_line(CMD_PROMPT, app->cmd_buffer);
    }
}

static void handle_command_char(App *app, char ch)
{
    if (app->cmd_len < CMD_BUFFER_SIZE - 1) {
        app->cmd_buffer[app->cmd_len++] = ch;
        app->cmd_buffer[app->cmd_len] = '\0';
        screen_draw_command_line(CMD_PROMPT, app->cmd_buffer);
    }
}

/* Routes one key to its handler. Returns 0 when tfm should quit. */
static int dispatch_key(App *app, KeyEvent key)
{
    switch (key.type) {
        case KEY_F10:
            return 0;
        case KEY_F3:
            handle_edit(app);
            break;
        case KEY_F5:
            handle_copy(app);
            break;
        case KEY_F6:
            handle_move(app);
            break;
        case KEY_F7:
            handle_mkdir(app);
            break;
        case KEY_F8:
            handle_delete(app, key.shift);
            break;
        case KEY_F9:
            handle_undo(app);
            break;
        case KEY_UP:
        case KEY_DOWN:
        case KEY_HOME:
        case KEY_END:
        case KEY_PGUP:
        case KEY_PGDN:
            handle_navigation(app, key.type);
            break;
        case KEY_CHAR:
            if (key.ch == '\t') {
                app->focus = (app->focus == FOCUS_LEFT) ? FOCUS_RIGHT : FOCUS_LEFT;
                redraw_ui(app);
            } else if (key.ch == '\r' || key.ch == '\n') {
                if (app->cmd_len == 0) {
                    open_selected_entry(app);
                } else {
                    run_command_line(app);
                }
            } else if (key.ch == 127 || key.ch == 8) {
                handle_backspace(app);
            } else if ((unsigned char)key.ch >= 32) {
                /* key.ch is a signed char; UTF-8 continuation bytes (>=0x80)
                 * are negative as char and would fail a naive "< 127" check
                 * despite being valid bytes to append one at a time. */
                handle_command_char(app, key.ch);
            }
            break;
        default:
            break;
    }
    return 1;
}

/* Report invalid color names in tfm.ini instead of silently falling
 * back, resetting each one to "system". */
static void report_invalid_colors(App *app)
{
    Config *cfg = &app->cfg;
    struct {
        const char *field_name;
        char *value;
    } color_fields[] = {
        {"border_color", cfg->border_color},
        {"panel_border_color", cfg->panel_border_color},
        {"text_color", cfg->text_color},
        {"cursor_color", cfg->cursor_color},
        {"dir_color", cfg->dir_color},
    };
    char invalid_msg[256] = "";
    int any_invalid = 0;

    for (size_t i = 0; i < sizeof(color_fields) / sizeof(color_fields[0]); i++) {
        if (!screen_color_name_is_valid(color_fields[i].value)) {
            if (any_invalid) {
                strncat(invalid_msg, ", ", sizeof(invalid_msg) - strlen(invalid_msg) - 1);
            }
            strncat(invalid_msg, color_fields[i].field_name, sizeof(invalid_msg) - strlen(invalid_msg) - 1);
            snprintf(color_fields[i].value, 32, "system");
            any_invalid = 1;
        }
    }

    if (any_invalid) {
        /* The progress-bar color was cached from the raw, unvalidated
         * border_color; refresh it in case that was one of the fields
         * just reset to "system". */
        screen_set_progress_bar_color(cfg->border_color);

        char full_msg[TUI_MSG_BUFFER_SIZE];
        snprintf(full_msg, sizeof(full_msg), "Unknown color in tfm.ini: %s (using system)", invalid_msg);
        tui_show_popup("Config warning", full_msg);
        redraw_ui(app);
    }
}

static void run_event_loop(App *app)
{
    for (;;) {
        /* Checked before the blocking read, not just after: if the signal
         * arrived before this loop was ever reached (e.g. during
         * splash/config load), there's no pending signal left to
         * interrupt input_read_key()'s read() - it would block until the
         * next keypress. */
        if (g_shutdown_requested) {
            return;
        }

        KeyEvent key = input_read_key();

        if (g_shutdown_requested) {
            return;
        }

        if (key.type == KEY_EOF) {
            /* Stdin permanently closed (e.g. `tfm < /dev/null`) - exit
             * instead of spinning forever re-reading a dead fd. */
            return;
        }

        if (input_consume_resize_flag()) {
            redraw_ui(app);
            /* A resize can be flagged in the same iteration a real key was
             * read, so only skip dispatch when there is genuinely no key
             * (KEY_NONE, e.g. the EINTR-aborted read that often
             * accompanies SIGWINCH itself). */
            if (key.type == KEY_NONE) {
                continue;
            }
        }

        if (!dispatch_key(app, key)) {
            return;
        }
    }
}

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("tfm %s\n", TFM_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: tfm [--version] [--help]\n"
                   "Dual-panel terminal file manager. Run with no arguments to start.\n");
            return 0;
        }
    }

    /* Without this, every locale-sensitive libc call (case folding,
     * collation order in dir.c's directory sort) runs in the default "C"
     * locale regardless of the user's actual environment. Failure (an
     * unset/invalid LC_* env var) is not fatal: the current locale
     * (normally "C") simply stays active. */
    setlocale(LC_ALL, "");

    install_terminating_signal_handlers();

    screen_enter_alt_screen();
    atexit(screen_leave_alt_screen);
    /* Registered before splash_show(): the splash hides the cursor for its
     * ~2.5s animation, so a SIGTERM/SIGHUP/SIGQUIT during that window
     * needs this handler already in place. */
    atexit(screen_show_cursor);

    splash_show("TFM", "Taiku File Manager");

    /* static: App embeds Config and two Panels, several PATH_MAX buffers
     * each - no reason to put that on main()'s stack when there is only
     * ever one instance. */
    static App app;

    /* PATH_MAX + 128, not TUI_MSG_BUFFER_SIZE - config.c's error messages
     * embed a path that can legitimately be up to PATH_MAX bytes. */
    char config_load_error[PATH_MAX + 128] = "";
    config_load(&app.cfg, config_load_error, sizeof(config_load_error));

    screen_set_fancy_style(strcasecmp(app.cfg.icons, "omarchy") == 0);
    screen_set_progress_bar_color(app.cfg.border_color);

    app.left_path_is_actual = strcasecmp(app.cfg.left_path, "actual") == 0;

    char left_start_dir[PATH_MAX];
    if (app.left_path_is_actual) {
        if (getcwd(left_start_dir, sizeof(left_start_dir)) == NULL) {
            const char *home = getenv("HOME");
            snprintf(left_start_dir, sizeof(left_start_dir), "%s", home != NULL ? home : "/");
        }
    } else {
        snprintf(left_start_dir, sizeof(left_start_dir), "%s", app.cfg.left_path);
    }

    panel_init(&app.left, left_start_dir);
    panel_init(&app.right, app.cfg.right_path);
    app.focus = FOCUS_LEFT;
    app.cmd_buffer[0] = '\0';
    app.cmd_len = 0;

    input_enable_raw_mode();

    redraw_ui(&app);

    if (config_load_error[0] != '\0') {
        tui_show_popup("Config warning", config_load_error);
        redraw_ui(&app);
    }

    report_invalid_colors(&app);

    run_event_loop(&app);

    input_disable_raw_mode();

    panel_free(&app.left);
    panel_free(&app.right);

    /* No screen_clear() needed: leaving the alt screen buffer (atexit)
     * restores the previous terminal content. That also means there's no
     * TUI left to show a popup in, so a save failure goes to stderr,
     * visible in the shell tfm returns control to. */
    char config_save_error[PATH_MAX + 128] = ""; /* see config_load_error's comment above */
    config_save(&app.cfg, config_save_error, sizeof(config_save_error));
    if (config_save_error[0] != '\0') {
        fprintf(stderr, "tfm: failed to save config: %s\n", config_save_error);
    }

    return 0;
}
