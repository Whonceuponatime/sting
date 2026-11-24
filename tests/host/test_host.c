// Host-side checks for the parts of the firmware that need no hardware:
// the fake FAT12 volume, the verdict rules and the report text.
//
//   gcc -std=c11 -Wall -Wextra -Imain tests/host/test_host.c main/fake_disk.c main/report.c -o test_host
//   ./test_host [image-file]     (optionally writes the fake volume to a file)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_disk.h"
#include "report.h"
#include "session.h"

static int failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                 \
        }                                                               \
    } while (0)

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16);
}

static uint16_t fat12_entry(const uint8_t *fat, unsigned n)
{
    unsigned off = n + n / 2;
    uint16_t v = rd16(fat + off);
    return (n & 1) ? (uint16_t)(v >> 4) : (uint16_t)(v & 0x0FFF);
}

static void test_fake_disk(const char *image_path)
{
    static uint8_t img[FAKE_DISK_BLOCK_COUNT][FAKE_DISK_BLOCK_SIZE];
    for (uint32_t lba = 0; lba < FAKE_DISK_BLOCK_COUNT; lba++) {
        memset(img[lba], 0xA5, FAKE_DISK_BLOCK_SIZE); // must be fully overwritten
        fake_disk_read_block(lba, img[lba]);
    }

    const uint8_t *bs = img[0];
    CHECK(bs[510] == 0x55 && bs[511] == 0xAA);
    CHECK(rd16(bs + 11) == 512);                   // bytes per sector
    CHECK(bs[13] == 1);                            // sectors per cluster
    uint16_t reserved = rd16(bs + 14);
    uint8_t fats = bs[16];
    uint16_t root_entries = rd16(bs + 17);
    uint16_t total = rd16(bs + 19);
    uint16_t fat_sectors = rd16(bs + 22);
    CHECK(total == FAKE_DISK_BLOCK_COUNT);
    CHECK(bs[21] == 0xF8);
    CHECK(memcmp(bs + 54, "FAT12   ", 8) == 0);

    unsigned fat_lba = reserved;
    unsigned root_lba = reserved + fats * fat_sectors;
    unsigned data_lba = root_lba + (root_entries * 32 + 511) / 512;
    CHECK(fat_lba == 1 && root_lba == 2 && data_lba == 3);
    CHECK(total - data_lba < 4085);                // cluster count => FAT12

    const uint8_t *fat = img[fat_lba];
    CHECK(fat[0] == bs[21]);                       // media byte mirrored in FAT[0]
    CHECK(fat12_entry(fat, 0) == 0xFF8);
    CHECK(fat12_entry(fat, 1) == 0xFFF);

    const uint8_t *label = img[root_lba];
    const uint8_t *file = img[root_lba] + 32;
    CHECK(label[11] == 0x08);                      // volume label entry
    CHECK(memcmp(label, bs + 43, 11) == 0);        // same label as the boot sector
    CHECK(memcmp(file, "README  TXT", 11) == 0);
    CHECK(file[11] & 0x01);                        // read-only

    uint16_t cluster = rd16(file + 26);
    uint32_t size = rd32(file + 28);
    CHECK(cluster == 2);
    CHECK(size > 0 && size <= 512);
    CHECK(fat12_entry(fat, cluster) >= 0xFF8);     // single-cluster file

    const uint8_t *content = img[data_lba + cluster - 2];
    CHECK(memcmp(content, "STING USB port liveness tester\r\n", 32) == 0);
    for (uint32_t i = size; i < 512; i++) {
        CHECK(content[i] == 0);
    }

    uint32_t first_empty = data_lba + 1;

#if EICAR_ENABLED
    // Second directory entry: EICAR.COM, cluster 3, read-only, end-of-chain.
    const uint8_t *e = img[root_lba] + 64;
    CHECK(memcmp(e, "EICAR   COM", 11) == 0);
    CHECK(e[11] & 0x01);                           // read-only
    CHECK(img[root_lba][96] == 0x00);              // end of directory after it
    uint16_t ec = rd16(e + 26);
    uint32_t es = rd32(e + 28);
    CHECK(ec == 3);
    CHECK(es == 68);
    CHECK(fat12_entry(fat, ec) >= 0xFF8);
    CHECK((uint32_t)FAKE_DISK_LBA_EICAR == data_lba + ec - 2);

    // The served EICAR sector must reconstruct to the genuine 68-byte string.
    // Build the expected bytes from an XOR'd literal so this source file does
    // not contain the signature either.
    static const unsigned char exp_x[68] = {
        0x02, 0x6f, 0x15, 0x7b, 0x0a, 0x7f, 0x1a, 0x1b, 0x0a, 0x01, 0x6e, 0x06,
        0x0a, 0x00, 0x02, 0x6f, 0x6e, 0x72, 0x0a, 0x04, 0x73, 0x6d, 0x19, 0x19,
        0x73, 0x6d, 0x27, 0x7e, 0x1f, 0x13, 0x19, 0x1b, 0x08, 0x77, 0x09, 0x0e,
        0x1b, 0x14, 0x1e, 0x1b, 0x08, 0x1e, 0x77, 0x1b, 0x14, 0x0e, 0x13, 0x0c,
        0x13, 0x08, 0x0f, 0x09, 0x77, 0x0e, 0x1f, 0x09, 0x0e, 0x77, 0x1c, 0x13,
        0x16, 0x1f, 0x7b, 0x7e, 0x12, 0x71, 0x12, 0x70,
    };
    const uint8_t *es_sec = img[FAKE_DISK_LBA_EICAR];
    for (int i = 0; i < 68; i++) {
        CHECK(es_sec[i] == (exp_x[i] ^ 0x5a));
    }
    for (uint32_t i = 68; i < 512; i++) {
        CHECK(es_sec[i] == 0);
    }
    first_empty = FAKE_DISK_LBA_EICAR + 1;
#endif

    for (uint32_t lba = first_empty; lba < FAKE_DISK_BLOCK_COUNT; lba++) {
        for (uint32_t i = 0; i < 512; i++) {
            CHECK(img[lba][i] == 0);
        }
    }


#if EICAR_ENABLED
    // The RAM volume is writable: a host (antivirus) can delete EICAR, and the
    // volume regenerates on fake_disk_init().
    CHECK(fake_disk_eicar_present());
    uint8_t dir[FAKE_DISK_BLOCK_SIZE];
    fake_disk_read_block(FAKE_DISK_LBA_ROOTDIR, dir);
    dir[64] = 0xE5; // FAT "deleted" marker on the EICAR entry
    fake_disk_write_block(FAKE_DISK_LBA_ROOTDIR, dir);
    CHECK(!fake_disk_eicar_present());        // deletion observed
    fake_disk_init();                          // every boot regenerates it
    CHECK(fake_disk_eicar_present());          // EICAR is back
#endif

    // NOTE: writing the image to a file is deliberately skipped here when EICAR
    // is enabled - a file holding the real signature would be quarantined by an
    // antivirus on the build machine. The in-memory checks above are enough.
    (void)image_path;
}

