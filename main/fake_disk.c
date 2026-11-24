// Fake drive: a small read-only FAT12 volume.
//
// Layout (1 sector per cluster, 512-byte sectors):
//   sector 0     boot sector / BPB
//   sector 1     FAT
//   sector 2     root directory (16 entries)
//   sector 3     README.TXT  (cluster 2)
//   sector 4     EICAR.COM   (cluster 3)  -- only when EICAR_ENABLED
//   sector 5-15  empty
//
// The README holds no executable content. The optional EICAR.COM holds the
// standard, harmless EICAR antivirus test string (see config.h and below).

#include "fake_disk.h"

#include <string.h>

#include "config.h"

static const char README[] =
    "STING USB port liveness tester\r\n"
    "\r\n"
    "This is a read-only test volume presented by a port test tool.\r\n"
    "It holds no data and no executable content.\r\n";

#define README_SIZE (sizeof(README) - 1)
_Static_assert(sizeof(README) - 1 <= FAKE_DISK_BLOCK_SIZE, "README must fit in one sector");

#if EICAR_ENABLED
// The EICAR anti-malware test file, stored XOR'd with EICAR_KEY so the literal
// 68-byte signature never appears in this source or in the firmware image (an
// antivirus on the BUILD machine would otherwise quarantine them). It is
// reconstructed into the sector buffer only when the host reads it, so the
// system under test sees the genuine, detectable file. EICAR is not malware:
// it is an inert pattern published by EICAR purely to test antivirus software.
#define EICAR_KEY  0x5a
#define EICAR_SIZE 68
static const uint8_t EICAR_X[EICAR_SIZE] = {
    0x02, 0x6f, 0x15, 0x7b, 0x0a, 0x7f, 0x1a, 0x1b, 0x0a, 0x01, 0x6e, 0x06,
    0x0a, 0x00, 0x02, 0x6f, 0x6e, 0x72, 0x0a, 0x04, 0x73, 0x6d, 0x19, 0x19,
    0x73, 0x6d, 0x27, 0x7e, 0x1f, 0x13, 0x19, 0x1b, 0x08, 0x77, 0x09, 0x0e,
    0x1b, 0x14, 0x1e, 0x1b, 0x08, 0x1e, 0x77, 0x1b, 0x14, 0x0e, 0x13, 0x0c,
    0x13, 0x08, 0x0f, 0x09, 0x77, 0x0e, 0x1f, 0x09, 0x0e, 0x77, 0x1c, 0x13,
    0x16, 0x1f, 0x7b, 0x7e, 0x12, 0x71, 0x12, 0x70,
};
_Static_assert(EICAR_SIZE <= FAKE_DISK_BLOCK_SIZE, "EICAR must fit in one sector");
#endif

#define VOLUME_LABEL "STING TEST " // exactly 11 characters

// FAT date for 2026-01-01
#define FAT_DATE (((2026 - 1980) << 9) | (1 << 5) | 1)

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static void boot_sector(uint8_t *b)
{
    b[0] = 0xEB; // jump
    b[1] = 0x3C;
    b[2] = 0x90;
    memcpy(b + 3, "MSDOS5.0", 8);          // OEM name
    put16(b + 11, FAKE_DISK_BLOCK_SIZE);   // bytes per sector
    b[13] = 1;                             // sectors per cluster
    put16(b + 14, 1);                      // reserved sectors
    b[16] = 1;                             // number of FATs
    put16(b + 17, 16);                     // root directory entries
    put16(b + 19, FAKE_DISK_BLOCK_COUNT);  // total sectors
    b[21] = 0xF8;                          // media descriptor
    put16(b + 22, 1);                      // sectors per FAT
    put16(b + 24, 1);                      // sectors per track
    put16(b + 26, 1);                      // heads
    put32(b + 28, 0);                      // hidden sectors
    put32(b + 32, 0);                      // total sectors (32-bit, unused)
    b[36] = 0x80;                          // drive number
    b[38] = 0x29;                          // extended boot signature
    put32(b + 39, 0x53544E47);             // volume serial
    memcpy(b + 43, VOLUME_LABEL, 11);
    memcpy(b + 54, "FAT12   ", 8);
    b[510] = 0x55;
    b[511] = 0xAA;
}

