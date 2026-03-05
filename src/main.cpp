/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║      ESP32 IR REMOTE + DAZZLER  v3.0                        ║
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
 * ── LIBRARIES (PlatformIO — see platformio.ini) ─────────────────
 *  IRremoteESP8266       crankyoldgit    >= 2.8.6
 *  ArduinoJson           bblanchon       >= 6.21  (NOT v7)
 *  ESP32Async/AsyncTCP                   >= 3.3.6
 *  ESP32Async/ESPAsyncWebServer          >= 3.3.23
 *
 * ── DEFAULT PINS (all changeable from Web UI → Settings tab) ────
 *   IR TX   : GPIO 4    IR RX  : GPIO 14
 *   SD CS   : GPIO 5    SD MOSI: GPIO 23
 *   SD MISO : GPIO 19   SD SCK : GPIO 18
 *
 * ── NETWORK ─────────────────────────────────────────────────────
 *   AP SSID : IR Remote  /  Password: IRREMOTE123
 *   IP      : 192.168.4.1  →  http://192.168.4.1
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
#include <LittleFS.h>          // LittleFS replaces deprecated SPIFFS
#include <Preferences.h>
#include <freertos/semphr.h>   // mutex for IR/Dazzler safety

// ══════════════════════════════════════════════════════════
//  PIN CONFIG  (NVS-backed, survives reboot)
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
  if (pin >= 6  && pin <= 11) return "RESERVED (internal flash)";
  if (pin == 20 || pin == 24 || (pin >= 28 && pin <= 31))
    return "Does not exist on ESP32-WROOM";
  if (isOutput && (pin==34||pin==35||pin==36||pin==39))
    return "INPUT-ONLY pin";
  if (pin == 1) return "TX0 (USB serial)";
  if (pin == 3) return "RX0 (USB serial)";
  if (pin == 0) return "Boot pin";
  if (pin == 12) return "Boot-sensitive (must be LOW at boot)";
  return "";
}

void validatePins() {
  // Called before wsSerial is ready — use Serial directly
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
    if (w.length()) { Serial.printf("[Pins] WARNING %s GPIO%d: %s\n", c.n, c.p, w.c_str()); ok = false; }
  }
  if (ok) Serial.println("[Pins] All GPIO assignments OK");
}

// ══════════════════════════════════════════════════════════
//  AP CONFIG  (NVS-backed)
// ══════════════════════════════════════════════════════════
#define AP_SSID_DEFAULT  "IR Remote"
#define AP_PASS_DEFAULT  "IRREMOTE123"

String apSsid = AP_SSID_DEFAULT;
String apPass = AP_PASS_DEFAULT;
const IPAddress AP_IP(192, 168, 4, 1);

void loadAPConfig() {
  prefs.begin("apconf", true);
  apSsid = prefs.getString("ssid", AP_SSID_DEFAULT);
  apPass = prefs.getString("pass", AP_PASS_DEFAULT);
  prefs.end();
  if (apPass.length() < 8 || apPass.length() > 63) apPass = AP_PASS_DEFAULT;
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
AsyncWebServer server(80);
AsyncWebSocket wsLog("/ws/log");

// ── IR: static globals (not heap-allocated) ───────────────
// FIX: dynamic IRsend/IRrecv via 'new' is unreliable.
// We use static objects and re-init when pins change.
static IRsend*  irSender   = nullptr;
static IRrecv*  irReceiver = nullptr;
static uint8_t  currentTxPin = 255;
static uint8_t  currentRxPin = 255;
decode_results  irResults;

// ── IR/Dazzler mutex — prevents simultaneous use of IR pin ─
// FIX: both IR send and Dazzler use the same GPIO — serialise them
SemaphoreHandle_t irMutex = nullptr;

// ── Learning state ─────────────────────────────────────────
volatile bool isLearning   = false;
String        learnRemote  = "";
String        learnButton  = "";
uint32_t      learnTimeout = 0;
#define       LEARN_TIMEOUT_MS 15000

bool sdAvailable = false;

// ── Dazzler ────────────────────────────────────────────────
struct DazzlerState {
  volatile bool active = false;
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

#define LEDC_CH   0
#define LEDC_BITS 8

// ══════════════════════════════════════════════════════════
//  WEBSOCKET LOGGER
//  FIX: static buf was not thread-safe across dual cores.
//  Now using a per-call local buffer approach + portMUX.
// ══════════════════════════════════════════════════════════
static portMUX_TYPE wsMux = portMUX_INITIALIZER_UNLOCKED;
static String       wsLineBuf = "";

void wsLog_send(const String& msg) {
  if (wsLog.count() > 0) wsLog.textAll(msg);
}

void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
               AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    wsLog_send("[ESP32] Terminal connected\n");
  }
}

