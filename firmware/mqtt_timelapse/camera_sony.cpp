// camera_sony - see camera_sony.h.
#include "camera_sony.h"

#include <atomic>
#include <Preferences.h>
#include <freertos/semphr.h>
#include <host/ble_hs.h>

namespace Timelapse {
namespace Sony {
namespace {

// --- Protocol (reverse engineered, see camera_sony.h) -----------------------
// Advertisement: manufacturer data = company 0x012D (Sony), type 0x0003
// (camera), protocol, model, then tag 0x22 and a mode byte with these bits.
const uint16_t SONY_COMPANY_ID   = 0x012D;
const uint16_t SONY_TYPE_CAMERA  = 0x0003;
const uint8_t  MODE_PAIRING_SUPPORTED = 1 << 7;
const uint8_t  MODE_PAIRING_ENABLED   = 1 << 6;   // camera is in Bluetooth > Pairing
const uint8_t  MODE_REMOTE_ENABLED    = 1 << 1;   // "Bluetooth Rmt Ctrl" is On

// Remote control service 8000ff00-ff00-ffff-ffff-ffffffffffff and its write
// characteristic 0xFF01 (NimBLE wants 128-bit UUIDs little-endian).
const ble_uuid128_t CTRL_SVC = BLE_UUID128_INIT(0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                                0xff, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0x80);
const uint16_t CTRL_CHR = 0xFF01;
const ble_uuid128_t CTRL_CHR_128 = BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                                                    0x00, 0x10, 0x00, 0x00, 0x01, 0xff, 0x00, 0x00);

// Button codes, sent as these two bytes. A photo = focus down, shutter down,
// shutter up, focus up: without the focus press first the camera ignores
// about two shutter presses in three (Greg Leeds). Harmless in manual focus.
const uint8_t FOCUS_DOWN[]   = {0x01, 0x07};
const uint8_t FOCUS_UP[]     = {0x01, 0x06};
const uint8_t SHUTTER_DOWN[] = {0x01, 0x09};
const uint8_t SHUTTER_UP[]   = {0x01, 0x08};
const uint32_t SHUTTER_HOLD_MS  = 100;
const uint32_t WRITE_TIMEOUT_MS = 1000;

// Scanning shares the radio with WiFi: 30 ms of every 100 ms, in 10 s rounds
// (a new round also catches a camera that just switched to pairing mode).
const int32_t SCAN_ROUND_MS = 10000;
const unsigned long RETRY_MS = 2000;
const unsigned long HINT_MS  = 60000;

enum State : uint8_t { IDLE, SCANNING, CONNECTING, SECURING, DISCOVERING, READY };
const char* const STATE_NAMES[] = {"idle", "scanning", "connecting", "pairing / encrypting",
                                   "looking for the remote service", "ready"};

Print*      g_log = nullptr;
const char* g_ns  = "mqtt_tl";
uint8_t     g_ownAddrType = BLE_OWN_ADDR_PUBLIC;

// Shared with the NimBLE host task, which runs every callback below.
std::atomic<uint8_t> g_state{IDLE};
std::atomic<bool>    g_synced{false};
uint16_t   g_conn = BLE_HS_CONN_HANDLE_NONE;
uint16_t   g_svcStart = 0, g_svcEnd = 0, g_ctrl = 0;
ble_addr_t g_saved{};            // the camera paired last time (NVS "tl_cam")
bool       g_haveSaved = false;
ble_addr_t g_peer{};             // the camera being connected
SemaphoreHandle_t g_writeDone = nullptr;
std::atomic<int>  g_writeStatus{0};

// Events for loop() to log: callbacks only count, the main task prints.
std::atomic<uint8_t> evFound{0}, evConnected{0}, evConnectFailed{0}, evEncrypted{0},
                     evEncFailed{0}, evReady{0}, evNoService{0}, evDisconnected{0},
                     evConfirm{0}, evRemoteOff{0};
std::atomic<int>      g_lastStatus{0};
std::atomic<uint32_t> g_code{0};
std::atomic<bool>     g_saveNeeded{false};
unsigned long g_nextScanAt = 0, g_lastHint = 0;

String addrStr(const ble_addr_t& a) {
  char s[18];
  snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X",
           a.val[5], a.val[4], a.val[3], a.val[2], a.val[1], a.val[0]);
  return String(s);
}

void loadSaved() {
  uint8_t b[7];
  Preferences p;
  p.begin(g_ns, true);
  g_haveSaved = p.isKey("tl_cam") && p.getBytes("tl_cam", b, sizeof(b)) == sizeof(b);
  p.end();
  if (g_haveSaved) {
    g_saved.type = b[0];
    memcpy(g_saved.val, b + 1, 6);
  }
}

void saveCamera(const ble_addr_t& a) {
  uint8_t b[7] = {a.type};
  memcpy(b + 1, a.val, 6);
  Preferences p;
  p.begin(g_ns, false);
  p.putBytes("tl_cam", b, sizeof(b));
  p.end();
  g_saved = a;
  g_haveSaved = true;
}

// True if this is a Sony camera's manufacturer data; *mode = its mode byte.
bool parseSony(const uint8_t* d, uint8_t n, uint8_t* mode) {
  if (!d || n < 13) return false;
  if ((uint16_t)(d[0] | d[1] << 8) != SONY_COMPANY_ID) return false;
  if ((uint16_t)(d[2] | d[3] << 8) != SONY_TYPE_CAMERA) return false;
  if (d[8] != 0x22) return false;   // tag in front of the mode byte
  *mode = d[9];
  return true;
}

bool isCtrlChr(const ble_uuid_t* u) {
  return ble_uuid_u16(u) == CTRL_CHR || ble_uuid_cmp(u, &CTRL_CHR_128.u) == 0;
}

int gapEvent(ble_gap_event* ev, void*);

void startScan() {
  ble_gap_disc_params dp{};
  dp.itvl = BLE_GAP_SCAN_ITVL_MS(100);
  dp.window = BLE_GAP_SCAN_WIN_MS(30);
  dp.passive = 0;
  dp.filter_duplicates = 1;
  if (ble_gap_disc(g_ownAddrType, SCAN_ROUND_MS, &dp, gapEvent, nullptr) == 0) g_state = SCANNING;
}

void connectTo(const ble_addr_t& a) {
  ble_gap_disc_cancel();
  g_peer = a;
  ble_gap_conn_params cp{};
  cp.scan_itvl = 16;
  cp.scan_window = 16;
  cp.itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN;
  cp.itvl_max = BLE_GAP_INITIAL_CONN_ITVL_MAX;
  cp.latency = 0;
  cp.supervision_timeout = BLE_GAP_SUPERVISION_TIMEOUT_MS(4000);   // WiFi shares the radio
  if (ble_gap_connect(g_ownAddrType, &a, 10000, &cp, gapEvent, nullptr) == 0) {
    g_state = CONNECTING;
  } else {
    g_state = IDLE;
    evConnectFailed++;
  }
}

void hangUp(uint16_t conn) { ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM); }