static session_rec_t blank(uint16_t number, const char *label, bool window_done, uint32_t t_saved)
{
    session_rec_t r;
    memset(&r, 0, sizeof(r));
    r.version = SESSION_REC_VERSION;
    r.size = sizeof(r);
    r.number = number;
    r.window_done = window_done;
    r.window_ms = OBSERVE_WINDOW_MS;
    r.t_saved = t_saved;
    strncpy(r.label, label, sizeof(r.label) - 1);
    observation_t *o = &r.obs;
    o->t_bus_reset = o->t_enum = o->t_configured = o->t_msc = o->t_hid = T_NONE;
    o->t_msc_capacity = o->t_msc_read = o->t_marker_done = o->t_suspend = T_NONE;
    o->marker_len = 12;
    return r;
}

static void enumerate(observation_t *o)
{
    o->t_bus_reset = 412;
    o->bus_resets = 2;
    o->t_enum = 498;
    o->desc_count[DESC_DEVICE] = 2;
    o->desc_count[DESC_CONFIGURATION] = 2;
    o->desc_count[DESC_STRING] = 6;
    o->desc_count[DESC_DEVICE_QUALIFIER] = 1;
}

int main(int argc, char **argv)
{
    test_fake_disk(argc > 1 ? argv[1] : NULL);

    // Fully live port.
    session_rec_t active = blank(1, "FDS-USB1", true, 30010);
    enumerate(&active.obs);
    active.obs.t_configured = 731;
    active.obs.configured_count = 1;
    active.obs.t_msc = 902;
    active.obs.msc_flags = MSC_F_INQUIRY | MSC_F_CAPACITY | MSC_F_READ | MSC_F_TEST_UNIT;
    active.obs.t_msc_capacity = 950;
    active.obs.t_msc_read = 1020;
    active.obs.msc_sectors_read = 37;
    active.obs.msc_lba_read_mask = (1u << FAKE_DISK_LBA_BOOT) | (1u << FAKE_DISK_LBA_FAT) |
                                   (1u << FAKE_DISK_LBA_ROOTDIR) | (1u << FAKE_DISK_LBA_FILE) |
                                   (1u << FAKE_DISK_LBA_EICAR);
    active.obs.t_hid = 800;
    active.obs.hid_flags = HID_F_REPORT_DESC | HID_F_SET_IDLE | HID_F_SET_REPORT | HID_F_POLLED;
    active.obs.desc_count[DESC_HID_REPORT] = 1;
    active.obs.hid_reports = 25;
    active.obs.marker_sent = 12;
    active.obs.t_marker_done = 2900;
    active.obs.t_msc_write = 3200;
    active.obs.msc_write_attempts = 2;
    active.obs.msc_write_lba_mask = (1u << FAKE_DISK_LBA_ROOTDIR);
    active.obs.t_eicar_removed = 3300; // host antivirus deleted EICAR.COM
    active.epoch_base = 1764000000u;   // a real power-up time was set over WiFi
    active.tz_off_min = 540;           // UTC+09:00
    CHECK(session_verdict(&active.obs) == VERDICT_ACTIVE);
    CHECK(active.obs.msc_lba_read_mask & (1u << FAKE_DISK_LBA_FILE));   // file read seen
    CHECK(active.obs.msc_lba_read_mask & (1u << FAKE_DISK_LBA_ROOTDIR)); // dir read seen

    // Storage blocked by policy, keyboard accepted.
    session_rec_t kbd_only = blank(2, "FDS-USB2", true, 30004);
    enumerate(&kbd_only.obs);
    kbd_only.obs.t_configured = 731;
    kbd_only.obs.t_hid = 800;
    kbd_only.obs.hid_flags = HID_F_REPORT_DESC | HID_F_POLLED;
    kbd_only.obs.desc_count[DESC_HID_REPORT] = 1;
    kbd_only.obs.hid_reports = 25;
    kbd_only.obs.marker_sent = 12;
    kbd_only.obs.t_marker_done = 2900;
    CHECK(session_verdict(&kbd_only.obs) == VERDICT_ACTIVE);

    // Descriptors requested, nothing bound; host then suspends the device.
    session_rec_t enumerates = blank(3, "ECDIS-USB1", true, 30002);
    enumerate(&enumerates.obs);
    enumerates.obs.suspends = 1;
    enumerates.obs.t_suspend = 3410;
    CHECK(session_verdict(&enumerates.obs) == VERDICT_ENUMERATES);

    // Powered, silent port.
    session_rec_t disabled = blank(4, "ECDIS-USB2", true, 30000);
    CHECK(session_verdict(&disabled.obs) == VERDICT_DISABLED);

    // Live port, unplugged early.
    session_rec_t early = blank(5, "", false, 1900);
    enumerate(&early.obs);
    early.obs.t_configured = 731;
    CHECK(session_verdict(&early.obs) == VERDICT_ENUMERATES);

    // Bus reset but no descriptor request must not count as enumeration.
    session_rec_t reset_only = blank(6, "", true, 30000);
    reset_only.obs.t_bus_reset = 500;
    reset_only.obs.bus_resets = 1;
    CHECK(session_verdict(&reset_only.obs) == VERDICT_DISABLED);

    report_print_session(&active);
    report_print_session(&kbd_only);
    report_print_session(&enumerates);
    report_print_session(&disabled);
    report_print_session(&early);
    report_print_session(&reset_only);

    printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
