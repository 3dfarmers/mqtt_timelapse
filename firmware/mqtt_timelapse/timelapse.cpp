// timelapse - see timelapse.h for the contract.
#include "timelapse.h"

#include <atomic>
#include <math.h>
#include <Preferences.h>
// NimBLE host API straight from the core. The Arduino BLE wrapper would cost
// ~30 KB more flash for client, scan and bookkeeping a HID peripheral never uses.
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_hs.h>
#include <host/util/util.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

extern "C" void ble_store_config_init(void);   // NimBLE NVS bond store, no public header

namespace Timelapse {
namespace {

// --- Marker detection -------------------------------------------------------
// A new bed target becomes the base once it has been held this long. Filters
// heat-up, the first-layer -> second-layer change and the cool-down.
const unsigned long BASE_HOLD_MS = 1000;
// A "marker" value held this long was a real setpoint (someone set base+1 on
// the screen), not a marker. Promote it so detection does not stay disarmed.
// No dwell comes anywhere near this.
const unsigned long MARKER_STUCK_MS = 30000;
const float MARKER_TOL = 0.3f;
const float SAME_TOL   = 0.05f;  // X1 resends the full state; repeats are not changes

// --- HID --------------------------------------------------------------------
// The key the "photo" press sends (HID Consumer page). Volume Up is tested
// with Open Camera on Android; most camera apps (iPhone Camera, Samsung,
// Google Camera) also shoot on a volume key. Other options: 0x00EA Volume
// Down, 0x00CD Play/Pause.
const uint16_t SHUTTER_USAGE = 0x00E9;   // Volume Up
const uint32_t KEY_PRESS_MS  = 50;
const unsigned long ADV_CHECK_MS = 2000;

// Consumer Control, one 16-bit usage per report (array). Press = usage,
// release = 0. Report ID 1 lives in the Report Reference descriptor, not in
// the value (HOGP).
const uint8_t REPORT_MAP[] = {
  0x05, 0x0C,        // Usage Page (Consumer)
  0x09, 0x01,        // Usage (Consumer Control)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x01,        //   Report ID (1)
  0x19, 0x00,        //   Usage Minimum (0)
  0x2A, 0x3C, 0x02,  //   Usage Maximum (0x023C)
  0x15, 0x00,        //   Logical Minimum (0)
  0x26, 0x3C, 0x02,  //   Logical Maximum (0x023C)
  0x75, 0x10,        //   Report Size (16)
  0x95, 0x01,        //   Report Count (1)
  0x81, 0x00,        //   Input (Data, Array, Absolute)
  0xC0               // End Collection
};

Print*      g_log = nullptr;
const char* g_ns  = "mqtt_tl";
Config      g_cfg;
void (*g_hook)() = nullptr;

// Detection state. Main task only (onBedTarget runs inside the sketch's MQTT
// callback, which runs from loop()).
float g_cur  = NAN;            // last bed target seen
float g_base = NAN;            // last bed target held >= BASE_HOLD_MS
unsigned long g_curSince = 0;
bool  g_armed   = true;        // cleared by a marker, set by a return to base
bool  g_running = false;
char  g_state[16] = "unknown";

// Per print, reset when a print starts or by `tl reset`.
uint32_t g_shots = 0, g_markers = 0, g_missed = 0;
uint32_t g_shotsSinceBoot = 0;
bool g_haveShot = false;
unsigned long g_lastShotMs = 0, g_lastMarkerMs = 0;
int g_lastShotLayer = -1, g_lastMarkerLayer = -1;

// BLE. GAP events run on the NimBLE host task and only bump these counters;
// loop() turns them into log lines on the main task.
bool g_bleUp = false;
TaskHandle_t g_shutterTask = nullptr;
uint16_t g_reportHandle = 0;          // filled in by ble_gatts_add_svcs()
uint8_t  g_ownAddrType = BLE_OWN_ADDR_PUBLIC;
uint8_t  g_report[2] = {0, 0};        // current input report (written by the shutter task)
uint8_t  g_protoMode = 0x01;          // report protocol
std::atomic<bool> g_synced{false}, g_subscribed{false};
std::atomic<int>  g_links{0}, g_discReason{0};
std::atomic<uint8_t> g_connEv{0}, g_discEv{0}, g_encOkEv{0}, g_encFailEv{0}, g_resetEv{0};

bool isActive(const char* s) {
  return !strcmp(s, "RUNNING") || !strcmp(s, "PAUSE") ||
         !strcmp(s, "PREPARE") || !strcmp(s, "SLICING");
}

void resetPrintCounters() {
  g_shots = g_markers = g_missed = 0;
  g_lastShotLayer = g_lastMarkerLayer = -1;
}

// ---------------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------------
void loadConfig() {
  const Config d = g_cfg;   // compiled defaults (+ the sketch's default name)
  Preferences p;
  p.begin(g_ns, true);
  g_cfg.enabled       = p.getBool("tl_enabled", d.enabled);
  g_cfg.minIntervalMs = p.getUShort("tl_min_int_ms", d.minIntervalMs);
  g_cfg.markerDelta   = p.getUChar("tl_marker_delta", d.markerDelta);
  g_cfg.logTemps      = p.getBool("tl_log_temps", d.logTemps);
  char name[sizeof(g_cfg.bleName)];
  if (p.isKey("tl_ble_name") && p.getString("tl_ble_name", name, sizeof(name)) > 0 && name[0])
    strlcpy(g_cfg.bleName, name, sizeof(g_cfg.bleName));
  p.end();
  if (g_cfg.markerDelta < 1) g_cfg.markerDelta = 1;
}

void saveConfig() {
  Preferences p;
  p.begin(g_ns, false);
  p.putBool("tl_enabled", g_cfg.enabled);
  p.putUShort("tl_min_int_ms", g_cfg.minIntervalMs);
  p.putUChar("tl_marker_delta", g_cfg.markerDelta);
  p.putString("tl_ble_name", g_cfg.bleName);
  p.putBool("tl_log_temps", g_cfg.logTemps);
  p.end();
}

// ---------------------------------------------------------------------------
// BLE HID
// ---------------------------------------------------------------------------
// HOGP device: Device Information + Battery + HID service with one Consumer
// Control input report. Every read goes through gattAccess(); `arg` says
// which attribute is being read.
enum : uintptr_t {
  A_HID_INFO, A_REPORT_MAP, A_CTRL_POINT, A_PROTO_MODE, A_REPORT, A_REPORT_REF,
  A_MANUF, A_PNP, A_BATTERY
};
const uint16_t APPEARANCE_KEYBOARD = 0x03C1;
const uint8_t  HID_INFO[]   = {0x11, 0x01, 0x00, 0x01};  // HID 1.11, country 0, remote wake
const uint8_t  REPORT_REF[] = {0x01, 0x01};              // report ID 1, input
const uint8_t  PNP_ID[]     = {0x01, 0xE5, 0x02, 0x01, 0x00, 0x00, 0x01};  // BT SIG: Espressif, pid 1, v1.0
const uint8_t  BATTERY[]    = {100};
const char     MANUFACTURER[] = "mqtt_timelapse";

int gattAccess(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void* arg) {
  const void* data = nullptr;
  uint16_t len = 0;
  switch ((uintptr_t)arg) {
    case A_HID_INFO:   data = HID_INFO;     len = sizeof(HID_INFO);     break;
    case A_REPORT_MAP: data = REPORT_MAP;   len = sizeof(REPORT_MAP);   break;
    case A_REPORT:     data = g_report;     len = sizeof(g_report);     break;
    case A_REPORT_REF: data = REPORT_REF;   len = sizeof(REPORT_REF);   break;
    case A_MANUF:      data = MANUFACTURER; len = sizeof(MANUFACTURER) - 1; break;
    case A_PNP:        data = PNP_ID;       len = sizeof(PNP_ID);       break;
    case A_BATTERY:    data = BATTERY;      len = sizeof(BATTERY);      break;
    case A_PROTO_MODE:
      if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (OS_MBUF_PKTLEN(ctxt->om) >= 1) os_mbuf_copydata(ctxt->om, 0, 1, &g_protoMode);
        return 0;
      }
      data = &g_protoMode; len = 1;
      break;
    case A_CTRL_POINT:
      return 0;   // host suspend / exit suspend: nothing to do
  }
  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR || ctxt->op == BLE_GATT_ACCESS_OP_WRITE_DSC)
    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
  return os_mbuf_append(ctxt->om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

#define TL_ARG(a) reinterpret_cast<void*>(static_cast<uintptr_t>(a))
const ble_uuid16_t U_DIS = BLE_UUID16_INIT(0x180A), U_MANUF = BLE_UUID16_INIT(0x2A29),
                   U_PNP = BLE_UUID16_INIT(0x2A50), U_BAS = BLE_UUID16_INIT(0x180F),
                   U_BATT = BLE_UUID16_INIT(0x2A19), U_HID = BLE_UUID16_INIT(0x1812),
                   U_HID_INFO = BLE_UUID16_INIT(0x2A4A), U_REPORT_MAP = BLE_UUID16_INIT(0x2A4B),
                   U_CTRL = BLE_UUID16_INIT(0x2A4C), U_PROTO = BLE_UUID16_INIT(0x2A4E),
                   U_REPORT = BLE_UUID16_INIT(0x2A4D), U_REPORT_REF = BLE_UUID16_INIT(0x2908);

ble_gatt_dsc_def g_reportDscs[] = {
  {.uuid = &U_REPORT_REF.u, .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
   .access_cb = gattAccess, .arg = TL_ARG(A_REPORT_REF)},
  {},
};
// Report Map and Report need an encrypted link: the phone pairs on first
// access, and only a bonded phone can take photos.
const ble_gatt_chr_def g_hidChrs[] = {
  {.uuid = &U_HID_INFO.u, .access_cb = gattAccess, .arg = TL_ARG(A_HID_INFO),
   .flags = BLE_GATT_CHR_F_READ},
  {.uuid = &U_REPORT_MAP.u, .access_cb = gattAccess, .arg = TL_ARG(A_REPORT_MAP),
   .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
  {.uuid = &U_CTRL.u, .access_cb = gattAccess, .arg = TL_ARG(A_CTRL_POINT),
   .flags = BLE_GATT_CHR_F_WRITE_NO_RSP},
  {.uuid = &U_PROTO.u, .access_cb = gattAccess, .arg = TL_ARG(A_PROTO_MODE),
   .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP},
  {.uuid = &U_REPORT.u, .access_cb = gattAccess, .arg = TL_ARG(A_REPORT),
   .descriptors = g_reportDscs,
   .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
   .val_handle = &g_reportHandle},
  {},
};
const ble_gatt_chr_def g_disChrs[] = {
  {.uuid = &U_MANUF.u, .access_cb = gattAccess, .arg = TL_ARG(A_MANUF), .flags = BLE_GATT_CHR_F_READ},
  {.uuid = &U_PNP.u, .access_cb = gattAccess, .arg = TL_ARG(A_PNP), .flags = BLE_GATT_CHR_F_READ},
  {},
};
const ble_gatt_chr_def g_basChrs[] = {
  {.uuid = &U_BATT.u, .access_cb = gattAccess, .arg = TL_ARG(A_BATTERY),
   .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY},
  {},
};
const ble_gatt_svc_def g_services[] = {
  {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &U_DIS.u, .characteristics = g_disChrs},
  {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &U_BAS.u, .characteristics = g_basChrs},
  {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &U_HID.u, .characteristics = g_hidChrs},
  {},
};

int gapEvent(ble_gap_event* ev, void*);

// Callable from any task (NimBLE serialises host calls with its own lock).
void startAdvertising() {
  if (!g_synced) return;
  static const ble_uuid16_t hidUuid = BLE_UUID16_INIT(0x1812);
  size_t nameLen = strlen(g_cfg.bleName);

  ble_hs_adv_fields adv{};
  adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  adv.appearance = APPEARANCE_KEYBOARD;
  adv.appearance_is_present = 1;
  adv.uuids16 = &hidUuid;
  adv.num_uuids16 = 1;
  adv.uuids16_is_complete = 1;
  if (nameLen <= 18) {   // 31-byte packet: flags 3 + appearance 4 + UUID 4 + name 2+n
    adv.name = (const uint8_t*)g_cfg.bleName;
    adv.name_len = nameLen;
    adv.name_is_complete = 1;
  }
  ble_hs_adv_fields rsp{};   // the full name always rides in the scan response
  rsp.name = (const uint8_t*)g_cfg.bleName;
  rsp.name_len = nameLen;
  rsp.name_is_complete = 1;

  ble_gap_adv_params p{};
  p.conn_mode = BLE_GAP_CONN_MODE_UND;
  p.disc_mode = BLE_GAP_DISC_MODE_GEN;
  p.itvl_min = BLE_GAP_ADV_ITVL_MS(30);
  p.itvl_max = BLE_GAP_ADV_ITVL_MS(60);

  if (ble_gap_adv_set_fields(&adv) == 0 && ble_gap_adv_rsp_set_fields(&rsp) == 0)
    ble_gap_adv_start(g_ownAddrType, nullptr, BLE_HS_FOREVER, &p, gapEvent, nullptr);
}

// NimBLE host task context: counters only, loop() does the logging.
int gapEvent(ble_gap_event* ev, void*) {
  switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (ev->connect.status != 0) { startAdvertising(); return 0; }
      g_links++;
      g_connEv++;
      // Ask for encryption at once, like a commercial remote: a new phone
      // bonds, a bonded one re-encrypts with its stored key.
      ble_gap_security_initiate(ev->connect.conn_handle);
      return 0;
    case BLE_GAP_EVENT_DISCONNECT:
      if (g_links > 0) g_links--;
      g_subscribed = false;
      g_discReason = ev->disconnect.reason;
      g_discEv++;
      startAdvertising();
      return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      startAdvertising();
      return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
      if (ev->enc_change.status == 0) g_encOkEv++;
      else g_encFailEv++;
      return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:   // also fires when a bonded phone's CCCD is restored
      if (ev->subscribe.attr_handle == g_reportHandle) g_subscribed = ev->subscribe.cur_notify;
      return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      // The phone dropped its bond but we still hold one: forget ours and pair again.
      ble_gap_conn_desc d;
      if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0)
        ble_store_util_delete_peer(&d.peer_id_addr);
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
  }
  return 0;
}

void onSync() {
  ble_hs_util_ensure_addr(0);
  ble_hs_id_infer_auto(0, &g_ownAddrType);
  g_synced = true;
  startAdvertising();
}

void onReset(int) {
  g_synced = false;
  g_resetEv++;
}

void hostTask(void*) {
  nimble_port_run();            // returns only on nimble_port_stop()
  nimble_port_freertos_deinit();
}

// Notifies every phone subscribed to the input report. The value is read back
// through gattAccess(), so g_report is only ever written here.
void sendUsage(uint16_t usage) {
  g_report[0] = (uint8_t)(usage & 0xFF);
  g_report[1] = (uint8_t)(usage >> 8);
  ble_gatts_chr_updated(g_reportHandle);
}

// Owns the HID characteristic: press, hold, release. Running it here rather
// than from loop() keeps the press length exact even when loop() stalls (a TLS
// read can block for seconds), and a key held that long would auto-repeat on
// the phone and take a burst instead of one frame.
void shutterTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    sendUsage(SHUTTER_USAGE);
    vTaskDelay(pdMS_TO_TICKS(KEY_PRESS_MS));
    sendUsage(0);
  }
}

