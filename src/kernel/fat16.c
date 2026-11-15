#include "fat16.h"

#include "ata_pio.h"
#include "block.h"
#include "interrupts.h"

#include <stddef.h>

#define FAT16_MIN_CLUSTERS 4085u
#define FAT16_MAX_CLUSTERS 65524u
#define FAT16_EOC_MIN 0xFFF8u
#define FAT16_RESERVED_MIN 0xFFF0u
#define FAT16_BAD_CLUSTER 0xFFF7u
#define FAT16_ATTR_DIRECTORY 0x10u
#define FAT16_ATTR_VOLUME_ID 0x08u
#define FAT16_ATTR_LONG_NAME 0x0Fu
#define FAT16_ATTR_ARCHIVE 0x20u
#define FAT16_WRITE_CLUSTER_LIMIT 4096u
#define FAT16_EOC 0xFFFFu

struct fat16_volume {
    uint32_t volume_start;
    uint32_t volume_end;
    uint32_t total_sectors;
    uint32_t fat_start;
    uint32_t root_start;
    uint32_t root_sectors;
    uint32_t data_start;
    uint32_t cluster_count;
    uint16_t root_entries;
    uint16_t sectors_per_fat;
    uint8_t sectors_per_cluster;
    uint8_t fat_count;
    uint8_t mounted;
};

static struct fat16_volume volume;
static uint8_t fat16_sector[BLOCK_SECTOR_SIZE];
static uint8_t fat16_write_sector[BLOCK_SECTOR_SIZE];
static uint16_t fat16_write_clusters[FAT16_WRITE_CLUSTER_LIMIT];
static const struct block_device *active_device;

