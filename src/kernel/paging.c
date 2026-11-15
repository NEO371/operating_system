#include "paging.h"

#include "interrupts.h"
#include "memory.h"

#include <stdint.h>

#define PAGE_SIZE 4096u
#define PAGE_SHIFT 12u
#define TABLE_ENTRIES 1024u
#define DIRECTORY_COUNT 1024u
#define PTE_PRESENT 0x001u
#define PTE_WRITABLE 0x002u
#define PTE_WRITE_THROUGH 0x008u
#define PTE_CACHE_DISABLE 0x010u
#define PTE_USER 0x004u
#define USER_DATA_ADDRESS 0x40000000u
#define USER_STACK_PAGE_ADDRESS 0xBFFFD000u

static uint32_t page_table_physical[DIRECTORY_COUNT];
static uint32_t *page_directory;
static uint32_t page_directory_physical;
static uint8_t paging_ready;

extern uint8_t _text_start[];
extern uint8_t _text_end[];
extern uint8_t _rodata_start[];
extern uint8_t _rodata_end[];
extern uint8_t _user_text_start[], _user_text_end[];
static uint32_t active_directory_physical;

static int range_overlaps(uint32_t start, uint32_t end,
                          const uint8_t *low_symbol,
                          const uint8_t *high_symbol) {
    uint32_t low = (uint32_t)(uintptr_t)low_symbol;
    uint32_t high = (uint32_t)(uintptr_t)high_symbol;
    return start < high && low < end;
}

static int is_user_page(uint32_t address) {
    return range_overlaps(address, address + PAGE_SIZE,
                          _user_text_start, _user_text_end);
}

static int is_user_text_page(uint32_t address) {
    return range_overlaps(address, address + PAGE_SIZE,
                          _user_text_start, _user_text_end);
}

static int is_user_directory(uint32_t index) {
    uint32_t start = index << 22;
    uint32_t end = start + 0x00400000u;
    return range_overlaps(start, end, _user_text_start, _user_text_end);
}

static int map_private_user_page(struct paging_address_space *space,
                                 uint32_t address, uint32_t *table_physical,
                                 uint32_t *page_physical) {
    uint32_t directory_index = address >> 22;
    uint32_t table_index = (address >> PAGE_SHIFT) & (TABLE_ENTRIES - 1u);
    uint32_t *table;

    if (((uint32_t *)(uintptr_t)space->directory_physical)[directory_index] != 0u) {
        return 0;
    }
    *table_physical = memory_alloc_page();
    *page_physical = memory_alloc_page();
    if (*table_physical == 0u || *page_physical == 0u) return 0;
    table = (uint32_t *)(uintptr_t)*table_physical;
    table[table_index] = *page_physical | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    ((uint32_t *)(uintptr_t)space->directory_physical)[directory_index] =
        *table_physical | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    return 1;
}

static int is_read_only_kernel_page(uint32_t address) {
    uint32_t text_start = (uint32_t)(uintptr_t)_text_start;
    uint32_t text_end = (uint32_t)(uintptr_t)_text_end;
    uint32_t rodata_start = (uint32_t)(uintptr_t)_rodata_start;
    uint32_t rodata_end = (uint32_t)(uintptr_t)_rodata_end;
    return (address >= text_start && address < text_end) ||
           (address >= rodata_start && address < rodata_end);
}

static int is_framebuffer_page(const struct multiboot_info *info,
                               uint32_t address) {
    uint64_t start;
    uint64_t end;
    uint64_t page = address;

    if ((info->flags & (1u << 12)) == 0u ||
        (info->framebuffer_addr >> 32) != 0u) return 0;
    start = info->framebuffer_addr & ~(uint64_t)(PAGE_SIZE - 1u);
    end = info->framebuffer_addr +
          (uint64_t)info->framebuffer_pitch * info->framebuffer_height;
    if (end < info->framebuffer_addr) return 0;
    end = (end + PAGE_SIZE - 1u) & ~(uint64_t)(PAGE_SIZE - 1u);
    return page >= start && page < end;
}

static uint32_t *allocate_page_table(uint32_t directory_index) {
    uint32_t physical = memory_alloc_page();
    if (physical == 0u) return 0;
    page_table_physical[directory_index] = physical;
    page_directory[directory_index] = physical | PTE_PRESENT | PTE_WRITABLE |
        (is_user_directory(directory_index) ? PTE_USER : 0u);
    return (uint32_t *)(uintptr_t)physical;
}

static int allocate_required_tables(void) {
    uint32_t page = 1u;
    uint32_t present_page;
    while (memory_next_mapped_page(page, &present_page)) {
        uint32_t directory_index = present_page >> 10;
        if (page_table_physical[directory_index] == 0u &&
            allocate_page_table(directory_index) == 0) return 0;
        page = present_page + 1u;
    }
    return 1;
}

