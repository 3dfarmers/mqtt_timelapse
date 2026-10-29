# CLAUDE.md

Guidance for Claude Code when someone asks to adapt this project to their
printer, board or phone. The user is usually a maker at home following a
YouTube video, not an embedded developer: explain what you change and why, in
plain words, and give them commands they can paste.

## What it does

A sliced Bambu file gets a marker after each timelapse park (bed target +1 °C
for a few seconds). An ESP32 reads the printer's MQTT reports, sees the step and
sends a Bluetooth HID "Volume Up" to a paired phone. The camera app shoots.

```
tools/inject_timelapse_markers.py   G-code side: inserts the marker, fixes the MD5
firmware/mqtt_timelapse/
  mqtt_timelapse.ino   WiFi, Bambu MQTT (TLS 8883, user bblp, access code),
                       report parsing, serial commands, settings in NVS
  timelapse.h/.cpp     marker detection + Bluetooth LE HID shutter (NimBLE)
  camera_sony.h/.cpp   Sony camera shutter: the board as Sony's Bluetooth remote
  config.example.h     template for config.h (gitignored, holds the secrets)
```

Data flow: `onReport()` (MQTT callback) parses `gcode_state`, `layer_num`,
`bed_target_temper` and calls `Timelapse::onPrinterState()` /
`Timelapse::onBedTarget()`. The module decides whether a change is a marker and
wakes its own shutter task, which sends press, waits 50 ms, sends release.

## Rules

- **Secrets.** Never write the user's WiFi password or printer access code into
  a tracked file, a commit, or your replies. They go into `config.h` (gitignored)
  or the Serial Monitor (`wifi ...`, `printer ...`), typed by the user. If they
  paste them into the chat, say you will not store them and point to those two places.
- Keep `extern "C" bool btInUse(void) { return true; }` in the sketch. Without
  it the core frees the Bluetooth controller RAM at boot and the shutter never starts.
- Do not set `WiFi.setSleep(false)`: WiFi and Bluetooth share one radio and the
  coexistence scheduler needs modem sleep.
