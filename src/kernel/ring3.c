#include "ring3.h"

#include "gdt.h"
#include "memory.h"
#include "paging.h"
#include "syscall.h"
#include "resource_monitor.h"

#include <stdint.h>
#include <stddef.h>

extern void ring3_enter(void);
extern void ring3_enter_at(uint32_t user_entry, uint32_t user_stack_pointer);
extern void ring3_leave(void) __attribute__((noreturn));
extern uint8_t ring3_kernel_stack_top[];
extern uint8_t _text_start[];
extern uint8_t _user_text_start[], _user_text_end[];

#define USER_DATA_ADDRESS 0x40000000u
#define USER_STACK_TOP 0xBFFFE000u
#define USER_STACK_PAGE_ADDRESS 0xBFFFD000u
#define USER_ARGUMENT_LIMIT 128u
#define USER_ARGC_LIMIT 8u
#define ELF_MODULE_MAX_SIZE (1024u * 1024u)
#define SCHEDULER_TASK_COUNT 3u
#define SCHEDULER_KERNEL_STACK_SIZE 8192u
#define SCHEDULER_WATCHDOG_TICKS 500u
#define SCHEDULER_USER_COUNTER_LIMIT 5000000u
#define SCHEDULER_HISTORY_CAPACITY 128u

enum scheduled_task_state {
    TASK_UNUSED, TASK_RUNNABLE, TASK_WAITING, TASK_EXITED, TASK_REAPED
};

struct scheduled_task {
    struct paging_address_space address_space;
    uint32_t *irq_context;
    uint32_t *kernel_context;
    uint32_t process_id;
    uint32_t parent_id;
    uint32_t waiting_for;
    uint32_t exit_status;
    uint32_t program_heap_start;
    uint32_t program_break;
    uint32_t final_data0;
    uint32_t final_user_pid;
    enum scheduled_task_state state;
    uint8_t boot_task;
    uint8_t loaded_image;
    uint8_t named_image;
    uint8_t syscall_seen;
    uint8_t write_seen;
    uint8_t invalid_pointer_tests;
    uint8_t spawn_seen;
    uint8_t wait_seen;
    uint8_t clean_child_seen;
    uint8_t fault_child_seen;
    uint8_t elf_child_seen;
    uint8_t spawn_count;
    uint8_t exit_snapshot_valid;
    uint8_t __attribute__((aligned(16))) kernel_stack[SCHEDULER_KERNEL_STACK_SIZE];
};

static struct scheduled_task scheduled_tasks[SCHEDULER_TASK_COUNT];
static volatile uint32_t scheduler_active;
static volatile uint32_t scheduler_current;
static volatile uint32_t scheduler_ticks;
static volatile uint32_t scheduler_switches;
struct scheduler_history_entry { uint32_t process_id; uint32_t tick; };
static struct scheduler_history_entry scheduler_history[SCHEDULER_HISTORY_CAPACITY];
static volatile uint32_t scheduler_history_count;
static volatile uint32_t scheduler_history_next;
static volatile uint32_t scheduler_passed;
static uint32_t scheduler_next_pid;
static volatile uint8_t scheduler_preempted_live[SCHEDULER_TASK_COUNT];
static uint32_t scheduler_boot_context;
static volatile uint32_t expected_user_value;
static volatile uint8_t smoke_reported;
static volatile uint8_t isolation_reported;
static struct paging_address_space *foreground_user_space;
static volatile uint32_t foreground_exit_status;
static volatile uint8_t foreground_pid_seen;
static volatile uint8_t foreground_write_seen;
static uint32_t foreground_program_heap_start;
static uint32_t foreground_program_break;
static struct ring3_elf_module registered_elf_modules[MINI_OS_MODULE_LIMIT];
static uint32_t registered_elf_module_count;

static void scheduler_history_record(uint32_t process_id, uint32_t tick) {
    scheduler_history[scheduler_history_next].process_id = process_id;
    scheduler_history[scheduler_history_next].tick = tick;
    scheduler_history_next = (scheduler_history_next + 1u) %
                            SCHEDULER_HISTORY_CAPACITY;
    if (scheduler_history_count < SCHEDULER_HISTORY_CAPACITY)
        ++scheduler_history_count;
}

struct __attribute__((packed)) elf32_header {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint32_t entry;
    uint32_t program_header_offset;
    uint32_t section_header_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t program_header_size;
    uint16_t program_header_count;
    uint16_t section_header_size;
    uint16_t section_header_count;
    uint16_t section_name_index;
};

struct __attribute__((packed)) elf32_program_header {
    uint32_t type;
    uint32_t offset;
    uint32_t virtual_address;
    uint32_t physical_address;
    uint32_t file_size;
    uint32_t memory_size;
    uint32_t flags;
    uint32_t alignment;
};

_Static_assert(sizeof(struct elf32_header) == 52u, "ELF32 header size mismatch");
_Static_assert(sizeof(struct elf32_program_header) == 32u,
               "ELF32 program-header size mismatch");

#define ELF32_PT_LOAD 1u
#define ELF32_PF_X 1u
#define ELF32_PF_W 2u
#define ELF32_PF_R 4u
#define ELF32_USER_MIN 0x00400000u

static int elf_build_initial_stack(struct paging_address_space *space,
                                   const char *program_name,
                                   const char *arguments,
                                   uint32_t arguments_length,
                                   uint32_t *user_stack_pointer) {
    uint8_t *stack;
    uint32_t cursor = 0x1000u;
    uint32_t argv_addresses[USER_ARGC_LIMIT];
    uint32_t argc = 1u;
    uint32_t program_name_length = 0u;
    uint32_t word_count;
    uint32_t stack_offset;
    uint32_t *words;

    if (space == 0 || program_name == 0 || user_stack_pointer == 0 ||
        arguments_length > USER_ARGUMENT_LIMIT ||
        (arguments_length != 0u && arguments == 0) ||
        space->stack_page_physical == 0u) return 0;
    while (program_name_length < MINI_OS_MODULE_NAME_CAPACITY &&
           program_name[program_name_length] != '\0')
        ++program_name_length;
    if (program_name_length == 0u ||
        program_name_length >= MINI_OS_MODULE_NAME_CAPACITY) return 0;
    ++program_name_length;
    stack = (uint8_t *)(uintptr_t)space->stack_page_physical;
    if (arguments_length != 0u) {
        uint32_t start = 0u;
        uint32_t index;
        for (index = 0u; index + 1u < arguments_length; ++index) {
            if (arguments[index] != '\0') continue;
            if (index == start || argc >= USER_ARGC_LIMIT) return 0;
            cursor -= index - start + 1u;
            for (uint32_t byte = 0u; byte <= index - start; ++byte)
                stack[cursor + byte] = (uint8_t)arguments[start + byte];
            argv_addresses[argc++] = USER_STACK_PAGE_ADDRESS + cursor;
            start = index + 1u;
        }
        if (start != arguments_length - 1u ||
            arguments[arguments_length - 1u] != '\0') return 0;
    }
    cursor -= program_name_length;
    for (uint32_t i = 0u; i < program_name_length; ++i)
        stack[cursor + i] = (uint8_t)program_name[i];
    argv_addresses[0] = USER_STACK_PAGE_ADDRESS + cursor;
    cursor &= ~3u;

    word_count = argc + 3u; /* argc, argv[], argv NULL, empty envp NULL */
    stack_offset = (cursor - word_count * sizeof(uint32_t)) & ~15u;
    if (stack_offset < 64u) return 0;
    words = (uint32_t *)(void *)(stack + stack_offset);
    words[0] = argc;
    for (uint32_t arg = 0u; arg < argc; ++arg)
        words[arg + 1u] = argv_addresses[arg];
    words[argc + 1u] = 0u; /* argv terminator */
    words[argc + 2u] = 0u; /* empty envp */
    *user_stack_pointer = USER_STACK_PAGE_ADDRESS + stack_offset;
    return 1;
}

