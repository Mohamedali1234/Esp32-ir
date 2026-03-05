/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║      ESP32 IR REMOTE + DAZZLER  v2.1                        ║
 * ║      Target: Generic ESP32 (ESP32-WROOM-32 / DevKitC)       ║
 * ║      IR Send/Receive · SD Card · Web UI · IR Flood          ║
 * ╚══════════════════════════════════════════════════════════════╝
 *
 * ── ARDUINO IDE SETUP ───────────────────────────────────────────
 *  Board Manager URL:
 *    https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
 *  Board:            ESP32 Dev Module
 *  Partition Scheme: Default 4MB with spiffs  ← IMPORTANT
 *  Flash Size:       4MB
 *  CPU Frequency:    240MHz
 *  Upload Speed:     921600
 *
 * ── LIBRARIES (Arduino Library Manager) ────────────────────────
 *  IRremoteESP8266   by crankyoldgit       >= 2.8.6
 *  ArduinoJson       by Benoit Blanchon    >= 6.21  (NOT v7)
 *  ESPAsyncWebServer by lacamera           >= 3.0
 *  AsyncTCP          by me-no-dev          >= 1.1
 *    (or esphome/AsyncTCP-esphome for PlatformIO)
 *
 * ── UPLOADING THE WEB UI ────────────────────────────────────────
 *  Arduino IDE:
 *    1. Install "ESP32 Sketch Data Upload" plugin (ESP32FS)
 *    2. Place data/index.html in sketch's data/ folder
 *    3. Tools → ESP32 Sketch Data Upload
 *  PlatformIO:
 *    pio run --target uploadfs
 *
 * ── GENERIC ESP32 GPIO GUIDE ────────────────────────────────────
 *
 *  ✅ SAFE OUTPUT PINS (use for IR TX, SD):
 *     2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33
 *
 *  ✅ SAFE INPUT PINS (use for IR RX):
 *     2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33, 34, 35, 36, 39
 *
 *  ⚠️  INPUT-ONLY (no OUTPUT, no pull-up): 34, 35, 36 (VP), 39 (VN)
 *     → OK for IR RX, but do NOT use for IR TX or SD
 *
 *  ❌ AVOID / RESERVED:
 *     0  – Boot mode pin, affects flash if pulled LOW at boot
 *     1  – TX0 (USB serial), avoid during upload
 *     3  – RX0 (USB serial), avoid during upload
 *     6–11 – Connected to internal SPI flash, DO NOT USE
 *     20 – Does not exist on ESP32-WROOM
 *     24 – Does not exist on ESP32-WROOM
 *     28–31 – Does not exist on ESP32-WROOM
 *
 *  ⚠️  BOOT-SENSITIVE (pulled HIGH at boot, fine after):
 *     2  – Must be LOW or floating at boot (onboard LED on most boards)
 *     5  – Must be HIGH at boot (SD CS default — fine)
 *     12 – Must be LOW at boot (avoid for SD MOSI)
 *     15 – Must be HIGH at boot
 *
 *  💡 SPI BUS NOTE:
 *     ESP32 has two SPI buses you can use:
 *       VSPI (default): SCK=18, MISO=19, MOSI=23, CS=5   ← used here
 *       HSPI:           SCK=14, MISO=12, MOSI=13, CS=15
 *     Stick to VSPI defaults unless you have a conflict.
 *
 * ── DEFAULT PINS (all changeable from Web UI → Settings tab) ────
 *   IR TX   : GPIO 4   (output, safe, no boot conflict)
 *   IR RX   : GPIO 14  (input,  safe, HSPI SCK but unused here)
 *   SD CS   : GPIO 5   (VSPI CS, HIGH at boot = correct for SD)
 *   SD MOSI : GPIO 23  (VSPI MOSI)
 *   SD MISO : GPIO 19  (VSPI MISO)
 *   SD SCK  : GPIO 18  (VSPI SCK)
 *
 * ── NETWORK ─────────────────────────────────────────────────────
 *   AP SSID : ESP32-Remote
 *   Password: 12345678
 *   IP      : 192.168.4.1
 *   Web UI  : http://192.168.4.1
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <SPIFFS.h>
#include <Preferences.h>   // NVS – stores pin config across reboots

// ══════════════════════════════════════════════════════════
//  PIN CONFIG  (loaded from NVS, defaults below)
// ══════════════════════════════════════════════════════════
Preferences prefs;