int bondCount() {
  int n = 0;
  if (g_bleUp && ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &n) == 0) return n;
  return -1;
}

int linkCount() { return g_bleUp ? g_links.load() : 0; }

bool bleBegin() {
  unsigned heapBefore = ESP.getFreeHeap();
  if (nimble_port_init() != ESP_OK) return false;   // also starts the BT controller

  // "Just Works" with bonding, like any commercial shutter remote. Bonds are
  // kept in NVS by NimBLE, so the phone reconnects after a reboot without
  // pairing again. Identity keys are exchanged, which is what lets the board
  // recognise the phone behind its rotating random address.
  ble_hs_cfg.reset_cb = onReset;
  ble_hs_cfg.sync_cb = onSync;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 1;   // LE Secure Connections
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

  ble_svc_gap_init();
  ble_svc_gatt_init();
  if (ble_gatts_count_cfg(g_services) != 0 || ble_gatts_add_svcs(g_services) != 0) return false;
  ble_svc_gap_device_name_set(g_cfg.bleName);
  ble_svc_gap_device_appearance_set(APPEARANCE_KEYBOARD);
  ble_store_config_init();

  if (xTaskCreate(shutterTask, "tl_shutter", 3072, nullptr, 2, &g_shutterTask) != pdPASS)
    return false;
  nimble_port_freertos_init(hostTask);
  for (int i = 0; i < 200 && !g_synced; i++) delay(10);   // host sync, normally < 100 ms

  g_bleUp = true;
  g_log->printf("[TL] Bluetooth shutter up as \"%s\", bonds %d, %s (heap %u -> %u)\n",
                g_cfg.bleName, bondCount(), g_synced ? "advertising" : "host NOT synced yet",
                heapBefore, (unsigned)ESP.getFreeHeap());
  return true;
}

