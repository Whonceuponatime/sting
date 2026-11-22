// Text rendering of one stored session. Pure C (printf only) so the exact
// same code can be run on a PC: see tests/host.

#include "report.h"

#include <stdio.h>

#include "fake_disk.h"

static const char *const DESC_NAMES[DESC_KIND_COUNT] = {
    [DESC_DEVICE] = "DEVICE",
    [DESC_CONFIGURATION] = "CONFIGURATION",
    [DESC_STRING] = "STRING",
    [DESC_DEVICE_QUALIFIER] = "DEVICE_QUALIFIER",
    [DESC_HID_REPORT] = "HID_REPORT",
};

static const char *yes_no(bool v)
{
    return v ? "yes" : "no";
}

// "t+12.345 s"
static const char *fmt_time(uint32_t t_ms, char buf[24])
{
    snprintf(buf, 24, "t+%lu.%03lu s", (unsigned long)(t_ms / 1000), (unsigned long)(t_ms % 1000));
    return buf;
}

static void print_stage(int index, const char *name, uint32_t t_ms, const char *reached_text)
{
    char tb[24];
    if (t_ms == T_NONE) {
        printf("  %d %-13s no\n", index, name);
    } else {
        printf("  %d %-13s YES  %-12s %s\n", index, name, fmt_time(t_ms, tb), reached_text);
    }
}

// What the host's sector reads reveal about its access to the volume. A host
// that only reads the boot sector merely identified the disk; one that reads
// the root directory listed the files; one that then reads the file sector
// pulled a file's contents. The last is direct evidence that file exchange is
// possible on this port, and is how a host inspecting the volume on insertion
// (AutoPlay / autorun-style behaviour) shows up.
static void print_read_detail(const observation_t *o)
{
    uint16_t m = o->msc_lba_read_mask;
    if (m == 0) {
        return;
    }
    bool boot = m & (1u << FAKE_DISK_LBA_BOOT);
    bool dir = m & (1u << FAKE_DISK_LBA_ROOTDIR);
    bool file = m & (1u << FAKE_DISK_LBA_FILE);

    printf("%20ssectors accessed:", "");
    const char *sep = " ";
    for (int lba = 0; lba < FAKE_DISK_BLOCK_COUNT; lba++) {
        if (m & (1u << lba)) {
            const char *tag = "";
            if (lba == FAKE_DISK_LBA_BOOT) tag = "=boot";
            else if (lba == FAKE_DISK_LBA_FAT) tag = "=FAT";
            else if (lba == FAKE_DISK_LBA_ROOTDIR) tag = "=dir";
            else if (lba == FAKE_DISK_LBA_FILE) tag = "=file";
            printf("%s%d%s", sep, lba, tag);
            sep = ", ";
        }
    }
    printf("\n");

    printf("%20shost access reached: ", "");
    if (file) {
        printf("file contents - the host read a file off the volume "
               "(file exchange is possible on this port)\n");
    } else if (dir) {
        printf("file listing - the host read the directory but not the file contents\n");
    } else if (boot) {
        printf("volume identification only - the host read the boot sector, no directory or file\n");
    } else {
        printf("data sectors only (no boot sector read)\n");
    }
}