struct PinConfig {
  uint8_t irTx   = 4;
  uint8_t irRx   = 14;
  uint8_t sdCs   = 5;
  uint8_t sdMosi = 23;
  uint8_t sdMiso = 19;
  uint8_t sdSck  = 18;
} pins;

void loadPins() {
  prefs.begin("pins", true);
  pins.irTx   = prefs.getUChar("irTx",   4);
  pins.irRx   = prefs.getUChar("irRx",   14);
  pins.sdCs   = prefs.getUChar("sdCs",   5);
  pins.sdMosi = prefs.getUChar("sdMosi", 23);
  pins.sdMiso = prefs.getUChar("sdMiso", 19);
  pins.sdSck  = prefs.getUChar("sdSck",  18);
  prefs.end();
}

void savePins() {
  prefs.begin("pins", false);
  prefs.putUChar("irTx",   pins.irTx);
  prefs.putUChar("irRx",   pins.irRx);
  prefs.putUChar("sdCs",   pins.sdCs);
  prefs.putUChar("sdMosi", pins.sdMosi);
  prefs.putUChar("sdMiso", pins.sdMiso);
  prefs.putUChar("sdSck",  pins.sdSck);
  prefs.end();
}

// ══════════════════════════════════════════════════════════
//  GPIO SAFETY VALIDATOR
// ══════════════════════════════════════════════════════════
String gpioWarning(uint8_t pin, bool isOutput) {
  if (pin >= 6  && pin <= 11) return "RESERVED (internal flash) — DO NOT USE";
  if (pin == 20 || pin == 24 || (pin >= 28 && pin <= 31))
    return "Does not exist on ESP32-WROOM";
  if (isOutput && (pin==34||pin==35||pin==36||pin==39))
    return "INPUT-ONLY pin — cannot drive output";
  if (pin == 1) return "TX0 (USB serial) — avoid";
  if (pin == 3) return "RX0 (USB serial) — avoid";
  if (pin == 0) return "Boot pin — must be HIGH at boot";
  if (pin == 12) return "Boot-sensitive: must be LOW at boot";
  return "";
}

void validatePins() {
  struct { uint8_t p; bool out; const char* n; } chk[] = {
    {pins.irTx,   true,  "IR TX"},
    {pins.irRx,   false, "IR RX"},
    {pins.sdCs,   true,  "SD CS"},
    {pins.sdMosi, true,  "SD MOSI"},
    {pins.sdMiso, false, "SD MISO"},
    {pins.sdSck,  true,  "SD SCK"},
  };
  bool ok = true;
  for (auto& c : chk) {
    String w = gpioWarning(c.p, c.out);
    if (w.length()) { Serial.printf("[Pins] WARNING %s GPIO%d: %s\n", c.n, c.p, w.c_str()); ok=false; }
  }
  if (ok) Serial.println("[Pins] All GPIO assignments OK");
}

// ══════════════════════════════════════════════════════════
//  AP CONFIG  (loaded from NVS, defaults below)
// ══════════════════════════════════════════════════════════
#define AP_SSID_DEFAULT  "IR Remote"
#define AP_PASS_DEFAULT  "IRREMOTE123"

String      apSsid = AP_SSID_DEFAULT;
String      apPass = AP_PASS_DEFAULT;
const IPAddress AP_IP(192, 168, 4, 1);

void loadAPConfig() {
  prefs.begin("apconf", true);
  apSsid = prefs.getString("ssid", AP_SSID_DEFAULT);
  apPass = prefs.getString("pass", AP_PASS_DEFAULT);
  prefs.end();
  if (apPass.length() < 8)  apPass = AP_PASS_DEFAULT;
  if (apSsid.length() == 0 || apSsid.length() > 32) apSsid = AP_SSID_DEFAULT;
}

void saveAPConfig() {
  prefs.begin("apconf", false);
  prefs.putString("ssid", apSsid);
  prefs.putString("pass", apPass);
  prefs.end();
}

// ══════════════════════════════════════════════════════════
//  GLOBALS
// ══════════════════════════════════════════════════════════
AsyncWebServer   server(80);
AsyncWebSocket   wsLog("/ws/log");

IRsend*          irsend  = nullptr;
IRrecv*          irrecv  = nullptr;
decode_results   irResults;

bool     sdAvailable   = false;
bool     isLearning    = false;
String   learnRemote   = "";
String   learnButton   = "";
uint32_t learnTimeout  = 0;
#define  LEARN_TIMEOUT_MS 15000

