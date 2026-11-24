// USB probe: TinyUSB device with one mass-storage and one keyboard interface.
//
// Every TinyUSB callback below is a point where the *host* did something, so
// each one records what happened and when. The tester never initiates
// anything on the bus except the keyboard marker (see marker_task).

#include "usb_probe.h"

#include <stdio.h>
#include <string.h>

#include "device/dcd.h"
#include "esp_mac.h"
#include "esp_private/usb_phy.h"
#include "esp_timer.h"
#include "fake_disk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"

// ---------------------------------------------------------------------------
// Observation bookkeeping
// ---------------------------------------------------------------------------

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static observation_t s_obs;
static uint32_t s_generation;
static uint32_t s_last_change;

// Incremented on every bus reset; lets the marker notice a dropped transfer.
static volatile uint32_t s_link_epoch;
static volatile bool s_hid_inflight;
static volatile bool s_hid_failed;

static char s_serial[13] = "000000000000";

#define LOCK()   portENTER_CRITICAL_SAFE(&s_lock)
#define UNLOCK() portEXIT_CRITICAL_SAFE(&s_lock)

uint32_t probe_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Helpers below must be called with the lock held.
static void stamp(uint32_t *t, uint32_t now)
{
    if (*t == T_NONE) {
        *t = now;
    }
}

static void bump(uint16_t *counter)
{
    if (*counter != UINT16_MAX) {
        (*counter)++;
    }
}

static void changed(uint32_t now)
{
    s_generation++;
    s_last_change = now;
}

static void note_bus_reset(void)
{
    uint32_t now = probe_now_ms();
    LOCK();
    s_link_epoch++;
    stamp(&s_obs.t_bus_reset, now);
    bump(&s_obs.bus_resets);
    changed(now);
    UNLOCK();
}

static void note_descriptor(int kind)
{
    uint32_t now = probe_now_ms();
    LOCK();
    stamp(&s_obs.t_enum, now);
    bump(&s_obs.desc_count[kind]);
    changed(now);
    UNLOCK();
}

static void note_msc(uint8_t flag)
{
    uint32_t now = probe_now_ms();
    LOCK();
    if ((s_obs.msc_flags & flag) == 0) {
        s_obs.msc_flags |= flag;
        stamp(&s_obs.t_msc, now);
        if (flag == MSC_F_CAPACITY) {
            stamp(&s_obs.t_msc_capacity, now);
        }
        if (flag == MSC_F_READ) {
            stamp(&s_obs.t_msc_read, now);
        }
        changed(now);
    }
    UNLOCK();
}

static void note_hid(uint8_t flag)
{
    uint32_t now = probe_now_ms();
    LOCK();
    if ((s_obs.hid_flags & flag) == 0) {
        s_obs.hid_flags |= flag;
        stamp(&s_obs.t_hid, now);
        changed(now);
    }
    UNLOCK();
}

// The host wrote to the volume (e.g. antivirus quarantining EICAR, or the OS
// writing metadata). The write is applied to the RAM volume; nothing reaches
// flash and the volume is regenerated at the next boot.
static void note_write(uint32_t lba)
{
    uint32_t now = probe_now_ms();
    LOCK();
    stamp(&s_obs.t_msc_write, now);
    if (s_obs.msc_write_attempts != UINT16_MAX) {
        s_obs.msc_write_attempts++;
    }
    if (lba < 16) {
        s_obs.msc_write_lba_mask |= (uint16_t)(1u << lba);
    }
    changed(now);
    UNLOCK();
}

// The host deleted the EICAR directory entry: its antivirus quarantined it.
static void note_eicar_removed(void)
{
    uint32_t now = probe_now_ms();
    LOCK();
    stamp(&s_obs.t_eicar_removed, now);
    changed(now);
    UNLOCK();
}

void probe_snapshot(observation_t *out, uint32_t *generation, uint32_t *last_change_ms)
{
    LOCK();
    *out = s_obs;
    if (generation != NULL) {
        *generation = s_generation;
    }
    if (last_change_ms != NULL) {
        *last_change_ms = s_last_change;
    }
    UNLOCK();
}

const char *probe_serial(void)
{
    return s_serial;
}

// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------

enum { ITF_MSC = 0, ITF_HID, ITF_COUNT };

#define EP_MSC_OUT 0x01
#define EP_MSC_IN  0x81
#define EP_HID_IN  0x82

enum {
    STR_LANGID = 0,
    STR_MANUFACTURER,
    STR_PRODUCT,
    STR_SERIAL,
    STR_MSC,
    STR_HID,
};