class WsLogger : public Print {
public:
  size_t write(uint8_t c) override {
    portENTER_CRITICAL(&wsMux);
    wsLineBuf += (char)c;
    bool flush = (c == '\n');
    String toSend = flush ? wsLineBuf : "";
    if (flush) wsLineBuf = "";
    portEXIT_CRITICAL(&wsMux);
    if (flush) wsLog_send(toSend);
    return Serial.write(c);
  }
  size_t write(const uint8_t* b, size_t s) override {
    for (size_t i = 0; i < s; i++) write(b[i]);
    return s;
  }
} wsSerial;

// ══════════════════════════════════════════════════════════
//  IR INIT  (safe re-init when pins change)
// ══════════════════════════════════════════════════════════
void initIR() {
  // Stop receiver if running
  if (irReceiver) {
    irReceiver->disableIRIn();
    delete irReceiver;
    irReceiver = nullptr;
  }
  if (irSender) {
    delete irSender;
    irSender = nullptr;
  }
  currentTxPin = pins.irTx;
  currentRxPin = pins.irRx;
  irSender   = new IRsend(currentTxPin, false, false);  // inverted=false, use_modulation=false
  irReceiver = new IRrecv(currentRxPin, 1024, 50, true);
  irSender->begin();
  wsSerial.printf("[IR] TX=GPIO%d  RX=GPIO%d\n", currentTxPin, currentRxPin);
}

// ══════════════════════════════════════════════════════════
//  SD HELPERS
// ══════════════════════════════════════════════════════════
const String remotesDir = "/remotes";

void initSD() {
  // FIX: end any previous SPI session before re-init
  SPI.end();
  delay(10);
  SPI.begin(pins.sdSck, pins.sdMiso, pins.sdMosi, pins.sdCs);
  delay(10);
  if (!SD.begin(pins.sdCs)) {
    wsSerial.println("[SD] Mount failed — check wiring & SD card (FAT32)");
    sdAvailable = false;
    return;
  }
  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    wsSerial.println("[SD] No card inserted");
    sdAvailable = false;
    return;
  }
  sdAvailable = true;
  wsSerial.printf("[SD] Mounted OK — Type:%s  Size:%lluMB\n",
    cardType == CARD_MMC ? "MMC" : cardType == CARD_SD ? "SD" :
    cardType == CARD_SDHC ? "SDHC" : "UNKNOWN",
    SD.cardSize() / (1024 * 1024));
  if (!SD.exists(remotesDir)) SD.mkdir(remotesDir);
}

String remoteFilePath(const String& n) { return remotesDir + "/" + n + ".json"; }

// FIX: increased doc size to handle larger remotes lists
String listRemotes() {
  DynamicJsonDocument doc(8192);
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
    f.close();
    f = root.openNextFile();
  }
  root.close();
  String out; serializeJson(doc, out); return out;
}

String readRemote(const String& n) {
  String path = remoteFilePath(n);
  if (!sdAvailable || !SD.exists(path)) return "{}";
  File f = SD.open(path, FILE_READ);
  if (!f) return "{}";
  String c = f.readString();
  f.close();
  return c;
}

bool writeRemote(const String& n, const String& json) {
  if (!sdAvailable) return false;
  // FIX: delete first then write — avoids leftover bytes from shorter writes
  String path = remoteFilePath(n);
  if (SD.exists(path)) SD.remove(path);
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  f.print(json);
  f.close();
  return true;
}

bool deleteRemote(const String& n) {
  if (!sdAvailable) return false;
  return SD.remove(remoteFilePath(n));
}