static void release_paging_structures(void) {
    uint32_t index;
    for (index = 0u; index < DIRECTORY_COUNT; ++index) {
        if (page_table_physical[index] != 0u) {
            memory_free_page(page_table_physical[index]);
            page_table_physical[index] = 0u;
        }
    }
    if (page_directory_physical != 0u) {
        memory_free_page(page_directory_physical);
        page_directory_physical = 0u;
    }
    page_directory = 0;
}

static void populate_tables(const struct multiboot_info *info) {
    uint32_t page = 1u;
    while (memory_next_mapped_page(page, &page)) {
        uint32_t address = page << PAGE_SHIFT;
        uint32_t directory_index = page >> 10;
        uint32_t table_index = page & (TABLE_ENTRIES - 1u);
        uint32_t *table;
        uint32_t flags = PTE_PRESENT;

        if (!memory_page_is_mapped(address)) continue;
        table = (uint32_t *)(uintptr_t)page_table_physical[directory_index];
        if (is_framebuffer_page(info, address)) {
            flags |= PTE_WRITABLE | PTE_WRITE_THROUGH | PTE_CACHE_DISABLE;
        } else if (!is_read_only_kernel_page(address) &&
                   !is_user_text_page(address)) {
            flags |= PTE_WRITABLE;
        }
        if (is_user_page(address)) flags |= PTE_USER;
        table[table_index] = address | flags;
        ++page;
    }
}

int paging_init(const struct multiboot_info *info) {
    uint32_t index;
    uint32_t cr0;
    volatile uint32_t *directory;

    if (!memory_is_ready() || info == 0) return 0;
    for (index = 0u; index < DIRECTORY_COUNT; ++index) {
        page_table_physical[index] = 0u;
    }
    page_directory_physical = memory_alloc_page();
    if (page_directory_physical == 0u) return 0;
    page_directory = (uint32_t *)(uintptr_t)page_directory_physical;
    directory = (volatile uint32_t *)page_directory;
    for (index = 0u; index < DIRECTORY_COUNT; ++index) directory[index] = 0u;

    if (!allocate_required_tables()) {
        kernel_log("MiniOS: could not allocate page tables; paging remains off.\n");
        release_paging_structures();
        return 0;
    }
    populate_tables(info);

    /* CR0.WP makes supervisor writes respect the read-only kernel mappings. */
    __asm__ volatile ("mov %0, %%cr3" : : "r"(page_directory_physical) : "memory");
    active_directory_physical = page_directory_physical;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1u << 31) | (1u << 16);
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
    paging_ready = 1u;
    kernel_log("MiniOS: paging enabled; null page unmapped; kernel text/rodata read-only.\n");
    return 1;
}

int paging_is_ready(void) { return paging_ready != 0u; }

int paging_address_space_create(struct paging_address_space *space,
                                uint32_t initial_data) {
    uint32_t index;
    uint32_t *directory;
    if (space == 0 || !paging_ready || active_directory_physical != page_directory_physical) return 0;
    space->directory_physical = 0u;
    space->data_table_physical = 0u;
    space->stack_table_physical = 0u;
    space->data_page_physical = 0u;
    space->stack_page_physical = 0u;
    space->program_table_count = 0u;
    space->program_page_count = 0u;
    for (index = 0u; index < PAGING_USER_PROGRAM_TABLE_LIMIT; ++index) {
        space->program_table_physical[index] = 0u;
        space->program_table_directory_index[index] = 0u;
    }
    for (index = 0u; index < PAGING_USER_PROGRAM_PAGE_LIMIT; ++index) {
        space->program_page_physical[index] = 0u;
        space->program_page_virtual[index] = 0u;
    }

    space->directory_physical = memory_alloc_page();
    if (space->directory_physical == 0u) return 0;
    directory = (uint32_t *)(uintptr_t)space->directory_physical;
    for (index = 0u; index < DIRECTORY_COUNT; ++index) {
        directory[index] = page_directory[index];
    }
    if (!map_private_user_page(space, USER_DATA_ADDRESS,
                               &space->data_table_physical,
                               &space->data_page_physical) ||
        !map_private_user_page(space, USER_STACK_PAGE_ADDRESS,
                               &space->stack_table_physical,
                               &space->stack_page_physical)) {
        (void)paging_address_space_destroy(space);
        return 0;
    }
    *(volatile uint32_t *)(uintptr_t)space->data_page_physical = initial_data;
    return 1;
}

