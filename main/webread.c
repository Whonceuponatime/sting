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
#include <stdlib.h>
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
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>STING live</title><style>"
    ":root{--bg:#0f1115;--card:#171a21;--fg:#e8edf4;--mut:#93a1b3;--line:#262b34}"
    "*{box-sizing:border-box}"
    "body{margin:0;background:var(--bg);color:var(--fg);"
    "font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;line-height:1.5}"
    ".wrap{max-width:680px;margin:0 auto;padding:0 14px 32px}"
    "#banner{position:sticky;top:0;margin:0 -14px;padding:22px 16px;text-align:center;"
    "font-size:30px;font-weight:800;letter-spacing:.5px;color:#fff;background:#3a4150;"
    "box-shadow:0 2px 10px rgba(0,0,0,.4)}"
    "#binfo{font-size:14px;font-weight:600;opacity:.92;margin-top:4px}"
    "#status{display:flex;align-items:center;justify-content:center;gap:8px;"
    "padding:10px;color:var(--mut);font-size:13px}"
    "#dot{width:9px;height:9px;border-radius:50%;background:#3fb950;"
    "box-shadow:0 0 0 0 rgba(63,185,80,.6);animation:p 2s infinite}"
    "@keyframes p{0%{box-shadow:0 0 0 0 rgba(63,185,80,.5)}70%{box-shadow:0 0 0 10px rgba(63,185,80,0)}"
    "100%{box-shadow:0 0 0 0 rgba(63,185,80,0)}}"
    "#dot.stale{background:#d29922;animation:none}"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:12px;"
    "padding:10px 14px;margin:10px 0}"
    ".ln{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:13px;"
    "white-space:pre-wrap;word-break:break-word;padding:2px 0}"
    ".ses{font-weight:800;font-size:16px;color:#fff;margin:4px 0 8px;"
    "font-family:system-ui,sans-serif;border-bottom:1px solid var(--line);padding-bottom:6px}"
    ".yes{color:#7ee2a8}.yes b{color:#fff}"
    ".no{color:#6b7684}"
    ".detail{color:var(--mut);font-size:12px;padding-left:4px}"
    ".note{color:#e3b341}"
    ".verdict{font-weight:800;font-size:15px;color:#fff;background:#222833;"
    "border-radius:8px;padding:10px 12px;margin:8px 0;font-family:system-ui,sans-serif}"
    "a.btn{display:block;margin:14px 0 0;padding:14px;text-align:center;background:#1f6feb;"
    "color:#fff;text-decoration:none;border-radius:10px;font-weight:700;font-size:15px}"
    ".hint{color:var(--mut);font-size:12px;text-align:center;margin-top:14px}"
    "</style></head><body><div class=wrap>"
    "<div id=banner>Connecting<div id=binfo>&nbsp;</div></div>"
    "<div id=status><span id=dot></span><span id=upd>starting live view...</span></div>"
    "<div class=card id=out>Loading...</div>"
    "<a class=btn href=\"/report\">Open full stored report</a>"
    "<div class=hint>Network: STING &middot; sting.local or 192.168.4.1 &middot; refreshes every second</div>"
    "<script>"
    "var M={ACTIVE:['#1a7f37','FULLY LIVE - configured, storage and/or keyboard accepted'],"
    "ENUMERATES:['#9e6a03','LOGICALLY LIVE - descriptors requested, no driver bound'],"
    "DISABLED:['#b22','DISABLED - powered, no enumeration in the window']};"
    "function esc(s){return s.replace(/[&<>]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;'}[c];});}"
    "function cls(l){"
    "if(/^\\s*VERDICT:/.test(l))return'verdict';"
    "if(/^---/.test(l)||/^===/.test(l))return'ses';"
    "if(/^\\s*note:/.test(l))return'note';"
    "if(/\\bYES\\b/.test(l))return'yes';"
    "if(/^\\s*[1-6]\\s+\\S.*\\bno\\s*$/.test(l))return'no';"
    "if(/^\\s{6,}\\S/.test(l))return'detail';"
    "return'ln';}"
    "function render(t){var o='';var a=t.split('\\n');for(var i=0;i<a.length;i++){"
    "var l=a[i];if(l.trim()==='')continue;var c=cls(l);"
    "var h=esc(l).replace(/\\bYES\\b/,'<b>YES</b>');"
    "o+='<div class=\"ln '+c+'\">'+h+'</div>';}return o;}"
    "var last=0;"
    "async function tick(){var b=document.getElementById('banner'),bi=document.getElementById('binfo'),"
    "dot=document.getElementById('dot'),upd=document.getElementById('upd');try{"
    "var q='?e='+Math.floor(Date.now()/1000)+'&z='+(-new Date().getTimezoneOffset());"
    "var r=await fetch('/live'+q,{cache:'no-store'});var t=await r.text();"
    "document.getElementById('out').innerHTML=render(t);"
    "var m=t.match(/VERDICT:\\s*(\\w+)/);var v=m?m[1]:'';"
    "if(M[v]){b.style.background=M[v][0];b.firstChild.textContent=v;bi.textContent=M[v][1];}"
    "else if(t.indexOf('LAPTOP')>=0){b.style.background='#1f6feb';b.firstChild.textContent='LAPTOP MODE';"
    "bi.textContent='reading reports - not testing a port';}"
    "else{b.style.background='#3a4150';b.firstChild.textContent='WAITING';"
    "bi.textContent='plug the USB port into a port to test';}"
    "last=Date.now();dot.className='';upd.textContent='live - updated just now';"
    "}catch(e){dot.className='stale';upd.textContent='disconnected - is WiFi STING still joined?';}}"
    "setInterval(tick,1000);"
    "setInterval(function(){if(last&&Date.now()-last>3000){"
    "document.getElementById('dot').className='stale';}},1000);"
    "tick();"
    "</script></div></body></html>";

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

// Pick up the phone's clock from ?e=<epoch seconds>&z=<tz offset minutes>.
static void absorb_time_query(httpd_req_t *req)
{
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen <= 1 || qlen > sizeof(s_tmp)) {
        return;
    }
    if (httpd_req_get_url_query_str(req, s_tmp, qlen) != ESP_OK) {
        return;
    }
    char ev[16];
    char zv[12];
    if (httpd_query_key_value(s_tmp, "e", ev, sizeof(ev)) != ESP_OK) {
        return;
    }
    uint32_t epoch = (uint32_t)strtoul(ev, NULL, 10);
    int16_t tz = 0;
    if (httpd_query_key_value(s_tmp, "z", zv, sizeof(zv)) == ESP_OK) {
        tz = (int16_t)atoi(zv);
    }
    session_set_wallclock(epoch, tz);
}

static esp_err_t live_get(httpd_req_t *req)
{
    session_rec_t rec;
    absorb_time_query(req);
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