// Main task only. Returns false (and says why) when nothing was sent.
bool pressShutter(const char** whyNot) {
  if (!g_bleUp) { *whyNot = "Bluetooth did not start (see the boot log)"; return false; }
  if (linkCount() == 0) { *whyNot = "phone not connected"; return false; }
  xTaskNotifyGive(g_shutterTask);
  return true;
}

// ---------------------------------------------------------------------------
// Marker handling
// ---------------------------------------------------------------------------
void onMarker(unsigned long now, int layer) {
  g_markers++;
  g_lastMarkerMs = now;
  g_lastMarkerLayer = layer;
  if (g_hook) g_hook();

  if (!g_armed) {
    g_log->printf("[TL] t=%lu marker layer %d ignored: target never returned to base\n", now, layer);
    return;
  }
  g_armed = false;
  if (!g_running) {
    g_log->printf("[TL] t=%lu marker layer %d ignored: gcode_state %s\n", now, layer, g_state);
    return;
  }
  if (g_haveShot && now - g_lastShotMs < g_cfg.minIntervalMs) {
    g_log->printf("[TL] t=%lu marker layer %d ignored: %lu ms after the last shot (min %u)\n",
                  now, layer, now - g_lastShotMs, g_cfg.minIntervalMs);
    return;
  }
  const char* whyNot = "";
  if (!pressShutter(&whyNot)) {
    g_missed++;
    g_log->printf("[TL] t=%lu MISSED layer %d: %s\n", now, layer, whyNot);
    return;
  }
  g_shots++;
  g_shotsSinceBoot++;
  g_haveShot = true;
  g_lastShotMs = now;
  g_lastShotLayer = layer;
  g_log->printf("[TL] t=%lu SHOT %lu layer %d\n", now, (unsigned long)g_shots, layer);
}

