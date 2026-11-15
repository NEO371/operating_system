#include "kernel.h"

#include "gui.h"
#include "input.h"
#include "interrupts.h"
#include "memory.h"
#include "paging.h"
#include "ring3.h"
#include "heap.h"
#include "fault_tests.h"
#include "fat16.h"
#include "pci.h"
#include "resource_monitor.h"
#include "mouse.h"
#include "dbms.h"

#include <stdint.h>
#include <stddef.h>

extern uint8_t _kernel_start[];
extern uint8_t _kernel_end[];

#define MULTIBOOT_INFO_MODULES (1u << 3)
#define USER_MODULE_MAX_SIZE (1024u * 1024u)
#define MOUSE_POINTER_SCALE 2

void uint32_to_hex(uint32_t value, char output[11]) {
    static const char digits[] = "0123456789ABCDEF";
    int32_t shift;

    output[0] = '0';
    output[1] = 'x';
    for (shift = 28; shift >= 0; shift -= 4) {
        output[2 + (28 - shift) / 4] = digits[(value >> (uint32_t)shift) & 0xFu];
    }
    output[10] = '\0';
}

void uint32_to_dec(uint32_t value, char output[11]) {
    char reverse[10];
    uint32_t count = 0u;
    uint32_t index;

    if (value == 0u) {
        output[0] = '0';
        output[1] = '\0';
        return;
    }
    while (value != 0u) {
        reverse[count++] = (char)('0' + value % 10u);
        value /= 10u;
    }
    for (index = 0u; index < count; ++index) {
        output[index] = reverse[count - index - 1u];
    }
    output[count] = '\0';
}

static uint32_t string_length(const char *string) {
    uint32_t length = 0u;
    while (string[length] != '\0') {
        ++length;
    }
    return length;
}

static void draw_dec_value(uint32_t x, uint32_t y, const char *label,
                           uint32_t value, uint32_t color) {
    char number[11];
    uint32_to_dec(value, number);
    gui_draw_string(x, y, label, GUI_MUTED);
    gui_draw_string(x + string_length(label) * 8u, y, number, color);
}

static void halt_forever(void) {
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

static int known_physical_range(uint32_t start, uint32_t length) {
    uint64_t end = (uint64_t)start + length;
    uint64_t page;
    if (length == 0u || end > 0x100000000ull) return 0;
    page = (uint64_t)start & ~(uint64_t)0xFFFu;
    for (; page < end; page += 0x1000u)
        if (!memory_page_is_mapped((uint32_t)page)) return 0;
    return 1;
}

static int module_command_name(uint32_t command_address,
                               char name[MINI_OS_MODULE_NAME_CAPACITY]) {
    uint32_t length;
    if (command_address == 0u) return 0;
    for (length = 0u; length < MINI_OS_MODULE_NAME_CAPACITY; ++length) {
        uint64_t address = (uint64_t)command_address + length;
        char character;
        if (address > 0xFFFFFFFFu ||
            !memory_page_is_mapped((uint32_t)address & ~0xFFFu)) return 0;
        character = *(const char *)(uintptr_t)(uint32_t)address;
        if (character == '\0' || character == ' ' || character == '\t') break;
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '-' ||
              character == '_' || character == '.')) return 0;
        name[length] = character;
    }
    if (length == 0u || length >= MINI_OS_MODULE_NAME_CAPACITY) return 0;
    name[length] = '\0';
    return 1;
}

static int boot_user_modules(const struct multiboot_info *info) {
    uint64_t descriptors_end;
    const struct multiboot_module *modules;
    uint32_t registered = 0u;
    ring3_reset_elf_modules();
    if (info == 0 ||
        (info->flags & MULTIBOOT_INFO_MODULES) == 0u ||
        info->mods_addr == 0u || info->mods_count == 0u ||
        info->mods_count > MINI_OS_MODULE_LIMIT) return 0;
    descriptors_end = (uint64_t)info->mods_addr +
        (uint64_t)info->mods_count * sizeof(struct multiboot_module);
    if (descriptors_end > 0x100000000ull ||
        !known_physical_range(info->mods_addr,
            info->mods_count * sizeof(struct multiboot_module))) return 0;
    modules = (const struct multiboot_module *)(uintptr_t)info->mods_addr;
    for (uint32_t index = 0u; index < info->mods_count; ++index) {
        uint32_t size;
        char name[MINI_OS_MODULE_NAME_CAPACITY];
        if (modules[index].end <= modules[index].start) continue;
        size = modules[index].end - modules[index].start;
        if (size > USER_MODULE_MAX_SIZE ||
            !known_physical_range(modules[index].start, size) ||
            !module_command_name(modules[index].string, name)) continue;
        if (!ring3_register_elf_module(name,
                (const uint8_t *)(uintptr_t)modules[index].start, size)) {
            kernel_log("MiniOS: rejected duplicate or invalid ELF module name.\n");
            ring3_reset_elf_modules();
            return 0;
        }
        ++registered;
    }
    if (registered == 0u) ring3_reset_elf_modules();
    return registered != 0u;
}

static int run_boot_user_elf(const struct multiboot_info *info) {
    if (!boot_user_modules(info)) {
        kernel_log("MiniOS: no valid named Multiboot user modules were provided.\n");
        return 0;
    }
    kernel_log("MiniOS: loading default named ELF module.\n");
    return ring3_run_default_elf_module();
}

static char dbms_terminal_input[64];
static char dbms_last_query[64];
static char dbms_terminal_output[512] = "Welcome. Type HELP to get started.";
static int32_t terminal_window_dx, terminal_window_dy;

static void draw_terminal_output_region(void) {
    uint32_t y = 206u;
    gui_draw_rect(48u, 198u, 700u, 210u, GUI_CLIENT);
    if (dbms_last_query[0] != '\0') {
        gui_draw_string(52u, y, ">", GUI_PURPLE);
        gui_draw_text_box(68u, y, 672u, 18u, 18u, dbms_last_query, GUI_TEXT);
        y += 20u;
    }
    gui_draw_text_box(52u, y, 692u, 408u - y, 18u,
                      dbms_terminal_output, GUI_TEXT);
}

static void draw_terminal_input_region(void) {
    gui_draw_rect(48u, 420u, 700u, 26u, GUI_CLIENT);
    gui_draw_string(52u, 430u, "sql>", GUI_GREEN);
    gui_draw_string(92u, 430u, dbms_terminal_input, GUI_TEXT);
    gui_draw_char(92u + string_length(dbms_terminal_input) * 8u,
                  430u, '_', GUI_PURPLE);
}

static void update_terminal_input_cell(char typed_character, uint32_t old_length) {
    uint32_t old_cursor_x = 92u + old_length * 8u;
    uint32_t new_length = string_length(dbms_terminal_input);
    gui_set_translation(terminal_window_dx, terminal_window_dy);
    gui_draw_rect(old_cursor_x, 430u, 8u, 8u, GUI_CLIENT);
    if (typed_character == '\b') {
        if (old_length != 0u)
            gui_draw_rect(92u + (old_length - 1u) * 8u, 430u, 8u, 8u, GUI_CLIENT);
    } else if (new_length > old_length) {
        gui_draw_char(old_cursor_x, 430u, typed_character, GUI_TEXT);
    }
    gui_draw_char(92u + new_length * 8u, 430u, '_', GUI_PURPLE);
    gui_set_translation(0, 0);
}

static void draw_terminal(void) {
    gui_draw_window(36u, 54u, 728u, 428u, "MiniOS DBMS Terminal");
    gui_draw_string(52u, 96u, "MiniOS SQL console  |  persistent FAT16 database", GUI_GREEN);
    gui_draw_string(52u, 116u, "CREATE / INSERT / SELECT / UPDATE / DELETE / DROP", GUI_TEXT);
    gui_draw_string(52u, 136u, "Type HELP for syntax; ESC returns to desktop.", GUI_MUTED);
    gui_draw_string(52u, 162u, dbms_storage_is_ready() ?
                    "[OK] persistent DBMS disk mounted" :
                    "[--] DBMS disk unavailable; writes disabled",
                    dbms_storage_is_ready() ? GUI_GREEN : GUI_RED);
    gui_draw_string(52u, 182u, memory_is_ready() ?
                    (paging_is_ready() ? "[OK] memory allocator + paging active" :
                                         "[--] memory ready; paging unavailable") :
                    "[--] memory allocator unavailable",
                    memory_is_ready() && paging_is_ready() ? GUI_GREEN : GUI_YELLOW);
    gui_draw_rect(48u, 412u, 700u, 1u, GUI_BORDER);
    draw_terminal_output_region();
    draw_terminal_input_region();
}

static int32_t network_window_dx, network_window_dy;
static int32_t devices_window_dx, devices_window_dy;

static void draw_terminal_screen(const struct multiboot_info *info) {
    (void)info;
    gui_draw_desktop_background();
    gui_set_translation(terminal_window_dx, terminal_window_dy);
    draw_terminal();
    gui_set_translation(0, 0);
}

static uint32_t task_scroll[4];
static uint8_t task_selected_panel;
static uint8_t task_auto_scroll[4] = { 1u, 1u, 1u, 1u };
static uint32_t task_process_refresh_tick;
static uint32_t task_view_clear_before_id;
static int32_t task_window_dx, task_window_dy;
static int32_t snake_window_dx, snake_window_dy;
static int snake_is_active(void);

static uint32_t append_number(char *buffer, uint32_t offset, uint32_t value) {
    char number[11];
    uint32_t i;
    uint32_to_dec(value, number);
    for (i = 0u; number[i] != '\0' && offset < 190u; ++i)
        buffer[offset++] = number[i];
    return offset;
}

static uint32_t append_text(char *buffer, uint32_t offset, const char *value) {
    uint32_t i;
    for (i = 0u; value[i] != '\0' && offset < 190u; ++i)
        buffer[offset++] = value[i];
    return offset;
}