static uint32_t elf_user_page_physical(const struct paging_address_space *space,
                                       uint32_t virtual_address) {
    uint32_t page = virtual_address & ~0xFFFu;
    uint32_t index;
    for (index = 0u; index < space->program_page_count; ++index)
        if (space->program_page_virtual[index] == page)
            return space->program_page_physical[index];
    return 0u;
}

static int elf_zero_user_range(struct paging_address_space *space,
                               uint32_t start, uint32_t length) {
    uint32_t cursor = start;
    uint32_t remaining = length;
    while (remaining != 0u) {
        uint32_t physical = elf_user_page_physical(space, cursor);
        uint32_t offset = cursor & 0xFFFu;
        uint32_t chunk = 0x1000u - offset;
        uint8_t *destination;
        if (physical == 0u) return 0;
        if (chunk > remaining) chunk = remaining;
        destination = (uint8_t *)(uintptr_t)(physical + offset);
        for (uint32_t byte = 0u; byte < chunk; ++byte) destination[byte] = 0u;
        cursor += chunk;
        remaining -= chunk;
    }
    return 1;
}

static int elf_copy_to_user(struct paging_address_space *space,
                            uint32_t destination_address,
                            const uint8_t *source, uint32_t length) {
    uint32_t cursor = destination_address;
    uint32_t remaining = length;
    while (remaining != 0u) {
        uint32_t physical = elf_user_page_physical(space, cursor);
        uint32_t offset = cursor & 0xFFFu;
        uint32_t chunk = 0x1000u - offset;
        uint8_t *destination;
        if (physical == 0u) return 0;
        if (chunk > remaining) chunk = remaining;
        destination = (uint8_t *)(uintptr_t)(physical + offset);
        for (uint32_t byte = 0u; byte < chunk; ++byte)
            destination[byte] = source[byte];
        cursor += chunk;
        source += chunk;
        remaining -= chunk;
    }
    return 1;
}

static int elf32_load_user_image(struct paging_address_space *space,
                                 const uint8_t *image, uint32_t image_size,
                                 uint32_t *entry_out,
                                 uint32_t *heap_start_out) {
    const struct elf32_header *header;
    const struct elf32_program_header *programs;
    uint64_t table_end;
    uint32_t load_count = 0u;
    uint32_t page_budget = 0u;
    uint64_t max_virtual_end = 0u;
    int entry_is_executable = 0;

    if (space == 0 || image == 0 || entry_out == 0 || heap_start_out == 0 ||
        image_size < sizeof(struct elf32_header)) return 0;
    header = (const struct elf32_header *)(const void *)image;
    if (header->ident[0] != 0x7Fu || header->ident[1] != 'E' ||
        header->ident[2] != 'L' || header->ident[3] != 'F' ||
        header->ident[4] != 1u || header->ident[5] != 1u ||
        header->ident[6] != 1u || header->type != 2u ||
        header->machine != 3u || header->version != 1u ||
        header->header_size != sizeof(struct elf32_header) ||
        header->program_header_size != sizeof(struct elf32_program_header) ||
        header->program_header_count == 0u || header->program_header_count > 16u)
        return 0;
    table_end = (uint64_t)header->program_header_offset +
        (uint64_t)header->program_header_count * header->program_header_size;
    if (table_end > image_size) return 0;
    programs = (const struct elf32_program_header *)(const void *)
        (image + header->program_header_offset);

    for (uint32_t index = 0u; index < header->program_header_count; ++index) {
        const struct elf32_program_header *program = &programs[index];
        uint64_t virtual_end;
        uint64_t file_end;
        uint32_t page_start;
        uint32_t page_end;
        uint32_t alignment = program->alignment;
        if (program->type == 2u || program->type == 3u) return 0;
        if (program->type != ELF32_PT_LOAD) continue;
        /* Linkers may emit an unused, zero-sized writable load segment. */
        if (program->memory_size == 0u && program->file_size == 0u) continue;
        ++load_count;
        virtual_end = (uint64_t)program->virtual_address + program->memory_size;
        file_end = (uint64_t)program->offset + program->file_size;
        if (program->memory_size == 0u ||
            program->file_size > program->memory_size || file_end > image_size ||
            program->virtual_address < ELF32_USER_MIN ||
            virtual_end > USER_DATA_ADDRESS || virtual_end > 0xFFFFFFFFull ||
            (program->flags & ELF32_PF_R) == 0u ||
            (program->flags & ~(ELF32_PF_R | ELF32_PF_W | ELF32_PF_X)) != 0u ||
            ((program->flags & (ELF32_PF_W | ELF32_PF_X)) ==
             (ELF32_PF_W | ELF32_PF_X))) return 0;
        if (virtual_end > max_virtual_end) max_virtual_end = virtual_end;
        if (alignment > 1u &&
            ((alignment & (alignment - 1u)) != 0u ||
             ((program->virtual_address - program->offset) &
              (alignment - 1u)) != 0u)) return 0;
        page_start = program->virtual_address & ~0xFFFu;
        page_end = (uint32_t)((virtual_end + 0xFFFu) & ~0xFFFu);
        page_budget += (page_end - page_start) >> 12;
        if (page_budget > PAGING_USER_PROGRAM_PAGE_LIMIT) return 0;
        if ((program->flags & ELF32_PF_X) != 0u &&
            header->entry >= program->virtual_address &&
            header->entry < (uint64_t)program->virtual_address +
                            program->file_size) entry_is_executable = 1;

        for (uint32_t previous = 0u; previous < index; ++previous) {
            const struct elf32_program_header *other = &programs[previous];
            uint64_t other_end;
            uint32_t other_start_page, other_end_page;
            if (other->type != ELF32_PT_LOAD ||
                (other->memory_size == 0u && other->file_size == 0u)) continue;
            other_end = (uint64_t)other->virtual_address + other->memory_size;
            if ((uint64_t)program->virtual_address < other_end &&
                (uint64_t)other->virtual_address < virtual_end) return 0;
            other_start_page = other->virtual_address & ~0xFFFu;
            other_end_page = (uint32_t)((other_end + 0xFFFu) & ~0xFFFu);
            if (page_start < other_end_page && other_start_page < page_end &&
                (((program->flags & ELF32_PF_X) != 0u &&
                  (other->flags & ELF32_PF_W) != 0u) ||
                 ((program->flags & ELF32_PF_W) != 0u &&
                  (other->flags & ELF32_PF_X) != 0u))) return 0;
        }
    }
    if (load_count == 0u || !entry_is_executable) return 0;

    for (uint32_t index = 0u; index < header->program_header_count; ++index) {
        const struct elf32_program_header *program = &programs[index];
        uint64_t virtual_end;
        uint32_t page;
        uint32_t page_end;
        if (program->type != ELF32_PT_LOAD || program->memory_size == 0u) continue;
        virtual_end = (uint64_t)program->virtual_address + program->memory_size;
        page = program->virtual_address & ~0xFFFu;
        page_end = (uint32_t)((virtual_end + 0xFFFu) & ~0xFFFu);
        while (page < page_end) {
            if (!paging_address_space_map_user_page(
                    space, page, (program->flags & ELF32_PF_W) != 0u, 0))
                return 0;
            page += 0x1000u;
        }
    }
    for (uint32_t index = 0u; index < header->program_header_count; ++index) {
        const struct elf32_program_header *program = &programs[index];
        if (program->type == ELF32_PT_LOAD && program->memory_size != 0u &&
            program->memory_size > program->file_size &&
            !elf_zero_user_range(space,
                program->virtual_address + program->file_size,
                program->memory_size - program->file_size)) return 0;
    }
    for (uint32_t index = 0u; index < header->program_header_count; ++index) {
        const struct elf32_program_header *program = &programs[index];
        if (program->type == ELF32_PT_LOAD && program->file_size != 0u &&
            !elf_copy_to_user(space, program->virtual_address,
                image + program->offset, program->file_size)) return 0;
    }
    *entry_out = header->entry;
    *heap_start_out = (uint32_t)((max_virtual_end + 0xFFFu) & ~0xFFFu);
    return 1;
}

