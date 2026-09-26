#ifndef TFM_CONFIG_H
#define TFM_CONFIG_H

#include "tfm_common.h"

typedef struct {
    char left_path[PATH_MAX];
    char right_path[PATH_MAX];
    char border_color[32];        /* outer border: "system" or a color name, see screen.h */
    char panel_border_color[32];  /* border of the two panels */
    char text_color[32];          /* directory listing text color */
    char cursor_color[32];        /* selection bar color */
    char dir_color[32];           /* directory entry name color */
    char mark_color[32];          /* marked entries and the mark summary */
    char icons[16];                /* "omarchy" (Nerd Font icons) or "off" */
    char gui_theme[16];            /* tfm-gui only: "omarchy" (take accent/
                                     * light-dark from the active Omarchy theme)
                                     * or "system" (default libadwaita/GNOME
                                     * settings). "omarchy" falls back to
                                     * "system" automatically when no Omarchy
                                     * theme is found. */
} Config;

/* Fills cfg with sane defaults ($HOME as start dir for both panels,
 * falling back to "/" if $HOME is unset). */
void config_set_defaults(Config *cfg);

/* Loads config from ~/.tfm/tfm.ini, or defaults if the file doesn't exist.
 * error_msg is optional (NULL/0 to ignore): on return it is "" if the file
 * loaded or was missing (the normal first run), or a reason such as
 * "Cannot open ...: Permission denied" if an existing file couldn't be
 * read. */
void config_load(Config *cfg, char *error_msg, size_t error_msg_size);

/* Saves config to ~/.tfm/tfm.ini, creating ~/.tfm if needed. error_msg/
 * error_msg_size are optional (pass NULL/0 to ignore) - on return,
 * error_msg[0] is '\0' on success or a human-readable reason for the
 * failure (naming the real syscall failure) otherwise. */
void config_save(const Config *cfg, char *error_msg, size_t error_msg_size);

#endif
