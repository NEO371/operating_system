#include "resource_monitor.h"

#include "heap.h"
#include "interrupts.h"
#include "syscall.h"

#include <stddef.h>
#include <stdint.h>

#define RESOURCE_TRACKED_ALLOCATION_LIMIT 32u
#define RESOURCE_KERNEL_ALLOCATION_LIMIT 64u

struct tracked_allocation {
    void *pointer;
    uint32_t process_id;
    uint32_t request_id;
    uint32_t bytes;
    uint8_t active;
};

struct kernel_allocation {
    void *pointer;
    uint32_t bytes;
    uint8_t active;
};

static struct resource_event events[RESOURCE_EVENT_CAPACITY];
static struct tracked_allocation allocations[RESOURCE_TRACKED_ALLOCATION_LIMIT];
static struct kernel_allocation kernel_allocations[RESOURCE_KERNEL_ALLOCATION_LIMIT];
static struct resource_statistics statistics;
static uint32_t next_event_id;
static uint32_t next_request_id;
static uint32_t event_count;
static uint32_t event_write_index;
static uint32_t heap_context_process_id;
static uint32_t heap_context_request_id;
static char heap_context_application[16];
static uint8_t heap_context_active;
static uint8_t monitor_ready;

static uint32_t saturating_add(uint32_t left, uint32_t right) {
    return UINT32_MAX - left < right ? UINT32_MAX : left + right;
}

static void copy_text(char *destination, uint32_t capacity, const char *source) {
    uint32_t index = 0u;
    if (capacity == 0u) return;
    if (source != 0) {
        while (index + 1u < capacity && source[index] != '\0') {
            destination[index] = source[index];
            ++index;
        }
    }
    destination[index] = '\0';
}

static int text_equals(const char *left, const char *right) {
    uint32_t i = 0u;
    if (left == 0 || right == 0) return 0;
    while (left[i] != '\0' && right[i] != '\0') {
        if (left[i] != right[i]) return 0;
        ++i;
    }
    return left[i] == right[i];
}

static void record_event(uint32_t request_id, uint32_t process_id,
                         uint8_t perspective, const char *application,
                         const char *resource, uint32_t requested_bytes,
                         uint32_t allocated_bytes, const char *operation,
                         const char *status, const char *reason) {
    struct resource_event *event = &events[event_write_index];
    event->event_id = ++next_event_id;
    event->timestamp_ticks = timer_ticks();
    event->request_id = request_id;
    event->process_id = process_id;
    event->requested_bytes = requested_bytes;
    event->allocated_bytes = allocated_bytes;
    event->released_bytes = (text_equals(operation, "RELEASE") ||
                             text_equals(operation, "FREE")) &&
                            (text_equals(status, "RELEASED") ||
                             text_equals(status, "SUCCESS")) ? allocated_bytes : 0u;
    event->argument0 = 0u;
    event->argument1 = 0u;
    event->argument2 = 0u;
    event->result = 0u;
    event->perspective = perspective;
    copy_text(event->application, sizeof(event->application), application);
    copy_text(event->resource, sizeof(event->resource), resource);
    copy_text(event->operation, sizeof(event->operation), operation);
    copy_text(event->status, sizeof(event->status), status);
    copy_text(event->reason, sizeof(event->reason), reason);
    event_write_index = (event_write_index + 1u) % RESOURCE_EVENT_CAPACITY;
    if (event_count < RESOURCE_EVENT_CAPACITY) ++event_count;
}