static uint32_t elf_set_program_break(struct paging_address_space *space,
                                     uint32_t heap_start,
                                     uint32_t *current_break,
                                     uint32_t requested_break) {
    uint32_t previous;
    uint32_t old_limit;
    uint32_t new_limit;
    uint32_t page;

    if (space == 0 || current_break == 0 || heap_start == 0u)
        return 0u - MINIOS_ERR_INVAL;
    previous = *current_break;
    if (requested_break == 0u) return previous;
    if (requested_break < heap_start || requested_break > USER_DATA_ADDRESS)
        return 0u - MINIOS_ERR_INVAL;
    if (requested_break == previous) return previous;
    old_limit = (previous + 0xFFFu) & ~0xFFFu;
    new_limit = (requested_break + 0xFFFu) & ~0xFFFu;
    if (!paging_activate_kernel()) return 0u - MINIOS_ERR_NOMEM;
    if (requested_break > previous) {
        for (page = old_limit; page < new_limit; page += 0x1000u) {
            if (!paging_address_space_map_user_page(space, page, 1, 0)) {
                for (uint32_t rollback = old_limit; rollback < page;
                     rollback += 0x1000u)
                    (void)paging_address_space_unmap_user_page(space, rollback);
                (void)paging_address_space_activate(space);
                return 0u - MINIOS_ERR_NOMEM;
            }
        }
    } else {
        for (page = new_limit; page < old_limit; page += 0x1000u) {
            if (!paging_address_space_unmap_user_page(space, page)) {
                (void)paging_address_space_activate(space);
                return 0u - MINIOS_ERR_NOMEM;
            }
        }
    }
    if (!paging_address_space_activate(space)) return 0u - MINIOS_ERR_NOMEM;
    *current_break = requested_break;
    return requested_break;
}

extern void process_context_switch(void *old_context_slot, uint32_t new_context);
extern void process_restore_irq_frame(uint32_t *context) __attribute__((noreturn));
extern void process_restore_syscall_frame(uint32_t *context) __attribute__((noreturn));
extern void scheduler_user_entry(void);
extern void scheduler_child_entry(void);
extern void scheduler_fault_child_entry(void);
extern uint8_t scheduler_user_terminated[];
extern uint8_t stack_top[];

static void scheduler_complete_task_exit(uint32_t index, uint32_t status,
                                         struct interrupt_frame *frame);


int ring3_run_smoke(void) {
    struct paging_address_space spaces[2] = {{0}};
    static const uint32_t initial_values[2] = {0x100u, 0x200u};
    uint32_t index;
    uint32_t free_pages_before = memory_free_pages();
    int setup_ok = 1;
    int all_ok = 1;

    tss_set_kernel_stack((uint32_t)(uintptr_t)ring3_kernel_stack_top);
    for (index = 0u; index < 2u; ++index) {
        if (!paging_address_space_create(&spaces[index], initial_values[index])) {
            setup_ok = 0;
            break;
        }
    }
    if (setup_ok &&
        (spaces[0].data_page_physical == spaces[1].data_page_physical ||
         spaces[0].stack_page_physical == spaces[1].stack_page_physical)) {
        setup_ok = 0;
    }
    if (!setup_ok) {
        kernel_log("MiniOS: isolated address-space setup failed.\n");
        all_ok = 0;
    } else {
        for (index = 0u; index < 2u; ++index) {
            smoke_reported = 0u;
            isolation_reported = 0u;
            expected_user_value = initial_values[index] + 1u;
            if (!paging_address_space_activate(&spaces[index])) {
                kernel_log("MiniOS: could not activate user address space.\n");
                all_ok = 0;
                break;
            }
            kernel_log("MiniOS: entering isolated ring-3 process.\n");
            ring3_enter();
            if (!paging_activate_kernel()) {
                kernel_log("MiniOS: could not restore kernel address space.\n");
                all_ok = 0;
                break;
            }
            if (smoke_reported != 0u && isolation_reported != 0u &&
                *(volatile uint32_t *)(uintptr_t)spaces[index].data_page_physical ==
                    expected_user_value) {
                kernel_log("MiniOS: isolated process data, syscall, and supervisor guard passed.\n");
            } else {
                kernel_log("MiniOS: isolated process protection check failed.\n");
                all_ok = 0;
            }
        }
    }
    (void)paging_activate_kernel();
    for (index = 0u; index < 2u; ++index)
        if (spaces[index].directory_physical != 0u)
            (void)paging_address_space_destroy(&spaces[index]);
    if (memory_free_pages() == free_pages_before) {
        kernel_log("MiniOS: per-process page tables and user pages reclaimed.\n");
    } else {
        kernel_log("MiniOS: isolated address-space teardown leaked physical pages.\n");
        all_ok = 0;
    }
    return all_ok;
}