// ── Dazzler state ─────────────────────────────────────────
struct DazzlerState {
  bool     active      = false;
  uint32_t freqHz      = 38000;
  uint8_t  dutyCycle   = 50;
  String   pattern     = "steady";
  uint32_t stopAt      = 0;
  uint32_t strobeMs    = 100;
  bool     strobePhase = true;
  uint32_t lastStrobe  = 0;
  uint32_t burstOnMs   = 50;
  uint32_t burstOffMs  = 200;
} dz;

#define LEDC_CH    0
#define LEDC_BITS  8

// ══════════════════════════════════════════════════════════
//  WEBSOCKET LOG  (live serial → browser)
// ══════════════════════════════════════════════════════════
void wsLog_send(const String& msg) {
  wsLog.textAll(msg);
}

void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType type,
               void*, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) wsLog_send("[ESP32] WebSocket connected\n");
}

// Override Serial print to also send to WS
class WsLogger : public Print {
public:
  size_t write(uint8_t c) override {
    static String buf;
    buf += (char)c;
    if (c == '\n') { wsLog_send(buf); buf = ""; }
    return Serial.write(c);
  }
  size_t write(const uint8_t* b, size_t s) override {
    for (size_t i = 0; i < s; i++) write(b[i]);
    return s;
  }
} wsSerial;

// ══════════════════════════════════════════════════════════
//  SD HELPERS
// ══════════════════════════════════════════════════════════
const String remotesDir = "/remotes";

void initSD() {
  SPI.begin(pins.sdSck, pins.sdMiso, pins.sdMosi, pins.sdCs);
  if (!SD.begin(pins.sdCs)) {
    wsSerial.println("[SD] Mount failed – check wiring & card");
    sdAvailable = false;
    return;
  }
  sdAvailable = true;
  wsSerial.println("[SD] Mounted OK");
  if (!SD.exists(remotesDir)) SD.mkdir(remotesDir);
}

String remoteFilePath(const String& n) { return remotesDir + "/" + n + ".json"; }

String listRemotes() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  if (!sdAvailable) { String s; serializeJson(doc, s); return s; }
  File root = SD.open(remotesDir);
  if (!root || !root.isDirectory()) { String s; serializeJson(doc, s); return s; }
  File f = root.openNextFile();
  while (f) {
    String name = String(f.name());
    if (!f.isDirectory() && name.endsWith(".json")) {
      name.replace(".json", "");
      int sl = name.lastIndexOf('/');
      if (sl >= 0) name = name.substring(sl + 1);
      arr.add(name);
    }
    f = root.openNextFile();
  }
  String out; serializeJson(doc, out); return out;
}

String readRemote(const String& n) {
  String path = remoteFilePath(n);
  if (!sdAvailable || !SD.exists(path)) return "{}";
  File f = SD.open(path, FILE_READ);
  if (!f) return "{}";
  String c = f.readString(); f.close(); return c;
}

bool writeRemote(const String& n, const String& json) {
  if (!sdAvailable) return false;
  File f = SD.open(remoteFilePath(n), FILE_WRITE);
  if (!f) return false;
  f.print(json); f.close(); return true;
}

bool deleteRemote(const String& n) {
  if (!sdAvailable) return false;
  return SD.remove(remoteFilePath(n));
}

// ══════════════════════════════════════════════════════════
//  IR SEND
// ══════════════════════════════════════════════════════════
bool sendIRCode(const String& protocol, uint64_t code, uint16_t bits, uint16_t repeat) {
  if (!irsend) return false;
  decode_type_t prot = strToDecodeType(protocol.c_str());
  switch (prot) {
    case NEC:      irsend->sendNEC(code, bits, repeat);      break;
    case SONY:     irsend->sendSony(code, bits, repeat);     break;
    case RC5:      irsend->sendRC5(code, bits, repeat);      break;
    case RC6:      irsend->sendRC6(code, bits, repeat);      break;
    case SAMSUNG:  irsend->sendSAMSUNG(code, bits, repeat);  break;
    case LG:       irsend->sendLG(code, bits, repeat);       break;
    case PANASONIC:irsend->sendPanasonic(bits, code);        break;
    case SHARP:    irsend->sendSharpRaw(code, bits, repeat); break;
    case JVC:      irsend->sendJVC(code, bits, repeat);      break;
    case WHYNTER:  irsend->sendWhynter(code, bits);          break;
    default:
      wsSerial.printf("[IR] Unknown protocol: %s\n", protocol.c_str());
      return false;
  }
  wsSerial.printf("[IR] Sent %s 0x%llX (%d bits)\n", protocol.c_str(), code, bits);
  return true;
}

