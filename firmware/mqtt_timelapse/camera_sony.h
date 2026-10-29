// =============================================================================
// camera_sony - a Sony camera as the shutter, over its Bluetooth remote
// =============================================================================
// Sony cameras with a "Bluetooth Rmt Ctrl" setting (ZV-E10 II, a6400, a7 IV,
// ...) accept the same Bluetooth remote protocol as Sony's RMT-P1BT. The board
// plays that remote: it finds the camera, pairs once, and sends focus and
// shutter presses. Here the board is the Bluetooth CENTRAL (it connects to the
// camera), the opposite of phone mode, where the phone connects to the board.
//
// Sony does not publish the protocol. The values used here come from the
// community's reverse engineering: furble (github.com/gkoh/furble, MIT) and
// Greg Leeds (gregleeds.com/reverse-engineering-sony-camera-bluetooth).
//
// Internal to the timelapse module: timelapse.cpp starts the NimBLE host,
// sets the security for this mode and calls in here.
// =============================================================================
#pragma once
#include <Arduino.h>

namespace Timelapse {
namespace Sony {

void begin(Print& log, const char* nvsNamespace);
void onSync();     // NimBLE host synced: scanning may start
void onReset();    // NimBLE host reset: every link is gone
void loop();       // main task: scan scheduling, log lines, saving the camera
bool ready();      // connected, encrypted, remote characteristic found
bool shoot();      // shutter task only: focus, shutter down, hold, release
void status(Print& out);
void forget();     // drop the saved camera (the caller clears the bonds)

}  // namespace Sony
}  // namespace Timelapse
