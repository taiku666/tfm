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
#include "panel.h"
#include "screen.h"
#include "shell.h"
#include "splash.h"
#include "tfm_common.h"

#define CMD_BUFFER_SIZE 256
#define CMD_PROMPT "$ "

/* Shared size for the various short one-line messages built in this file
 * (synthetic "cd <name>" commands, popup/confirm text) - was previously
 * three separate literal 300s that had to be kept in sync by hand. */
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

static void redraw_ui(const Config *cfg, const Panel *left, const Panel *right, Focus focus,
                       const char *cmd_buffer)
{
    Layout layout;
    compute_layout(&layout);

    PanelTheme theme;
    theme.border_color = cfg->panel_border_color;
    theme.text_color = cfg->text_color;
    theme.cursor_color = cfg->cursor_color;
    theme.dir_color = cfg->dir_color;
    theme.icons_enabled = strcasecmp(cfg->icons, "omarchy") == 0;

    static const FunctionKey FUNCTION_KEYS[] = {
        {"F5", "Copy"}, {"F6", "Move"}, {"F7", "Mkdir"}, {"F8", "Delete"}, {"F9", "Undo"}, {"F10", "Quit"},
    };

    screen_clear();
    screen_draw_border(cfg->border_color, "TFM - Taiku File Manager");
    screen_draw_menu_bar("Menu");
    screen_draw_function_bar(FUNCTION_KEYS, sizeof(FUNCTION_KEYS) / sizeof(FUNCTION_KEYS[0]),
                              cfg->panel_border_color);

    panel_draw(left, layout.panel_top, layout.left_col, layout.left_width, layout.panel_height, &theme,
               focus == FOCUS_LEFT);
    panel_draw(right, layout.panel_top, layout.right_col, layout.right_width, layout.panel_height, &theme,
               focus == FOCUS_RIGHT);

    screen_draw_command_line(CMD_PROMPT, cmd_buffer);
}

/* Shared hide-draw-wait-show pattern for info/error popups, previously
 * duplicated at many call sites.
 *
 * Loops and redraws on resize instead of drawing once and calling
 * input_wait_any_key(): the latter silently consumes a SIGWINCH during
 * the wait without redrawing anything, so a resize while this popup is up
 * used to leave it on screen at its old, now wrong, size/position until
 * dismissed - unlike screen_prompt_buttons()/screen_prompt_text(), which
 * already redraw themselves on every resize. */
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

/* Returns the panel opposite to panel; the F5-F8 handlers all need both
 * the active and the other panel. */
