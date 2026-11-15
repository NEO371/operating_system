#include "heap.h"

#include "interrupts.h"
#include "memory.h"
#include "resource_monitor.h"

#include <stdint.h>

#define PAGE_SIZE 4096u
#define HEAP_ALIGNMENT 8u
#define HEAP_MAX_ARENA_PAGES 256u
#define ARENA_MAGIC 0x48454150u
#define BLOCK_MAGIC 0x424C4F43u
#define FOOTER_MAGIC 0xF007E2u

struct heap_arena {
    uint32_t magic;
    uint32_t page_count;
    struct heap_arena *next;
    uint32_t reserved;
} __attribute__((aligned(HEAP_ALIGNMENT)));

struct heap_block {
    uint32_t size;
    uint32_t magic;
    uint32_t is_free;
    struct heap_arena *arena;
    struct heap_block *previous_free;
    struct heap_block *next_free;
};

struct heap_footer {
    uint32_t size;
    uint32_t magic;
};

_Static_assert(sizeof(struct heap_arena) % HEAP_ALIGNMENT == 0u,
               "heap arena header must be aligned");
_Static_assert(sizeof(struct heap_block) % HEAP_ALIGNMENT == 0u,
               "heap block header must be aligned");
_Static_assert(sizeof(struct heap_footer) % HEAP_ALIGNMENT == 0u,
               "heap footer must preserve block alignment");

static struct heap_arena *arenas;
static struct heap_block *free_blocks;
static uint8_t heap_ready;

static uint32_t align_size(uint32_t size) {
    return (size + (HEAP_ALIGNMENT - 1u)) & ~(HEAP_ALIGNMENT - 1u);
}

static uint8_t *arena_first_block(const struct heap_arena *arena) {
    return (uint8_t *)(uintptr_t)arena + align_size((uint32_t)sizeof(*arena));
}

static uint8_t *arena_end(const struct heap_arena *arena) {
    uint32_t base = (uint32_t)(uintptr_t)arena;
    return (uint8_t *)(uintptr_t)(base + arena->page_count * PAGE_SIZE);
}

static void write_footer(struct heap_block *block) {
    struct heap_footer *footer = (struct heap_footer *)
        ((uint8_t *)block + block->size - sizeof(*footer));
    footer->size = block->size;
    footer->magic = FOOTER_MAGIC ^ block->size;
}

static int block_valid(const struct heap_arena *arena,
                       const struct heap_block *block) {
    uintptr_t first = (uintptr_t)arena_first_block(arena);
    uintptr_t end = (uintptr_t)arena_end(arena);
    uintptr_t address = (uintptr_t)block;
    const struct heap_footer *footer;

    if (arena->magic != ARENA_MAGIC || address < first || address >= end ||
        (address & (HEAP_ALIGNMENT - 1u)) != 0u ||
        block->magic != BLOCK_MAGIC || block->arena != arena ||
        block->size < sizeof(*block) + sizeof(struct heap_footer) ||
        (block->size & (HEAP_ALIGNMENT - 1u)) != 0u ||
        block->size > end - address) return 0;
    footer = (const struct heap_footer *)
        (address + block->size - sizeof(*footer));
    return footer->size == block->size &&
           footer->magic == (FOOTER_MAGIC ^ block->size);
}

static void free_list_remove(struct heap_block *block) {
    if (block->previous_free != 0) {
        block->previous_free->next_free = block->next_free;
    } else {
        free_blocks = block->next_free;
    }
    if (block->next_free != 0) {
        block->next_free->previous_free = block->previous_free;
    }
    block->previous_free = 0;
    block->next_free = 0;
}

static void free_list_insert(struct heap_block *block) {
    block->is_free = 1u;
    block->previous_free = 0;
    block->next_free = free_blocks;
    if (free_blocks != 0) free_blocks->previous_free = block;
    free_blocks = block;
}

static struct heap_block *add_arena(uint32_t requested_block_size) {
    uint64_t total_needed = (uint64_t)requested_block_size + sizeof(struct heap_arena);
    uint32_t page_count = (uint32_t)((total_needed + PAGE_SIZE - 1u) / PAGE_SIZE);
    uint32_t physical;
    uint32_t arena_bytes;
    uint32_t first_offset = align_size((uint32_t)sizeof(struct heap_arena));
    struct heap_arena *arena;
    struct heap_block *block;