static int ring3_run_elf_image(const struct ring3_elf_module *module) {
    struct paging_address_space space = {0};
    uint32_t free_pages_before = memory_free_pages();
    uint32_t entry = 0u;
    uint32_t program_heap_start = 0u;
    uint32_t user_stack_pointer = USER_STACK_TOP;
    int passed = 0;

    if (module == 0 || !paging_address_space_create(&space, 0u) ||
        !elf32_load_user_image(&space, module->image, module->image_size, &entry,
                               &program_heap_start) ||
        !elf_build_initial_stack(&space, module->name, 0, 0u,
                                 &user_stack_pointer)) {
        kernel_log("MiniOS: rejected standalone user ELF image.\n");
        if (space.directory_physical != 0u) {
            (void)paging_activate_kernel();
            (void)paging_address_space_destroy(&space);
        }
        return 0;
    }
    foreground_exit_status = 0xFFFFFFFFu;
    foreground_pid_seen = 0u;
    foreground_write_seen = 0u;
    foreground_program_heap_start = program_heap_start;
    foreground_program_break = program_heap_start;
    foreground_user_space = &space;
    if (!paging_address_space_activate(&space)) {
        foreground_user_space = 0;
        (void)paging_activate_kernel();
        (void)paging_address_space_destroy(&space);
        return 0;
    }
    tss_set_kernel_stack((uint32_t)(uintptr_t)ring3_kernel_stack_top);
    kernel_log("MiniOS: entering standalone ELF user process.\n");
    ring3_enter_at(entry, user_stack_pointer);
    foreground_user_space = 0;
    if (!paging_activate_kernel()) passed = 0;
    else {
        tss_set_kernel_stack((uint32_t)(uintptr_t)stack_top);
        passed = foreground_exit_status == 0u && foreground_pid_seen != 0u &&
                 foreground_write_seen != 0u;
    }
    if (passed) kernel_log("MiniOS: standalone ELF process exited cleanly.\n");
    else kernel_log("MiniOS: standalone ELF process verification failed.\n");
    if (!paging_address_space_destroy(&space)) passed = 0;
    if (memory_free_pages() != free_pages_before) passed = 0;
    return passed;
}

int ring3_register_elf_module(const char *name, const uint8_t *image,
                              uint32_t image_size) {
    uint32_t name_length = 0u;
    uint32_t index;
    struct ring3_elf_module *module;

    if (name == 0 || image == 0 || image_size == 0u ||
        image_size > ELF_MODULE_MAX_SIZE ||
        registered_elf_module_count >= MINI_OS_MODULE_LIMIT) return 0;
    while (name_length < MINI_OS_MODULE_NAME_CAPACITY && name[name_length] != '\0') {
        char c = name[name_length];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return 0;
        ++name_length;
    }
    if (name_length == 0u || name_length >= MINI_OS_MODULE_NAME_CAPACITY)
        return 0;
    for (index = 0u; index < registered_elf_module_count; ++index) {
        uint32_t i = 0u;
        while (registered_elf_modules[index].name[i] == name[i] && name[i] != '\0')
            ++i;
        if (registered_elf_modules[index].name[i] == name[i]) return 0;
    }
    module = &registered_elf_modules[registered_elf_module_count++];
    for (index = 0u; index <= name_length; ++index) module->name[index] = name[index];
    module->image = image;
    module->image_size = image_size;
    kernel_log("MiniOS: registered ELF module: ");
    kernel_log(module->name);
    kernel_log("\n");
    return 1;
}

void ring3_reset_elf_modules(void) {
    for (uint32_t index = 0u; index < registered_elf_module_count; ++index) {
        registered_elf_modules[index].name[0] = '\0';
        registered_elf_modules[index].image = 0;
        registered_elf_modules[index].image_size = 0u;
    }
    registered_elf_module_count = 0u;
}

int ring3_run_default_elf_module(void) {
    if (registered_elf_module_count == 0u) return 0;
    return ring3_run_elf_image(&registered_elf_modules[0]);
}

static uint32_t read_eflags(void) {
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0" : "=r"(flags));
    return flags;
}

static void scheduler_task_bootstrap(void) __attribute__((noreturn));

static int scheduler_task_initialize(uint32_t index, uint32_t process_id,
                                     uint32_t parent_id, uint32_t entry,
                                     int boot_task,
                                     const uint8_t *image,
                                     uint32_t image_size,
                                     int loaded_image,
                                     int named_image,
                                     const char *program_name,
                                     const char *arguments,
                                     uint32_t arguments_length) {
    struct scheduled_task *task = &scheduled_tasks[index];
    uint32_t *frame;
    uint32_t *context;
    uint32_t user_stack_pointer = USER_STACK_TOP;

    task->address_space.directory_physical = 0u;
    task->address_space.data_table_physical = 0u;
    task->address_space.stack_table_physical = 0u;
    task->address_space.data_page_physical = 0u;
    task->address_space.stack_page_physical = 0u;
    task->irq_context = 0;
    task->kernel_context = 0;
    task->process_id = process_id;
    task->parent_id = parent_id;
    task->waiting_for = 0u;
    task->exit_status = 0xFFFFFFFFu;
    task->program_heap_start = 0u;
    task->program_break = 0u;
    task->final_data0 = 0u;
    task->final_user_pid = 0u;
    task->state = TASK_UNUSED;
    task->boot_task = boot_task != 0;
    task->loaded_image = loaded_image != 0;
    task->named_image = named_image != 0;
    task->syscall_seen = 0u;
    task->write_seen = 0u;
    task->invalid_pointer_tests = 0u;
    task->spawn_seen = 0u;
    task->wait_seen = 0u;
    task->clean_child_seen = 0u;
    task->fault_child_seen = 0u;
    task->elf_child_seen = 0u;
    task->spawn_count = 0u;
    task->exit_snapshot_valid = 0u;
    if (!paging_address_space_create(&task->address_space, 0u)) return 0;
    if (loaded_image) {
        if (!elf32_load_user_image(&task->address_space, image,
                                   image_size, &entry,
                                   &task->program_heap_start) ||
            !elf_build_initial_stack(&task->address_space, program_name, arguments,
                                     arguments_length,
                                     &user_stack_pointer)) {
            (void)paging_address_space_destroy(&task->address_space);
            return 0;
        }
        task->program_break = task->program_heap_start;
    }
    ((uint32_t *)(uintptr_t)task->address_space.data_page_physical)[1] = process_id;

    frame = (uint32_t *)(void *)(task->kernel_stack +
        SCHEDULER_KERNEL_STACK_SIZE - 13u * sizeof(uint32_t));
    for (uint32_t word = 0u; word < 8u; ++word) frame[word] = 0u;
    frame[7] = process_id;
    frame[8] = entry;
    frame[9] = USER_CODE_SELECTOR;
    frame[10] = 0x202u;
    frame[11] = user_stack_pointer;
    frame[12] = USER_DATA_SELECTOR;
    context = frame - 6;
    context[0] = 0u;
    context[1] = 0u;
    context[2] = 0u;
    context[3] = 0u;
    context[4] = 0x2u;
    context[5] = (uint32_t)(uintptr_t)scheduler_task_bootstrap;
    task->irq_context = frame;
    task->kernel_context = context;
    task->state = TASK_RUNNABLE;
    return 1;
}

