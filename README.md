# MQTT Timelapse

One phone photo per layer, triggered by your Bambu Lab printer. No sensor, no
wire to the printer, no app on the phone besides a camera.

```
 sliced file ──► inject_timelapse_markers.py ──► printer parks the head, bed target +1 °C for 3 s
                                                        │  (MQTT over your WiFi)
                                                        ▼
                     phone camera ◄── Bluetooth "Volume Up" ◄── ESP32 board
```

1. A small script adds a marker to your sliced G-code, right after Bambu's own
   timelapse park: the **bed target goes up by 1 °C** for a few seconds, then back.
   The bed is so heavy that 1 °C for 3 seconds changes nothing.
2. The printer reports its bed target over MQTT on your network, as it always does.
3. An **ESP32 board** (about 10 €) reads those reports. On the +1 °C step it
   presses **Volume Up** on your phone over Bluetooth, like a selfie remote.
4. Your camera app takes the photo while the head is parked. Stitch the photos
   into a video at the end.

## What you need

| | |
|---|---|
| Printer | Bambu Lab in LAN mode (LAN-only is fine). Tested on a **P1S**. X1 and P1 files work; for A1, P2S and H2 run the script without an output first and check what it finds. |
| Board | Any **ESP32-S3** dev board (tested). ESP32-C3, C6 and C5 should work. Not the original ESP32. |
| Phone or camera | Android with **Open Camera** (tested). Most camera apps take a photo on a volume key, including the iPhone Camera app (untested here). Or a **Sony camera** with Bluetooth remote support (see below). Tripod, charger. |
| Computer | Arduino IDE 2 (or `arduino-cli`), Python 3.8+, Bambu Studio |

## 1. Flash the board

1. Arduino IDE > **Boards Manager**: install **esp32** by Espressif Systems, version 3.3.x.
2. **Library Manager**: install **PubSubClient** by Nick O'Leary.
3. Open `firmware/mqtt_timelapse/mqtt_timelapse.ino`.
4. Copy `config.example.h` to **`config.h`** in the same folder and fill in your
   WiFi (2.4 GHz) and printer: IP address and LAN access code from the printer's
   network settings, serial number from Settings > Device. `config.h` is
   gitignored, so it never ends up on GitHub.
5. Tools > Board: **ESP32S3 Dev Module**. If you plug into the board's native USB
   port, set **USB CDC On Boot: Enabled**. Upload. On an ESP32-C3 or C6, also set
   Tools > Partition Scheme: **Minimal SPIFFS (1.9MB APP with OTA)**; the
   firmware nearly fills the default one there.
6. Tools > Serial Monitor at **115200** baud, type `status`.

Prefer not to put passwords in a file? Skip step 4 and type them into the
Serial Monitor instead. They are saved on the board:

```
wifi "My WiFi" mypassword
printer 192.168.1.50 12345678 01P00A000000000
```

`arduino-cli` instead of the IDE:

```bash
arduino-cli core install esp32:esp32
arduino-cli lib install PubSubClient
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc firmware/mqtt_timelapse
arduino-cli upload -p COM5 --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc firmware/mqtt_timelapse
```

A good boot looks like:

```
=== MQTT Timelapse 1.0.0 ===
WiFi settings: config.h | printer settings: config.h
[TL] Bluetooth shutter up as "Timelapse Shutter", bonds 0, advertising (heap 245584 -> 169240)
[WiFi] connected, IP 192.168.1.92, signal -61 dBm
[MQTT] connected, listening on device/01P00A000000000/report
[PRINTER] state IDLE
```

## 2. Pair the phone and set up the camera

1. Phone Bluetooth settings > pair new device > **Timelapse Shutter**. No PIN.
2. Type `tl shot` in the Serial Monitor. Your phone's volume should go up.
3. **Open Camera** (Android): Settings > More camera controls > Volume keys:
   **Take photo**. Then lock focus on the part, lock exposure and white balance,
   turn off the shutter sound and "Pause after taking photo".