static void draw_resource_event_line(uint32_t x, uint32_t y,
                                    const struct resource_event *event,
                                    uint8_t global_view) {
    char line[192];
    uint32_t length = 0u;
    if (event == 0) return;
    if (global_view != 0u) {
        line[length++] = 'T';
        length = append_number(line, length, event->timestamp_ticks);
        line[length++] = ' ';
        line[length++] = 'E';
        length = append_number(line, length, event->event_id);
        line[length++] = ' ';
        line[length++] = 'R';
        length = append_number(line, length, event->request_id);
        line[length++] = ' ';
        line[length++] = 'P';
        length = append_number(line, length, event->process_id);
        line[length++] = ' ';
        length = append_text(line, length, event->application);
        line[length++] = ' ';
        length = append_text(line, length, event->resource);
        line[length++] = ' ';
        length = append_text(line, length,
            event->perspective == RESOURCE_APPLICATION ? "APP " :
            (event->perspective == RESOURCE_KERNEL ? "KERNEL " : "SYSTEM "));
    } else {
        line[length++] = 'E';
        length = append_number(line, length, event->event_id);
        line[length++] = ' ';
        line[length++] = 'P';
        length = append_number(line, length, event->process_id);
        line[length++] = ' ';
        length = append_text(line, length, event->application);
        line[length++] = ' ';
    }
    length = append_text(line, length, event->operation);
    line[length++] = ' ';
    length = append_text(line, length, event->status);
    if (global_view != 0u && event->resource[0] == 'S' &&
        event->resource[1] == 'Y') {
        length = append_text(line, length, " A=");
        length = append_number(line, length, event->argument0);
        length = append_text(line, length, " B=");
        length = append_number(line, length, event->argument1);
        length = append_text(line, length, " C=");
        length = append_number(line, length, event->argument2);
        length = append_text(line, length, " -> ");
        length = append_number(line, length, event->result);
    } else if (event->released_bytes != 0u) {
        line[length++] = ' ';
        length = append_number(line, length, event->released_bytes);
        length = append_text(line, length, "B released");
    } else if (event->allocated_bytes != 0u) {
        line[length++] = ' ';
        length = append_number(line, length, event->allocated_bytes);
        length = append_text(line, length, "B actual");
    } else if (event->requested_bytes != 0u) {
        line[length++] = ' ';
        length = append_number(line, length, event->requested_bytes);
        length = append_text(line, length, "B requested");
    }
    if (event->reason[0] != '\0' && event->reason[0] != 'N') {
        line[length++] = ' ';
        length = append_text(line, length, event->reason);
    }
    line[length] = '\0';
    gui_draw_text_box(x, y, global_view != 0u ? 706u : 332u, 30u, 14u, line,
        (event->status[0] == 'S' ||
         (event->status[0] == 'R' && event->status[7] == 'D')) ? GUI_GREEN :
        ((event->status[0] == 'F' ||
          (event->status[0] == 'R' && event->status[7] == '_')) ? GUI_RED : GUI_TEXT));
}

static void draw_resource_panel(uint32_t x, uint32_t y, const char *title,
                                uint8_t perspective, uint32_t scroll,
                                uint8_t selected, uint8_t auto_scroll) {
    uint32_t count = resource_monitor_event_count_after(
        perspective, task_view_clear_before_id);
    uint32_t row;
    char label[40];
    uint32_t label_size = 0u;
    const char *suffix = auto_scroll != 0u && scroll == 0u ?
                         " LIVE AUTO:ON" : " PAUSED HISTORY";
    while (title[label_size] != '\0' && label_size < 28u) {
        label[label_size] = title[label_size];
        ++label_size;
    }
    while (*suffix != '\0' && label_size < sizeof(label) - 1u)
        label[label_size++] = *suffix++;
    label[label_size] = '\0';
    gui_draw_string(x, y, label, selected ? GUI_YELLOW : GUI_GREEN);
    for (row = 0u; row < 3u; ++row) {
        const struct resource_event *event =
            resource_monitor_event_from_newest_after(perspective,
                task_view_clear_before_id, scroll + row);
        if (event == 0) break;
        draw_resource_event_line(x, y + 20u + row * 32u, event, 0u);
    }
    if (count == 0u) gui_draw_string(x, y + 20u, "Waiting for resource activity", GUI_MUTED);
}

static void draw_task_manager(void) {
    const struct resource_statistics *stats = resource_monitor_statistics();
    uint32_t total = memory_managed_pages();
    uint32_t free = memory_free_pages();
    uint32_t rows;
    gui_draw_window(28u, 32u, 744u, 550u, "Task & Resource Manager");
    gui_draw_rect(730u, 38u, 14u, 14u, GUI_BLUE);
    gui_draw_rect(733u, 44u, 8u, 2u, GUI_WHITE);
    gui_draw_string(44u, 74u, "SYSTEM SUMMARY", GUI_PURPLE);
    gui_draw_string(44u, 94u, "KERNEL RUNNING", GUI_GREEN);
    gui_draw_string(188u, 94u, heap_is_ready() ? "HEAP READY" : "HEAP UNAVAILABLE",
                    heap_is_ready() ? GUI_GREEN : GUI_YELLOW);
    draw_dec_value(330u, 94u, "Managed pages: ", total, GUI_TEXT);
    draw_dec_value(532u, 94u, "Free pages: ", free, GUI_GREEN);
    gui_draw_string(44u, 114u, snake_is_active() ?
                    "ACTIVE: SNAKE PID 0 (kernel context)" : "ACTIVE: NONE",
                    snake_is_active() ? GUI_TEXT : GUI_MUTED);
    draw_dec_value(400u, 114u, "Allocated pages: ", total >= free ? total - free : 0u, GUI_TEXT);
    draw_dec_value(620u, 114u, "Calls: ", stats->user_syscalls, GUI_BLUE);
    draw_dec_value(44u, 134u, "Requests: ", resource_monitor_total_requests(), GUI_TEXT);
    draw_dec_value(202u, 134u, "Success: ", resource_monitor_successful_requests(), GUI_GREEN);
    draw_dec_value(352u, 134u, "Failed: ", resource_monitor_failed_requests(), GUI_RED);
    draw_dec_value(500u, 134u, "Releases: ", resource_monitor_release_operations(), GUI_TEXT);
    gui_draw_rect(606u, 130u, 142u, 20u,
                  task_selected_panel == 3u ? GUI_BLUE : GUI_TITLE);
    gui_draw_string(612u, 136u, "PROCESSES / GANTT", GUI_TEXT);

    if (task_selected_panel == 3u) {
        static const uint32_t chart_slots = 40u;
        uint32_t count = ring3_scheduler_history_count();
        uint32_t visible = count < chart_slots ? count : chart_slots;
        uint32_t row;
        uint32_t current_pid = ring3_scheduler_current_pid();
        static const char *const process_states[] = {
            "UNKNOWN", "RUNNING", "READY", "WAITING", "EXITED", "REAPED"
        };
        static const uint32_t process_state_colors[] = {
            GUI_MUTED, GUI_GREEN, GUI_YELLOW, GUI_BLUE, GUI_MUTED, GUI_MUTED
        };
        gui_draw_string(44u, 158u, "CURRENT PROCESSES  |  REAL TIMER SAMPLES (10 ms / tick)", GUI_PURPLE);
        gui_draw_rect(40u, 176u, 716u, 248u, GUI_CLIENT);
        gui_draw_string(52u, 186u, "PID 0   MiniOS kernel + desktop loop", GUI_TEXT);
        gui_draw_string(590u, 186u, "RUNNING", GUI_GREEN);
        gui_draw_string(52u, 216u, "PID 1   Ring-3 scheduler worker", GUI_TEXT);
        gui_draw_string(590u, 216u,
            process_states[ring3_scheduler_process_state(1u) < 6u ?
                ring3_scheduler_process_state(1u) : 0u],
            process_state_colors[ring3_scheduler_process_state(1u) < 6u ?
                ring3_scheduler_process_state(1u) : 0u]);
        gui_draw_string(52u, 246u, "PID 2   Ring-3 scheduler worker", GUI_TEXT);
        gui_draw_string(590u, 246u,
            process_states[ring3_scheduler_process_state(2u) < 6u ?
                ring3_scheduler_process_state(2u) : 0u],
            process_state_colors[ring3_scheduler_process_state(2u) < 6u ?
                ring3_scheduler_process_state(2u) : 0u]);
        gui_draw_string(52u, 276u, "PID 3   Spawned child (if scheduled)", GUI_TEXT);
        gui_draw_string(590u, 276u,
            process_states[ring3_scheduler_process_state(3u) < 6u ?
                ring3_scheduler_process_state(3u) : 0u],
            process_state_colors[ring3_scheduler_process_state(3u) < 6u ?
                ring3_scheduler_process_state(3u) : 0u]);
        gui_draw_string(52u, 312u, ring3_scheduler_is_active() ?
                        "Scheduler active; current PID:" :
                        "Last scheduler run (boot checks); current PID:", GUI_MUTED);
        draw_dec_value(420u, 312u, "", ring3_scheduler_is_active() ? current_pid : 0u,
                       ring3_scheduler_is_active() ? GUI_GREEN : GUI_TEXT);
        gui_draw_string(52u, 342u, "GANTT  |  oldest sample  ---------------------->  newest", GUI_BLUE);
        for (row = 0u; row < 4u; ++row) {
            uint32_t slot;
            uint32_t y = 364u + row * 12u;
            for (slot = 0u; slot < chart_slots; ++slot) {
                uint32_t pid = 0xFFFFFFFFu;
                uint32_t tick;
                uint32_t x = 54u + slot * 16u;
                uint32_t color = GUI_BORDER;
                if (slot < visible &&
                           ring3_scheduler_history_get(visible - 1u - slot,
                                                       &pid, &tick)) {
                    (void)tick;
                    if (pid == row) color = row == 0u ? GUI_BLUE :
                        (row == 1u ? GUI_GREEN :
                         (row == 2u ? GUI_YELLOW : GUI_RED));
                }
                gui_draw_rect(x, y, 12u, 7u, color);
            }
        }
        gui_draw_text_box(44u, 430u, 706u, 26u, 14u,
            "Live PID 0 is the kernel-hosted UI; ring-3 samples are retained from the latest scheduler run.",
            GUI_MUTED);
        gui_draw_string(44u, 460u, "LEFT/RIGHT: switch view  |  ESC: desktop", GUI_TEXT);
        gui_draw_string(44u, 488u, "Samples retained: ", GUI_MUTED);
        draw_dec_value(170u, 488u, "", count, GUI_TEXT);
        draw_dec_value(250u, 488u, "PIT ticks: ", timer_ticks(), GUI_TEXT);
        gui_draw_text_box(44u, 532u, 706u, 28u, 14u,
            "Processes are refreshed from scheduler and timer state; timeline shows real ring-3 time slices.",
            GUI_MUTED);
        gui_draw_string(44u, 574u, "LEFT/RIGHT select view | ESC desktop", GUI_TEXT);
        return;
    }

    gui_draw_rect(40u, 158u, 352u, 126u, GUI_CLIENT);
    gui_draw_rect(404u, 158u, 352u, 126u, GUI_CLIENT);
    draw_resource_panel(50u, 166u, "APPLICATION RESOURCE LEDGER", RESOURCE_APPLICATION,
                        task_scroll[0], task_selected_panel == 0u, task_auto_scroll[0]);
    draw_resource_panel(414u, 166u, "KERNEL: MEMORY EVENTS", RESOURCE_KERNEL,
                        task_scroll[1], task_selected_panel == 1u, task_auto_scroll[1]);

    gui_draw_rect(40u, 294u, 716u, 140u, GUI_CLIENT);
    gui_draw_string(50u, 302u, task_auto_scroll[2] != 0u && task_scroll[2] == 0u ?
                    "LIVE RESOURCE EVENT LOG AUTO:ON" :
                    "PAUSED - VIEWING HISTORY",
                    task_selected_panel == 2u ? GUI_YELLOW : GUI_GREEN);
    rows = resource_monitor_event_count_after(0u, task_view_clear_before_id);
    for (uint32_t row = 0u; row < 3u && row < rows; ++row) {
        const struct resource_event *event =
            resource_monitor_event_from_newest_after(0u,
                task_view_clear_before_id, task_scroll[2] + row);
        draw_resource_event_line(50u, 322u + row * 32u, event, 1u);
    }
    if (rows == 0u) gui_draw_string(50u, 322u, "No resource events recorded", GUI_MUTED);

    gui_draw_string(44u, 444u, "SNAKE RESOURCE STATISTICS (kmalloc payload bytes)", GUI_GREEN);
    draw_dec_value(44u, 464u, "App bytes: ", stats->current_bytes, GUI_BLUE);
    draw_dec_value(222u, 464u, "Peak: ", stats->peak_bytes, GUI_YELLOW);
    draw_dec_value(360u, 464u, "Kernel bytes: ", stats->kernel_current_bytes, GUI_TEXT);
    draw_dec_value(580u, 464u, "Live blocks: ", stats->outstanding_allocations, GUI_TEXT);
    draw_dec_value(44u, 484u, "Cumulative alloc: ", stats->cumulative_allocated_bytes, GUI_TEXT);
    draw_dec_value(282u, 484u, "Cumulative release: ", stats->cumulative_released_bytes, GUI_TEXT);
    draw_dec_value(44u, 504u, "Heap alloc volume: ",
                   stats->kernel_cumulative_allocated_bytes, GUI_TEXT);
    draw_dec_value(322u, 504u, "Heap released: ",
                   stats->kernel_cumulative_released_bytes, GUI_TEXT);
    gui_draw_rect(44u, 526u, 146u, 22u, GUI_TITLE);
    gui_draw_rect(198u, 526u, 126u, 22u, GUI_TITLE);
    gui_draw_rect(332u, 526u, 188u, 22u, GUI_TITLE);
    gui_draw_string(50u, 532u, task_auto_scroll[task_selected_panel] != 0u ?
                    "PAUSE LIVE VIEW" : "RESUME LIVE VIEW", GUI_BLUE);
    gui_draw_string(204u, 532u, "CLEAR VIEW", GUI_YELLOW);
    gui_draw_string(338u, 532u, "EXPORT UNAVAILABLE", GUI_MUTED);
    gui_draw_string(44u, 554u,
        "RAM: newest 128 events | FAT16 read-only; no reboot archive | T = PIT ticks",
        GUI_MUTED);
    gui_draw_string(44u, 574u,
        "UP/DOWN scroll | LEFT/RIGHT select view | 4 Snake | ESC desktop",
        GUI_TEXT);
}

