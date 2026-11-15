#include "input.h"

#include <stdint.h>

#define KEY_QUEUE_CAPACITY 64u
#define TEXT_QUEUE_CAPACITY 128u

static uint8_t extended_prefix;
static uint8_t control_down;
static uint8_t y_down;
static volatile uint8_t key_queue[KEY_QUEUE_CAPACITY];
static volatile uint8_t queue_read;
static volatile uint8_t queue_write;
static volatile char text_queue[TEXT_QUEUE_CAPACITY];
static volatile uint8_t text_read;
static volatile uint8_t text_write;
static uint8_t text_shift_down;
static uint8_t text_caps_lock;
static uint8_t text_extended_prefix;
static uint8_t text_control_down;

static char text_for_scancode(uint8_t scan_code, uint8_t shifted) {
    static const char plain[][2] = {
        {0x02, '1'}, {0x03, '2'}, {0x04, '3'}, {0x05, '4'},
        {0x06, '5'}, {0x07, '6'}, {0x08, '7'}, {0x09, '8'},
        {0x0A, '9'}, {0x0B, '0'}, {0x0C, '-'}, {0x0D, '='},
        {0x10, 'q'}, {0x11, 'w'}, {0x12, 'e'}, {0x13, 'r'},
        {0x14, 't'}, {0x15, 'y'}, {0x16, 'u'}, {0x17, 'i'},
        {0x18, 'o'}, {0x19, 'p'}, {0x1A, '['}, {0x1B, ']'},
        {0x1E, 'a'}, {0x1F, 's'}, {0x20, 'd'}, {0x21, 'f'},
        {0x22, 'g'}, {0x23, 'h'}, {0x24, 'j'}, {0x25, 'k'},
        {0x26, 'l'}, {0x27, ';'}, {0x28, '\''}, {0x29, '`'},
        {0x2B, '\\'}, {0x2C, 'z'}, {0x2D, 'x'}, {0x2E, 'c'},
        {0x2F, 'v'}, {0x30, 'b'}, {0x31, 'n'}, {0x32, 'm'},
        {0x33, ','}, {0x34, '.'}, {0x35, '/'}, {0x39, ' '}
    };
    static const char shifted_values[] = "!@#$%^&*()_+QWERTYUIOP{}ASDFGHJKL:\"~|ZXCVBNM<>?";
    uint32_t index;
    uint32_t letter_index = 0u;
    for (index = 0u; index < sizeof(plain) / sizeof(plain[0]); ++index) {
        char value;
        if ((uint8_t)plain[index][0] != scan_code) continue;
        value = plain[index][1];
        if (value >= 'a' && value <= 'z') {
            if ((shifted != 0u) != (text_caps_lock != 0u))
                value = (char)(value - 'a' + 'A');
            return value;
        }
        if (value != ' ') {
            uint32_t n;
            for (n = 0u; n < index; ++n)
                if (plain[n][1] < 'a' || plain[n][1] > 'z')
                    ++letter_index;
            if (shifted != 0u && letter_index < sizeof(shifted_values) - 1u)
                return shifted_values[letter_index];
        }
        return value;
    }
    return 0;
}

static void queue_text_char(char character) {
    uint8_t write = text_write;
    uint8_t next = (uint8_t)((write + 1u) & (TEXT_QUEUE_CAPACITY - 1u));
    if (next == text_read) return;
    text_queue[write] = character;
    text_write = next;
}

static void handle_text_scancode(uint8_t scan_code) {
    uint8_t released = (uint8_t)(scan_code & 0x7Fu);
    char character;
    if (scan_code == 0xE0u) {
        text_extended_prefix = 1u;
        return;
    }
    if (text_extended_prefix != 0u) {
        text_extended_prefix = 0u;
        return;
    }
    if ((scan_code & 0x80u) != 0u) {
        if (released == 0x1Du) text_control_down = 0u;
        if (released == 0x2Au || released == 0x36u) text_shift_down = 0u;
        return;
    }
    if (scan_code == 0x1Du) {
        text_control_down = 1u;
        return;
    }
    if (scan_code == 0x2Au || scan_code == 0x36u) {
        text_shift_down = 1u;
        return;
    }
    if (scan_code == 0x3Au) {
        text_caps_lock ^= 1u;
        return;
    }
    if (scan_code == 0x0Eu) {
        if (text_control_down == 0u) queue_text_char('\b');
        return;
    }
    if (text_control_down != 0u) return;
    character = text_for_scancode(scan_code, text_shift_down);
    if (character != 0) queue_text_char(character);
}