void printStatus() {
  unsigned long now = millis();
  g_log->printf("[TL] photos: %s | shutter: %s | phone links: %d%s | bonds: %d | name: %s\n",
                g_cfg.enabled ? "on" : "OFF (tl on)",
                g_bleUp ? "up" : "did not start",
                linkCount(), g_subscribed ? " (subscribed)" : "", bondCount(), g_cfg.bleName);
  g_log->printf("[TL] this print: %lu photos, %lu markers, %lu missed | since boot: %lu photos\n",
                (unsigned long)g_shots, (unsigned long)g_markers, (unsigned long)g_missed,
                (unsigned long)g_shotsSinceBoot);
  if (g_haveShot)
    g_log->printf("[TL] last shot: layer %d, %lu ms ago (t=%lu)\n",
                  g_lastShotLayer, now - g_lastShotMs, g_lastShotMs);
  if (g_markers)
    g_log->printf("[TL] last marker: layer %d, %lu ms ago (t=%lu)\n",
                  g_lastMarkerLayer, now - g_lastMarkerMs, g_lastMarkerMs);
  g_log->printf("[TL] bed target %.1f, base %.1f, %s | printer %s\n",
                g_cur, g_base, g_armed ? "armed" : "waiting for return to base", g_state);
  g_log->printf("[TL] config: min interval %u ms, marker +%u, log temps %s\n",
                g_cfg.minIntervalMs, g_cfg.markerDelta, g_cfg.logTemps ? "on" : "off");
  g_log->printf("[TL] heap free %u, largest block %u, min ever %u | shutter stack unused %u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
                (unsigned)ESP.getMinFreeHeap(),
                g_shutterTask ? (unsigned)uxTaskGetStackHighWaterMark(g_shutterTask) : 0u);
}