4. Phone: screen stays on while charging (Developer options > Stay awake),
   battery for the camera app set to unrestricted, Do Not Disturb on.
5. `tl shot` again: exactly one photo. Reboot the board (`reboot`): the phone
   reconnects by itself within seconds, no pairing.

### Or: a Sony camera instead of the phone

Sony cameras with a **Bluetooth Rmt Ctrl** setting (for example the ZV-E10 II,
a6400 or a7 IV) can be the camera. The board then acts as Sony's
Bluetooth remote. Sony does not publish this protocol; the values come from the
community's reverse engineering ([furble](https://github.com/gkoh/furble),
[Greg Leeds](https://gregleeds.com/reverse-engineering-sony-camera-bluetooth/)).

1. `config.h`: `#define SHUTTER "sony"`, or type `tl shutter sony` and `reboot`.
2. Camera: Menu > Network > Bluetooth: **Bluetooth Function On**,
   **Bluetooth Rmt Ctrl On**, then **Pairing**.
3. The board finds the camera (`[CAM] Sony camera ... connecting`); confirm the
   pairing on the camera screen. `[CAM] camera READY` = done. From then on it
   reconnects by itself whenever the camera is on.
4. `tl shot`: the camera takes a photo.
5. Camera settings for a timelapse: manual focus, photo mode, auto review off,
   power-save start time as long as possible, powered over USB-C, SD card with room.

## 3. Prepare the print

1. Bambu Studio: Others > Special mode > **Timelapse: Smooth**. This parks the
   head at the side for each frame (it adds a prime tower, keep it).
2. Slice, then **File > Export > Export plate sliced file** (`.gcode.3mf`).
3. Run the script:

```bash
python tools/inject_timelapse_markers.py my_part.gcode.3mf my_part_tl.gcode.3mf --dwell 8
```

It prints what it found (layers, timelapse frames, bed temperatures), the first
three insertions and the extra print time (8 s × layers). Without an output
file it only inspects. It refuses files it does not understand rather than
guessing where to put the marker.

4. Open `my_part_tl.gcode.3mf` in Bambu Studio and **print it from the preview
   without re-slicing** (re-slicing throws the markers away), or copy it to the SD card.
5. In the print dialog switch **Timelapse ON**. With it off the printer skips
   the whole timelapse block, and the markers with it.

## 4. Print and make the video

The board logs every photo with its layer:

```
[TL] t=812345 SHOT 57 layer 57
[TL] print ended (FINISH): 212 photos, 212 markers, 0 missed
```

Copy the photos off the phone and stitch them (30 photos = 1 second):

```bash
ffmpeg -framerate 30 -pattern_type glob -i '*.jpg' -c:v libx264 -pix_fmt yuv420p timelapse.mp4
```

(`-pattern_type glob` needs macOS, Linux, Git Bash or WSL.)

## Commands (Serial Monitor, 115200 baud)

| Command | |
|---|---|
| `status` | WiFi, printer, last report, layer, shutter, memory |
| `wifi "<name>" <password>` | save WiFi (quotes only for names with spaces) |
| `printer <ip> <access code> <serial>` | save the printer |
| `forget` | drop saved settings, back to `config.h` |
| `scan` | list the 2.4 GHz networks the board sees |
| `dump on` / `dump off` | print every raw printer report (for adapting to new printers) |
| `pushall` | ask the printer for a full report |
| `tl shot` | press the shutter once |
| `tl shutter phone\|sony` | phone keyboard or Sony camera remote (reboot to apply) |
| `tl on` / `tl off` | take photos on markers, or pause |
| `tl status` | phone link, photos this print, last marker |
| `tl interval <ms>` | minimum time between photos (default 1500) |
| `tl delta <n>` | marker step in °C (default 1, must match `--delta`) |
| `tl logtemps on\|off` | log every bed target change |
| `tl name <text>` | Bluetooth name (reboot to apply) |
| `tl unpair` | delete all Bluetooth bonds on the board (and the saved camera) |
| `reboot` | |

## Choosing the dwell (do this once)

The head has to stay parked until the photo is taken: network delay + about
0.3 s Bluetooth + about 0.5 s for the phone to shoot + margin.

1. Slice a short test part and run the script with `--dwell 5 --every 10`
   (a marker on every 10th layer only).
2. The board's LED blinks 150 ms on every marker (`MARKER_LED` in `config.h`).
   Film the LED and the toolhead together at 60 fps. The frames between "head
   parked" and "LED blinks" are your delay.
3. dwell = worst delay + 1.3 s, rounded up to whole seconds, plus margin.
4. Every marked layer should show a `SHOT` line.

Measured on a P1S (449-layer print, 5 s dwell): the phone fired within 0.3 s
of the board, but the printer's reports arrive in roughly 2 s steps, so the
marker sometimes reached the board late. About 1 frame in 10 caught the head
already leaving the park. **Use `--dwell 8` on a P1S.** 8 s × 300 layers adds
40 minutes. Check your own prints: a frame with the head blurred or moved
toward the prime tower means the dwell is too short.

## How the marker is detected

- Base = the last bed target held for at least 1 s.
- A marker is base + delta (±0.3 °C), or +delta from the value just before it.
  The script adds a 3 s pause before the marker on layers where the bed target
  itself just changed (usually layer 2), so the board has learned the new base.
- It fires once while the printer state is `RUNNING`, at least `tl interval`
  after the previous photo, and re-arms when the target returns to base.
- Any other change (heat-up, first layer → second, cool-down) just moves the base.
- If your print really changes the bed target by exactly +1 °C somewhere, the
  script warns you: use `--delta 2` and `tl delta 2`.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `[WiFi] network not found` | 2.4 GHz only. Type `scan`: if the list is empty, the board's antenna is the problem, not the password. |
| `[WiFi] the router rejected the password` | Re-enter it with `wifi`. |
| `[MQTT] connect failed, rc=-2` | Printer not reachable: IP, LAN mode, same network. After many quick reboots the printer can refuse for a minute; it retries by itself. |
| `[MQTT] connect failed, rc=5` | Wrong access code: check it on the printer screen again. |
| `connected but no report yet` | Wrong serial number. |
| `tl shot` raises the volume but no photo | Camera app is not in front, or its volume key is not set to take a photo. |
| `pairing FAILED` | The phone still has an old bond. Forget the device on the phone, pair again. |
| `[CAM] waiting for a camera in pairing mode` | Sony: Menu > Network > Bluetooth > Pairing, with Bluetooth Rmt Ctrl On. |
| `[CAM] ... Bluetooth Rmt Ctrl is off` | Turn it on in Menu > Network > Bluetooth. |
| `the camera did not accept the shutter press` | Camera asleep, in playback or menu, or the card is full. |
| No `SHOT` lines during a print | Timelapse was off in the print dialog, or the file was re-sliced. |
| Blurry frames | Lock focus and exposure; raise the dwell. |

## Adapt it with Claude Code

This repo is set up so [Claude Code](https://claude.com/claude-code) can change
it for your setup: `CLAUDE.md` tells it how the parts fit together and what to
check. Clone the repo, open it in Claude Code and ask, for example:

- "My printer is an A1 mini. Check my sliced file and adapt the script if needed."
- "I have an ESP32-C3 SuperMini. Change the LED and give me the upload command."
- "My camera app needs Volume Down instead of Volume Up."
- "I have a Prusa with PrusaLink. Make the firmware read the bed target from it."

Put your WiFi password and access code into `config.h` or the Serial Monitor
yourself; Claude does not need them.

## Tested with

ESP32-S3 (FarmBoard 2) · esp32 core 3.3.8 · PubSubClient 2.8 · Bambu Lab P1S in
LAN-only mode · Android with Open Camera.

Made by 3D Farmers. MIT licensed, see `LICENSE`. You change the G-code your
printer runs at your own risk: inspect the script's output before you print.