void report_print_session(const session_rec_t *r)
{
    const observation_t *o = &r->obs;
    char tb[24];

    printf("\n--- Session %u", (unsigned)r->number);
    if (r->label[0] != '\0') {
        printf("  [%s]", r->label);
    }
    printf(" ---\n");

    if (r->window_done) {
        printf("  observed        %lu s window complete (last update %s)\n",
               (unsigned long)(r->window_ms / 1000), fmt_time(r->t_saved, tb));
    } else {
        printf("  observed        INCOMPLETE - unplugged before the %lu s window closed (last update %s)\n",
               (unsigned long)(r->window_ms / 1000), fmt_time(r->t_saved, tb));
    }

    // 1. Power
    printf("  1 %-13s YES  %-12s tester booted from port power\n", "power", "t+0.000 s");

    // 2. Bus activity
    if (o->t_bus_reset == T_NONE) {
        printf("  2 %-13s no\n", "bus activity");
    } else {
        printf("  2 %-13s YES  %-12s first bus reset (%u total)\n", "bus activity",
               fmt_time(o->t_bus_reset, tb), (unsigned)o->bus_resets);
    }

    // 3. Enumeration
    print_stage(3, "enumeration", o->t_enum, "first descriptor request");
    if (o->t_enum != T_NONE) {
        printf("%20srequested:", "");
        const char *sep = " ";
        for (int k = 0; k < DESC_KIND_COUNT; k++) {
            if (o->desc_count[k] > 0) {
                printf("%s%s x%u", sep, DESC_NAMES[k], (unsigned)o->desc_count[k]);
                sep = ", ";
            }
        }
        printf("\n");
    }

    // 4. Configuration
    print_stage(4, "configuration", o->t_configured, "host selected the configuration");

    // 5. Mass storage
    print_stage(5, "mass storage", o->t_msc, "first SCSI command from the host");
    if (o->t_msc != T_NONE) {
        printf("%20sinquiry: %s | capacity read: %s", "",
               yes_no(o->msc_flags & MSC_F_INQUIRY), yes_no(o->msc_flags & MSC_F_CAPACITY));
        if (o->t_msc_capacity != T_NONE) {
            printf(" (%s)", fmt_time(o->t_msc_capacity, tb));
        }
        printf(" | sectors read: %lu", (unsigned long)o->msc_sectors_read);
        if (o->t_msc_read != T_NONE) {
            printf(" (first %s)", fmt_time(o->t_msc_read, tb));
        }
        printf("\n");
        print_read_detail(o);
    }

    // 6. Keyboard
    print_stage(6, "keyboard", o->t_hid, "host keyboard (HID) driver attached");
    if (o->t_hid != T_NONE) {
        printf("%20sreport descriptor fetched: %s | LED state written by host: %s | input reports collected: %u\n",
               "", yes_no(o->hid_flags & HID_F_REPORT_DESC), yes_no(o->hid_flags & HID_F_SET_REPORT),
               (unsigned)o->hid_reports);
    }
    if (o->marker_len == 0) {
        printf("%20smarker: disabled in this firmware build (no keystrokes sent)\n", "");
    } else if (o->t_hid != T_NONE || o->marker_sent > 0) {
        printf("%20smarker: %u of %u keys typed", "", (unsigned)o->marker_sent, (unsigned)o->marker_len);
        if (o->t_marker_done != T_NONE) {
            printf(" (done %s)", fmt_time(o->t_marker_done, tb));
        }
        printf("\n");
    }

    if (o->suspends > 0) {
        printf("  note: host suspended the device %u time(s), first at %s\n",
               (unsigned)o->suspends, fmt_time(o->t_suspend, tb));
    }
    if (o->t_bus_reset != T_NONE && o->t_enum == T_NONE) {
        printf("  note: bus reset signalling was seen, but no descriptors were ever requested\n");
    }
    if (o->t_enum != T_NONE && o->t_enum >= r->window_ms) {
        printf("  note: first enumeration came after the %lu s window\n",
               (unsigned long)(r->window_ms / 1000));
    }

    verdict_t v = session_verdict(o);
    printf("  VERDICT: %s", verdict_name(v));
    switch (v) {
    case VERDICT_ACTIVE:
        printf(" - port is fully live: configured, %s%s%s accepted\n",
               o->t_msc != T_NONE ? "storage" : "",
               (o->t_msc != T_NONE && o->t_hid != T_NONE) ? " and " : "",
               o->t_hid != T_NONE ? "keyboard" : "");
        break;
    case VERDICT_ENUMERATES:
        printf(" - port is logically live: descriptors requested, no class driver bound\n");
        break;
    default:
        if (r->window_done) {
            printf(" - powered, but no enumeration within %lu s\n", (unsigned long)(r->window_ms / 1000));
        } else {
            printf(" - no enumeration seen, but the window was not completed\n");
        }
        break;
    }
    if (!r->window_done && v != VERDICT_DISABLED) {
        printf("  (provisional: the port may have reached a higher stage had it stayed plugged in)\n");
    }
}
