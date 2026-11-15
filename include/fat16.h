#ifndef MINI_OS_FAT16_H
#define MINI_OS_FAT16_H

#include <stdint.h>

struct block_device;

int fat16_mount(void);
int fat16_mount_device(const struct block_device *device);
int fat16_read_root_file(const char short_name[11], uint8_t *destination,
                         uint32_t capacity, uint32_t *size_out);
int fat16_write_root_file(const char short_name[11], const uint8_t *source,
                          uint32_t size);
int storage_self_check(void);
int storage_reject_invalid_partition_check(void);

#endif
