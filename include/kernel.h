#ifndef MINI_OS_KERNEL_H
#define MINI_OS_KERNEL_H

#include <stdint.h>
#include <stddef.h>

#define MULTIBOOT_BOOT_MAGIC 0x2BADB002u

/* Multiboot v1 information block through its framebuffer extension. */
struct __attribute__((packed)) multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint8_t red_field_position;
    uint8_t red_mask_size;
    uint8_t green_field_position;
    uint8_t green_mask_size;
    uint8_t blue_field_position;
    uint8_t blue_mask_size;
};

struct __attribute__((packed)) multiboot_module {
    uint32_t start;
    uint32_t end;
    uint32_t string;
    uint32_t reserved;
};

#define MINI_OS_MODULE_NAME_CAPACITY 32u
#define MINI_OS_MODULE_LIMIT 8u

struct ring3_elf_module {
    char name[MINI_OS_MODULE_NAME_CAPACITY];
    const uint8_t *image;
    uint32_t image_size;
};

_Static_assert(offsetof(struct multiboot_info, framebuffer_addr) == 88u,
               "Multiboot framebuffer fields must match the v1 ABI");
_Static_assert(sizeof(struct multiboot_info) == 116u,
               "Multiboot information structure layout mismatch");
_Static_assert(sizeof(struct multiboot_module) == 16u,
               "Multiboot module descriptor layout mismatch");

void kernel_main(uint32_t boot_magic, uint32_t multiboot_info_address);

#endif