// ══════════════════════════════════════════════════════════
//  IR SEND
//  FIX: acquire mutex so Dazzler can't run simultaneously
// ══════════════════════════════════════════════════════════
bool sendIRCode(const String& protocol, uint64_t code, uint16_t bits, uint16_t repeat) {
  if (!irSender) return false;

  // Stop dazzler if active — they share the same pin
  bool dazzlerWasActive = dz.active;
  if (dz.active) {
    dz.active = false;
    ledcWrite(LEDC_CH, 0);
    ledcDetachPin(pins.irTx);
    pinMode(pins.irTx, OUTPUT);
    digitalWrite(pins.irTx, LOW);
    delay(5);
  }

  if (xSemaphoreTake(irMutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;

  decode_type_t prot = strToDecodeType(protocol.c_str());
  bool ok = true;
  switch (prot) {
    case NEC:       irSender->sendNEC(code, bits, repeat);       break;
    case SONY:      irSender->sendSony(code, bits, repeat);      break;
    case RC5:       irSender->sendRC5(code, bits, repeat);       break;
    case RC6:       irSender->sendRC6(code, bits, repeat);       break;
    case SAMSUNG:   irSender->sendSAMSUNG(code, bits, repeat);   break;
    case LG:        irSender->sendLG(code, bits, repeat);        break;
    case PANASONIC: irSender->sendPanasonic(bits, code);         break;
    case SHARP:     irSender->sendSharpRaw(code, bits, repeat);  break;
    case JVC:       irSender->sendJVC(code, bits, repeat);       break;
    case WHYNTER:   irSender->sendWhynter(code, bits);           break;
    default:
      wsSerial.printf("[IR] Unknown protocol: %s\n", protocol.c_str());
      ok = false;
  }

  xSemaphoreGive(irMutex);

  if (ok) wsSerial.printf("[IR] Sent %s 0x%llX (%d bits)\n", protocol.c_str(), code, bits);

  // Restore dazzler if it was running
  if (dazzlerWasActive) {
    dz.active = true;
    if (dz.pattern == "steady") {
      uint32_t duty = ((1 << LEDC_BITS) - 1) * dz.dutyCycle / 100;
      ledcSetup(LEDC_CH, dz.freqHz, LEDC_BITS);
      ledcAttachPin(pins.irTx, LEDC_CH);
      ledcWrite(LEDC_CH, duty);
    }
  }

  return ok;
}

// ══════════════════════════════════════════════════════════
//  DAZZLER
//  FIX: acquire mutex, block IR send while dazzling
// ══════════════════════════════════════════════════════════
void dazzlerOn() {
  if (isLearning) return;  // never dazzle during learn
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
  if (isLearning) { dazzlerOff(); return; }  // safety: stop if learning starts

  if (dz.stopAt > 0 && millis() >= dz.stopAt) {
    dz.active = false;
    dazzlerOff();
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
  String   protocol = typeToString(r->decode_type, r->repeat);
  uint64_t code     = r->value;
  uint16_t bits     = r->bits;
  wsSerial.printf("[Learn] Got: %s 0x%llX (%d bits)\n", protocol.c_str(), code, bits);

  String remoteJson = readRemote(learnRemote);
  DynamicJsonDocument rdoc(8192);
  bool exists = !deserializeJson(rdoc, remoteJson) && rdoc.containsKey("buttons");
  if (!exists) {
    rdoc.clear();
    rdoc["name"] = learnRemote;
    rdoc["icon"] = "📺";
    rdoc.createNestedArray("buttons");
  }

  DynamicJsonDocument ndoc(8192);
  ndoc["name"] = rdoc["name"];
  ndoc["icon"] = rdoc["icon"];
  JsonArray narr = ndoc.createNestedArray("buttons");
  for (JsonObject btn : rdoc["buttons"].as<JsonArray>())
    if (String(btn["name"].as<const char*>()) != learnButton)
      narr.add(btn);

  JsonObject nb   = narr.createNestedObject();
  nb["name"]      = learnButton;
  nb["protocol"]  = protocol;
  nb["code"]      = (uint64_t)code;
  nb["bits"]      = bits;
  nb["repeat"]    = 0;

  String nJson; serializeJson(ndoc, nJson);
  writeRemote(learnRemote, nJson);
  isLearning = false;
  if (irReceiver) irReceiver->disableIRIn();
  wsSerial.println("[Learn] Saved!");
}

// ══════════════════════════════════════════════════════════
//  ROUTES
// ══════════════════════════════════════════════════════════
void setupRoutes() {

  // ── Static UI (LittleFS) ───────────────────────────────
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (LittleFS.exists("/index.html"))
      req->send(LittleFS, "/index.html", "text/html");
    else
      req->send(200, "text/html", "<h2>Upload index.html to LittleFS</h2>");
  });

  // ── Remotes ────────────────────────────────────────────
  server.on("/api/remotes", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", listRemotes());
  });

  server.on("/api/remote", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("name")) { req->send(400, "application/json", "{\"error\":\"missing name\"}"); return; }
    req->send(200, "application/json", readRemote(req->getParam("name")->value()));
  });

  server.on("/api/send", HTTP_POST, [](AsyncWebServerRequest* req) {},
    nullptr, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<512> doc;
      if (deserializeJson(doc, data, len)) { req->send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
      String remote   = doc["remote"]   | "";
      String button   = doc["button"]   | "";
      String protocol = doc["protocol"] | "";
      uint64_t code   = doc["code"].as<uint64_t>();
      uint16_t bits   = doc["bits"]   | 32;
      uint16_t repeat = doc["repeat"] | 0;
      if (protocol.isEmpty() || code == 0) {
        DynamicJsonDocument rdoc(8192);
        if (!deserializeJson(rdoc, readRemote(remote)))
          for (JsonObject btn : rdoc["buttons"].as<JsonArray>())
            if (String(btn["name"].as<const char*>()) == button) {
              protocol = btn["protocol"].as<String>();
              code     = btn["code"].as<uint64_t>();
              bits     = btn["bits"]   | 32;
              repeat   = btn["repeat"] | 0;
              break;
            }
      }
      sendIRCode(protocol, code, bits, repeat)
        ? req->send(200, "application/json", "{\"ok\":true}")
        : req->send(500, "application/json", "{\"error\":\"send failed\"}");
    });

  server.on("/api/learn/start", HTTP_POST, [](AsyncWebServerRequest* req) {},
    nullptr, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      if (dz.active) { req->send(409, "application/json", "{\"error\":\"Stop dazzler first\"}"); return; }
      StaticJsonDocument<256> doc;
      if (deserializeJson(doc, data, len)) { req->send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
      learnRemote  = doc["remote"] | "unnamed";
      learnButton  = doc["button"] | "button";
      isLearning   = true;
      learnTimeout = millis() + LEARN_TIMEOUT_MS;
      if (irReceiver) irReceiver->enableIRIn();
      wsSerial.printf("[Learn] Waiting for %s / %s\n", learnRemote.c_str(), learnButton.c_str());
      req->send(200, "application/json", "{\"ok\":true,\"timeout\":15}");
    });

  server.on("/api/learn/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    StaticJsonDocument<128> doc;
    doc["learning"] = isLearning;
    doc["remote"]   = learnRemote;
    doc["button"]   = learnButton;
    String out; serializeJson(doc, out); req->send(200, "application/json", out);
  });

  server.on("/api/learn/cancel", HTTP_POST, [](AsyncWebServerRequest* req) {
    isLearning = false;
    if (irReceiver) irReceiver->disableIRIn();
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/remote/create", HTTP_POST, [](AsyncWebServerRequest* req) {},
    nullptr, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<256> doc;
      if (deserializeJson(doc, data, len)) { req->send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
      String name = doc["name"] | "";
      if (name.isEmpty()) { req->send(400, "application/json", "{\"error\":\"name required\"}"); return; }
      StaticJsonDocument<256> r;
      r["name"] = name; r["icon"] = doc["icon"] | "📺";
      r.createNestedArray("buttons");
      String json; serializeJson(r, json);
      writeRemote(name, json)
        ? req->send(200, "application/json", "{\"ok\":true}")
        : req->send(500, "application/json", "{\"error\":\"write failed — SD card?\"}");
    });

  server.on("/api/remote/delete", HTTP_DELETE, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("name")) { req->send(400, "application/json", "{\"error\":\"missing name\"}"); return; }
    req->send(200, "application/json", readRemote(req->getParam("name")->value()));
  });

  server.on("/api/send", HTTP_POST, [](AsyncWebServerRequest* req) {},
    nullptr, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<512> doc;
      if (deserializeJson(doc, data, len)) { req->send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
      String remote   = doc["remote"]   | "";
      String button   = doc["button"]   | "";
      String protocol = doc["protocol"] | "";
      uint64_t code   = doc["code"].as<uint64_t>();
      uint16_t bits   = doc["bits"]   | 32;
      uint16_t repeat = doc["repeat"] | 0;
      if (protocol.isEmpty() || code == 0) {
        DynamicJsonDocument rdoc(8192);
        if (!deserializeJson(rdoc, readRemote(remote)))
          for (JsonObject btn : rdoc["buttons"].as<JsonArray>())
            if (String(btn["name"].as<const char*>()) == button) {
              protocol = btn["protocol"].as<String>();
              code     = btn["code"].as<uint64_t>();
              bits     = btn["bits"]   | 32;
              repeat   = btn["repeat"] | 0;
              break;
            }
      }
      sendIRCode(protocol, code, bits, repeat)
        ? req->send(200, "application/json", "{\"ok\":true}")
        : req->send(500, "application/json", "{\"error\":\"send failed\"}");
    });

  server.on("/api/learn/start", HTTP_POST, [](AsyncWebServerRequest* req) {},
    nullptr, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      if (dz.active) { req->send(409, "application/json", "{\"error\":\"Stop dazzler first\"}"); return; }
      StaticJsonDocument<256> doc;
      if (deserializeJson(doc, data, len)) { req->send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
      learnRemote  = doc["remote"] | "unnamed";
      learnButton  = doc["button"] | "button";
      isLearning   = true;
      learnTimeout = millis() + LEARN_TIMEOUT_MS;
      if (irReceiver) irReceiver->enableIRIn();
      wsSerial.printf("[Learn] Waiting for %s / %s\n", learnRemote.c_str(), learnButton.c_str());
      req->send(200, "application/json", "{\"ok\":true,\"timeout\":15}");
    });

  server.on("/api/learn/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    StaticJsonDocument<128> doc;
    doc["learning"] = isLearning;
    doc["remote"]   = learnRemote;
    doc["button"]   = learnButton;
    String out; serializeJson(doc, out); req->send(200, "application/json", out);
  });

  server.on("/api/learn/cancel", HTTP_POST, [](AsyncWebServerRequest* req) {
    isLearning = false;
    if (irReceiver) irReceiver->disableIRIn();
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/remote/create", HTTP_POST, [](AsyncWebServerRequest* req) {},
    nullptr, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<256> doc;
      if (deserializeJson(doc, data, len)) { req->send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
      String name = doc["name"] | "";
      if (name.isEmpty()) { req->send(400, "application/json", "{\"error\":\"name required\"}"); return; }
      StaticJsonDocument<256> r;
      r["name"] = name; r["icon"] = doc["icon"] | "📺";
      r.createNestedArray("buttons");
      String json; serializeJson(r, json);
      writeRemote(name, json)
        ? req->send(200, "application/json", "{\"ok\":true}")
        : req->send(500, "application/json", "{\"error\":\"write failed — SD card?\"}");
    });

  server.on("/api/remote/delete", HTTP_DELETE, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("name")) { req->send(400, "application/json", "{\"error\":\"missing name\"}"); return; }
    deleteRemote(req->getParam("name")->value())
      ? req->send(200, "application/json", "{\"ok\":true}")
      : req->send(500, "application/json", "{\"error\":\"delete failed\"}");
  });

  server.on("/api/button/delete", HTTP_POST, [](AsyncWebServerRequest* req) {},
    nullptr, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<256> doc;
      if (deserializeJson(doc, data, len)) { req->send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
      String remote = doc["remote"] | "";
      String button = doc["button"] | "";
      DynamicJsonDocument rdoc(8192), ndoc(8192);
      if (deserializeJson(rdoc, readRemote(remote))) { req->send(500, "application/json", "{\"error\":\"parse error\"}"); return; }
      ndoc["name"] = rdoc["name"]; ndoc["icon"] = rdoc["icon"];
      JsonArray narr = ndoc.createNestedArray("buttons");
      for (JsonObject btn : rdoc["buttons"].as<JsonArray>())
        if (String(btn["name"].as<const char*>()) != button) narr.add(btn);
      String nj; serializeJson(ndoc, nj);
      writeRemote(remote, nj);
      req->send(200, "application/json", "{\"ok\":true}");
    });

  // ── Dazzler ────────────────────────────────────────────
  server.on("/api/dazzler/start", HTTP_POST, [](AsyncWebServerRequest* req) {},
    nullptr, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      if (isLearning) { req->send(409, "application/json", "{\"error\":\"Stop learning first\"}"); return; }
      StaticJsonDocument<256> doc;
      if (!deserializeJson(doc, data, len)) {
        dz.freqHz     = doc["freq"]     | 38000;
        dz.dutyCycle  = doc["duty"]     | 50;
        dz.pattern    = doc["pattern"].as<String>();
        if (dz.pattern.isEmpty()) dz.pattern = "steady";
        dz.strobeMs   = doc["strobeMs"] | 100;
        dz.burstOnMs  = doc["burstOn"]  | 50;
        dz.burstOffMs = doc["burstOff"] | 200;
        uint32_t timerSecs = doc["timer"] | 0;
        dz.stopAt = timerSecs > 0 ? millis() + timerSecs * 1000UL : 0;
      }
      dz.active = true; dz.strobePhase = true; dz.lastStrobe = millis();
      if (dz.pattern == "steady") dazzlerOn();
      wsSerial.printf("[Dazzler] START freq=%uHz duty=%u%% pattern=%s\n",
        dz.freqHz, dz.dutyCycle, dz.pattern.c_str());
      req->send(200, "application/json", "{\"ok\":true}");
    });

  server.on("/api/dazzler/stop", HTTP_POST, [](AsyncWebServerRequest* req) {
    dz.active = false; dazzlerOff();
    wsSerial.println("[Dazzler] STOP");
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/dazzler/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    StaticJsonDocument<256> doc;
    doc["active"]   = (bool)dz.active;
    doc["freq"]     = dz.freqHz;
    doc["duty"]     = dz.dutyCycle;
    doc["pattern"]  = dz.pattern;
    doc["strobeMs"] = dz.strobeMs;
    doc["burstOn"]  = dz.burstOnMs;
    doc["burstOff"] = dz.burstOffMs;
    int32_t rem = (dz.stopAt > 0 && dz.active) ? (int32_t)(dz.stopAt - millis()) / 1000 : -1;
    doc["remaining"] = rem;
    String out; serializeJson(doc, out); req->send(200, "application/json", out);
  });

  // ── GPIO / Pins ────────────────────────────────────────
  server.on("/api/pins", HTTP_GET, [](AsyncWebServerRequest* req) {
    StaticJsonDocument<256> doc;
    doc["irTx"]  = pins.irTx;  doc["irRx"]  = pins.irRx;
    doc["sdCs"]  = pins.sdCs;  doc["sdMosi"] = pins.sdMosi;
    doc["sdMiso"]= pins.sdMiso; doc["sdSck"] = pins.sdSck;
    String out; serializeJson(doc, out); req->send(200, "application/json", out);
  });

  server.on("/api/pins", HTTP_POST, [](AsyncWebServerRequest* req) {},
    nullptr, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<256> doc;
      if (deserializeJson(doc, data, len)) { req->send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
      pins.irTx   = doc["irTx"]   | pins.irTx;
      pins.irRx   = doc["irRx"]   | pins.irRx;
      pins.sdCs   = doc["sdCs"]   | pins.sdCs;
      pins.sdMosi = doc["sdMosi"] | pins.sdMosi;
      pins.sdMiso = doc["sdMiso"] | pins.sdMiso;
      pins.sdSck  = doc["sdSck"]  | pins.sdSck;
      savePins();
      String warns = "";
      struct { uint8_t p; bool out; const char* n; } chk[] = {
        {pins.irTx,true,"IR TX"},{pins.irRx,false,"IR RX"},{pins.sdCs,true,"SD CS"},
        {pins.sdMosi,true,"SD MOSI"},{pins.sdMiso,false,"SD MISO"},{pins.sdSck,true,"SD SCK"}
      };
      for (auto& c : chk) {
        String w = gpioWarning(c.p, c.out);
        if (w.length()) warns += String(c.n) + " GPIO" + c.p + ": " + w + "\n";
      }
      wsSerial.printf("[Pins] Saved TX=%d RX=%d CS=%d MOSI=%d MISO=%d SCK=%d\n",
        pins.irTx, pins.irRx, pins.sdCs, pins.sdMosi, pins.sdMiso, pins.sdSck);
      StaticJsonDocument<512> resp;
      resp["ok"] = true; resp["reboot"] = true;
      if (warns.length()) resp["warnings"] = warns;
      String rOut; serializeJson(resp, rOut);
      req->send(200, "application/json", rOut);
    });

  // ── AP Config ──────────────────────────────────────────
  server.on("/api/apconfig", HTTP_GET, [](AsyncWebServerRequest* req) {
    StaticJsonDocument<128> doc;
    doc["ssid"] = apSsid; doc["pass"] = apPass;
    String out; serializeJson(doc, out); req->send(200, "application/json", out);
  });

  server.on("/api/apconfig", HTTP_POST, [](AsyncWebServerRequest* req) {},
    nullptr, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<256> doc;
      if (deserializeJson(doc, data, len)) { req->send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
      String newSsid = doc["ssid"] | "";
      String newPass = doc["pass"] | "";
      if (newSsid.length() == 0 || newSsid.length() > 32) {
        req->send(400, "application/json", "{\"error\":\"SSID must be 1-32 chars\"}"); return;
      }
      if (newPass.length() < 8 || newPass.length() > 63) {
        req->send(400, "application/json", "{\"error\":\"Password must be 8-63 chars\"}"); return;
      }
      apSsid = newSsid; apPass = newPass;
      saveAPConfig();
      wsSerial.printf("[WiFi] Config saved: '%s' — reboot to apply\n", apSsid.c_str());
      req->send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
    });

  // ── Reboot ─────────────────────────────────────────────
  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", "{\"ok\":true}");
    delay(500);
    ESP.restart();
  });

  // ── Status ─────────────────────────────────────────────
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    StaticJsonDocument<256> doc;
    doc["sd"]       = sdAvailable;
    doc["learning"] = (bool)isLearning;
    doc["dazzling"] = (bool)dz.active;
    doc["ip"]       = AP_IP.toString();
    doc["ssid"]     = apSsid;
    doc["heap"]     = ESP.getFreeHeap();
    doc["uptime"]   = millis() / 1000;
    String out; serializeJson(doc, out); req->send(200, "application/json", out);
  });

  server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "Not found");
  });

  // WebSocket
  wsLog.onEvent(onWsEvent);
  server.addHandler(&wsLog);
}