static void draw_task_manager_window(void) {
    gui_set_translation(task_window_dx, task_window_dy);
    draw_task_manager();
    gui_set_translation(0, 0);
}

enum desktop_app {
    APP_LOGIN, APP_DESKTOP, APP_TERMINAL, APP_TASK_MANAGER, APP_NETWORK,
    APP_SNAKE, APP_DEVICES, APP_ASSISTANT
};

static enum desktop_app selected_icon = APP_DESKTOP;
static uint8_t start_menu_visible;
static char login_name[24];
static char login_password[24];
static uint8_t login_focus_password;
static uint8_t login_error;
static char assistant_prompt[96];
static char assistant_answer[192];
static uint8_t assistant_has_answer;

static enum desktop_app desktop_icon_at(uint32_t x, uint32_t y) {
    if (y >= 112u && y < 220u) {
        if (x >= 32u && x < 164u) return APP_TERMINAL;
        if (x >= 204u && x < 336u) return APP_TASK_MANAGER;
        if (x >= 376u && x < 508u) return APP_NETWORK;
        if (x >= 548u && x < 680u) return APP_SNAKE;
    }
    if (y >= 252u && y < 360u && x >= 32u && x < 164u)
        return APP_DEVICES;
    if (y >= 252u && y < 360u && x >= 204u && x < 336u)
        return APP_ASSISTANT;
    return APP_DESKTOP;
}

static enum key_code app_shortcut(enum desktop_app target) {
    switch (target) {
        case APP_TERMINAL: return KEY_1;
        case APP_TASK_MANAGER: return KEY_2;
        case APP_NETWORK: return KEY_3;
        case APP_SNAKE: return KEY_4;
        case APP_DEVICES: return KEY_5;
        case APP_ASSISTANT: return KEY_6;
        default: return KEY_NONE;
    }
}

static int32_t clamp_window_offset(int32_t offset, uint32_t origin,
                                   uint32_t size, uint32_t screen_size) {
    int32_t minimum = -(int32_t)origin;
    int32_t maximum = (int32_t)screen_size - (int32_t)(origin + size);
    if (maximum < minimum) maximum = minimum;
    if (offset < minimum) return minimum;
    if (offset > maximum) return maximum;
    return offset;
}

#define SNAKE_COLS 24u
#define SNAKE_ROWS 16u
#define SNAKE_CAPACITY (SNAKE_COLS * SNAKE_ROWS)
#define SNAKE_CELL 14u
#define SNAKE_X 232u
#define SNAKE_Y 154u

static uint8_t *snake_x;
static uint8_t *snake_y;
static uint8_t *snake_occupied;
static void *snake_allocation;
static uint32_t snake_request_id;
static uint8_t snake_active;
static uint32_t snake_length;
static uint8_t food_x;
static uint8_t food_y;
static int8_t direction_x;
static int8_t direction_y;
static int8_t next_direction_x;
static int8_t next_direction_y;
static uint8_t direction_pending;
static uint8_t snake_game_over;
static uint8_t snake_won;
static uint8_t snake_paused;
static uint32_t random_state = 0xC0FFEEu;
static void snake_reset(void);

static int snake_is_active(void) { return snake_active != 0u; }

static int snake_start(void) {
    uint8_t *state = 0;
    resource_monitor_lifecycle_event(0u, "Snake", "START", "PENDING");
    snake_request_id = resource_allocate(0u, "Snake", "MEMORY",
        SNAKE_CAPACITY * 3u, (void **)&state);
    if (state == 0) {
        resource_monitor_lifecycle_event(0u, "Snake", "START", "FAILED");
        return 0;
    }
    snake_allocation = state;
    snake_x = state;
    snake_y = state + SNAKE_CAPACITY;
    snake_occupied = state + SNAKE_CAPACITY * 2u;
    snake_active = 1u;
    resource_monitor_lifecycle_event(0u, "Snake", "START", "SUCCESS");
    snake_reset();
    return 1;
}

static int snake_stop(void) {
    if (snake_active == 0u) return 1;
    if (!resource_release(0u, "Snake", snake_request_id, "MEMORY",
                          snake_allocation, SNAKE_CAPACITY * 3u)) {
        resource_monitor_lifecycle_event(0u, "Snake", "TERMINATE", "FAILED");
        return 0;
    }
    resource_monitor_lifecycle_event(0u, "Snake", "TERMINATE", "SUCCESS");
    snake_x = 0;
    snake_y = 0;
    snake_occupied = 0;
    snake_allocation = 0;
    snake_active = 0u;
    return 1;
}

static uint32_t random_next(void) {
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
}

static void snake_place_food(void) {
    uint32_t i;
    uint32_t free_cells = 0u;
    uint32_t selected;

    for (i = 0u; i < SNAKE_CAPACITY; ++i) snake_occupied[i] = 0u;
    for (i = 0u; i < snake_length; ++i) {
        uint32_t cell = (uint32_t)snake_y[i] * SNAKE_COLS + snake_x[i];
        snake_occupied[cell] = 1u;
    }
    for (i = 0u; i < SNAKE_CAPACITY; ++i) {
        if (snake_occupied[i] == 0u) ++free_cells;
    }

    if (free_cells == 0u) {
        snake_game_over = 1u;
        snake_won = 1u;
        return;
    }

    /* Select by rank among empty cells so dense boards cannot fail randomly. */
    selected = random_next() % free_cells;
    for (i = 0u; i < SNAKE_CAPACITY; ++i) {
        if (snake_occupied[i] != 0u) continue;
        if (selected == 0u) {
            food_x = (uint8_t)(i % SNAKE_COLS);
            food_y = (uint8_t)(i / SNAKE_COLS);
            snake_won = 0u;
            return;
        }
        --selected;
    }
}

static void snake_reset(void) {
    snake_length = 3u;
    snake_x[0] = 12u; snake_y[0] = 8u;
    snake_x[1] = 11u; snake_y[1] = 8u;
    snake_x[2] = 10u; snake_y[2] = 8u;
    direction_x = 1; direction_y = 0;
    next_direction_x = direction_x;
    next_direction_y = direction_y;
    direction_pending = 0u;
    snake_game_over = 0u;
    snake_won = 0u;
    snake_paused = 0u;
    snake_place_food();
}

