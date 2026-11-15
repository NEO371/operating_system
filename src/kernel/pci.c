#include "pci.h"

#include "interrupts.h"

#include <stdint.h>

#define PCI_CONFIG_ADDRESS 0xCF8u
#define PCI_CONFIG_DATA 0xCFCu
#define PCI_CLASS_DISPLAY 0x03u

static struct pci_device devices[PCI_DEVICE_LIMIT];
static uint32_t device_count;
static uint32_t overflow_count;
static uint8_t enumeration_complete;

static void config_write_address(uint32_t address) {
    __asm__ volatile ("outl %0, %w1" : : "a"(address),
                      "d"((uint16_t)PCI_CONFIG_ADDRESS));
}

static uint32_t config_read_data(void) {
    uint32_t value;
    __asm__ volatile ("inl %w1, %0" : "=a"(value)
                      : "d"((uint16_t)PCI_CONFIG_DATA));
    return value;
}

static uint32_t config_read(uint8_t bus, uint8_t slot, uint8_t function,
                            uint8_t offset) {
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) |
        ((uint32_t)slot << 11) | ((uint32_t)function << 8) |
        ((uint32_t)offset & 0xFCu);
    config_write_address(address);
    return config_read_data();
}

static uint8_t config_byte(uint8_t bus, uint8_t slot, uint8_t function,
                           uint8_t offset) {
    uint32_t value = config_read(bus, slot, function, offset);
    return (uint8_t)(value >> ((offset & 3u) * 8u));
}

static void record_function(uint8_t bus, uint8_t slot, uint8_t function) {
    uint32_t id = config_read(bus, slot, function, 0u);
    struct pci_device *device;
    uint8_t header;

    if ((uint16_t)id == 0xFFFFu) return;
    if (device_count == PCI_DEVICE_LIMIT) {
        ++overflow_count;
        return;
    }

    device = &devices[device_count++];
    device->bus = bus;
    device->slot = slot;
    device->function = function;
    device->vendor_id = (uint16_t)id;
    device->device_id = (uint16_t)(id >> 16);
    device->programming_interface = config_byte(bus, slot, function, 9u);
    device->subclass = config_byte(bus, slot, function, 10u);
    device->class_code = config_byte(bus, slot, function, 11u);
    header = config_byte(bus, slot, function, 14u);
    device->header_type = (uint8_t)(header & 0x7Fu);
}

void pci_enumerate(void) {
    uint32_t bus;

    device_count = 0u;
    overflow_count = 0u;
    enumeration_complete = 0u;

    /* Exhaustive bus scan keeps bridge traversal and BAR probing out of this driver. */
    for (bus = 0u; bus <= 255u; ++bus) {
        uint32_t slot;
        for (slot = 0u; slot < 32u; ++slot) {
            uint32_t function;
            uint32_t functions = 1u;
            uint32_t id = config_read((uint8_t)bus, (uint8_t)slot, 0u, 0u);
            if ((uint16_t)id == 0xFFFFu) continue;
            record_function((uint8_t)bus, (uint8_t)slot, 0u);
            if ((config_byte((uint8_t)bus, (uint8_t)slot, 0u, 14u) & 0x80u) != 0u)
                functions = 8u;
            for (function = 1u; function < functions; ++function)
                record_function((uint8_t)bus, (uint8_t)slot, (uint8_t)function);
        }
    }
    enumeration_complete = 1u;
}

uint32_t pci_device_count(void) {
    return device_count;
}

const struct pci_device *pci_device_at(uint32_t index) {
    if (index >= device_count) return 0;
    return &devices[index];
}

int pci_self_check(void) {
    uint32_t index;
    int found_display = 0;

    if (!enumeration_complete || device_count == 0u) return 0;
    for (index = 0u; index < device_count; ++index) {
        if (devices[index].vendor_id == 0xFFFFu) return 0;
        if (devices[index].class_code == PCI_CLASS_DISPLAY) found_display = 1;
    }
    return found_display && overflow_count == 0u;
}

void pci_test_exit_qemu(int passed) {
    uint32_t code = passed ? 0x10u : 0x11u;
    kernel_log(passed ? "MiniOS: read-only PCI enumeration check passed.\n" :
                        "MiniOS: PCI enumeration check failed.\n");
    __asm__ volatile ("outl %0, %1" : : "a"(code), "Nd"(0xF4u));
    for (;;) __asm__ volatile ("cli; hlt");
}
