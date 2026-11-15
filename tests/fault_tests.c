#include "fault_tests.h"

#include "interrupts.h"

#include <stdint.h>

#define PAGE_FAULT_VECTOR 14u
#define PAGE_FAULT_PRESENT 0x01u
#define PAGE_FAULT_WRITE 0x02u
#define PAGE_FAULT_USER 0x04u
#define PAGE_FAULT_ACCESS_MASK (PAGE_FAULT_PRESENT | PAGE_FAULT_WRITE | PAGE_FAULT_USER)

struct expected_fault {
    uint32_t address;
    uint32_t error_bits;
    uint32_t recovery_eip;
};

static volatile uint8_t fault_armed;
static volatile uint32_t observed_faults;
static struct expected_fault expected;

extern void paging_test_write_null(void);
extern void paging_test_null_resume(void);
extern void paging_test_write_rodata(void);
extern void paging_test_rodata_resume(void);
extern uint8_t paging_test_ro_word[];

static void arm_fault(uint32_t address, uint32_t error_bits,
                      uint32_t recovery_eip) {
    expected.address = address;
    expected.error_bits = error_bits;
    expected.recovery_eip = recovery_eip;
    fault_armed = 1u;
}

int fault_test_handle(struct interrupt_frame *frame) {
    uint32_t fault_address;
    if (fault_armed == 0u || frame->vector != PAGE_FAULT_VECTOR) return 0;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_address));
    if (fault_address != expected.address ||
        (frame->error_code & PAGE_FAULT_ACCESS_MASK) != expected.error_bits) {
        return 0;
    }
    fault_armed = 0u;
    ++observed_faults;
    frame->eip = expected.recovery_eip;
    return 1;
}

int paging_fault_tests_run(void) {
    observed_faults = 0u;
    arm_fault(0u, PAGE_FAULT_WRITE,
              (uint32_t)(uintptr_t)paging_test_null_resume);
    paging_test_write_null();
    if (fault_armed != 0u || observed_faults != 1u) return 0;

    arm_fault((uint32_t)(uintptr_t)paging_test_ro_word,
              PAGE_FAULT_PRESENT | PAGE_FAULT_WRITE,
              (uint32_t)(uintptr_t)paging_test_rodata_resume);
    paging_test_write_rodata();
    if (fault_armed != 0u || observed_faults != 2u) return 0;
    return 1;
}

void paging_fault_tests_exit_qemu(void) {
#ifdef MINIOS_FAULT_TEST
    const uint32_t exit_code = 0x10u;
    __asm__ volatile ("outl %0, $0xF4" : : "a"(exit_code));
    for (;;) __asm__ volatile ("cli; hlt");
#endif
}
