/* GTK4/libadwaita GUI shell for tfm.
 *
 * Omarchy themes terminals/editors/browsers via
 * ~/.local/state/omarchy/current/theme/, but not GTK4/libadwaita apps -
 * libadwaita only knows a fixed palette of accent colors (AdwAccentColor)
 * and reads light/dark from GNOME gsettings, which need not match the
 * active Omarchy theme. This file bridges that gap by reading the active
 * Omarchy theme (see omarchy_theme.c) and forcing a matching color scheme
 * plus an accent color set via CSS. */

#define _DEFAULT_SOURCE

#include <adwaita.h>
#include <dirent.h>
#include <errno.h>
#include <glib-unix.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "dir.h"
#include "omarchy_theme.h"
#include "editor.h"
#include "fileops.h"
#include "opener.h"
#include "shell.h"
#include "tfm_common.h"

/* Fallback GTK monospace font size (points) used whenever the active
 * terminal's own configured size can't be determined. */
#define GUI_DEFAULT_FONT_SIZE 11.0

/* One entry in a panel's list (file/dir or ".." to go up). Wrapped as a
 * GObject so GtkListView/GListStore can manage it. */
#define TFM_TYPE_FILE_ITEM (tfm_file_item_get_type())
G_DECLARE_FINAL_TYPE(TfmFileItem, tfm_file_item, TFM, FILE_ITEM, GObject)

struct _TfmFileItem {
    GObject parent_instance;
    char *name;
    int is_dir;
};

G_DEFINE_TYPE(TfmFileItem, tfm_file_item, G_TYPE_OBJECT)

static void tfm_file_item_finalize(GObject *obj)
{
    TfmFileItem *self = TFM_FILE_ITEM(obj);
    g_free(self->name);
    G_OBJECT_CLASS(tfm_file_item_parent_class)->finalize(obj);
}

static void tfm_file_item_class_init(TfmFileItemClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = tfm_file_item_finalize;
}

static void tfm_file_item_init(TfmFileItem *self)
{
    (void)self;
}

static TfmFileItem *tfm_file_item_new(const char *name, int is_dir)
{
    TfmFileItem *item = g_object_new(TFM_TYPE_FILE_ITEM, NULL);
    item->name = g_strdup(name);
    item->is_dir = is_dir;
    return item;
}

/* A panel (left/right): current path + its list. */
typedef struct {
    char path[PATH_MAX];
    GListStore *store;
    GtkWidget *path_label;
    GtkWidget *list_view;
    GtkWidget *container; /* carries the "active panel" border, see focus_panel() */
} GuiPanel;

static GuiPanel g_panel[2];
static int g_focused_panel = 0;
static GtkWidget *g_shell_entry = NULL;
static GtkWindow *g_window = NULL;

/* Mirrors the TUI's app_other_panel() (src/main.c). */
static GuiPanel *other_panel_of_focused(void)
{
    return &g_panel[g_focused_panel == 0 ? 1 : 0];
}

/* Counts nested modal dialogs/progress popups (Rename, New Folder, error/
 * overwrite prompts, Copy/Move progress). on_window_key_pressed() is
 * installed as a GTK_PHASE_CAPTURE controller on the top-level window, so
 * it sees keypresses BEFORE any child widget - including an open AdwDialog,
 * which lives in the same window rather than its own GdkSurface. Without
 * this counter, keystrokes meant for a dialog's entry/buttons would get
 * intercepted (e.g. redirected into the shell line), and F5-F8/F10 could
 * re-trigger while an operation is already running. */
static int g_modal_depth = 0;

static void gui_modal_enter(void)
{
    g_modal_depth++;
}

static void gui_modal_leave(void)
{
    g_modal_depth--;
}

/* Switches keyboard focus between panels. Also marks the active panel
 * with a CSS border, since a GtkListView's row-selection color doesn't
 * visibly change on focus loss - without this there was no way to tell
 * which panel was active. */
static void focus_panel(int index)
{
    gtk_widget_remove_css_class(g_panel[g_focused_panel].container, "tfm-panel-active");
    g_focused_panel = index;
    gtk_widget_add_css_class(g_panel[index].container, "tfm-panel-active");
    gtk_widget_grab_focus(g_panel[index].list_view);
}

static void show_error_dialog(const char *title, const char *message);
static gboolean confirm_dialog(const char *title, const char *message, const char *confirm_label);

/* Holds an initial-load-failure message (see panel_load() below) until
 * the main window is presented. panel_load() first runs before
 * gtk_window_present(), and a blocking AdwAlertDialog parented to an
 * unmapped window never becomes visible, hanging the app in its nested
 * main loop. One slot per panel; flushed via g_idle_add() in activate(). */
static char g_pending_panel_load_error[2][PATH_MAX * 2 + 64];

/* Same "defer until the window is mapped" reasoning as
 * g_pending_panel_load_error above, for a config_load() failure at
 * startup - main() calls config_load() before activate() ever runs, so
 * there is no window at all yet to parent a blocking dialog to. */
static char g_pending_config_load_error[PATH_MAX + 64];

/* Loads path into panel. A failed reload keeps the old listing, like the
 * TUI's panel_reload(). A failed *initial* load (panel->path still empty)
 * falls back to $HOME, then "/", and queues an error in
 * g_pending_panel_load_error[panel_index]: an empty path would silently
 * turn every path_join(panel->path, name) into "/name" and be persisted
 * into tfm.ini on exit. Returns 1 if panel now shows path (or the
 * fallback), 0 if panel is unchanged. */
static int panel_load_indexed(GuiPanel *panel, const char *path, int panel_index)
{
    DirEntryInfo *entries = NULL;
    size_t count = 0;
    if (dir_list(path, &entries, &count) != 0) {
        if (panel->path[0] != '\0') {
            return 0;
        }

        int saved_errno = errno;
        const char *home = getenv("HOME");
        const char *fallback = (home != NULL && home[0] != '\0') ? home : "/";
        char *out = g_pending_panel_load_error[panel_index];
        size_t out_size = sizeof(g_pending_panel_load_error[panel_index]);

        if (strcmp(fallback, path) == 0 || dir_list(fallback, &entries, &count) != 0) {
            snprintf(out, out_size, "Could not open \"%s\": %s", path, strerror(saved_errno));
            return 0;
        }

        snprintf(out, out_size, "Could not open \"%s\": %s\nFalling back to \"%s\".", path,
                 strerror(saved_errno), fallback);
        path = fallback;
    }

    /* Remember the selected item's name before rebuilding the listing:
     * GtkSingleSelection forgets the item across a wholesale store
     * replacement, so a reload would otherwise throw the user back to the
     * top of a long listing. */
    char selected_name[256] = "";
    GtkSelectionModel *old_model = gtk_list_view_get_model(GTK_LIST_VIEW(panel->list_view));
    if (old_model != NULL) {
        TfmFileItem *cur =
            TFM_FILE_ITEM(gtk_single_selection_get_selected_item(GTK_SINGLE_SELECTION(old_model)));
        if (cur != NULL) {
            snprintf(selected_name, sizeof(selected_name), "%s", cur->name);
        }
    }

    g_list_store_remove_all(panel->store);
    guint restore_index = 0;
    gboolean restore_found = FALSE;
    for (size_t i = 0; i < count; i++) {
        TfmFileItem *item = tfm_file_item_new(entries[i].name, entries[i].is_dir);
        g_list_store_append(panel->store, item);
        g_object_unref(item);

        if (!restore_found && selected_name[0] != '\0' && strcmp(entries[i].name, selected_name) == 0) {
            restore_index = (guint)i;
            restore_found = TRUE;
        }
    }
    dir_list_free(entries);

    /* old_model is still valid: remove_all()/append() mutate the store in
     * place. Re-select by name, or the top entry if that item is gone. */
    if (old_model != NULL && count > 0) {
        guint index_to_select = restore_found ? restore_index : 0;
        gtk_single_selection_set_selected(GTK_SINGLE_SELECTION(old_model), index_to_select);
        /* Selecting an item outside the currently visible rows doesn't
         * by itself scroll it into view. */
        gtk_list_view_scroll_to(GTK_LIST_VIEW(panel->list_view), index_to_select, GTK_LIST_SCROLL_NONE, NULL);
    }

    /* Callers may pass panel->path itself as path (a plain reload), and
     * snprintf with overlapping source/dest is UB. */
    if (path != panel->path) {
        snprintf(panel->path, sizeof(panel->path), "%s", path);
    }
    if (panel->path_label != NULL) {
        gtk_label_set_label(GTK_LABEL(panel->path_label), panel->path);
    }
    return 1;
}

