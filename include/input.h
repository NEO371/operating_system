#ifndef MINI_OS_INPUT_H
#define MINI_OS_INPUT_H

#include <stdint.h>

enum key_code {
    KEY_NONE = 0,
    KEY_ESCAPE,
    KEY_ENTER,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_1,
    KEY_2,
    KEY_3,
    KEY_4,
    KEY_5,
    KEY_6,
    KEY_SPACE,
    KEY_TAB,
    KEY_BACKSPACE,
    KEY_TASK_TOGGLE
};

/* Non-blocking PS/2 keyboard poll. Returns a key only for a make event. */
enum key_code keyboard_poll(void);
char keyboard_text_poll(void);
int keyboard_self_check(void);
void keyboard_handle_scancode(uint8_t scan_code);

#endif