void printHelp() {
  g_log->println("Timelapse (bed-target marker -> Bluetooth Volume Up):");
  g_log->println("  tl status            phone link, photos this print, last trigger");
  g_log->println("  tl shot              press the shutter once now (test)");
  g_log->println("  tl reset             reset the photo counter");
  g_log->println("  tl on | tl off       take photos on markers, or pause");
  g_log->println("  tl interval <ms>     min time between photos (default 1500)");
  g_log->println("  tl delta <n>         marker step in C (default 1, = --delta)");
  g_log->println("  tl logtemps on|off   log every bed target change (default on)");
  g_log->println("  tl name <text>       Bluetooth name (reboot to apply)");
  g_log->println("  tl unpair            delete all Bluetooth bonds on this board");
}

bool parseOnOff(const String& v, bool* out) {
  if (v.equalsIgnoreCase("on") || v == "1")  { *out = true;  return true; }
  if (v.equalsIgnoreCase("off") || v == "0") { *out = false; return true; }
  return false;
}

}  // namespace

// =============================================================================
// Public API
// =============================================================================
void begin(Print& log, const char* nvsNamespace, const char* defaultName) {
  g_log = &log;
  g_ns  = nvsNamespace;
  if (defaultName && defaultName[0]) strlcpy(g_cfg.bleName, defaultName, sizeof(g_cfg.bleName));
  loadConfig();
  if (!bleBegin())
    g_log->println("[TL] Bluetooth start FAILED - no shutter. Is btInUse() true in the sketch?");
  if (!g_cfg.enabled) g_log->println("[TL] photos are OFF (tl on)");
}

