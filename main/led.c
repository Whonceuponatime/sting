#include "led.h"

#include <stddef.h>

#include "config.h"
#include "led_strip.h"

static led_strip_handle_t s_strips[2];
static uint32_t s_current = UINT32_MAX;

static led_strip_handle_t open_strip(int gpio)
{
    if (gpio < 0) {
        return NULL;
    }
    led_strip_config_t strip_config = {
        .strip_gpio_num = gpio,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    led_strip_handle_t strip = NULL;
    if (led_strip_new_rmt_device(&strip_config, &rmt_config, &strip) != ESP_OK) {
        return NULL;
    }
    led_strip_clear(strip);
    return strip;
}

void led_init(void)
{
    s_strips[0] = open_strip(LED_GPIO_PRIMARY);
    s_strips[1] = open_strip(LED_GPIO_SECONDARY);
}

void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t packed = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    if (packed == s_current) {
        return;
    }
    s_current = packed;
    for (size_t i = 0; i < 2; i++) {
        if (s_strips[i] == NULL) {
            continue;
        }
        led_strip_set_pixel(s_strips[i], 0,
                            r * LED_BRIGHTNESS / 255,
                            g * LED_BRIGHTNESS / 255,
                            b * LED_BRIGHTNESS / 255);
        led_strip_refresh(s_strips[i]);
    }
}
