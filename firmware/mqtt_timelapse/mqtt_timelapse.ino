/*
 * MQTT Timelapse - one phone photo per layer, triggered by your Bambu Lab printer
 * https://github.com/3dfarmers/mqtt_timelapse
 *
 * The board reads the printer's MQTT reports over your WiFi. When the bed
 * target steps up by 1 C - a marker tools/inject_timelapse_markers.py puts
 * right after Bambu's timelapse park - it presses Volume Up on your phone over
 * Bluetooth. Your camera app takes the photo.
 *
 * Board:    ESP32-S3 (tested), ESP32-C3 / C6 / C5 should work. Not the classic ESP32.
 * Core:     esp32 by Espressif 3.3.x (Arduino IDE > Boards Manager)
 * Library:  PubSubClient by Nick O'Leary (Library Manager)
 * Settings: config.h (copy config.example.h), or the Serial Monitor - see README.md
 * Commands: type `help` in the Serial Monitor (115200 baud)
 */
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include "timelapse.h"

#if __has_include("config.h")
#include "config.h"
#define CONFIG_SOURCE "config.h"
#else
#include "config.example.h"
#define CONFIG_SOURCE "config.example.h (no config.h)"
#endif

#define FW_VERSION       "1.0.0"
#define NVS_NAMESPACE    "mqtt_tl"
#define MQTT_PORT        8883
// The biggest Bambu report measured is 18 KB (H2S push_status). PubSubClient
// silently drops any packet larger than this buffer.
#define MQTT_BUFFER_SIZE 24576
#define RETRY_MS         5000UL
#define RETRY_SLOW_MS    30000UL   // after 3 failed connects in a row
#define SILENT_WARN_MS   30000UL   // connected but no report: warn
#define LED_PULSE_MS     150UL

// No Arduino BLE library is linked, so tell the core to keep the Bluetooth
// controller's RAM instead of freeing it at boot (timelapse.h).
extern "C" bool btInUse(void) { return true; }

// -----------------------------------------------------------------------------
// Settings: NVS (typed in the Serial Monitor) first, config.h second
// -----------------------------------------------------------------------------
struct Settings {
  String ssid, pass, ip, code, serial;
  bool wifiSaved = false, printerSaved = false;   // typed in the Serial Monitor
} cfg;

static String nvsStr(Preferences& p, const char* key, const char* fallback) {
  return p.isKey(key) ? p.getString(key, fallback) : String(fallback);
}

void loadSettings() {
  Preferences p;
  p.begin(NVS_NAMESPACE, true);
  cfg.ssid   = nvsStr(p, "wifi_ssid", WIFI_SSID);
  cfg.pass   = nvsStr(p, "wifi_pass", WIFI_PASSWORD);
  cfg.ip     = nvsStr(p, "prt_ip", PRINTER_IP);
  cfg.code   = nvsStr(p, "prt_code", PRINTER_ACCESS_CODE);
  cfg.serial = nvsStr(p, "prt_serial", PRINTER_SERIAL);
  cfg.wifiSaved = p.isKey("wifi_ssid");
  cfg.printerSaved = p.isKey("prt_ip");
  p.end();
  // Topics are case-sensitive and Bambu serials are uppercase: a lowercase
  // serial connects fine and then never receives a report.
  cfg.serial.trim();
  cfg.serial.toUpperCase();
}

bool wifiConfigured()    { return cfg.ssid.length() && cfg.ssid != "your-wifi-name"; }
bool printerConfigured() {
  return cfg.ip.length() && cfg.code.length() && cfg.serial.length() &&
         cfg.serial != "01P00A000000000" && cfg.code != "12345678";
}

void saveStrings(const char* k1, const String& v1, const char* k2, const String& v2,
                 const char* k3 = nullptr, const String& v3 = String()) {
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  p.putString(k1, v1);
  p.putString(k2, v2);
  if (k3) p.putString(k3, v3);
  p.end();
}

