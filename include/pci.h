#ifndef MINI_OS_PCI_H
#define MINI_OS_PCI_H

#include <stdint.h>

#define PCI_DEVICE_LIMIT 32u

struct pci_device {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t programming_interface;
    uint8_t header_type;
    uint16_t vendor_id;
    uint16_t device_id;
};

/* Read PCI configuration space through mechanism #1; this never writes devices. */
void pci_enumerate(void);
uint32_t pci_device_count(void);
const struct pci_device *pci_device_at(uint32_t index);
int pci_self_check(void);
void pci_test_exit_qemu(int passed);

#endif
