#include "memory.h"

#include "kernel.h"
#include "interrupts.h"

#include <stddef.h>

#define PAGE_SHIFT 12u
#define PAGE_SIZE (1u << PAGE_SHIFT)
#define MAX_PHYSICAL_PAGES (1u << 20)
#define PAGE_STATE_BYTES (MAX_PHYSICAL_PAGES / 4u)
#define FOUR_GIB (1ULL << 32)
#define MULTIBOOT_INFO_MEMORY_MAP (1u << 6)
#define MULTIBOOT_INFO_MODULES (1u << 3)
#define MULTIBOOT_INFO_FRAMEBUFFER (1u << 12)

enum page_state {
    PAGE_UNAVAILABLE = 0u,
    PAGE_FREE = 1u,
    PAGE_ALLOCATED = 2u,
    PAGE_RESERVED = 3u
};

struct __attribute__((packed)) multiboot_mmap_entry {
    uint32_t size;
    uint64_t address;
    uint64_t length;
    uint32_t type;
};

static uint8_t page_states[PAGE_STATE_BYTES];
static uint32_t free_pages;
static uint32_t managed_pages;
static uint32_t allocation_hint;
static uint8_t ready;

static uint8_t get_page_state(uint32_t page) {
    uint32_t byte = page >> 2;
    uint32_t shift = (page & 3u) * 2u;
    return (uint8_t)((page_states[byte] >> shift) & 3u);
}

static void set_page_state(uint32_t page, uint8_t state) {
    uint32_t byte = page >> 2;
    uint32_t shift = (page & 3u) * 2u;
    uint8_t mask = (uint8_t)(3u << shift);
    page_states[byte] = (uint8_t)((page_states[byte] & (uint8_t)~mask) |
                                  (uint8_t)(state << shift));
}

static void reserve_range(uint64_t address, uint64_t length) {
    uint64_t end;
    uint32_t first_page;
    uint32_t end_page;
    uint32_t page;

    if (length == 0u || address >= FOUR_GIB) return;
    end = address + length;
    if (end < address || end > FOUR_GIB) end = FOUR_GIB;
    first_page = (uint32_t)(address >> PAGE_SHIFT);
    end_page = (uint32_t)((end + PAGE_SIZE - 1u) >> PAGE_SHIFT);
    if (end_page > MAX_PHYSICAL_PAGES) end_page = MAX_PHYSICAL_PAGES;
    for (page = first_page; page < end_page; ++page) {
        uint8_t state = get_page_state(page);
        if (state == PAGE_FREE) --free_pages;
        if (state != PAGE_ALLOCATED) set_page_state(page, PAGE_RESERVED);
    }
}

static void release_usable_range(uint64_t address, uint64_t length) {
    uint64_t end;
    uint64_t first_address;
    uint32_t first_page;
    uint32_t end_page;
    uint32_t page;

    if (length == 0u || address >= FOUR_GIB ||
        address > UINT64_MAX - length) return;
    end = address + length;
    if (end > FOUR_GIB) end = FOUR_GIB;
    first_address = (address + PAGE_SIZE - 1u) & ~(uint64_t)(PAGE_SIZE - 1u);
    end &= ~(uint64_t)(PAGE_SIZE - 1u);
    if (end <= first_address) return;
    first_page = (uint32_t)(first_address >> PAGE_SHIFT);
    end_page = (uint32_t)(end >> PAGE_SHIFT);
    if (end_page > MAX_PHYSICAL_PAGES) end_page = MAX_PHYSICAL_PAGES;
    for (page = first_page; page < end_page; ++page) {
        if (get_page_state(page) == PAGE_UNAVAILABLE) {
            set_page_state(page, PAGE_FREE);
            ++managed_pages;
            ++free_pages;
        }
    }
}

static int clear_page_states(void) {
    uint32_t index;
    for (index = 0u; index < PAGE_STATE_BYTES; ++index) {
        page_states[index] = 0u;
    }
    free_pages = 0u;
    managed_pages = 0u;
    allocation_hint = 1u;
    ready = 0u;
    return 1;
}

static int consume_memory_map(const struct multiboot_info *info) {
    uint64_t cursor;
    uint64_t map_end;
    uint32_t entries = 0u;

    if ((info->flags & MULTIBOOT_INFO_MEMORY_MAP) == 0u ||
        info->mmap_length == 0u || info->mmap_addr == 0u) return 0;
    cursor = info->mmap_addr;
    map_end = cursor + info->mmap_length;
    if (map_end > FOUR_GIB || map_end < cursor) return 0;

    while (cursor < map_end) {
        const struct multiboot_mmap_entry *entry;
        uint64_t next;
        uint32_t payload_size;

        if (map_end - cursor < sizeof(uint32_t)) return 0;
        entry = (const struct multiboot_mmap_entry *)(uintptr_t)(uint32_t)cursor;
        payload_size = entry->size;
        if (payload_size < 20u || payload_size > map_end - cursor - sizeof(uint32_t)) {
            return 0;
        }
        if (entry->type == 1u) release_usable_range(entry->address, entry->length);
        next = cursor + sizeof(uint32_t) + payload_size;
        if (next <= cursor) return 0;
        cursor = next;
        ++entries;
    }
    return cursor == map_end && entries != 0u;
}

