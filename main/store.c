#include "store.h"

#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

#define NS        "sting"
#define KEY_COUNT "count"
#define KEY_LABEL "label"

static nvs_handle_t s_nvs;
static bool s_open;
static uint16_t s_count;

static void session_key(uint16_t number, char key[8])
{
    snprintf(key, 8, "s%04u", (unsigned)number);
}

static esp_err_t open_namespace(void)
{
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        return err;
    }
    s_open = true;
    s_count = 0;
    err = nvs_get_u16(s_nvs, KEY_COUNT, &s_count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    return err;
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
    return s_count;
}

bool store_read(uint16_t number, session_rec_t *out)
{
    if (!s_open || number == 0 || number > s_count) {
        return false;
    }
    char key[8];
    session_key(number, key);
    size_t len = sizeof(*out);
    memset(out, 0, sizeof(*out));
    if (nvs_get_blob(s_nvs, key, out, &len) != ESP_OK) {
        return false;
    }
    return len == sizeof(*out) && out->version == SESSION_REC_VERSION && out->size == sizeof(*out);
}

static esp_err_t write_record(const session_rec_t *rec)
{
    char key[8];
    session_key(rec->number, key);
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
    if (s_count >= MAX_SESSIONS) {
        return ESP_ERR_NO_MEM;
    }
    rec->number = s_count + 1;
    store_get_label(rec->label, sizeof(rec->label));

    esp_err_t err = write_record(rec);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u16(s_nvs, KEY_COUNT, rec->number);
    if (err != ESP_OK) {
        return err;
    }
    s_count = rec->number;
    if (rec->label[0] != '\0') {
        nvs_erase_key(s_nvs, KEY_LABEL); // the label is used up
    }
    return nvs_commit(s_nvs);
}

esp_err_t store_update(const session_rec_t *rec)
{
    if (!s_open || rec->number == 0 || rec->number > s_count) {
        return ESP_ERR_INVALID_STATE;
    }
    return write_record(rec);
}

esp_err_t store_drop(const session_rec_t *rec)
{
    if (!s_open || rec->number == 0 || rec->number != s_count) {
        return ESP_ERR_INVALID_STATE;
    }
    char key[8];
    session_key(rec->number, key);
    nvs_erase_key(s_nvs, key);
    s_count = rec->number - 1;
    esp_err_t err = nvs_set_u16(s_nvs, KEY_COUNT, s_count);
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
    s_count = 0;
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