void resource_monitor_init(void) {
    uint32_t i;
    for (i = 0u; i < RESOURCE_EVENT_CAPACITY; ++i) {
        events[i].event_id = 0u;
    }
    for (i = 0u; i < RESOURCE_TRACKED_ALLOCATION_LIMIT; ++i) {
        allocations[i].active = 0u;
    }
    for (i = 0u; i < RESOURCE_KERNEL_ALLOCATION_LIMIT; ++i)
        kernel_allocations[i].active = 0u;
    statistics.current_bytes = 0u;
    statistics.kernel_current_bytes = 0u;
    statistics.peak_bytes = 0u;
    statistics.kernel_peak_bytes = 0u;
    statistics.successful_requests = 0u;
    statistics.failed_requests = 0u;
    statistics.release_operations = 0u;
    statistics.outstanding_allocations = 0u;
    statistics.cumulative_allocated_bytes = 0u;
    statistics.cumulative_released_bytes = 0u;
    statistics.kernel_cumulative_allocated_bytes = 0u;
    statistics.kernel_cumulative_released_bytes = 0u;
    statistics.kernel_successful_allocations = 0u;
    statistics.kernel_failed_allocations = 0u;
    statistics.user_syscalls = 0u;
    next_event_id = 0u;
    next_request_id = 0u;
    event_count = 0u;
    event_write_index = 0u;
    heap_context_active = 0u;
    heap_context_application[0] = '\0';
    monitor_ready = 1u;
}

void resource_monitor_heap_context_begin(uint32_t process_id,
                                         uint32_t request_id,
                                         const char *application) {
    heap_context_process_id = process_id;
    heap_context_request_id = request_id;
    copy_text(heap_context_application, sizeof(heap_context_application), application);
    heap_context_active = 1u;
}

void resource_monitor_heap_context_end(void) {
    heap_context_active = 0u;
    heap_context_process_id = 0u;
    heap_context_request_id = 0u;
    heap_context_application[0] = '\0';
}

static struct kernel_allocation *kernel_tracking_slot(void) {
    uint32_t i;
    for (i = 0u; i < RESOURCE_KERNEL_ALLOCATION_LIMIT; ++i)
        if (kernel_allocations[i].active == 0u) return &kernel_allocations[i];
    return 0;
}

void resource_monitor_heap_allocate(void *allocation, uint32_t requested_amount,
                                    uint32_t actual_amount, int success) {
    uint32_t request_id = heap_context_active ? heap_context_request_id : 0u;
    uint32_t process_id = heap_context_active ? heap_context_process_id : 0u;
    const char *source = heap_context_active ? heap_context_application : "Kernel";
    if (monitor_ready == 0u) return;
    if (success) {
        statistics.kernel_successful_allocations = saturating_add(
            statistics.kernel_successful_allocations, 1u);
        statistics.kernel_cumulative_allocated_bytes = saturating_add(
            statistics.kernel_cumulative_allocated_bytes, actual_amount);
        if (!heap_context_active) {
            struct kernel_allocation *slot = kernel_tracking_slot();
            if (slot != 0 && allocation != 0) {
                slot->pointer = allocation;
                slot->bytes = actual_amount;
                slot->active = 1u;
                statistics.kernel_current_bytes = saturating_add(
                    statistics.kernel_current_bytes, actual_amount);
                if (statistics.kernel_current_bytes > statistics.kernel_peak_bytes)
                    statistics.kernel_peak_bytes = statistics.kernel_current_bytes;
            }
        }
        record_event(request_id, process_id, RESOURCE_KERNEL, source, "MEMORY",
                     requested_amount, actual_amount, "ALLOCATE", "SUCCESS", "NONE");
    } else {
        statistics.kernel_failed_allocations = saturating_add(
            statistics.kernel_failed_allocations, 1u);
        record_event(request_id, process_id, RESOURCE_KERNEL, source, "MEMORY",
                     requested_amount, 0u, "ALLOCATE", "FAILED", "HEAP_ALLOCATION_FAILED");
    }
}

