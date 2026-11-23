// Text rendering of one stored session. All output goes through emit(), which
// writes either to stdout (the UART report) or into a caller's buffer (the
// WiFi live read), so both show the exact same text. Pure C: the host tests in
// tests/host run it on a PC.

#include "report.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "fake_disk.h"

// Output target for the current render. The HTTP server serves one request at
// a time and the CLI runs on its own task, so a single static target is safe.
static char *s_buf;
static size_t s_cap;
static size_t s_len;
static bool s_to_buf;

static void emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (s_to_buf) {
        if (s_len < s_cap) {
            int n = vsnprintf(s_buf + s_len, s_cap - s_len, fmt, ap);
            if (n > 0) {
                size_t avail = s_cap - s_len - 1; // keep the NUL
                s_len += ((size_t)n <= avail) ? (size_t)n : avail;
            }
        }
    } else {
        vprintf(fmt, ap);
    }
    va_end(ap);
}

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

// Real local time of an event, given the session's power-up epoch and the
// client's timezone offset: "2026-11-24 14:30:55 (UTC+09:00)". Empty if the
// clock was never set for this session.
static const char *fmt_wall(uint32_t epoch_base, int16_t tz_off_min, uint32_t t_ms, char buf[64])
{
    buf[0] = '\0';
    if (epoch_base == 0) {
        return buf;
    }
    time_t local = (time_t)epoch_base + (time_t)tz_off_min * 60 + (time_t)(t_ms / 1000);
    struct tm *tm = gmtime(&local); // offset already folded in, so gmtime == local
    char when[24] = "unknown";
    if (tm != NULL) {
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", tm);
    }
    int off = tz_off_min;
    char sign = off < 0 ? '-' : '+';
    if (off < 0) {
        off = -off;
    }
    snprintf(buf, 64, "%s (UTC%c%02d:%02d)", when, sign, off / 60, off % 60);
    return buf;
}

