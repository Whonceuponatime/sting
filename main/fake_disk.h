// A tiny read-only FAT12 volume generated on the fly (nothing is stored).
#pragma once

#include <stdint.h>

#define FAKE_DISK_BLOCK_SIZE  512
#define FAKE_DISK_BLOCK_COUNT 16 // 8 KB volume

// Fill `block` (FAKE_DISK_BLOCK_SIZE bytes) with the contents of sector `lba`.
void fake_disk_read_block(uint32_t lba, uint8_t *block);