static void scheduler_task_bootstrap(void) {
    struct scheduled_task *task = &scheduled_tasks[scheduler_current];
    tss_set_kernel_stack((uint32_t)(uintptr_t)
        (task->kernel_stack + SCHEDULER_KERNEL_STACK_SIZE));
    if (!paging_address_space_activate(&task->address_space)) {
        scheduler_passed = 0u;
        for (;;) __asm__ volatile ("cli; hlt");
    }
    process_restore_irq_frame(task->irq_context);
}

static void scheduler_finish(uint32_t outgoing_index) __attribute__((noreturn));

static void scheduler_finish(uint32_t outgoing_index) {
    uint32_t index;
    scheduler_passed = scheduler_passed != 0u &&
                       scheduler_ticks <= SCHEDULER_WATCHDOG_TICKS &&
                       scheduler_switches != 0u;
    for (index = 0u; index < SCHEDULER_TASK_COUNT; ++index) {
        int is_child = scheduled_tasks[index].parent_id != 0u;
        if (scheduled_tasks[index].state == TASK_UNUSED) {
            scheduler_passed = 0u;
            continue;
        }
        if (!scheduled_tasks[index].syscall_seen ||
            (is_child &&
             (scheduled_tasks[index].state != TASK_REAPED ||
              scheduled_tasks[index].address_space.directory_physical != 0u ||
              (scheduled_tasks[index].loaded_image &&
               ((!scheduled_tasks[index].named_image &&
                 scheduled_tasks[index].exit_status != 0u) ||
                (scheduled_tasks[index].named_image &&
                 scheduled_tasks[index].exit_status != 17u) ||
                !scheduled_tasks[index].write_seen)) ||
              (!scheduled_tasks[index].loaded_image &&
               scheduled_tasks[index].exit_status != 142u) ||
              !scheduled_tasks[index].exit_snapshot_valid ||
              scheduled_tasks[index].final_user_pid !=
                  scheduled_tasks[index].process_id)) ||
            (!is_child && scheduled_tasks[index].state != TASK_EXITED) ||
            (scheduled_tasks[index].boot_task &&
             (!scheduled_tasks[index].write_seen ||
              scheduled_tasks[index].invalid_pointer_tests != 3u ||
              scheduler_preempted_live[index] == 0u ||
              scheduled_tasks[index].address_space.data_page_physical == 0u ||
              ((volatile uint32_t *)(uintptr_t)
                  scheduled_tasks[index].address_space.data_page_physical)[0] <
                      SCHEDULER_USER_COUNTER_LIMIT ||
              scheduled_tasks[index].exit_status != 0u ||
              ((volatile uint32_t *)(uintptr_t)
                  scheduled_tasks[index].address_space.data_page_physical)[1] !=
                      scheduled_tasks[index].process_id))) {
            scheduler_passed = 0u;
        }
    }
    if (!scheduled_tasks[0].spawn_seen || !scheduled_tasks[0].wait_seen ||
        !scheduled_tasks[0].clean_child_seen ||
        !scheduled_tasks[0].fault_child_seen ||
        !scheduled_tasks[0].elf_child_seen ||
        scheduled_tasks[0].spawn_count != 4u ||
        scheduled_tasks[2].parent_id != scheduled_tasks[0].process_id ||
        !scheduled_tasks[2].loaded_image ||
        !scheduled_tasks[2].named_image)
        scheduler_passed = 0u;
    scheduler_active = 0u;
    if (!paging_activate_kernel()) scheduler_passed = 0u;
    tss_set_kernel_stack((uint32_t)(uintptr_t)stack_top);
    process_context_switch(&scheduled_tasks[outgoing_index].kernel_context,
                           scheduler_boot_context);
    for (;;) __asm__ volatile ("cli; hlt");
}

uint32_t *ring3_scheduler_on_timer(uint32_t *saved_context) {
    uint32_t outgoing = scheduler_current;
    uint32_t next = outgoing;
    uint32_t step;

    scheduler_history_record(ring3_scheduler_current_pid(), timer_ticks());
    if (scheduler_active == 0u || (saved_context[9] & 3u) != 3u)
        return saved_context;
    scheduled_tasks[outgoing].irq_context = saved_context;
    ++scheduler_ticks;
    if (scheduler_ticks > SCHEDULER_WATCHDOG_TICKS) {
        scheduler_passed = 0u;
        scheduler_finish(outgoing);
    }
    for (step = 1u; step <= SCHEDULER_TASK_COUNT; ++step) {
        uint32_t candidate = (outgoing + step) % SCHEDULER_TASK_COUNT;
        if (scheduled_tasks[candidate].state == TASK_RUNNABLE) {
            next = candidate;
            break;
        }
    }
    if (scheduled_tasks[next].state != TASK_RUNNABLE) scheduler_finish(outgoing);
    if (next != outgoing && scheduled_tasks[outgoing].state == TASK_RUNNABLE)
        scheduler_preempted_live[outgoing] = 1u;
    if (!paging_address_space_activate(&scheduled_tasks[next].address_space)) {
        scheduler_passed = 0u;
        scheduler_finish(outgoing);
    }
    scheduler_current = next;
    tss_set_kernel_stack((uint32_t)(uintptr_t)
        (scheduled_tasks[next].kernel_stack + SCHEDULER_KERNEL_STACK_SIZE));
    if (next != outgoing) ++scheduler_switches;
    return scheduled_tasks[next].irq_context;
}

int ring3_scheduler_run(void) {
    uint32_t flags = read_eflags();
    uint32_t index;
    static const char user_message[] = "user process started\n";
    uint32_t free_pages_before = memory_free_pages();
    int setup_ok = 1;

    __asm__ volatile ("cli" : : : "memory");
    for (index = 0u; index < 2u; ++index) {
        if (!scheduler_task_initialize(index, index + 1u, 0u,
                                       (uint32_t)(uintptr_t)scheduler_user_entry,
                                       1, 0, 0u, 0, 0, 0, 0, 0u)) {
            setup_ok = 0;
            break;
        }
        scheduler_preempted_live[index] = 0u;
        for (uint32_t byte = 0u; byte < sizeof(user_message) - 1u; ++byte)
            ((char *)(uintptr_t)scheduled_tasks[index].address_space.data_page_physical)[8u + byte] =
                user_message[byte];
    }
    for (index = 2u; index < SCHEDULER_TASK_COUNT; ++index)
        scheduled_tasks[index].state = TASK_UNUSED;
    scheduler_next_pid = 3u;
    if (!setup_ok) {
        for (index = 0u; index < SCHEDULER_TASK_COUNT; ++index)
            if (scheduled_tasks[index].address_space.directory_physical != 0u)
                (void)paging_address_space_destroy(&scheduled_tasks[index].address_space);
        if ((flags & (1u << 9)) != 0u) __asm__ volatile ("sti" : : : "memory");
        return 0;
    }

    scheduler_ticks = 0u;
    scheduler_switches = 0u;
    scheduler_passed = 1u;
    scheduler_current = 0u;
    scheduler_active = 1u;
    if (!paging_address_space_activate(&scheduled_tasks[0].address_space)) {
        scheduler_active = 0u;
        setup_ok = 0;
    } else {
        kernel_log("MiniOS: starting PIT-driven ring-3 round-robin scheduler.\n");
        process_context_switch(&scheduler_boot_context,
                               (uint32_t)(uintptr_t)scheduled_tasks[0].kernel_context);
    }

    scheduler_active = 0u;
    (void)paging_activate_kernel();
    tss_set_kernel_stack((uint32_t)(uintptr_t)stack_top);
    for (index = 0u; index < SCHEDULER_TASK_COUNT; ++index)
        (void)paging_address_space_destroy(&scheduled_tasks[index].address_space);
    if (memory_free_pages() != free_pages_before) scheduler_passed = 0u;
    if (!setup_ok) scheduler_passed = 0u;
    if (scheduler_passed != 0u) {
        kernel_log("MiniOS: scheduled process spawn, wait, exit, and reap passed.\n");
    } else {
        kernel_log("MiniOS: round-robin scheduler verification failed.\n");
    }
    if ((flags & (1u << 9)) != 0u) __asm__ volatile ("sti" : : : "memory");
    return scheduler_passed != 0u;
}