// -----------------------------------------------------------------------------
// WiFi
// -----------------------------------------------------------------------------
uint8_t wifiFirstReason = 0;
unsigned long wifiStartMs = 0;
bool wifiExplained = false;

void wifiStart() {
  if (!wifiConfigured()) {
    Serial.println("[WiFi] not set - type: wifi \"<wifi name>\" <password>");
    return;
  }
  static bool hooked = false;
  if (!hooked) {
    hooked = true;
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
      if (!wifiFirstReason) wifiFirstReason = info.wifi_sta_disconnected.reason;
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  }
  Serial.printf("[WiFi] connecting to \"%s\"\n", cfg.ssid.c_str());
  wifiFirstReason = 0;
  wifiExplained = false;
  WiFi.mode(WIFI_STA);   // keep the default modem sleep: WiFi and Bluetooth share one radio
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  wifiStartMs = millis();
}

void wifiLoop() {
  static bool was = false;
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !was)
    Serial.printf("[WiFi] connected, IP %s, signal %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  if (!up && was) Serial.println("[WiFi] lost - reconnecting");
  was = up;

  // Still not connected after 20 s: say why, once.
  if (!up && !wifiExplained && wifiStartMs && millis() - wifiStartMs > 20000) {
    wifiExplained = true;
    Serial.printf("[WiFi] not connected after 20 s, reason %u %s\n", wifiFirstReason,
                  WiFi.disconnectReasonName((wifi_err_reason_t)wifiFirstReason));
    if (wifiFirstReason == WIFI_REASON_NO_AP_FOUND)
      Serial.println("[WiFi] network not found: check the name, and that it is on 2.4 GHz (try `scan`)");
    else if (wifiFirstReason == WIFI_REASON_AUTH_FAIL || wifiFirstReason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
             wifiFirstReason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT)
      Serial.println("[WiFi] the router rejected the password");
  }
}

void wifiScan() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  int n = WiFi.scanNetworks();
  Serial.printf("[WiFi] %d networks on 2.4 GHz (strongest first):\n", n);
  for (int i = 0; i < n && i < 15; i++)
    Serial.printf("  %-32s ch%2d %4d dBm\n", WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i));
  WiFi.scanDelete();
  wifiStart();
}

// -----------------------------------------------------------------------------
// Bambu MQTT (TLS on 8883, user "bblp", password = LAN access code)
// -----------------------------------------------------------------------------
WiFiClientSecure tls;
PubSubClient mqtt(tls);
char topicReport[48], topicRequest[48];
unsigned long lastConnectTry = 0, lastReportMs = 0, connectedSinceMs = 0;
uint8_t connectFails = 0;
bool dumpReports = false;

String printerState = "unknown";
int layerNum = -1, totalLayers = -1, percent = -1;

// strstr-based field readers: Bambu reports are flat enough, and a JSON
// library would need a second copy of an 18 KB report in RAM.
static const char* fieldValue(const char* buf, const char* key) {
  const char* p = strstr(buf, key);
  if (!p) return nullptr;
  p += strlen(key);
  while (*p == '"' || *p == ':' || *p == ' ') p++;
  return p;
}
static bool readInt(const char* buf, const char* key, int* out) {
  const char* p = fieldValue(buf, key);
  if (!p) return false;
  char* end;
  long v = strtol(p, &end, 10);
  if (end == p) return false;
  *out = (int)v;
  return true;
}
static bool readFloat(const char* buf, const char* key, float* out) {
  const char* p = fieldValue(buf, key);
  if (!p) return false;
  char* end;
  float v = strtof(p, &end);
  if (end == p) return false;
  *out = v;
  return true;
}
static bool readStr(const char* buf, const char* key, char* out, size_t outSize) {
  const char* p = strstr(buf, key);
  if (!p) return false;
  p = strchr(p + strlen(key), '"');   // opening quote of the value
  if (!p) return false;
  const char* e = strchr(++p, '"');
  if (!e) return false;
  size_t n = (size_t)(e - p) < outSize - 1 ? (size_t)(e - p) : outSize - 1;
  memcpy(out, p, n);
  out[n] = 0;
  return true;
}

