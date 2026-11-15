#include "block.h"

#include <stddef.h>

static struct block_device registered_devices[BLOCK_DEVICE_LIMIT];
static uint32_t registered_device_count;

static uint32_t string_length_bounded(const char *text, uint32_t capacity) {
    uint32_t length;
    for (length = 0u; length < capacity && text[length] != '\0'; ++length) { }
    return length;
}

static int names_equal(const char *left, const char *right) {
    uint32_t index;
    for (index = 0u; index < BLOCK_DEVICE_NAME_CAPACITY; ++index) {
        if (left[index] != right[index]) return 0;
        if (left[index] == '\0') return 1;
    }
    return 1;
}

int block_device_register(const struct block_device *device) {
    uint32_t index;
    uint32_t name_length;
    struct block_device *registered;

    if (device == 0 || device->read == 0 || device->sector_count == 0u ||
        device->sector_size < 128u || device->sector_size > 4096u ||
        (device->sector_size & (device->sector_size - 1u)) != 0u ||
        (device->flags != BLOCK_DEVICE_READ_ONLY &&
         device->flags != BLOCK_DEVICE_WRITABLE) ||
        (device->flags == BLOCK_DEVICE_READ_ONLY && device->write != 0) ||
        (device->flags == BLOCK_DEVICE_WRITABLE && device->write == 0)) return 0;
    name_length = string_length_bounded(device->name, BLOCK_DEVICE_NAME_CAPACITY);
    if (name_length == 0u || name_length >= BLOCK_DEVICE_NAME_CAPACITY) return 0;
    for (index = 0u; index < registered_device_count; ++index) {
        if (names_equal(registered_devices[index].name, device->name)) return 0;
    }
    if (registered_device_count >= BLOCK_DEVICE_LIMIT) return 0;

    registered = &registered_devices[registered_device_count++];
    for (index = 0u; index < BLOCK_DEVICE_NAME_CAPACITY; ++index) {
        registered->name[index] = device->name[index];
        if (device->name[index] == '\0') {
            for (++index; index < BLOCK_DEVICE_NAME_CAPACITY; ++index)
                registered->name[index] = '\0';
            break;
        }
    }
    registered->sector_size = device->sector_size;
    registered->sector_count = device->sector_count;
    registered->flags = device->flags;
    registered->driver_data = device->driver_data;
    registered->read = device->read;
    registered->write = device->write;
    return 1;
}

uint32_t block_device_count(void) {
    return registered_device_count;
}

const struct block_device *block_device_at(uint32_t index) {
    return index < registered_device_count ? &registered_devices[index] : 0;
}

const struct block_device *block_device_find(const char *name) {
    uint32_t index;
    if (name == 0 || string_length_bounded(name, BLOCK_DEVICE_NAME_CAPACITY) ==
                     BLOCK_DEVICE_NAME_CAPACITY) return 0;
    for (index = 0u; index < registered_device_count; ++index)
        if (names_equal(registered_devices[index].name, name))
            return &registered_devices[index];
    return 0;
}

int block_device_read(const struct block_device *device,
                      uint32_t first_sector, uint32_t sector_count,
                      uint8_t *buffer) {
    uint32_t index;
    int registered = 0;
    uint64_t byte_count;
    if (device == 0 || buffer == 0 || sector_count == 0u) return 0;
    for (index = 0u; index < registered_device_count; ++index) {
        if (device == &registered_devices[index]) {
            registered = 1;
            break;
        }
    }
    if (!registered || device->read == 0 ||
        first_sector > device->sector_count ||
        sector_count > device->sector_count - first_sector) return 0;
    byte_count = (uint64_t)device->sector_size * sector_count;
    if (byte_count > 0xFFFFFFFFu) return 0;
    return device->read(device, first_sector, sector_count, buffer);
}

int block_device_write(const struct block_device *device,
                       uint32_t first_sector, uint32_t sector_count,
                       const uint8_t *buffer) {
    uint32_t index;
    int registered = 0;
    uint64_t byte_count;
    if (device == 0 || buffer == 0 || sector_count == 0u) return 0;
    for (index = 0u; index < registered_device_count; ++index) {
        if (device == &registered_devices[index]) {
            registered = 1;
            break;
        }
    }
    if (!registered || device->flags != BLOCK_DEVICE_WRITABLE ||
        device->write == 0 || first_sector > device->sector_count ||
        sector_count > device->sector_count - first_sector) return 0;
    byte_count = (uint64_t)device->sector_size * sector_count;
    if (byte_count > 0xFFFFFFFFu) return 0;
    return device->write(device, first_sector, sector_count, buffer);
}