/* Every caller except build_panel_widget()'s initial load already has a
 * non-empty panel->path (a normal reload of an already-loaded panel), so
 * panel_load_indexed()'s initial-failure branch - the only place
 * panel_index is read - can never trigger here; the index value is
 * irrelevant. */
static int panel_load(GuiPanel *panel, const char *path)
{
    return panel_load_indexed(panel, path, 0);
}

static void panel_navigate_into(GuiPanel *panel, const char *name)
{
    char new_path[PATH_MAX];
    if (strcmp(name, "..") == 0) {
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s", panel->path);
        char *slash = strrchr(tmp, '/');
        if (slash == NULL) {
            return;
        }
        if (slash == tmp) {
            snprintf(new_path, sizeof(new_path), "/");
        } else {
            *slash = '\0';
            snprintf(new_path, sizeof(new_path), "%s", tmp);
        }
    } else if (panel->path[0] == '\0') {
        /* panel->path can only be empty in the extremely rare startup case
         * where even the $HOME/"/" fallback in panel_load_indexed() failed
         * (see its comment) - without this guard, "%s/%s" below would
         * silently build "/name" (filesystem root) instead of refusing the
         * navigation. */
        return;
    } else if (strcmp(panel->path, "/") == 0) {
        if ((size_t)snprintf(new_path, sizeof(new_path), "/%s", name) >= sizeof(new_path)) {
            return;
        }
    } else {
        if ((size_t)snprintf(new_path, sizeof(new_path), "%s/%s", panel->path, name) >=
            sizeof(new_path)) {
            return;
        }
    }

    /* Pre-check readability, mirroring builtin_cd()'s own opendir() probe:
     * panel_load() treats a failure here as a transient reload error and
     * silently keeps the old listing, so without this check a now-
     * unreadable directory would double-click into nothing. */
    DIR *dp = opendir(new_path);
    if (dp == NULL) {
        show_error_dialog("Error", strerror(errno));
        return;
    }
    closedir(dp);

    panel_load(panel, new_path);
}

/* Kept across theme applications and the SIGUSR1 reload handler so the
 * old CSS provider can be removed before reloading (otherwise providers
 * of equal priority would stack up). */
static GtkCssProvider *g_theme_css_provider = NULL;
static Config g_cfg;

/* Midpoint of the 0-255 luma range: above it, a black foreground reads
 * better on the accent color; at or below it, white does. */
#define LUMA_CONTRAST_THRESHOLD 150.0

/* Rough brightness estimate (Rec. 601 luma) to pick a readable
 * foreground (black/white) for an accent color - Omarchy themes only
 * supply the accent color itself, not a matching contrast color.
 * Accepts "#rrggbb" and the CSS "#rgb" shorthand a hand-edited
 * colors.toml may use. */
static const char *contrasting_fg_for(const char *hex_color)
{
    unsigned int r, g, b;
    size_t len = strlen(hex_color);
    if (len == 7 && hex_color[0] == '#') {
        if (sscanf(hex_color + 1, "%02x%02x%02x", &r, &g, &b) != 3) {
            return "#ffffff";
        }
    } else if (len == 4 && hex_color[0] == '#') {
        unsigned int r1, g1, b1;
        if (sscanf(hex_color + 1, "%1x%1x%1x", &r1, &g1, &b1) != 3) {
            return "#ffffff";
        }
        r = r1 * 16 + r1;
        g = g1 * 16 + g1;
        b = b1 * 16 + b1;
    } else {
        return "#ffffff";
    }
    double luma = 0.299 * r + 0.587 * g + 0.114 * b;
    return (luma > LUMA_CONTRAST_THRESHOLD) ? "#000000" : "#ffffff";
}

/* Loads the active Omarchy theme (if cfg->gui_theme == "omarchy") and
 * applies its color scheme + accent color. Falls back to default
 * libadwaita/system theming when gui_theme=="system" or no Omarchy theme
 * was found (e.g. outside Omarchy) - both cases just do nothing further. */