static Panel *other_panel_of(Panel *panel, Panel *panel_left, Panel *panel_right)
{
    return (panel == panel_left) ? panel_right : panel_left;
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

    if (!builtin_cd(panel->path, synthetic_cmd, error_msg, error_msg_size)) {
        return 0;
    }

    panel_reload(panel);
    return 1;
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

/* Set by the terminating-signal handler below; checked at the top of the
 * main loop so shutdown goes through the normal exit path (atexit
 * handlers AND config_save()) instead of calling exit()/config_save()
 * directly from a signal handler, which isn't async-signal-safe
 * (config_save() calls fopen/fprintf/fclose). sig_atomic_t + volatile is
 * the one type POSIX guarantees is safe to write from a handler and read
 * from normal code. No SA_RESTART on these handlers, so the blocking
 * read() in input_read_key() is interrupted (EINTR) and control returns
 * to the main loop promptly instead of waiting for the next keypress. */
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
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGQUIT, &action, NULL);
    /* SIGINT: with ISIG cleared in raw mode (input.c), Ctrl-C arrives as
     * ordinary KEY_CHAR data, not this signal - but `kill -INT <pid>`
     * from outside the terminal still sends a real SIGINT, which
     * previously bypassed all cleanup (stuck raw mode/alt-screen/hidden
     * cursor). */
    sigaction(SIGINT, &action, NULL);
    /* SIGPIPE: default action is to kill the process; writing to a
     * closed pipe (e.g. stdout piped into a reader that exits early)
     * would otherwise terminate tfm with no cleanup at all. */
    signal(SIGPIPE, SIG_IGN);
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
     * locale regardless of the user's actual environment - e.g. accented
     * filenames never case-fold or collate the way the user's own
     * alphabet expects. Failure (an unset/invalid LC_* env var) is not
     * fatal: the C library simply keeps whatever locale was already
     * active (normally "C"), same as never having called this. */
    setlocale(LC_ALL, "");

    install_terminating_signal_handlers();

    screen_enter_alt_screen();
    atexit(screen_leave_alt_screen);
    /* Registered before splash_show() (not after, as before) - the
     * splash hides the cursor itself and only restores it at the end of
     * its ~2.5s animation, so a SIGTERM/SIGHUP/SIGQUIT during that
     * window needs this atexit handler already registered to avoid
     * exiting with the cursor left hidden. */
    atexit(screen_show_cursor);

    splash_show("TFM", "Taiku File Manager");

    Config cfg;
    config_load(&cfg);

    screen_set_fancy_style(strcasecmp(cfg.icons, "omarchy") == 0);
    screen_set_progress_bar_color(cfg.border_color);

    /* left_path == "actual" means the left panel starts at tfm's launch
     * directory; the flag is kept so saving config doesn't overwrite
     * "actual" with a concrete path after a cd. */
    int left_path_is_actual = strcasecmp(cfg.left_path, "actual") == 0;

    char left_start_dir[PATH_MAX];
    if (left_path_is_actual) {
        if (getcwd(left_start_dir, sizeof(left_start_dir)) == NULL) {
            const char *home = getenv("HOME");
            snprintf(left_start_dir, sizeof(left_start_dir), "%s", home != NULL ? home : "/");
        }
    } else {
        snprintf(left_start_dir, sizeof(left_start_dir), "%s", cfg.left_path);
    }

    Panel panel_left;
    Panel panel_right;
    panel_init(&panel_left, left_start_dir);
    panel_init(&panel_right, cfg.right_path);

    Focus focus = FOCUS_LEFT;

    char cmd_buffer[CMD_BUFFER_SIZE] = "";
    size_t cmd_len = 0;

    input_enable_raw_mode();

    redraw_ui(&cfg, &panel_left, &panel_right, focus, cmd_buffer);

    /* Report invalid color names in tfm.ini instead of silently falling
     * back. */
    {
        struct {
            const char *field_name;
            char *value;
        } color_fields[] = {
            {"border_color", cfg.border_color},
            {"panel_border_color", cfg.panel_border_color},
            {"text_color", cfg.text_color},
            {"cursor_color", cfg.cursor_color},
            {"dir_color", cfg.dir_color},
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
            /* screen_set_progress_bar_color() was already called above
             * with the raw, unvalidated cfg.border_color - if that field
             * was one of the ones just fixed up to "system", the cached
             * progress-bar color needs refreshing too, or every progress
             * popup for the rest of the session keeps using the invalid
             * (colorless) name despite this warning saying it was fixed. */
            screen_set_progress_bar_color(cfg.border_color);

            char full_msg[TUI_MSG_BUFFER_SIZE];
            snprintf(full_msg, sizeof(full_msg), "Unknown color in tfm.ini: %s (using system)", invalid_msg);
            tui_show_popup("Config warning", full_msg);
            redraw_ui(&cfg, &panel_left, &panel_right, focus, cmd_buffer);
        }
    }

    int running = 1;
    while (running) {
        /* Checked before the blocking read, not just after: if the
         * signal arrived before this loop was ever reached (e.g. during
         * splash/config load), there's no longer a pending signal left
         * to interrupt input_read_key()'s read() - it would otherwise
         * block indefinitely waiting for a keypress that never comes. */
        if (g_shutdown_requested) {
            break;
        }

        KeyEvent key = input_read_key();

        if (g_shutdown_requested) {
            break;
        }

        if (key.type == KEY_EOF) {
            /* Stdin permanently closed (e.g. `tfm < /dev/null`) - exit
             * through the normal loop-end path below instead of spinning
             * forever re-reading an fd that will never produce a key. */
            break;
        }

        if (input_consume_resize_flag()) {
            redraw_ui(&cfg, &panel_left, &panel_right, focus, cmd_buffer);
            /* A resize (SIGWINCH) can be flagged at the same time a real
             * key was already successfully read (input_read_key()'s
             * read() completing right before the signal, or the flag
             * being set by an earlier, still-unconsumed resize) - only
             * skip dispatch when there is genuinely no key to dispatch
             * (KEY_NONE, e.g. the EINTR-aborted read that often
             * accompanies the signal itself), instead of unconditionally
             * discarding whatever key was read this iteration. */
            if (key.type == KEY_NONE) {
                continue;
            }
        }

        Panel *active_panel = (focus == FOCUS_RIGHT) ? &panel_right : &panel_left;

        if (key.type == KEY_F10) {
            running = 0;
        } else if (key.type == KEY_F5) {
            Panel *other_panel = other_panel_of(active_panel, &panel_left, &panel_right);

            if (active_panel->count > 0) {
                const DirEntryInfo *entry = &active_panel->entries[active_panel->selected_index];
                if (strcmp(entry->name, "..") != 0) {
                    char src_path[PATH_MAX];
                    if (!path_join(src_path, sizeof(src_path), active_panel->path, entry->name)) {
                        tui_show_popup("Error", "Path too long");
                    } else {
                        screen_hide_cursor();
                        fileops_copy(src_path, other_panel->path, &tui_fileop_callbacks);
                        screen_show_cursor();
                        /* Discard a resize that happened during the copy;
                         * otherwise the next real keypress in the main
                         * loop would be misread as "just a resize" (see
                         * input_consume_resize_flag()). */
                        input_consume_resize_flag();

                        /* Only reload the target panel; the source panel's
                         * selection should stay put. If both panels show
                         * the same directory, the target is also the
                         * source, so reload it too. */
                        panel_reload(other_panel);
                        if (strcmp(active_panel->path, other_panel->path) == 0) {
                            panel_reload(active_panel);
                        }
                    }
                }
            }

            redraw_ui(&cfg, &panel_left, &panel_right, focus, cmd_buffer);
        } else if (key.type == KEY_F6) {
            Panel *other_panel = other_panel_of(active_panel, &panel_left, &panel_right);

            if (active_panel->count > 0) {
                const DirEntryInfo *entry = &active_panel->entries[active_panel->selected_index];
                if (strcmp(entry->name, "..") != 0) {
                    if (strcmp(active_panel->path, other_panel->path) == 0) {
                        /* Same directory in both panels: moving there is
                         * meaningless, so rename instead. */
                        char new_name[256];
                        snprintf(new_name, sizeof(new_name), "%s", entry->name);

                        if (screen_prompt_text("Rename", new_name, sizeof(new_name)) &&
                            new_name[0] != '\0' && strcmp(new_name, entry->name) != 0) {
                            char old_path[PATH_MAX];
                            char new_path[PATH_MAX];
                            struct stat existing_st;
                            int confirmed = 1;
                            if (!path_join(old_path, sizeof(old_path), active_panel->path, entry->name) ||
                                !path_join(new_path, sizeof(new_path), active_panel->path, new_name)) {
                                tui_show_popup("Error", "Path too long");
                                confirmed = 0;
                            } else if (lstat(new_path, &existing_st) == 0) {
                                if (screen_prompt_overwrite(new_path) != SCREEN_CHOICE_OVERWRITE) {
                                    confirmed = 0;
                                }
                            }

                            if (!confirmed) {
                                /* Path-too-long already reported above;
                                 * a declined overwrite is a silent
                                 * no-op, matching Skip/Abort elsewhere. */
                            } else if (rename(old_path, new_path) != 0) {
                                tui_show_popup("Error", strerror(errno));
                            } else {
                                /* Both panels show the same directory, so
                                 * both must be reloaded to see the new
                                 * name. */
                                panel_reload(active_panel);
                                panel_reload(other_panel);
                            }
                        }
                    } else {
                        char src_path[PATH_MAX];
                        if (!path_join(src_path, sizeof(src_path), active_panel->path, entry->name)) {
                            tui_show_popup("Error", "Path too long");
                        } else {
                            screen_hide_cursor();
                            fileops_move(src_path, other_panel->path, &tui_fileop_callbacks);
                            screen_show_cursor();
                            /* See comment at F5/fileops_copy(). */
                            input_consume_resize_flag();

                            /* Unlike copy, the source panel also changes
                             * (entry disappears). */
                            panel_reload(active_panel);
                            panel_reload(other_panel);
                        }
                    }
                }
            }

            redraw_ui(&cfg, &panel_left, &panel_right, focus, cmd_buffer);
        } else if (key.type == KEY_F7) {
            Panel *other_panel = other_panel_of(active_panel, &panel_left, &panel_right);

            char new_dir_name[256] = "";
            if (screen_prompt_text("New folder", new_dir_name, sizeof(new_dir_name)) &&
                new_dir_name[0] != '\0') {
                char new_dir_path[PATH_MAX];
                if (!path_join(new_dir_path, sizeof(new_dir_path), active_panel->path, new_dir_name)) {
                    tui_show_popup("Error", "Path too long");
                } else if (mkdir(new_dir_path, 0755) != 0) {
                    tui_show_popup("Error", strerror(errno));
                } else {
                    panel_reload(active_panel);
                    if (strcmp(active_panel->path, other_panel->path) == 0) {
                        panel_reload(other_panel);
                    }
                }
            }

            redraw_ui(&cfg, &panel_left, &panel_right, focus, cmd_buffer);
        } else if (key.type == KEY_F8) {
            Panel *other_panel = other_panel_of(active_panel, &panel_left, &panel_right);

            if (active_panel->count > 0) {
                const DirEntryInfo *entry = &active_panel->entries[active_panel->selected_index];
                if (strcmp(entry->name, "..") != 0) {
                    /* Shift+F8 bypasses the trash (below) for a real,
                     * permanent delete - e.g. for a large file not worth
                     * doubling disk usage for, or sensitive data that
                     * shouldn't linger in ~/.local/share/Trash. */
                    int permanent = key.shift;

                    char confirm_msg[TUI_MSG_BUFFER_SIZE];
                    snprintf(confirm_msg, sizeof(confirm_msg), "%s%s%s?",
                             permanent ? "Permanently delete " : "Delete ", entry->name,
                             entry->is_dir ? "/" : "");

                    screen_hide_cursor();
                    int confirmed = screen_prompt_confirm(permanent ? "Permanently delete" : "Delete",
                                                           confirm_msg);

                    if (confirmed) {
                        char target_path[PATH_MAX];
                        if (!path_join(target_path, sizeof(target_path), active_panel->path, entry->name)) {
                            tui_show_popup("Error", "Path too long");
                        } else {
                            if (permanent) {
                                fileops_delete(target_path, &tui_fileop_callbacks);
                            } else {
                                fileops_trash(target_path, &tui_fileop_callbacks);
                            }
                            /* See comment at F5/fileops_copy(). */
                            input_consume_resize_flag();

                            panel_reload(active_panel);
                            if (strcmp(active_panel->path, other_panel->path) == 0) {
                                panel_reload(other_panel);
                            }
                        }
                    }
                    screen_show_cursor();
                }
            }

            redraw_ui(&cfg, &panel_left, &panel_right, focus, cmd_buffer);
        } else if (key.type == KEY_F9) {
            /* Undo: restores the single most-recently-trashed item (see
             * fileops_restore_last_trashed()) - not a general undo of
             * copy/move, and not a trash browser; only ever the most
             * recent delete. Reloads both panels unconditionally since
             * the restored item's original directory may be either one,
             * or neither (if the user has since navigated away). */
            char restored_path[PATH_MAX] = "";
            if (fileops_restore_last_trashed(&tui_fileop_callbacks, restored_path,
                                              sizeof(restored_path))) {
                /* Sized for the full restored_path (PATH_MAX), not
                 * TUI_MSG_BUFFER_SIZE - that's for short, bounded
                 * messages elsewhere in this file; a restored path can
                 * legitimately be close to PATH_MAX itself. */
                char msg[PATH_MAX + 32];
                snprintf(msg, sizeof(msg), "Restored: %s", restored_path);
                tui_show_popup("Undo", msg);
            }
            input_consume_resize_flag();
            panel_reload(&panel_left);
            panel_reload(&panel_right);
            redraw_ui(&cfg, &panel_left, &panel_right, focus, cmd_buffer);
        } else if (key.type == KEY_UP) {
            Layout layout;
            compute_layout(&layout);
            panel_move_selection(active_panel, -1, panel_visible_rows(&layout));
            redraw_ui(&cfg, &panel_left, &panel_right, focus, cmd_buffer);
        } else if (key.type == KEY_DOWN) {
            Layout layout;
            compute_layout(&layout);
            panel_move_selection(active_panel, 1, panel_visible_rows(&layout));
            redraw_ui(&cfg, &panel_left, &panel_right, focus, cmd_buffer);
        } else if (key.type == KEY_CHAR && key.ch == '\t') {
            focus = (focus == FOCUS_LEFT) ? FOCUS_RIGHT : FOCUS_LEFT;
            redraw_ui(&cfg, &panel_left, &panel_right, focus, cmd_buffer);
        } else if (key.type == KEY_CHAR && (key.ch == '\r' || key.ch == '\n')) {
            if (cmd_len == 0) {
                int show_error = 0;
                char enter_error[128] = "";

                if (active_panel->count > 0 &&
                    !active_panel->entries[active_panel->selected_index].is_dir &&
                    strcmp(active_panel->entries[active_panel->selected_index].name, "..") != 0 &&
                    config_is_editor_extension(&cfg,
                                                active_panel->entries[active_panel->selected_index].name)) {
                    /* Extension listed in tfm.ini's [editor] extensions:
                     * open in $EDITOR. Other extensions are ignored. */
                    char file_path[PATH_MAX];
                    if (!path_join(file_path, sizeof(file_path), active_panel->path,
                                    active_panel->entries[active_panel->selected_index].name)) {
                        show_error = 1;
                        snprintf(enter_error, sizeof(enter_error), "Path too long");
                    } else {
                        input_disable_raw_mode();
                        int exit_code = editor_open(file_path);
                        input_enable_raw_mode();
                        /* See comment at F5/fileops_copy(): discard any
                         * resize that occurred while the editor had
                         * control (input_read_key() wasn't being called). */
                        input_consume_resize_flag();

                        panel_reload(&panel_left);
                        panel_reload(&panel_right);

                        if (exit_code != 0) {
                            show_error = 1;
                            snprintf(enter_error, sizeof(enter_error), "Editor exited with code %d",
                                     exit_code);
                        }
                    }
                    redraw_ui(&cfg, &panel_left, &panel_right, focus, cmd_buffer);
                } else if (enter_selected_entry(active_panel, enter_error, sizeof(enter_error))) {
                    if (active_panel == &panel_left) {
                        if (!left_path_is_actual) {
                            snprintf(cfg.left_path, sizeof(cfg.left_path), "%s", active_panel->path);
                        }
                    } else {
                        snprintf(cfg.right_path, sizeof(cfg.right_path), "%s", active_panel->path);
                    }
                    redraw_ui(&cfg, &panel_left, &panel_right, focus, cmd_buffer);
                } else if (enter_error[0] != '\0') {
                    show_error = 1;
                }

                if (show_error) {
                    tui_show_popup("Error", enter_error);
                    redraw_ui(&cfg, &panel_left, &panel_right, focus, cmd_buffer);
                }
            } else {
                int show_error = 0;
                char error_message[CMD_BUFFER_SIZE + 32] = "";

                if (is_cd_command(cmd_buffer)) {
                    if (!builtin_cd(active_panel->path, cmd_buffer, error_message, sizeof(error_message))) {
                        show_error = 1;
                    } else {
                        panel_reload(active_panel);
                        if (active_panel == &panel_left) {
                            if (!left_path_is_actual) {
                                snprintf(cfg.left_path, sizeof(cfg.left_path), "%s", active_panel->path);
                            }
                        } else {
                            snprintf(cfg.right_path, sizeof(cfg.right_path), "%s", active_panel->path);
                        }
                    }
                } else {
                    input_disable_raw_mode();
                    int exit_code = shell_execute(cmd_buffer, active_panel->path);
                    input_enable_raw_mode();
                    /* See comment at F5/fileops_copy(). */
                    input_consume_resize_flag();

                    panel_reload(&panel_left);
                    panel_reload(&panel_right);

                    if (exit_code != 0) {
                        show_error = 1;
                        snprintf(error_message, sizeof(error_message), "Exit code %d: %s", exit_code,
                                 cmd_buffer);
                    }
                }

                cmd_len = 0;
                cmd_buffer[0] = '\0';

                redraw_ui(&cfg, &panel_left, &panel_right, focus, cmd_buffer);

                if (show_error) {
                    tui_show_popup("Error", error_message);
                    redraw_ui(&cfg, &panel_left, &panel_right, focus, cmd_buffer);
                }
            }
        } else if (key.type == KEY_CHAR && (key.ch == 127 || key.ch == 8)) {
            if (cmd_len > 0) {
                /* Step back one UTF-8 codepoint, not one byte - removing
                 * only the last byte of a multi-byte character (e.g. an
                 * umlaut) would leave a dangling continuation byte in the
                 * buffer. */
                cmd_len -= utf8_prev_char_len(cmd_buffer, cmd_len);
                cmd_buffer[cmd_len] = '\0';
                screen_draw_command_line(CMD_PROMPT, cmd_buffer);
            }
        } else if (key.type == KEY_CHAR && (unsigned char)key.ch >= 32 && key.ch != 127) {
            /* key.ch is a signed char; UTF-8 continuation bytes (>=0x80)
             * are negative as char and would fail a naive "< 127" check
             * despite being valid bytes to append one at a time. Cast to
             * unsigned char, as in screen.c's rename/mkdir dialog. */
            if (cmd_len < CMD_BUFFER_SIZE - 1) {
                cmd_buffer[cmd_len++] = key.ch;
                cmd_buffer[cmd_len] = '\0';
                screen_draw_command_line(CMD_PROMPT, cmd_buffer);
            }
        }
    }

    input_disable_raw_mode();

    panel_free(&panel_left);
    panel_free(&panel_right);

    /* No screen_clear() needed here: leaving the alt screen buffer
     * (atexit) restores the previous terminal content automatically. */

    config_save(&cfg);

    return 0;
}