static void snake_turn(enum key_code key) {
    int8_t requested_x = direction_x;
    int8_t requested_y = direction_y;

    if (snake_game_over || direction_pending) return;
    if (key == KEY_UP) { requested_x = 0; requested_y = -1; }
    else if (key == KEY_DOWN) { requested_x = 0; requested_y = 1; }
    else if (key == KEY_LEFT) { requested_x = -1; requested_y = 0; }
    else if (key == KEY_RIGHT) { requested_x = 1; requested_y = 0; }
    else return;

    /* Reject a reversal against the last direction actually moved. */
    if (requested_x == -direction_x && requested_y == -direction_y) return;
    if (requested_x == direction_x && requested_y == direction_y) return;
    next_direction_x = requested_x;
    next_direction_y = requested_y;
    direction_pending = 1u;
}

static void snake_step(void) {
    int16_t nx;
    int16_t ny;
    uint32_t i;
    uint8_t ate;
    if (snake_game_over || snake_paused) return;
    if (direction_pending) {
        direction_x = next_direction_x;
        direction_y = next_direction_y;
        direction_pending = 0u;
    }
    nx = (int16_t)snake_x[0] + direction_x;
    ny = (int16_t)snake_y[0] + direction_y;
    if (nx < 0 || ny < 0 || nx >= (int16_t)SNAKE_COLS || ny >= (int16_t)SNAKE_ROWS) {
        snake_game_over = 1u; return;
    }
    ate = (uint8_t)(nx == food_x && ny == food_y);
    for (i = 0u; i < snake_length; ++i) {
        if (!ate && i == snake_length - 1u) break;
        if (snake_x[i] == (uint8_t)nx && snake_y[i] == (uint8_t)ny) {
            snake_game_over = 1u; return;
        }
    }
    if (ate && snake_length < SNAKE_CAPACITY) ++snake_length;
    for (i = snake_length - 1u; i > 0u; --i) {
        snake_x[i] = snake_x[i - 1u]; snake_y[i] = snake_y[i - 1u];
    }
    snake_x[0] = (uint8_t)nx; snake_y[0] = (uint8_t)ny;
    if (ate) snake_place_food();
}

static void draw_app_icon(uint32_t x, uint32_t y, const char *label,
                          uint8_t kind, uint8_t selected) {
    uint32_t i;
    gui_draw_rect(x, y, 132u, 108u, selected ? GUI_BLUE : GUI_BORDER);
    gui_draw_rect(x + 2u, y + 2u, 128u, 104u, GUI_CLIENT);
    if (kind == 1u) {
        gui_draw_rect(x + 42u, y + 25u, 48u, 36u, GUI_TITLE);
        gui_draw_rect(x + 46u, y + 30u, 40u, 24u, GUI_CLIENT);
        gui_draw_string(x + 50u, y + 38u, ">_", GUI_GREEN);
    } else if (kind == 2u) {
        for (i = 0u; i < 3u; ++i) {
            gui_draw_rect(x + 40u, y + 28u + i * 10u, 52u, 6u,
                          i == 1u ? GUI_GREEN : GUI_BLUE);
        }
    } else if (kind == 3u) {
        gui_draw_rect(x + 61u, y + 24u, 10u, 10u, GUI_GREEN);
        gui_draw_rect(x + 44u, y + 48u, 10u, 10u, GUI_YELLOW);
        gui_draw_rect(x + 78u, y + 48u, 10u, 10u, GUI_YELLOW);
        gui_draw_rect(x + 49u, y + 37u, 4u, 13u, GUI_TEXT);
        gui_draw_rect(x + 79u, y + 37u, 4u, 13u, GUI_TEXT);
    } else if (kind == 4u) {
        gui_draw_rect(x + 42u, y + 26u, 48u, 38u, GUI_BLACK);
        gui_draw_rect(x + 48u, y + 42u, 8u, 8u, GUI_GREEN);
        gui_draw_rect(x + 56u, y + 42u, 8u, 8u, GUI_GREEN);
        gui_draw_rect(x + 64u, y + 34u, 8u, 8u, GUI_GREEN);
        gui_draw_rect(x + 72u, y + 34u, 8u, 8u, GUI_RED);
    } else if (kind == 6u) {
        gui_draw_rect(x + 42u, y + 25u, 48u, 42u, GUI_TITLE);
        gui_draw_rect(x + 48u, y + 31u, 36u, 30u, GUI_CLIENT);
        gui_draw_string(x + 55u, y + 40u, "AI", GUI_GREEN);
        gui_draw_rect(x + 65u, y + 34u, 4u, 4u, GUI_YELLOW);
    } else {
        gui_draw_rect(x + 42u, y + 25u, 48u, 42u, GUI_BLACK);
        gui_draw_rect(x + 48u, y + 31u, 36u, 6u, GUI_GREEN);
        gui_draw_rect(x + 48u, y + 42u, 6u, 6u, GUI_BLUE);
        gui_draw_rect(x + 60u, y + 42u, 6u, 6u, GUI_BLUE);
        gui_draw_rect(x + 72u, y + 42u, 6u, 6u, GUI_BLUE);
        gui_draw_rect(x + 48u, y + 54u, 30u, 6u, GUI_YELLOW);
    }
    gui_draw_string(x + 18u, y + 80u, label, GUI_WHITE);
}

static void draw_login_screen(void) {
    gui_draw_desktop_background();
    gui_draw_rect(0u, 0u, gui_width(), 5u, GUI_GREEN);
    gui_draw_string(286u, 64u, "MINIOS", GUI_PURPLE);
    gui_draw_string(252u, 84u, "A small OS, ready to explore", GUI_MUTED);
    gui_draw_window(204u, 126u, 392u, 382u, "Welcome | Sign in");
    gui_draw_string(244u, 174u, "SIGN IN TO YOUR WORKSPACE", GUI_GREEN);
    gui_draw_string(244u, 206u, "Username", login_focus_password ? GUI_MUTED : GUI_YELLOW);
    gui_draw_rect(240u, 222u, 320u, 34u, login_focus_password ? GUI_BORDER : GUI_BLUE);
    gui_draw_rect(242u, 224u, 316u, 30u, GUI_CLIENT);
    if (login_name[0] != '\0')
        gui_draw_string(252u, 235u, login_name, GUI_TEXT);
    else if (login_focus_password != 0u)
        gui_draw_string(252u, 235u, "Type your username", GUI_MUTED);
    if (login_focus_password == 0u)
        gui_draw_rect(252u + string_length(login_name) * 8u, 232u,
                      2u, 16u, GUI_PURPLE);
    gui_draw_string(244u, 274u, "Password", login_focus_password ? GUI_YELLOW : GUI_MUTED);
    gui_draw_rect(240u, 290u, 320u, 34u, login_focus_password ? GUI_BLUE : GUI_BORDER);
    gui_draw_rect(242u, 292u, 316u, 30u, GUI_CLIENT);
    if (login_password[0] == '\0' && login_focus_password == 0u) {
        gui_draw_string(252u, 303u, "Type your password", GUI_MUTED);
    } else if (login_password[0] != '\0') {
        uint32_t index;
        for (index = 0u; login_password[index] != '\0'; ++index)
            gui_draw_char(252u + index * 8u, 303u, '*', GUI_TEXT);
    }
    if (login_focus_password != 0u)
        gui_draw_rect(252u + string_length(login_password) * 8u, 300u,
                      2u, 16u, GUI_PURPLE);
    gui_draw_rect(326u, 350u, 148u, 36u, GUI_GREEN);
    gui_draw_string(369u, 364u, "SIGN IN", GUI_BLACK);
    if (login_error != 0u)
        gui_draw_string(248u, 405u, "Login failed. Check the demo credentials.", GUI_RED);
    else
        gui_draw_string(228u, 405u, "Click field or press TAB | ENTER signs in", GUI_MUTED);
    gui_draw_string(264u, 446u, "Demo account: demo  /  1234", GUI_YELLOW);
    gui_draw_string(246u, 476u, "Demo login only; not secure authentication.", GUI_MUTED);
}

static char ascii_lower(char character) {
    if (character >= 'A' && character <= 'Z')
        return (char)(character - 'A' + 'a');
    return character;
}

static int text_contains(const char *text, const char *needle) {
    uint32_t start;
    uint32_t needle_length = string_length(needle);
    if (needle_length == 0u) return 1;
    for (start = 0u; text[start] != '\0'; ++start) {
        uint32_t offset = 0u;
        while (offset < needle_length && text[start + offset] != '\0' &&
               ascii_lower(text[start + offset]) == ascii_lower(needle[offset]))
            ++offset;
        if (offset == needle_length) return 1;
    }
    return 0;
}

static void assistant_reply(void) {
    const char *reply;
    if (assistant_prompt[0] == '\0') {
        reply = "Type a question first. Try: help, memory, CPU, Snake, or what is MiniOS?";
    } else if (text_contains(assistant_prompt, "hello")) {
        reply = "Hello! I am MiniAI, MiniOS's offline demo assistant. Ask me about this system.";
    } else if (text_contains(assistant_prompt, "help") ||
               text_contains(assistant_prompt, "what can")) {
        reply = "I can explain MiniOS, report free memory, describe keyboard controls, and help with Snake.";
    } else if (text_contains(assistant_prompt, "memory") ||
               text_contains(assistant_prompt, "ram")) {
        uint32_t length = append_text(assistant_answer, 0u, "MiniOS currently has ");
        length = append_number(assistant_answer, length, memory_free_pages() * 4u);
        length = append_text(assistant_answer, length,
            " KiB of free managed physical memory. This is a live kernel reading.");
        assistant_answer[length] = '\0';
        assistant_has_answer = 1u;
        return;
    } else if (text_contains(assistant_prompt, "cpu") ||
               text_contains(assistant_prompt, "processor")) {
        reply = "MiniOS is a 32-bit x86 protected-mode kernel. CPU utilization is not measured yet.";
    } else if (text_contains(assistant_prompt, "snake")) {
        reply = "Open Snake with shortcut 4. Use the arrow keys to steer, Space to pause, and Enter to restart.";
    } else if (text_contains(assistant_prompt, "minios") ||
               text_contains(assistant_prompt, "operating system")) {
        reply = "MiniOS is an educational x86 OS prototype with a framebuffer desktop, paging, a heap, and demo user processes.";
    } else {
        reply = "I do not know that yet. This offline demo understands help, memory, CPU, Snake, and MiniOS questions.";
    }
    uint32_t index = 0u;
    while (reply[index] != '\0' && index < sizeof(assistant_answer) - 1u) {
        assistant_answer[index] = reply[index];
        ++index;
    }
    assistant_answer[index] = '\0';
    assistant_has_answer = 1u;
}