static void print_stage(int index, const char *name, uint32_t t_ms, const char *reached_text)
{
    char tb[24];
    if (t_ms == T_NONE) {
        emit("  %d %-13s no\n", index, name);
    } else {
        emit("  %d %-13s YES  %-12s %s\n", index, name, fmt_time(t_ms, tb), reached_text);
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

    emit("%20ssectors accessed:", "");
    const char *sep = " ";
    for (int lba = 0; lba < FAKE_DISK_BLOCK_COUNT; lba++) {
        if (m & (1u << lba)) {
            const char *tag = "";
            if (lba == FAKE_DISK_LBA_BOOT) tag = "=boot";
            else if (lba == FAKE_DISK_LBA_FAT) tag = "=FAT";
            else if (lba == FAKE_DISK_LBA_ROOTDIR) tag = "=dir";
            else if (lba == FAKE_DISK_LBA_FILE) tag = "=file";
            else if (lba == FAKE_DISK_LBA_EICAR) tag = "=eicar";
            emit("%s%d%s", sep, lba, tag);
            sep = ", ";
        }
    }
    emit("\n");

    emit("%20shost access reached: ", "");
    if (file) {
        emit("file contents - the host read a file off the volume "
             "(file exchange is possible on this port)\n");
    } else if (dir) {
        emit("file listing - the host read the directory but not the file contents\n");
    } else if (boot) {
        emit("volume identification only - the host read the boot sector, no directory or file\n");
    } else {
        emit("data sectors only (no boot sector read)\n");
    }

    bool eicar = m & (1u << FAKE_DISK_LBA_EICAR);
    if (eicar) {
        emit("%20santivirus test (EICAR): the host READ the EICAR test file. If it runs "
             "antivirus on\n%20sUSB media, it should alert on or quarantine it - confirm on "
             "the host side.\n", "", "");
    } else if (dir) {
        emit("%20santivirus test (EICAR): host listed the directory but did NOT read the EICAR "
             "file\n%20s(an active antivirus may have blocked the read before it reached the "
             "device).\n", "", "");
    }
}

// Render one session through emit() (to stdout or the active buffer).
static void render_session(const session_rec_t *r)
{
    const observation_t *o = &r->obs;
    char tb[24];

    emit("\n--- Session %u", (unsigned)r->number);
    if (r->label[0] != '\0') {
        emit("  [%s]", r->label);
    }
    emit(" ---\n");

    char wb[64];
    if (r->epoch_base != 0) {
        emit("  when            %s\n", fmt_wall(r->epoch_base, r->tz_off_min, 0, wb));
    } else {
        emit("  when            clock not set for this session (relative times only)\n");
    }

    if (r->window_done) {
        emit("  observed        %lu s window complete (last update %s)\n",
             (unsigned long)(r->window_ms / 1000), fmt_time(r->t_saved, tb));
    } else {
        emit("  observed        INCOMPLETE - unplugged before the %lu s window closed (last update %s)\n",
             (unsigned long)(r->window_ms / 1000), fmt_time(r->t_saved, tb));
    }

    // 1. Power
    emit("  1 %-13s YES  %-12s tester booted from port power\n", "power", "t+0.000 s");

    // 2. Bus activity
    if (o->t_bus_reset == T_NONE) {
        emit("  2 %-13s no\n", "bus activity");
    } else {
        emit("  2 %-13s YES  %-12s first bus reset (%u total)\n", "bus activity",
             fmt_time(o->t_bus_reset, tb), (unsigned)o->bus_resets);
    }

    // 3. Enumeration
    print_stage(3, "enumeration", o->t_enum, "first descriptor request");
    if (o->t_enum != T_NONE) {
        emit("%20srequested:", "");
        const char *sep = " ";
        for (int k = 0; k < DESC_KIND_COUNT; k++) {
            if (o->desc_count[k] > 0) {
                emit("%s%s x%u", sep, DESC_NAMES[k], (unsigned)o->desc_count[k]);
                sep = ", ";
            }
        }
        emit("\n");
    }

    // 4. Configuration
    print_stage(4, "configuration", o->t_configured, "host selected the configuration");

    // 5. Mass storage
    print_stage(5, "mass storage", o->t_msc, "first SCSI command from the host");
    if (o->t_msc != T_NONE) {
        emit("%20sinquiry: %s | capacity read: %s", "",
             yes_no(o->msc_flags & MSC_F_INQUIRY), yes_no(o->msc_flags & MSC_F_CAPACITY));
        if (o->t_msc_capacity != T_NONE) {
            emit(" (%s)", fmt_time(o->t_msc_capacity, tb));
        }
        emit(" | sectors read: %lu", (unsigned long)o->msc_sectors_read);
        if (o->t_msc_read != T_NONE) {
            emit(" (first %s)", fmt_time(o->t_msc_read, tb));
        }
        emit("\n");
        print_read_detail(o);
    }

    // 6. Keyboard
    print_stage(6, "keyboard", o->t_hid, "host keyboard (HID) driver attached");
    if (o->t_hid != T_NONE) {
        emit("%20sreport descriptor fetched: %s | LED state written by host: %s | input reports collected: %u\n",
             "", yes_no(o->hid_flags & HID_F_REPORT_DESC), yes_no(o->hid_flags & HID_F_SET_REPORT),
             (unsigned)o->hid_reports);
    }
    if (o->marker_len == 0) {
        emit("%20smarker: disabled in this firmware build (no keystrokes sent)\n", "");
    } else if (o->t_hid != T_NONE || o->marker_sent > 0) {
        emit("%20smarker: %u of %u keys typed", "", (unsigned)o->marker_sent, (unsigned)o->marker_len);
        if (o->t_marker_done != T_NONE) {
            emit(" (done %s)", fmt_time(o->t_marker_done, tb));
        }
        emit("\n");
    }

    if (o->suspends > 0) {
        emit("  note: host suspended the device %u time(s), first at %s\n",
             (unsigned)o->suspends, fmt_time(o->t_suspend, tb));
    }
    if (o->t_bus_reset != T_NONE && o->t_enum == T_NONE) {
        emit("  note: bus reset signalling was seen, but no descriptors were ever requested\n");
    }
    if (o->t_enum != T_NONE && o->t_enum >= r->window_ms) {
        emit("  note: first enumeration came after the %lu s window\n",
             (unsigned long)(r->window_ms / 1000));
    }

    verdict_t v = session_verdict(o);
    emit("  VERDICT: %s", verdict_name(v));
    switch (v) {
    case VERDICT_ACTIVE:
        emit(" - port is fully live: configured, %s%s%s accepted\n",
             o->t_msc != T_NONE ? "storage" : "",
             (o->t_msc != T_NONE && o->t_hid != T_NONE) ? " and " : "",
             o->t_hid != T_NONE ? "keyboard" : "");
        break;
    case VERDICT_ENUMERATES:
        emit(" - port is logically live: descriptors requested, no class driver bound\n");
        break;
    default:
        if (r->window_done) {
            emit(" - powered, but no enumeration within %lu s\n", (unsigned long)(r->window_ms / 1000));
        } else {
            emit(" - no enumeration seen, but the window was not completed\n");
        }
        break;
    }
    if (!r->window_done && v != VERDICT_DISABLED) {
        emit("  (provisional: the port may have reached a higher stage had it stayed plugged in)\n");
    }
}

void report_print_session(const session_rec_t *r)
{
    s_to_buf = false;
    render_session(r);
}

size_t report_session_to_buf(const session_rec_t *r, char *buf, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    s_buf = buf;
    s_cap = cap;
    s_len = 0;
    s_to_buf = true;
    buf[0] = '\0';
    render_session(r);
    s_to_buf = false;
    return s_len;
}