static enum key_code translate_scancode(uint8_t scan_code) {
    uint8_t extended;
    if (scan_code == 0xE0u) {
        extended_prefix = 1u;
        return KEY_NONE;
    }
    extended = extended_prefix;
    extended_prefix = 0u;
    if ((scan_code & 0x80u) != 0u) {
        uint8_t released = (uint8_t)(scan_code & 0x7Fu);
        if (released == 0x1Du) control_down = 0u;
        if (!extended && released == 0x15u) y_down = 0u;
        return KEY_NONE;
    }
    if (scan_code == 0x1Du) {
        control_down = 1u;
        return KEY_NONE;
    }
    if (!extended && scan_code == 0x15u) {
        if (control_down != 0u && y_down == 0u) {
            y_down = 1u;
            return KEY_TASK_TOGGLE;
        }
        y_down = 1u;
        return KEY_NONE;
    }
    if (extended != 0u) {
        switch (scan_code) {
            case 0x48u: return KEY_UP;
            case 0x50u: return KEY_DOWN;
            case 0x4Bu: return KEY_LEFT;
            case 0x4Du: return KEY_RIGHT;
            default: return KEY_NONE;
        }
    }
    switch (scan_code) {
        case 0x01u: return KEY_ESCAPE;
        case 0x1Cu: return KEY_ENTER;
        case 0x02u: return KEY_1;
        case 0x03u: return KEY_2;
        case 0x04u: return KEY_3;
        case 0x05u: return KEY_4;
        case 0x06u: return KEY_5;
        case 0x07u: return KEY_6;
        case 0x39u: return KEY_SPACE;
        case 0x0Fu: return KEY_TAB;
        case 0x0Eu: return KEY_BACKSPACE;
        default: return KEY_NONE;
    }
}

enum key_code keyboard_poll(void) {
    uint8_t read = queue_read;
    enum key_code key;
    if (read == queue_write) return KEY_NONE;
    key = (enum key_code)key_queue[read];
    queue_read = (uint8_t)((read + 1u) & (KEY_QUEUE_CAPACITY - 1u));
    return key;
}

char keyboard_text_poll(void) {
    uint8_t read = text_read;
    char character;
    if (read == text_write) return 0;
    character = text_queue[read];
    text_read = (uint8_t)((read + 1u) & (TEXT_QUEUE_CAPACITY - 1u));
    return character;
}

void keyboard_handle_scancode(uint8_t scan_code) {
    handle_text_scancode(scan_code);
    enum key_code key = translate_scancode(scan_code);
    uint8_t write;
    uint8_t next;
    if (key == KEY_NONE) return;
    write = queue_write;
    next = (uint8_t)((write + 1u) & (KEY_QUEUE_CAPACITY - 1u));
    if (next == queue_read) return; /* Drop the newest key on overflow. */
    key_queue[write] = (uint8_t)key;
    queue_write = next;
}

int keyboard_self_check(void) {
    int valid;
    queue_read = 0u;
    queue_write = 0u;
    text_read = 0u;
    text_write = 0u;
    extended_prefix = 0u;
    text_extended_prefix = 0u;
    control_down = 0u;
    y_down = 0u;
    text_shift_down = 0u;
    text_caps_lock = 0u;
    text_control_down = 0u;
    keyboard_handle_scancode(0x02u); /* Set-1 make code for 1. */
    keyboard_handle_scancode(0x82u); /* Break codes must not enqueue. */
    keyboard_handle_scancode(0xE0u);
    keyboard_handle_scancode(0x4Bu); /* Extended left arrow. */
    keyboard_handle_scancode(0x06u); /* Set-1 make code for 5. */
    valid = keyboard_poll() == KEY_1 && keyboard_poll() == KEY_LEFT &&
            keyboard_poll() == KEY_5 &&
            keyboard_poll() == KEY_NONE;
    keyboard_handle_scancode(0x1Du); /* Left Ctrl make. */
    keyboard_handle_scancode(0x15u); /* Y make toggles once. */
    keyboard_handle_scancode(0x15u); /* Typematic repeat is suppressed. */
    keyboard_handle_scancode(0x95u); /* Y break rearms the shortcut. */
    keyboard_handle_scancode(0x15u);
    valid = valid && keyboard_poll() == KEY_TASK_TOGGLE &&
            keyboard_poll() == KEY_TASK_TOGGLE && keyboard_poll() == KEY_NONE;
    queue_read = 0u;
    queue_write = 0u;
    text_read = 0u;
    text_write = 0u;
    extended_prefix = 0u;
    text_extended_prefix = 0u;
    control_down = 0u;
    y_down = 0u;
    text_shift_down = 0u;
    text_caps_lock = 0u;
    text_control_down = 0u;
    return valid;
}
