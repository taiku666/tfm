#define _POSIX_C_SOURCE 199309L

#include "splash.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define FONT_W 5
#define FONT_H 5
#define BLOCK "\xe2\x96\x88" /* █ */
#define ANSI_RESET "\x1b[0m"
#define MAX_BIG_TEXT 512

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void get_term_size(int *rows, int *cols)
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

static const char *RAINBOW[] = {
    "\x1b[91m", "\x1b[93m", "\x1b[92m", "\x1b[96m", "\x1b[94m", "\x1b[95m",
};
#define RAINBOW_N (int)(sizeof(RAINBOW) / sizeof(RAINBOW[0]))

/* 5x5 block font, '#' = pixel on, '.' = pixel off. Only covers the
 * letters needed for "TFM"/"Taiku File Manager"; unknown characters
 * fall back to a single center dot (see get_glyph). */
typedef struct {
    char ch;
    const char *rows[FONT_H];
} Glyph;

static const Glyph GLYPHS[] = {
    {'A', {".###.", "#...#", "#####", "#...#", "#...#"}},
    {'E', {"#####", "#....", "###..", "#....", "#####"}},
    {'F', {"#####", "#....", "###..", "#....", "#...."}},
    {'G', {".###.", "#....", "#.###", "#...#", ".###."}},
    {'I', {"#####", "..#..", "..#..", "..#..", "#####"}},
    {'K', {"#...#", "#..#.", "###..", "#..#.", "#...#"}},
    {'L', {"#....", "#....", "#....", "#....", "#####"}},
    {'M', {"#...#", "##.##", "#.#.#", "#...#", "#...#"}},
    {'N', {"#...#", "##..#", "#.#.#", "#..##", "#...#"}},
    {'R', {"####.", "#...#", "####.", "#..#.", "#...#"}},
    {'T', {"#####", "..#..", "..#..", "..#..", "..#.."}},
    {'U', {"#...#", "#...#", "#...#", "#...#", "#####"}},
    {' ', {".....", ".....", ".....", ".....", "....."}},
};
#define GLYPH_COUNT (int)(sizeof(GLYPHS) / sizeof(GLYPHS[0]))

static const char *const *get_glyph(char ch)
{
    char upper = (char)toupper((unsigned char)ch);
    for (int i = 0; i < GLYPH_COUNT; i++) {
        if (GLYPHS[i].ch == upper) {
            return GLYPHS[i].rows;
        }
    }
    /* Unknown character: show a dot instead of a blank gap. */
    static const char *fallback[FONT_H] = {".....", ".....", "..#..", ".....", "....."};
    return fallback;
}

static int build_big_text(const char *text, char rows_out[FONT_H][MAX_BIG_TEXT])
{
    int n = (int)strlen(text);
    int width = n > 0 ? n * (FONT_W + 1) - 1 : 0;
    if (width >= MAX_BIG_TEXT) {
        width = MAX_BIG_TEXT - 1;
    }

    for (int r = 0; r < FONT_H; r++) {
        memset(rows_out[r], '.', (size_t)width);
        rows_out[r][width] = '\0';
    }

    for (int i = 0; i < n; i++) {
        int offset = i * (FONT_W + 1);
        if (offset + FONT_W > width) {
            break;
        }
        const char *const *glyph = get_glyph(text[i]);
        for (int r = 0; r < FONT_H; r++) {
            memcpy(&rows_out[r][offset], glyph[r], FONT_W);
        }
    }

    return width;
}

static void draw_big_row(int row, int cols, const char *pattern, int width, int text_col, int color_phase)
{
    printf("\x1b[%d;1H", row);
    for (int c = 0; c < cols; c++) {
        int p = c - text_col;
        if (p >= 0 && p < width && pattern[p] == '#') {
            int char_index = p / (FONT_W + 1);
            printf("%s" BLOCK ANSI_RESET, RAINBOW[(color_phase + char_index) % RAINBOW_N]);
        } else {
            printf(" ");
        }
    }
}

/* Redraws the full row width each time instead of clearing separately,
 * to avoid flicker. */
static void draw_plain_row(int row, int cols, const char *text, int text_col)
{
    char *line = malloc((size_t)cols + 1);
    if (line == NULL) {
        return;
    }

    memset(line, ' ', (size_t)cols);
    line[cols] = '\0';

    int len = (int)strlen(text);
    for (int i = 0; i < len; i++) {
        int pos = text_col + i;
        if (pos >= 0 && pos < cols) {
            line[pos] = text[i];
        }
    }

    printf("\x1b[%d;1H%s", row, line);
    free(line);
}

void splash_show(const char *title, const char *subtitle)
{
    int rows, cols;
    get_term_size(&rows, &cols);

    static char big_rows[FONT_H][MAX_BIG_TEXT];
    int big_width = build_big_text(title, big_rows);

    int subtitle_len = (int)strlen(subtitle);

    int title_top_row = rows / 2 - FONT_H;
    if (title_top_row < 1) {
        title_top_row = 1;
    }
    int subtitle_row = title_top_row + FONT_H + 1;

    int title_target_col = (cols - big_width) / 2;
    int subtitle_target_col = (cols - subtitle_len) / 2;

    printf("\x1b[?25l"); /* hide cursor */
    printf("\x1b[2J");   /* clear screen once */
    fflush(stdout);

    int min_target = title_target_col < subtitle_target_col ? title_target_col : subtitle_target_col;
    int frames_in = cols - min_target;
    if (frames_in < 0) {
        frames_in = 0;
    }

    int color_phase = 0;

    for (int i = 0; i <= frames_in; i++) {
        int title_col = cols - i;
        int subtitle_col = cols - i;
        if (title_col < title_target_col) {
            title_col = title_target_col;
        }
        if (subtitle_col < subtitle_target_col) {
            subtitle_col = subtitle_target_col;
        }

        for (int r = 0; r < FONT_H; r++) {
            draw_big_row(title_top_row + r, cols, big_rows[r], big_width, title_col, color_phase);
        }
        draw_plain_row(subtitle_row, cols, subtitle, subtitle_col);
        fflush(stdout);

        color_phase++;
        sleep_ms(12);

        if (title_col == title_target_col && subtitle_col == subtitle_target_col) {
            break;
        }
    }

    /* hold in place while the rainbow keeps cycling */
    for (int i = 0; i < 42; i++) {
        for (int r = 0; r < FONT_H; r++) {
            draw_big_row(title_top_row + r, cols, big_rows[r], big_width, title_target_col, color_phase);
        }
        fflush(stdout);
        color_phase++;
        sleep_ms(45);
    }

    printf("\x1b[2J\x1b[H"); /* clear screen, cursor to home */
    printf("\x1b[?25h");     /* show cursor */
    fflush(stdout);
}
