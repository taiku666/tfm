/* GUI tests for tfm-gui, run headless by tests/run_gui_tests.sh (make
 * test-gui). gui_main.c is #included rather than linked, so the tests can
 * reach its static state and handlers without test hooks in shipped code.
 *
 * Dialogs block in nested main loops, so a test queues the dialogs it
 * expects and a polling "responder" answers them from inside those loops.
 * Any dialog not in the queue - what a missing g_modal_depth guard
 * produces - fails the test and is closed, so a regression can't hang. */

#define _DEFAULT_SOURCE

/* Before the g_application_quit macro below, so GIO's own declaration of
 * it stays untouched; gui_main.c's later #include is then a no-op. */
#include <adwaita.h>
#include <signal.h>
#include <stdarg.h>

/* Records quit requests instead of quitting, so a test can check F10 is
 * blocked mid-operation without the app going away. */
static int g_quit_requests;
static int g_quit_passthrough;
static void test_application_quit(GApplication *application);

/* features.h (pulled in above) has already set _DEFAULT_SOURCE to 1;
 * gui_main.c's own empty #define would otherwise warn as a redefinition. */
#undef _DEFAULT_SOURCE
#define main tfm_gui_main
#define g_application_quit test_application_quit
#include "../src_gui/gui_main.c"
#undef g_application_quit
#undef main

static void test_application_quit(GApplication *application)
{
    g_quit_requests++;
    if (g_quit_passthrough) {
        g_application_quit(application);
    }
}

#include "test.h"
#include "test_fs_helpers.h"

/* --- fixture ------------------------------------------------------------- */

static char g_home[PATH_MAX];
static char g_left[PATH_MAX];
static char g_right[PATH_MAX];
static char g_flag[PATH_MAX];        /* the wait scripts below exit once this exists */
static char g_wait_script[PATH_MAX]; /* $EDITOR and the shell/run tests' program */
static char g_colors_toml[PATH_MAX];

static const char COLORS_TOML[] = "mode = \"dark\"\n"
                                  "accent = \"#7aa2f7\"\n"
                                  "background = \"#1a1b26\"\n"
                                  "foreground = \"#c0caf5\"\n";

/* Waits for g_flag, then appends a line to its argument (if any) - so a
 * test sees both that the modal wait ended and that the program ran. */
static const char WAIT_SCRIPT_FMT[] = "#!/bin/sh\n"
                                      "while [ ! -e '%s' ]; do sleep 0.02; done\n"
                                      "[ -n \"$1\" ] && echo ran >> \"$1\"\n"
                                      "exit 0\n";

/* noinline for the same -Wformat-truncation false positive as
 * join_path() in test_fs_helpers.h. */
static __attribute__((noinline)) void fixture_path(char *out, const char *dir, const char *name)
{
    if (join_path(out, PATH_MAX, dir, "/") >= PATH_MAX ||
        (size_t)snprintf(out + strlen(out), PATH_MAX - strlen(out), "%s", name) >= PATH_MAX - strlen(out)) {
        abort();
    }
}

/* Resets left/ to {a, b, c, sub/}, right/ to empty, and both panels to
 * them, so no test depends on the one before it. */
static void reset_fixture(void)
{
    force_remove_tree(g_left);
    force_remove_tree(g_right);
    mkdir(g_left, 0755);
    mkdir(g_right, 0755);
    const char *names[] = {"a", "b", "c"};
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
        char path[PATH_MAX];
        fixture_path(path, g_left, names[i]);
        write_file(path, names[i]);
    }
    char sub[PATH_MAX];
    fixture_path(sub, g_left, "sub");
    mkdir(sub, 0755);
    unlink(g_flag);

    gui_clear_marks(&g_panel[0]);
    gui_clear_marks(&g_panel[1]);
    panel_load(&g_panel[0], g_left);
    panel_load(&g_panel[1], g_right);
    focus_panel(0);
}

static char *read_back(const char *dir, const char *name)
{
    static char buf[256];
    char path[PATH_MAX];
    fixture_path(path, dir, name);
    if (read_file(path, buf, sizeof(buf)) < 0) {
        return NULL;
    }
    return buf;
}

static int exists_in(const char *dir, const char *name)
{
    char path[PATH_MAX];
    fixture_path(path, dir, name);
    return path_exists(path);
}

static void pump_for(int ms)
{
    gint64 end = g_get_monotonic_time() + ms * 1000;
    while (g_get_monotonic_time() < end) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
}

