#ifndef MINI_OS_RING3_H
#define MINI_OS_RING3_H

#include "interrupts.h"
#include "kernel.h"

int ring3_run_smoke(void);
int ring3_register_elf_module(const char *name, const uint8_t *image,
                              uint32_t image_size);
void ring3_reset_elf_modules(void);
int ring3_run_default_elf_module(void);
int ring3_scheduler_run(void);
uint32_t *ring3_scheduler_on_timer(uint32_t *saved_context);
uint32_t ring3_scheduler_history_count(void);
int ring3_scheduler_history_get(uint32_t newest_offset, uint32_t *process_id,
                                uint32_t *tick);
int ring3_scheduler_is_active(void);
uint32_t ring3_scheduler_current_pid(void);
uint32_t ring3_scheduler_process_state(uint32_t process_id);
void ring3_syscall_dispatch(struct interrupt_frame *frame);
int ring3_user_fault_handle(struct interrupt_frame *frame);
#ifdef MINIOS_PROCESS_TEST
void ring3_process_tests_exit_qemu(int passed);
#endif
#ifdef MINIOS_SCHEDULER_TEST
void ring3_scheduler_tests_exit_qemu(int passed);
#endif

#endif
