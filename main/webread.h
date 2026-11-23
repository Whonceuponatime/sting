// WiFi live read: the tester raises its own access point and serves a live
// view of the current test plus all stored sessions to a phone browser.
#pragma once

// Start the WiFi access point and web server on a background task, a moment
// after boot. No-op when WIFI_READ_ENABLED is 0. Safe to call once at startup.
void webread_start(void);