static void apply_omarchy_theme(void)
{
    /* Every reason not to apply (gui_theme=="system", no Omarchy theme,
     * no display) takes the same revert-to-system branch below, so a live
     * SIGUSR1 reload from omarchy to system actually reverts the color
     * scheme and removes the CSS provider. */
    int use_omarchy = strcasecmp(g_cfg.gui_theme, "system") != 0;
    GdkDisplay *display = gdk_display_get_default();
    OmarchyThemeColors colors;
    int can_apply = use_omarchy && display != NULL && omarchy_theme_load(&colors);

    if (!can_apply) {
        adw_style_manager_set_color_scheme(adw_style_manager_get_default(), ADW_COLOR_SCHEME_DEFAULT);
        if (display != NULL && g_theme_css_provider != NULL) {
            gtk_style_context_remove_provider_for_display(display,
                                                            GTK_STYLE_PROVIDER(g_theme_css_provider));
            g_object_unref(g_theme_css_provider);
            g_theme_css_provider = NULL;
        }
        return;
    }

    AdwStyleManager *style_manager = adw_style_manager_get_default();
    adw_style_manager_set_color_scheme(style_manager,
                                        colors.is_dark ? ADW_COLOR_SCHEME_FORCE_DARK
                                                        : ADW_COLOR_SCHEME_FORCE_LIGHT);

    /* colors.toml doesn't guarantee every key - without background/
     * foreground, fall back to the libadwaita default for the current
     * scheme instead of emitting an empty/invalid CSS color. */
    const char *background = colors.background[0] != '\0'
                                  ? colors.background
                                  : (colors.is_dark ? "#242424" : "#fafafa");
    const char *foreground = colors.foreground[0] != '\0'
                                  ? colors.foreground
                                  : (colors.is_dark ? "#ffffff" : "#000000");
    const char *header_bg = colors.dark_background[0] != '\0' ? colors.dark_background : background;
    const char *selection = colors.selection[0] != '\0' ? colors.selection : colors.accent;

    char css[2048];
    snprintf(css, sizeof(css),
             "@define-color accent_color %s;\n"
             "@define-color accent_bg_color %s;\n"
             "@define-color accent_fg_color %s;\n"
             "@define-color window_bg_color %s;\n"
             "@define-color window_fg_color %s;\n"
             "@define-color view_bg_color %s;\n"
             "@define-color view_fg_color %s;\n"
             "@define-color headerbar_bg_color %s;\n"
             "@define-color headerbar_fg_color %s;\n"
             "@define-color popover_bg_color %s;\n"
             "@define-color popover_fg_color %s;\n"
             "@define-color card_bg_color %s;\n"
             "@define-color card_fg_color %s;\n"
             "@define-color dialog_bg_color %s;\n"
             "@define-color dialog_fg_color %s;\n"
             "@define-color sidebar_bg_color %s;\n"
             "@define-color sidebar_fg_color %s;\n"
             "@define-color borders alpha(%s, 0.4);\n"
             "list row:selected, row:selected { background-color: %s; }\n",
             colors.accent, colors.accent, contrasting_fg_for(colors.accent), background, foreground,
             background, foreground, header_bg, foreground, header_bg, foreground, header_bg,
             foreground, background, foreground, header_bg, foreground, foreground, selection);

    /* Match the panel border's corner radius to Hyprland's window
     * rounding. A border rather than a filled background: a filled box
     * needs padding >= radius/sqrt(2) (~14px at radius 18) or the square
     * content corner clips the arc, while a border stays 2px at any
     * radius. */
    int corner_radius = omarchy_hypr_corner_rounding();
    size_t used = strlen(css);
    snprintf(css + used, sizeof(css) - used,
             ".tfm-panel-active { border: 2px solid @accent_color; border-radius: %dpx; "
             "padding: 3px; }\n",
             corner_radius > 0 ? corner_radius : 0);

    /* Reload the existing provider in place rather than recreating it on
     * every SIGUSR1: staying attached to the display avoids a flash of
     * unstyled content between remove and re-add. Created only the first
     * time, or after the revert branch above tore it down. */
    if (g_theme_css_provider == NULL) {
        g_theme_css_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(g_theme_css_provider),
                                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_string(g_theme_css_provider, css);
}

/* omarchy-theme-set sends SIGUSR1 to tfm-gui (via the hook in
 * contrib/omarchy-hooks/tfm-gui-reload-theme) on a theme change, so
 * running windows pick it up live without a restart. */
static gboolean on_sigusr1(gpointer user_data)
{
    (void)user_data;
    /* Reload tfm.ini, not just colors.toml - otherwise a mid-session
     * change to gui_theme (omarchy/system) would only take effect after
     * a restart. */
    char reload_error[PATH_MAX + 64] = "";
    config_load(&g_cfg, reload_error, sizeof(reload_error));
    /* The main window is already mapped by the time SIGUSR1 can arrive
     * (unlike the startup config_load() in main(), below) - safe to show
     * the dialog directly instead of deferring it. */
    if (reload_error[0] != '\0') {
        show_error_dialog("Config warning", reload_error);
    }
    apply_omarchy_theme();
    return G_SOURCE_CONTINUE;
}

static void on_factory_setup(GtkSignalListItemFactory *factory, GtkListItem *list_item,
                              gpointer user_data)
{
    (void)factory;
    (void)user_data;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *icon = gtk_image_new();
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_list_item_set_child(list_item, box);
}

static void on_factory_bind(GtkSignalListItemFactory *factory, GtkListItem *list_item,
                             gpointer user_data)
{
    (void)factory;
    (void)user_data;
    TfmFileItem *item = TFM_FILE_ITEM(gtk_list_item_get_item(list_item));
    GtkWidget *box = gtk_list_item_get_child(list_item);
    GtkWidget *icon = gtk_widget_get_first_child(box);
    GtkWidget *label = gtk_widget_get_next_sibling(icon);

    gtk_image_set_from_icon_name(GTK_IMAGE(icon),
                                  item->is_dir ? "folder-symbolic" : "text-x-generic-symbolic");
    gtk_label_set_label(GTK_LABEL(label), item->name);
}

/* Pumps pending GTK events while editor_open_cb() waits on the external
 * editor process - otherwise the window would appear frozen for the
 * whole editor session (same pattern used by gui_fileop_on_progress()
 * during Copy/Move). */
static void gui_pump_main_context(void *ctx)
{
    (void)ctx;
    while (g_main_context_pending(NULL)) {
        g_main_context_iteration(NULL, FALSE);
    }
}

/* F3: open the file at file_path in $EDITOR, whatever its type. */
static void edit_file(const char *file_path)
{
    /* Bracketed like every other pumped wait in this file - without it,
     * F5-F8/Tab or another activation during the editor session could
     * copy/move/delete the file out from under the still-open editor. */
    gui_modal_enter();
    int exit_code = editor_open_cb(file_path, gui_pump_main_context, NULL);
    gui_modal_leave();
    if (exit_code != 0) {
        char message[64];
        snprintf(message, sizeof(message), "Editor exited with code %d", exit_code);
        show_error_dialog("Error", message);
    }
}

/* Enter on an executable: confirm, then run it in the panel's directory
 * like a command typed into the shell entry. */
static void run_program(GuiPanel *panel, const char *name)
{
    char message[300];
    snprintf(message, sizeof(message), "Run \"%s\"?", name);
    if (!confirm_dialog("Run", message, "Run")) {
        return;
    }

    /* "./" so the shell runs this file, not a same-named command found
     * on $PATH. name is at most 255 bytes; quoting at most quadruples it. */
    char quoted[256 * 4 + 3];
    char command[sizeof(quoted) + 2];
    if (!shell_quote(quoted, sizeof(quoted), name)) {
        show_error_dialog("Error", "Name too long");
        return;
    }
    snprintf(command, sizeof(command), "./%s", quoted);

    gui_modal_enter();
    int exit_code = shell_execute_cb(command, panel->path, gui_pump_main_context, NULL);
    gui_modal_leave();
    if (exit_code != 0) {
        snprintf(message, sizeof(message), "Exit code %d: %s", exit_code, name);
        show_error_dialog("Error", message);
    }

    panel_load(&g_panel[0], g_panel[0].path);
    panel_load(&g_panel[1], g_panel[1].path);
}

/* Opens file_path in its default application. GIO directly rather than
 * the TUI's `gio open` subprocess: same mimeapps.list lookup, but the
 * launch context hands the new window a Wayland activation token so it
 * comes up focused. */
static void open_with_default_app(const char *file_path, const char *name)
{
    GFile *file = g_file_new_for_path(file_path);
    char *uri = g_file_get_uri(file);
    GdkAppLaunchContext *context = gdk_display_get_app_launch_context(gdk_display_get_default());
    GError *error = NULL;

    if (!g_app_info_launch_default_for_uri(uri, G_APP_LAUNCH_CONTEXT(context), &error)) {
        char message[512];
        snprintf(message, sizeof(message), "Could not open \"%s\": %s", name,
                 error != NULL ? error->message : "unknown error");
        show_error_dialog("Error", message);
        g_clear_error(&error);
    }

    g_object_unref(context);
    g_free(uri);
    g_object_unref(file);
}

/* Enter/double-click: cd into a directory, run an executable, or open
 * any other file in its default application. */
static void on_item_activated(GtkListView *list_view, guint position, gpointer user_data)
{
    if (g_modal_depth > 0) {
        /* Re-entrant activation (e.g. Enter fired again while an editor
         * session or another pumped operation is already in progress) -
         * every other pumping call site in this file is guarded the same
         * way. */
        return;
    }

    GuiPanel *panel = user_data;
    (void)list_view;
    TfmFileItem *item = g_list_model_get_item(G_LIST_MODEL(panel->store), position);
    if (item == NULL) {
        return;
    }
    if (item->is_dir) {
        panel_navigate_into(panel, item->name);
    } else {
        char file_path[PATH_MAX];
        if (!path_join(file_path, sizeof(file_path), panel->path, item->name)) {
            show_error_dialog("Error", "Path too long");
        } else if (opener_is_executable(file_path)) {
            run_program(panel, item->name);
        } else {
            open_with_default_app(file_path, item->name);
        }
    }
    g_object_unref(item);
}

/* Builds a panel widget (path label on top, scrollable file list below)
 * and initializes *panel accordingly. */
static GtkWidget *build_panel_widget(GuiPanel *panel, const char *initial_path)
{
    panel->store = g_list_store_new(TFM_TYPE_FILE_ITEM);
    panel->path_label = gtk_label_new(initial_path);
    gtk_label_set_xalign(GTK_LABEL(panel->path_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(panel->path_label), PANGO_ELLIPSIZE_START);
    gtk_widget_add_css_class(panel->path_label, "heading");
    gtk_widget_add_css_class(panel->path_label, "tfm-mono");
    gtk_widget_set_margin_start(panel->path_label, 8);
    gtk_widget_set_margin_end(panel->path_label, 8);
    gtk_widget_set_margin_top(panel->path_label, 8);
    gtk_widget_set_margin_bottom(panel->path_label, 4);

    GtkSingleSelection *selection = gtk_single_selection_new(G_LIST_MODEL(panel->store));

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(on_factory_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(on_factory_bind), NULL);

    panel->list_view = gtk_list_view_new(GTK_SELECTION_MODEL(selection), factory);
    gtk_widget_add_css_class(panel->list_view, "tfm-mono");
    g_signal_connect(panel->list_view, "activate", G_CALLBACK(on_item_activated), panel);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), panel->list_view);
    gtk_widget_set_vexpand(scrolled, TRUE);
    /* Explicit margins here (matching path_label's own margins above)
     * guarantee spacing from the rounded corner regardless of CSS
     * padding - relying on CSS padding alone left a visible gap only at
     * the bottom, never the top. */
    gtk_widget_set_margin_start(scrolled, 4);
    gtk_widget_set_margin_end(scrolled, 4);
    gtk_widget_set_margin_bottom(scrolled, 4);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), panel->path_label);
    gtk_box_append(GTK_BOX(box), scrolled);
    panel->container = box;

    /* panel is always one of the two slots in the global g_panel[] array
     * (see activate()'s two build_panel_widget() calls) - pointer
     * arithmetic recovers which one, so panel_load_indexed()'s
     * initial-failure path (see its doc comment) can record its message
     * into that panel's own slot. */
    panel_load_indexed(panel, initial_path, (int)(panel - g_panel));

    return box;
}

/* cd can't run as an external process (a child can't change its parent's
 * working directory), so it's handled as a builtin instead of being
 * passed to shell_execute(). */
static int gui_is_cd_command(const char *command)
{
    return strncmp(command, "cd", 2) == 0 && (command[2] == '\0' || command[2] == ' ');
}

/* Runs the entered command in the focused panel's directory and reloads
 * both panels afterward in case the command changed files. */
