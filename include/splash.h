#ifndef SPLASH_H
#define SPLASH_H

/* Shows an animated splash screen: title and subtitle scroll in from
 * the right, hold briefly with a cycling rainbow color, then end with
 * a screen clear. Blocking - returns once the animation finishes.
 * Standalone module, no project dependencies - splash.h/splash.c can
 * be copied as-is into other C projects.
 *
 * Usage: call as the first line in main():
 *     splash_show("TFM", "Taiku File Manager");
 */
void splash_show(const char *title, const char *subtitle);

#endif