void requestPushall() {
  if (!mqtt.connected()) return;
  mqtt.publish(topicRequest, "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\"}}");
}

// One printer report. P1 and A1 send only the fields that changed; X1, P2S and
// H2 send everything, many times a second. Either way a field that is absent
// means "unchanged".
void onReport(char* topic, byte* payload, unsigned int length) {
  lastReportMs = millis();

  // Parse in place: the payload sits inside PubSubClient's buffer, so the byte
  // after it is free unless the packet filled the buffer. Packet = 1 type byte
  // + up to 4 length bytes + 2 topic-length bytes + topic + payload.
  if (length + strlen(topic) + 9 >= MQTT_BUFFER_SIZE) {
    Serial.printf("[MQTT] %u-byte report fills the buffer - raise MQTT_BUFFER_SIZE\n", length);
    return;
  }
  char* buf = (char*)payload;
  buf[length] = 0;

  if (dumpReports) Serial.printf("[DUMP] %u bytes: %.2000s\n", length, buf);

  // The full-report printers repeat the same state many times a second.
  // Parse those at most every 150 ms; small (changed-fields) reports always.
  static unsigned long lastParse = 0;
  if (length >= 4000 && millis() - lastParse < 150) return;
  lastParse = millis();

  char state[24];
  if (readStr(buf, "\"gcode_state\"", state, sizeof(state)) && printerState != state) {
    printerState = state;
    Serial.printf("[PRINTER] state %s\n", state);
    Timelapse::onPrinterState(state);
  }
  int v;
  if (readInt(buf, "\"total_layer_num\"", &v)) totalLayers = v;
  if (readInt(buf, "\"layer_num\"", &v) && v != layerNum) {
    layerNum = v;
    if (printerState == "RUNNING") Serial.printf("[PRINTER] layer %d / %d\n", layerNum, totalLayers);
  }
  if (readInt(buf, "\"mc_percent\"", &v)) percent = v;
  float bed;
  if (readFloat(buf, "\"bed_target_temper\"", &bed)) Timelapse::onBedTarget(bed, layerNum);
}

void mqttConnect() {
  snprintf(topicReport, sizeof(topicReport), "device/%s/report", cfg.serial.c_str());
  snprintf(topicRequest, sizeof(topicRequest), "device/%s/request", cfg.serial.c_str());
  char clientId[24];
  snprintf(clientId, sizeof(clientId), "mqtt_tl_%06X", (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF));

  Serial.printf("[MQTT] connecting to %s:%d\n", cfg.ip.c_str(), MQTT_PORT);
  mqtt.setServer(cfg.ip.c_str(), MQTT_PORT);
  if (mqtt.connect(clientId, "bblp", cfg.code.c_str())) {
    connectFails = 0;
    connectedSinceMs = millis();
    lastReportMs = 0;
    mqtt.subscribe(topicReport);
    Serial.printf("[MQTT] connected, listening on %s\n", topicReport);
    requestPushall();   // Bambu printers do not send a report on subscribe
    return;
  }
  connectFails++;
  int rc = mqtt.state();
  Serial.printf("[MQTT] connect failed, rc=%d%s\n", rc,
                rc == MQTT_CONNECT_BAD_CREDENTIALS || rc == MQTT_CONNECT_UNAUTHORIZED
                  ? " - access code rejected"
                : rc == MQTT_CONNECT_FAILED
                  ? " - printer not reachable: IP right? LAN mode on? same network?"
                  : "");
  if (connectFails == 3) Serial.println("[MQTT] retrying every 30 s from now on");
}