static void draw_assistant_screen(void) {
    gui_draw_desktop_background();
    gui_draw_window(64u, 48u, 672u, 504u, "MiniAI Assistant | Offline mode");
    gui_draw_string(92u, 96u, "ASK MINIOS", GUI_GREEN);
    gui_draw_string(92u, 118u, "Answers use local rules and live kernel metrics.", GUI_MUTED);
    gui_draw_rect(88u, 148u, 624u, 46u, GUI_BORDER);
    gui_draw_rect(90u, 150u, 620u, 42u, GUI_CLIENT);
    gui_draw_text_box(102u, 154u, 600u, 36u, 16u,
        assistant_prompt[0] ? assistant_prompt : "Type a question, then press ENTER",
        assistant_prompt[0] ? GUI_TEXT : GUI_MUTED);
    gui_draw_rect(630u, 208u, 82u, 30u, GUI_GREEN);
    gui_draw_string(650u, 219u, "ASK  >", GUI_BLACK);
    gui_draw_string(92u, 224u, "RESPONSE", GUI_YELLOW);
    gui_draw_rect(88u, 244u, 624u, 220u, GUI_BORDER);
    gui_draw_rect(90u, 246u, 620u, 216u, GUI_CLIENT);
    if (assistant_has_answer)
        gui_draw_text_box(108u, 270u, 584u, 180u, 18u,
                          assistant_answer, GUI_TEXT);
    else
        gui_draw_text_box(108u, 270u, 584u, 180u, 18u,
            "Try: What is MiniOS?  |  How much memory is free?  |  Help me with Snake",
            GUI_MUTED);
    gui_draw_string(92u, 490u, "ENTER asks  |  BACKSPACE edits  |  ESC returns to desktop", GUI_MUTED);
    gui_draw_string(92u, 516u, "Offline assistant demo; no external AI service is connected.", GUI_YELLOW);
}

static void edit_text_buffer(char *buffer, uint32_t capacity, char character) {
    uint32_t length = string_length(buffer);
    if (character == '\b') {
        if (length != 0u) buffer[length - 1u] = '\0';
    } else if (character >= 0x20 && character <= 0x7E && length + 1u < capacity) {
        buffer[length] = character;
        buffer[length + 1u] = '\0';
    }
}

static int text_equals_case_insensitive(const char *left, const char *right) {
    uint32_t index = 0u;
    while (left[index] != '\0' && right[index] != '\0' &&
           ascii_lower(left[index]) == ascii_lower(right[index])) ++index;
    return left[index] == '\0' && right[index] == '\0';
}

static int login_credentials_valid(void) {
    return text_equals_case_insensitive(login_name, "demo") &&
           text_equals_case_insensitive(login_password, "1234");
}

static void draw_network_page(void) {
    gui_draw_desktop_background();
    gui_set_translation(network_window_dx, network_window_dy);
    gui_draw_window(150u, 80u, 500u, 400u, "Network Status");
    gui_draw_string(176u, 132u, "NETWORK INTERFACE", GUI_GREEN);
    gui_draw_string(176u, 166u, "Status: unavailable", GUI_YELLOW);
    gui_draw_string(176u, 192u, "No Ethernet or Wi-Fi driver is installed.", GUI_TEXT);
    gui_draw_string(176u, 220u, "This kernel has no TCP/IP stack or DHCP client.", GUI_TEXT);
    gui_draw_string(176u, 270u, "Network access is disabled in the QEMU run profile.", GUI_MUTED);
    gui_draw_string(176u, 330u, "Press ESC to return to the desktop.", GUI_TEXT);
    gui_set_translation(0, 0);
}

static void draw_device_manager(void) {
    uint32_t index;
    char number[11];

    gui_draw_desktop_background();
    gui_set_translation(devices_window_dx, devices_window_dy);
    gui_draw_window(112u, 48u, 576u, 500u, "Hardware Inventory | PCI read-only");
    draw_dec_value(144u, 94u, "Functions found: ", pci_device_count(), GUI_GREEN);
    gui_draw_string(144u, 116u, "Config reads only; BARs are not probed.", GUI_MUTED);
    gui_draw_string(144u, 150u, "Bus Slot Fn Vendor Device Class Subclass ProgIf", GUI_TEXT);

    for (index = 0u; index < pci_device_count() && index < 20u; ++index) {
        const struct pci_device *device = pci_device_at(index);
        uint32_t y = 174u + index * 16u;
        if (device == 0) break;

        uint32_to_hex(device->bus, number);
        gui_draw_string(148u, y, number + 8, GUI_MUTED);
        uint32_to_hex(device->slot, number);
        gui_draw_string(180u, y, number + 8, GUI_MUTED);
        uint32_to_hex(device->function, number);
        gui_draw_string(214u, y, number + 9, GUI_MUTED);
        uint32_to_hex(device->vendor_id, number);
        gui_draw_string(246u, y, number + 6, GUI_GREEN);
        uint32_to_hex(device->device_id, number);
        gui_draw_string(302u, y, number + 6, GUI_GREEN);
        uint32_to_hex(device->class_code, number);
        gui_draw_string(366u, y, number + 8, GUI_TEXT);
        uint32_to_hex(device->subclass, number);
        gui_draw_string(428u, y, number + 8, GUI_TEXT);
        uint32_to_hex(device->programming_interface, number);
        gui_draw_string(504u, y, number + 8, GUI_TEXT);
    }
    if (pci_device_count() == 0u)
        gui_draw_string(148u, 182u, "No PCI functions were discovered.", GUI_YELLOW);
    else if (pci_device_count() > 20u)
        gui_draw_string(148u, 510u, "Display limited to first 20 functions.", GUI_MUTED);
    gui_draw_string(144u, 526u, "Press ESC to return to the desktop.", GUI_TEXT);
    gui_set_translation(0, 0);
}

static void draw_snake(void) {
    uint32_t i;
    uint32_t score = snake_length - 3u;
    char score_text[11];
    gui_draw_desktop_background();
    gui_set_translation(snake_window_dx, snake_window_dy);
    gui_draw_window(100u, 52u, 600u, 480u, "Snake | Arrow keys move | Space pauses");
    gui_draw_rect(SNAKE_X, SNAKE_Y, SNAKE_COLS * SNAKE_CELL,
                  SNAKE_ROWS * SNAKE_CELL, GUI_BLACK);
    for (i = 1u; i < SNAKE_COLS; ++i)
        gui_draw_rect(SNAKE_X + i * SNAKE_CELL, SNAKE_Y, 1u,
                      SNAKE_ROWS * SNAKE_CELL, GUI_BORDER);
    for (i = 1u; i < SNAKE_ROWS; ++i)
        gui_draw_rect(SNAKE_X, SNAKE_Y + i * SNAKE_CELL,
                      SNAKE_COLS * SNAKE_CELL, 1u, GUI_BORDER);
    gui_draw_rect(SNAKE_X + food_x * SNAKE_CELL + 2u,
                  SNAKE_Y + food_y * SNAKE_CELL + 2u, 10u, 10u, GUI_RED);
    for (i = 0u; i < snake_length; ++i) {
        gui_draw_rect(SNAKE_X + snake_x[i] * SNAKE_CELL + 1u,
                      SNAKE_Y + snake_y[i] * SNAKE_CELL + 1u, 12u, 12u,
                      i == 0u ? GUI_GREEN : GUI_BLUE);
    }
    uint32_to_dec(score, score_text);
    gui_draw_string(232u, 400u, "Score:", GUI_TEXT);
    gui_draw_string(288u, 400u, score_text, GUI_GREEN);
    if (snake_game_over && snake_won) gui_draw_string(370u, 400u, "YOU WIN - ENTER restarts", GUI_GREEN);
    else if (snake_game_over) gui_draw_string(370u, 400u, "GAME OVER - ENTER restarts", GUI_YELLOW);
    else if (snake_paused) gui_draw_string(370u, 400u, "PAUSED - SPACE resumes", GUI_YELLOW);
    gui_set_translation(0, 0);
}

/* Refresh only the small status strip when pause/game-over state changes. */
static void draw_snake_status(void) {
    gui_set_translation(snake_window_dx, snake_window_dy);
    gui_draw_rect(370u, 396u, 310u, 22u, GUI_CLIENT);
    if (snake_game_over && snake_won)
        gui_draw_string(370u, 400u, "YOU WIN - ENTER restarts", GUI_GREEN);
    else if (snake_game_over)
        gui_draw_string(370u, 400u, "GAME OVER - ENTER restarts", GUI_YELLOW);
    else if (snake_paused)
        gui_draw_string(370u, 400u, "PAUSED - SPACE resumes", GUI_YELLOW);
    gui_set_translation(0, 0);
}

static void draw_snake_cell(uint8_t x, uint8_t y, uint32_t color) {
    gui_draw_rect(SNAKE_X + (uint32_t)x * SNAKE_CELL + 1u,
                  SNAKE_Y + (uint32_t)y * SNAKE_CELL + 1u,
                  12u, 12u, color);
}

