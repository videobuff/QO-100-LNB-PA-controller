/*
  QO-100 Controller — Classic ESP32 (WROOM) — v016b-jsonhandler

  Wat is nieuw t.o.v. v016-json7:
  - JSON parsing via AsyncCallbackJsonWebHandler voor /api/save_settings en /api/sim
    -> fixt 400 Bad Request bij fetch/curl met application/json.
  - /api/sim blijft ook werken zonder body (toggle).

  Vereist libs:
    - ESP Async WebServer 3.8.0+
    - AsyncTCP (ESP32)
    - ArduinoJson v7.x
    - WiFiManager
*/

#include <WiFi.h>
#include <WiFiManager.h>
#include <SPIFFS.h>
#include <FS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>     // <<-- BELANGRIJK: voor AsyncCallbackJsonWebHandler
#include <Update.h>

// ---------------- Pins (classic WROOM) ----------------
const int PIN_MAIN_PA   = 14;  // PA relais
const int PIN_POL       = 26;  // SSB/DATV
const int PIN_RECOVERY  = 4;   // LOW bij boot -> reset login

// Analoge pinnen (WROOM ADC1: 36,39,34,35,32,33 e.d.)
const int PIN_ADC_FOR   = 36;  // Forward power
const int PIN_ADC_REF   = 39;  // Reflected power
const int PIN_ADC_5V    = 34;  // rails
const int PIN_ADC_12V   = 35;
const int PIN_ADC_18V   = 32;
const int PIN_ADC_28V   = 33;

// --------------- Globals ----------------
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
Preferences preferences;

String  firmwareVersion = "v016b-jsonhandler";
String  httpUser = "admin";
String  httpPass = "password";

bool    mainPaState   = false;
bool    swrLatched    = false;
bool    modeIsSSB     = true;
bool    simMode       = true;

float   swrThreshold  = 3.5f;
float   forwardPower  = 0.0f;
float   reflectedPower= 0.0f;
float   temperature   = 25.0f;

float   v5Scale  = 1.6818f;
float   v12Scale = 4.0909f;
float   v18Scale = 6.4545f;
float   v28Scale = 10.0909f;

float   uiForwardMax   = 50.0f;
float   uiReflectedMax = 25.0f;
int     uiNeedleThick  = 2;
String  uiNeedleColor  = "#000000";

unsigned long lastTick = 0;

// --------------- Helpers ----------------
static inline bool requireAuth(AsyncWebServerRequest* request) {
  if (!request->authenticate(httpUser.c_str(), httpPass.c_str())) {
    request->requestAuthentication();
    return false;
  }
  return true;
}

void loadPrefs() {
  preferences.begin("settings", true);
  httpUser       = preferences.getString("httpUser", "admin");
  httpPass       = preferences.getString("httpPass", "password");

  uiForwardMax   = preferences.getFloat("forwardMax", 50.0f);
  uiReflectedMax = preferences.getFloat("reflectedMax", 25.0f);
  uiNeedleThick  = preferences.getInt  ("needleThickness", 2);
  uiNeedleColor  = preferences.getString("needleColor", "#000000");
  swrThreshold   = preferences.getFloat("swrThreshold", 3.5f);

  v5Scale        = preferences.getFloat("v5Scale",  1.6818f);
  v12Scale       = preferences.getFloat("v12Scale", 4.0909f);
  v18Scale       = preferences.getFloat("v18Scale", 6.4545f);
  v28Scale       = preferences.getFloat("v28Scale",10.0909f);

  simMode        = preferences.getBool ("sim", true);
  preferences.end();
}

void saveAuthDefaultsIfRecovery() {
  pinMode(PIN_RECOVERY, INPUT_PULLUP);
  if (digitalRead(PIN_RECOVERY) == LOW) {
    preferences.begin("settings", false);
    preferences.putString("httpUser", "admin");
    preferences.putString("httpPass", "password");
    preferences.end();
    Serial.println("[recovery] login reset to admin/password");
  }
}

static inline float computeSWR(float fwd, float ref) {
  if (fwd <= 0.01f) return 1.0f;
  float r = ref / (fwd + 1e-6f);
  r = constrain(r, 0.0f, 0.9999f);
  float gamma = sqrtf(r);
  float swr = (1 + gamma) / (1 - gamma);
  if (!isfinite(swr)) swr = 99.9f;
  return swr;
}