int onChr(uint16_t conn, const ble_gatt_error* err, const ble_gatt_chr* chr, void*) {
  if (err->status == 0 && chr) {
    if (isCtrlChr(&chr->uuid.u)) g_ctrl = chr->val_handle;
    return 0;
  }
  if (err->status == BLE_HS_EDONE && g_ctrl) {
    g_state = READY;
    evReady++;
    if (!g_haveSaved || ble_addr_cmp(&g_saved, &g_peer) != 0) g_saveNeeded = true;
  } else {
    evNoService++;
    hangUp(conn);
  }
  return 0;
}

int onSvc(uint16_t conn, const ble_gatt_error* err, const ble_gatt_svc* svc, void*) {
  if (err->status == 0 && svc) {
    g_svcStart = svc->start_handle;
    g_svcEnd = svc->end_handle;
    return 0;
  }
  if (err->status == BLE_HS_EDONE && g_svcEnd) {
    g_ctrl = 0;
    ble_gattc_disc_all_chrs(conn, g_svcStart, g_svcEnd, onChr, nullptr);
  } else {
    evNoService++;
    hangUp(conn);
  }
  return 0;
}

// NimBLE host task context: state changes and counters only.
int gapEvent(ble_gap_event* ev, void*) {
  switch (ev->type) {
    case BLE_GAP_EVENT_DISC: {
      if (g_state != SCANNING) return 0;
      ble_hs_adv_fields f;
      if (ble_hs_adv_parse_fields(&f, ev->disc.data, ev->disc.length_data) != 0) return 0;
      uint8_t mode = 0;
      if (!parseSony(f.mfg_data, f.mfg_data_len, &mode)) return 0;
      bool known    = g_haveSaved && ble_addr_cmp(&ev->disc.addr, &g_saved) == 0;
      bool pairable = (mode & (MODE_PAIRING_SUPPORTED | MODE_PAIRING_ENABLED)) ==
                      (MODE_PAIRING_SUPPORTED | MODE_PAIRING_ENABLED);
      if (!(mode & MODE_REMOTE_ENABLED)) { evRemoteOff++; return 0; }
      // The camera from last time, or any camera the user put in pairing mode.
      if (!known && !pairable) return 0;
      evFound++;
      connectTo(ev->disc.addr);
      return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
      if (g_state == SCANNING) g_state = IDLE;
      return 0;
    case BLE_GAP_EVENT_CONNECT:
      if (ev->connect.status != 0) {
        g_lastStatus = ev->connect.status;
        g_state = IDLE;
        evConnectFailed++;
        return 0;
      }
      g_conn = ev->connect.conn_handle;
      g_state = SECURING;
      evConnected++;
      ble_gap_security_initiate(g_conn);   // pairs the first time, re-encrypts after
      return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
      // The camera asks to confirm the pairing on its screen; accept on ours.
      ble_sm_io io{};
      io.action = ev->passkey.params.action;
      if (io.action == BLE_SM_IOACT_NUMCMP) {
        io.numcmp_accept = 1;
        g_code = ev->passkey.params.numcmp;
      } else if (io.action == BLE_SM_IOACT_DISP) {
        io.passkey = 123456;               // in case the camera asks to type a code
        g_code = io.passkey;
      } else {
        return 0;
      }
      evConfirm++;
      ble_sm_inject_io(ev->passkey.conn_handle, &io);
      return 0;
    }
    case BLE_GAP_EVENT_ENC_CHANGE:
      if (ev->enc_change.status == 0) {
        g_state = DISCOVERING;
        evEncrypted++;
        g_svcStart = g_svcEnd = 0;
        ble_gattc_disc_svc_by_uuid(ev->enc_change.conn_handle, &CTRL_SVC.u, onSvc, nullptr);
      } else {
        g_lastStatus = ev->enc_change.status;
        evEncFailed++;
        // Key missing on the camera's side (it forgot the board): drop ours
        // so the next attempt pairs fresh.
        if (ev->enc_change.status == BLE_HS_HCI_ERR(0x06)) {
          ble_gap_conn_desc d;
          if (ble_gap_conn_find(ev->enc_change.conn_handle, &d) == 0)
            ble_store_util_delete_peer(&d.peer_id_addr);
        }
        hangUp(ev->enc_change.conn_handle);
      }
      return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      ble_gap_conn_desc d;
      if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0)
        ble_store_util_delete_peer(&d.peer_id_addr);
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_DISCONNECT:
      g_conn = BLE_HS_CONN_HANDLE_NONE;
      g_ctrl = 0;
      g_lastStatus = ev->disconnect.reason;
      g_state = IDLE;
      evDisconnected++;
      return 0;
  }
  return 0;
}

