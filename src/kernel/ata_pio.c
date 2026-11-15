#include "ata_pio.h"

#include "block.h"

#include <stddef.h>

#define ATA_DATA 0x1F0u
#define ATA_ERROR 0x1F1u
#define ATA_SECTOR_COUNT 0x1F2u
#define ATA_LBA_LOW 0x1F3u
#define ATA_LBA_MID 0x1F4u
#define ATA_LBA_HIGH 0x1F5u
#define ATA_DRIVE 0x1F6u
#define ATA_STATUS 0x1F7u
#define ATA_COMMAND 0x1F7u
#define ATA_ALT_STATUS 0x3F6u

#define ATA_STATUS_ERROR 0x01u
#define ATA_STATUS_DEVICE_FAULT 0x20u
#define ATA_STATUS_DRQ 0x08u
#define ATA_STATUS_BUSY 0x80u
#define ATA_MAX_LBA28_SECTORS 0x10000000u
#define ATA_POLL_LIMIT 1000000u
#define ATA_CMD_READ_SECTORS 0x20u
#define ATA_CMD_WRITE_SECTORS 0x30u
#define ATA_CMD_FLUSH_CACHE 0xE7u

static uint16_t identify_words[256];
static uint32_t drive_sector_count;
static uint8_t drive_ready;

static int ata_read_sector(uint32_t lba, uint8_t sector[BLOCK_SECTOR_SIZE]);
static int ata_write_sector(uint32_t lba,
                            const uint8_t sector[BLOCK_SECTOR_SIZE]);

static int ata_read_blocks(const struct block_device *device,
                           uint32_t first_sector, uint32_t sector_count,
                           uint8_t *buffer) {
    uint32_t index;
    if (device == 0 || device->driver_data != &drive_sector_count ||
        buffer == 0 || sector_count == 0u || first_sector > drive_sector_count ||
        sector_count > drive_sector_count - first_sector ||
        sector_count > 0xFFFFFFFFu / BLOCK_SECTOR_SIZE) return 0;
    for (index = 0u; index < sector_count; ++index) {
        if (!ata_read_sector(first_sector + index,
                buffer + (uint64_t)index * BLOCK_SECTOR_SIZE)) return 0;
    }
    return 1;
}

static int ata_write_blocks(const struct block_device *device,
                            uint32_t first_sector, uint32_t sector_count,
                            const uint8_t *buffer) {
    uint32_t index;
    if (device == 0 || device->driver_data != &drive_sector_count ||
        buffer == 0 || sector_count == 0u || first_sector > drive_sector_count ||
        sector_count > drive_sector_count - first_sector ||
        sector_count > 0xFFFFFFFFu / BLOCK_SECTOR_SIZE) return 0;
    for (index = 0u; index < sector_count; ++index) {
        if (!ata_write_sector(first_sector + index,
                buffer + (uint64_t)index * BLOCK_SECTOR_SIZE)) return 0;
    }
    return 1;
}