static void on_shell_entry_activate(GtkEntry *entry, gpointer user_data)
{
    (void)user_data;
    if (g_modal_depth > 0) {
        /* Enter fired again (e.g. key-repeat, or another widget forwarding
         * activate) while a previous command from this same entry is still
         * being pumped - without this, it would re-enter a nested nested
         * pumped wait and double the panel_load() calls below. */
        return;
    }

    /* gtk_editable_get_text() returns a pointer owned by the GtkEntry -
     * shell_execute_cb()/builtin_cd() below pump the main loop, during
     * which the user can keep typing in this same entry and invalidate
     * that pointer (use-after-free) or change what it points to (wrong
     * command/error text). Copy it before doing anything that pumps. */
    char *command = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));
    GuiPanel *active = &g_panel[g_focused_panel];

    if (gui_is_cd_command(command)) {
        /* Resolved into a scratch copy, not active->path, as in the TUI's
         * panel_change_dir(): builtin_cd() rewrites its buffer, and a
         * directory that then fails to list must not leave the new path
         * over the old listing (and get persisted on exit). panel_load()
         * only writes panel->path once the listing has loaded. */
        char new_path[PATH_MAX];
        char error_msg[128];
        snprintf(new_path, sizeof(new_path), "%s", active->path);
        if (!builtin_cd(new_path, command, error_msg, sizeof(error_msg))) {
            show_error_dialog("Error", error_msg);
        } else if (!panel_load(active, new_path)) {
            snprintf(error_msg, sizeof(error_msg), "Cannot read directory: %s", strerror(errno));
            show_error_dialog("Error", error_msg);
        }
        /* The unconditional reload of both panels below re-lists `active`
         * a second time after a successful cd - one redundant listing, but
         * the alternative (loading the new path only there) can't report a
         * failure, so it's the cost of never showing a path that wasn't
         * actually entered. */
    } else {
        /* Pump the main context while waiting (shell_execute_cb() with
         * gui_pump_main_context(), like editor launches): a plain blocking
         * waitpid() would freeze the single-threaded window for the whole
         * command. The modal bracket blocks F5-F8/F10/Tab meanwhile, so
         * e.g. F8 can't delete files out from under a running command. */
        gui_modal_enter();
        int exit_code = shell_execute_cb(command, active->path, gui_pump_main_context, NULL);
        gui_modal_leave();
        if (exit_code != 0) {
            char message[300];
            int written = snprintf(message, sizeof(message), "Exit code %d: %s", exit_code, command);
            if (written < 0 || (size_t)written >= sizeof(message)) {
                /* command is user-typed and unbounded - make a silent
                 * truncation visible instead of just cutting it off
                 * mid-word with no indication anything was cut. */
                snprintf(message + sizeof(message) - 4, 4, "...");
            }
            show_error_dialog("Error", message);
        }
    }

    g_free(command);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");

    panel_load(&g_panel[0], g_panel[0].path);
    panel_load(&g_panel[1], g_panel[1].path);
    focus_panel(g_focused_panel);
}

static GtkWidget *build_shell_bar(void)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 6);

    GtkWidget *prompt = gtk_label_new("$");
    gtk_widget_add_css_class(prompt, "tfm-mono");

    GtkWidget *entry = gtk_entry_new();
    gtk_widget_add_css_class(entry, "tfm-mono");
    gtk_widget_set_hexpand(entry, TRUE);
    g_signal_connect(entry, "activate", G_CALLBACK(on_shell_entry_activate), NULL);
    g_shell_entry = entry;

    gtk_box_append(GTK_BOX(bar), prompt);
    gtk_box_append(GTK_BOX(bar), entry);

    return bar;
}

/* Bottom function-key bar: F3 Edit, F5 Copy, F6 Move, F7 Mkdir, F8
 * Delete, F9 Undo, F10 Quit. */
typedef struct {
    const char *key;
    const char *label;
} FunctionKeyDef;

static const FunctionKeyDef FUNCTION_KEYS[] = {
    {"F3", "Edit"}, {"F5", "Copy"}, {"F6", "Move"}, {"F7", "Mkdir"},
    {"F8", "Delete"}, {"F9", "Undo"}, {"F10", "Quit"},
};

/* Returns the panel's currently selected item (transfer none per
 * gtk_single_selection_get_selected_item() - do not unref). */
static TfmFileItem *panel_get_selected_item(GuiPanel *panel)
{
    GtkSelectionModel *model = gtk_list_view_get_model(GTK_LIST_VIEW(panel->list_view));
    if (!GTK_IS_SINGLE_SELECTION(model)) {
        return NULL;
    }
    return TFM_FILE_ITEM(gtk_single_selection_get_selected_item(GTK_SINGLE_SELECTION(model)));
}

/* AdwAlertDialog only offers an async choose()/choose_finish() API, but
 * fileops.c expects synchronous return values from its callbacks (see
 * fileops.h). A nested GMainLoop makes the async dialog API usable
 * synchronously without having to rework fileops.c itself. */
typedef struct {
    GMainLoop *loop;
    char *response;
} AlertWait;

static void on_alert_choose_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    AlertWait *wait = user_data;
    const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
    wait->response = response != NULL ? g_strdup(response) : NULL;
    g_main_loop_quit(wait->loop);
}

/* Shows dialog modally and blocks until the user responds. Returns the
 * chosen response ID (caller frees with g_free()), or NULL on error. */
static char *run_alert_dialog_blocking(AdwDialog *dialog)
{
    AlertWait wait = {g_main_loop_new(NULL, FALSE), NULL};
    gui_modal_enter();
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(g_window), NULL,
                             on_alert_choose_done, &wait);
    g_main_loop_run(wait.loop);
    gui_modal_leave();
    g_main_loop_unref(wait.loop);
    return wait.response;
}

/* One response option of an alert dialog. */
typedef struct {
    const char *id;
    const char *label;
    AdwResponseAppearance appearance; /* ADW_RESPONSE_DEFAULT = no emphasis */
} DialogResponse;

/* Builds/shows an AdwAlertDialog with arbitrary responses and blocks
 * until answered - shared base for show_error_dialog/confirm_dialog/
 * gui_fileop_on_error/gui_fileop_on_overwrite. message is never
 * interpreted as Pango markup, since it may contain filenames or shell
 * commands with &/</> in them. Returns the chosen response ID (caller
 * frees with g_free()), or NULL on error. */
static char *show_alert_dialog(const char *title, const char *message,
                                const DialogResponse *responses, size_t response_count,
                                const char *default_response, const char *close_response)
{
    AdwDialog *dialog = adw_alert_dialog_new(title, message);
    adw_alert_dialog_set_body_use_markup(ADW_ALERT_DIALOG(dialog), FALSE);
    for (size_t i = 0; i < response_count; i++) {
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), responses[i].id, responses[i].label);
        if (responses[i].appearance != ADW_RESPONSE_DEFAULT) {
            adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), responses[i].id,
                                                      responses[i].appearance);
        }
    }
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), default_response);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), close_response);
    return run_alert_dialog_blocking(dialog);
}

static void show_error_dialog(const char *title, const char *message)
{
    static const DialogResponse responses[] = {
        {"ok", "OK", ADW_RESPONSE_SUGGESTED},
    };
    g_free(show_alert_dialog(title, message, responses, G_N_ELEMENTS(responses), "ok", "ok"));
}

/* Yes/no confirmation (F8 Delete/Permanently delete) - confirm_label is
 * the destructive response's own label (distinguishing a regular,
 * recoverable trash from a permanent delete matters enough to say so on
 * the button itself, not just the dialog title), "Cancel" is the
 * default/Escape response. */
static gboolean confirm_dialog(const char *title, const char *message, const char *confirm_label)
{
    DialogResponse responses[] = {
        {"cancel", "Cancel", ADW_RESPONSE_DEFAULT},
        {"confirm", confirm_label, ADW_RESPONSE_DESTRUCTIVE},
    };
    char *response =
        show_alert_dialog(title, message, responses, G_N_ELEMENTS(responses), "cancel", "cancel");
    gboolean confirmed = response != NULL && strcmp(response, "confirm") == 0;
    g_free(response);
    return confirmed;
}

/* Text input dialog (F6 Rename, F7 New folder). Returns the entered text
 * (caller frees with g_free()), or NULL on cancel.
 *
 * A hand-built AdwDialog rather than AdwAlertDialog: an embedded
 * GtkEntry's Enter can't cleanly complete AdwAlertDialog's async choose()
 * (it falls through to the close response, Cancel). Here Enter in the
 * entry triggers the same handler as the OK button. */
typedef struct {
    GMainLoop *loop;
    AdwDialog *dialog;
    gboolean confirmed;
} PromptDialogState;

static void prompt_dialog_respond(PromptDialogState *state, gboolean confirmed)
{
    state->confirmed = confirmed;
    /* can-close is FALSE (see prompt_text_dialog()), so allow the close
     * through first - otherwise adw_dialog_close() below would just
     * re-emit close-attempt and recurse forever when called from that
     * handler. */
    adw_dialog_set_can_close(state->dialog, TRUE);
    adw_dialog_close(state->dialog);
    g_main_loop_quit(state->loop);
}

static void on_prompt_ok(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    prompt_dialog_respond(user_data, TRUE);
}

static void on_prompt_cancel(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    prompt_dialog_respond(user_data, FALSE);
}