int paging_address_space_activate(const struct paging_address_space *space) {
    if (space == 0 || space->directory_physical == 0u || !paging_ready) return 0;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(space->directory_physical) : "memory");
    active_directory_physical = space->directory_physical;
    return 1;
}

int paging_activate_kernel(void) {
    if (!paging_ready || page_directory_physical == 0u) return 0;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(page_directory_physical) : "memory");
    active_directory_physical = page_directory_physical;
    return 1;
}

int paging_address_space_destroy(struct paging_address_space *space) {
    uint32_t index;
    if (space == 0 || space->directory_physical == 0u ||
        active_directory_physical == space->directory_physical) return 0;
    if (space->data_page_physical != 0u) (void)memory_free_page(space->data_page_physical);
    if (space->stack_page_physical != 0u) (void)memory_free_page(space->stack_page_physical);
    if (space->data_table_physical != 0u) (void)memory_free_page(space->data_table_physical);
    if (space->stack_table_physical != 0u) (void)memory_free_page(space->stack_table_physical);
    for (index = 0u; index < space->program_page_count; ++index)
        if (space->program_page_physical[index] != 0u)
            (void)memory_free_page(space->program_page_physical[index]);
    for (index = 0u; index < space->program_table_count; ++index)
        if (space->program_table_physical[index] != 0u)
            (void)memory_free_page(space->program_table_physical[index]);
    (void)memory_free_page(space->directory_physical);
    space->directory_physical = 0u;
    space->data_table_physical = 0u;
    space->stack_table_physical = 0u;
    space->data_page_physical = 0u;
    space->stack_page_physical = 0u;
    for (index = 0u; index < PAGING_USER_PROGRAM_TABLE_LIMIT; ++index) {
        space->program_table_physical[index] = 0u;
        space->program_table_directory_index[index] = 0u;
    }
    for (index = 0u; index < PAGING_USER_PROGRAM_PAGE_LIMIT; ++index) {
        space->program_page_physical[index] = 0u;
        space->program_page_virtual[index] = 0u;
    }
    space->program_table_count = 0u;
    space->program_page_count = 0u;
    return 1;
}

int paging_address_space_map_user_page(struct paging_address_space *space,
                                       uint32_t virtual_address,
                                       int writable,
                                       uint32_t *physical_address) {
    uint32_t page_address = virtual_address & ~(PAGE_SIZE - 1u);
    uint32_t directory_index = page_address >> 22;
    uint32_t table_index = (page_address >> PAGE_SHIFT) & (TABLE_ENTRIES - 1u);
    uint32_t *directory;
    uint32_t table_slot;
    uint32_t page_slot;
    uint32_t table_physical = 0u;
    uint32_t page_physical;
    uint32_t *table;

    if (space == 0 || space->directory_physical == 0u ||
        active_directory_physical != page_directory_physical ||
        (virtual_address & (PAGE_SIZE - 1u)) != 0u ||
        page_address < 0x00400000u || page_address >= USER_DATA_ADDRESS ||
        page_address == USER_STACK_PAGE_ADDRESS)
        return 0;
    for (page_slot = 0u; page_slot < space->program_page_count; ++page_slot) {
        if (space->program_page_virtual[page_slot] == page_address) {
            uint32_t existing_table;
            for (existing_table = 0u;
                 existing_table < space->program_table_count;
                 ++existing_table) {
                if (space->program_table_directory_index[existing_table] ==
                    directory_index) {
                    table_physical =
                        space->program_table_physical[existing_table];
                    table = (uint32_t *)(uintptr_t)table_physical;
                    if (writable) table[table_index] |= PTE_WRITABLE;
                    if (physical_address != 0)
                        *physical_address = space->program_page_physical[page_slot];
                    return 1;
                }
            }
            return 0;
        }
    }
    if (directory_index == (USER_DATA_ADDRESS >> 22) ||
        directory_index == (USER_STACK_PAGE_ADDRESS >> 22)) return 0;
    if (space->program_page_count >= PAGING_USER_PROGRAM_PAGE_LIMIT) return 0;

    for (table_slot = 0u; table_slot < space->program_table_count; ++table_slot)
        if (space->program_table_directory_index[table_slot] == directory_index) {
            table_physical = space->program_table_physical[table_slot];
            break;
        }
    page_physical = memory_alloc_page();
    if (page_physical == 0u) return 0;
    if (table_physical == 0u) {
        uint32_t old_entry;
        uint32_t *new_table;
        if (space->program_table_count >= PAGING_USER_PROGRAM_TABLE_LIMIT) {
            (void)memory_free_page(page_physical);
            return 0;
        }
        table_physical = memory_alloc_page();
        if (table_physical == 0u) {
            (void)memory_free_page(page_physical);
            return 0;
        }
        directory = (uint32_t *)(uintptr_t)space->directory_physical;
        old_entry = directory[directory_index];
        new_table = (uint32_t *)(uintptr_t)table_physical;
        if ((old_entry & PTE_PRESENT) != 0u) {
            const uint32_t *old_table = (const uint32_t *)(uintptr_t)
                (old_entry & 0xFFFFF000u);
            for (uint32_t entry = 0u; entry < TABLE_ENTRIES; ++entry)
                new_table[entry] = old_table[entry];
        }
        directory[directory_index] = table_physical | PTE_PRESENT |
                                     PTE_WRITABLE | PTE_USER;
        table_slot = space->program_table_count++;
        space->program_table_physical[table_slot] = table_physical;
        space->program_table_directory_index[table_slot] = directory_index;
    }
    table = (uint32_t *)(uintptr_t)table_physical;
    if ((table[table_index] & PTE_PRESENT) != 0u) {
        (void)memory_free_page(page_physical);
        return 0;
    }
    table[table_index] = page_physical | PTE_PRESENT | PTE_USER |
                         (writable ? PTE_WRITABLE : 0u);
    page_slot = space->program_page_count++;
    space->program_page_physical[page_slot] = page_physical;
    space->program_page_virtual[page_slot] = page_address;
    if (physical_address != 0) *physical_address = page_physical;
    return 1;
}