float readADCScaled(int pin, float scale) {
  int raw = analogRead(pin);      // 0..4095 (ADC1 default 12-bit)
  float v  = (raw / 4095.0f) * 3.3f * scale;
  return v;
}

void sampleReal() {
  float vFor = readADCScaled(PIN_ADC_FOR, 1.0f);
  float vRef = readADCScaled(PIN_ADC_REF, 1.0f);
  if (vRef > 0.4f * vFor) vRef = 0.4f * vFor;

  forwardPower   = vFor * uiForwardMax / 3.3f;
  reflectedPower = vRef * uiReflectedMax / 3.3f;
  temperature    = 25.0f + 2.0f * sinf(millis() / 5000.0f);
}

void sampleSim() {
  float base = 10.0f + 10.0f * sinf(millis() / 1500.0f);
  forwardPower   = constrain(base + random(-20, 21) / 10.0f, 0.0f, uiForwardMax);
  float maxRef   = 0.4f * forwardPower;
  reflectedPower = constrain((forwardPower * 0.2f) + random(-10, 11) / 10.0f, 0.0f, maxRef);
  temperature    = 28.0f + 1.5f * sinf(millis() / 6000.0f);
}

void broadcastStatus() {
  if (simMode) sampleSim();
  else         sampleReal();

  float swr = computeSWR(forwardPower, reflectedPower);

  if (!swrLatched && (swr > swrThreshold)) {
    swrLatched = true;
    mainPaState = false;
    digitalWrite(PIN_MAIN_PA, LOW);
  }

  JsonDocument doc;
  doc["type"]          = "data";
  doc["forward"]       = forwardPower;
  doc["reflected"]     = reflectedPower;
  doc["swr"]           = swr;
  doc["temperature"]   = temperature;
  doc["mainPaState"]   = mainPaState ? "ON" : "OFF";
  doc["swrThreshold"]  = swrThreshold;
  doc["version"]       = firmwareVersion;
  doc["mode"]          = modeIsSSB ? "SSB" : "DATV";
  doc["sim"]           = simMode;

  doc["v5"]  = readADCScaled(PIN_ADC_5V,  v5Scale);
  doc["v12"] = readADCScaled(PIN_ADC_12V, v12Scale);
  doc["v18"] = readADCScaled(PIN_ADC_18V, v18Scale);
  doc["v28"] = readADCScaled(PIN_ADC_28V, v28Scale);

  doc["ui"]["forwardMax"]      = uiForwardMax;
  doc["ui"]["reflectedMax"]    = uiReflectedMax;
  doc["ui"]["needleThickness"] = uiNeedleThick;
  doc["ui"]["needleColor"]     = uiNeedleColor;

  String json; serializeJson(doc, json);
  ws.textAll(json);
}

// --------------- Basic handlers ----------------
void handleMainPaToggle(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) return;

  float swrNow = computeSWR(forwardPower, reflectedPower);
  if (swrLatched && (swrNow <= (swrThreshold - 0.2f))) {
    swrLatched = false;
  }

  if (swrLatched) {
    mainPaState = false;
  } else {
    mainPaState = !mainPaState;
  }
  digitalWrite(PIN_MAIN_PA, mainPaState ? HIGH : LOW);

  JsonDocument resp;
  resp["state"] = mainPaState ? "ON" : "OFF";
  String out; serializeJson(resp, out);
  request->send(200, "application/json", out);
}

void handleToggleMode(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) return;
  modeIsSSB = !modeIsSSB;
  digitalWrite(PIN_POL, modeIsSSB ? HIGH : LOW);

  JsonDocument resp;
  resp["mode"] = modeIsSSB ? "SSB" : "DATV";
  String out; serializeJson(resp, out);
  request->send(200, "application/json", out);
}

void handleLatchSWR(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) return;
  if (!swrLatched) {
    swrLatched = true;
    mainPaState = false;
    digitalWrite(PIN_MAIN_PA, LOW);

    JsonDocument resp;
    resp["swrThreshold"] = swrThreshold;
    String out; serializeJson(resp, out);
    request->send(200, "application/json", out);
  } else {
    request->send(400, "text/plain", "SWR already latched");
  }
}

