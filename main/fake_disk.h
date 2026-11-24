// A tiny read-only FAT12 volume generated on the fly (nothing is stored).
#pragma once

#include <stdbool.h>
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

// Generate a fresh volume in RAM (boot sector, FAT, directory, README and,
// when EICAR_ENABLED, EICAR.COM). Call once at power-up; the volume is never
// persisted, so every boot starts clean with EICAR present again.
void fake_disk_init(void);

// Fill `block` (FAKE_DISK_BLOCK_SIZE bytes) with the current contents of
// sector `lba` (reflecting any writes the host has made this session).
void fake_disk_read_block(uint32_t lba, uint8_t *block);

// Apply a host write of one sector to the RAM volume (data is FAKE_DISK_BLOCK_
// SIZE bytes). Nothing is written to flash.
void fake_disk_write_block(uint32_t lba, const uint8_t *data);

// True while the EICAR.COM directory entry is still present. Goes false once a
// host antivirus deletes/quarantines it. Always false when EICAR_ENABLED is 0.
bool fake_disk_eicar_present(void);
