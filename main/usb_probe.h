// The probe: a USB composite device (fake drive + keyboard) on the native
// "USB" port that records everything the host does to it.
#pragma once

#include <stdint.h>

#include "session.h"

// Bring up the USB device stack and start observing.
void probe_start(void);

// Milliseconds since tester power-up.
uint32_t probe_now_ms(void);

// Copy the observations so far. `generation` increases with every change and
// `last_change_ms` is when the latest change happened; either may be NULL.
void probe_snapshot(observation_t *out, uint32_t *generation, uint32_t *last_change_ms);

// USB serial number string of this tester (derived from the chip's MAC).
const char *probe_serial(void);
