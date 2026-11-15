#include "mouse.h"

#include <stdint.h>

#define MOUSE_QUEUE_CAPACITY 32u
#define PS2_STATUS_PORT 0x64u
#define PS2_COMMAND_PORT 0x64u
#define PS2_DATA_PORT 0x60u

static volatile struct mouse_event queue[MOUSE_QUEUE_CAPACITY];
static volatile uint8_t queue_read;
static volatile uint8_t queue_write;
static uint8_t packet[3];
static uint8_t packet_index;
static uint8_t ready;

static uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static int wait_input_clear(void) {
    uint32_t timeout;
    for (timeout = 0u; timeout < 100000u; ++timeout)
        if ((inb(PS2_STATUS_PORT) & 0x02u) == 0u) return 1;
    return 0;
}

static int wait_output_full(void) {
    uint32_t timeout;
    for (timeout = 0u; timeout < 100000u; ++timeout)
        if ((inb(PS2_STATUS_PORT) & 0x01u) != 0u) return 1;
    return 0;
}

static int send_controller_command(uint8_t command) {
    if (!wait_input_clear()) return 0;
    outb(PS2_COMMAND_PORT, command);
    return 1;
}

static int send_mouse_command(uint8_t command) {
    if (!send_controller_command(0xD4u) || !wait_input_clear()) return 0;
    outb(PS2_DATA_PORT, command);
    if (!wait_output_full()) return 0;
    return inb(PS2_DATA_PORT) == 0xFAu;
}

int mouse_init(void) {
    uint8_t command_byte;
    ready = 0u;
    queue_read = 0u;
    queue_write = 0u;
    packet_index = 0u;
    if (!send_controller_command(0xA8u) ||
        !send_controller_command(0x20u) || !wait_output_full()) return 0;
    command_byte = inb(PS2_DATA_PORT);
    command_byte = (uint8_t)((command_byte | 0x02u) & (uint8_t)~0x20u);
    if (!send_controller_command(0x60u) || !wait_input_clear()) return 0;
    outb(PS2_DATA_PORT, command_byte);
    if (!send_mouse_command(0xF6u) || !send_mouse_command(0xF4u)) return 0;
    ready = 1u;
    return 1;
}

static void enqueue_packet(void) {
    uint8_t write;
    uint8_t next;
    uint8_t flags = packet[0];
    struct mouse_event event;
    if ((flags & 0xC0u) != 0u) return;
    event.delta_x = (int8_t)packet[1];
    event.delta_y = (int16_t)-(int8_t)packet[2];
    event.buttons = flags & 0x07u;
    write = queue_write;
    next = (uint8_t)((write + 1u) & (MOUSE_QUEUE_CAPACITY - 1u));
    if (next == queue_read) return;
    queue[write].delta_x = event.delta_x;
    queue[write].delta_y = event.delta_y;
    queue[write].buttons = event.buttons;
    queue_write = next;
}

void mouse_handle_byte(uint8_t byte) {
    if (ready == 0u) return;
    if (packet_index == 0u && (byte & 0x08u) == 0u) return;
    packet[packet_index++] = byte;
    if (packet_index == sizeof(packet)) {
        packet_index = 0u;
        enqueue_packet();
    }
}

int mouse_poll(struct mouse_event *event) {
    uint8_t read;
    if (event == 0) return 0;
    read = queue_read;
    if (read == queue_write) return 0;
    event->delta_x = queue[read].delta_x;
    event->delta_y = queue[read].delta_y;
    event->buttons = queue[read].buttons;
    queue_read = (uint8_t)((read + 1u) & (MOUSE_QUEUE_CAPACITY - 1u));
    return 1;
}

int mouse_self_check(void) {
    int valid;
    packet_index = 0u;
    queue_read = 0u;
    queue_write = 0u;
    ready = 1u;
    mouse_handle_byte(0x08u);
    mouse_handle_byte(0xFEu);
    mouse_handle_byte(0x02u);
    {
        struct mouse_event event;
        valid = mouse_poll(&event) && event.delta_x == -2 &&
                event.delta_y == -2 && event.buttons == 0u && !mouse_poll(&event);
    }
    ready = 0u;
    packet_index = 0u;
    queue_read = 0u;
    queue_write = 0u;
    return valid;
}
