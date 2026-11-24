// What the tester observes during one plug-in, and how it is judged.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

#define T_NONE UINT32_MAX // "stage not reached"

// Descriptor types the host can request.
enum {
    DESC_DEVICE,
    DESC_CONFIGURATION,
    DESC_STRING,
    DESC_DEVICE_QUALIFIER,
    DESC_HID_REPORT,
    DESC_KIND_COUNT
};

// Evidence that the host's mass-storage driver talked to the fake drive.
#define MSC_F_INQUIRY   0x01
#define MSC_F_CAPACITY  0x02 // READ CAPACITY / READ FORMAT CAPACITIES
#define MSC_F_READ      0x04 // READ(10)
#define MSC_F_TEST_UNIT 0x08 // TEST UNIT READY
#define MSC_F_OTHER     0x10 // any other SCSI command

// Evidence that the host's HID driver attached to the keyboard.
#define HID_F_REPORT_DESC  0x01 // HID report descriptor fetched
#define HID_F_SET_IDLE     0x02
#define HID_F_SET_PROTOCOL 0x04
#define HID_F_SET_REPORT   0x08 // host wrote the keyboard LED state
#define HID_F_GET_REPORT   0x10
#define HID_F_POLLED       0x20 // host collected an input report

// All times are milliseconds since tester power-up.
typedef struct {
    uint32_t t_bus_reset;    // stage 2: first bus reset
    uint32_t t_enum;         // stage 3: first descriptor request
    uint32_t t_configured;   // stage 4: SET_CONFIGURATION
    uint32_t t_msc;          // stage 5: first SCSI command
    uint32_t t_hid;          // stage 6: first HID driver activity
    uint32_t t_msc_capacity;
    uint32_t t_msc_read;
    uint32_t t_msc_write;      // first host write to the volume
    uint32_t t_eicar_removed;  // host deleted/quarantined EICAR.COM (antivirus)
    uint32_t t_marker_done;
    uint32_t t_suspend;
    uint32_t msc_sectors_read;
    uint16_t bus_resets;
    uint16_t suspends;
    uint16_t configured_count;
    uint16_t hid_reports;    // input reports the host collected
    uint16_t desc_count[DESC_KIND_COUNT];
    // One bit per sector the host read (bit N = LBA N). The fake drive has 16
    // sectors, so which bits are set shows whether the host only identified the
    // disk or went on to read the directory and the file: evidence of the host
    // inspecting the volume's contents, not just its size.
    uint16_t msc_lba_read_mask;
    // Writes the host made to the volume, and which sectors. A write that
    // removes the EICAR directory entry is antivirus quarantining the file
    // (see t_eicar_removed).
    uint16_t msc_write_attempts;
    uint16_t msc_write_lba_mask;
    uint8_t msc_flags;
    uint8_t hid_flags;
    uint8_t marker_len;      // keys in the marker
    uint8_t marker_sent;     // keys the host actually collected
} observation_t;

#define SESSION_REC_VERSION 5

// One stored session (NVS blob).
typedef struct {
    uint16_t version;
    uint16_t size;
    uint16_t number;      // absolute session number
    uint8_t window_done;  // 1 once the observation window had fully elapsed
    uint8_t reserved;
    uint32_t t_saved;     // uptime when this record was last written
    uint32_t window_ms;
    // Real wall-clock time at power-up (t+0), as Unix epoch seconds UTC, set by
    // a phone over WiFi. 0 = unknown (then only relative times are available).
    // tz_off_min is the phone's timezone offset east of UTC, in minutes.
    uint32_t epoch_base;
    int16_t tz_off_min;
    uint16_t reserved2;
    char label[LABEL_MAX];
    char fw[12];
    observation_t obs;
} session_rec_t;

typedef enum {
    VERDICT_DISABLED,   // powered, no enumeration
    VERDICT_ENUMERATES, // descriptors requested, no class driver bound
    VERDICT_ACTIVE,     // configured, storage and/or keyboard accepted
} verdict_t;

static inline verdict_t session_verdict(const observation_t *o)
{
    if (o->t_msc != T_NONE || o->t_hid != T_NONE) {
        return VERDICT_ACTIVE;
    }
    if (o->t_enum != T_NONE) {
        return VERDICT_ENUMERATES;
    }
    return VERDICT_DISABLED;
}

static inline const char *verdict_name(verdict_t v)
{
    switch (v) {
    case VERDICT_ACTIVE:     return "ACTIVE";
    case VERDICT_ENUMERATES: return "ENUMERATES";
    default:                 return "DISABLED";
    }
}

// main.c: called by the CLI when a valid command arrives on the UART port.
// That proves this power-up is a laptop connection, not a port test, so the
// current session is removed from the log and recording stops until reboot.
void session_enter_cli_mode(void);

// True once the tester is in CLI/laptop mode (see session_enter_cli_mode).
bool session_is_cli_mode(void);

// Fill `out` with the current session built from the live observations.
// Returns false in CLI/laptop mode, where no port test is in progress.
bool session_live_snapshot(session_rec_t *out);

// Set the real wall-clock time from a connected client (the WiFi page):
// `epoch` is current Unix time in seconds UTC, `tz_off_min` the client's
// timezone offset east of UTC in minutes. The current session is then stamped
// with the computed power-up time so the report can show a real date.
void session_set_wallclock(uint32_t epoch, int16_t tz_off_min);