void loop() {
  if (!g_log) return;
  unsigned long now = millis();

  // Promote a held value to base. Marker values only after MARKER_STUCK_MS.
  // Runs while photos are off too, so `tl on` starts with a known base.
  if (!isnan(g_cur) && !isnan(g_base) && fabsf(g_cur - g_base) >= SAME_TOL) {
    bool markerValue = fabsf(g_cur - (g_base + g_cfg.markerDelta)) <= MARKER_TOL;
    if (now - g_curSince >= (markerValue ? MARKER_STUCK_MS : BASE_HOLD_MS)) {
      if (g_cfg.enabled && (g_cfg.logTemps || markerValue))
        g_log->printf("[TL] t=%lu base %.1f -> %.1f\n", now, g_base, g_cur);
      g_base  = g_cur;
      g_armed = true;
    }
  }

  if (!g_bleUp) return;

  static uint8_t conn = 0, disc = 0, encOk = 0, encFail = 0, reset = 0;
  static bool subscribed = false;
  if (g_connEv != conn) {
    conn = g_connEv;
    g_log->printf("[TL] t=%lu phone connected (links %d)\n", now, linkCount());
  }
  if (g_encOkEv != encOk) {
    encOk = g_encOkEv;
    g_log->printf("[TL] t=%lu link encrypted, bonds %d\n", now, bondCount());
  }
  if (g_encFailEv != encFail) {
    encFail = g_encFailEv;
    g_log->printf("[TL] t=%lu pairing FAILED - if the phone still lists \"%s\", "
                  "forget it there and pair again\n", now, g_cfg.bleName);
  }
  if (g_subscribed != subscribed) {
    subscribed = g_subscribed;
    g_log->printf("[TL] t=%lu phone %s the shutter\n", now,
                  subscribed ? "subscribed to" : "unsubscribed from");
  }
  if (g_discEv != disc) {
    disc = g_discEv;
    g_log->printf("[TL] t=%lu phone disconnected (reason 0x%03X, links %d), advertising\n",
                  now, g_discReason.load(), linkCount());
  }
  if (g_resetEv != reset) {
    reset = g_resetEv;
    g_log->printf("[TL] t=%lu Bluetooth host reset - resyncing\n", now);
  }

  // The GAP handler restarts advertising after every disconnect; this catches
  // anything that leaves the board neither connected nor advertising.
  static unsigned long lastAdvCheck = 0;
  if (now - lastAdvCheck >= ADV_CHECK_MS) {
    lastAdvCheck = now;
    if (g_synced && linkCount() == 0 && !ble_gap_adv_active()) {
      startAdvertising();
      g_log->printf("[TL] t=%lu advertising restarted\n", now);
    }
  }
}