void resource_monitor_heap_free(void *allocation, uint32_t amount, int success) {
    uint32_t request_id = heap_context_active ? heap_context_request_id : 0u;
    uint32_t process_id = heap_context_active ? heap_context_process_id : 0u;
    const char *source = heap_context_active ? heap_context_application : "Kernel";
    uint32_t i;
    if (monitor_ready == 0u) return;
    if (success) {
        if (!heap_context_active) {
            for (i = 0u; i < RESOURCE_KERNEL_ALLOCATION_LIMIT; ++i) {
                struct kernel_allocation *slot = &kernel_allocations[i];
                if (slot->active != 0u && slot->pointer == allocation) {
                    statistics.kernel_current_bytes =
                        statistics.kernel_current_bytes >= slot->bytes ?
                        statistics.kernel_current_bytes - slot->bytes : 0u;
                    statistics.kernel_cumulative_released_bytes = saturating_add(
                        statistics.kernel_cumulative_released_bytes, slot->bytes);
                    slot->active = 0u;
                    break;
                }
            }
        } else {
            statistics.kernel_cumulative_released_bytes = saturating_add(
                statistics.kernel_cumulative_released_bytes, amount);
        }
        record_event(request_id, process_id, RESOURCE_KERNEL, source, "MEMORY",
                     amount, amount, "FREE", "SUCCESS", "NONE");
    } else {
        record_event(request_id, process_id, RESOURCE_KERNEL, source, "MEMORY",
                     amount, 0u, "FREE", "FAILED", "HEAP_FREE_REJECTED");
    }
}

void resource_monitor_lifecycle_event(uint32_t process_id,
                                      const char *application,
                                      const char *operation,
                                      const char *status) {
    record_event(0u, process_id, RESOURCE_SYSTEM, application, "NONE", 0u,
                 0u, operation, status, "NONE");
}

void resource_monitor_user_syscall(uint32_t process_id, uint32_t syscall_number,
                                   uint32_t argument0, uint32_t argument1,
                                   uint32_t argument2, uint32_t result) {
    const char *operation;
    switch (syscall_number) {
        case MINIOS_SYS_GETPID: operation = "GETPID"; break;
        case MINIOS_SYS_EXIT: operation = "EXIT"; break;
        case MINIOS_SYS_WRITE: operation = "WRITE"; break;
        case MINIOS_SYS_SPAWN: operation = "SPAWN"; break;
        case MINIOS_SYS_WAIT: operation = "WAIT"; break;
        case MINIOS_SYS_BRK: operation = "BRK"; break;
        case MINIOS_SYS_SPAWN_NAMED: operation = "SPAWN_NAMED"; break;
        default: operation = "UNKNOWN"; break;
    }
    if (next_request_id == UINT32_MAX) return;
    statistics.user_syscalls = saturating_add(statistics.user_syscalls, 1u);
    record_event(++next_request_id, process_id, RESOURCE_APPLICATION, "User",
                 "SYSCALL", syscall_number == MINIOS_SYS_WRITE ? argument2 : 0u,
                 0u, operation,
                 (result & 0x80000000u) != 0u ? "FAILED" : "SUCCESS", "NONE");
    events[(event_write_index + RESOURCE_EVENT_CAPACITY - 1u) %
           RESOURCE_EVENT_CAPACITY].argument0 = argument0;
    events[(event_write_index + RESOURCE_EVENT_CAPACITY - 1u) %
           RESOURCE_EVENT_CAPACITY].argument1 = argument1;
    events[(event_write_index + RESOURCE_EVENT_CAPACITY - 1u) %
           RESOURCE_EVENT_CAPACITY].argument2 = argument2;
    events[(event_write_index + RESOURCE_EVENT_CAPACITY - 1u) %
           RESOURCE_EVENT_CAPACITY].result = result;
}

static struct tracked_allocation *find_free_tracking_slot(void) {
    uint32_t i;
    for (i = 0u; i < RESOURCE_TRACKED_ALLOCATION_LIMIT; ++i)
        if (allocations[i].active == 0u) return &allocations[i];
    return 0;
}