int paging_address_space_unmap_user_page(struct paging_address_space *space,
                                         uint32_t virtual_address) {
    uint32_t page_address = virtual_address & ~(PAGE_SIZE - 1u);
    uint32_t directory_index = page_address >> 22;
    uint32_t table_index = (page_address >> PAGE_SHIFT) & (TABLE_ENTRIES - 1u);
    uint32_t page_slot;
    uint32_t table_slot;
    uint32_t table_physical = 0u;
    uint32_t *table;

    if (space == 0 || space->directory_physical == 0u ||
        active_directory_physical != page_directory_physical ||
        (virtual_address & (PAGE_SIZE - 1u)) != 0u) return 0;
    for (page_slot = 0u; page_slot < space->program_page_count; ++page_slot)
        if (space->program_page_virtual[page_slot] == page_address) break;
    if (page_slot == space->program_page_count) return 0;
    for (table_slot = 0u; table_slot < space->program_table_count; ++table_slot)
        if (space->program_table_directory_index[table_slot] == directory_index) {
            table_physical = space->program_table_physical[table_slot];
            break;
        }
    if (table_physical == 0u) return 0;
    table = (uint32_t *)(uintptr_t)table_physical;
    if ((table[table_index] & PTE_PRESENT) == 0u ||
        (table[table_index] & 0xFFFFF000u) !=
            space->program_page_physical[page_slot]) return 0;
    if (!memory_free_page(space->program_page_physical[page_slot])) return 0;
    table[table_index] = 0u;
    --space->program_page_count;
    space->program_page_physical[page_slot] =
        space->program_page_physical[space->program_page_count];
    space->program_page_virtual[page_slot] =
        space->program_page_virtual[space->program_page_count];
    space->program_page_physical[space->program_page_count] = 0u;
    space->program_page_virtual[space->program_page_count] = 0u;
    return 1;
}

int paging_user_range_valid(const struct paging_address_space *space,
                            uint32_t address, uint32_t length,
                            int write_access) {
    uint64_t end = (uint64_t)address + length;
    uint64_t page;
    const uint32_t *directory;

    if (space == 0 || space->directory_physical == 0u ||
        end > 0x100000000ull) return 0;
    if (length == 0u) return 1;
    directory = (const uint32_t *)(uintptr_t)space->directory_physical;
    page = (uint64_t)address & ~(uint64_t)(PAGE_SIZE - 1u);
    while (page < end) {
        uint32_t virtual_address = (uint32_t)page;
        uint32_t directory_index = virtual_address >> 22;
        uint32_t table_index = (virtual_address >> PAGE_SHIFT) &
                               (TABLE_ENTRIES - 1u);
        uint32_t directory_entry = directory[directory_index];
        const uint32_t *table;
        uint32_t page_entry;

        if ((directory_entry & (PTE_PRESENT | PTE_USER)) !=
            (PTE_PRESENT | PTE_USER)) return 0;
        table = (const uint32_t *)(uintptr_t)(directory_entry & 0xFFFFF000u);
        page_entry = table[table_index];
        if ((page_entry & (PTE_PRESENT | PTE_USER)) !=
            (PTE_PRESENT | PTE_USER)) return 0;
        if (write_access && (page_entry & PTE_WRITABLE) == 0u) return 0;
        page += PAGE_SIZE;
    }
    return 1;
}