static uint16_t read_le16(const uint8_t *bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static void write_le16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static uint32_t read_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void copy_bytes(uint8_t *destination, const uint8_t *source,
                       uint32_t length) {
    for (uint32_t index = 0u; index < length; ++index)
        destination[index] = source[index];
}

static int fat16_mount_volume(uint32_t start_lba, uint32_t available_sectors) {
    uint8_t boot_sector[BLOCK_SECTOR_SIZE];
    uint32_t total_sectors;
    uint32_t root_sectors;
    uint32_t metadata_sectors;
    uint32_t data_sectors;
    uint32_t cluster_count;
    uint16_t bytes_per_sector;
    uint16_t reserved_sectors;
    uint16_t sectors_per_fat;
    uint16_t root_entries;
    uint8_t sectors_per_cluster;
    uint8_t fat_count;

    uint64_t volume_end;
    if (active_device == 0 || start_lba >= active_device->sector_count ||
        available_sectors == 0u ||
        (uint64_t)start_lba + available_sectors > active_device->sector_count ||
        !block_device_read(active_device, start_lba, 1u, boot_sector) ||
        boot_sector[510] != 0x55u || boot_sector[511] != 0xAAu) return 0;

    bytes_per_sector = read_le16(&boot_sector[11]);
    sectors_per_cluster = boot_sector[13];
    reserved_sectors = read_le16(&boot_sector[14]);
    fat_count = boot_sector[16];
    root_entries = read_le16(&boot_sector[17]);
    total_sectors = read_le16(&boot_sector[19]);
    if (total_sectors == 0u) total_sectors = read_le32(&boot_sector[32]);
    sectors_per_fat = read_le16(&boot_sector[22]);

    if (bytes_per_sector != BLOCK_SECTOR_SIZE ||
        sectors_per_cluster == 0u || sectors_per_cluster > 128u ||
        (sectors_per_cluster & (sectors_per_cluster - 1u)) != 0u ||
        reserved_sectors == 0u || fat_count == 0u || fat_count > 4u ||
        root_entries == 0u || sectors_per_fat == 0u ||
        total_sectors == 0u || total_sectors > available_sectors) return 0;

    root_sectors = ((uint32_t)root_entries * 32u + BLOCK_SECTOR_SIZE - 1u) /
                   BLOCK_SECTOR_SIZE;
    metadata_sectors = (uint32_t)reserved_sectors +
                       (uint32_t)fat_count * sectors_per_fat + root_sectors;
    if (metadata_sectors >= total_sectors ||
        (uint32_t)sectors_per_fat * BLOCK_SECTOR_SIZE / 2u <
            FAT16_MIN_CLUSTERS) return 0;
    data_sectors = total_sectors - metadata_sectors;
    cluster_count = data_sectors / sectors_per_cluster;
    if (cluster_count < FAT16_MIN_CLUSTERS ||
        cluster_count > FAT16_MAX_CLUSTERS ||
        (uint32_t)sectors_per_fat * BLOCK_SECTOR_SIZE / 2u < cluster_count + 2u)
        return 0;

    volume_end = (uint64_t)start_lba + total_sectors;
    if (volume_end > active_device->sector_count || volume_end > 0xFFFFFFFFu)
        return 0;

    volume.volume_start = start_lba;
    volume.volume_end = (uint32_t)volume_end;
    volume.total_sectors = total_sectors;
    volume.fat_start = start_lba + reserved_sectors;
    volume.root_start = start_lba + (uint32_t)reserved_sectors +
                        (uint32_t)fat_count * sectors_per_fat;
    volume.root_sectors = root_sectors;
    volume.data_start = volume.root_start + root_sectors;
    volume.cluster_count = cluster_count;
    volume.root_entries = root_entries;
    volume.sectors_per_fat = sectors_per_fat;
    volume.sectors_per_cluster = sectors_per_cluster;
    volume.fat_count = fat_count;
    volume.mounted = 1u;
    return 1;
}

static int fat16_partition_type(uint8_t type) {
    return type == 0x04u || type == 0x06u || type == 0x0Eu;
}

int fat16_mount_device(const struct block_device *device) {
    uint8_t mbr[BLOCK_SECTOR_SIZE];
    uint32_t disk_sectors;

    volume.mounted = 0u;
    active_device = 0;
    if (device == 0 || device->sector_size != BLOCK_SECTOR_SIZE ||
        device->sector_count == 0u ||
        (device->flags != BLOCK_DEVICE_READ_ONLY &&
         device->flags != BLOCK_DEVICE_WRITABLE)) return 0;
    active_device = device;
    disk_sectors = device->sector_count;
    /* Support FAT16 superfloppy images as well as primary MBR partitions. */
    if (fat16_mount_volume(0u, disk_sectors)) return 1;
    if (!block_device_read(active_device, 0u, 1u, mbr) ||
        mbr[510] != 0x55u || mbr[511] != 0xAAu)
        return 0;

    for (uint32_t index = 0u; index < 4u; ++index) {
        const uint8_t *entry = &mbr[446u + index * 16u];
        uint8_t boot_flag = entry[0];
        uint8_t type = entry[4];
        uint32_t start_lba = read_le32(&entry[8]);
        uint32_t sector_count = read_le32(&entry[12]);
        uint64_t partition_end = (uint64_t)start_lba + sector_count;

        if ((boot_flag != 0u && boot_flag != 0x80u) ||
            !fat16_partition_type(type) || start_lba == 0u ||
            sector_count == 0u || partition_end > disk_sectors) continue;
        if (fat16_mount_volume(start_lba, sector_count)) return 1;
    }
    return 0;
}

int fat16_mount(void) {
    const struct block_device *device;
    volume.mounted = 0u;
    active_device = 0;
    if (block_device_count() == 0u && !ata_pio_init()) return 0;
    device = block_device_at(0u);
    return fat16_mount_device(device);
}

static int fat16_find_root_entry(const char short_name[11], uint16_t *cluster_out,
                                 uint32_t *size_out) {
    uint32_t entries_per_sector = BLOCK_SECTOR_SIZE / 32u;
    for (uint32_t sector_index = 0u; sector_index < volume.root_sectors;
         ++sector_index) {
        if (!block_device_read(active_device, volume.root_start + sector_index,
                               1u, fat16_sector))
            return 0;
        uint32_t entries_left = (uint32_t)volume.root_entries -
                                sector_index * entries_per_sector;
        if (entries_left > entries_per_sector) entries_left = entries_per_sector;
        for (uint32_t entry_index = 0u; entry_index < entries_left;
             ++entry_index) {
            const uint8_t *entry = &fat16_sector[entry_index * 32u];
            uint8_t attributes;
            if (entry[0] == 0u) return 0;
            if (entry[0] == 0xE5u) continue;
            attributes = entry[11];
            if (attributes == FAT16_ATTR_LONG_NAME ||
                (attributes & (FAT16_ATTR_DIRECTORY | FAT16_ATTR_VOLUME_ID)) != 0u)
                continue;
            {
                uint32_t character;
                for (character = 0u; character < 11u; ++character)
                    if (entry[character] != (uint8_t)short_name[character]) break;
                if (character != 11u) continue;
            }
            *cluster_out = read_le16(&entry[26]);
            *size_out = read_le32(&entry[28]);
            return 1;
        }
    }
    return 0;
}

static int fat16_next_cluster(uint16_t cluster, uint16_t *next_out) {
    uint32_t byte_offset = (uint32_t)cluster * 2u;
    uint32_t fat_sector = byte_offset / BLOCK_SECTOR_SIZE;
    uint32_t entry_offset = byte_offset % BLOCK_SECTOR_SIZE;
    if (fat_sector >= volume.sectors_per_fat || next_out == 0 ||
        !block_device_read(active_device, volume.fat_start + fat_sector, 1u,
                           fat16_sector)) return 0;
    *next_out = read_le16(&fat16_sector[entry_offset]);
    return 1;
}

static int fat16_get_cluster_entry(uint16_t cluster, uint16_t *value_out) {
    uint32_t byte_offset = (uint32_t)cluster * 2u;
    uint32_t fat_sector = byte_offset / BLOCK_SECTOR_SIZE;
    uint32_t entry_offset = byte_offset % BLOCK_SECTOR_SIZE;
    if (fat_sector >= volume.sectors_per_fat || value_out == 0 ||
        !block_device_read(active_device, volume.fat_start + fat_sector, 1u,
                           fat16_sector)) return 0;
    *value_out = read_le16(&fat16_sector[entry_offset]);
    return 1;
}

static int fat16_set_cluster_entry(uint16_t cluster, uint16_t value) {
    uint32_t byte_offset = (uint32_t)cluster * 2u;
    uint32_t fat_sector = byte_offset / BLOCK_SECTOR_SIZE;
    uint32_t entry_offset = byte_offset % BLOCK_SECTOR_SIZE;
    if (fat_sector >= volume.sectors_per_fat || active_device == 0 ||
        active_device->flags != BLOCK_DEVICE_WRITABLE) return 0;
    for (uint32_t copy = 0u; copy < volume.fat_count; ++copy) {
        uint32_t lba = volume.fat_start + copy * volume.sectors_per_fat + fat_sector;
        if (!block_device_read(active_device, lba, 1u, fat16_sector)) return 0;
        write_le16(&fat16_sector[entry_offset], value);
        if (!block_device_write(active_device, lba, 1u, fat16_sector)) return 0;
    }
    return 1;
}

static int fat16_valid_short_name(const char name[11]) {
    uint32_t index;
    int base_padding = 0;
    int extension_padding = 0;
    if (name == 0 || name[0] == ' ') return 0;
    for (index = 0u; index < 11u; ++index) {
        char ch = name[index];
        if (ch == ' ') {
            if (index < 8u) base_padding = 1;
            else extension_padding = 1;
            continue;
        }
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '-')) return 0;
        if (index < 8u && base_padding) return 0;
        if (index >= 8u && extension_padding) return 0;
    }
    return 1;
}

