// Session log in flash (NVS), as a ring buffer.
//
// Sessions are keyed by an absolute, ever-increasing number. The log keeps the
// newest MAX_SESSIONS; once full, the oldest is dropped to make room, so
// recording never stops. "first" is the number of the oldest kept session and
// "next" is the number the next new session will get, so count = next - first.

#include "store.h"

#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

#define NS        "sting"
#define KEY_FIRST "first"
#define KEY_NEXT  "next"
#define KEY_LABEL "label"

static nvs_handle_t s_nvs;
static bool s_open;
static uint32_t s_first = 1; // oldest kept session number
static uint32_t s_next = 1;  // number for the next new session

static void session_key(uint32_t number, char key[16])
{
    snprintf(key, 16, "s%u", (unsigned)number);
}

static esp_err_t open_namespace(void)
{
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        return err;
    }
    s_open = true;
    s_first = 1;
    s_next = 1;
    uint32_t v;
    if (nvs_get_u32(s_nvs, KEY_FIRST, &v) == ESP_OK) {
        s_first = v;
    }
    if (nvs_get_u32(s_nvs, KEY_NEXT, &v) == ESP_OK) {
        s_next = v;
    }
    if (s_next < s_first) { // corrupt counters: treat as empty, do not wipe data
        s_first = s_next;
    }
    return ESP_OK;
}

esp_err_t store_init(void)
{
    // Deliberately no "erase and retry" here: the log is evidence, so a
    // damaged partition is surfaced to the user (who can run `clear`).
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        return err;
    }
    return open_namespace();
}

uint16_t store_count(void)
{
    return (uint16_t)(s_next - s_first);
}

uint16_t store_next_number(void)
{
    return (uint16_t)s_next;
}

bool store_read(uint16_t ordinal, session_rec_t *out)
{
    // ordinal is 1..count, oldest first.
    if (!s_open || ordinal == 0 || ordinal > store_count()) {
        return false;
    }
    char key[16];
    session_key(s_first + ordinal - 1, key);
    size_t len = sizeof(*out);
    memset(out, 0, sizeof(*out));
    if (nvs_get_blob(s_nvs, key, out, &len) != ESP_OK) {
        return false;
    }
    return len == sizeof(*out) && out->version == SESSION_REC_VERSION && out->size == sizeof(*out);
}

static esp_err_t write_record(uint32_t number, const session_rec_t *rec)
{
    char key[16];
    session_key(number, key);
    esp_err_t err = nvs_set_blob(s_nvs, key, rec, sizeof(*rec));
    if (err != ESP_OK) {
        return err;
    }
    return nvs_commit(s_nvs);
}

esp_err_t store_begin(session_rec_t *rec)
{
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    // Make room by evicting the oldest, so recording never stops.
    while (store_count() >= MAX_SESSIONS) {
        char key[16];
        session_key(s_first, key);
        nvs_erase_key(s_nvs, key);
        s_first++;
        nvs_set_u32(s_nvs, KEY_FIRST, s_first);
    }

    rec->number = (uint16_t)s_next;
    store_get_label(rec->label, sizeof(rec->label));

    esp_err_t err = write_record(s_next, rec);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(s_nvs, KEY_NEXT, s_next + 1);
    if (err != ESP_OK) {
        return err;
    }
    s_next++;
    if (rec->label[0] != '\0') {
        nvs_erase_key(s_nvs, KEY_LABEL); // the label is used up
    }
    return nvs_commit(s_nvs);
}

esp_err_t store_update(const session_rec_t *rec)
{
    // The newest session is the only one still being written to.
    if (!s_open || s_next == s_first || rec->number != (uint16_t)(s_next - 1)) {
        return ESP_ERR_INVALID_STATE;
    }
    return write_record(s_next - 1, rec);
}

esp_err_t store_drop(const session_rec_t *rec)
{
    if (!s_open || s_next == s_first || rec->number != (uint16_t)(s_next - 1)) {
        return ESP_ERR_INVALID_STATE;
    }
    char key[16];
    session_key(s_next - 1, key);
    nvs_erase_key(s_nvs, key);
    s_next--;
    esp_err_t err = nvs_set_u32(s_nvs, KEY_NEXT, s_next);
    if (err == ESP_OK && rec->label[0] != '\0') {
        err = nvs_set_str(s_nvs, KEY_LABEL, rec->label); // hand the label back
    }
    if (err != ESP_OK) {
        return err;
    }
    return nvs_commit(s_nvs);
}

esp_err_t store_clear(void)
{
    if (s_open) {
        nvs_close(s_nvs);
        s_open = false;
    }
    s_first = 1;
    s_next = 1;
    // Erase the whole partition so nothing of the old log survives, and so a
    // damaged partition becomes usable again.
    nvs_flash_deinit();
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_flash_init();
    if (err != ESP_OK) {
        return err;
    }
    return open_namespace();
}

esp_err_t store_set_label(const char *label)
{
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err;
    if (label == NULL || label[0] == '\0') {
        err = nvs_erase_key(s_nvs, KEY_LABEL);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_str(s_nvs, KEY_LABEL, label);
    }
    if (err != ESP_OK) {
        return err;
    }
    return nvs_commit(s_nvs);
}

void store_get_label(char *out, size_t out_size)
{
    out[0] = '\0';
    if (!s_open) {
        return;
    }
    size_t len = out_size;
    if (nvs_get_str(s_nvs, KEY_LABEL, out, &len) != ESP_OK) {
        out[0] = '\0';
    }
}