void mqttLoop() {
  if (!printerConfigured() || WiFi.status() != WL_CONNECTED) return;
  if (!mqtt.connected()) {
    unsigned long gap = connectFails >= 3 ? RETRY_SLOW_MS : RETRY_MS;
    if (lastConnectTry == 0 || millis() - lastConnectTry >= gap) {
      lastConnectTry = millis();
      mqttConnect();
    }
    return;
  }
  mqtt.loop();

  // No state yet: the first pushall may have been ignored. Ask again.
  static unsigned long lastPushall = 0;
  if (printerState == "unknown" && millis() - lastPushall >= RETRY_MS) {
    lastPushall = millis();
    requestPushall();
  }
  // Connected but deaf: the broker accepts any serial in the topic, so a wrong
  // serial number looks exactly like this.
  static unsigned long lastWarn = 0;
  if (lastReportMs == 0 && millis() - connectedSinceMs > SILENT_WARN_MS &&
      millis() - lastWarn > SILENT_WARN_MS) {
    lastWarn = millis();
    Serial.println("[MQTT] connected but no report yet - check the serial number");
  }
}

// -----------------------------------------------------------------------------
// Marker LED
// -----------------------------------------------------------------------------
unsigned long ledOnAt = 0;
void markerLed() {
  if (MARKER_LED < 0) return;
  digitalWrite(MARKER_LED, HIGH);
  ledOnAt = millis() | 1;   // 0 = no pulse running
}
void ledLoop() {
  if (ledOnAt && millis() - ledOnAt >= LED_PULSE_MS) {
    digitalWrite(MARKER_LED, LOW);
    ledOnAt = 0;
  }
}

// -----------------------------------------------------------------------------
// Serial commands
// -----------------------------------------------------------------------------
void printHelp() {
  Serial.println("Setup:");
  Serial.println("  wifi \"<name>\" <password>            save WiFi (quotes only needed for spaces)");
  Serial.println("  printer <ip> <access code> <serial>  save the Bambu printer");
  Serial.println("  forget                               drop saved settings, back to config.h");
  Serial.println("  scan                                 list the WiFi networks the board sees");
  Serial.println("Diagnostics:");
  Serial.println("  status                               WiFi, printer and shutter state");
  Serial.println("  dump on | dump off                   print every raw printer report");
  Serial.println("  pushall                              ask the printer for a full report");
  Serial.println("  reboot");
  Timelapse::handleCommand("tl help");
}

const char* settingsSource(bool saved) { return saved ? "saved on the board" : CONFIG_SOURCE; }

