#ifndef MINI_OS_INTERRUPTS_H
#define MINI_OS_INTERRUPTS_H

#include <stdint.h>

struct interrupt_frame {
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t original_esp;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
    uint32_t vector;
    uint32_t error_code;
    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
};

void interrupts_init(void);
void interrupts_set_syscall_gate(void (*handler)(void));
void interrupts_enable_timer_keyboard(void);
uint32_t *irq_dispatch(uint32_t irq, uint32_t *saved_context);
uint32_t timer_ticks(void);
void kernel_log(const char *text);
void kernel_log_buffer(const char *buffer, uint32_t length);
void kernel_log_hex(uint32_t value);
int fault_test_handle(struct interrupt_frame *frame);

#endif