static char *prompt_text_dialog(const char *heading, const char *initial_text)
{
    PromptDialogState state = {g_main_loop_new(NULL, FALSE), NULL, FALSE};

    AdwDialog *dialog = adw_dialog_new();
    state.dialog = dialog;
    adw_dialog_set_content_width(dialog, 360);
    /* Without can-close=FALSE, Escape closes AdwDialog via its own path
     * independent of on_prompt_ok/on_prompt_cancel, so g_main_loop_quit()
     * below is never reached and the call hangs forever (gui_progress_show()
     * sets can-close=FALSE for the same reason). Treat close-attempt as
     * Cancel. */
    adw_dialog_set_can_close(dialog, FALSE);
    g_signal_connect(dialog, "close-attempt", G_CALLBACK(on_prompt_cancel), &state);

    GtkWidget *header = adw_header_bar_new();
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), adw_window_title_new(heading, NULL));

    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    GtkWidget *ok_btn = gtk_button_new_with_label("OK");
    gtk_widget_add_css_class(ok_btn, "suggested-action");
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_prompt_cancel), &state);
    g_signal_connect(ok_btn, "clicked", G_CALLBACK(on_prompt_ok), &state);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel_btn);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), ok_btn);

    GtkWidget *entry = gtk_entry_new();
    gtk_widget_add_css_class(entry, "tfm-mono");
    gtk_editable_set_text(GTK_EDITABLE(entry), initial_text != NULL ? initial_text : "");
    gtk_widget_set_margin_start(entry, 12);
    gtk_widget_set_margin_end(entry, 12);
    gtk_widget_set_margin_top(entry, 12);
    gtk_widget_set_margin_bottom(entry, 12);
    g_signal_connect(entry, "activate", G_CALLBACK(on_prompt_ok), &state);
    g_object_ref(entry); /* keep alive past the dialog's close/destroy below */

    GtkWidget *toolbar_view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), entry);
    adw_dialog_set_child(dialog, toolbar_view);

    adw_dialog_present(dialog, GTK_WIDGET(g_window));
    gtk_widget_grab_focus(entry);

    gui_modal_enter();
    g_main_loop_run(state.loop);
    gui_modal_leave();
    g_main_loop_unref(state.loop);

    char *result = state.confirmed ? g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry))) : NULL;
    g_object_unref(entry);
    return result;
}

/* fileops.c is UI-independent (see fileops.h) and calls these callbacks
 * on errors/conflicts - the GUI counterpart to tui_fileop_callbacks.
 *
 * Default responses (Enter / close) - none may be destructive:
 *   - error dialog (this function): Enter -> Retry, close -> Abort.
 *   - overwrite dialog (gui_fileop_on_overwrite): Enter -> Skip,
 *     close -> Abort.
 *   - delete confirmation (action_delete): close -> Cancel. */
static FileOpChoice gui_fileop_on_error(void *ctx, const char *title, const char *message)
{
    (void)ctx;
    static const DialogResponse responses[] = {
        {"skip", "Skip", ADW_RESPONSE_DEFAULT},
        {"retry", "Retry", ADW_RESPONSE_SUGGESTED},
        {"abort", "Abort", ADW_RESPONSE_DESTRUCTIVE},
    };
    char *response =
        show_alert_dialog(title, message, responses, G_N_ELEMENTS(responses), "retry", "abort");
    FileOpChoice choice = FILEOPS_CHOICE_ABORT;
    if (response != NULL) {
        if (strcmp(response, "skip") == 0) {
            choice = FILEOPS_CHOICE_SKIP;
        } else if (strcmp(response, "retry") == 0) {
            choice = FILEOPS_CHOICE_RETRY;
        }
        g_free(response);
    }
    return choice;
}

static FileOpChoice gui_fileop_on_overwrite(void *ctx, const char *path)
{
    (void)ctx;
    char message[PATH_MAX + 32];
    snprintf(message, sizeof(message), "%s already exists.", path);

    static const DialogResponse responses[] = {
        {"skip", "Skip", ADW_RESPONSE_DEFAULT},
        {"overwrite", "Overwrite", ADW_RESPONSE_DESTRUCTIVE},
        {"abort", "Abort", ADW_RESPONSE_DEFAULT},
    };
    char *response =
        show_alert_dialog("File exists", message, responses, G_N_ELEMENTS(responses), "skip", "abort");
    FileOpChoice choice = FILEOPS_CHOICE_ABORT;
    if (response != NULL) {
        if (strcmp(response, "skip") == 0) {
            choice = FILEOPS_CHOICE_SKIP;
        } else if (strcmp(response, "overwrite") == 0) {
            choice = FILEOPS_CHOICE_OVERWRITE;
        }
        g_free(response);
    }
    return choice;
}

/* Progress display (F5 Copy/F6 Move). fileops_copy()/fileops_move() run
 * blocking on the GTK main thread - without pumping pending events in
 * between, the display would appear frozen for the whole operation. */
typedef struct {
    AdwDialog *dialog;
    GtkWidget *title_label;
    GtkWidget *item_label;
    GtkWidget *progress_bar;
} GuiProgressState;

static GuiProgressState g_progress = {NULL, NULL, NULL, NULL};

static void gui_progress_show(void)
{
    AdwDialog *dialog = adw_dialog_new();
    adw_dialog_set_content_width(dialog, 380);
    adw_dialog_set_can_close(dialog, FALSE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);

    GtkWidget *title_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(title_label), 0.0f);
    gtk_widget_add_css_class(title_label, "heading");

    GtkWidget *item_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(item_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(item_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(item_label, "tfm-mono");

    GtkWidget *progress_bar = gtk_progress_bar_new();

    gtk_box_append(GTK_BOX(box), title_label);
    gtk_box_append(GTK_BOX(box), item_label);
    gtk_box_append(GTK_BOX(box), progress_bar);
    adw_dialog_set_child(dialog, box);
    adw_dialog_present(dialog, GTK_WIDGET(g_window));

    g_progress.dialog = dialog;
    g_progress.title_label = title_label;
    g_progress.item_label = item_label;
    g_progress.progress_bar = progress_bar;
}

static void gui_progress_hide(void)
{
    if (g_progress.dialog != NULL) {
        adw_dialog_force_close(g_progress.dialog);
        g_progress.dialog = NULL;
    }
}

static void gui_fileop_on_progress(void *ctx, const char *title, const char *item, double percent)
{
    (void)ctx;
    if (g_progress.dialog == NULL) {
        gui_progress_show();
    }
    gtk_label_set_label(GTK_LABEL(g_progress.title_label), title);
    gtk_label_set_label(GTK_LABEL(g_progress.item_label), item != NULL ? item : "");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_progress.progress_bar), percent / 100.0);

    /* Called from fileops' own blocking loop - pump pending events here
     * or GTK won't redraw the updated display until the whole operation
     * finishes. */
    while (g_main_context_pending(NULL)) {
        g_main_context_iteration(NULL, FALSE);
    }
}

static const FileOpCallbacks gui_fileop_callbacks = {
    .on_error = gui_fileop_on_error,
    .on_overwrite = gui_fileop_on_overwrite,
    .on_progress = gui_fileop_on_progress,
    .ctx = NULL,
};

static void action_edit(void)
{
    GuiPanel *active = &g_panel[g_focused_panel];
    TfmFileItem *item = panel_get_selected_item(active);
    if (item == NULL || item->is_dir) {
        return;
    }
    char file_path[PATH_MAX];
    if (!path_join(file_path, sizeof(file_path), active->path, item->name)) {
        show_error_dialog("Error", "Path too long");
        return;
    }
    edit_file(file_path);
    panel_load(&g_panel[0], g_panel[0].path);
    panel_load(&g_panel[1], g_panel[1].path);
}

static void action_copy(void)
{
    GuiPanel *active = &g_panel[g_focused_panel];
    GuiPanel *other = other_panel_of_focused();
    TfmFileItem *item = panel_get_selected_item(active);
    if (item == NULL || strcmp(item->name, "..") == 0) {
        return;
    }
    char src_path[PATH_MAX];
    if (!path_join(src_path, sizeof(src_path), active->path, item->name)) {
        show_error_dialog("Error", "Path too long");
        return;
    }
    gui_modal_enter();
    fileops_copy(src_path, other->path, &gui_fileop_callbacks);
    gui_progress_hide();
    gui_modal_leave();
    panel_load(other, other->path);
    if (strcmp(active->path, other->path) == 0) {
        panel_load(active, active->path);
    }
}