uint32_t ring3_scheduler_history_count(void) {
    return scheduler_history_count;
}

int ring3_scheduler_history_get(uint32_t newest_offset, uint32_t *process_id,
                                uint32_t *tick) {
    uint32_t count = scheduler_history_count;
    uint32_t index;
    if (newest_offset >= count || process_id == 0 || tick == 0) return 0;
    index = (scheduler_history_next + SCHEDULER_HISTORY_CAPACITY - 1u -
             newest_offset) % SCHEDULER_HISTORY_CAPACITY;
    *process_id = scheduler_history[index].process_id;
    *tick = scheduler_history[index].tick;
    return 1;
}

int ring3_scheduler_is_active(void) { return scheduler_active != 0u; }

uint32_t ring3_scheduler_current_pid(void) {
    return scheduler_active != 0u && scheduler_current < SCHEDULER_TASK_COUNT ?
        scheduled_tasks[scheduler_current].process_id : 0u;
}

uint32_t ring3_scheduler_process_state(uint32_t process_id) {
    uint32_t index;
    if (process_id == 0u) return 1u;
    for (index = 0u; index < SCHEDULER_TASK_COUNT; ++index) {
        if (scheduled_tasks[index].process_id != process_id ||
            scheduled_tasks[index].state == TASK_UNUSED) continue;
        if (scheduler_active != 0u && scheduler_current == index) return 1u;
        if (scheduled_tasks[index].state == TASK_RUNNABLE) return 2u;
        if (scheduled_tasks[index].state == TASK_WAITING) return 3u;
        if (scheduled_tasks[index].state == TASK_EXITED) return 4u;
        if (scheduled_tasks[index].state == TASK_REAPED) return 5u;
        return 0u;
    }
    return 0u;
}

#ifdef MINIOS_SCHEDULER_TEST
void ring3_scheduler_tests_exit_qemu(int passed) {
    uint32_t code = passed ? 0x10u : 0x0Fu;
    __asm__ volatile ("outl %0, %1" : : "a"(code), "Nd"((uint16_t)0xF4u));
    for (;;) __asm__ volatile ("cli; hlt");
}
#endif

#ifdef MINIOS_PROCESS_TEST
void ring3_process_tests_exit_qemu(int passed) {
    uint32_t code = passed ? 0x10u : 0x0Fu;
    __asm__ volatile ("outl %0, %1" : : "a"(code), "Nd"((uint16_t)0xF4u));
    for (;;) __asm__ volatile ("cli; hlt");
}
#endif

int ring3_user_fault_handle(struct interrupt_frame *frame) {
    uint32_t fault_address;
    uint32_t expected_address = (uint32_t)(uintptr_t)_text_start;
    if ((frame->cs & 3u) != 3u) return 0;
    if (scheduler_active != 0u) {
        uint32_t index = scheduler_current;
        frame->eip = (uint32_t)(uintptr_t)scheduler_user_terminated;
        kernel_log("MiniOS: isolated ring-3 task fault, vector=");
        kernel_log_hex(frame->vector);
        kernel_log("\n");
        scheduler_complete_task_exit(index, 128u + frame->vector, frame);
        return 1;
    }
    if (foreground_user_space != 0) {
        foreground_exit_status = 128u + frame->vector;
        kernel_log("MiniOS: standalone ELF process fault, vector=");
        kernel_log_hex(frame->vector);
        kernel_log("\n");
        ring3_leave();
    }
    if (frame->vector != 14u) return 0;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_address));
    /* Present=1, write=0, user=1: read blocked by the supervisor PTE. */
    if (fault_address != expected_address ||
        (frame->error_code & 0x7u) != 0x5u) return 0;
    isolation_reported = 1u;
    kernel_log("MiniOS: ring-3 read of supervisor kernel text was blocked by paging.\n");
    ring3_leave();
}

static void scheduler_switch_away_from_syscall(uint32_t *syscall_frame)
    __attribute__((noreturn));

static void scheduler_switch_away_from_syscall(uint32_t *syscall_frame) {
    uint32_t outgoing = scheduler_current;
    uint32_t step;
    scheduled_tasks[outgoing].irq_context = syscall_frame;
    for (step = 1u; step <= SCHEDULER_TASK_COUNT; ++step) {
        uint32_t candidate = (outgoing + step) % SCHEDULER_TASK_COUNT;
        if (scheduled_tasks[candidate].state == TASK_RUNNABLE) {
            if (!paging_address_space_activate(
                    &scheduled_tasks[candidate].address_space)) break;
            scheduler_current = candidate;
            tss_set_kernel_stack((uint32_t)(uintptr_t)
                (scheduled_tasks[candidate].kernel_stack +
                 SCHEDULER_KERNEL_STACK_SIZE));
            ++scheduler_switches;
            process_restore_irq_frame(scheduled_tasks[candidate].irq_context);
        }
    }
    scheduler_passed = 0u;
    scheduler_finish(outgoing);
}