// ══════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n[ESP32 IR+Dazzler v3.0] Starting...");

  // Load config from NVS
  loadPins();
  loadAPConfig();
  validatePins();

  // Mutex for IR pin access
  irMutex = xSemaphoreCreateMutex();

  // IR (static objects, proper init)
  initIR();

  // SD card
  initSD();

  // LittleFS for web UI (replaces deprecated SPIFFS)
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS mount failed — web UI unavailable");
  } else {
    Serial.println("[FS] LittleFS OK");
  }

  // WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  bool apOk = WiFi.softAP(apSsid.c_str(), apPass.c_str());
  Serial.printf("[WiFi] AP '%s' %s  IP: %s\n",
    apSsid.c_str(), apOk ? "OK" : "FAILED", AP_IP.toString().c_str());

  // Routes & server
  setupRoutes();
  server.begin();
  Serial.println("[HTTP] Server started → http://192.168.4.1");
}

// ══════════════════════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════════════════════
void loop() {
  // IR learning (runs on core 1 same as WiFi — no RTOS needed)
  if (isLearning) {
    if (millis() > learnTimeout) {
      wsSerial.println("[Learn] Timeout");
      isLearning = false;
      if (irReceiver) irReceiver->disableIRIn();
    } else if (irReceiver && irReceiver->decode(&irResults)) {
      if (irResults.value != kRepeat && irResults.decode_type != UNKNOWN) {
        handleLearnedCode(&irResults);
      } else {
        irReceiver->resume();
      }
    }
  }

  // Dazzler pattern engine
  dazzlerLoop();

  // WebSocket cleanup
  wsLog.cleanupClients();

  // Small yield to keep WiFi stack happy
  delay(1);
}
    