static void action_move(void)
{
    GuiPanel *active = &g_panel[g_focused_panel];
    GuiPanel *other = other_panel_of_focused();
    TfmFileItem *item = panel_get_selected_item(active);
    if (item == NULL || strcmp(item->name, "..") == 0) {
        return;
    }

    if (strcmp(active->path, other->path) == 0) {
        /* Same directory in both panels: moving makes no sense, rename instead. */
        char *new_name = prompt_text_dialog("Rename", item->name);
        if (new_name != NULL) {
            if (new_name[0] != '\0' && strcmp(new_name, item->name) != 0) {
                char old_path[PATH_MAX];
                char new_path[PATH_MAX];
                struct stat existing_st;
                int confirmed = 1;
                if (!is_safe_path_component(new_name)) {
                    /* A name containing '/' (e.g. "../../important")
                     * would otherwise rename the file OUTSIDE the
                     * current directory via the bare rename() below,
                     * with none of fileops_move()'s safety checks. */
                    show_error_dialog("Error", "Name cannot contain '/' or be '.'/'..'");
                    confirmed = 0;
                } else if (!path_join(old_path, sizeof(old_path), active->path, item->name) ||
                    !path_join(new_path, sizeof(new_path), active->path, new_name)) {
                    show_error_dialog("Error", "Path too long");
                    confirmed = 0;
                } else if (lstat(new_path, &existing_st) == 0) {
                    /* rename() replaces an existing destination atomically
                     * and silently - mirror the TUI's F6 rename prompt
                     * (main.c) instead of losing the existing file. */
                    if (gui_fileop_on_overwrite(NULL, new_path) != FILEOPS_CHOICE_OVERWRITE) {
                        confirmed = 0;
                    }
                }

                if (!confirmed) {
                    /* Path-too-long already reported above; a declined
                     * overwrite is a silent no-op, matching Skip/Abort
                     * elsewhere. */
                } else if (rename(old_path, new_path) != 0) {
                    show_error_dialog("Error", strerror(errno));
                } else {
                    panel_load(active, active->path);
                    panel_load(other, other->path);
                }
            }
            g_free(new_name);
        }
    } else {
        char src_path[PATH_MAX];
        if (!path_join(src_path, sizeof(src_path), active->path, item->name)) {
            show_error_dialog("Error", "Path too long");
            return;
        }
        gui_modal_enter();
        fileops_move(src_path, other->path, &gui_fileop_callbacks);
        gui_progress_hide();
        gui_modal_leave();
        panel_load(active, active->path);
        panel_load(other, other->path);
    }
}

static void action_mkdir(void)
{
    GuiPanel *active = &g_panel[g_focused_panel];
    GuiPanel *other = other_panel_of_focused();
    char *name = prompt_text_dialog("New folder", "");
    if (name == NULL) {
        return;
    }
    if (name[0] != '\0') {
        char new_dir_path[PATH_MAX];
        if (!is_safe_path_component(name)) {
            /* Without this, a name like "existingsub/newname" silently
             * creates the directory INSIDE an existing subdirectory
             * instead of in the current directory, as "New folder"
             * implies. */
            show_error_dialog("Error", "Name cannot contain '/' or be '.'/'..'");
        } else if (!path_join(new_dir_path, sizeof(new_dir_path), active->path, name)) {
            show_error_dialog("Error", "Path too long");
        } else if (mkdir(new_dir_path, 0755) != 0) {
            show_error_dialog("Error", strerror(errno));
        } else {
            panel_load(active, active->path);
            if (strcmp(active->path, other->path) == 0) {
                panel_load(other, other->path);
            }
        }
    }
    g_free(name);
}

/* Shared by action_delete()/action_delete_permanent() - permanent
 * bypasses the trash for a real, unrecoverable delete (e.g. a large
 * file not worth doubling disk usage for, or sensitive data that
 * shouldn't linger in ~/.local/share/Trash). */
static void action_delete_impl(int permanent)
{
    GuiPanel *active = &g_panel[g_focused_panel];
    GuiPanel *other = other_panel_of_focused();
    TfmFileItem *item = panel_get_selected_item(active);
    if (item == NULL || strcmp(item->name, "..") == 0) {
        return;
    }

    char message[300];
    snprintf(message, sizeof(message), "%s%s%s?", permanent ? "Permanently delete " : "Delete ",
             item->name, item->is_dir ? "/" : "");
    if (!confirm_dialog(permanent ? "Permanently delete" : "Delete", message,
                         permanent ? "Permanently delete" : "Delete")) {
        return;
    }

    char target_path[PATH_MAX];
    if (!path_join(target_path, sizeof(target_path), active->path, item->name)) {
        show_error_dialog("Error", "Path too long");
        return;
    }
    gui_modal_enter();
    if (permanent) {
        fileops_delete(target_path, &gui_fileop_callbacks);
    } else {
        fileops_trash(target_path, &gui_fileop_callbacks);
    }
    gui_progress_hide();
    gui_modal_leave();
    panel_load(active, active->path);
    if (strcmp(active->path, other->path) == 0) {
        panel_load(other, other->path);
    }
}

static void action_delete(void)
{
    action_delete_impl(0);
}

static void action_delete_permanent(void)
{
    action_delete_impl(1);
}

/* Undo: restores the single most-recently-trashed item (see
 * fileops_restore_last_trashed()) - not a general undo of copy/move.
 * Reloads both panels since the restored item's directory may be either
 * one, or neither. */
static void action_undo(void)
{
    char restored_path[PATH_MAX] = "";
    gui_modal_enter();
    int restored =
        fileops_restore_last_trashed(&gui_fileop_callbacks, restored_path, sizeof(restored_path));
    gui_progress_hide();
    gui_modal_leave();

    if (restored) {
        char message[PATH_MAX + 32];
        snprintf(message, sizeof(message), "Restored: %s", restored_path);
        show_error_dialog("Undo", message);
    }

    panel_load(&g_panel[0], g_panel[0].path);
    panel_load(&g_panel[1], g_panel[1].path);
}

static void on_function_button_clicked(GtkButton *button, gpointer user_data)
{
    GtkApplication *app = user_data;
    const char *key = g_object_get_data(G_OBJECT(button), "tfm-key");
    if (strcmp(key, "F10") == 0) {
        g_application_quit(G_APPLICATION(app));
        return;
    }
    if (g_modal_depth > 0) {
        /* Unlike the keyboard controller (:1270) and window-close
         * (:1360), these mouse clicks had no g_modal_depth guard at all -
         * a click during an editor session or a pumped fileop could
         * re-enter copy/move/mkdir/delete on the same file. */
        return;
    }
    if (strcmp(key, "F3") == 0) {
        action_edit();
    } else if (strcmp(key, "F5") == 0) {
        action_copy();
    } else if (strcmp(key, "F6") == 0) {
        action_move();
    } else if (strcmp(key, "F7") == 0) {
        action_mkdir();
    } else if (strcmp(key, "F8") == 0) {
        action_delete();
    } else if (strcmp(key, "F9") == 0) {
        action_undo();
    }
}

static GtkWidget *build_function_bar(GtkApplication *app)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    /* Homogeneous so buttons shrink together (text ellipsizes) on a
     * narrow window instead of pushing the later F-keys (F8/F10) off the
     * right edge - hexpand alone only distributes extra space, never
     * shrinks below natural text width. */
    gtk_box_set_homogeneous(GTK_BOX(bar), TRUE);
    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);

    for (size_t i = 0; i < G_N_ELEMENTS(FUNCTION_KEYS); i++) {
        char label_text[32];
        snprintf(label_text, sizeof(label_text), "%s %s", FUNCTION_KEYS[i].key,
                 FUNCTION_KEYS[i].label);
        GtkWidget *btn = gtk_button_new_with_label(label_text);
        gtk_widget_set_hexpand(btn, TRUE);
        GtkWidget *btn_label = gtk_button_get_child(GTK_BUTTON(btn));
        if (GTK_IS_LABEL(btn_label)) {
            gtk_label_set_ellipsize(GTK_LABEL(btn_label), PANGO_ELLIPSIZE_END);
        }
        g_object_set_data(G_OBJECT(btn), "tfm-key", (gpointer)FUNCTION_KEYS[i].key);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_function_button_clicked), app);
        gtk_box_append(GTK_BOX(bar), btn);
    }

    return bar;
}

/* Static base styling independent of the Omarchy theme: a thin divider
 * between the two panels (@borders adapts automatically to light/dark). */
static void apply_base_css(void)
{
    GdkDisplay *display = gdk_display_get_default();
    if (display == NULL) {
        return;
    }
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        provider, ".tfm-panel-active { border: 2px solid @accent_color; padding: 3px; }\n"
                  ".tfm-panel-active .heading { color: @accent_color; }\n");
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* Checks whether foot.desktop is the first (highest-priority) entry in
 * one xdg-terminals.list file, i.e. whether foot is actually the user's
 * configured default terminal. */
