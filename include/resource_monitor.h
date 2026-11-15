#ifndef MINI_OS_RESOURCE_MONITOR_H
#define MINI_OS_RESOURCE_MONITOR_H

#include <stdint.h>

#define RESOURCE_EVENT_CAPACITY 512u

enum resource_perspective {
    RESOURCE_APPLICATION = 1u,
    RESOURCE_KERNEL = 2u,
    RESOURCE_SYSTEM = 3u
};

struct resource_event {
    uint32_t event_id;
    uint32_t timestamp_ticks;
    uint32_t request_id;
    uint32_t process_id;
    uint32_t requested_bytes;
    uint32_t allocated_bytes;
    uint32_t released_bytes;
    uint32_t argument0;
    uint32_t argument1;
    uint32_t argument2;
    uint32_t result;
    uint8_t perspective;
    char application[16];
    char resource[12];
    char operation[16];
    char status[16];
    char reason[32];
};

struct resource_statistics {
    uint32_t current_bytes;
    uint32_t kernel_current_bytes;
    uint32_t peak_bytes;
    uint32_t kernel_peak_bytes;
    uint32_t successful_requests;
    uint32_t failed_requests;
    uint32_t release_operations;
    uint32_t outstanding_allocations;
    uint32_t cumulative_allocated_bytes;
    uint32_t cumulative_released_bytes;
    uint32_t kernel_cumulative_allocated_bytes;
    uint32_t kernel_cumulative_released_bytes;
    uint32_t kernel_successful_allocations;
    uint32_t kernel_failed_allocations;
    uint32_t user_syscalls;
};

void resource_monitor_init(void);
void resource_monitor_lifecycle_event(uint32_t process_id,
                                      const char *application,
                                      const char *operation,
                                      const char *status);
void resource_monitor_user_syscall(uint32_t process_id, uint32_t syscall_number,
                                   uint32_t argument0, uint32_t argument1,
                                   uint32_t argument2, uint32_t result);
uint32_t resource_allocate(uint32_t process_id, const char *application,
                           const char *resource, uint32_t amount,
                           void **allocation_out);
int resource_release(uint32_t process_id, const char *application,
                     uint32_t request_id, const char *resource,
                     void *allocation, uint32_t amount);
void resource_monitor_heap_context_begin(uint32_t process_id,
                                         uint32_t request_id,
                                         const char *application);
void resource_monitor_heap_context_end(void);
void resource_monitor_heap_allocate(void *allocation, uint32_t requested_amount,
                                    uint32_t actual_amount, int success);
void resource_monitor_heap_free(void *allocation, uint32_t amount,
                                int success);
const struct resource_statistics *resource_monitor_statistics(void);
uint32_t resource_monitor_event_count(uint8_t perspective);
uint32_t resource_monitor_event_count_after(uint8_t perspective,
                                           uint32_t event_id);
uint32_t resource_monitor_last_event_id(void);
const struct resource_event *resource_monitor_event_from_newest(
    uint8_t perspective, uint32_t offset);
const struct resource_event *resource_monitor_event_from_newest_after(
    uint8_t perspective, uint32_t event_id, uint32_t offset);
uint32_t resource_monitor_events_after(uint32_t event_id, uint8_t perspective);
uint32_t resource_monitor_total_requests(void);
uint32_t resource_monitor_successful_requests(void);
uint32_t resource_monitor_failed_requests(void);
uint32_t resource_monitor_release_operations(void);

#endif
