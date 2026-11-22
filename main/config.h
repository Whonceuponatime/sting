// STING - USB Port Liveness Tester: build-time settings.
#pragma once

#define STING_VERSION "0.2.0"

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

// ---- Flash-write scheduling -------------------------------------------------------
// Flash writes briefly stall USB interrupt handling, so a changed session is
// saved once the bus has been quiet for a moment rather than mid-enumeration.
#define SAVE_QUIET_MS         400   // bus quiet for this long -> save
#define SAVE_MAX_DEFER_MS     3000  // ...but never hold a change longer than this
#define SAVE_POST_WINDOW_MS   5000  // min interval between saves after the window
#define SAVE_MAX_PER_SESSION  100   // flash-wear guard if left plugged in