// ══════════════════════════════════════════════════════════
//  DAZZLER
// ══════════════════════════════════════════════════════════
void dazzlerOn() {
  uint32_t duty = ((1 << LEDC_BITS) - 1) * dz.dutyCycle / 100;
  ledcSetup(LEDC_CH, dz.freqHz, LEDC_BITS);
  ledcAttachPin(pins.irTx, LEDC_CH);
  ledcWrite(LEDC_CH, duty);
}

void dazzlerOff() {
  ledcWrite(LEDC_CH, 0);
  ledcDetachPin(pins.irTx);
  pinMode(pins.irTx, OUTPUT);
  digitalWrite(pins.irTx, LOW);
}

void dazzlerLoop() {
  if (!dz.active) return;
  if (dz.stopAt > 0 && millis() >= dz.stopAt) {
    dz.active = false; dazzlerOff();
    wsSerial.println("[Dazzler] Auto-stopped");
    return;
  }
  uint32_t now = millis();
  if (dz.pattern == "strobe") {
    if (now - dz.lastStrobe >= dz.strobeMs) {
      dz.strobePhase = !dz.strobePhase;
      dz.lastStrobe  = now;
      dz.strobePhase ? dazzlerOn() : dazzlerOff();
    }
  } else if (dz.pattern == "burst") {
    uint32_t period = dz.strobePhase ? dz.burstOnMs : dz.burstOffMs;
    if (now - dz.lastStrobe >= period) {
      dz.strobePhase = !dz.strobePhase;
      dz.lastStrobe  = now;
      dz.strobePhase ? dazzlerOn() : dazzlerOff();
    }
  }
}

// ══════════════════════════════════════════════════════════
//  IR LEARN HANDLER
// ══════════════════════════════════════════════════════════
void handleLearnedCode(decode_results* r) {
  String protocol = typeToString(r->decode_type, r->repeat);
  uint64_t code   = r->value;
  uint16_t bits   = r->bits;
  wsSerial.printf("[Learn] Got: %s 0x%llX (%d bits)\n", protocol.c_str(), code, bits);

  String remoteJson = readRemote(learnRemote);
  DynamicJsonDocument rdoc(8192);
  bool exists = !deserializeJson(rdoc, remoteJson) && rdoc.containsKey("buttons");
  if (!exists) { rdoc["name"] = learnRemote; rdoc["icon"] = "📺"; rdoc.createNestedArray("buttons"); }

  DynamicJsonDocument ndoc(8192);
  ndoc["name"] = rdoc["name"]; ndoc["icon"] = rdoc["icon"];
  JsonArray narr = ndoc.createNestedArray("buttons");
  for (JsonObject btn : rdoc["buttons"].as<JsonArray>())
    if (String(btn["name"].as<const char*>()) != learnButton) narr.add(btn);

  JsonObject nb = narr.createNestedObject();
  nb["name"] = learnButton; nb["protocol"] = protocol;
  nb["code"] = code; nb["bits"] = bits; nb["repeat"] = 0;

  String nJson; serializeJson(ndoc, nJson);
  writeRemote(learnRemote, nJson);
  isLearning = false;
  if (irrecv) irrecv->disableIRIn();
  wsSerial.println("[Learn] Saved!");
}