static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00, // class is defined per interface
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

static const uint8_t s_hid_report_desc[] = { TUD_HID_REPORT_DESC_KEYBOARD() };

#define CONFIG_DESC_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t s_config_desc[] = {
    // config number, interface count, string index, total length, attributes, power (mA)
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_DESC_LEN, 0x00, 100),
    // interface, string index, EP out, EP in, EP size
    TUD_MSC_DESCRIPTOR(ITF_MSC, STR_MSC, EP_MSC_OUT, EP_MSC_IN, 64),
    // interface, string index, boot protocol, report descriptor length, EP in, EP size, poll interval (ms)
    TUD_HID_DESCRIPTOR(ITF_HID, STR_HID, HID_ITF_PROTOCOL_KEYBOARD, sizeof(s_hid_report_desc),
                       EP_HID_IN, CFG_TUD_HID_EP_BUFSIZE, 10),
};

static const char *string_for(uint8_t index)
{
    switch (index) {
    case STR_MANUFACTURER: return USB_MANUFACTURER;
    case STR_PRODUCT:      return USB_PRODUCT;
    case STR_SERIAL:       return s_serial;
    case STR_MSC:          return "STING test volume";
    case STR_HID:          return "STING marker keyboard";
    default:               return NULL;
    }
}

// ---------------------------------------------------------------------------
// TinyUSB device callbacks: bus, enumeration, configuration
// ---------------------------------------------------------------------------

// Called for every device-controller event, possibly from interrupt context.
void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr)
{
    (void)rhport;
    (void)in_isr;
    if (eventid == DCD_EVENT_BUS_RESET) {
        note_bus_reset();
    }
}

uint8_t const *tud_descriptor_device_cb(void)
{
    note_descriptor(DESC_DEVICE);
    return (uint8_t const *)&s_device_desc;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    note_descriptor(DESC_CONFIGURATION);
    return s_config_desc;
}

// The tester is full-speed only, so the correct answer to a device-qualifier
// request is a STALL (returning NULL does that). The request is still logged.
uint8_t const *tud_descriptor_device_qualifier_cb(void)
{
    note_descriptor(DESC_DEVICE_QUALIFIER);
    return NULL;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t desc[1 + 32];
    size_t len;

    note_descriptor(DESC_STRING);

    if (index == STR_LANGID) {
        desc[1] = 0x0409; // English (US)
        len = 1;
    } else {
        const char *str = string_for(index);
        if (str == NULL) {
            return NULL; // unknown index (e.g. 0xEE, Microsoft OS descriptor) -> STALL
        }
        len = strlen(str);
        if (len > 32) {
            len = 32;
        }
        for (size_t i = 0; i < len; i++) {
            desc[1 + i] = (uint16_t)str[i];
        }
    }
    desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc;
}

void tud_mount_cb(void)
{
    uint32_t now = probe_now_ms();
    LOCK();
    stamp(&s_obs.t_configured, now);
    bump(&s_obs.configured_count);
    changed(now);
    UNLOCK();
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    uint32_t now = probe_now_ms();
    LOCK();
    stamp(&s_obs.t_suspend, now);
    bump(&s_obs.suspends);
    changed(now);
    UNLOCK();
}