void onPrinterState(const char* s) {
  bool wasActive = isActive(g_state);
  bool active    = isActive(s);
  if (g_log && g_cfg.enabled) {
    if (active && !wasActive) {
      g_log->printf("[TL] print started (%s) - photo counter reset\n", s);
    } else if (!active && wasActive) {
      g_log->printf("[TL] print ended (%s): %lu photos, %lu markers, %lu missed\n", s,
                    (unsigned long)g_shots, (unsigned long)g_markers, (unsigned long)g_missed);
    }
  }
  if (active && !wasActive) resetPrintCounters();
  g_running = !strcmp(s, "RUNNING");
  strlcpy(g_state, s, sizeof(g_state));
}

void onBedTarget(float v, int layer) {
  if (isnan(v) || !g_log) return;
  if (!isnan(g_cur) && fabsf(v - g_cur) < SAME_TOL) return;

  // Tracked even while photos are off: P1/A1 only report a value when it
  // changes, so a base learned late would be the first marker itself.
  unsigned long now = millis();
  float prev = g_cur;
  g_cur = v;
  g_curSince = now;
  if (g_cfg.enabled && g_cfg.logTemps)
    g_log->printf("[TL] t=%lu bed_target %.1f -> %.1f layer %d base %.1f\n",
                  now, prev, v, layer, g_base);

  if (isnan(g_base)) { g_base = v; return; }   // first value seen is the base

  float d = g_cfg.markerDelta;
  // A marker is a +d step from the base. Also accept +d from the value just
  // before it: when the bed target itself changed shortly before the marker
  // (first layer -> second layer), that new value has not been held long
  // enough to become the base yet. The injector adds a settle pause on those
  // layers, so this is the second line of defence, not the first.
  if (fabsf(v - (g_base + d)) <= MARKER_TOL ||
      (!isnan(prev) && fabsf(prev - g_base) >= SAME_TOL && fabsf(v - (prev + d)) <= MARKER_TOL)) {
    if (g_cfg.enabled) onMarker(now, layer);
    else g_armed = false;
    return;
  }
  if (fabsf(v - g_base) <= MARKER_TOL) g_armed = true;   // back at base: re-arm
  // Anything else is a candidate base; loop() promotes it after BASE_HOLD_MS.
}