static uint32_t scheduler_spawn(uint32_t entry, uint32_t module_index,
                                uint32_t argument_address,
                                uint32_t argument_length, int named_image) {
    uint32_t slot;
    uint32_t parent = scheduler_current;
    uint32_t process_id;
    int loaded_image = entry == 0u;
    const struct ring3_elf_module *module = 0;
    char arguments[USER_ARGUMENT_LIMIT];

    if (loaded_image) {
        if (module_index >= registered_elf_module_count)
            return 0u - MINIOS_ERR_NOENT;
        module = &registered_elf_modules[module_index];
    }
    if (argument_length > USER_ARGUMENT_LIMIT)
        return 0u - MINIOS_ERR_INVAL;
    if (argument_length == 0u) {
        if (argument_address != 0u) return 0u - MINIOS_ERR_INVAL;
    } else {
        if (argument_length < 2u) return 0u - MINIOS_ERR_INVAL;
        if (argument_address == 0u ||
            !paging_user_range_valid(&scheduled_tasks[parent].address_space,
                                     argument_address, argument_length, 0))
            return 0u - MINIOS_ERR_FAULT;
        for (uint32_t i = 0u; i < argument_length; ++i) {
            arguments[i] = *(const char *)(uintptr_t)(argument_address + i);
        }
        if (arguments[argument_length - 1u] != '\0' ||
            arguments[argument_length - 2u] != '\0')
            return 0u - MINIOS_ERR_INVAL;
        {
            uint32_t argc = 1u;
            for (uint32_t i = 0u; i + 1u < argument_length; ++i) {
                if (arguments[i] == '\0') {
                    if (i == 0u || arguments[i - 1u] == '\0')
                        return 0u - MINIOS_ERR_INVAL;
                    ++argc;
                    if (argc >= USER_ARGC_LIMIT && i + 1u < argument_length - 1u)
                        return 0u - MINIOS_ERR_INVAL;
                }
            }
        }
        if (!loaded_image) return 0u - MINIOS_ERR_INVAL;
    }
    if (!loaded_image &&
        (entry < (uint32_t)(uintptr_t)_user_text_start ||
         entry >= (uint32_t)(uintptr_t)_user_text_end))
        return 0u - MINIOS_ERR_INVAL;
    for (slot = 0u; slot < SCHEDULER_TASK_COUNT; ++slot)
        if (scheduled_tasks[slot].state == TASK_UNUSED ||
            (scheduled_tasks[slot].state == TASK_REAPED &&
             scheduled_tasks[slot].address_space.directory_physical == 0u))
            break;
    if (slot == SCHEDULER_TASK_COUNT) return 0u - MINIOS_ERR_AGAIN;
    if (!paging_activate_kernel()) return 0u - MINIOS_ERR_NOMEM;
    process_id = scheduler_next_pid++;
    if (!scheduler_task_initialize(slot, process_id,
                                   scheduled_tasks[parent].process_id,
                                   entry, 0,
                                   loaded_image ? module->image : 0,
                                   loaded_image ? module->image_size : 0u,
                                   loaded_image,
                                   named_image,
                                   loaded_image ? module->name : 0,
                                   argument_length != 0u ? arguments : 0,
                                   argument_length)) {
        if (!paging_address_space_activate(
                &scheduled_tasks[parent].address_space))
            scheduler_passed = 0u;
        return 0u - MINIOS_ERR_NOMEM;
    }
    scheduled_tasks[parent].spawn_seen = 1u;
    ++scheduled_tasks[parent].spawn_count;
    if (!paging_address_space_activate(&scheduled_tasks[parent].address_space)) {
        scheduler_passed = 0u;
        return 0u - MINIOS_ERR_NOMEM;
    }
    kernel_log("MiniOS: ring-3 task spawned, pid=");
    kernel_log_hex(process_id);
    kernel_log("\n");
    return process_id;
}

static int module_name_character_valid(char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '-' ||
           character == '_' || character == '.';
}

static uint32_t scheduler_spawn_named(uint32_t name_address,
                                      uint32_t argument_address,
                                      uint32_t argument_length) {
    struct paging_address_space *parent_space =
        &scheduled_tasks[scheduler_current].address_space;
    char name[MINI_OS_MODULE_NAME_CAPACITY];
    uint32_t name_length;
    uint32_t module_index;

    if (name_address == 0u) return 0u - MINIOS_ERR_FAULT;
    for (name_length = 0u; name_length < MINI_OS_MODULE_NAME_CAPACITY;
         ++name_length) {
        uint64_t current = (uint64_t)name_address + name_length;
        char character;
        if (current > 0xFFFFFFFFu ||
            !paging_user_range_valid(parent_space, (uint32_t)current, 1u, 0))
            return 0u - MINIOS_ERR_FAULT;
        character = *(const char *)(uintptr_t)(uint32_t)current;
        if (character == '\0') break;
        if (!module_name_character_valid(character))
            return 0u - MINIOS_ERR_INVAL;
        name[name_length] = character;
    }
    if (name_length == 0u || name_length >= MINI_OS_MODULE_NAME_CAPACITY)
        return 0u - MINIOS_ERR_INVAL;
    name[name_length] = '\0';

    for (module_index = 0u; module_index < registered_elf_module_count;
         ++module_index) {
        uint32_t character = 0u;
        while (name[character] == registered_elf_modules[module_index].name[character] &&
               name[character] != '\0') ++character;
        if (name[character] == registered_elf_modules[module_index].name[character])
            return scheduler_spawn(0u, module_index, argument_address,
                                   argument_length, 1);
    }
    kernel_log("MiniOS: requested ELF module was not found: ");
    kernel_log(name);
    kernel_log("\n");
    return 0u - MINIOS_ERR_NOENT;
}

static void scheduler_snapshot_exit(uint32_t index) {
    struct scheduled_task *task = &scheduled_tasks[index];
    volatile uint32_t *data = (volatile uint32_t *)(uintptr_t)
        task->address_space.data_page_physical;
    task->final_data0 = data[0];
    task->final_user_pid = data[1];
    task->exit_snapshot_valid = 1u;
}

static int scheduler_reap(uint32_t index) {
    struct scheduled_task *task = &scheduled_tasks[index];
    if (!task->exit_snapshot_valid) scheduler_snapshot_exit(index);
    if (task->address_space.directory_physical != 0u &&
        !paging_address_space_destroy(&task->address_space)) return 0;
    task->state = TASK_REAPED;
    kernel_log("MiniOS: child reaped; user address space reclaimed.\n");
    return 1;
}

static void scheduler_complete_task_exit(uint32_t index, uint32_t status,
                                         struct interrupt_frame *frame) {
    uint32_t parent_index;
    struct scheduled_task *task = &scheduled_tasks[index];
    scheduler_snapshot_exit(index);
    task->state = TASK_EXITED;
    task->exit_status = status;
    frame->eax = 0u;
    frame->eip = (uint32_t)(uintptr_t)scheduler_user_terminated;

    for (parent_index = 0u; parent_index < SCHEDULER_TASK_COUNT;
         ++parent_index) {
        struct scheduled_task *parent = &scheduled_tasks[parent_index];
        if (parent->state == TASK_WAITING &&
            parent->waiting_for == task->process_id) {
            struct interrupt_frame *parent_frame =
                (struct interrupt_frame *)(void *)parent->irq_context;
            parent_frame->eax = status;
            parent->waiting_for = 0u;
            parent->wait_seen = 1u;
            if (status == 42u) parent->clean_child_seen = 1u;
            if (status == 142u) parent->fault_child_seen = 1u;
            if (task->loaded_image) parent->elf_child_seen = 1u;
            parent->state = TASK_RUNNABLE;
            if (!paging_address_space_activate(&parent->address_space)) {
                scheduler_passed = 0u;
                return;
            }
            scheduler_current = parent_index;
            tss_set_kernel_stack((uint32_t)(uintptr_t)
                (parent->kernel_stack + SCHEDULER_KERNEL_STACK_SIZE));
            if (!scheduler_reap(index)) scheduler_passed = 0u;
            process_restore_syscall_frame(parent->irq_context);
        }
    }
}