// ---------------------------------------------------------------------------
// Mass storage: read-only fake drive
// ---------------------------------------------------------------------------

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    note_msc(MSC_F_INQUIRY);
    memcpy(vendor_id, "STING   ", 8);
    memcpy(product_id, "Port Tester     ", 16);
    memcpy(product_rev, "0.1 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    note_msc(MSC_F_TEST_UNIT);
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    note_msc(MSC_F_CAPACITY);
    *block_count = FAKE_DISK_BLOCK_COUNT;
    *block_size = FAKE_DISK_BLOCK_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;
    (void)start;
    (void)load_eject;
    note_msc(MSC_F_OTHER);
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;
    static uint8_t block[FAKE_DISK_BLOCK_SIZE];

    note_msc(MSC_F_READ);
    if (lba >= FAKE_DISK_BLOCK_COUNT) {
        return -1;
    }

    uint8_t *out = buffer;
    uint32_t done = 0;
    uint32_t sectors = 0;
    uint16_t touched = 0; // sectors this request read from (bit N = LBA N)
    while (done < bufsize && lba < FAKE_DISK_BLOCK_COUNT) {
        uint32_t chunk = FAKE_DISK_BLOCK_SIZE - offset;
        if (chunk > bufsize - done) {
            chunk = bufsize - done;
        }
        fake_disk_read_block(lba, block);
        memcpy(out + done, block + offset, chunk);
        touched |= (uint16_t)(1u << lba);
        done += chunk;
        offset += chunk;
        if (offset == FAKE_DISK_BLOCK_SIZE) { // finished a whole sector
            offset = 0;
            lba++;
            sectors++;
        }
    }

    if (sectors > 0 || touched != 0) {
        uint32_t now = probe_now_ms();
        LOCK();
        s_obs.msc_sectors_read += sectors;
        s_obs.msc_lba_read_mask |= touched;
        changed(now);
        UNLOCK();
    }
    return (int32_t)done;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    // Writable, but only in RAM: the host can delete EICAR (so antivirus
    // quarantine is observable), and the volume is regenerated at the next boot.
    return true;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    static uint8_t block[FAKE_DISK_BLOCK_SIZE];
    bool had_eicar = fake_disk_eicar_present();

    uint32_t done = 0;
    while (done < bufsize && lba < FAKE_DISK_BLOCK_COUNT) {
        uint32_t chunk = FAKE_DISK_BLOCK_SIZE - offset;
        if (chunk > bufsize - done) {
            chunk = bufsize - done;
        }
        fake_disk_read_block(lba, block);          // read-modify-write the sector
        memcpy(block + offset, buffer + done, chunk);
        fake_disk_write_block(lba, block);
        note_write(lba);
        done += chunk;
        offset += chunk;
        if (offset == FAKE_DISK_BLOCK_SIZE) {
            offset = 0;
            lba++;
        }
    }

    // If the EICAR entry was present and is now gone, the host's antivirus
    // quarantined it.
    if (had_eicar && !fake_disk_eicar_present()) {
        note_eicar_removed();
    }
    return (int32_t)done;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;
    note_msc(MSC_F_OTHER);
    if (scsi_cmd[0] == SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL) {
        return 0; // nothing to do, report success
    }
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00); // invalid command
    return -1;
}

// ---------------------------------------------------------------------------
// HID keyboard
// ---------------------------------------------------------------------------

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    note_descriptor(DESC_HID_REPORT);
    note_hid(HID_F_REPORT_DESC);
    return s_hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    note_hid(HID_F_GET_REPORT);
    return 0;
}

// The host writes the keyboard LED state (Num/Caps/Scroll Lock) here.
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
    note_hid(HID_F_SET_REPORT);
}

bool tud_hid_set_idle_cb(uint8_t instance, uint8_t idle_rate)
{
    (void)instance;
    (void)idle_rate;
    note_hid(HID_F_SET_IDLE);
    return true;
}

void tud_hid_set_protocol_cb(uint8_t instance, uint8_t protocol)
{
    (void)instance;
    (void)protocol;
    note_hid(HID_F_SET_PROTOCOL);
}

// The host collected an input report from the keyboard's interrupt endpoint.
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len)
{
    (void)instance;
    (void)report;
    (void)len;
    uint32_t now = probe_now_ms();
    LOCK();
    s_obs.hid_flags |= HID_F_POLLED;
    stamp(&s_obs.t_hid, now);
    bump(&s_obs.hid_reports);
    changed(now);
    UNLOCK();
    s_hid_inflight = false;
}

// The input report was not delivered (transfer error). hid_send() retries it.
void tud_hid_report_failed_cb(uint8_t instance, hid_report_type_t report_type, uint8_t const *report,
                              uint16_t xferred_bytes)
{
    (void)instance;
    (void)report;
    (void)xferred_bytes;
    if (report_type == HID_REPORT_TYPE_INPUT) {
        s_hid_failed = true;
        s_hid_inflight = false;
    }
}

// ---------------------------------------------------------------------------
// Keyboard marker
// ---------------------------------------------------------------------------

// Only [a-z0-9] map to a key; everything else returns 0 and is skipped.
static uint8_t keycode_for(char c)
{
    if (c >= 'a' && c <= 'z') {
        return (uint8_t)(HID_KEY_A + (c - 'a'));
    }
    if (c >= '1' && c <= '9') {
        return (uint8_t)(HID_KEY_1 + (c - '1'));
    }
    if (c == '0') {
        return HID_KEY_0;
    }
    return 0;
}