static int xdg_terminals_list_picks_foot(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return -1; /* file doesn't exist - caller should try the next one */
    }
    int result = -1;
    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '#' || *p == '\n' || *p == '\0') {
            continue;
        }
        size_t len = strlen(p);
        while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' ||
                            p[len - 1] == ' ' || p[len - 1] == '\t')) {
            p[--len] = '\0';
        }
        result = (strcmp(p, "foot.desktop") == 0);
        break;
    }
    fclose(fp);
    return result;
}

/* Determines whether foot is the user's actually configured default
 * terminal, following the same xdg-terminals.list search order as
 * xdg-terminal-exec itself: $XDG_CONFIG_HOME (or ~/.config), then
 * $XDG_CONFIG_DIRS (or /etc/xdg), each checked both directly and under an
 * "xdg-terminal-exec/" subdirectory. The first file that exists wins;
 * without any such file, Omarchy's own packaged default list also names
 * foot, so that's the final fallback. */
static int is_foot_the_active_terminal(void)
{
    const char *home = getenv("HOME");
    const char *config_home = getenv("XDG_CONFIG_HOME");
    const char *config_dirs = getenv("XDG_CONFIG_DIRS");
    char dir_buf[PATH_MAX];
    const char *dirs[2];
    int ndirs = 0;

    if (config_home != NULL && config_home[0] != '\0') {
        dirs[ndirs++] = config_home;
    } else if (home != NULL) {
        /* A truncated path from a very long $HOME would silently search
         * a shorter, unrelated directory instead of failing. */
        if ((size_t)snprintf(dir_buf, sizeof(dir_buf), "%s/.config", home) < sizeof(dir_buf)) {
            dirs[ndirs++] = dir_buf;
        }
    }
    /* Only the first $XDG_CONFIG_DIRS entry is checked here (matching this
     * function's scope: "is foot active", not a full multi-dir search) -
     * good enough since Omarchy/most distros set a single-entry default of
     * /etc/xdg. */
    const char *config_dirs_first = (config_dirs != NULL && config_dirs[0] != '\0')
                                         ? config_dirs
                                         : "/etc/xdg";
    dirs[ndirs++] = config_dirs_first;

    for (int i = 0; i < ndirs; i++) {
        char path[PATH_MAX];
        /* Both snprintf() calls' truncation checked - a truncated path
         * would almost certainly just fail its fopen() anyway (ENOENT),
         * but skipping it outright is more honest than silently
         * searching a shortened, unrelated path. */
        if ((size_t)snprintf(path, sizeof(path), "%s/xdg-terminal-exec/xdg-terminals.list",
                              dirs[i]) < sizeof(path)) {
            int r = xdg_terminals_list_picks_foot(path);
            if (r >= 0) {
                return r;
            }
        }
        if ((size_t)snprintf(path, sizeof(path), "%s/xdg-terminals.list", dirs[i]) < sizeof(path)) {
            int r = xdg_terminals_list_picks_foot(path);
            if (r >= 0) {
                return r;
            }
        }
    }

    return xdg_terminals_list_picks_foot(
               "/usr/share/omarchy/default/xdg-terminal-exec/hyprland-xdg-terminals.list") != 0;
}

/* Reads the font size configured in foot from "font=<Name>:size=<N>" in
 * ~/.config/foot/foot.ini, so tfm-gui starts at the same size as the
 * terminal UI - but only when foot is the active default terminal (see
 * is_foot_the_active_terminal()), since a stale foot.ini left behind by
 * an alacritty/kitty/ghostty user would otherwise apply the wrong size.
 * Returns GUI_DEFAULT_FONT_SIZE on error/missing value/non-foot terminal. */
static double read_terminal_font_size(void)
{
    if (!is_foot_the_active_terminal()) {
        return GUI_DEFAULT_FONT_SIZE;
    }

    const char *home = getenv("HOME");
    if (home == NULL) {
        return GUI_DEFAULT_FONT_SIZE;
    }
    char path[PATH_MAX];
    if ((size_t)snprintf(path, sizeof(path), "%s/.config/foot/foot.ini", home) >= sizeof(path)) {
        return GUI_DEFAULT_FONT_SIZE;
    }

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return GUI_DEFAULT_FONT_SIZE;
    }

    double size = GUI_DEFAULT_FONT_SIZE;
    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        /* Only the "font=" line itself, not e.g. "font-bold=" -
         * foot.ini has several keys starting with "font" but only
         * "font=" carries the base size. */
        if (strncmp(p, "font=", 5) != 0) {
            continue;
        }
        char *size_tag = strstr(p, "size=");
        if (size_tag != NULL) {
            size = atof(size_tag + 5);
            break;
        }
    }
    fclose(fp);
    /* atof() happily returns "inf" for junk/overflow input, which would
     * otherwise flow straight into generated CSS as e.g. "infpt". Clamp
     * to a sane font-size range instead of just checking > 0. */
    if (!isfinite(size) || size < 6.0 || size > 32.0) {
        return GUI_DEFAULT_FONT_SIZE;
    }
    return size;
}

static double g_font_size = GUI_DEFAULT_FONT_SIZE;
static double g_default_font_size = GUI_DEFAULT_FONT_SIZE;
static GtkCssProvider *g_font_css_provider = NULL;

/* Applies the current font size (see Ctrl+Plus/Minus/0 in
 * on_window_key_pressed) via CSS to all ".tfm-mono" widgets - reloads the
 * existing provider's content in place instead of destroying/recreating it
 * on every Ctrl+Plus/Minus/0, same reasoning as apply_omarchy_theme(). */
static void apply_font_size(void)
{
    GdkDisplay *display = gdk_display_get_default();
    if (display == NULL) {
        return;
    }

    char css[256];
    snprintf(css, sizeof(css),
             ".tfm-mono { font-family: \"JetBrainsMono Nerd Font\", monospace; font-size: %gpt; }\n",
             g_font_size);

    if (g_font_css_provider == NULL) {
        g_font_css_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(g_font_css_provider),
                                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_string(g_font_css_provider, css);
}

static gboolean on_window_key_pressed(GtkEventControllerKey *controller, guint keyval,
                                       guint keycode, GdkModifierType state, gpointer user_data)
{
    (void)controller;
    (void)keycode;
    GtkWindow *window = GTK_WINDOW(user_data);

    /* While a modal dialog or file operation is active, let the key reach
     * the focused child widget instead of intercepting it here - see
     * g_modal_depth above. */
    if (g_modal_depth > 0) {
        return GDK_EVENT_PROPAGATE;
    }

    if (keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab || keyval == GDK_KEY_KP_Tab) {
        focus_panel(g_focused_panel == 0 ? 1 : 0);
        return GDK_EVENT_STOP;
    }

    /* F3 and F5-F10 also work as keyboard shortcuts, not just via the
     * button bar. */
    if (keyval == GDK_KEY_F3) {
        action_edit();
        return GDK_EVENT_STOP;
    }
    if (keyval == GDK_KEY_F5) {
        action_copy();
        return GDK_EVENT_STOP;
    }
    if (keyval == GDK_KEY_F6) {
        action_move();
        return GDK_EVENT_STOP;
    }
    if (keyval == GDK_KEY_F7) {
        action_mkdir();
        return GDK_EVENT_STOP;
    }
    if (keyval == GDK_KEY_F8) {
        /* Shift+F8 bypasses the trash for a real, permanent delete - see
         * action_delete_impl()'s comment. */
        if (state & GDK_SHIFT_MASK) {
            action_delete_permanent();
        } else {
            action_delete();
        }
        return GDK_EVENT_STOP;
    }
    if (keyval == GDK_KEY_F9) {
        action_undo();
        return GDK_EVENT_STOP;
    }
    /* Ctrl+Z as the more familiar "undo" shortcut, alongside F9 (which
     * mirrors the TUI's own binding). */
    if (keyval == GDK_KEY_z && (state & GDK_CONTROL_MASK)) {
        action_undo();
        return GDK_EVENT_STOP;
    }
    if (keyval == GDK_KEY_F10) {
        g_application_quit(G_APPLICATION(g_application_get_default()));
        return GDK_EVENT_STOP;
    }

    /* Ctrl+Plus/Minus/0, as usual in terminal emulators: grow/shrink font
     * size, or reset to the size read from foot.ini. */
    if (state & GDK_CONTROL_MASK) {
        if (keyval == GDK_KEY_plus || keyval == GDK_KEY_equal || keyval == GDK_KEY_KP_Add) {
            g_font_size += 1.0;
            apply_font_size();
            return GDK_EVENT_STOP;
        }
        if (keyval == GDK_KEY_minus || keyval == GDK_KEY_KP_Subtract) {
            if (g_font_size > 5.0) {
                g_font_size -= 1.0;
                apply_font_size();
            }
            return GDK_EVENT_STOP;
        }
        if (keyval == GDK_KEY_0 || keyval == GDK_KEY_KP_0) {
            g_font_size = g_default_font_size;
            apply_font_size();
            return GDK_EVENT_STOP;
        }
    }

    /* A panel holds keyboard focus for arrow keys/Enter, but typed
     * characters should still go straight to the shell line without
     * having to click it first. */
    GtkWidget *current_focus = gtk_window_get_focus(window);
    if (current_focus != g_shell_entry &&
        !(state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_META_MASK |
                    GDK_SUPER_MASK | GDK_HYPER_MASK))) {
        gunichar ch = gdk_keyval_to_unicode(keyval);
        if (ch != 0 && g_unichar_isgraph(ch)) {
            char utf8[8];
            int len = g_unichar_to_utf8(ch, utf8);
            utf8[len] = '\0';

            gtk_widget_grab_focus(g_shell_entry);
            int pos = gtk_editable_get_position(GTK_EDITABLE(g_shell_entry));
            gtk_editable_insert_text(GTK_EDITABLE(g_shell_entry), utf8, len, &pos);
            gtk_editable_set_position(GTK_EDITABLE(g_shell_entry), pos);
            return GDK_EVENT_STOP;
        }
    }

    return GDK_EVENT_PROPAGATE;
}