/* Called around one movement tick; untouched cells and the window stay put. */
static void draw_snake_step(uint8_t old_head_x, uint8_t old_head_y,
                            uint8_t old_tail_x, uint8_t old_tail_y,
                            uint32_t old_length, uint8_t old_food_x,
                            uint8_t old_food_y, uint8_t old_game_over) {
    uint32_t score;
    char score_text[11];

    gui_set_translation(snake_window_dx, snake_window_dy);
    if (snake_x[0] != old_head_x || snake_y[0] != old_head_y) {
        draw_snake_cell(old_head_x, old_head_y, GUI_BLUE);
        if (snake_length == old_length)
            draw_snake_cell(old_tail_x, old_tail_y, GUI_BLACK);
        else {
            /* The food cell is cleared before placing its replacement. */
            draw_snake_cell(old_food_x, old_food_y, GUI_BLACK);
            gui_draw_rect(SNAKE_X + (uint32_t)food_x * SNAKE_CELL + 2u,
                          SNAKE_Y + (uint32_t)food_y * SNAKE_CELL + 2u,
                          10u, 10u, GUI_RED);
            score = snake_length - 3u;
            uint32_to_dec(score, score_text);
            gui_draw_rect(288u, 396u, 72u, 20u, GUI_CLIENT);
            gui_draw_string(288u, 400u, score_text, GUI_GREEN);
        }
        draw_snake_cell(snake_x[0], snake_y[0], GUI_GREEN);
    }
    if (!old_game_over && snake_game_over) draw_snake_status();
    gui_set_translation(0, 0);
}

#if defined(MINIOS_STORAGE_TEST) || defined(MINIOS_STORAGE_REJECT_TEST) || \
    defined(MINIOS_DBMS_TEST)
static void storage_tests_exit_qemu(int passed) {
    uint32_t code = passed ? 0x10u : 0x0Fu;
    __asm__ volatile ("outl %0, %1" : : "a"(code), "Nd"((uint16_t)0xF4u));
    for (;;) __asm__ volatile ("cli; hlt");
}
#endif

static void render_desktop(void) {
    uint32_t width = gui_width();
    uint32_t height = gui_height();
    uint32_t taskbar_y = height > 40u ? height - 40u : 0u;

    gui_draw_desktop_background();
    gui_draw_rect(18u, 58u, width - 36u, 1u, GUI_BORDER);
    gui_draw_string(24u, 20u, "MINIOS  /  WORKSPACE", GUI_PURPLE);
    gui_draw_string(24u, 38u, "Welcome back, admin  |  32-bit x86 workspace", GUI_MUTED);
    draw_app_icon(32u, 112u, "Terminal", 1u, selected_icon == APP_TERMINAL);
    draw_app_icon(204u, 112u, "Task Manager", 2u, selected_icon == APP_TASK_MANAGER);
    draw_app_icon(376u, 112u, "Network", 3u, selected_icon == APP_NETWORK);
    draw_app_icon(548u, 112u, "Snake", 4u, selected_icon == APP_SNAKE);
    draw_app_icon(32u, 252u, "Devices", 5u, selected_icon == APP_DEVICES);
    draw_app_icon(204u, 252u, "MiniAI", 6u, selected_icon == APP_ASSISTANT);
    gui_draw_string(24u, height - 58u, "Double-click an app or press 1-6  |  Ctrl+Y opens Task Manager", GUI_MUTED);
    gui_draw_rect(0u, taskbar_y, width, 40u, GUI_TASKBAR);
    gui_draw_rect(0u, taskbar_y, width, 2u, GUI_BORDER);
    gui_draw_rect(12u, taskbar_y + 7u, 82u, 26u, GUI_TITLE);
    gui_draw_string(27u, taskbar_y + 16u, "LAUNCH", GUI_WHITE);
    gui_draw_string(112u, taskbar_y + 16u, snake_is_active() ? "Snake running" : "Workspace ready", GUI_TEXT);
    gui_draw_string(width - 170u, taskbar_y + 16u, "TICKS", GUI_MUTED);
    draw_dec_value(width - 122u, taskbar_y + 16u, "", timer_ticks(), GUI_GREEN);
    if (start_menu_visible != 0u) {
        gui_draw_rect(12u, taskbar_y - 146u, 190u, 138u, GUI_BORDER);
        gui_draw_rect(14u, taskbar_y - 144u, 186u, 134u, GUI_CLIENT);
        gui_draw_string(24u, taskbar_y - 132u, "APPLICATIONS", GUI_GREEN);
        gui_draw_string(24u, taskbar_y - 108u, "Task Manager  (Ctrl+Y)", GUI_TEXT);
        gui_draw_string(24u, taskbar_y - 84u, "Snake", GUI_TEXT);
        gui_draw_string(24u, taskbar_y - 60u, "Terminal", GUI_TEXT);
        gui_draw_string(24u, taskbar_y - 36u, "Devices", GUI_TEXT);
    }
}