static int reserve_multiboot_modules(const struct multiboot_info *info) {
    uint64_t descriptors_end;
    uint32_t index;
    const struct multiboot_module *modules;

    if ((info->flags & MULTIBOOT_INFO_MODULES) == 0u || info->mods_count == 0u) return 1;
    if (info->mods_addr == 0u) return 0;
    descriptors_end = (uint64_t)info->mods_addr +
                      (uint64_t)info->mods_count * sizeof(*modules);
    if (descriptors_end > FOUR_GIB) return 0;
    modules = (const struct multiboot_module *)(uintptr_t)info->mods_addr;
    reserve_range(info->mods_addr, descriptors_end - info->mods_addr);
    if (info->cmdline != 0u) reserve_range(info->cmdline, 256u);
    if (info->boot_loader_name != 0u)
        reserve_range(info->boot_loader_name, 128u);
    for (index = 0u; index < info->mods_count; ++index) {
        if (modules[index].end < modules[index].start) return 0;
        reserve_range(modules[index].start,
                      modules[index].end - modules[index].start);
        if (modules[index].string != 0u)
            reserve_range(modules[index].string, 256u);
    }
    return 1;
}

static int allocator_self_check(void) {
    uint32_t free_before = free_pages;
    uint32_t first = memory_alloc_page();
    uint32_t second = memory_alloc_page();
    uint32_t word;
    volatile const uint32_t *first_contents;
    volatile const uint32_t *second_contents;

    if (first == 0u || second == 0u || first == second ||
        (first & (PAGE_SIZE - 1u)) != 0u ||
        (second & (PAGE_SIZE - 1u)) != 0u) {
        if (first != 0u) memory_free_page(first);
        if (second != 0u && second != first) memory_free_page(second);
        return 0;
    }
    first_contents = (volatile const uint32_t *)(uintptr_t)first;
    second_contents = (volatile const uint32_t *)(uintptr_t)second;
    for (word = 0u; word < PAGE_SIZE / sizeof(uint32_t); ++word) {
        if (first_contents[word] != 0u || second_contents[word] != 0u) {
            memory_free_page(first);
            memory_free_page(second);
            return 0;
        }
    }
    if (!memory_free_page(first) || memory_free_page(first) ||
        !memory_free_page(second) || free_pages != free_before) return 0;
    return 1;
}

static uint32_t try_contiguous_range(uint32_t first, uint32_t last,
                                     uint32_t count) {
    uint32_t candidate = first;
    while (candidate <= last) {
        uint32_t offset;
        for (offset = 0u; offset < count; ++offset) {
            if (get_page_state(candidate + offset) != PAGE_FREE) break;
        }
        if (offset == count) {
            uint32_t page_offset;
            for (offset = 0u; offset < count; ++offset) {
                set_page_state(candidate + offset, PAGE_ALLOCATED);
            }
            free_pages -= count;
            allocation_hint = candidate + count;
            if (allocation_hint >= MAX_PHYSICAL_PAGES) allocation_hint = 1u;
            for (page_offset = 0u; page_offset < count; ++page_offset) {
                uint32_t word;
                volatile uint32_t *contents = (volatile uint32_t *)(uintptr_t)
                    ((candidate + page_offset) << PAGE_SHIFT);
                for (word = 0u; word < PAGE_SIZE / sizeof(uint32_t); ++word) {
                    contents[word] = 0u;
                }
            }
            return candidate << PAGE_SHIFT;
        }
        candidate += offset + 1u;
    }
    return 0u;
}

uint32_t memory_alloc_contiguous_pages(uint32_t count) {
    uint32_t last_start;
    uint32_t address;

    if (ready == 0u || count == 0u || count >= MAX_PHYSICAL_PAGES ||
        count > free_pages) return 0u;
    /* Leave the final 4 KiB page unused so an end pointer cannot wrap to 0. */
    last_start = MAX_PHYSICAL_PAGES - count - 1u;
    if (allocation_hint <= last_start) {
        address = try_contiguous_range(allocation_hint, last_start, count);
        if (address != 0u) return address;
    }
    if (allocation_hint > 1u) {
        uint32_t last = allocation_hint - 1u;
        if (last > last_start) last = last_start;
        if (last >= 1u) return try_contiguous_range(1u, last, count);
    }
    return 0u;
}

int memory_free_contiguous_pages(uint32_t physical_address, uint32_t count) {
    uint32_t first_page;
    uint32_t offset;

    if (ready == 0u || count == 0u ||
        (physical_address & (PAGE_SIZE - 1u)) != 0u) return 0;
    first_page = physical_address >> PAGE_SHIFT;
    if (first_page == 0u || first_page >= MAX_PHYSICAL_PAGES ||
        count > MAX_PHYSICAL_PAGES - first_page) return 0;
    for (offset = 0u; offset < count; ++offset) {
        if (get_page_state(first_page + offset) != PAGE_ALLOCATED) return 0;
    }
    for (offset = 0u; offset < count; ++offset) {
        set_page_state(first_page + offset, PAGE_FREE);
    }
    free_pages += count;
    if (first_page < allocation_hint) allocation_hint = first_page;
    return 1;
}