/* g_modal_depth only guards the window's key controller, not the window
 * close itself. A compositor-side close (SUPER+Q, CSD close button)
 * bypasses any AdwDialog grab and is delivered while a fileop pumps the
 * main context or a nested GMainLoop waits on a dialog - destroying the
 * window then would leave callbacks running on freed widgets. */
static gboolean on_window_close_request(GtkWindow *window, gpointer user_data)
{
    (void)window;
    (void)user_data;

    if (g_modal_depth > 0) {
        /* show_error_dialog() pumps a nested main loop, so a repeated close
         * click re-enters this handler while "Please wait" is still up.
         * A dedicated flag (g_modal_depth stays > 0 throughout, so it
         * can't serve) refuses those re-entrant closes instead of
         * stacking another dialog per click. */
        static int showing_wait_dialog = 0;
        if (!showing_wait_dialog) {
            showing_wait_dialog = 1;
            show_error_dialog("Please wait",
                               "An operation or dialog is still in progress. Please finish it "
                               "before closing the window.");
            showing_wait_dialog = 0;
        }
        return TRUE; /* refuse to close */
    }

    return FALSE; /* allow normal close */
}

/* Flushes g_pending_panel_load_error (see panel_load_indexed()) once the
 * main window is actually mapped. Scheduled via g_idle_add() rather than
 * called right after gtk_window_present(): present() only requests
 * mapping, so a blocking dialog shown synchronously in activate() hits
 * the unmapped-parent hang described at g_pending_panel_load_error. */
static gboolean flush_pending_panel_load_errors(gpointer user_data)
{
    (void)user_data;
    for (size_t i = 0; i < G_N_ELEMENTS(g_pending_panel_load_error); i++) {
        if (g_pending_panel_load_error[i][0] != '\0') {
            show_error_dialog("Error", g_pending_panel_load_error[i]);
            g_pending_panel_load_error[i][0] = '\0';
        }
    }
    if (g_pending_config_load_error[0] != '\0') {
        show_error_dialog("Config warning", g_pending_config_load_error);
        g_pending_config_load_error[0] = '\0';
    }
    return G_SOURCE_REMOVE;
}

static void activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;

    apply_base_css();
    apply_font_size();
    apply_omarchy_theme();

    GtkWidget *window = adw_application_window_new(app);
    g_window = GTK_WINDOW(window);
    gtk_window_set_title(GTK_WINDOW(window), "TFM");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 600);
    g_signal_connect(window, "close-request", G_CALLBACK(on_window_close_request), NULL);

    GtkWidget *toolbar_view = adw_toolbar_view_new();
    GtkWidget *header_bar = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header_bar),
                                     adw_window_title_new("TFM", "Taiku File Manager"));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), header_bar);

    /* Fixed 50/50 split instead of a freely resizable GtkPaned - panel
     * width shouldn't be skewed by label content. Non-zero spacing gives
     * the active panel's rounded accent border (.tfm-panel-active) room
     * on the side facing its neighbor - at 0 spacing the neighbor panel
     * sat right on the edge and visibly clipped the corner. */
    GtkWidget *panels_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_vexpand(panels_box, TRUE);
    gtk_box_set_homogeneous(GTK_BOX(panels_box), TRUE);
    /* Margin so the active panel's accent border doesn't touch the window edge. */
    gtk_widget_set_margin_start(panels_box, 6);
    gtk_widget_set_margin_end(panels_box, 6);
    gtk_widget_set_margin_bottom(panels_box, 6);

    /* "actual" is a sentinel value for left_path (tfm's startup
     * directory) - not a real path. */
    char left_start_path[PATH_MAX];
    if (strcasecmp(g_cfg.left_path, "actual") == 0) {
        if (getcwd(left_start_path, sizeof(left_start_path)) == NULL) {
            const char *home = getenv("HOME");
            snprintf(left_start_path, sizeof(left_start_path), "%s", home != NULL ? home : "/");
        }
    } else {
        snprintf(left_start_path, sizeof(left_start_path), "%s", g_cfg.left_path);
    }

    GtkWidget *left = build_panel_widget(&g_panel[0], left_start_path);
    GtkWidget *right = build_panel_widget(&g_panel[1], g_cfg.right_path);
    gtk_box_append(GTK_BOX(panels_box), left);
    gtk_box_append(GTK_BOX(panels_box), right);

    GtkWidget *bottom_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(bottom_area), build_shell_bar());
    gtk_box_append(GTK_BOX(bottom_area), build_function_bar(app));

    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar_view), bottom_area);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), panels_box);

    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), toolbar_view);

    /* Tab/Shift+Tab should only switch between the two panels, not cycle
     * through every focusable widget (including the F-key buttons). */
    GtkEventController *key_controller = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_controller, GTK_PHASE_CAPTURE);
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_window_key_pressed), window);
    gtk_widget_add_controller(window, key_controller);

    focus_panel(0);
    gtk_window_present(GTK_WINDOW(window));

    if (g_pending_panel_load_error[0][0] != '\0' || g_pending_panel_load_error[1][0] != '\0' ||
        g_pending_config_load_error[0] != '\0') {
        g_idle_add(flush_pending_panel_load_errors, NULL);
    }
}

/* Unlike the terminal UI, the GUI has no "actual" sentinel for left_path
 * - both panel paths are saved directly from current state. A path is
 * still empty if its initial load and the $HOME/"/" fallback both
 * failed; then the configured value is kept, so the broken path survives
 * in tfm.ini for the user to fix instead of being replaced by "". */
static void on_shutdown(GApplication *app, gpointer user_data)
{
    (void)app;
    (void)user_data;
    if (g_panel[0].path[0] != '\0') {
        snprintf(g_cfg.left_path, sizeof(g_cfg.left_path), "%s", g_panel[0].path);
    }
    if (g_panel[1].path[0] != '\0') {
        snprintf(g_cfg.right_path, sizeof(g_cfg.right_path), "%s", g_panel[1].path);
    }
    /* The window is already going away by the time "shutdown" fires -
     * same reasoning as the TUI's exit-path config_save(): no UI left to
     * show a dialog in, so report a failure to stderr instead (visible
     * if tfm-gui was launched from a terminal). */
    char save_error[PATH_MAX + 64] = "";
    config_save(&g_cfg, save_error, sizeof(save_error));
    if (save_error[0] != '\0') {
        fprintf(stderr, "tfm-gui: failed to save config: %s\n", save_error);
    }
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("tfm-gui %s\n", TFM_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: tfm-gui [--version] [--help]\n"
                   "GTK4/libadwaita GUI file manager. Run with no arguments to start.\n");
            return 0;
        }
    }

    /* Called explicitly rather than relying on gtk_init()/adw_init()
     * having already done it internally - dir.c's directory sort (shared
     * with the TUI) needs the process locale set for case-folding and
     * collation to be locale-aware rather than plain "C"-locale ASCII,
     * and this must happen before the first panel is loaded below. */
    setlocale(LC_ALL, "");

    config_load(&g_cfg, g_pending_config_load_error, sizeof(g_pending_config_load_error));
    g_default_font_size = read_terminal_font_size();
    g_font_size = g_default_font_size;

    g_unix_signal_add(SIGUSR1, on_sigusr1, NULL);

    AdwApplication *app = adw_application_new("de.taiku.tfm", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
