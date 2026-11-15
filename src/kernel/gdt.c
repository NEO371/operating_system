#include "gdt.h"

#include <stdint.h>

struct __attribute__((packed)) tss32 {
    uint16_t previous_task, reserved0;
    uint32_t esp0;
    uint16_t ss0, reserved1;
    uint32_t esp1;
    uint16_t ss1, reserved2;
    uint32_t esp2;
    uint16_t ss2, reserved3;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint16_t es, reserved4, cs, reserved5, ss, reserved6;
    uint16_t ds, reserved7, fs, reserved8, gs, reserved9;
    uint16_t ldt, reserved10, trap, iomap_base;
};
_Static_assert(sizeof(struct tss32) == 104u, "32-bit TSS layout mismatch");

static struct gdt_entry gdt[6];
static struct gdt_ptr gdtr;
static struct tss32 tss;
extern uint8_t stack_top[];

static void gdt_set_entry(uint32_t index, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t flags) {
    struct gdt_entry *entry = &gdt[index];

    entry->limit_low = (uint16_t)(limit & 0xFFFFu);
    entry->base_low = (uint16_t)(base & 0xFFFFu);
    entry->base_middle = (uint8_t)((base >> 16) & 0xFFu);
    entry->access = access;
    entry->granularity = (uint8_t)(((limit >> 16) & 0x0Fu) | (flags & 0xF0u));
    entry->base_high = (uint8_t)((base >> 24) & 0xFFu);
}

void gdt_init(void) {
    /* Granularity=4 KiB and limit=0xFFFFF produce an inclusive 4 GiB range. */
    gdt_set_entry(0, 0, 0, 0, 0);
    gdt_set_entry(1, 0, 0xFFFFFu, 0x9Au, 0xC0u); /* present, ring 0, code RX */
    gdt_set_entry(2, 0, 0xFFFFFu, 0x92u, 0xC0u); /* present, ring 0, data RW */
    gdt_set_entry(3, 0, 0xFFFFFu, 0xFAu, 0xC0u); /* present, ring 3, code RX */
    gdt_set_entry(4, 0, 0xFFFFFu, 0xF2u, 0xC0u); /* present, ring 3, data RW */
    tss.ss0 = KERNEL_DATA_SELECTOR;
    tss.esp0 = (uint32_t)(uintptr_t)stack_top;
    tss.iomap_base = (uint16_t)sizeof(tss);
    gdt_set_entry(5, (uint32_t)(uintptr_t)&tss,
                  (uint32_t)sizeof(tss) - 1u, 0x89u, 0u);

    gdtr.limit = (uint16_t)(sizeof(gdt) - 1u);
    gdtr.base = (uint32_t)(uintptr_t)gdt;
    gdt_flush((uint32_t)(uintptr_t)&gdtr);
    __asm__ volatile ("ltr %0" : : "r"((uint16_t)TSS_SELECTOR) : "memory");
}

void tss_set_kernel_stack(uint32_t stack_top_address) {
    tss.esp0 = stack_top_address;
}

uint32_t gdt_segment_base(uint16_t selector) {
    uint32_t index = (uint32_t)(selector >> 3);
    const struct gdt_entry *entry;

    if ((selector & 0x04u) != 0u || index >= 5u) {
        return 0u;
    }
    entry = &gdt[index];
    return (uint32_t)entry->base_low |
           ((uint32_t)entry->base_middle << 16) |
           ((uint32_t)entry->base_high << 24);
}
