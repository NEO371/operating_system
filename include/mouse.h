#ifndef MINI_OS_MOUSE_H
#define MINI_OS_MOUSE_H

#include <stdint.h>

struct mouse_event {
    int16_t delta_x;
    int16_t delta_y;
    uint8_t buttons;
};

int mouse_init(void);
void mouse_handle_byte(uint8_t byte);
int mouse_poll(struct mouse_event *event);
int mouse_self_check(void);

#endif