    if (page_count == 0u) page_count = 1u;
    if (page_count > HEAP_MAX_ARENA_PAGES) return 0;
    physical = memory_alloc_contiguous_pages(page_count);
    if (physical == 0u) return 0;
    arena_bytes = page_count * PAGE_SIZE;
    if (arena_bytes <= first_offset + sizeof(*block) + sizeof(struct heap_footer)) {
        memory_free_contiguous_pages(physical, page_count);
        return 0;
    }

    arena = (struct heap_arena *)(uintptr_t)physical;
    arena->magic = ARENA_MAGIC;
    arena->page_count = page_count;
    arena->next = arenas;
    arena->reserved = 0u;
    arenas = arena;

    block = (struct heap_block *)(void *)((uint8_t *)arena + first_offset);
    block->size = arena_bytes - first_offset;
    block->magic = BLOCK_MAGIC;
    block->arena = arena;
    block->is_free = 1u;
    block->previous_free = 0;
    block->next_free = 0;
    write_footer(block);
    free_list_insert(block);
    return block;
}

static struct heap_block *find_free_block(uint32_t requested_size) {
    struct heap_block *block;
    for (block = free_blocks; block != 0; block = block->next_free) {
        if (block->size >= requested_size) return block;
    }
    return 0;
}

static void *kmalloc_internal(size_t size) {
    uint64_t required64;
    uint32_t required;
    struct heap_block *block;

    if (heap_ready == 0u || size == 0u ||
        size > UINT32_MAX - sizeof(struct heap_block) - sizeof(struct heap_footer) -
               (HEAP_ALIGNMENT - 1u)) return 0;
    required64 = (uint64_t)size + sizeof(struct heap_block) + sizeof(struct heap_footer);
    required64 = (required64 + HEAP_ALIGNMENT - 1u) & ~(uint64_t)(HEAP_ALIGNMENT - 1u);
    if (required64 > UINT32_MAX) return 0;
    required = (uint32_t)required64;

    block = find_free_block(required);
    if (block == 0) {
        if (add_arena(required) == 0) return 0;
        block = find_free_block(required);
        if (block == 0) return 0;
    }

    free_list_remove(block);
    if (block->size - required >= sizeof(struct heap_block) + sizeof(struct heap_footer) +
                                  HEAP_ALIGNMENT) {
        uint32_t original_size = block->size;
        struct heap_block *remainder = (struct heap_block *)
            ((uint8_t *)block + required);
        block->size = required;
        remainder->size = original_size - required;
        remainder->magic = BLOCK_MAGIC;
        remainder->is_free = 1u;
        remainder->arena = block->arena;
        remainder->previous_free = 0;
        remainder->next_free = 0;
        write_footer(block);
        write_footer(remainder);
        free_list_insert(remainder);
    }
    block->magic = BLOCK_MAGIC;
    block->is_free = 0u;
    block->previous_free = 0;
    block->next_free = 0;
    write_footer(block);
    return (uint8_t *)block + sizeof(*block);
}

static uint32_t allocation_payload_size(void *pointer);

void *kmalloc(size_t size) {
    void *allocation = kmalloc_internal(size);
    uint32_t requested = size > UINT32_MAX ? 0u : (uint32_t)size;
    uint32_t actual = allocation != 0 ? allocation_payload_size(allocation) : 0u;
    resource_monitor_heap_allocate(allocation, requested, actual, allocation != 0);
    return allocation;
}

static struct heap_arena *find_arena(uintptr_t pointer) {
    struct heap_arena *arena;
    for (arena = arenas; arena != 0; arena = arena->next) {
        uintptr_t first = (uintptr_t)arena_first_block(arena) + sizeof(struct heap_block);
        uintptr_t end = (uintptr_t)arena_end(arena);
        if (pointer >= first && pointer < end) return arena;
    }
    return 0;
}

static uint32_t allocation_payload_size(void *pointer) {
    uintptr_t user_address = (uintptr_t)pointer;
    struct heap_arena *arena;
    struct heap_block *block;
    if (pointer == 0 || user_address < sizeof(struct heap_block)) return 0u;
    arena = find_arena(user_address);
    if (arena == 0) return 0u;
    block = (struct heap_block *)(user_address - sizeof(*block));
    if (!block_valid(arena, block) || block->is_free != 0u ||
        block->size < sizeof(*block) + sizeof(struct heap_footer)) return 0u;
    return block->size - (uint32_t)sizeof(*block) -
           (uint32_t)sizeof(struct heap_footer);
}