- The MQTT callback parses the payload in place (NUL after the payload inside
  PubSubClient's buffer). Do not add a copy of the report; RAM is shared with
  TLS (~54 KB) and NimBLE (~77 KB).
- The G-code script only inserts lines: every original line must stay
  byte-identical, and it refuses rather than guesses. Keep both properties.
- **Moves on someone's printer.** Never invent park coordinates or new motion.
  If a printer has no built-in park (non-Bambu, Traditional timelapse), ask the
  user for a park position they know is safe and have them try it without a
  part on the bed first.
- After any firmware change: compile, flash, then `status`, `tl shot` and a
  reboot (phone must reconnect by itself). After a script change: run it on the
  user's real file without an output first and show what it found.

## Build and flash

Core: esp32 by Espressif 3.3.x. Library: PubSubClient 2.8.

```bash
arduino-cli core install esp32:esp32
arduino-cli lib install PubSubClient
arduino-cli board list                       # find the port
arduino-cli compile --fqbn <FQBN> firmware/mqtt_timelapse
arduino-cli upload -p <PORT> --fqbn <FQBN> firmware/mqtt_timelapse
arduino-cli monitor -p <PORT> -c baudrate=115200
```

| Chip | FQBN | Notes |
|---|---|---|
| ESP32-S3 | `esp32:esp32:esp32s3:CDCOnBoot=cdc` | tested; 1.20 MB of the 1.25 MB default app slot |
| ESP32-C3 | `esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=min_spiffs` | compiles, untested on hardware; 1,309,549 bytes = 99.9% of the default slot, hence min_spiffs (1.9 MB); SuperMini boards usually have a plain LED on GPIO 8, active low |
| ESP32-C6 / C5 | `esp32:esp32:esp32c6:CDCOnBoot=cdc,PartitionScheme=min_spiffs` | NimBLE in the core, untested |
| ESP32 (classic) | - | not supported: the core uses Bluedroid there, so `timelapse.cpp` has no NimBLE headers |

`CDCOnBoot=cdc` is for boards plugged in by their native USB port. Boards with
a USB-UART chip (CP2102, CH340) can drop it.

## Adapting

### Another Bambu printer

1. `dump on` in the Serial Monitor during a print: check that reports contain
   `bed_target_temper` and `gcode_state`. Newer models may report temperatures in
   a different structure; if so, change the field reads in `onReport()`.
2. Run the script on a sliced file without an output. It needs `M971 S11`
   inside a `M1002 judge_flag timelapse_record_flag` / `M622 J1` ... `M623` block
   with an XY move (the park) before it. If the model's template differs, look at
   the file's timelapse section and adapt `scan()` - keep the refusal for anything
   it does not recognise.
3. Full-report printers (X1, P2S, H2) send 9-18 KB reports many times a second;
   `MQTT_BUFFER_SIZE` (24 KB) must stay above the largest one (`[MQTT] ... fills the
   buffer` says when it does not).

### Another board

Pick the FQBN above. `MARKER_LED` in `config.h`: `LED_BUILTIN` where the board
defines one, a GPIO number otherwise, `-1` for none. An active-low LED needs the
writes in `markerLed()` / `ledLoop()` swapped.

### Another phone or camera app

The key is `SHUTTER_USAGE` in `timelapse.cpp` (HID Consumer page, default
`0x00E9` Volume Up; `0x00EA` Volume Down). An app that only reacts to keyboard
keys would need a keyboard collection added to `REPORT_MAP` and a second report;
check the app's settings for a volume-key option first.

### A real camera instead of a phone

`SHUTTER "sony"` (or `tl shutter sony` + reboot) turns the board from a
Bluetooth peripheral (phone connects to it) into a central: it scans for a Sony
camera, pairs, and writes the remote's button codes. The mode is fixed per boot
because the two roles need different pairing security (phone: Just Works;
Sony: authenticated, numeric comparison confirmed on the camera).

- Detection: manufacturer data 0x012D / type 0x0003, mode byte after tag 0x22
  (pairing enabled = bit 6, remote enabled = bit 1).
- Control: service `8000ff00-ff00-ffff-ffff-ffffffffffff`, characteristic
  `0xFF01`, written with response: `01 07` focus down, `01 09` shutter down,
  `01 08` shutter up, `01 06` focus up.
- The protocol is reverse engineered (furble, Greg Leeds; links in
  `camera_sony.h`). If a Sony model behaves differently, compare with
  furble's `lib/furble/Sony.*`.
- Other brands: furble also implements Canon, Fujifilm and Nikon remotes.
  A new brand = a new `camera_<brand>.cpp` with the same seven functions as
  `camera_sony.h`, a new `ShutterMode` value, and its pairing security in
  `bleBegin()`. Test pairing, `tl shot` and reconnect on the real camera before
  claiming support.

### A printer that is not a Bambu

Replace the MQTT part of the sketch with a poll of the printer's API (every
0.5-1 s, non-blocking, short timeouts) and keep feeding the same two calls:
`Timelapse::onPrinterState("RUNNING")` while printing (any other string
otherwise) and `Timelapse::onBedTarget(target, layer)`.

| Firmware | Request | Bed target | Printing |
|---|---|---|---|
| Klipper / Moonraker | `GET /printer/objects/query?heater_bed=target&print_stats=state` | `heater_bed.target` | `print_stats.state == "printing"` |
| PrusaLink | `GET /api/v1/status` (API key header) | `printer.target_bed` | `printer.state == "PRINTING"` |
| OctoPrint | `GET /api/printer` (API key header) | `temperature.bed.target` | `state.flags.printing` |

The G-code side has no Bambu timelapse block there: the marker goes into the
slicer's layer-change G-code together with a park move the user provides
(Rules). The poll interval adds to the delay, so the dwell must be longer.

### Detection tuning

`timelapse.cpp`: `BASE_HOLD_MS` (1 s until a value becomes the base),
`MARKER_TOL` (±0.3 °C), `MARKER_STUCK_MS` (30 s until a held marker counts as a
real setpoint), `KEY_PRESS_MS` (50 ms). User-facing: `tl interval`, `tl delta`.

## Measured (ESP32-S3, core 3.3.8, P1S)

- Flash: 1,207,849 bytes with the Sony mode (92% of the default slot); static RAM 48 KB.
- Heap: 245 KB free at boot, 169 KB after NimBLE starts, ~40 KB free with WiFi,
  the TLS link and Bluetooth all running (lowest seen 29 KB).
- Phone reconnects 2-5 s after the board starts advertising, using the stored bond,
  also after a firmware update over USB.
- Shutter task: a phone press uses ~2.1 KB of its 4 KB stack, a Sony shot ~1.7 KB.
- Sony ZV-E10 II: first pairing ~4 s from "connecting" to READY; after a reboot
  the saved camera is found at ~5 s and READY at ~6 s, no pairing screen. All
  four button writes (focus/shutter down, up) are acknowledged.
- P1S largest report seen: 4.6 KB (pushall reply). H2S: 18 KB.
