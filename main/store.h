// Persistent session log (NVS). Survives power loss between plug-ins.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "session.h"

// Open the log. Never erases anything; a damaged log is reported, not wiped.
esp_err_t store_init(void);

// Number of sessions currently stored.
uint16_t store_count(void);

// Read session `number` (1..store_count()). False if missing or unreadable.
bool store_read(uint16_t number, session_rec_t *out);

// Append a new session: assigns rec->number and attaches the pending label.
// ESP_ERR_NO_MEM when the log already holds MAX_SESSIONS sessions.
esp_err_t store_begin(session_rec_t *rec);

// Rewrite a session previously created with store_begin().
esp_err_t store_update(const session_rec_t *rec);

// Remove the newest session (must be `rec`) and give its label back.
esp_err_t store_drop(const session_rec_t *rec);

// Wipe every session and the pending label.
esp_err_t store_clear(void);

// Label for the next session. Empty string = none.
esp_err_t store_set_label(const char *label);
void store_get_label(char *out, size_t out_size);