int onWrite(uint16_t, const ble_gatt_error* err, ble_gatt_attr*, void*) {
  g_writeStatus = err->status;
  xSemaphoreGive(g_writeDone);
  return 0;
}

// One button code, written with response. Blocks the shutter task only.
bool write(const uint8_t* cmd) {
  if (g_state != READY) return false;
  xSemaphoreTake(g_writeDone, 0);   // drop a late answer from a timed-out write
  if (ble_gattc_write_flat(g_conn, g_ctrl, cmd, 2, onWrite, nullptr) != 0) return false;
  if (xSemaphoreTake(g_writeDone, pdMS_TO_TICKS(WRITE_TIMEOUT_MS)) != pdTRUE) return false;
  return g_writeStatus == 0;
}

}  // namespace

void begin(Print& log, const char* nvsNamespace) {
  g_log = &log;
  g_ns = nvsNamespace;
  if (!g_writeDone) g_writeDone = xSemaphoreCreateBinary();
  loadSaved();
  if (g_haveSaved)
    g_log->printf("[CAM] Sony mode, saved camera %s\n", addrStr(g_saved).c_str());
  else
    g_log->println("[CAM] Sony mode, no camera yet - camera: Menu > Network > Bluetooth > Pairing");
}

void onSync() {
  ble_hs_id_infer_auto(0, &g_ownAddrType);
  g_synced = true;
}

