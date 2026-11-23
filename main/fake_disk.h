// A tiny read-only FAT12 volume generated on the fly (nothing is stored).
#pragma once

#include <stdint.h>

#define FAKE_DISK_BLOCK_SIZE  512
#define FAKE_DISK_BLOCK_COUNT 16 // 8 KB volume

// Volume layout (one sector each). The report reads these to say what a host's
// sector reads mean: reading only the boot sector identifies the disk, while
// reading the root directory and the file sector shows the host inspected the
// volume's contents.
#define FAKE_DISK_LBA_BOOT    0 // boot sector / BPB
#define FAKE_DISK_LBA_FAT     1 // file allocation table
#define FAKE_DISK_LBA_ROOTDIR 2 // root directory (file listing)
#define FAKE_DISK_LBA_FILE    3 // README.TXT contents
#define FAKE_DISK_LBA_EICAR   4 // EICAR.COM contents (antivirus test file)

// Fill `block` (FAKE_DISK_BLOCK_SIZE bytes) with the contents of sector `lba`.
void fake_disk_read_block(uint32_t lba, uint8_t *block);
