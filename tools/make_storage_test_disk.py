#!/usr/bin/env python3
"""Create a tiny deterministic FAT16 disk image for QEMU-only tests."""

import struct
import sys
from pathlib import Path


SECTOR_SIZE = 512
TOTAL_SECTORS = 32768
PARTITION_START = 2048
PARTITION_SECTORS = 30000
RESERVED_SECTORS = 1
FAT_COUNT = 2
SECTORS_PER_FAT = 128
ROOT_ENTRIES = 512
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECTOR_SIZE
PAYLOAD = bytes((index * 37 + 11) & 0xFF for index in range(700))


def main() -> None:
    valid_modes = ("--superfloppy", "--invalid-partition")
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] not in valid_modes):
        raise SystemExit("usage: make_storage_test_disk.py OUTPUT.img [--superfloppy|--invalid-partition]")
    image = bytearray(TOTAL_SECTORS * SECTOR_SIZE)
    partitioned = len(sys.argv) == 2
    invalid_partition = len(sys.argv) == 3 and sys.argv[2] == "--invalid-partition"
    volume_start = PARTITION_START if partitioned else 0
    volume_sectors = PARTITION_SECTORS if partitioned else TOTAL_SECTORS
    if partitioned or invalid_partition:
        image[446:462] = struct.pack(
            "<B3sB3sII", 0, b"\x00\x02\x00", 0x06, b"\xFE\xFF\xFF",
            PARTITION_START,
            TOTAL_SECTORS if invalid_partition else PARTITION_SECTORS,
        )
        image[510:512] = b"\x55\xAA"

    if invalid_partition:
        output = Path(sys.argv[1])
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(image)
        return

    boot_start = volume_start * SECTOR_SIZE
    boot = memoryview(image)[boot_start:boot_start + SECTOR_SIZE]
    boot[0:3] = b"\xEB\x3C\x90"
    boot[3:11] = b"MINIOS  "
    struct.pack_into("<H", boot, 11, SECTOR_SIZE)
    boot[13] = 1
    struct.pack_into("<H", boot, 14, RESERVED_SECTORS)
    boot[16] = FAT_COUNT
    struct.pack_into("<H", boot, 17, ROOT_ENTRIES)
    struct.pack_into("<H", boot, 19, volume_sectors)
    boot[21] = 0xF8
    struct.pack_into("<H", boot, 22, SECTORS_PER_FAT)
    struct.pack_into("<H", boot, 24, 32)
    struct.pack_into("<H", boot, 26, 64)
    struct.pack_into("<I", boot, 28, 0)
    struct.pack_into("<I", boot, 32, 0)
    boot[36] = 0x80
    boot[38] = 0x29
    struct.pack_into("<I", boot, 39, 0x4D4F5349)
    boot[43:54] = b"MINIOS TEST"
    boot[54:62] = b"FAT16   "
    boot[510:512] = b"\x55\xAA"

    for fat_index in range(FAT_COUNT):
        fat_start = (volume_start + RESERVED_SECTORS +
                     fat_index * SECTORS_PER_FAT) * SECTOR_SIZE
        struct.pack_into("<HHHH", image, fat_start, 0xFFF8, 0xFFFF, 3, 0xFFFF)

    root_start = (volume_start + RESERVED_SECTORS +
                  FAT_COUNT * SECTORS_PER_FAT) * SECTOR_SIZE
    image[root_start:root_start + 11] = b"MINIOS TEST"
    image[root_start + 11] = 0x08
    verify_entry = root_start + 32
    image[verify_entry:verify_entry + 11] = b"VERIFY  TXT"
    image[verify_entry + 11] = 0x20
    struct.pack_into("<H", image, verify_entry + 26, 2)
    struct.pack_into("<I", image, verify_entry + 28, len(PAYLOAD))

    data_start = (volume_start + RESERVED_SECTORS + FAT_COUNT * SECTORS_PER_FAT +
                  ROOT_SECTORS) * SECTOR_SIZE
    image[data_start:data_start + len(PAYLOAD)] = PAYLOAD
    output = Path(sys.argv[1])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(image)


if __name__ == "__main__":
    main()