static uint8_t marker_key_count(void)
{
    uint8_t n = 0;
    if (MARKER_ENABLED) {
        for (const char *p = MARKER_TEXT; *p != '\0' && n < UINT8_MAX; p++) {
            if (keycode_for(*p) != 0) {
                n++;
            }
        }
    }
    return n;
}

// Block until the host has kept the device configured for MARKER_DELAY_MS.
static void wait_until_settled(void)
{
    uint32_t settled = 0;
    while (settled < MARKER_DELAY_MS) {
        vTaskDelay(pdMS_TO_TICKS(50));
        settled = (tud_mounted() && !tud_suspended()) ? settled + 50 : 0;
    }
}

// Queue one keyboard report (keycode 0 = all keys released) and wait until the
// host collects it, however long that takes. A report that fails in transit is
// sent again, and a queued "key down" is never abandoned, so a key can never
// be left pressed while the link is up.
// Returns false only if a bus reset dropped the report (the host then forgets
// the keyboard's state anyway).
static bool hid_send(uint8_t keycode)
{
    for (;;) {
        uint32_t epoch = s_link_epoch;
        if (!(tud_mounted() && !tud_suspended() && tud_hid_ready())) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        uint8_t keys[6] = { keycode, 0, 0, 0, 0, 0 };
        s_hid_failed = false;
        s_hid_inflight = true;
        if (!tud_hid_keyboard_report(0, 0, keys)) {
            s_hid_inflight = false;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        while (s_hid_inflight) {
            if (s_link_epoch != epoch) {
                s_hid_inflight = false;
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        if (!s_hid_failed) {
            return true;
        }
        // transfer error: queue the same report again
    }
}

static void marker_task(void *arg)
{
    (void)arg;
    const char *text = MARKER_ENABLED ? MARKER_TEXT : "";
    bool need_neutral = true;

    wait_until_settled();
    for (;;) {
        // An all-keys-released report first: it types nothing, but the host
        // collecting it proves a keyboard driver is polling the interface.
        if (need_neutral) {
            if (!hid_send(0)) {
                wait_until_settled();
                continue;
            }
            need_neutral = false;
        }
        if (*text == '\0') {
            break;
        }
        uint8_t keycode = keycode_for(*text);
        if (keycode == 0) {
            text++;
            continue;
        }
        if (!hid_send(keycode)) { // key down lost to a bus reset: retry this key
            wait_until_settled();
            need_neutral = true;
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(MARKER_KEY_MS));
        bool released = hid_send(0); // key up

        uint32_t now = probe_now_ms();
        LOCK();
        s_obs.marker_sent++;
        changed(now);
        UNLOCK();
        text++;

        if (!released) { // a bus reset releases the key on the host side anyway
            wait_until_settled();
            need_neutral = true;
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(MARKER_KEY_MS));
    }

    if (MARKER_ENABLED) {
        uint32_t now = probe_now_ms();
        LOCK();
        stamp(&s_obs.t_marker_done, now);
        changed(now);
        UNLOCK();
    }
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Start-up
// ---------------------------------------------------------------------------

static void usb_task(void *arg)
{
    (void)arg;
    for (;;) {
        tud_task(); // blocks until there is a USB event to process
    }
}

void probe_start(void)
{
    memset(&s_obs, 0, sizeof(s_obs));
    s_obs.t_bus_reset = T_NONE;
    s_obs.t_enum = T_NONE;
    s_obs.t_configured = T_NONE;
    s_obs.t_msc = T_NONE;
    s_obs.t_hid = T_NONE;
    s_obs.t_msc_capacity = T_NONE;
    s_obs.t_msc_read = T_NONE;
    s_obs.t_msc_write = T_NONE;
    s_obs.t_eicar_removed = T_NONE;
    s_obs.t_marker_done = T_NONE;
    s_obs.t_suspend = T_NONE;
    s_obs.marker_len = marker_key_count();

    fake_disk_init(); // fresh RAM volume every boot (EICAR present again)

    uint8_t mac[6] = { 0 };
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    // Route the chip's internal USB PHY to the USB-OTG controller (device mode).
    usb_phy_config_t phy_config = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
    };
    usb_phy_handle_t phy = NULL;
    ESP_ERROR_CHECK(usb_new_phy(&phy_config, &phy));

    const tusb_rhport_init_t rhport_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL,
    };
    if (!tusb_init(0, &rhport_init)) {
        abort();
    }
    xTaskCreate(usb_task, "usb", 4096, NULL, 5, NULL);
    xTaskCreate(marker_task, "marker", 3072, NULL, 4, NULL);
}