void handleResetSWR(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) return;
  float swrNow = computeSWR(forwardPower, reflectedPower);
  if (swrLatched && (swrNow <= (swrThreshold - 0.2f))) {
    swrLatched = false;
    request->send(200, "text/plain", "SWR_RESET");
  } else if (!swrLatched) {
    request->send(200, "text/plain", "WAIT_TO_RESET");
  } else {
    request->send(200, "text/plain", "WAIT_TO_RESET");
  }
}

void handleCheckUpdate(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) return;
  String resp = "{\"updateAvailable\":false,\"currentVersion\":\"" + firmwareVersion + "\",\"latestVersion\":\"" + firmwareVersion + "\",\"updateUrl\":\"\"}";
  request->send(200, "application/json", resp);
}

void onUpdateUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
  if (!requireAuth(request)) return;
  if (index == 0) {
    if (!Update.begin()) {
      Update.printError(Serial);
    }
  }
  if (Update.write(data, len) != len) {
    Update.printError(Serial);
  }
  if (final) {
    if (Update.end(true)) {
      ws.textAll("{\"type\":\"update_progress\",\"status\":\"success\",\"progress\":100}");
    } else {
      Update.printError(Serial);
    }
  }
}
void handleUpdate(AsyncWebServerRequest* request){
  if (!requireAuth(request)) return;
  request->send(200, "text/plain", "OK");
}

// POST /api/sim zonder body => toggle (fallback)
void handleSimToggleNoBody(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) return;
  // geen body => toggle
  preferences.begin("settings", false);
  simMode = !simMode;
  preferences.putBool("sim", simMode);
  preferences.end();

  JsonDocument resp;
  resp["sim"] = simMode;
  String out; serializeJson(resp, out);
  request->send(200, "application/json", out);
}

// --------------- WebSocket ----------------
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    JsonDocument hello;
    hello["type"]    = "hello";
    hello["version"] = firmwareVersion;
    hello["sim"]     = simMode;
    String s; serializeJson(hello, s);
    client->text(s);
  }
}