void kernel_main(uint32_t boot_magic, uint32_t multiboot_info_address) {
    const struct multiboot_info *info =
        (const struct multiboot_info *)(uintptr_t)multiboot_info_address;
    enum desktop_app app = APP_LOGIN;
    enum desktop_app task_manager_return = APP_DESKTOP;
    uint32_t next_snake_tick = 0u;
    uint32_t last_task_event_id = UINT32_MAX;
    uint32_t mouse_x = 0u, mouse_y = 0u;
    uint32_t drag_mouse_x = 0u, drag_mouse_y = 0u;
    int32_t drag_origin_x = 0, drag_origin_y = 0;
    enum desktop_app dragged_window = APP_DESKTOP;
    uint8_t last_mouse_buttons = 0u;
    uint8_t window_dragging = 0u;
    uint8_t timer_smoke_reported = 0u;
    uint8_t keyboard_event_reported = 0u;
    uint8_t mouse_event_reported = 0u;
#ifdef MINIOS_SCHEDULER_TEST
    int user_elf_passed = 0;
#endif

    if (boot_magic != MULTIBOOT_BOOT_MAGIC) {
        halt_forever();
    }
    interrupts_init();
    pci_enumerate();
    kernel_log("MiniOS: read-only PCI scan found ");
    kernel_log_hex(pci_device_count());
    kernel_log(" functions.\n");
#ifdef MINIOS_PCI_TEST
    pci_test_exit_qemu(pci_self_check());
#endif
    if (!memory_init(info, multiboot_info_address,
                     (uint32_t)(uintptr_t)_kernel_start,
                     (uint32_t)(uintptr_t)_kernel_end)) {
        kernel_log("MiniOS: memory map unavailable or invalid; allocator disabled.\n");
    }
    if (!keyboard_self_check()) {
        kernel_log("MiniOS: keyboard decoder self-check failed.\n");
    } else {
        kernel_log("MiniOS: keyboard decoder self-check passed.\n");
    }
    if (!gui_init(info)) {
        kernel_log("MiniOS: Multiboot framebuffer is missing or unsupported.\n");
        halt_forever();
    }
    mouse_x = gui_width() / 2u;
    mouse_y = gui_height() / 2u;
    if (!mouse_self_check()) kernel_log("MiniOS: PS/2 mouse packet self-check failed.\n");
    if (mouse_init()) {
        kernel_log("MiniOS: PS/2 mouse enabled.\n");
        gui_cursor_move(mouse_x, mouse_y);
    } else {
        kernel_log("MiniOS: PS/2 mouse unavailable; keyboard remains active.\n");
    }
    if (!paging_init(info)) {
        kernel_log("MiniOS: paging setup failed; running without paging.\n");
    }
#ifdef MINIOS_FAULT_TEST
    if (!paging_is_ready() || !paging_fault_tests_run()) {
        kernel_log("MiniOS: paging protection fault checks failed.\n");
        halt_forever();
    }
    kernel_log("MiniOS: null guard and CR0.WP fault checks passed.\n");
    paging_fault_tests_exit_qemu();
#endif
    if (paging_is_ready()) {
        heap_init();
    } else {
        kernel_log("MiniOS: kernel heap disabled because paging is unavailable.\n");
    }
    resource_monitor_init();
    dbms_init();
#ifdef MINIOS_DBMS_TEST
    storage_tests_exit_qemu(dbms_persistence_self_check());
#endif
#ifdef MINIOS_STORAGE_TEST
    storage_tests_exit_qemu(paging_is_ready() && memory_is_ready() &&
                            storage_self_check());
#elif defined(MINIOS_STORAGE_REJECT_TEST)
    storage_tests_exit_qemu(paging_is_ready() && memory_is_ready() &&
                            storage_reject_invalid_partition_check());
#endif
    if (paging_is_ready()) {
#ifdef MINIOS_PROCESS_TEST
        int process_checks_passed = ring3_run_smoke();
        process_checks_passed = run_boot_user_elf(info) && process_checks_passed;
        ring3_process_tests_exit_qemu(process_checks_passed);
#else
        (void)ring3_run_smoke();
#ifdef MINIOS_SCHEDULER_TEST
        user_elf_passed = run_boot_user_elf(info);
#else
        (void)run_boot_user_elf(info);
#endif
#endif
    }
    draw_login_screen();
    interrupts_enable_timer_keyboard();
#ifdef MINIOS_SCHEDULER_TEST
    ring3_scheduler_tests_exit_qemu(user_elf_passed && ring3_scheduler_run());
#else
    (void)ring3_scheduler_run();
#endif
    for (;;) {
        enum key_code key = keyboard_poll();
        uint8_t force_redraw = 0u;
        char typed_character;
        struct mouse_event mouse;
        while (mouse_poll(&mouse)) {
            int32_t next_x = (int32_t)mouse_x + mouse.delta_x * MOUSE_POINTER_SCALE;
            int32_t next_y = (int32_t)mouse_y + mouse.delta_y * MOUSE_POINTER_SCALE;
            uint8_t left_down = mouse.buttons & 1u;
            if (next_x < 0) next_x = 0;
            if (next_y < 0) next_y = 0;
            if ((uint32_t)next_x >= gui_width()) next_x = (int32_t)gui_width() - 1;
            if ((uint32_t)next_y >= gui_height()) next_y = (int32_t)gui_height() - 1;
            mouse_x = (uint32_t)next_x;
            mouse_y = (uint32_t)next_y;
            gui_cursor_move(mouse_x, mouse_y);
            if (mouse_event_reported == 0u) {
                kernel_log("MiniOS: mouse event delivered to desktop loop.\n");
                mouse_event_reported = 1u;
            }
            if (window_dragging != 0u && left_down != 0u) {
                int32_t move_x = (int32_t)mouse_x - (int32_t)drag_mouse_x;
                int32_t move_y = (int32_t)mouse_y - (int32_t)drag_mouse_y;
                if (dragged_window == APP_TASK_MANAGER) {
                    task_window_dx = clamp_window_offset(drag_origin_x + move_x,
                        28u, 744u, gui_width());
                    task_window_dy = clamp_window_offset(drag_origin_y + move_y,
                        32u, 550u, gui_height());
                } else if (dragged_window == APP_SNAKE) {
                    snake_window_dx = clamp_window_offset(drag_origin_x + move_x,
                        100u, 600u, gui_width());
                    snake_window_dy = clamp_window_offset(drag_origin_y + move_y,
                        52u, 480u, gui_height());
                } else if (dragged_window == APP_TERMINAL) {
                    terminal_window_dx = clamp_window_offset(drag_origin_x + move_x,
                        36u, 728u, gui_width());
                    terminal_window_dy = clamp_window_offset(drag_origin_y + move_y,
                        54u, 428u, gui_height());
                } else if (dragged_window == APP_NETWORK) {
                    network_window_dx = clamp_window_offset(drag_origin_x + move_x,
                        150u, 500u, gui_width());
                    network_window_dy = clamp_window_offset(drag_origin_y + move_y,
                        80u, 400u, gui_height());
                } else if (dragged_window == APP_DEVICES) {
                    devices_window_dx = clamp_window_offset(drag_origin_x + move_x,
                        112u, 576u, gui_width());
                    devices_window_dy = clamp_window_offset(drag_origin_y + move_y,
                        48u, 500u, gui_height());
                }
                force_redraw = 1u;
            }
            if (left_down == 0u) window_dragging = 0u;
            if (left_down != 0u && (last_mouse_buttons & 1u) == 0u) {
                uint32_t taskbar_y = gui_height() > 40u ? gui_height() - 40u : 0u;
                if (app == APP_DESKTOP) {
                    if (start_menu_visible != 0u && mouse_x >= 12u && mouse_x < 202u &&
                        mouse_y >= taskbar_y - 144u && mouse_y < taskbar_y - 10u) {
                        if (mouse_y < taskbar_y - 96u) key = KEY_2;
                        else if (mouse_y < taskbar_y - 72u) key = KEY_4;
                        else if (mouse_y < taskbar_y - 48u) key = KEY_1;
                        else key = KEY_5;
                        start_menu_visible = 0u;
                    } else if (mouse_x >= 12u && mouse_x < 94u && mouse_y >= taskbar_y) {
                        start_menu_visible ^= 1u;
                        force_redraw = 1u;
                    } else if (start_menu_visible != 0u) {
                        start_menu_visible = 0u;
                        force_redraw = 1u;
                    } else {
                        enum desktop_app clicked = desktop_icon_at(mouse_x, mouse_y);
                        if (clicked != APP_DESKTOP) {
                            selected_icon = clicked;
                            key = app_shortcut(clicked);
                        }
                    }
                } else if (app == APP_LOGIN) {
                    if (mouse_x >= 224u && mouse_x < 576u &&
                        mouse_y >= 200u && mouse_y < 256u) {
                        login_focus_password = 0u;
                        force_redraw = 1u;
                    } else if (mouse_x >= 224u && mouse_x < 576u &&
                               mouse_y >= 268u && mouse_y < 324u) {
                        login_focus_password = 1u;
                        force_redraw = 1u;
                    } else if (mouse_x >= 326u && mouse_x < 474u &&
                               mouse_y >= 350u && mouse_y < 386u) {
                        login_focus_password = 1u;
                        key = KEY_ENTER;
                    }
                } else if (app == APP_TASK_MANAGER) {
                    uint32_t wx = (uint32_t)((int32_t)28 + task_window_dx);
                    uint32_t wy = (uint32_t)((int32_t)32 + task_window_dy);
                    if (mouse_x >= wx + 722u && mouse_x < wx + 738u &&
                        mouse_y >= wy + 6u && mouse_y < wy + 22u) key = KEY_ESCAPE;
                    else if (mouse_x >= wx + 702u && mouse_x < wx + 718u &&
                             mouse_y >= wy + 6u && mouse_y < wy + 22u)
                        key = KEY_TASK_TOGGLE;
                    else if (mouse_y >= wy + 2u && mouse_y < wy + 28u) {
                        window_dragging = 1u;
                        dragged_window = APP_TASK_MANAGER;
                        drag_mouse_x = mouse_x; drag_mouse_y = mouse_y;
                        drag_origin_x = task_window_dx; drag_origin_y = task_window_dy;
                    } else if (mouse_y >= wy + 158u && mouse_y < wy + 284u) {
                        task_selected_panel = mouse_x < (uint32_t)((int32_t)400 + task_window_dx) ?
                            0u : 1u;
                        force_redraw = 1u;
                    } else if (mouse_y >= wy + 294u && mouse_y < wy + 434u) {
                        task_selected_panel = 2u;
                        force_redraw = 1u;
                    } else if (mouse_y >= wy + 130u && mouse_y < wy + 152u &&
                               mouse_x >= wx + 606u && mouse_x < wx + 748u) {
                        task_selected_panel = 3u;
                        force_redraw = 1u;
                    } else if (mouse_y >= wy + 526u && mouse_y < wy + 548u &&
                               mouse_x >= wx + 44u && mouse_x < wx + 190u) {
                        if (task_selected_panel < 3u) {
                            task_auto_scroll[task_selected_panel] ^= 1u;
                            if (task_auto_scroll[task_selected_panel] != 0u)
                                task_scroll[task_selected_panel] = 0u;
                            force_redraw = 1u;
                        }
                    } else if (mouse_y >= wy + 526u && mouse_y < wy + 548u &&
                               mouse_x >= wx + 198u && mouse_x < wx + 324u) {
                        task_view_clear_before_id = resource_monitor_last_event_id();
                        task_scroll[0] = task_scroll[1] = task_scroll[2] = 0u;
                        task_auto_scroll[0] = task_auto_scroll[1] =
                            task_auto_scroll[2] = 1u;
                        force_redraw = 1u;
                    }
                } else {
                    uint32_t close_x = 0u, close_y = 0u;
                    uint8_t has_window = 1u;
                    if (app == APP_SNAKE) {
                        close_x = (uint32_t)((int32_t)678 + snake_window_dx);
                        close_y = (uint32_t)((int32_t)58 + snake_window_dy);
                    }
                    else if (app == APP_TERMINAL) {
                        close_x = (uint32_t)((int32_t)742 + terminal_window_dx);
                        close_y = (uint32_t)((int32_t)60 + terminal_window_dy);
                    }
                    else if (app == APP_NETWORK) {
                        close_x = (uint32_t)((int32_t)628 + network_window_dx);
                        close_y = (uint32_t)((int32_t)86 + network_window_dy);
                    }
                    else if (app == APP_DEVICES) {
                        close_x = (uint32_t)((int32_t)666 + devices_window_dx);
                        close_y = (uint32_t)((int32_t)54 + devices_window_dy);
                    }
                    else if (app == APP_ASSISTANT) {
                        close_x = 714u;
                        close_y = 54u;
                    }
                    else has_window = 0u;
                    if (has_window != 0u && mouse_x >= close_x && mouse_x < close_x + 16u &&
                        mouse_y >= close_y && mouse_y < close_y + 16u) key = KEY_ESCAPE;
                    else if (app == APP_SNAKE &&
                             mouse_y >= (uint32_t)((int32_t)54 + snake_window_dy) &&
                             mouse_y < (uint32_t)((int32_t)80 + snake_window_dy) &&
                             mouse_x >= (uint32_t)((int32_t)100 + snake_window_dx) &&
                             mouse_x < (uint32_t)((int32_t)700 + snake_window_dx)) {
                        window_dragging = 1u;
                        dragged_window = APP_SNAKE;
                        drag_mouse_x = mouse_x; drag_mouse_y = mouse_y;
                        drag_origin_x = snake_window_dx; drag_origin_y = snake_window_dy;
                    } else if (app == APP_TERMINAL &&
                               mouse_y >= (uint32_t)((int32_t)54 + terminal_window_dy) &&
                               mouse_y < (uint32_t)((int32_t)80 + terminal_window_dy) &&
                               mouse_x >= (uint32_t)((int32_t)36 + terminal_window_dx) &&
                               mouse_x < (uint32_t)((int32_t)764 + terminal_window_dx)) {
                        window_dragging = 1u;
                        dragged_window = APP_TERMINAL;
                        drag_mouse_x = mouse_x; drag_mouse_y = mouse_y;
                        drag_origin_x = terminal_window_dx; drag_origin_y = terminal_window_dy;
                    } else if (app == APP_NETWORK &&
                               mouse_y >= (uint32_t)((int32_t)80 + network_window_dy) &&
                               mouse_y < (uint32_t)((int32_t)106 + network_window_dy) &&
                               mouse_x >= (uint32_t)((int32_t)150 + network_window_dx) &&
                               mouse_x < (uint32_t)((int32_t)650 + network_window_dx)) {
                        window_dragging = 1u;
                        dragged_window = APP_NETWORK;
                        drag_mouse_x = mouse_x; drag_mouse_y = mouse_y;
                        drag_origin_x = network_window_dx; drag_origin_y = network_window_dy;
                    } else if (app == APP_DEVICES &&
                               mouse_y >= (uint32_t)((int32_t)48 + devices_window_dy) &&
                               mouse_y < (uint32_t)((int32_t)74 + devices_window_dy) &&
                               mouse_x >= (uint32_t)((int32_t)112 + devices_window_dx) &&
                               mouse_x < (uint32_t)((int32_t)688 + devices_window_dx)) {
                        window_dragging = 1u;
                        dragged_window = APP_DEVICES;
                        drag_mouse_x = mouse_x; drag_mouse_y = mouse_y;
                        drag_origin_x = devices_window_dx; drag_origin_y = devices_window_dy;
                    } else if (app == APP_ASSISTANT && mouse_x >= 630u &&
                               mouse_x < 712u && mouse_y >= 208u && mouse_y < 238u) {
                        key = KEY_ENTER;
                    }
                }
            }
            last_mouse_buttons = mouse.buttons;
        }
        while ((typed_character = keyboard_text_poll()) != 0) {
            if (app == APP_LOGIN) {
                edit_text_buffer(login_focus_password ? login_password : login_name,
                    24u, typed_character);
                login_error = 0u;
                force_redraw = 1u;
            } else if (app == APP_ASSISTANT) {
                edit_text_buffer(assistant_prompt, sizeof(assistant_prompt),
                                 typed_character);
                assistant_has_answer = 0u;
                force_redraw = 1u;
            } else if (app == APP_TERMINAL) {
                uint32_t length = string_length(dbms_terminal_input);
                uint32_t old_length = length;
                if (typed_character == '\b') {
                    if (length != 0u) dbms_terminal_input[length - 1u] = '\0';
                } else if (length + 1u < sizeof(dbms_terminal_input)) {
                    dbms_terminal_input[length] = typed_character;
                    dbms_terminal_input[length + 1u] = '\0';
                }
                if (length != string_length(dbms_terminal_input))
                    update_terminal_input_cell(typed_character, old_length);
            }
        }
        if (key == KEY_TASK_TOGGLE && app != APP_LOGIN) {
            if (app == APP_TASK_MANAGER) {
                app = task_manager_return;
                kernel_log("MiniOS: Task Manager hidden by Ctrl+Y.\n");
            } else {
                task_manager_return = app;
                app = APP_TASK_MANAGER;
                kernel_log("MiniOS: Task Manager opened by Ctrl+Y.\n");
            }
            key = KEY_NONE;
            force_redraw = 1u;
        }
        if (key != KEY_NONE && keyboard_event_reported == 0u) {
            kernel_log("MiniOS: keyboard event delivered to desktop loop.\n");
            keyboard_event_reported = 1u;
        }
        if (timer_smoke_reported == 0u && timer_ticks() >= 100u) {
            kernel_log("MiniOS: PIT tick delivery verified (100 ticks).\n");
            timer_smoke_reported = 1u;
        }
        if (app == APP_LOGIN) {
            if (key == KEY_TAB) {
                login_focus_password ^= 1u;
                force_redraw = 1u;
            } else if (key == KEY_ENTER) {
                if (login_focus_password == 0u) {
                    login_focus_password = 1u;
                } else if (login_credentials_valid()) {
                    app = APP_DESKTOP;
                    login_error = 0u;
                    force_redraw = 1u;
                } else {
                    login_error = 1u;
                    login_password[0] = '\0';
                    login_focus_password = 1u;
                    force_redraw = 1u;
                }
            } else if (key == KEY_ESCAPE) {
                login_password[0] = '\0';
                login_focus_password = 0u;
                login_error = 0u;
                force_redraw = 1u;
            }
        } else if (app == APP_DESKTOP) {
            if (key == KEY_1) app = APP_TERMINAL;
            else if (key == KEY_2) { task_manager_return = APP_DESKTOP; app = APP_TASK_MANAGER; }
            else if (key == KEY_3) app = APP_NETWORK;
            else if (key == KEY_5) app = APP_DEVICES;
            else if (key == KEY_6) app = APP_ASSISTANT;
            else if (key == KEY_4) {
                if (snake_active != 0u || snake_start()) {
                    app = APP_SNAKE;
                    next_snake_tick = timer_ticks() + 10u;
                    kernel_log("MiniOS: Snake app launched from keyboard shortcut.\n");
                } else {
                    kernel_log("MiniOS: Snake start failed; resource allocation was rejected.\n");
                }
            }
            if (app == APP_TERMINAL) {
                draw_terminal_screen(info);
            } else if (app == APP_NETWORK) draw_network_page();
            else if (app == APP_SNAKE) draw_snake();
            else if (app == APP_DEVICES) draw_device_manager();
            else if (app == APP_ASSISTANT) draw_assistant_screen();
        } else if (app == APP_TASK_MANAGER) {
            uint32_t panel_count;
            uint32_t visible_rows;
            if (key == KEY_ESCAPE) {
                app = task_manager_return;
                force_redraw = 1u;
                kernel_log("MiniOS: returned to desktop.\n");
            } else if (key == KEY_4 && snake_active != 0u) {
                app = APP_SNAKE;
                draw_snake();
            } else if (key == KEY_LEFT) {
                task_selected_panel = task_selected_panel == 0u ? 3u :
                                      (uint8_t)(task_selected_panel - 1u);
            } else if (key == KEY_RIGHT) {
                task_selected_panel = (uint8_t)((task_selected_panel + 1u) % 4u);
            } else if (key == KEY_UP || key == KEY_DOWN) {
                if (task_selected_panel < 3u) {
                    visible_rows = 3u;
                    panel_count = resource_monitor_event_count_after(
                        task_selected_panel == 0u ? RESOURCE_APPLICATION :
                        (task_selected_panel == 1u ? RESOURCE_KERNEL : 0u),
                        task_view_clear_before_id);
                    if (key == KEY_UP && panel_count > visible_rows &&
                        task_scroll[task_selected_panel] < panel_count - visible_rows) {
                        ++task_scroll[task_selected_panel];
                        task_auto_scroll[task_selected_panel] = 0u;
                    } else if (key == KEY_DOWN && task_scroll[task_selected_panel] != 0u) {
                        --task_scroll[task_selected_panel];
                        if (task_scroll[task_selected_panel] == 0u)
                            task_auto_scroll[task_selected_panel] = 1u;
                    }
                }
            }
        } else if (app == APP_TERMINAL) {
            if (key == KEY_ESCAPE) {
                app = APP_DESKTOP;
                force_redraw = 1u;
            } else if (key == KEY_ENTER) {
                uint32_t length = string_length(dbms_terminal_input);
                if (length != 0u) {
                    uint32_t i;
                    for (i = 0u; i <= length; ++i) dbms_last_query[i] = dbms_terminal_input[i];
                    dbms_execute(dbms_terminal_input, dbms_terminal_output,
                                 sizeof(dbms_terminal_output));
                    kernel_log("MiniOS SQL> ");
                    kernel_log_buffer(dbms_terminal_input, length);
                    kernel_log("\nMiniOS SQL result:\n");
                    kernel_log_buffer(dbms_terminal_output,
                                      string_length(dbms_terminal_output));
                    kernel_log("\n");
                    dbms_terminal_input[0] = '\0';
                    gui_set_translation(terminal_window_dx, terminal_window_dy);
                    draw_terminal_output_region();
                    draw_terminal_input_region();
                    gui_set_translation(0, 0);
                }
            }
        } else if (app == APP_ASSISTANT) {
            if (key == KEY_ESCAPE) {
                app = APP_DESKTOP;
                force_redraw = 1u;
            } else if (key == KEY_ENTER) {
                assistant_reply();
                force_redraw = 1u;
            }
        } else if (key == KEY_ESCAPE) {
            if (app != APP_SNAKE || snake_stop()) {
                app = APP_DESKTOP;
                render_desktop();
                kernel_log("MiniOS: returned to desktop.\n");
            } else {
                gui_draw_string(288u, 424u, "Resource release failed; press ESC to retry.", GUI_RED);
            }
        } else if (app == APP_NETWORK) {
            /* Keep the status view available; network hardware is not configured. */
        } else if (app == APP_DEVICES) {
            /* Inventory is populated once at boot and uses config reads only. */
        } else if (app == APP_SNAKE) {
            if (key == KEY_2) {
                task_manager_return = APP_SNAKE;
                app = APP_TASK_MANAGER;
            } else if (key == KEY_ENTER && snake_game_over) {
                snake_reset();
                next_snake_tick = timer_ticks() + 10u;
                draw_snake();
            } else if (key == KEY_SPACE && !snake_game_over) {
                snake_paused ^= 1u;
                draw_snake_status();
            } else {
                snake_turn(key);
            }
        }
        if (app == APP_TASK_MANAGER && task_selected_panel == 3u &&
            timer_ticks() >= task_process_refresh_tick + 5u) {
            gui_draw_desktop_background();
            draw_task_manager_window();
            task_process_refresh_tick = timer_ticks();
        } else if (app == APP_TASK_MANAGER &&
            (key != KEY_NONE || resource_monitor_last_event_id() != last_task_event_id)) {
            if (resource_monitor_last_event_id() != last_task_event_id) {
                uint8_t panel;
                for (panel = 0u; panel < 3u; ++panel) {
                    if (task_auto_scroll[panel] == 0u) {
                        uint8_t perspective = panel == 0u ? RESOURCE_APPLICATION :
                                              (panel == 1u ? RESOURCE_KERNEL : 0u);
                        task_scroll[panel] += resource_monitor_events_after(
                            last_task_event_id, perspective);
                    }
                }
            }
            gui_draw_desktop_background();
            draw_task_manager_window();
            last_task_event_id = resource_monitor_last_event_id();
        } else if (app == APP_SNAKE && key == KEY_4) {
            draw_snake();
        }
        if (force_redraw != 0u) {
            if (app == APP_LOGIN) draw_login_screen();
            else if (app == APP_DESKTOP) render_desktop();
            else if (app == APP_TERMINAL) draw_terminal_screen(info);
            else if (app == APP_TASK_MANAGER) {
                gui_draw_desktop_background();
                draw_task_manager_window();
                last_task_event_id = resource_monitor_last_event_id();
            } else if (app == APP_NETWORK) draw_network_page();
            else if (app == APP_SNAKE) draw_snake();
            else if (app == APP_DEVICES) draw_device_manager();
            else if (app == APP_ASSISTANT) draw_assistant_screen();
        }
        if (snake_active != 0u && snake_paused == 0u &&
            !snake_game_over && timer_ticks() >= next_snake_tick) {
            uint8_t old_head_x = snake_x[0];
            uint8_t old_head_y = snake_y[0];
            uint8_t old_tail_x = snake_x[snake_length - 1u];
            uint8_t old_tail_y = snake_y[snake_length - 1u];
            uint8_t old_food_x = food_x;
            uint8_t old_food_y = food_y;
            uint8_t old_game_over = snake_game_over;
            uint32_t old_length = snake_length;
            next_snake_tick = timer_ticks() + 10u;
            snake_step();
            if (app == APP_SNAKE)
                draw_snake_step(old_head_x, old_head_y, old_tail_x, old_tail_y,
                                old_length, old_food_x, old_food_y,
                                old_game_over);
        }
        __asm__ volatile ("sti; hlt" : : : "memory");
    }
}
