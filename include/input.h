#ifndef TFM_INPUT_H
#define TFM_INPUT_H

typedef enum {
    KEY_NONE = 0,
    KEY_CHAR,
    KEY_ESC,
    KEY_F1,
    KEY_F2,
    KEY_F3,
    KEY_F4,
    KEY_F5,
    KEY_F6,
    KEY_F7,
    KEY_F8,
    KEY_F9,
    KEY_F10,
    KEY_F11,
    KEY_F12,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_UNKNOWN
} KeyType;

typedef struct {
    KeyType type;
    char ch; /* valid only when type == KEY_CHAR */
} KeyEvent;

/* Puts the terminal into raw mode (no echo, no line buffering). Must be
 * called before the first input_read_key() use. */
void input_enable_raw_mode(void);

/* Restores the original terminal configuration. */
void input_disable_raw_mode(void);

/* Blocks until a key is pressed and returns it. */
KeyEvent input_read_key(void);

/* Returns 1 and clears the internal flag if a SIGWINCH occurred since
 * the last call, else 0. Lets both the main loop and blocking popup
 * dialogs (screen.c) react to a resize. */
int input_consume_resize_flag(void);

/* Like input_read_key(), for "wait for any key" call sites (popup
 * confirmation etc.) that ignore the return value: also consumes a
 * resize flag set by SIGWINCH during the wait, so it isn't left for
 * main.c's loop to misread as "just a resize" (see
 * input_consume_resize_flag()). */
void input_wait_any_key(void);

#endif