static uint8_t io_read8(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint16_t io_read16(uint16_t port) {
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void io_write16(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static void io_write8(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void ata_delay_400ns(void) {
    (void)io_read8(ATA_ALT_STATUS);
    (void)io_read8(ATA_ALT_STATUS);
    (void)io_read8(ATA_ALT_STATUS);
    (void)io_read8(ATA_ALT_STATUS);
}

static int ata_wait(uint8_t require_data) {
    uint32_t poll;
    for (poll = 0u; poll < ATA_POLL_LIMIT; ++poll) {
        uint8_t status = io_read8(ATA_STATUS);
        if (status == 0u || status == 0xFFu) return 0;
        if ((status & (ATA_STATUS_ERROR | ATA_STATUS_DEVICE_FAULT)) != 0u)
            return 0;
        if ((status & ATA_STATUS_BUSY) == 0u &&
            (require_data == 0u || (status & ATA_STATUS_DRQ) != 0u)) return 1;
    }
    return 0;
}

int ata_pio_init(void) {
    uint8_t status;
    uint32_t poll;
    uint32_t sectors;

    struct block_device device = {
        .name = "ata0",
        .sector_size = BLOCK_SECTOR_SIZE,
        .sector_count = 0u,
        .flags = BLOCK_DEVICE_WRITABLE,
        .driver_data = &drive_sector_count,
        .read = ata_read_blocks,
        .write = ata_write_blocks
    };

    if (drive_ready != 0u) return block_device_find("ata0") != 0;
    drive_ready = 0u;
    drive_sector_count = 0u;
    io_write8(ATA_DRIVE, 0xA0u); /* Primary bus, master, ATA identify. */
    ata_delay_400ns();
    io_write8(ATA_SECTOR_COUNT, 0u);
    io_write8(ATA_LBA_LOW, 0u);
    io_write8(ATA_LBA_MID, 0u);
    io_write8(ATA_LBA_HIGH, 0u);
    io_write8(ATA_COMMAND, 0xECu); /* IDENTIFY DEVICE */
    status = io_read8(ATA_STATUS);
    if (status == 0u || status == 0xFFu) return 0;

    for (poll = 0u; poll < ATA_POLL_LIMIT; ++poll) {
        status = io_read8(ATA_STATUS);
        if (status == 0u || status == 0xFFu ||
            (status & (ATA_STATUS_ERROR | ATA_STATUS_DEVICE_FAULT)) != 0u)
            return 0;
        if ((status & ATA_STATUS_BUSY) == 0u) {
            if (io_read8(ATA_LBA_MID) != 0u || io_read8(ATA_LBA_HIGH) != 0u)
                return 0; /* ATAPI/non-ATA device. */
            if ((status & ATA_STATUS_DRQ) != 0u) break;
        }
    }
    if (poll == ATA_POLL_LIMIT) return 0;

    for (poll = 0u; poll < 256u; ++poll)
        identify_words[poll] = io_read16(ATA_DATA);
    sectors = (uint32_t)identify_words[60] |
              ((uint32_t)identify_words[61] << 16);
    if (sectors == 0u) return 0;
    if (sectors > ATA_MAX_LBA28_SECTORS) sectors = ATA_MAX_LBA28_SECTORS;
    drive_sector_count = sectors;
    device.sector_count = sectors;
    if (!block_device_register(&device)) {
        drive_sector_count = 0u;
        return 0;
    }
    drive_ready = 1u;
    return 1;
}

static int ata_read_sector(uint32_t lba, uint8_t sector[BLOCK_SECTOR_SIZE]) {
    uint32_t word;

    if (drive_ready == 0u || sector == 0 || lba >= drive_sector_count ||
        lba >= ATA_MAX_LBA28_SECTORS || !ata_wait(0u)) return 0;
    io_write8(ATA_DRIVE, (uint8_t)(0xE0u | ((lba >> 24) & 0x0Fu)));
    ata_delay_400ns();
    io_write8(ATA_SECTOR_COUNT, 1u);
    io_write8(ATA_LBA_LOW, (uint8_t)lba);
    io_write8(ATA_LBA_MID, (uint8_t)(lba >> 8));
    io_write8(ATA_LBA_HIGH, (uint8_t)(lba >> 16));
    io_write8(ATA_COMMAND, ATA_CMD_READ_SECTORS);
    if (!ata_wait(1u)) return 0;

    for (word = 0u; word < BLOCK_SECTOR_SIZE / sizeof(uint16_t); ++word) {
        uint16_t value = io_read16(ATA_DATA);
        sector[word * 2u] = (uint8_t)value;
        sector[word * 2u + 1u] = (uint8_t)(value >> 8);
    }
    return ata_wait(0u);
}

static int ata_write_sector(uint32_t lba,
                            const uint8_t sector[BLOCK_SECTOR_SIZE]) {
    uint32_t word;

    if (drive_ready == 0u || sector == 0 || lba >= drive_sector_count ||
        lba >= ATA_MAX_LBA28_SECTORS || !ata_wait(0u)) return 0;
    io_write8(ATA_DRIVE, (uint8_t)(0xE0u | ((lba >> 24) & 0x0Fu)));
    ata_delay_400ns();
    io_write8(ATA_SECTOR_COUNT, 1u);
    io_write8(ATA_LBA_LOW, (uint8_t)lba);
    io_write8(ATA_LBA_MID, (uint8_t)(lba >> 8));
    io_write8(ATA_LBA_HIGH, (uint8_t)(lba >> 16));
    io_write8(ATA_COMMAND, ATA_CMD_WRITE_SECTORS);
    if (!ata_wait(1u)) return 0;

    for (word = 0u; word < BLOCK_SECTOR_SIZE / sizeof(uint16_t); ++word) {
        uint16_t value = (uint16_t)sector[word * 2u] |
                         ((uint16_t)sector[word * 2u + 1u] << 8);
        io_write16(ATA_DATA, value);
    }
    if (!ata_wait(0u)) return 0;

    /* Force completed sectors out of the drive's volatile write cache. */
    io_write8(ATA_COMMAND, ATA_CMD_FLUSH_CACHE);
    ata_delay_400ns();
    return ata_wait(0u);
}
