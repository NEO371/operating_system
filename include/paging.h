#ifndef MINI_OS_PAGING_H
#define MINI_OS_PAGING_H

#include "kernel.h"

#define PAGING_USER_PROGRAM_PAGE_LIMIT 32u
#define PAGING_USER_PROGRAM_TABLE_LIMIT 32u

struct paging_address_space {
    uint32_t directory_physical;
    uint32_t data_table_physical;
    uint32_t stack_table_physical;
    uint32_t data_page_physical;
    uint32_t stack_page_physical;
    uint32_t program_table_physical[PAGING_USER_PROGRAM_TABLE_LIMIT];
    uint32_t program_table_directory_index[PAGING_USER_PROGRAM_TABLE_LIMIT];
    uint32_t program_page_physical[PAGING_USER_PROGRAM_PAGE_LIMIT];
    uint32_t program_page_virtual[PAGING_USER_PROGRAM_PAGE_LIMIT];
    uint32_t program_table_count;
    uint32_t program_page_count;
};

int paging_init(const struct multiboot_info *info);
int paging_is_ready(void);
int paging_address_space_create(struct paging_address_space *space,
                                uint32_t initial_data);
int paging_address_space_activate(const struct paging_address_space *space);
int paging_activate_kernel(void);
int paging_address_space_destroy(struct paging_address_space *space);
int paging_user_range_valid(const struct paging_address_space *space,
                            uint32_t address, uint32_t length,
                            int write_access);
int paging_address_space_map_user_page(struct paging_address_space *space,
                                       uint32_t virtual_address,
                                       int writable,
                                       uint32_t *physical_address);
int paging_address_space_unmap_user_page(struct paging_address_space *space,
                                         uint32_t virtual_address);

#endif