uint32_t resource_allocate(uint32_t process_id, const char *application,
                           const char *resource, uint32_t amount,
                           void **allocation_out) {
    struct tracked_allocation *slot;
    void *allocation = 0;
    uint32_t request_id;

    if (allocation_out != 0) *allocation_out = 0;
    if (next_request_id == UINT32_MAX) return 0u;
    request_id = ++next_request_id;
    record_event(request_id, process_id, RESOURCE_APPLICATION, application,
                 resource, amount, 0u, "REQUEST", "PENDING", "NONE");
    slot = find_free_tracking_slot();
    if (amount == 0u || allocation_out == 0 || !text_equals(resource, "MEMORY")) {
        const char *reason = !text_equals(resource, "MEMORY") ?
            "UNSUPPORTED_RESOURCE" : "INVALID_REQUEST";
        statistics.failed_requests = saturating_add(statistics.failed_requests, 1u);
        record_event(request_id, process_id, RESOURCE_KERNEL, application,
                     resource, amount, 0u, "VALIDATE", "FAILED", reason);
        record_event(request_id, process_id, RESOURCE_APPLICATION, application,
                     resource, amount, 0u, "RESULT", "FAILED", reason);
        return request_id;
    }

    record_event(request_id, process_id, RESOURCE_KERNEL, application,
                 resource, amount, 0u, "VALIDATE", "VALIDATED", "NONE");

    if (slot == 0 || amount > UINT32_MAX - statistics.current_bytes) {
        statistics.failed_requests = saturating_add(statistics.failed_requests, 1u);
        record_event(request_id, process_id, RESOURCE_KERNEL, application,
                     resource, amount, 0u, "ALLOCATE", "FAILED",
                     "RESOURCE_LIMIT_EXCEEDED");
        record_event(request_id, process_id, RESOURCE_APPLICATION, application,
                     resource, amount, 0u, "RESULT", "FAILED",
                     "RESOURCE_LIMIT_EXCEEDED");
        return request_id;
    }

    resource_monitor_heap_context_begin(process_id, request_id, application);
    if (heap_is_ready()) allocation = kmalloc((size_t)amount);
    resource_monitor_heap_context_end();
    if (allocation == 0) {
        const char *reason = heap_is_ready() ? "INSUFFICIENT_MEMORY" :
                                               "RESOURCE_MANAGER_UNAVAILABLE";
        statistics.failed_requests = saturating_add(statistics.failed_requests, 1u);
        record_event(request_id, process_id, RESOURCE_APPLICATION, application,
                     resource, amount, 0u, "RESULT", "FAILED", reason);
        return request_id;
    }

    slot->pointer = allocation;
    slot->process_id = process_id;
    slot->request_id = request_id;
    slot->bytes = amount;
    slot->active = 1u;
    *allocation_out = allocation;
    statistics.current_bytes = saturating_add(statistics.current_bytes, amount);
    if (statistics.current_bytes > statistics.peak_bytes)
        statistics.peak_bytes = statistics.current_bytes;
    statistics.successful_requests = saturating_add(statistics.successful_requests, 1u);
    statistics.outstanding_allocations = saturating_add(
        statistics.outstanding_allocations, 1u);
    statistics.cumulative_allocated_bytes = saturating_add(
        statistics.cumulative_allocated_bytes, amount);
    record_event(request_id, process_id, RESOURCE_APPLICATION, application,
                 resource, amount, amount, "RESULT", "SUCCESS", "NONE");
    return request_id;
}