// ══════════════════════════════════════════════════════════
//  ROUTES
// ══════════════════════════════════════════════════════════
void setupRoutes() {

  // Static UI
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (sdAvailable && SD.exists("/www/index.html"))
      req->send(SD, "/www/index.html", "text/html");
    else
      req->send(SPIFFS, "/index.html", "text/html");
  });

  // ── IR Remote routes ───────────────────────────────────
  server.on("/api/remotes", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", listRemotes());
  });

  server.on("/api/remote", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("name")) { req->send(400, "application/json", "{\"error\":\"missing name\"}"); return; }
    req->send(200, "application/json", readRemote(req->getParam("name")->value()));
  });

  server.on("/api/send", HTTP_POST, [](AsyncWebServerRequest* req) {},
    nullptr, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      StaticJsonDocument<512> doc;
      if (deserializeJson(doc, data, len)) { req->send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
      String remote = doc["remote"]|"", button = doc["button"]|"", protocol = doc["protocol"]|"";
      uint64_t code = doc["code"].as<uint64_t>();
      uint16_t bits = doc["bits"]|32, repeat = doc["repeat"]|0;
      if (protocol.isEmpty() || code == 0) {
        DynamicJsonDocument rdoc(8192);
        if (!deserializeJson(rdoc, readRemote(remote)))
          for (JsonObject btn : rdoc["buttons"].as<JsonArray>())
            if (String(btn["name"].as<const char*>()) == button) {
              protocol = btn["protocol"].as<String>(); code = btn["code"].as<uint64_t>();
              bits = btn["bits"]|32; repeat = btn["repeat"]|0; break;
            }
      }
      sendIRCode(protocol, code, bits, repeat)
        ? req->send(200, "application/json", "{\"ok\":true}")
        : req->send(500, "application/json", "{\"error\":\"send failed\"}");
    });

  server.on("/api/learn/start", HTTP_POST, [](AsyncWebServerRequest* req) {},
    nullptr, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      StaticJsonDocument<256> doc;
      if (deserializeJson(doc, data, len)) { req->send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
      learnRemote = doc["remote"]|"unnamed"; learnButton = doc["button"]|"button";
      isLearning = true; learnTimeout = millis() + LEARN_TIMEOUT_MS;
      if (irrecv) irrecv->enableIRIn();
      wsSerial.printf("[Learn] Waiting for %s / %s\n", learnRemote.c_str(), learnButton.c_str());
      req->send(200, "application/json", "{\"ok\":true,\"timeout\":15}");
    });

  server.on("/api/learn/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    StaticJsonDocument<128> doc;
    doc["learning"] = isLearning; doc["remote"] = learnRemote; doc["button"] = learnButton;
    String out; serializeJson(doc, out); req->send(200, "application/json", out);
  });

  server.on("/api/learn/cancel", HTTP_POST, [](AsyncWebServerRequest* req) {
    isLearning = false; if (irrecv) irrecv->disableIRIn();
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/remote/create", HTTP_POST, [](AsyncWebServerRequest* req) {},
    nullptr, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      StaticJsonDocument<256> doc;
      if (deserializeJson(doc, data, len)) { req->send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
      String name = doc["name"]|"";
      if (name.isEmpty()) { req->send(400, "application/json", "{\"error\":\"name required\"}"); return; }
      StaticJsonDocument<256> r; r["name"]=name; r["icon"]=doc["icon"]|"📺"; r.createNestedArray("buttons");
      String json; serializeJson(r, json);
      writeRemote(name, json) ? req->send(200, "application/json", "{\"ok\":true}")
                              : req->send(500, "application/json", "{\"error\":\"write failed\"}");
    });

  server.on("/api/remote/delete", HTTP_DELETE, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("name")) { req->send(400, "application/json", "{\"error\":\"missing name\"}"); return; }
    deleteRemote(req->getParam("name")->value())
      ? req->send(200, "application/json", "{\"ok\":true}")
      : req->send(500, "application/json", "{\"error\":\"delete failed\"}");
  });

  server.on("/api/button/delete", HTTP_POST, [](AsyncWebServerRequest* req) {},
    nullptr, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      StaticJsonDocument<256> doc;
      if (deserializeJson(doc, data, len)) { req->send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
      String remote = doc["remote"]|"", button = doc["button"]|"";
      DynamicJsonDocument rdoc(8192), ndoc(8192);
      if (deserializeJson(rdoc, readRemote(remote))) { req->send(500, "application/json", "{\"error\":\"parse\"}"); return; }
      ndoc["name"]=rdoc["name"]; ndoc["icon"]=rdoc["icon"];
      JsonArray narr = ndoc.createNestedArray("buttons");
      for (JsonObject btn : rdoc["buttons"].as<JsonArray>())
        if (String(btn["name"].as<const char*>()) != button) narr.add(btn);
      String nj; serializeJson(ndoc, nj); writeRemote(remote, nj);
      req->send(200, "application/json", "{\"ok\":true}");
    });

  // ── Dazzler routes ─────────────────────────────────────
  server.on("/api/dazzler/start", HTTP_POST, [](AsyncWebServerRequest* req) {},
    nullptr, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      StaticJsonDocument<256> doc;
      if (!deserializeJson(doc, data, len)) {
        dz.freqHz    = doc["freq"]    |38000;
        dz.dutyCycle = doc["duty"]    |50;
        dz.pattern   = doc["pattern"].as<String>(); if (dz.pattern.isEmpty()) dz.pattern="steady";
        dz.strobeMs  = doc["strobeMs"]|100;
        dz.burstOnMs = doc["burstOn"] |50;
        dz.burstOffMs= doc["burstOff"]|200;
        uint32_t secs= doc["timer"]   |0;
        dz.stopAt    = s
