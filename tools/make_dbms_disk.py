#!/usr/bin/env python3
"""Create MiniOS's persistent FAT16 DBMS disk without replacing user data."""

import sys


SECTOR_SIZE = 512
TOTAL_SECTORS = 16_384  # 8 MiB
SECTORS_PER_FAT = 64
ROOT_ENTRIES = 512


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} OUTPUT_IMAGE", file=sys.stderr)
        return 2

    output_path = sys.argv[1]
    try:
        image = open(output_path, "xb")
    except FileExistsError:
        print(f"Keeping existing database disk: {output_path}")
        return 0

    try:
        image.truncate(TOTAL_SECTORS * SECTOR_SIZE)
        boot = bytearray(SECTOR_SIZE)
        boot[0:3] = b"\xEB\x3C\x90"
        boot[3:11] = b"MINIOS  "
        boot[11:13] = SECTOR_SIZE.to_bytes(2, "little")
        boot[13] = 1
        boot[14:16] = (1).to_bytes(2, "little")
        boot[16] = 2
        boot[17:19] = ROOT_ENTRIES.to_bytes(2, "little")
        boot[19:21] = TOTAL_SECTORS.to_bytes(2, "little")
        boot[21] = 0xF8
        boot[22:24] = SECTORS_PER_FAT.to_bytes(2, "little")
        boot[24:26] = (32).to_bytes(2, "little")
        boot[26:28] = (64).to_bytes(2, "little")
        boot[36] = 0x80
        boot[38] = 0x29
        boot[39:43] = b"MNS1"
        boot[43:54] = b"MINIOS DBMS"
        boot[54:62] = b"FAT16   "
        boot[510:512] = b"\x55\xAA"
        image.seek(0)
        image.write(boot)

        fat = bytearray(SECTOR_SIZE)
        fat[0:2] = b"\xF8\xFF"
        fat[2:4] = b"\xFF\xFF"
        for fat_lba in (1, 1 + SECTORS_PER_FAT):
            image.seek(fat_lba * SECTOR_SIZE)
            image.write(fat)
    finally:
        image.close()

    print(f"Created {TOTAL_SECTORS * SECTOR_SIZE} byte FAT16 database disk: {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