void onReset() {
  g_synced = false;
  g_state = IDLE;
  g_conn = BLE_HS_CONN_HANDLE_NONE;
}

void loop() {
  if (!g_log) return;
  unsigned long now = millis();
  static uint8_t found = 0, conn = 0, cfail = 0, enc = 0, encf = 0, rdy = 0, nosvc = 0,
                 disc = 0, confirm = 0, remoteOff = 0;

  if (evFound != found) {
    found = evFound;
    g_log->printf("[CAM] t=%lu Sony camera %s - connecting\n", now, addrStr(g_peer).c_str());
  }
  if (evConnectFailed != cfail) {
    cfail = evConnectFailed;
    g_log->printf("[CAM] t=%lu connect failed (status %d), trying again\n", now, g_lastStatus.load());
    g_nextScanAt = now + RETRY_MS;
  }
  if (evConnected != conn) {
    conn = evConnected;
    g_log->printf("[CAM] t=%lu connected, securing the link\n", now);
  }
  if (evConfirm != confirm) {
    confirm = evConfirm;
    g_log->printf("[CAM] t=%lu pairing: confirm on the camera (code %06lu)\n", now,
                  (unsigned long)g_code.load());
  }
  if (evEncrypted != enc) {
    enc = evEncrypted;
    g_log->printf("[CAM] t=%lu link encrypted\n", now);
  }
  if (evEncFailed != encf) {
    encf = evEncFailed;
    g_log->printf("[CAM] t=%lu pairing FAILED (status 0x%X). Put the camera in Menu > Network > "
                  "Bluetooth > Pairing and wait for the next try\n", now, g_lastStatus.load());
  }
  if (evNoService != nosvc) {
    nosvc = evNoService;
    g_log->printf("[CAM] t=%lu the camera offers no remote service - is Bluetooth Rmt Ctrl on?\n", now);
  }
  if (evReady != rdy) {
    rdy = evReady;
    g_log->printf("[CAM] t=%lu camera READY - tl shot takes a photo\n", now);
  }
  if (evDisconnected != disc) {
    disc = evDisconnected;
    g_log->printf("[CAM] t=%lu camera disconnected (reason 0x%X), looking for it again\n", now,
                  g_lastStatus.load());
    g_nextScanAt = now + RETRY_MS;
  }
  if (evRemoteOff != remoteOff && now - g_lastHint > HINT_MS) {
    remoteOff = evRemoteOff;
    g_lastHint = now;
    g_log->println("[CAM] a Sony camera is nearby but Bluetooth Rmt Ctrl is off "
                   "(Menu > Network > Bluetooth)");
  }
  if (g_saveNeeded) {
    g_saveNeeded = false;
    saveCamera(g_peer);
    g_log->printf("[CAM] camera %s saved - it reconnects by itself from now on\n",
                  addrStr(g_peer).c_str());
  }

  if (g_synced && g_state == IDLE && (long)(now - g_nextScanAt) >= 0) startScan();

  if (g_state != READY && now - g_lastHint > HINT_MS) {
    g_lastHint = now;
    g_log->println(g_haveSaved
      ? "[CAM] waiting for the camera: switched on, Bluetooth Rmt Ctrl on?"
      : "[CAM] waiting for a camera in pairing mode: Menu > Network > Bluetooth > Pairing");
  }
}

bool ready() { return g_state == READY; }

bool shoot() {
  bool pressed = write(FOCUS_DOWN) && write(SHUTTER_DOWN);
  vTaskDelay(pdMS_TO_TICKS(SHUTTER_HOLD_MS));
  // Always release, even after a failed press: never leave the camera half-pressed.
  bool released = write(SHUTTER_UP);
  released = write(FOCUS_UP) && released;
  return pressed && released;
}

void status(Print& out) {
  out.printf("[CAM] Sony: %s | camera %s | saved %s\n", STATE_NAMES[g_state.load()],
             g_state == IDLE || g_state == SCANNING ? "-" : addrStr(g_peer).c_str(),
             g_haveSaved ? addrStr(g_saved).c_str() : "none");
}

void forget() {
  Preferences p;
  p.begin(g_ns, false);
  p.remove("tl_cam");
  p.end();
  g_haveSaved = false;
  if (g_conn != BLE_HS_CONN_HANDLE_NONE) hangUp(g_conn);
}

}  // namespace Sony
}  // namespace Timelapse