static int fat16_find_root_slot(const char name[11], uint32_t *lba_out,
                                uint32_t *offset_out, uint8_t old_entry[32],
                                int *exists_out) {
    uint32_t entries_per_sector = BLOCK_SECTOR_SIZE / 32u;
    uint32_t free_lba = 0u;
    uint32_t free_offset = 0u;
    int have_free = 0;
    if (lba_out == 0 || offset_out == 0 || old_entry == 0 || exists_out == 0)
        return 0;
    *exists_out = 0;
    for (uint32_t sector_index = 0u; sector_index < volume.root_sectors;
         ++sector_index) {
        if (!block_device_read(active_device, volume.root_start + sector_index,
                               1u, fat16_sector)) return 0;
        uint32_t entries_left = (uint32_t)volume.root_entries -
                                sector_index * entries_per_sector;
        if (entries_left > entries_per_sector) entries_left = entries_per_sector;
        for (uint32_t entry_index = 0u; entry_index < entries_left; ++entry_index) {
            uint32_t offset = entry_index * 32u;
            const uint8_t *entry = &fat16_sector[offset];
            int match = 1;
            if (entry[0] == 0u || entry[0] == 0xE5u) {
                if (!have_free) {
                    free_lba = volume.root_start + sector_index;
                    free_offset = offset;
                    have_free = 1;
                }
                if (entry[0] == 0u) goto slot_search_done;
                continue;
            }
            if (entry[11] == FAT16_ATTR_LONG_NAME ||
                (entry[11] & (FAT16_ATTR_DIRECTORY | FAT16_ATTR_VOLUME_ID)) != 0u)
                continue;
            for (uint32_t character = 0u; character < 11u; ++character) {
                if (entry[character] != (uint8_t)name[character]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                copy_bytes(old_entry, entry, 32u);
                *lba_out = volume.root_start + sector_index;
                *offset_out = offset;
                *exists_out = 1;
                return 1;
            }
        }
    }
slot_search_done:
    if (!have_free) return 0;
    if (!block_device_read(active_device, free_lba, 1u, fat16_sector)) return 0;
    copy_bytes(old_entry, &fat16_sector[free_offset], 32u);
    *lba_out = free_lba;
    *offset_out = free_offset;
    return 1;
}

static void fat16_release_new_clusters(uint32_t count) {
    for (uint32_t index = 0u; index < count; ++index)
        if (!fat16_set_cluster_entry(fat16_write_clusters[index], 0u)) break;
}

int fat16_write_root_file(const char short_name[11], const uint8_t *source,
                          uint32_t size) {
    uint8_t old_entry[32];
    uint32_t directory_lba;
    uint32_t directory_offset;
    uint32_t required_clusters;
    uint32_t allocated_clusters = 0u;
    uint64_t cluster_bytes;
    uint32_t remaining;
    uint32_t source_offset = 0u;
    uint16_t old_cluster = 0u;
    int exists;

    if (volume.mounted == 0u || active_device == 0 ||
        active_device->flags != BLOCK_DEVICE_WRITABLE ||
        !fat16_valid_short_name(short_name) || (size != 0u && source == 0))
        return 0;
    cluster_bytes = (uint64_t)volume.sectors_per_cluster * BLOCK_SECTOR_SIZE;
    required_clusters = size == 0u ? 0u :
        (uint32_t)(((uint64_t)size + cluster_bytes - 1u) / cluster_bytes);
    if (required_clusters > FAT16_WRITE_CLUSTER_LIMIT ||
        required_clusters > volume.cluster_count) return 0;
    if (!fat16_find_root_slot(short_name, &directory_lba, &directory_offset,
                              old_entry, &exists)) return 0;
    if (exists) old_cluster = read_le16(&old_entry[26]);

    for (uint32_t cluster = 2u; cluster < volume.cluster_count + 2u &&
         allocated_clusters < required_clusters; ++cluster) {
        uint16_t value;
        if (!fat16_get_cluster_entry((uint16_t)cluster, &value)) return 0;
        if (value == 0u) fat16_write_clusters[allocated_clusters++] = (uint16_t)cluster;
    }
    if (allocated_clusters != required_clusters) return 0;

    for (uint32_t index = 0u; index < allocated_clusters; ++index) {
        uint16_t next = index + 1u < allocated_clusters ?
            fat16_write_clusters[index + 1u] : FAT16_EOC;
        if (!fat16_set_cluster_entry(fat16_write_clusters[index], next)) {
            fat16_release_new_clusters(index + 1u);
            return 0;
        }
    }

    remaining = size;
    for (uint32_t index = 0u; index < allocated_clusters; ++index) {
        uint32_t first_sector = volume.data_start +
            ((uint32_t)fat16_write_clusters[index] - 2u) *
                volume.sectors_per_cluster;
        for (uint32_t sector = 0u; sector < volume.sectors_per_cluster; ++sector) {
            uint32_t chunk = remaining > BLOCK_SECTOR_SIZE ?
                             BLOCK_SECTOR_SIZE : remaining;
            for (uint32_t byte = 0u; byte < BLOCK_SECTOR_SIZE; ++byte)
                fat16_write_sector[byte] = 0u;
            if (chunk != 0u) {
                copy_bytes(fat16_write_sector, source + source_offset, chunk);
                source_offset += chunk;
                remaining -= chunk;
            }
            if (first_sector + sector >= volume.volume_end ||
                !block_device_write(active_device, first_sector + sector, 1u,
                                    fat16_write_sector)) {
                fat16_release_new_clusters(allocated_clusters);
                return 0;
            }
        }
    }

    if (!block_device_read(active_device, directory_lba, 1u, fat16_sector)) {
        fat16_release_new_clusters(allocated_clusters);
        return 0;
    }
    {
        uint8_t *entry = &fat16_sector[directory_offset];
        for (uint32_t index = 0u; index < 32u; ++index) entry[index] = 0u;
        copy_bytes(entry, (const uint8_t *)short_name, 11u);
        entry[11] = FAT16_ATTR_ARCHIVE;
        write_le16(&entry[26], allocated_clusters == 0u ? 0u :
                   fat16_write_clusters[0]);
        entry[28] = (uint8_t)size;
        entry[29] = (uint8_t)(size >> 8);
        entry[30] = (uint8_t)(size >> 16);
        entry[31] = (uint8_t)(size >> 24);
    }
    if (!block_device_write(active_device, directory_lba, 1u, fat16_sector)) {
        fat16_release_new_clusters(allocated_clusters);
        return 0;
    }

    /* Publish the replacement first; a failed old-chain cleanup only leaks space. */
    if (old_cluster >= 2u && old_cluster < volume.cluster_count + 2u) {
        uint16_t cluster = old_cluster;
        for (uint32_t traversed = 0u; traversed < volume.cluster_count; ++traversed) {
            uint16_t next;
            if (!fat16_get_cluster_entry(cluster, &next) ||
                !fat16_set_cluster_entry(cluster, 0u)) break;
            if (next >= FAT16_EOC_MIN || next == 0u || next == FAT16_BAD_CLUSTER ||
                next < 2u || next >= volume.cluster_count + 2u) break;
            cluster = next;
        }
    }
    return 1;
}

int fat16_read_root_file(const char short_name[11], uint8_t *destination,
                         uint32_t capacity, uint32_t *size_out) {
    uint16_t cluster;
    uint32_t file_size;
    uint32_t remaining;
    uint32_t copied = 0u;
    uint32_t traversed = 0u;

    if (volume.mounted == 0u || short_name == 0 || size_out == 0) return 0;
    if (!fat16_find_root_entry(short_name, &cluster, &file_size) ||
        file_size > capacity || (file_size != 0u && destination == 0)) return 0;
    *size_out = file_size;
    remaining = file_size;
    while (remaining != 0u) {
        uint32_t first_sector;
        if (cluster < 2u || cluster >= volume.cluster_count + 2u ||
            traversed++ >= volume.cluster_count) return 0;
        first_sector = volume.data_start +
            ((uint32_t)cluster - 2u) * volume.sectors_per_cluster;
        for (uint32_t index = 0u;
             index < volume.sectors_per_cluster && remaining != 0u; ++index) {
            uint32_t chunk = remaining > BLOCK_SECTOR_SIZE ?
                             BLOCK_SECTOR_SIZE : remaining;
            if (first_sector + index >= volume.volume_end ||
                !block_device_read(active_device, first_sector + index, 1u,
                                   fat16_sector)) return 0;
            copy_bytes(destination + copied, fat16_sector, chunk);
            copied += chunk;
            remaining -= chunk;
        }
        if (remaining != 0u) {
            uint16_t next;
            if (!fat16_next_cluster(cluster, &next) || next == FAT16_BAD_CLUSTER ||
                next >= FAT16_RESERVED_MIN || next < 2u) return 0;
            cluster = next;
        }
    }
    return 1;
}

int storage_self_check(void) {
    static const char filename[11] = {'V','E','R','I','F','Y',' ',' ','T','X','T'};
    uint8_t file_contents[1024];
    uint8_t sector_contents[BLOCK_SECTOR_SIZE * 2u];
    uint32_t file_size = 0u;

    if (!fat16_mount()) {
        kernel_log("MiniOS: ATA/FAT16 mount failed.\n");
        return 0;
    }
    if (!fat16_read_root_file(filename, file_contents, sizeof(file_contents),
                              &file_size) ||
        file_size != 700u) {
        kernel_log("MiniOS: FAT16 test file lookup failed.\n");
        return 0;
    }
    for (uint32_t index = 0u; index < file_size; ++index) {
        if (file_contents[index] != (uint8_t)(index * 37u + 11u)) {
            kernel_log("MiniOS: FAT16 test file contents differ.\n");
            return 0;
        }
    }
    if (!block_device_read(active_device, volume.data_start, 2u,
                           sector_contents)) {
        kernel_log("MiniOS: multi-sector block read failed.\n");
        return 0;
    }
    for (uint32_t index = 0u; index < file_size; ++index) {
        if (sector_contents[index] != (uint8_t)(index * 37u + 11u)) {
            kernel_log("MiniOS: multi-sector block contents differ.\n");
            return 0;
        }
    }
    if (fat16_read_root_file(filename, file_contents, file_size - 1u,
                             &file_size) ||
        fat16_read_root_file("MISSING TXT", file_contents, sizeof(file_contents),
                             &file_size) ||
        block_device_read(active_device, active_device->sector_count, 1u,
                          fat16_sector) ||
        block_device_read(active_device, active_device->sector_count - 1u, 2u,
                          sector_contents)) {
        kernel_log("MiniOS: storage bounds check failed.\n");
        return 0;
    }
    kernel_log("MiniOS: QEMU ATA PIO and FAT16 read-only checks passed.\n");
    return 1;
}

int storage_reject_invalid_partition_check(void) {
    if (block_device_count() == 0u && !ata_pio_init()) {
        kernel_log("MiniOS: ATA IDENTIFY failed in partition rejection check.\n");
        return 0;
    }
    if (fat16_mount()) {
        kernel_log("MiniOS: accepted an out-of-range MBR partition.\n");
        return 0;
    }
    kernel_log("MiniOS: out-of-range MBR partition was rejected.\n");
    return 1;
}
