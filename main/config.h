// STING - USB Port Liveness Tester: build-time settings.
#pragma once

#define STING_VERSION "0.7.0"

// ---- Observation window ---------------------------------------------------
// A port that shows no enumeration within this time is reported as DISABLED.
#define OBSERVE_WINDOW_MS 30000

// ---- USB identity presented to the system under test -----------------------
// Espressif's VID with the "MSC + HID composite" PID. The product string names
// the tester so it is easy to recognise in the host's device/event logs.
#define USB_VID          0x303A
#define USB_PID          0x4006
#define USB_MANUFACTURER "STING"
#define USB_PRODUCT      "USB Port Liveness Tester"

// ---- Keyboard marker --------------------------------------------------------
// The only keystrokes the tester ever sends. Restricted to [a-z0-9]: no Enter,
// Tab, Space, modifiers or function keys, so it cannot confirm a dialog or run
// anything. Characters outside [a-z0-9] are skipped.
// Set MARKER_ENABLED to 0 to send no keystrokes at all; keyboard acceptance is
// still detected from the host's HID driver activity.
#define MARKER_ENABLED  1
#define MARKER_TEXT     "stingusbtest"
#define MARKER_DELAY_MS 1500 // wait after configuration before typing
#define MARKER_KEY_MS   20   // key hold / gap time

// ---- Onboard RGB LED ----------------------------------------------------------
// DevKitC-1 v1.0 has the LED on GPIO48, v1.1 on GPIO38. Both are driven so the
// same firmware works on either revision. Set the secondary to -1 to disable.
#define LED_GPIO_PRIMARY   48
#define LED_GPIO_SECONDARY 38
#define LED_BRIGHTNESS     32 // 0..255

// ---- Session log ----------------------------------------------------------------
#define MAX_SESSIONS 200
#define LABEL_MAX    32 // including the terminating NUL

// ---- EICAR antivirus test file ----------------------------------------------------
// When enabled, the fake drive also carries EICAR.COM: the industry-standard
// EICAR anti-malware test string. It is NOT malware - it is a harmless 68-byte
// pattern that every antivirus recognises by agreement, published for exactly
// this purpose. It lets you test whether the system under test runs antivirus
// that scans USB media: if it does, it should alert on or quarantine the file
// when the volume is read. The string is stored XOR-obfuscated and only
// reconstructed in RAM when the host reads it, so the host's AV sees the real
// signature while this firmware image and its source never contain it.
// Set to 0 to leave the file off the drive entirely.
#define EICAR_ENABLED 1

// ---- WiFi live read ---------------------------------------------------------------
// The tester raises its own WiFi access point so a phone browser can watch the
// current test live and read all stored sessions. Point-to-point: the board is
// the AP, nothing touches any other network.
//
// The radio is a convenience, not evidence: for a radio-silent, cleanly
// auditable build (or where transmitters are not allowed), set
// WIFI_READ_ENABLED to 0 and nothing is ever transmitted.
//
// The access point is brought up a moment AFTER USB starts so it never
// competes with the first, timing-critical enumeration.
#define WIFI_READ_ENABLED     1
#define WIFI_READ_SSID        "STING"      // network name the phone joins
#define WIFI_READ_PASSWORD    ""           // "" = open network; >= 8 chars enables WPA2
#define WIFI_READ_HOSTNAME    "sting"      // http://sting.local/ via mDNS (see note below)
#define WIFI_READ_CHANNEL     1
#define WIFI_READ_MAX_CONN    4
#define WIFI_READ_START_DELAY_MS 1500      // let USB enumerate first
// The page is always at http://192.168.4.1/ . The friendly http://sting.local/
// name is served over mDNS, which resolves on Apple devices and most desktops
// but not in many Android browsers; there, use the numeric address.

// ---- Flash-write scheduling -------------------------------------------------------
// Flash writes briefly stall USB interrupt handling, so a changed session is
// saved once the bus has been quiet for a moment rather than mid-enumeration.
#define SAVE_QUIET_MS         400   // bus quiet for this long -> save
#define SAVE_MAX_DEFER_MS     3000  // ...but never hold a change longer than this
#define SAVE_POST_WINDOW_MS   5000  // min interval between saves after the window
#define SAVE_MAX_PER_SESSION  100   // flash-wear guard if left plugged in