// --------------- Setup / Loop ----------------
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.printf("[boot] QO-100 controller %s …\n", firmwareVersion.c_str());

  pinMode(PIN_MAIN_PA, OUTPUT);
  pinMode(PIN_POL, OUTPUT);
  mainPaState = false;
  digitalWrite(PIN_MAIN_PA, LOW);
  modeIsSSB = true;
  digitalWrite(PIN_POL, HIGH);

  analogReadResolution(12);

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed");
  }

  saveAuthDefaultsIfRecovery();
  loadPrefs();

  WiFiManager wm;
  bool ok = wm.autoConnect("QO100-Controller");
  if (!ok) {
    Serial.println("WiFi connect failed, reboot in 5s");
    delay(5000);
    ESP.restart();
  }
  Serial.print("WiFi connected. IP: ");
  Serial.println(WiFi.localIP());
  Serial.printf("[server] started on %s\n", WiFi.localIP().toString().c_str());

  // Static files
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");

  // --- JSON handlers (fix 400’s) ---
  // /api/save_settings (JSON body verplicht bij deze handler)
  auto saveHandler = new AsyncCallbackJsonWebHandler("/api/save_settings",
    [](AsyncWebServerRequest *request, JsonVariant &json) {
      if (!requireAuth(request)) return;

      JsonObject obj = json.as<JsonObject>();
      preferences.begin("settings", false);

      if (obj["forwardMax"].is<float>())      { uiForwardMax   = obj["forwardMax"].as<float>();      preferences.putFloat("forwardMax", uiForwardMax); }
      if (obj["reflectedMax"].is<float>())    { uiReflectedMax = obj["reflectedMax"].as<float>();    preferences.putFloat("reflectedMax", uiReflectedMax); }
      if (obj["needleThickness"].is<int>())   { uiNeedleThick  = obj["needleThickness"].as<int>();   preferences.putInt  ("needleThickness", uiNeedleThick); }
      if (obj["needleColor"].is<const char*>()){ uiNeedleColor  = obj["needleColor"].as<const char*>(); preferences.putString("needleColor", uiNeedleColor); }
      if (obj["swrThreshold"].is<float>())    { swrThreshold   = obj["swrThreshold"].as<float>();    preferences.putFloat("swrThreshold", swrThreshold); }

      if (obj["v5Scale"].is<float>())  { v5Scale  = obj["v5Scale"].as<float>();  preferences.putFloat("v5Scale",  v5Scale); }
      if (obj["v12Scale"].is<float>()) { v12Scale = obj["v12Scale"].as<float>(); preferences.putFloat("v12Scale", v12Scale); }
      if (obj["v18Scale"].is<float>()) { v18Scale = obj["v18Scale"].as<float>(); preferences.putFloat("v18Scale", v18Scale); }
      if (obj["v28Scale"].is<float>()) { v28Scale = obj["v28Scale"].as<float>(); preferences.putFloat("v28Scale", v28Scale); }

      if (obj["sim"].is<bool>())       { simMode = obj["sim"].as<bool>();        preferences.putBool ("sim", simMode); }

      if (obj["httpUser"].is<const char*>() && obj["httpPass"].is<const char*>()) {
        httpUser = obj["httpUser"].as<const char*>();
        httpPass = obj["httpPass"].as<const char*>();
        preferences.putString("httpUser", httpUser);
        preferences.putString("httpPass", httpPass);
      }

      preferences.end();

      JsonDocument resp;
      resp["ok"]              = true;
      resp["forwardMax"]      = uiForwardMax;
      resp["reflectedMax"]    = uiReflectedMax;
      resp["needleThickness"] = uiNeedleThick;
      resp["needleColor"]     = uiNeedleColor;
      resp["swrThreshold"]    = swrThreshold;
      resp["v5Scale"]         = v5Scale;
      resp["v12Scale"]        = v12Scale;
      resp["v18Scale"]        = v18Scale;
      resp["v28Scale"]        = v28Scale;
      resp["sim"]             = simMode;

      String out; serializeJson(resp, out);
      request->send(200, "application/json", out);
    }
  );
  saveHandler->setMethod(HTTP_POST);
  server.addHandler(saveHandler);

  // /api/sim (JSON met {"sim": true/false}), PLUS fallback zonder body => toggle
  auto simHandler = new AsyncCallbackJsonWebHandler("/api/sim",
    [](AsyncWebServerRequest *request, JsonVariant &json) {
      if (!requireAuth(request)) return;
      JsonObject obj = json.as<JsonObject>();
      bool changed = false;

      if (obj["sim"].is<bool>()) {
        bool newSim = obj["sim"].as<bool>();
        if (newSim != simMode) {
          simMode = newSim;
          changed = true;
        }
      } else {
        // geen geldige key -> toggle (zodat er nooit 400’s komen)
        simMode = !simMode;
        changed = true;
      }

      if (changed) {
        preferences.begin("settings", false);
        preferences.putBool("sim", simMode);
        preferences.end();
      }

      JsonDocument resp;
      resp["sim"] = simMode;
      String out; serializeJson(resp, out);
      request->send(200, "application/json", out);
    }
  );
  simHandler->setMethod(HTTP_POST);
  server.addHandler(simHandler);

  // --- overige API routes ---
  server.on("/api/main_pa_toggle", HTTP_POST, handleMainPaToggle);
  server.on("/api/toggle",         HTTP_POST, handleToggleMode);
  server.on("/api/latch_swr",      HTTP_POST, handleLatchSWR);
  server.on("/api/reset_swr",      HTTP_POST, handleResetSWR);

  server.on("/api/load_settings",  HTTP_GET, [](AsyncWebServerRequest* request){
    if (!requireAuth(request)) return;
    loadPrefs();
    JsonDocument doc;
    doc["forwardMax"]      = uiForwardMax;
    doc["reflectedMax"]    = uiReflectedMax;
    doc["needleThickness"] = uiNeedleThick;
    doc["needleColor"]     = uiNeedleColor;
    doc["swrThreshold"]    = swrThreshold;
    doc["v5Scale"]         = v5Scale;
    doc["v12Scale"]        = v12Scale;
    doc["v18Scale"]        = v18Scale;
    doc["v28Scale"]        = v28Scale;
    doc["sim"]             = simMode;
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/api/check_update",   HTTP_GET,  handleCheckUpdate);
  server.on("/api/update",         HTTP_POST, handleUpdate, onUpdateUpload);

  // Fallback: /api/sim zonder JSON body => toggle
  server.on("/api/sim", HTTP_POST, handleSimToggleNoBody);

  // WebSocket
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.begin();
}

void loop() {
  unsigned long now = millis();
  if (now - lastTick >= 1000) {
    lastTick = now;
    broadcastStatus();
  }
}