static void fat_sector(uint8_t *b)
{
    // Entries 0 and 1 are reserved; cluster 2 (README) is end-of-chain.
    b[0] = 0xF8;
    b[1] = 0xFF;
    b[2] = 0xFF;
    b[3] = 0xFF;
    b[4] = 0x0F;
#if EICAR_ENABLED
    // Cluster 3 (EICAR.COM) end-of-chain. FAT12 packs two clusters per 3 bytes:
    // cluster 2 -> bytes 3-4 (low nibble of b[4]), cluster 3 -> bytes 4-5.
    b[4] |= 0xF0;
    b[5] = 0xFF;
#endif
}

static void dir_entry(uint8_t *e, const char *name11, uint8_t attr, uint16_t cluster, uint32_t size)
{
    memcpy(e, name11, 11);
    e[11] = attr;
    put16(e + 16, FAT_DATE); // created
    put16(e + 18, FAT_DATE); // accessed
    put16(e + 24, FAT_DATE); // modified
    put16(e + 26, cluster);
    put32(e + 28, size);
}

static void root_dir_sector(uint8_t *b)
{
    dir_entry(b, VOLUME_LABEL, 0x08, 0, 0);                    // volume label
    dir_entry(b + 32, "README  TXT", 0x21, 2, README_SIZE);    // read-only + archive
#if EICAR_ENABLED
    dir_entry(b + 64, "EICAR   COM", 0x21, 3, EICAR_SIZE);     // read-only + archive
#endif
}

#if EICAR_ENABLED
static void eicar_sector(uint8_t *b)
{
    for (int i = 0; i < EICAR_SIZE; i++) {
        b[i] = (uint8_t)(EICAR_X[i] ^ EICAR_KEY);
    }
}
#endif

// The volume lives in RAM so the host can write to it (e.g. antivirus deleting
// EICAR). It is never persisted; fake_disk_init() regenerates it every boot.
static uint8_t s_img[FAKE_DISK_BLOCK_COUNT][FAKE_DISK_BLOCK_SIZE];
static bool s_ready;

void fake_disk_init(void)
{
    memset(s_img, 0, sizeof(s_img));
    boot_sector(s_img[FAKE_DISK_LBA_BOOT]);
    fat_sector(s_img[FAKE_DISK_LBA_FAT]);
    root_dir_sector(s_img[FAKE_DISK_LBA_ROOTDIR]);
    memcpy(s_img[FAKE_DISK_LBA_FILE], README, README_SIZE);
#if EICAR_ENABLED
    eicar_sector(s_img[FAKE_DISK_LBA_EICAR]);
#endif
    s_ready = true;
}

void fake_disk_read_block(uint32_t lba, uint8_t *block)
{
    if (!s_ready) {
        fake_disk_init();
    }
    if (lba < FAKE_DISK_BLOCK_COUNT) {
        memcpy(block, s_img[lba], FAKE_DISK_BLOCK_SIZE);
    } else {
        memset(block, 0, FAKE_DISK_BLOCK_SIZE);
    }
}

void fake_disk_write_block(uint32_t lba, const uint8_t *data)
{
    if (!s_ready) {
        fake_disk_init();
    }
    if (lba < FAKE_DISK_BLOCK_COUNT) {
        memcpy(s_img[lba], data, FAKE_DISK_BLOCK_SIZE);
    }
}

bool fake_disk_eicar_present(void)
{
#if EICAR_ENABLED
    if (!s_ready) {
        fake_disk_init();
    }
    // The EICAR directory entry is the second file entry (offset 64). Antivirus
    // deletion marks it 0xE5 or zeroes it; here it stops matching the name.
    const uint8_t *e = &s_img[FAKE_DISK_LBA_ROOTDIR][64];
    return memcmp(e, "EICAR   COM", 11) == 0;
#else
    return false;
#endif
}