static int scheduler_wait(uint32_t child_id,
                          struct interrupt_frame *frame) {
    uint32_t child;
    uint32_t parent = scheduler_current;
    for (child = 0u; child < SCHEDULER_TASK_COUNT; ++child) {
        struct scheduled_task *task = &scheduled_tasks[child];
        if (task->process_id != child_id ||
            task->parent_id != scheduled_tasks[parent].process_id) continue;
        if (task->state == TASK_EXITED) {
            frame->eax = task->exit_status;
            if (task->exit_status == 42u)
                scheduled_tasks[parent].clean_child_seen = 1u;
            if (task->exit_status == 142u)
                scheduled_tasks[parent].fault_child_seen = 1u;
            if (task->loaded_image)
                scheduled_tasks[parent].elf_child_seen = 1u;
            if (!scheduler_reap(child)) {
                frame->eax = 0u - MINIOS_ERR_NOMEM;
                return 0;
            }
            scheduled_tasks[parent].wait_seen = 1u;
            return 1;
        }
        if (task->state != TASK_RUNNABLE && task->state != TASK_WAITING)
            break;
        scheduled_tasks[parent].waiting_for = child_id;
        scheduled_tasks[parent].state = TASK_WAITING;
        scheduler_switch_away_from_syscall((uint32_t *)(void *)frame);
    }
    frame->eax = 0u - MINIOS_ERR_CHILD;
    return 0;
}

void ring3_syscall_dispatch(struct interrupt_frame *frame) {
    uint32_t syscall_number = frame->eax;
    uint32_t argument0 = frame->ebx;
    uint32_t argument1 = frame->ecx;
    uint32_t argument2 = frame->edx;
    if ((frame->cs & 3u) != 3u) {
        kernel_log("MiniOS: rejected syscall from a non-user context.\n");
        return;
    }
    if (scheduler_active != 0u) {
        uint32_t index = scheduler_current;
        if (frame->eax == MINIOS_SYS_GETPID &&
            scheduled_tasks[index].state == TASK_RUNNABLE) {
            scheduled_tasks[index].syscall_seen = 1u;
            frame->eax = scheduled_tasks[index].process_id;
        } else if (frame->eax == MINIOS_SYS_WRITE &&
                   scheduled_tasks[index].state == TASK_RUNNABLE) {
            if (frame->ebx != 1u) {
                frame->eax = 0u - MINIOS_ERR_BADF;
            } else if (frame->edx > 256u) {
                frame->eax = 0u - MINIOS_ERR_INVAL;
            } else if (!paging_user_range_valid(
                           &scheduled_tasks[index].address_space,
                           frame->ecx, frame->edx, 0)) {
                if (frame->ecx == (uint32_t)(uintptr_t)_text_start ||
                    frame->ecx == 0x40000FFFu ||
                    frame->ecx == 0xFFFFFFFEu)
                    ++scheduled_tasks[index].invalid_pointer_tests;
                frame->eax = 0u - MINIOS_ERR_FAULT;
            } else {
                kernel_log_buffer((const char *)(uintptr_t)frame->ecx,
                                  frame->edx);
                scheduled_tasks[index].write_seen = 1u;
                frame->eax = frame->edx;
            }
        } else if (frame->eax == MINIOS_SYS_SPAWN &&
                   scheduled_tasks[index].state == TASK_RUNNABLE) {
            frame->eax = scheduler_spawn(frame->ecx, 0u, frame->ebx,
                                          frame->edx, 0);
        } else if (frame->eax == MINIOS_SYS_SPAWN_NAMED &&
                   scheduled_tasks[index].state == TASK_RUNNABLE) {
            frame->eax = scheduler_spawn_named(frame->ecx, frame->ebx,
                                               frame->edx);
        } else if (frame->eax == MINIOS_SYS_WAIT &&
                   scheduled_tasks[index].state == TASK_RUNNABLE) {
            (void)scheduler_wait(frame->ebx, frame);
        } else if (frame->eax == MINIOS_SYS_BRK &&
                   scheduled_tasks[index].state == TASK_RUNNABLE &&
                   scheduled_tasks[index].loaded_image) {
            frame->eax = elf_set_program_break(
                &scheduled_tasks[index].address_space,
                scheduled_tasks[index].program_heap_start,
                &scheduled_tasks[index].program_break, frame->ebx);
        } else if (frame->eax == MINIOS_SYS_EXIT &&
                   scheduled_tasks[index].state == TASK_RUNNABLE) {
            scheduler_complete_task_exit(index, frame->ebx, frame);
        } else {
            scheduler_passed = 0u;
            frame->eax = 0u - MINIOS_ERR_NOSYS;
        }
        if (syscall_number != MINIOS_SYS_SMOKE_VERIFY)
            resource_monitor_user_syscall(scheduled_tasks[index].process_id,
                syscall_number, argument0, argument1, argument2, frame->eax);
        return;
    }
    if (frame->eax == MINIOS_SYS_SMOKE_VERIFY) {
        if (frame->ebx == expected_user_value &&
            *(volatile uint32_t *)(uintptr_t)USER_DATA_ADDRESS == expected_user_value) {
            smoke_reported = 1u;
            kernel_log("MiniOS: ring-3 code, stack, writable data, and int 0x80 verified.\n");
            frame->eax = 0u;
        } else {
            frame->eax = 1u;
            kernel_log("MiniOS: ring-3 smoke task data check failed.\n");
        }
    } else if (frame->eax == MINIOS_SYS_WRITE &&
               foreground_user_space != 0) {
        if (frame->ebx != 1u) frame->eax = 0u - MINIOS_ERR_BADF;
        else if (frame->edx > 256u) frame->eax = 0u - MINIOS_ERR_INVAL;
        else if (!paging_user_range_valid(foreground_user_space,
                                          frame->ecx, frame->edx, 0))
            frame->eax = 0u - MINIOS_ERR_FAULT;
        else {
            kernel_log_buffer((const char *)(uintptr_t)frame->ecx, frame->edx);
            foreground_write_seen = 1u;
            frame->eax = frame->edx;
        }
    } else if (frame->eax == MINIOS_SYS_BRK &&
               foreground_user_space != 0) {
        frame->eax = elf_set_program_break(foreground_user_space,
            foreground_program_heap_start, &foreground_program_break,
            frame->ebx);
    } else if (frame->eax == MINIOS_SYS_EXIT) {
        foreground_exit_status = frame->ebx;
        kernel_log("MiniOS: ring-3 smoke task requested exit.\n");
        resource_monitor_user_syscall(1u, syscall_number, argument0,
            argument1, argument2, 0u);
        ring3_leave();
    } else if (frame->eax == MINIOS_SYS_GETPID) {
        if (foreground_user_space != 0) foreground_pid_seen = 1u;
        frame->eax = 1u;
    } else {
        frame->eax = 0u - MINIOS_ERR_NOSYS;
    }
    if (syscall_number != MINIOS_SYS_SMOKE_VERIFY)
        resource_monitor_user_syscall(1u, syscall_number, argument0,
            argument1, argument2, frame->eax);
}