void setMarkerHook(void (*hook)()) { g_hook = hook; }

bool handleCommand(const String& lineIn) {
  String line = lineIn;
  line.trim();
  String lower = line;
  lower.toLowerCase();
  if (!(lower == "tl" || lower.startsWith("tl "))) return false;
  if (!g_log) return true;

  String args = line.substring(2);
  args.trim();
  int sp = args.indexOf(' ');
  String verb = sp < 0 ? args : args.substring(0, sp);
  String val  = sp < 0 ? String("") : args.substring(sp + 1);
  verb.toLowerCase();
  val.trim();

  if (verb == "" || verb == "help") {
    printHelp();
  } else if (verb == "status") {
    printStatus();
  } else if (verb == "shot") {
    const char* whyNot = "";
    if (pressShutter(&whyNot)) g_log->printf("[TL] t=%lu manual shot sent\n", millis());
    else g_log->printf("[TL] manual shot NOT sent: %s\n", whyNot);
  } else if (verb == "reset") {
    resetPrintCounters();
    g_log->println("[TL] photo counter reset");
  } else if (verb == "on" || verb == "off") {
    g_cfg.enabled = (verb == "on");
    saveConfig();
    g_log->printf("[TL] photos on markers: %s\n", g_cfg.enabled ? "ON" : "OFF");
  } else if (verb == "interval") {
    long ms = val.toInt();
    if (ms < 200 || ms > 60000) { g_log->println("Usage: tl interval <200..60000 ms>"); return true; }
    g_cfg.minIntervalMs = (uint16_t)ms;
    saveConfig();
    g_log->printf("[TL] min interval %u ms\n", g_cfg.minIntervalMs);
  } else if (verb == "delta") {
    long d = val.toInt();
    if (d < 1 || d > 10) { g_log->println("Usage: tl delta <1..10>  (must match --delta)"); return true; }
    g_cfg.markerDelta = (uint8_t)d;
    saveConfig();
    g_log->printf("[TL] marker step +%u C\n", g_cfg.markerDelta);
  } else if (verb == "logtemps") {
    bool on;
    if (!parseOnOff(val, &on)) { g_log->println("Usage: tl logtemps on|off"); return true; }
    g_cfg.logTemps = on;
    saveConfig();
    g_log->printf("[TL] log bed target changes: %s\n", on ? "on" : "off");
  } else if (verb == "name") {
    if (val.length() == 0 || val.length() >= sizeof(g_cfg.bleName)) {
      g_log->printf("Usage: tl name <1..%u chars>\n", (unsigned)sizeof(g_cfg.bleName) - 1);
      return true;
    }
    strlcpy(g_cfg.bleName, val.c_str(), sizeof(g_cfg.bleName));
    saveConfig();
    g_log->printf("[TL] Bluetooth name \"%s\" - reboot to apply\n", g_cfg.bleName);
  } else if (verb == "unpair") {
    if (!g_bleUp) { g_log->println("[TL] Bluetooth is off - nothing to unpair"); return true; }
    int rc = ble_store_clear();
    g_log->printf("[TL] bonds deleted (rc=%d). Forget \"%s\" on the phone too, then pair again.\n",
                  rc, g_cfg.bleName);
  } else {
    g_log->printf("[TL] unknown command '%s' - tl help\n", verb.c_str());
  }
  return true;
}

const Config& config() { return g_cfg; }

bool bleConnected() { return linkCount() > 0; }

}  // namespace Timelapse
