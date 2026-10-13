// =============================================================================
// timelapse - one phone photo per layer, triggered from printer telemetry
// =============================================================================
// tools/inject_timelapse_markers.py adds a marker block to the G-code right
// after Bambu's own timelapse frame, while the toolhead is parked:
//
//     M400              ; park move finished
//     M140 S{bed+1}     ; marker: bed TARGET + tl_marker_delta
//     G4 S{dwell}       ; hold the park while the photo is taken
//     M140 S{bed}       ; restore
//
// This module watches bed_target_temper for that step and answers it with a
// Consumer Control "Volume Up" over Bluetooth LE (HID). A phone camera app set
// to "volume key = take photo" takes the frame.
//
// Why the bed and not the nozzle: the bed's thermal mass makes +1 C for a few
// seconds harmless, and other tools (FarmLoop, for one) already use low nozzle
// targets (M104 S1..S5) as markers.
//
// No board pins, no printer transport: the sketch feeds it printer state and
// calls loop() every pass. Nothing here blocks the caller: the HID press and
// release run on their own small task, notifications are queued to the NimBLE
// host task.
//
// BLE: the NimBLE host API that arduino-esp32 3.x ships for the ESP32-S3, C3,
// C6 and C5 (no extra library). The classic ESP32 uses Bluedroid in that core
// and is not supported by this file.
//
// The sketch MUST define `extern "C" bool btInUse() { return true; }`.
// Without it the core frees the Bluetooth controller's RAM at boot (no
// Arduino BLE library is linked), and the shutter can never start.
// =============================================================================
#pragma once
#include <Arduino.h>

namespace Timelapse {

// Persisted in NVS under the sketch's namespace. NVS keys are limited to 15
// characters, hence "tl_min_int_ms".
struct Config {
  bool     enabled       = true;                 // tl_enabled: take photos on markers
  uint16_t minIntervalMs = 1500;                 // tl_min_int_ms
  uint8_t  markerDelta   = 1;                    // tl_marker_delta (must match --delta)
  char     bleName[30]   = "Timelapse Shutter";  // tl_ble_name
  bool     logTemps      = true;                 // tl_log_temps
};

// Loads the config and starts the Bluetooth shutter. defaultName (optional) is
// the name the phone sees until `tl name` sets another one.
void begin(Print& log, const char* nvsNamespace, const char* defaultName = nullptr);

// Non-blocking housekeeping: base tracking, Bluetooth event logging,
// advertising watchdog. Call every loop() pass.
void loop();

// Printer telemetry. onPrinterState() on every gcode_state change.
// onBedTarget() for every report that carries bed_target_temper; repeats of
// the same value are fine, only changes count. layer is the latest layer_num
// (-1 if unknown) and is used for the log only.
void onPrinterState(const char* gcodeState);
void onBedTarget(float target, int layer);

// Optional: called on every detected marker, fired or not (e.g. an LED pulse
// for the latency video). Runs on the caller's task; keep it short.
void setMarkerHook(void (*hook)());

// "tl ..." commands from the sketch's serial input. Returns false when the
// line is not a timelapse command.
bool handleCommand(const String& line);

const Config& config();
bool bleConnected();

}  // namespace Timelapse