void printStatus() {
  Serial.printf("MQTT Timelapse %s | WiFi settings: %s | printer settings: %s\n", FW_VERSION,
                settingsSource(cfg.wifiSaved), settingsSource(cfg.printerSaved));
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("WiFi: \"%s\" connected, IP %s, %d dBm\n", cfg.ssid.c_str(),
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  else
    Serial.printf("WiFi: \"%s\" not connected\n", cfg.ssid.c_str());
  if (!printerConfigured()) {
    Serial.println("Printer: not set - printer <ip> <access code> <serial>");
  } else {
    Serial.printf("Printer: %s, serial %s, MQTT %s", cfg.ip.c_str(), cfg.serial.c_str(),
                  mqtt.connected() ? "connected" : "not connected");
    if (lastReportMs) Serial.printf(", last report %lu ms ago", millis() - lastReportMs);
    Serial.println();
    Serial.printf("Print: %s, layer %d / %d, %d%%\n", printerState.c_str(), layerNum,
                  totalLayers, percent);
  }
  Timelapse::handleCommand("tl status");
}

// wifi "<name with spaces>" <password>   or   wifi <name> <password>
bool parseWifi(String a, String* ssid, String* pass) {
  a.trim();
  if (a.startsWith("\"")) {
    int q = a.indexOf('"', 1);
    if (q < 0) return false;
    *ssid = a.substring(1, q);
    *pass = a.substring(q + 1);
  } else {
    int sp = a.indexOf(' ');
    if (sp < 0) return false;
    *ssid = a.substring(0, sp);
    *pass = a.substring(sp + 1);
  }
  pass->trim();
  return ssid->length() > 0 && ssid->length() <= 32 && pass->length() <= 63;
}

void handleLine(String line) {
  line.trim();
  if (!line.length()) return;
  if (Timelapse::handleCommand(line)) return;
  String lower = line;
  lower.toLowerCase();

  if (lower == "help" || lower == "?") {
    printHelp();
  } else if (lower == "status") {
    printStatus();
  } else if (lower.startsWith("wifi ")) {
    String ssid, pass;
    if (!parseWifi(line.substring(5), &ssid, &pass)) {
      Serial.println("Usage: wifi \"<name>\" <password>");
      return;
    }
    saveStrings("wifi_ssid", ssid, "wifi_pass", pass);
    cfg.ssid = ssid;
    cfg.pass = pass;
    cfg.wifiSaved = true;
    Serial.printf("[WiFi] saved \"%s\"\n", ssid.c_str());   // never echo the password
    WiFi.disconnect();
    wifiStart();
  } else if (lower.startsWith("printer ")) {
    String a = line.substring(8);
    a.trim();
    int s1 = a.indexOf(' ');
    int s2 = s1 < 0 ? -1 : a.indexOf(' ', s1 + 1);
    if (s2 < 0) {
      Serial.println("Usage: printer <ip> <access code> <serial>");
      return;
    }
    String sn = a.substring(s2 + 1);
    sn.trim();
    sn.toUpperCase();
    saveStrings("prt_ip", a.substring(0, s1), "prt_code", a.substring(s1 + 1, s2), "prt_serial", sn);
    loadSettings();
    Serial.printf("[MQTT] saved printer %s, serial %s - connecting\n", cfg.ip.c_str(), cfg.serial.c_str());
    mqtt.disconnect();
    printerState = "unknown";
    connectFails = 0;
    lastConnectTry = 0;
  } else if (lower == "forget") {
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    for (const char* k : {"wifi_ssid", "wifi_pass", "prt_ip", "prt_code", "prt_serial"}) p.remove(k);
    p.end();
    Serial.println("Saved WiFi and printer settings removed - config.h applies after `reboot`");
  } else if (lower == "scan") {
    wifiScan();
  } else if (lower == "dump on" || lower == "dump off") {
    dumpReports = (lower == "dump on");
    Serial.printf("Raw report dump %s\n", dumpReports ? "ON (first 2000 characters each)" : "off");
  } else if (lower == "pushall") {
    requestPushall();
    Serial.println("[MQTT] pushall requested");
  } else if (lower == "reboot") {
    delay(200);
    ESP.restart();
  } else {
    Serial.println("Unknown command - type help");
  }
}

void serialLoop() {
  static String line;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length()) handleLine(line);
      line = "";
    } else if (line.length() < 200) {
      line += c;
    }
  }
}

// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1500);   // let a USB serial monitor attach
  Serial.printf("\n=== MQTT Timelapse %s ===\n", FW_VERSION);
  if (MARKER_LED >= 0) {
    pinMode(MARKER_LED, OUTPUT);
    digitalWrite(MARKER_LED, LOW);
  }

  loadSettings();
  Serial.printf("WiFi settings: %s | printer settings: %s%s\n", settingsSource(cfg.wifiSaved),
                settingsSource(cfg.printerSaved),
                (!wifiConfigured() || !printerConfigured()) ? " - incomplete, type help" : "");

  // Bluetooth first, so the phone can be paired and tested before the printer
  // is set up.
  Timelapse::begin(Serial, NVS_NAMESPACE, SHUTTER_NAME);
  Timelapse::setMarkerHook(markerLed);

  tls.setInsecure();   // the printer's certificate is self-signed
  mqtt.setBufferSize(MQTT_BUFFER_SIZE);
  mqtt.setCallback(onReport);
  mqtt.setKeepAlive(60);

  wifiStart();
  Serial.println("Type help for commands.");
}

void loop() {
  serialLoop();
  wifiLoop();
  mqttLoop();
  Timelapse::loop();
  ledLoop();
  delay(5);
}
