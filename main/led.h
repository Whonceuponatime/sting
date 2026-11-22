// Onboard RGB LED (WS2812).
#pragma once

#include <stdint.h>

void led_init(void);

// Set the colour; each channel 0..255 before LED_BRIGHTNESS scaling.
// Repeated calls with the same colour are ignored.
void led_set(uint8_t r, uint8_t g, uint8_t b);