int memory_init(const struct multiboot_info *info, uint32_t info_address,
                uint32_t kernel_start, uint32_t kernel_end) {
    uint64_t framebuffer_length;

    clear_page_states();
    if (info == NULL || kernel_end < kernel_start || !consume_memory_map(info)) {
        clear_page_states();
        return 0;
    }

    /* Keep firmware, boot data, and the complete loaded kernel out of the pool. */
    reserve_range(0u, 0x00100000u);
    reserve_range(kernel_start, kernel_end - kernel_start);
    reserve_range(info_address, sizeof(*info));
    reserve_range(info->mmap_addr, info->mmap_length);

    if (!reserve_multiboot_modules(info)) {
        clear_page_states();
        return 0;
    }

    if ((info->flags & MULTIBOOT_INFO_FRAMEBUFFER) != 0u) {
        framebuffer_length = (uint64_t)info->framebuffer_pitch * info->framebuffer_height;
        reserve_range(info->framebuffer_addr, framebuffer_length);
    }

    if (free_pages == 0u) {
        clear_page_states();
        return 0;
    }
    ready = 1u;
    if (!allocator_self_check()) {
        clear_page_states();
        kernel_log("MiniOS: physical page allocator self-check failed.\n");
        return 0;
    }
    kernel_log("MiniOS: physical page allocator ready; managed pages=");
    kernel_log_hex(managed_pages);
    kernel_log(" free pages=");
    kernel_log_hex(free_pages);
    kernel_log("\n");
    return 1;
}

uint32_t memory_alloc_page(void) {
    uint32_t page;
    uint32_t scanned;
    if (ready == 0u) return 0u;
    for (scanned = 0u; scanned < MAX_PHYSICAL_PAGES - 1u; ++scanned) {
        page = allocation_hint + scanned;
        if (page >= MAX_PHYSICAL_PAGES) page -= MAX_PHYSICAL_PAGES - 1u;
        if (get_page_state(page) == PAGE_FREE) {
            volatile uint32_t *contents = (volatile uint32_t *)(uintptr_t)(page << PAGE_SHIFT);
            uint32_t word;
            set_page_state(page, PAGE_ALLOCATED);
            --free_pages;
            allocation_hint = page + 1u;
            if (allocation_hint >= MAX_PHYSICAL_PAGES) allocation_hint = 1u;
            for (word = 0u; word < PAGE_SIZE / sizeof(uint32_t); ++word) {
                contents[word] = 0u;
            }
            return page << PAGE_SHIFT;
        }
    }
    return 0u;
}

int memory_free_page(uint32_t physical_address) {
    uint32_t page;
    if (ready == 0u || (physical_address & (PAGE_SIZE - 1u)) != 0u) return 0;
    page = physical_address >> PAGE_SHIFT;
    if (page == 0u || page >= MAX_PHYSICAL_PAGES ||
        get_page_state(page) != PAGE_ALLOCATED) return 0;
    set_page_state(page, PAGE_FREE);
    ++free_pages;
    if (page < allocation_hint) allocation_hint = page;
    return 1;
}

int memory_page_is_mapped(uint32_t physical_address) {
    uint32_t page;
    if ((physical_address & (PAGE_SIZE - 1u)) != 0u) return 0;
    page = physical_address >> PAGE_SHIFT;
    return page < MAX_PHYSICAL_PAGES && get_page_state(page) != PAGE_UNAVAILABLE;
}

int memory_next_mapped_page(uint32_t first_page, uint32_t *mapped_page) {
    uint32_t page = first_page;
    if (mapped_page == 0) return 0;
    while (page < MAX_PHYSICAL_PAGES) {
        if ((page & 15u) == 0u && MAX_PHYSICAL_PAGES - page >= 16u) {
            uint32_t byte = page >> 2;
            if ((page_states[byte] | page_states[byte + 1u] |
                 page_states[byte + 2u] | page_states[byte + 3u]) == 0u) {
                page += 16u;
                continue;
            }
        }
        uint32_t byte_index = page >> 2;
        uint32_t lane = page & 3u;
        uint8_t packed = page_states[byte_index];
        if (packed == 0u) {
            page = (byte_index + 1u) << 2;
            continue;
        }
        for (; lane < 4u; ++lane) {
            if (((packed >> (lane * 2u)) & 3u) != PAGE_UNAVAILABLE) {
                *mapped_page = (byte_index << 2) + lane;
                return 1;
            }
        }
        page = (byte_index + 1u) << 2;
    }
    return 0;
}

uint32_t memory_free_pages(void) { return ready != 0u ? free_pages : 0u; }
uint32_t memory_managed_pages(void) { return ready != 0u ? managed_pages : 0u; }
int memory_is_ready(void) { return ready != 0u; }
