#ifndef MINI_OS_MEMORY_H
#define MINI_OS_MEMORY_H

#include <stdint.h>

struct multiboot_info;

/*
 * Build the physical-page map from a Multiboot v1 memory map.
 * The allocator is limited to 32-bit physical addresses and returns identity
 * addresses; DMA users must still apply device-specific address constraints.
 */
int memory_init(const struct multiboot_info *info, uint32_t info_address,
                uint32_t kernel_start, uint32_t kernel_end);
uint32_t memory_alloc_page(void);
int memory_free_page(uint32_t physical_address);
uint32_t memory_alloc_contiguous_pages(uint32_t count);
int memory_free_contiguous_pages(uint32_t physical_address, uint32_t count);
int memory_page_is_mapped(uint32_t physical_address);
int memory_next_mapped_page(uint32_t first_page, uint32_t *mapped_page);
uint32_t memory_free_pages(void);
uint32_t memory_managed_pages(void);
int memory_is_ready(void);

#endif
