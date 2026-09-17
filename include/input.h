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
    KEY_HOME,
    KEY_END,
    KEY_PGUP,
    KEY_PGDN,
    KEY_INSERT,
    KEY_DELETE,
    KEY_SHIFT_TAB,
    KEY_UNKNOWN,
    KEY_EOF /* stdin closed (e.g. `tfm < /dev/null`) - not a recoverable
             * condition, unlike KEY_NONE's transient "no key yet". */
} KeyType;

typedef struct {
    KeyType type;
    char ch;    /* valid only when type == KEY_CHAR */
    int shift;  /* 1 if Shift was held - currently only ever set for
                 * KEY_F8 (Shift+F8, the permanent-delete bypass); 0 for
                 * every other KeyType. */
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

#endif