static int pump_until(gboolean (*done)(void), int timeout_ms)
{
    gint64 end = g_get_monotonic_time() + timeout_ms * 1000;
    while (!done()) {
        if (g_get_monotonic_time() > end) {
            return 0;
        }
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
    return 1;
}

/* --- panel helpers ------------------------------------------------------- */

static int store_index_of(GuiPanel *panel, const char *name)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(panel->store));
    for (guint i = 0; i < n; i++) {
        TfmFileItem *item = g_list_model_get_item(G_LIST_MODEL(panel->store), i);
        int match = strcmp(item->name, name) == 0;
        g_object_unref(item);
        if (match) {
            return (int)i;
        }
    }
    return -1;
}

static void select_name(GuiPanel *panel, const char *name)
{
    int index = store_index_of(panel, name);
    GtkSelectionModel *model = gtk_list_view_get_model(GTK_LIST_VIEW(panel->list_view));
    if (index >= 0) {
        gtk_single_selection_set_selected(GTK_SINGLE_SELECTION(model), (guint)index);
    }
}

static const char *selected_name(GuiPanel *panel)
{
    TfmFileItem *item = panel_get_selected_item(panel);
    return item != NULL ? item->name : NULL;
}

static void mark_name(GuiPanel *panel, const char *name)
{
    select_name(panel, name);
    action_toggle_mark();
}

static gboolean press_key(guint keyval, GdkModifierType state)
{
    return on_window_key_pressed(NULL, keyval, 0, state, g_window);
}

/* Depth-first search of widget's subtree for a widget accepted by match. */
static GtkWidget *find_widget(GtkWidget *widget, gboolean (*match)(GtkWidget *, const char *),
                              const char *arg)
{
    if (widget == NULL) {
        return NULL;
    }
    if (match(widget, arg)) {
        return widget;
    }
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_widget(child, match, arg);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static gboolean is_button_labeled(GtkWidget *widget, const char *label)
{
    if (!GTK_IS_BUTTON(widget)) {
        return FALSE;
    }
    const char *text = gtk_button_get_label(GTK_BUTTON(widget));
    if (text == NULL) {
        /* AdwAlertDialog's response buttons carry their label in a child
         * GtkLabel rather than as the button's own label. */
        GtkWidget *child = gtk_button_get_child(GTK_BUTTON(widget));
        text = GTK_IS_LABEL(child) ? gtk_label_get_label(GTK_LABEL(child)) : NULL;
    }
    return text != NULL && strcmp(text, label) == 0;
}

static gboolean is_window_title(GtkWidget *widget, const char *unused)
{
    (void)unused;
    return ADW_IS_WINDOW_TITLE(widget);
}

static gboolean is_entry(GtkWidget *widget, const char *unused)
{
    (void)unused;
    return GTK_IS_ENTRY(widget);
}

/* A click on the real function-bar button, e.g. "F8 Delete". */
static void click_function_button(const char *label)
{
    GtkWidget *button = find_widget(GTK_WIDGET(g_window), is_button_labeled, label);
    if (button != NULL) {
        g_signal_emit_by_name(button, "clicked");
    } else {
        fprintf(stderr, "    (no function button \"%s\")\n", label);
        tfm_test_failed = 1;
    }
}

/* --- dialog responder ---------------------------------------------------- */

typedef struct {
    const char *heading;    /* expected heading/title */
    const char *body;       /* substring expected in the body, or NULL */
    const char *entry_text; /* typed into the dialog's entry first, or NULL */
    void (*probe)(void);    /* run while the dialog is up, or NULL */
    const char *button;     /* label of the button to press; NULL = the probe closes it */
} DialogStep;

static const DialogStep *g_steps;
static size_t g_step_count;
static size_t g_step_next;
static char g_responder_error[512];

static void responder_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void responder_fail(const char *fmt, ...)
{
    if (g_responder_error[0] == '\0') {
        va_list args;
        va_start(args, fmt);
        vsnprintf(g_responder_error, sizeof(g_responder_error), fmt, args);
        va_end(args);
    }
}

static const char *dialog_heading(AdwDialog *dialog)
{
    if (ADW_IS_ALERT_DIALOG(dialog)) {
        return adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(dialog));
    }
    GtkWidget *title = find_widget(GTK_WIDGET(dialog), is_window_title, NULL);
    return title != NULL ? adw_window_title_get_title(ADW_WINDOW_TITLE(title)) : "";
}

