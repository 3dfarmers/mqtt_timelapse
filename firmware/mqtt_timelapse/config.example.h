// =============================================================================
// Your settings. Copy this file to config.h (same folder) and fill it in.
// config.h is in .gitignore, so your passwords never end up on GitHub.
//
// No config.h? The sketch builds with these placeholders, and you can type the
// same settings into the Serial Monitor instead (115200 baud):
//     wifi "<wifi name>" <password>
//     printer <ip> <access code> <serial>
// Values typed there are saved on the board and win over this file.
// =============================================================================
#pragma once

// --- WiFi: 2.4 GHz only, the ESP32 has no 5 GHz radio ------------------------
#define WIFI_SSID      "your-wifi-name"
#define WIFI_PASSWORD  "your-wifi-password"

// --- Bambu Lab printer, LAN mode ---------------------------------------------
// Printer screen > Settings > WLAN (or Network): IP address and access code.
// Serial number: printer screen > Settings > Device, or Bambu Studio > Device.
#define PRINTER_IP           "192.168.1.50"
#define PRINTER_ACCESS_CODE  "12345678"
#define PRINTER_SERIAL       "01P00A000000000"

// --- What takes the photo ----------------------------------------------------
// "phone": the board is a Bluetooth keyboard, your phone's camera app shoots on
//          Volume Up.
// "sony":  the board is a Sony Bluetooth remote for a Sony camera with
//          Menu > Network > Bluetooth > Bluetooth Rmt Ctrl (ZV-E10 II, a6400, ...).
// `tl shutter phone|sony` in the Serial Monitor changes it later (reboot).
#define SHUTTER "phone"

// --- Optional ----------------------------------------------------------------
// The name your phone or camera sees when pairing.
#define SHUTTER_NAME "Timelapse Shutter"

// LED that blinks 150 ms on every marker, for the latency test. LED_BUILTIN is
// the onboard (RGB) LED on most ESP32-S3 and C6 dev boards. A plain GPIO
// number works too. -1 = no LED.
#ifdef LED_BUILTIN
#define MARKER_LED LED_BUILTIN
#else
#define MARKER_LED -1
#endif
