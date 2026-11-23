// STING - USB Port Liveness Tester
//
// Every power-up is one session: the probe on the native USB port watches what
// the host does for OBSERVE_WINDOW_MS, the LED shows the live verdict, and the
// result is written to flash so it survives being unplugged.

#include <string.h>

#include "cli.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led.h"
#include "session.h"
#include "store.h"
#include "usb_probe.h"
#include "webread.h"

#define TICK_MS 20

static SemaphoreHandle_t s_mutex; // guards the fields below and all log writes
static session_rec_t s_rec;
static bool s_recorded;  // s_rec exists in the log
static bool s_cli_mode;  // a laptop is on the UART port: not a port test
static bool s_store_fault;

void session_enter_cli_mode(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_cli_mode) {
        s_cli_mode = true;
        if (s_recorded) {
            store_drop(&s_rec);
            s_recorded = false;
        }
    }
    xSemaphoreGive(s_mutex);
}

bool session_is_cli_mode(void)
{
    return s_cli_mode;
}

// Build the current session from the live observations, so the WiFi live read
// shows exactly what the UART report would. Returns false in CLI/laptop mode,
// where there is no port test in progress.
bool session_live_snapshot(session_rec_t *out)
{
    if (s_cli_mode) {
        return false;
    }
    uint32_t now = probe_now_ms();
    memset(out, 0, sizeof(*out));
    out->version = SESSION_REC_VERSION;
    out->size = sizeof(*out);
    out->number = (uint16_t)(store_count() + 1); // provisional number
    out->window_done = now >= OBSERVE_WINDOW_MS;
    out->window_ms = OBSERVE_WINDOW_MS;
    out->t_saved = now;
    strlcpy(out->fw, STING_VERSION, sizeof(out->fw));
    store_get_label(out->label, sizeof(out->label));
    probe_snapshot(&out->obs, NULL, NULL);
    return true;
}

// Write the session to flash when there is something new worth keeping.
static void persist(uint32_t now)
{
    static uint32_t saved_generation;
    static uint32_t dirty_since;
    static uint32_t last_save;
    static uint32_t saves;
    static bool dirty;

    observation_t obs;
    uint32_t generation;
    uint32_t last_change;
    probe_snapshot(&obs, &generation, &last_change);

    bool window_done = now >= OBSERVE_WINDOW_MS;
    bool closing = window_done && !s_rec.window_done; // the verdict becomes final now

    // Nothing is logged until the host does something or the window closes.
    if (generation == 0 && !window_done) {
        return;
    }
    if (generation == saved_generation && s_recorded && !closing) {
        dirty = false;
        return;
    }
    if (!dirty) {
        dirty = true;
        dirty_since = now;
    }

    bool due;
    if (closing) {
        due = true;
    } else if (s_rec.window_done) {
        due = now - last_save >= SAVE_POST_WINDOW_MS && saves < SAVE_MAX_PER_SESSION;
    } else {
        due = now - last_change >= SAVE_QUIET_MS || now - dirty_since >= SAVE_MAX_DEFER_MS;
    }
    if (!due) {
        return;
    }

    s_rec.version = SESSION_REC_VERSION;
    s_rec.size = sizeof(s_rec);
    s_rec.window_done = window_done;
    s_rec.window_ms = OBSERVE_WINDOW_MS;
    s_rec.t_saved = now;
    strlcpy(s_rec.fw, STING_VERSION, sizeof(s_rec.fw));
    s_rec.obs = obs;

    esp_err_t err = s_recorded ? store_update(&s_rec) : store_begin(&s_rec);
    if (err != ESP_OK) {
        s_store_fault = true; // log full or flash error: shown on the LED
        s_rec.window_done = false;
        return;
    }
    s_recorded = true;
    saved_generation = generation;
    last_save = now;
    saves++;
    dirty = false;
}

static void show_led(uint32_t now)
{
    if (s_cli_mode) {
        led_set(0, 0, 255); // blue: laptop/CLI mode, not recording
        return;
    }
    if (s_store_fault && (now / 150) % 2 == 0) {
        led_set(255, 0, 255); // magenta flashes: result could NOT be saved
        return;
    }

    observation_t obs;
    probe_snapshot(&obs, NULL, NULL);

    // Blink while the observation window is open; solid once the final
    // verdict is safely in flash and the tester may be unplugged.
    bool final = s_recorded && s_rec.window_done;
    if (!final && !s_store_fault && (now / 250) % 2 == 1) {
        led_set(0, 0, 0);
        return;
    }
    switch (session_verdict(&obs)) {
    case VERDICT_ACTIVE:
        led_set(0, 255, 0); // green
        break;
    case VERDICT_ENUMERATES:
        led_set(255, 140, 0); // yellow
        break;
    default:
        led_set(255, 0, 0); // red
        break;
    }
}

void app_main(void)
{
    s_mutex = xSemaphoreCreateMutex();

    bool store_ok = store_init() == ESP_OK;
    s_store_fault = !store_ok;

    probe_start(); // USB first, so the host is never kept waiting
    led_init();
    cli_start(store_ok);
    webread_start(); // WiFi live read, brought up after USB has enumerated

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        uint32_t now = probe_now_ms();

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (!s_cli_mode && !s_store_fault) {
            persist(now);
        }
        show_led(now);
        xSemaphoreGive(s_mutex);
    }
}
