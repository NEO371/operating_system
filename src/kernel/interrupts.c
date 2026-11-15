#include "interrupts.h"

#include "gdt.h"
#include "input.h"
#include "mouse.h"
#include "ring3.h"

#include <stdint.h>

#define IDT_ENTRY_COUNT 256u
#define KERNEL_INTERRUPT_GATE 0x8Eu

struct __attribute__((packed)) idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t attributes;
    uint16_t offset_high;
};

struct __attribute__((packed)) idt_register {
    uint16_t limit;
    uint32_t base;
};

static struct idt_entry idt[IDT_ENTRY_COUNT];
extern void (*isr_stub_table[32])(void);
extern void (*irq_stub_table[16])(void);
extern void isr_default(void);
extern void isr128(void);
extern void idt_load(const struct idt_register *registers);
static volatile uint32_t system_ticks;
void exception_dispatch(struct interrupt_frame *frame);

static uint8_t port_read(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void port_write(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void io_wait(void) {
    port_write(0x80u, 0u);
}

static void pic_remap(void) {
    uint8_t master_mask = port_read(0x21u);
    uint8_t slave_mask = port_read(0xA1u);

    port_write(0x20u, 0x11u); io_wait();
    port_write(0xA0u, 0x11u); io_wait();
    port_write(0x21u, 0x20u); io_wait();
    port_write(0xA1u, 0x28u); io_wait();
    port_write(0x21u, 0x04u); io_wait();
    port_write(0xA1u, 0x02u); io_wait();
    port_write(0x21u, 0x01u); io_wait();
    port_write(0xA1u, 0x01u); io_wait();
    port_write(0x21u, (uint8_t)(master_mask | 0x03u));
    port_write(0xA1u, slave_mask);
}

static void pit_program_100hz(void) {
    const uint16_t divisor = 11932u;
    port_write(0x43u, 0x36u); /* Channel 0, low/high, square wave. */
    port_write(0x40u, (uint8_t)(divisor & 0xFFu));
    port_write(0x40u, (uint8_t)(divisor >> 8));
}

static void serial_init(void) {
    port_write(0x3F9u, 0x00u); /* Disable UART interrupts. */
    port_write(0x3FBu, 0x80u); /* Enable divisor latch. */
    port_write(0x3F8u, 0x03u); /* Divisor 3: 38400 baud. */
    port_write(0x3F9u, 0x00u);
    port_write(0x3FBu, 0x03u); /* 8 data bits, no parity, one stop bit. */
    port_write(0x3FAu, 0xC7u); /* Enable and clear FIFO. */
    port_write(0x3FCu, 0x0Bu); /* DTR, RTS, OUT2. */
}

static void serial_putc(char character) {
    uint32_t timeout = 100000u;
    while ((port_read(0x3FDu) & 0x20u) == 0u && timeout != 0u) {
        --timeout;
    }
    if (timeout != 0u) port_write(0x3F8u, (uint8_t)character);
}

static void serial_write(const char *text) {
    while (*text != '\0') {
        if (*text == '\n') serial_putc('\r');
        serial_putc(*text++);
    }
}

static void serial_hex(uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";
    int32_t shift;
    serial_write("0x");
    for (shift = 28; shift >= 0; shift -= 4) {
        serial_putc(digits[(value >> (uint32_t)shift) & 0xFu]);
    }
}

void kernel_log(const char *text) {
    serial_write(text);
}

void kernel_log_buffer(const char *buffer, uint32_t length) {
    uint32_t index;
    for (index = 0u; index < length; ++index) {
        if (buffer[index] == '\n') serial_putc('\r');
        serial_putc(buffer[index]);
    }
}

void kernel_log_hex(uint32_t value) {
    serial_hex(value);
}

static void idt_set_gate(uint32_t vector, void (*handler)(void)) {
    uintptr_t address = (uintptr_t)handler;
    struct idt_entry *entry = &idt[vector];
    entry->offset_low = (uint16_t)(address & 0xFFFFu);
    entry->selector = KERNEL_CODE_SELECTOR;
    entry->zero = 0u;
    entry->attributes = KERNEL_INTERRUPT_GATE;
    entry->offset_high = (uint16_t)((address >> 16) & 0xFFFFu);
}

void interrupts_set_syscall_gate(void (*handler)(void)) {
    uintptr_t address = (uintptr_t)handler;
    struct idt_entry *entry = &idt[0x80u];
    entry->offset_low = (uint16_t)(address & 0xFFFFu);
    entry->selector = KERNEL_CODE_SELECTOR;
    entry->zero = 0u;
    entry->attributes = 0xEEu; /* Present, DPL 3, 32-bit interrupt gate. */
    entry->offset_high = (uint16_t)((address >> 16) & 0xFFFFu);
}

void interrupts_init(void) {
    struct idt_register registers;
    uint32_t vector;

    for (vector = 0u; vector < IDT_ENTRY_COUNT; ++vector) {
        idt_set_gate(vector, isr_default);
    }
    for (vector = 0u; vector < 32u; ++vector) {
        idt_set_gate(vector, isr_stub_table[vector]);
    }
    for (vector = 0u; vector < 16u; ++vector) {
        idt_set_gate(32u + vector, irq_stub_table[vector]);
    }
    idt_set_gate(0x80u, isr128);
    interrupts_set_syscall_gate(isr128);
    registers.limit = (uint16_t)(sizeof(idt) - 1u);
    registers.base = (uint32_t)(uintptr_t)idt;
    idt_load(&registers);
    serial_init();
    serial_write("MiniOS: IDT loaded; CPU exception handlers active.\n");
}

void interrupts_enable_timer_keyboard(void) {
    __asm__ volatile ("cli");
    pic_remap();
    pit_program_100hz();
    /* Unmask timer, keyboard, and the PS/2 mouse through the IRQ2 cascade. */
    port_write(0x21u, 0xF8u);
    port_write(0xA1u, 0xEFu);
    __asm__ volatile ("sti" : : : "memory");
    serial_write("MiniOS: PIC remapped; PIT at 100 Hz; keyboard/timer IRQs enabled.\n");
}

uint32_t timer_ticks(void) {
    return system_ticks;
}

uint32_t *irq_dispatch(uint32_t irq, uint32_t *saved_context) {
    if (irq == 0u) {
        ++system_ticks;
    } else if (irq == 1u) {
        keyboard_handle_scancode(port_read(0x60u));
    } else if (irq == 12u) {
        mouse_handle_byte(port_read(0x60u));
    }
    if (irq >= 8u) port_write(0xA0u, 0x20u);
    port_write(0x20u, 0x20u);
    if (irq == 0u) return ring3_scheduler_on_timer(saved_context);
    return saved_context;
}

#ifndef MINIOS_FAULT_TEST
int fault_test_handle(struct interrupt_frame *frame) {
    (void)frame;
    return 0;
}
#endif

void exception_dispatch(struct interrupt_frame *frame) {
    static const char *const exception_names[32] = {
        "Divide Error", "Debug", "Non-maskable Interrupt", "Breakpoint",
        "Overflow", "Bound Range", "Invalid Opcode", "Device Not Available",
        "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS",
        "Segment Not Present", "Stack-Segment Fault", "General Protection",
        "Page Fault", "Reserved", "x87 Floating-Point", "Alignment Check",
        "Machine Check", "SIMD Floating-Point", "Virtualization", "Control Protection",
        "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
        "Hypervisor Injection", "VMM Communication", "Security", "Reserved"
    };

    if (frame->vector == 0x80u) {
        ring3_syscall_dispatch(frame);
        return;
    }
    if (ring3_user_fault_handle(frame)) return;
    if (fault_test_handle(frame)) return;
    __asm__ volatile ("cli");
    serial_write("\nMiniOS: fatal CPU exception: ");
    if (frame->vector < 32u) serial_write(exception_names[frame->vector]);
    else serial_write("unexpected interrupt or reserved vector");
    serial_write("\nvector="); serial_hex(frame->vector);
    serial_write(" error="); serial_hex(frame->error_code);
    serial_write(" eip="); serial_hex(frame->eip);
    serial_write(" cs="); serial_hex(frame->cs);
    serial_write(" eflags="); serial_hex(frame->eflags);
    if (frame->vector == 14u) {
        uint32_t fault_address;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_address));
        serial_write(" cr2="); serial_hex(fault_address);
    }
    serial_write("\nSystem halted.\n");
    for (;;) __asm__ volatile ("cli; hlt");
}
