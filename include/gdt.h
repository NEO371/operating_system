#ifndef MINI_OS_GDT_H
#define MINI_OS_GDT_H

#include <stdint.h>

enum {
    KERNEL_CODE_SELECTOR = 0x08,
    KERNEL_DATA_SELECTOR = 0x10,
    USER_CODE_SELECTOR = 0x1B,
    USER_DATA_SELECTOR = 0x23,
    TSS_SELECTOR = 0x28
};

struct __attribute__((packed)) gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
};

struct __attribute__((packed)) gdt_ptr {
    uint16_t limit;
    uint32_t base;
};

void gdt_flush(uint32_t gdtr_ptr);
void gdt_init(void);
uint32_t gdt_segment_base(uint16_t selector);
void tss_set_kernel_stack(uint32_t stack_top);

#endif