static int kfree_internal(void *pointer) {
    uintptr_t user_address = (uintptr_t)pointer;
    struct heap_arena *arena;
    struct heap_block *block;
    uintptr_t first;
    uintptr_t end;
    uintptr_t next_address;

    if (heap_ready == 0u || pointer == 0 ||
        (user_address & (HEAP_ALIGNMENT - 1u)) != 0u ||
        user_address < sizeof(struct heap_block)) return 0;
    arena = find_arena(user_address);
    if (arena == 0) return 0;
    block = (struct heap_block *)(user_address - sizeof(*block));
    if (!block_valid(arena, block) || block->is_free != 0u) return 0;
    first = (uintptr_t)arena_first_block(arena);
    end = (uintptr_t)arena_end(arena);
    block->is_free = 1u;

    if ((uintptr_t)block > first) {
        struct heap_footer *previous_footer = (struct heap_footer *)
            ((uint8_t *)block - sizeof(*previous_footer));
        if (previous_footer->magic == (FOOTER_MAGIC ^ previous_footer->size) &&
            previous_footer->size <= (uintptr_t)block - first &&
            previous_footer->size >= sizeof(struct heap_block) + sizeof(*previous_footer)) {
            struct heap_block *previous = (struct heap_block *)
                ((uint8_t *)block - previous_footer->size);
            if (block_valid(arena, previous) && previous->is_free != 0u) {
                free_list_remove(previous);
                previous->size += block->size;
                block = previous;
                write_footer(block);
            }
        }
    }

    next_address = (uintptr_t)block + block->size;
    if (next_address < end && end - next_address >= sizeof(struct heap_block) +
                                                    sizeof(struct heap_footer)) {
        struct heap_block *next = (struct heap_block *)next_address;
        if (block_valid(arena, next) && next->is_free != 0u) {
            free_list_remove(next);
            block->size += next->size;
            write_footer(block);
        }
    }
    free_list_insert(block);

    if ((uintptr_t)block == first && block->size == end - first) {
        uint32_t physical = (uint32_t)(uintptr_t)arena;
        uint32_t page_count = arena->page_count;
        struct heap_arena *next_arena = arena->next;
        struct heap_arena **arena_link = &arenas;
        while (*arena_link != 0 && *arena_link != arena) {
            arena_link = &(*arena_link)->next;
        }
        free_list_remove(block);
        if (*arena_link == arena) *arena_link = next_arena;
        if (!memory_free_contiguous_pages(physical, page_count)) {
            arena->next = next_arena;
            *arena_link = arena;
            free_list_insert(block);
            return 0;
        }
    }
    return 1;
}

int kfree(void *pointer) {
    uint32_t amount = allocation_payload_size(pointer);
    int success = kfree_internal(pointer);
    resource_monitor_heap_free(pointer, amount, success);
    return success;
}

static int heap_self_check(void) {
    void *small;
    void *medium;
    void *large;
    int valid;

    if (kmalloc(0u) != 0 || kmalloc((size_t)UINT32_MAX) != 0) return 0;
    small = kmalloc(13u);
    medium = kmalloc(200u);
    large = kmalloc(5000u);
    valid = small != 0 && medium != 0 && large != 0 &&
            (((uintptr_t)small & (HEAP_ALIGNMENT - 1u)) == 0u) &&
            (((uintptr_t)medium & (HEAP_ALIGNMENT - 1u)) == 0u) &&
            (((uintptr_t)large & (HEAP_ALIGNMENT - 1u)) == 0u);
    if (valid) {
        *(volatile uint32_t *)small = 0x13579BDFu;
        *(volatile uint32_t *)medium = 0x2468ACE0u;
        *(volatile uint32_t *)large = 0xA5A55A5Au;
        valid = *(volatile uint32_t *)small == 0x13579BDFu &&
                *(volatile uint32_t *)medium == 0x2468ACE0u &&
                *(volatile uint32_t *)large == 0xA5A55A5Au;
    }
    if (small != 0) {
        if (!kfree(small)) valid = 0;
        if (kfree(small)) valid = 0;
    }
    if (medium != 0 && !kfree(medium)) valid = 0;
    if (large != 0 && !kfree(large)) valid = 0;
    if (kfree((void *)(uintptr_t)0x1000u)) valid = 0;
    return valid;
}

int heap_init(void) {
    if (heap_ready != 0u) return 1;
    arenas = 0;
    free_blocks = 0;
    heap_ready = 1u;
    if (!heap_self_check()) {
        heap_ready = 0u;
        kernel_log("MiniOS: kernel heap self-check failed.\n");
        return 0;
    }
    kernel_log("MiniOS: bounded kernel heap ready; boot-time allocation checks passed.\n");
    return 1;
}

int heap_is_ready(void) { return heap_ready != 0u; }