int resource_release(uint32_t process_id, const char *application,
                     uint32_t request_id, const char *resource,
                     void *allocation, uint32_t amount) {
    uint32_t i;
    for (i = 0u; i < RESOURCE_TRACKED_ALLOCATION_LIMIT; ++i) {
        struct tracked_allocation *slot = &allocations[i];
        if (slot->active == 0u || slot->pointer != allocation ||
            slot->process_id != process_id || slot->request_id != request_id ||
            slot->bytes != amount) continue;
        resource_monitor_heap_context_begin(process_id, request_id, application);
        if (!kfree(allocation)) {
            resource_monitor_heap_context_end();
            record_event(request_id, process_id, RESOURCE_APPLICATION, application,
                         resource, amount, 0u, "RELEASE", "RELEASE_FAILED",
                         "RESOURCE_MANAGER_ERROR");
            return 0;
        }
        resource_monitor_heap_context_end();
        slot->active = 0u;
        statistics.current_bytes = statistics.current_bytes >= amount ?
            statistics.current_bytes - amount : 0u;
        statistics.outstanding_allocations =
            statistics.outstanding_allocations == 0u ? 0u :
            statistics.outstanding_allocations - 1u;
        statistics.release_operations = saturating_add(
            statistics.release_operations, 1u);
        statistics.cumulative_released_bytes = saturating_add(
            statistics.cumulative_released_bytes, amount);
        record_event(request_id, process_id, RESOURCE_APPLICATION, application,
                     resource, amount, amount, "RELEASE", "RELEASED", "NONE");
        return 1;
    }
    record_event(request_id, process_id, RESOURCE_KERNEL, application,
                 resource, amount, 0u, "RELEASE", "RELEASE_FAILED",
                 "INVALID_REQUEST");
    record_event(request_id, process_id, RESOURCE_APPLICATION, application,
                 resource, amount, 0u, "RELEASE", "RELEASE_FAILED",
                 "INVALID_REQUEST");
    return 0;
}

const struct resource_statistics *resource_monitor_statistics(void) {
    return &statistics;
}

uint32_t resource_monitor_event_count(uint8_t perspective) {
    uint32_t count = 0u;
    uint32_t i;
    for (i = 0u; i < event_count; ++i) {
        uint32_t index = (event_write_index + RESOURCE_EVENT_CAPACITY -
                          event_count + i) % RESOURCE_EVENT_CAPACITY;
        if (perspective == 0u || events[index].perspective == perspective) ++count;
    }
    return count;
}

uint32_t resource_monitor_event_count_after(uint8_t perspective,
                                           uint32_t event_id) {
    uint32_t count = 0u;
    uint32_t i;
    for (i = 0u; i < event_count; ++i) {
        uint32_t index = (event_write_index + RESOURCE_EVENT_CAPACITY -
                          event_count + i) % RESOURCE_EVENT_CAPACITY;
        if (events[index].event_id > event_id &&
            (perspective == 0u || events[index].perspective == perspective))
            ++count;
    }
    return count;
}

uint32_t resource_monitor_last_event_id(void) {
    return next_event_id;
}

const struct resource_event *resource_monitor_event_from_newest(
    uint8_t perspective, uint32_t offset) {
    uint32_t seen = 0u;
    uint32_t i = event_count;
    while (i != 0u) {
        uint32_t index;
        --i;
        index = (event_write_index + RESOURCE_EVENT_CAPACITY -
                 event_count + i) % RESOURCE_EVENT_CAPACITY;
        if (perspective != 0u && events[index].perspective != perspective) continue;
        if (seen++ == offset) return &events[index];
    }
    return 0;
}

const struct resource_event *resource_monitor_event_from_newest_after(
    uint8_t perspective, uint32_t event_id, uint32_t offset) {
    uint32_t seen = 0u;
    uint32_t i = event_count;
    while (i != 0u) {
        uint32_t index;
        --i;
        index = (event_write_index + RESOURCE_EVENT_CAPACITY -
                 event_count + i) % RESOURCE_EVENT_CAPACITY;
        if (events[index].event_id <= event_id ||
            (perspective != 0u && events[index].perspective != perspective)) continue;
        if (seen++ == offset) return &events[index];
    }
    return 0;
}

uint32_t resource_monitor_events_after(uint32_t event_id, uint8_t perspective) {
    return resource_monitor_event_count_after(perspective, event_id);
}

uint32_t resource_monitor_total_requests(void) {
    return saturating_add(statistics.successful_requests, statistics.failed_requests);
}

uint32_t resource_monitor_successful_requests(void) {
    return statistics.successful_requests;
}

uint32_t resource_monitor_failed_requests(void) {
    return statistics.failed_requests;
}

uint32_t resource_monitor_release_operations(void) {
    return statistics.release_operations;
}