static gboolean responder_poll(gpointer user_data)
{
    (void)user_data;
    AdwDialog *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(g_window));
    if (dialog == NULL || dialog == g_progress.dialog ||
        g_object_get_data(G_OBJECT(dialog), "tfm-test-handled") != NULL) {
        return G_SOURCE_CONTINUE;
    }
    g_object_set_data(G_OBJECT(dialog), "tfm-test-handled", GINT_TO_POINTER(1));

    const char *heading = dialog_heading(dialog);
    if (g_step_next >= g_step_count) {
        responder_fail("unexpected dialog \"%s\"", heading);
        adw_dialog_close(dialog);
        return G_SOURCE_CONTINUE;
    }
    /* Advanced before the probe runs: a probe may open a nested dialog,
     * which this source (can-recurse) answers with the next step. */
    const DialogStep *step = &g_steps[g_step_next++];
    if (strcmp(heading, step->heading) != 0) {
        responder_fail("expected a dialog titled \"%s\", got \"%s\"", step->heading, heading);
        adw_dialog_close(dialog);
        return G_SOURCE_CONTINUE;
    }
    if (step->body != NULL) {
        const char *body = ADW_IS_ALERT_DIALOG(dialog) ? adw_alert_dialog_get_body(ADW_ALERT_DIALOG(dialog)) : "";
        if (strstr(body, step->body) == NULL) {
            responder_fail("dialog body lacks \"%s\"", step->body);
        }
    }
    if (step->probe != NULL) {
        g_object_ref(dialog);
        step->probe();
        AdwDialog *still = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(g_window));
        gboolean closed = still != dialog;
        g_object_unref(dialog);
        if (closed != (step->button == NULL)) {
            responder_fail("dialog \"%s\" %s by its probe", step->heading,
                           closed ? "was closed" : "stayed open");
        }
        if (closed || step->button == NULL) {
            return G_SOURCE_CONTINUE;
        }
    }
    if (step->entry_text != NULL) {
        GtkWidget *entry = find_widget(GTK_WIDGET(dialog), is_entry, NULL);
        if (entry == NULL) {
            responder_fail("no entry in dialog \"%s\"", heading);
        } else {
            gtk_editable_set_text(GTK_EDITABLE(entry), step->entry_text);
        }
    }
    GtkWidget *button = find_widget(GTK_WIDGET(dialog), is_button_labeled, step->button);
    if (button == NULL) {
        responder_fail("no button \"%s\"", step->button);
        adw_dialog_close(dialog);
    } else {
        g_signal_emit_by_name(button, "clicked");
    }
    return G_SOURCE_CONTINUE;
}

static void expect_dialogs(const DialogStep *steps, size_t count)
{
    g_steps = steps;
    g_step_count = count;
    g_step_next = 0;
    g_responder_error[0] = '\0';
}

/* Fails the test unless every queued dialog appeared, in order, and
 * nothing else did. */
#define ASSERT_DIALOGS_DONE() \
    do { \
        if (g_responder_error[0] != '\0') { \
            fprintf(stderr, "    responder: %s\n", g_responder_error); \
        } \
        ASSERT_STR_EQ(g_responder_error, ""); \
        ASSERT_EQ(g_step_next, g_step_count); \
        expect_dialogs(NULL, 0); \
    } while (0)

/* --- probes: run while something modal is in progress ------------------- */

/* Probes can't use ASSERT_* (they don't run inside the test function),
 * so the first failed check is kept for the test to report. */
static char g_probe_error[256];
static int g_probe_ran;

