#ifndef MINI_OS_BLOCK_H
#define MINI_OS_BLOCK_H

#include <stdint.h>

#define BLOCK_DEVICE_LIMIT 8u
#define BLOCK_DEVICE_NAME_CAPACITY 16u
#define BLOCK_SECTOR_SIZE 512u
#define BLOCK_DEVICE_READ_ONLY 0x01u
#define BLOCK_DEVICE_WRITABLE 0x02u

struct block_device;

typedef int (*block_device_read_fn)(const struct block_device *device,
                                   uint32_t first_sector,
                                   uint32_t sector_count, uint8_t *buffer);
typedef int (*block_device_write_fn)(const struct block_device *device,
                                    uint32_t first_sector,
                                    uint32_t sector_count,
                                    const uint8_t *buffer);

struct block_device {
    char name[BLOCK_DEVICE_NAME_CAPACITY];
    uint32_t sector_size;
    uint32_t sector_count;
    uint32_t flags;
    void *driver_data;
    block_device_read_fn read;
    block_device_write_fn write;
};

/* Fixed-capacity registry for kernel block drivers. */
int block_device_register(const struct block_device *device);
uint32_t block_device_count(void);
const struct block_device *block_device_at(uint32_t index);
const struct block_device *block_device_find(const char *name);
int block_device_read(const struct block_device *device,
                      uint32_t first_sector, uint32_t sector_count,
                      uint8_t *buffer);
int block_device_write(const struct block_device *device,
                       uint32_t first_sector, uint32_t sector_count,
                       const uint8_t *buffer);

#endif
