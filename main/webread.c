// WiFi live read.
//
// The tester raises its own WPA2 access point (SoftAP). A phone joins it and
// opens http://192.168.4.1/ , which shows the current test live and links to
// the full stored report. It is a closed point-to-point link: the board is the
// access point and nothing is routed to any other network.
//
// The radio is brought up WIFI_READ_START_DELAY_MS after boot so it never
// competes with the first, timing-critical USB enumeration. Set
// WIFI_READ_ENABLED to 0 for a radio-silent build.

#include "webread.h"

#include "config.h"

#if WIFI_READ_ENABLED

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "report.h"
#include "session.h"
#include "store.h"
#include "usb_probe.h"

// One HTTP request is served at a time, so these scratch buffers are shared.
static char s_sess_buf[2048];
static char s_tmp[512];

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------

static const char PAGE_HTML[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>STING live</title><style>"
    "body{margin:0;background:#111;color:#ddd;font-family:system-ui,sans-serif}"
    "#banner{padding:16px;font-size:20px;font-weight:700;color:#fff;background:#555;text-align:center}"
    "#sub{padding:6px 12px;font-size:13px;color:#aaa;text-align:center}"
    "pre{margin:0;padding:12px;white-space:pre-wrap;word-break:break-word;"
    "font-family:ui-monospace,Consolas,monospace;font-size:12px;line-height:1.35}"
    "a.btn{display:block;margin:12px;padding:12px;text-align:center;background:#246;"
    "color:#fff;text-decoration:none;border-radius:8px;font-weight:600}"
    "</style></head><body>"
    "<div id=banner>connecting...</div>"
    "<div id=sub>live view - refreshes every second</div>"
    "<pre id=out>loading...</pre>"
    "<a class=btn href=\"/report\">Open full stored report</a>"
    "<script>"
    "var M={ACTIVE:['#0a7a28','fully live'],ENUMERATES:['#b8860b','logically live'],"
    "DISABLED:['#a00','disabled']};"
    "async function tick(){try{"
    "var r=await fetch('/live',{cache:'no-store'});var t=await r.text();"
    "document.getElementById('out').textContent=t;"
    "var m=t.match(/VERDICT:\\s*(\\w+)/);var v=m?m[1]:'';var b=document.getElementById('banner');"
    "if(M[v]){b.style.background=M[v][0];b.textContent=v+' - '+M[v][1];}"
    "else if(t.indexOf('laptop')>=0){b.style.background='#246';b.textContent='LAPTOP MODE';}"
    "else{b.style.background='#555';b.textContent='waiting for a port...';}"
    "}catch(e){document.getElementById('banner').textContent='disconnected';"
    "document.getElementById('banner').style.background='#555';}}"
    "setInterval(tick,1000);tick();"
    "</script></body></html>";

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t live_get(httpd_req_t *req)
{
    session_rec_t rec;
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (!session_live_snapshot(&rec)) {
        snprintf(s_tmp, sizeof(s_tmp),
                 "LAPTOP MODE - the tester is on the laptop/UART port, not testing a port.\n"
                 "%u session(s) stored. Open the full stored report below.\n",
                 (unsigned)store_count());
        return httpd_resp_send(req, s_tmp, HTTPD_RESP_USE_STRLEN);
    }

    uint32_t now = probe_now_ms();
    int head = snprintf(s_tmp, sizeof(s_tmp),
                        "STING live view  (uptime %lu.%03lu s / window %d s)\n"
                        "stored sessions: %u   next number: %u\n",
                        (unsigned long)(now / 1000), (unsigned long)(now % 1000),
                        OBSERVE_WINDOW_MS / 1000, (unsigned)store_count(), (unsigned)rec.number);
    if (head < 0) {
        head = 0;
    }
    httpd_resp_send_chunk(req, s_tmp, head);

    size_t n = report_session_to_buf(&rec, s_sess_buf, sizeof(s_sess_buf));
    httpd_resp_send_chunk(req, s_sess_buf, n);
    return httpd_resp_send_chunk(req, NULL, 0); // end
}

static esp_err_t report_get(httpd_req_t *req)
{
    session_rec_t rec;
    uint16_t count = store_count();

    httpd_resp_set_type(req, "text/plain");

    int head = snprintf(s_tmp, sizeof(s_tmp),
                        "=== STING USB PORT LIVENESS REPORT ===\n"
                        "firmware : sting %s\n"
                        "tester   : USB %04x:%04x \"%s\" serial %s\n"
                        "window   : %d s observation per session\n"
                        "times    : seconds since tester power-up (no real-time clock)\n"
                        "sessions : %u\n",
                        STING_VERSION, USB_VID, USB_PID, USB_PRODUCT, probe_serial(),
                        OBSERVE_WINDOW_MS / 1000, (unsigned)count);
    if (head < 0) {
        head = 0;
    }
    httpd_resp_send_chunk(req, s_tmp, head);

    for (uint16_t i = 1; i <= count; i++) {
        if (store_read(i, &rec)) {
            size_t n = report_session_to_buf(&rec, s_sess_buf, sizeof(s_sess_buf));
            httpd_resp_send_chunk(req, s_sess_buf, n);
        } else {
            int n = snprintf(s_tmp, sizeof(s_tmp),
                             "\n--- Session %u ---\n  (record unreadable or from a different firmware version)\n",
                             (unsigned)i);
            httpd_resp_send_chunk(req, s_tmp, n > 0 ? n : 0);
        }
    }

    int tail = snprintf(s_tmp, sizeof(s_tmp), "\n=== END OF REPORT ===\n");
    httpd_resp_send_chunk(req, s_tmp, tail > 0 ? tail : 0);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static void start_http(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 4;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        return;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_get },
        { .uri = "/live", .method = HTTP_GET, .handler = live_get },
        { .uri = "/report", .method = HTTP_GET, .handler = report_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}

static void start_ap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, WIFI_READ_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen((char *)ap.ap.ssid);
    ap.ap.channel = WIFI_READ_CHANNEL;
    ap.ap.max_connection = WIFI_READ_MAX_CONN;

    const char *pw = WIFI_READ_PASSWORD;
    if (strlen(pw) >= 8) {
        strlcpy((char *)ap.ap.password, pw, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN; // too short for WPA2: fall back to open
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Advertise http://<hostname>.local/ over mDNS. Resolves on Apple devices
    // and most desktops; many Android browsers do not resolve .local, so the
    // page is always reachable at the numeric AP address 192.168.4.1 as well.
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(WIFI_READ_HOSTNAME);
        mdns_instance_name_set("STING USB Port Liveness Tester");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }

    start_http();
}

static void webread_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(WIFI_READ_START_DELAY_MS));
    start_ap();
    vTaskDelete(NULL);
}

void webread_start(void)
{
    xTaskCreate(webread_task, "webread", 4096, NULL, 2, NULL);
}

#else // WIFI_READ_ENABLED == 0

void webread_start(void) {}

#endif