#define PROBE_CHECK(cond) \
    do { \
        if (!(cond) && g_probe_error[0] == '\0') { \
            snprintf(g_probe_error, sizeof(g_probe_error), "%s:%d: %s", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define ASSERT_PROBE_OK() \
    do { \
        if (g_probe_error[0] != '\0') { \
            fprintf(stderr, "    probe: %s\n", g_probe_error); \
        } \
        ASSERT_TRUE(g_probe_ran); \
        ASSERT_STR_EQ(g_probe_error, ""); \
    } while (0)

static void reset_probe(void)
{
    g_probe_error[0] = '\0';
    g_probe_ran = 0;
}

/* The modal-depth matrix: every way of starting another operation must be
 * a no-op while one is in progress. A guard that's missing opens a second
 * dialog, which the responder reports as unexpected. */
static void probe_everything_blocked(void)
{
    g_probe_ran = 1;
    PROBE_CHECK(g_modal_depth > 0);

    char left_before[PATH_MAX];
    char right_before[PATH_MAX];
    snprintf(left_before, sizeof(left_before), "%s", g_panel[0].path);
    snprintf(right_before, sizeof(right_before), "%s", g_panel[1].path);
    int focused = g_focused_panel;
    int quits = g_quit_requests;

    static const struct {
        guint keyval;
        GdkModifierType state;
    } keys[] = {
        {GDK_KEY_F3, 0}, {GDK_KEY_F5, 0}, {GDK_KEY_F6, 0},     {GDK_KEY_F7, 0},
        {GDK_KEY_F8, 0}, {GDK_KEY_F8, GDK_SHIFT_MASK},         {GDK_KEY_F9, 0},
        {GDK_KEY_z, GDK_CONTROL_MASK},                        {GDK_KEY_F10, 0},
        {GDK_KEY_Tab, 0}, {GDK_KEY_space, 0}, {GDK_KEY_asterisk, 0}, {GDK_KEY_x, 0},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(keys); i++) {
        PROBE_CHECK(press_key(keys[i].keyval, keys[i].state) == GDK_EVENT_PROPAGATE);
    }
    PROBE_CHECK(g_focused_panel == focused);

    static const char *buttons[] = {"F3 Edit", "F5 Copy", "F6 Move", "F7 Mkdir", "F8 Delete", "F9 Undo", "F10 Quit"};
    for (size_t i = 0; i < G_N_ELEMENTS(buttons); i++) {
        click_function_button(buttons[i]);
    }
    PROBE_CHECK(g_quit_requests == quits);

    /* Enter/double-click on "sub" must not cd... */
    int sub = store_index_of(&g_panel[0], "sub");
    PROBE_CHECK(sub >= 0);
    if (sub >= 0) {
        on_item_activated(NULL, (guint)sub, &g_panel[0]);
    }
    /* ...and Enter in the shell line must not run anything. */
    char ran[PATH_MAX];
    fixture_path(ran, g_home, "shell-ran");
    char command[PATH_MAX + 16];
    snprintf(command, sizeof(command), "touch '%s'", ran);
    gtk_editable_set_text(GTK_EDITABLE(g_shell_entry), command);
    on_shell_entry_activate(GTK_ENTRY(g_shell_entry), NULL);
    gtk_editable_set_text(GTK_EDITABLE(g_shell_entry), "");
    pump_for(50);
    PROBE_CHECK(!path_exists(ran));

    PROBE_CHECK(strcmp(g_panel[0].path, left_before) == 0);
    PROBE_CHECK(strcmp(g_panel[1].path, right_before) == 0);
    PROBE_CHECK(g_modal_depth > 0);
}

/* A second close while "Please wait" is up must not stack another one. */
static void probe_close_again(void)
{
    gtk_window_close(g_window);
    PROBE_CHECK(gtk_widget_get_visible(GTK_WIDGET(g_window)));
}

/* Closing the window (compositor close, CSD button) while a dialog is up:
 * libadwaita answers the dialog with its close response first, and the
 * window stays. */
static void probe_close_window(void)
{
    g_probe_ran = 1;
    gtk_window_close(g_window);
    PROBE_CHECK(gtk_widget_get_visible(GTK_WIDGET(g_window)));
}

/* During a wait with no dialog up, on_window_close_request() refuses the
 * close with "Please wait". */
static void probe_all(void)
{
    probe_everything_blocked();
    gtk_window_close(g_window);
    PROBE_CHECK(gtk_widget_get_visible(GTK_WIDGET(g_window)));
    PROBE_CHECK(g_modal_depth > 0);
}

static const DialogStep PLEASE_WAIT = {"Please wait", "still in progress", NULL, probe_close_again, "OK"};

/* For waits without a dialog (editor, shell command, program): runs the
 * probes once from inside the pump, then lets the wait script exit. */
static gboolean probe_during_pump(gpointer user_data)
{
    (void)user_data;
    probe_all();
    write_file(g_flag, "");
    return G_SOURCE_REMOVE;
}

/* --- tests --------------------------------------------------------------- */

TEST(startup_lists_both_panels_and_applies_the_omarchy_theme)
{
    reset_fixture();
    /* "..", "sub", a, b, c */
    ASSERT_EQ(g_list_model_get_n_items(G_LIST_MODEL(g_panel[0].store)), 5);
    ASSERT_STR_EQ(g_panel[0].path, g_left);
    ASSERT_STR_EQ(g_panel[1].path, g_right);
    ASSERT_EQ(g_modal_depth, 0);
    ASSERT_TRUE(g_theme_css_provider != NULL);
}

/* CR4-C7: rename() replaces an existing file silently, so the GUI's
 * same-folder F6 rename has to ask first. */
TEST(same_dir_rename_over_existing_file_prompts_and_skip_keeps_both)
{
    reset_fixture();
    panel_load(&g_panel[1], g_left);
    select_name(&g_panel[0], "a");
    static const DialogStep steps[] = {
        {"Rename", NULL, "b", NULL, "OK"},
        {"File exists", "already exists", NULL, NULL, "Skip"},
    };
    expect_dialogs(steps, G_N_ELEMENTS(steps));
    ASSERT_EQ(press_key(GDK_KEY_F6, 0), GDK_EVENT_STOP);
    ASSERT_DIALOGS_DONE();

    ASSERT_STR_EQ(read_back(g_left, "a"), "a");
    ASSERT_STR_EQ(read_back(g_left, "b"), "b");
    ASSERT_EQ(g_modal_depth, 0);
}

TEST(same_dir_rename_over_existing_file_overwrites_when_confirmed)
{
    reset_fixture();
    panel_load(&g_panel[1], g_left);
    select_name(&g_panel[0], "a");
    static const DialogStep steps[] = {
        {"Rename", NULL, "b", NULL, "OK"},
        {"File exists", NULL, NULL, NULL, "Overwrite"},
    };
    expect_dialogs(steps, G_N_ELEMENTS(steps));
    press_key(GDK_KEY_F6, 0);
    ASSERT_DIALOGS_DONE();

    ASSERT_FALSE(exists_in(g_left, "a"));
    ASSERT_STR_EQ(read_back(g_left, "b"), "a");
    /* Both panels show this folder and were reloaded. */
    ASSERT_EQ(store_index_of(&g_panel[0], "a"), -1);
    ASSERT_EQ(store_index_of(&g_panel[1], "a"), -1);
}

TEST(nothing_else_starts_while_a_confirm_dialog_is_open)
{
    reset_fixture();
    reset_probe();
    select_name(&g_panel[0], "a");
    static const DialogStep steps[] = {
        {"Delete", "Delete a?", NULL, probe_everything_blocked, "Cancel"},
    };
    expect_dialogs(steps, G_N_ELEMENTS(steps));
    press_key(GDK_KEY_F8, 0);
    ASSERT_DIALOGS_DONE();
    ASSERT_PROBE_OK();

    ASSERT_TRUE(exists_in(g_left, "a"));
    ASSERT_EQ(g_modal_depth, 0);
    ASSERT_EQ(g_quit_requests, 0);
}

TEST(closing_the_window_cancels_a_delete_confirmation)
{
    reset_fixture();
    reset_probe();
    select_name(&g_panel[0], "a");
    static const DialogStep steps[] = {
        {"Delete", "Delete a?", NULL, probe_close_window, NULL},
    };
    expect_dialogs(steps, G_N_ELEMENTS(steps));
    press_key(GDK_KEY_F8, 0);
    ASSERT_DIALOGS_DONE();
    ASSERT_PROBE_OK();

    ASSERT_TRUE(exists_in(g_left, "a"));
    ASSERT_EQ(g_modal_depth, 0);
}

TEST(nothing_else_starts_while_a_batch_copy_waits_on_a_conflict)
{
    reset_fixture();
    reset_probe();
    char existing[PATH_MAX];
    fixture_path(existing, g_right, "b");
    write_file(existing, "old b");
    panel_load(&g_panel[1], g_right);
    mark_name(&g_panel[0], "a");
    mark_name(&g_panel[0], "b");

    static const DialogStep steps[] = {
        {"File exists", "already exists", NULL, probe_everything_blocked, "Skip"},
    };
    expect_dialogs(steps, G_N_ELEMENTS(steps));
    press_key(GDK_KEY_F5, 0);
    ASSERT_DIALOGS_DONE();
    ASSERT_PROBE_OK();

    ASSERT_STR_EQ(read_back(g_right, "a"), "a");
    ASSERT_STR_EQ(read_back(g_right, "b"), "old b");
    /* Skip completes the batch, so the marks are cleared. */
    ASSERT_EQ(gui_mark_count(&g_panel[0]), 0);
    ASSERT_EQ(g_modal_depth, 0);
}

/* The overwrite prompt's close response is Abort, never Overwrite. */
TEST(closing_the_window_aborts_a_batch_at_an_overwrite_prompt)
{
    reset_fixture();
    reset_probe();
    char existing[PATH_MAX];
    fixture_path(existing, g_right, "b");
    write_file(existing, "old b");
    panel_load(&g_panel[1], g_right);
    mark_name(&g_panel[0], "b");
    mark_name(&g_panel[0], "c");

    static const DialogStep steps[] = {
        {"File exists", "already exists", NULL, probe_close_window, NULL},
    };
    expect_dialogs(steps, G_N_ELEMENTS(steps));
    press_key(GDK_KEY_F5, 0);
    ASSERT_DIALOGS_DONE();
    ASSERT_PROBE_OK();

    ASSERT_STR_EQ(read_back(g_right, "b"), "old b");
    ASSERT_FALSE(exists_in(g_right, "c"));
    /* An aborted batch keeps its marks for a retry. */
    ASSERT_EQ(gui_mark_count(&g_panel[0]), 2);
    ASSERT_EQ(g_modal_depth, 0);
}

TEST(nothing_else_starts_while_the_editor_is_open)
{
    reset_fixture();
    reset_probe();
    select_name(&g_panel[0], "a");
    const DialogStep steps[] = {PLEASE_WAIT};
    expect_dialogs(steps, G_N_ELEMENTS(steps));
    g_timeout_add(100, probe_during_pump, NULL);
    press_key(GDK_KEY_F3, 0);
    ASSERT_DIALOGS_DONE();
    ASSERT_PROBE_OK();

    ASSERT_STR_EQ(read_back(g_left, "a"), "aran\n");
    ASSERT_EQ(g_modal_depth, 0);
    ASSERT_EQ(g_quit_requests, 0);
}

TEST(nothing_else_starts_while_a_shell_command_runs)
{
    reset_fixture();
    reset_probe();
    char out[PATH_MAX];
    fixture_path(out, g_left, "c");
    char command[PATH_MAX * 2 + 8];
    snprintf(command, sizeof(command), "'%s' '%s'", g_wait_script, out);
    gtk_editable_set_text(GTK_EDITABLE(g_shell_entry), command);

    const DialogStep steps[] = {PLEASE_WAIT};
    expect_dialogs(steps, G_N_ELEMENTS(steps));
    g_timeout_add(100, probe_during_pump, NULL);
    on_shell_entry_activate(GTK_ENTRY(g_shell_entry), NULL);
    ASSERT_DIALOGS_DONE();
    ASSERT_PROBE_OK();

    ASSERT_STR_EQ(read_back(g_left, "c"), "cran\n");
    ASSERT_STR_EQ(gtk_editable_get_text(GTK_EDITABLE(g_shell_entry)), "");
    ASSERT_EQ(g_modal_depth, 0);
    ASSERT_EQ(g_quit_requests, 0);
}

TEST(nothing_else_starts_while_a_program_runs)
{
    reset_fixture();
    reset_probe();
    char program[PATH_MAX];
    fixture_path(program, g_left, "prog");
    char script[PATH_MAX + sizeof(WAIT_SCRIPT_FMT)];
    snprintf(script, sizeof(script), WAIT_SCRIPT_FMT, g_flag);
    write_file(program, script);
    chmod(program, 0755);
    panel_load(&g_panel[0], g_left);

    const DialogStep steps[] = {
        {"Run", "Run \"prog\"?", NULL, NULL, "Run"},
        PLEASE_WAIT,
    };
    expect_dialogs(steps, G_N_ELEMENTS(steps));
    g_timeout_add(300, probe_during_pump, NULL);
    int index = store_index_of(&g_panel[0], "prog");
    ASSERT_TRUE(index >= 0);
    on_item_activated(NULL, (guint)index, &g_panel[0]);
    ASSERT_DIALOGS_DONE();
    ASSERT_PROBE_OK();
    ASSERT_EQ(g_modal_depth, 0);
}

TEST(marks_survive_a_reload_and_batch_delete_and_undo_round_trip)
{
    reset_fixture();
    mark_name(&g_panel[0], "a");
    mark_name(&g_panel[0], "sub");
    /* ".." can't be marked. */
    mark_name(&g_panel[0], "..");
    ASSERT_EQ(gui_mark_count(&g_panel[0]), 2);
    panel_load(&g_panel[0], g_panel[0].path);
    ASSERT_EQ(gui_mark_count(&g_panel[0]), 2);

    static const DialogStep delete_steps[] = {
        {"Delete", "Delete 2 items?", NULL, NULL, "Delete"},
    };
    expect_dialogs(delete_steps, G_N_ELEMENTS(delete_steps));
    press_key(GDK_KEY_F8, 0);
    ASSERT_DIALOGS_DONE();
    ASSERT_FALSE(exists_in(g_left, "a"));
    ASSERT_FALSE(exists_in(g_left, "sub"));
    ASSERT_TRUE(exists_in(g_left, "b"));
    ASSERT_EQ(gui_mark_count(&g_panel[0]), 0);
    char trash[PATH_MAX];
    fixture_path(trash, g_home, ".local/share/Trash/files");
    ASSERT_TRUE(exists_in(trash, "a"));
    ASSERT_TRUE(exists_in(trash, "sub"));

    static const DialogStep undo_steps[] = {
        {"Undo", "Restored 2 items", NULL, NULL, "OK"},
    };
    expect_dialogs(undo_steps, G_N_ELEMENTS(undo_steps));
    press_key(GDK_KEY_F9, 0);
    ASSERT_DIALOGS_DONE();
    ASSERT_STR_EQ(read_back(g_left, "a"), "a");
    char sub[PATH_MAX];
    fixture_path(sub, g_left, "sub");
    ASSERT_TRUE(is_dir(sub));
    ASSERT_TRUE(store_index_of(&g_panel[0], "a") >= 0);
}

TEST(reload_keeps_the_selection_by_name)
{
    reset_fixture();
    select_name(&g_panel[0], "c");
    char path[PATH_MAX];
    fixture_path(path, g_left, "aa");
    write_file(path, "x");
    panel_load(&g_panel[0], g_panel[0].path);
    ASSERT_STR_EQ(selected_name(&g_panel[0]), "c");
}

static gboolean theme_provider_gone(void)
{
    return g_theme_css_provider == NULL;
}

static gboolean theme_provider_present(void)
{
    return g_theme_css_provider != NULL;
}

static void save_gui_theme(const char *theme)
{
    Config cfg = g_cfg;
    snprintf(cfg.gui_theme, sizeof(cfg.gui_theme), "%s", theme);
    char error[PATH_MAX + 64] = "";
    config_save(&cfg, error, sizeof(error));
}

/* The live theme hook: SIGUSR1 re-reads tfm.ini, so switching gui_theme
 * to "system" must drop the omarchy CSS and forced color scheme, and
 * switching back must restore them. */
TEST(sigusr1_theme_toggle_omarchy_to_system_and_back)
{
    ASSERT_TRUE(g_theme_css_provider != NULL);
    AdwStyleManager *style = adw_style_manager_get_default();
    ASSERT_EQ(adw_style_manager_get_color_scheme(style), ADW_COLOR_SCHEME_FORCE_DARK);

    save_gui_theme("system");
    raise(SIGUSR1);
    ASSERT_TRUE(pump_until(theme_provider_gone, 2000));
    ASSERT_EQ(adw_style_manager_get_color_scheme(style), ADW_COLOR_SCHEME_DEFAULT);

    save_gui_theme("omarchy");
    raise(SIGUSR1);
    ASSERT_TRUE(pump_until(theme_provider_present, 2000));
    ASSERT_EQ(adw_style_manager_get_color_scheme(style), ADW_COLOR_SCHEME_FORCE_DARK);

    /* A colors.toml without an accent falls back to system theming. */
    write_file(g_colors_toml, "mode = \"dark\"\n");
    raise(SIGUSR1);
    ASSERT_TRUE(pump_until(theme_provider_gone, 2000));
    write_file(g_colors_toml, COLORS_TOML);
    raise(SIGUSR1);
    ASSERT_TRUE(pump_until(theme_provider_present, 2000));
}

/* on_shutdown() must never persist an empty panel path over the
 * configured one. */
TEST(shutdown_never_saves_an_empty_panel_path)
{
    reset_fixture();
    char saved_right[PATH_MAX];
    snprintf(saved_right, sizeof(saved_right), "%s", g_panel[1].path);
    snprintf(g_cfg.right_path, sizeof(g_cfg.right_path), "%s", "/configured/right");
    g_panel[1].path[0] = '\0';

    on_shutdown(NULL, NULL);
    snprintf(g_panel[1].path, sizeof(g_panel[1].path), "%s", saved_right);

    Config reloaded;
    char error[PATH_MAX + 64] = "";
    config_load(&reloaded, error, sizeof(error));
    ASSERT_STR_EQ(error, "");
    ASSERT_STR_EQ(reloaded.left_path, g_left);
    ASSERT_STR_EQ(reloaded.right_path, "/configured/right");
}

/* --- runner -------------------------------------------------------------- */

static int g_exit_status = 1;

static gboolean window_mapped(void)
{
    return g_window != NULL && gtk_widget_get_mapped(GTK_WIDGET(g_window));
}

static gboolean run_all_tests(gpointer user_data)
{
    GApplication *application = user_data;
    if (!pump_until(window_mapped, 5000)) {
        fprintf(stderr, "FAIL: main window never mapped\n");
        g_quit_passthrough = 1;
        g_application_quit(application);
        return G_SOURCE_REMOVE;
    }

    guint responder = g_timeout_add(10, responder_poll, NULL);
    g_source_set_can_recurse(g_main_context_find_source_by_id(NULL, responder), TRUE);

    TFM_RUN(startup_lists_both_panels_and_applies_the_omarchy_theme);
    TFM_RUN(same_dir_rename_over_existing_file_prompts_and_skip_keeps_both);
    TFM_RUN(same_dir_rename_over_existing_file_overwrites_when_confirmed);
    TFM_RUN(nothing_else_starts_while_a_confirm_dialog_is_open);
    TFM_RUN(closing_the_window_cancels_a_delete_confirmation);
    TFM_RUN(nothing_else_starts_while_a_batch_copy_waits_on_a_conflict);
    TFM_RUN(closing_the_window_aborts_a_batch_at_an_overwrite_prompt);
    TFM_RUN(nothing_else_starts_while_the_editor_is_open);
    TFM_RUN(nothing_else_starts_while_a_shell_command_runs);
    TFM_RUN(nothing_else_starts_while_a_program_runs);
    TFM_RUN(marks_survive_a_reload_and_batch_delete_and_undo_round_trip);
    TFM_RUN(reload_keeps_the_selection_by_name);
    TFM_RUN(sigusr1_theme_toggle_omarchy_to_system_and_back);
    TFM_RUN(shutdown_never_saves_an_empty_panel_path);
    g_exit_status = TFM_SUMMARY();

    g_source_remove(responder);
    g_quit_passthrough = 1;
    g_application_quit(application);
    return G_SOURCE_REMOVE;
}

static void test_activate(GtkApplication *application, gpointer user_data)
{
    activate(application, user_data);
    g_idle_add(run_all_tests, application);
}

int main(void)
{
    /* A hung nested loop (a dialog nobody answers) must fail the run, not
     * stall CI until its own timeout. */
    alarm(120);

    const char *home = getenv("HOME");
    const char *backend = getenv("GDK_BACKEND");
    if (home == NULL || backend == NULL || strcmp(backend, "broadway") != 0) {
        fprintf(stderr, "Run this via tests/run_gui_tests.sh (make test-gui), not directly:\n"
                        "it needs an isolated $HOME and the broadway backend.\n");
        return 2;
    }
    snprintf(g_home, sizeof(g_home), "%s", home);
    fixture_path(g_left, g_home, "left");
    fixture_path(g_right, g_home, "right");
    fixture_path(g_flag, g_home, "flag");
    fixture_path(g_wait_script, g_home, "wait.sh");

    char script[PATH_MAX + sizeof(WAIT_SCRIPT_FMT)];
    snprintf(script, sizeof(script), WAIT_SCRIPT_FMT, g_flag);
    write_file(g_wait_script, script);
    chmod(g_wait_script, 0755);
    setenv("EDITOR", g_wait_script, 1);

    char theme_dir[PATH_MAX];
    fixture_path(theme_dir, g_home, ".local/state/omarchy/current/theme");
    g_mkdir_with_parents(theme_dir, 0755);
    fixture_path(g_colors_toml, theme_dir, "colors.toml");
    write_file(g_colors_toml, COLORS_TOML);

    mkdir(g_left, 0755);
    mkdir(g_right, 0755);

    setlocale(LC_ALL, "");
    config_load(&g_cfg, g_pending_config_load_error, sizeof(g_pending_config_load_error));
    g_pending_config_load_error[0] = '\0'; /* no tfm.ini yet */
    snprintf(g_cfg.left_path, sizeof(g_cfg.left_path), "%s", g_left);
    snprintf(g_cfg.right_path, sizeof(g_cfg.right_path), "%s", g_right);
    snprintf(g_cfg.gui_theme, sizeof(g_cfg.gui_theme), "omarchy");
    g_default_font_size = read_terminal_font_size();
    g_font_size = g_default_font_size;
    g_unix_signal_add(SIGUSR1, on_sigusr1, NULL);

    /* Own ID, non-unique: never hands off to (or collides with) a real
     * tfm-gui over D-Bus. */
    AdwApplication *app = adw_application_new("de.taiku.tfm.guitest", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(test_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
    g_application_run(G_APPLICATION(app), 0, NULL);
    g_object_unref(app);
    return g_exit_status;
}
