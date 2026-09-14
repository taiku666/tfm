#ifndef TFM_OMARCHY_THEME_H
#define TFM_OMARCHY_THEME_H

/* Reads the accent/background colors of the currently active Omarchy
 * theme. Omarchy themes terminals/editors/browsers but not GTK4/libadwaita
 * apps, so tfm-gui needs its own loader to fill that gap. */
typedef struct {
    int is_dark;
    char accent[16];
    char background[16];
    char foreground[16];
    char dark_background[16];
    char selection[16];
} OmarchyThemeColors;

/* Loads the active Omarchy theme's colors into *out. Returns 1 on
 * success (accent found; "mode" is optional and defaults to light),
 * 0 if the file is missing or unusable (e.g. not an Omarchy system) -
 * callers should fall back to standard libadwaita/system theming rather
 * than erroring. */
int omarchy_theme_load(OmarchyThemeColors *out);

/* Reads the active window-corner "rounding" value (in pixels) from the
 * Hyprland/Omarchy look'n'feel config, so tfm-gui's panel border (see
 * .tfm-panel-active in gui_main.c) can match the window manager's
 * corners. Returns 0 if no active (uncommented) rounding value was
 * found, in which case the caller should apply no extra rounding. */
int omarchy_hypr_corner_rounding(void);

#endif
