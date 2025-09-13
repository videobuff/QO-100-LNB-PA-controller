/*  QO-100 Controller – ESP32 (AsyncWebServer)
 *  Versie: v019.0
 *
 *  - AsyncWebServer + AsyncWebSocket (/ws)
 *  - WiFiManager captive portal
 *  - SPIFFS static files (index.html, style.css, script.js, ...)
 *  - BasicAuth uit Preferences, recovery via RECOVERY_PIN=4 (LOW bij boot => admin/password)
 *  - Settings in Preferences:
 *      forwardMax, reflectedMax, needleThickness, needleColor, swrThreshold,
 *      v5Scale, v12Scale, v18Scale, v28Scale, simMode
 *  - WebSocket broadcast elke seconde
 *  - Simdata met REF ≤ 40% van FOR
 *  - JSON responses voor /api/main_pa_toggle en /api/toggle
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <SPIFFS.h>

#define ARDUINOJSON_USE_DEPRECATED 0
#include <ArduinoJson.h>     // v7 API (JsonDocument)
#include <Preferences.h>
#include <Update.h>

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

// -------------------- Pins & hardware --------------------
const int mainPaPin    = 14;     // PA relais
const int polPin       = 26;     // SSB/DATV
const int RECOVERY_PIN = 4;      // LOW bij boot => login reset

// -------------------- Globals --------------------
bool  mainPaState    = false;
bool  swrLatched     = false;
float swrThreshold   = 3.0f;

float forwardPower   = 0.0f;
float reflectedPower = 0.0f;
float temperature    = 25.0f;

String firmwareVersion = "v019.0";

// simulatie
bool simMode = true;

// UI limieten (client kan ze opslaan/overschrijven)
float forwardMaxDefault   = 50.0f;
float reflectedMaxDefault = 25.0f;

// rails schalen
float v5Scale  = 1.6818f;
float v12Scale = 4.0909f;
float v18Scale = 6.4545f;
float v28Scale = 10.0909f;

// auth
String httpUser, httpPass;

// timing
unsigned long lastTick = 0;

// -------------------- Server & storage --------------------
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
Preferences preferences;

// -------------------- Helpers --------------------
static bool requireAuth(AsyncWebServerRequest* request) {
  if (!request->authenticate(httpUser.c_str(), httpPass.c_str())) {
    request->requestAuthentication();
    return false;
  }
  return true;
}

static void loadAuth() {
  preferences.begin("settings", true);
  httpUser = preferences.getString("httpUser", "admin");
  httpPass = preferences.getString("httpPass", "password");
  preferences.end();
}

static void saveAuth(const String& u, const String& p) {
  preferences.begin("settings", false);
  preferences.putString("httpUser", u);
  preferences.putString("httpPass", p);
  preferences.end();
}

static float computeSWR(float fwd, float refl) {
  if (fwd < 0.01f) return 1.0f;
  float r = refl / (fwd + 1e-6f);
  if (r < 0) r = 0;
  if (r > 0.9999f) r = 0.9999f;
  float gamma = sqrtf(r);
  float swr = (1 + gamma) / (1 - gamma);
  if (!isfinite(swr)) swr = 99.9f;
  return swr;
}

static void simulateMeasurements() {
  // forward 0..50
  forwardPower = random(0, 500) / 10.0f;
  // reflected ≤ 40% van forward
  float maxRef = forwardPower * 0.40f;
  reflectedPower = (maxRef <= 0 ? 0 : random(0, (int)(maxRef * 10.0f)) / 10.0f);
  // temp klein variëren
  temperature = 22.0f + random(-10, 11) * 0.2f;
}

static void broadcastStatus() {
  if (simMode) simulateMeasurements();
  float swr = computeSWR(forwardPower, reflectedPower);

  JsonDocument doc;
  doc["type"]         = "data";
  doc["forward"]      = forwardPower;
  doc["reflected"]    = reflectedPower;
  doc["swr"]          = swr;
  doc["temperature"]  = temperature;
  doc["mainPaState"]  = mainPaState ? "ON" : "OFF";
  doc["swrThreshold"] = swrThreshold;
  doc["version"]      = firmwareVersion;
  doc["sim"]          = simMode;

  String out;
  serializeJson(doc, out);
  ws.textAll(out);
}

// ---------- body helpers (raw body verzamelen voor AsyncWebServer) ----------
static void _accumulateBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
  if (index == 0) {
    req->_tempObject = new String();
    ((String*)req->_tempObject)->reserve(total);
  }
  String* buf = reinterpret_cast<String*>(req->_tempObject);
  buf->concat((const char*)data, len);
}

static String _takeBody(AsyncWebServerRequest* req) {
  String body;
  if (req->_tempObject) {
    body = *reinterpret_cast<String*>(req->_tempObject);
    delete reinterpret_cast<String*>(req->_tempObject);
    req->_tempObject = nullptr;
  }
  return body;
}

// -------------------- HTTP handlers --------------------
// MODE toggle -> JSON
static void onToggle(AsyncWebServerRequest* req) {
  if (!requireAuth(req)) return;
  digitalWrite(polPin, !digitalRead(polPin));
  bool ssb = digitalRead(polPin); // HIGH=SSB, LOW=DATV (voorbeeld)

  JsonDocument doc;
  doc["ok"]   = true;
  doc["mode"] = ssb ? "SSB" : "DATV";
  String s; serializeJson(doc, s);
  req->send(200, "application/json", s);
}

// PA toggle -> JSON
static void onMainPaToggle(AsyncWebServerRequest* req) {
  if (!requireAuth(req)) return;

  if (!swrLatched) {
    mainPaState = !mainPaState;
    digitalWrite(mainPaPin, mainPaState ? HIGH : LOW);
  }

  JsonDocument doc;
  doc["ok"]      = true;
  doc["state"]   = mainPaState ? "ON" : "OFF";
  doc["latched"] = swrLatched;
  String s; serializeJson(doc, s);
  req->send(200, "application/json", s);
}

static void onLatchSWR(AsyncWebServerRequest* req) {
  if (!requireAuth(req)) return;
  if (swrLatched) {
    req->send(400, "text/plain", "SWR already latched");
    return;
  }
  swrLatched  = true;
  mainPaState = false;
  digitalWrite(mainPaPin, LOW);

  JsonDocument doc;
  doc["ok"]           = true;
  doc["swrThreshold"] = swrThreshold;
  String s; serializeJson(doc, s);
  req->send(200, "application/json", s);
}

static void onResetSWR(AsyncWebServerRequest* req) {
  if (!requireAuth(req)) return;
  if (swrLatched) {
    swrLatched = false;
    req->send(200, "text/plain", "SWR_RESET");
  } else {
    req->send(200, "text/plain", "WAIT_TO_RESET");
  }
}

// /api/set_threshold  (JSON: {"threshold":4.2} of plain "3.8")
static void onSetThresholdBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
  if (!requireAuth(req)) return;
  _accumulateBody(req, data, len, index, total);
  if (index + len != total) return;

  String body = _takeBody(req);
  Serial.printf("[/api/set_threshold] raw: %s\n", body.c_str());

  float thr = NAN;
  if (body.startsWith("{")) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (!err && doc["threshold"].is<float>()) {
      thr = doc["threshold"].as<float>();
    }
  }
  if (isnan(thr)) {            // plain text fallback
    String t = body; t.trim();
    if (t.length()) thr = t.toFloat();
  }
  if (isnan(thr) || thr <= 0) {
    req->send(400, "text/plain", "Invalid threshold");
    return;
  }

  swrThreshold = thr;
  preferences.begin("settings", false);
  preferences.putFloat("swrThreshold", swrThreshold);
  preferences.end();
  req->send(200, "application/json", "{\"ok\":true}");
}

// /api/save_settings  (volledige JSON)
static void onSaveSettingsBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
  if (!requireAuth(req)) return;
  _accumulateBody(req, data, len, index, total);
  if (index + len != total) return;

  String body = _takeBody(req);
  Serial.printf("[/api/save_settings] raw: %s\n", body.c_str());
  if (body.isEmpty()) {
    req->send(400, "text/plain", "No settings provided");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    req->send(400, "text/plain", "Invalid JSON");
    return;
  }

  preferences.begin("settings", false);

  if (doc["forwardMax"].is<float>())        preferences.putFloat("forwardMax",      doc["forwardMax"].as<float>());
  if (doc["reflectedMax"].is<float>())      preferences.putFloat("reflectedMax",    doc["reflectedMax"].as<float>());
  if (doc["needleThickness"].is<int>())     preferences.putInt  ("needleThickness", doc["needleThickness"].as<int>());
  if (doc["needleColor"].is<const char*>()) preferences.putString("needleColor",     doc["needleColor"].as<const char*>());
  if (doc["swrThreshold"].is<float>())     {swrThreshold = doc["swrThreshold"].as<float>(); preferences.putFloat("swrThreshold", swrThreshold);}

  if (doc["v5Scale"].is<float>())   { v5Scale  = doc["v5Scale"].as<float>();  preferences.putFloat("v5Scale",  v5Scale); }
  if (doc["v12Scale"].is<float>())  { v12Scale = doc["v12Scale"].as<float>(); preferences.putFloat("v12Scale", v12Scale); }
  if (doc["v18Scale"].is<float>())  { v18Scale = doc["v18Scale"].as<float>(); preferences.putFloat("v18Scale", v18Scale); }
  if (doc["v28Scale"].is<float>())  { v28Scale = doc["v28Scale"].as<float>(); preferences.putFloat("v28Scale", v28Scale); }

  // optioneel login wijzigen
  if (doc["httpUser"].is<const char*>() && doc["httpPass"].is<const char*>()) {
    httpUser = doc["httpUser"].as<const char*>();
    httpPass = doc["httpPass"].as<const char*>();
    preferences.putString("httpUser", httpUser);
    preferences.putString("httpPass", httpPass);
  }

  if (doc["sim"].is<bool>()) {
    simMode = doc["sim"].as<bool>();
    preferences.putBool("simMode", simMode);
  }

  preferences.end();
  req->send(200, "application/json", "{\"ok\":true}");
}

// /api/load_settings  (GET)
static void onLoadSettings(AsyncWebServerRequest* req) {
  if (!requireAuth(req)) return;

  preferences.begin("settings", true);
  float forwardMax   = preferences.getFloat("forwardMax",      forwardMaxDefault);
  float reflectedMax = preferences.getFloat("reflectedMax",    reflectedMaxDefault);
  int   needleThick  = preferences.getInt  ("needleThickness", 2);
  String needleCol   = preferences.getString("needleColor",    "#000000");
  float thr          = preferences.getFloat("swrThreshold",    3.0f);

  v5Scale  = preferences.getFloat("v5Scale",  v5Scale);
  v12Scale = preferences.getFloat("v12Scale", v12Scale);
  v18Scale = preferences.getFloat("v18Scale", v18Scale);
  v28Scale = preferences.getFloat("v28Scale", v28Scale);

  simMode  = preferences.getBool("simMode", simMode);
  preferences.end();

  JsonDocument doc;
  doc["forwardMax"]      = forwardMax;
  doc["reflectedMax"]    = reflectedMax;
  doc["needleThickness"] = needleThick;
  doc["needleColor"]     = needleCol;
  doc["swrThreshold"]    = thr;

  doc["v5Scale"]  = v5Scale;
  doc["v12Scale"] = v12Scale;
  doc["v18Scale"] = v18Scale;
  doc["v28Scale"] = v28Scale;

  doc["sim"]      = simMode;

  String s; serializeJson(doc, s);
  req->send(200, "application/json", s);
}

static void onCheckUpdate(AsyncWebServerRequest* req) {
  if (!requireAuth(req)) return;
  req->send(200, "application/json",
            "{\"updateAvailable\":false,\"currentVersion\":\"v019.0\",\"latestVersion\":\"v019.0\",\"updateUrl\":\"\"}");
}

static void onUpdate(AsyncWebServerRequest* req) {
  if (!requireAuth(req)) return;
  req->send(200, "text/plain", "OK");
}

// -------------------- WebSocket --------------------
static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
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

// -------------------- Setup & Loop --------------------
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.printf("[boot] QO-100 controller %s …\n", firmwareVersion.c_str());

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed");
  }

  pinMode(mainPaPin, OUTPUT);
  pinMode(polPin, OUTPUT);
  pinMode(RECOVERY_PIN, INPUT_PULLUP);

  digitalWrite(polPin, HIGH); // start: SSB
  mainPaState = false;
  digitalWrite(mainPaPin, LOW);

  if (digitalRead(RECOVERY_PIN) == LOW) {
    saveAuth("admin", "password");
    Serial.println("[recovery] login reset naar admin/password");
  }
  loadAuth();

  preferences.begin("settings", false);
  swrThreshold = preferences.getFloat("swrThreshold", 3.0f);
  simMode      = preferences.getBool("simMode", true);
  preferences.end();

  WiFiManager wm;
  bool ok = wm.autoConnect("QO100-Controller");
  if (!ok) {
    Serial.println("WiFi connect failed, reboot in 5s");
    delay(5000);
    ESP.restart();
  }
  Serial.printf("WiFi connected. IP: %s\n", WiFi.localIP().toString().c_str());

  // Static files
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");

  // Eenvoudige endpoints
  server.on("/api/toggle",         HTTP_POST, onToggle);
  server.on("/api/main_pa_toggle", HTTP_POST, onMainPaToggle);
  server.on("/api/latch_swr",      HTTP_POST, onLatchSWR);
  server.on("/api/reset_swr",      HTTP_POST, onResetSWR);
  server.on("/api/load_settings",  HTTP_GET,  onLoadSettings);
  server.on("/api/check_update",   HTTP_GET,  onCheckUpdate);
  server.on("/api/update",         HTTP_POST, onUpdate);

  // Body endpoints
  server.on("/api/set_threshold", HTTP_POST,
            [](AsyncWebServerRequest* req){}, nullptr, onSetThresholdBody);

  server.on("/api/save_settings", HTTP_POST,
            [](AsyncWebServerRequest* req){}, nullptr, onSaveSettingsBody);

  // WebSocket
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.begin();
  Serial.printf("[server] started on %s\n", WiFi.localIP().toString().c_str());
}

void loop() {
  unsigned long now = millis();
  if (now - lastTick >= 1000) {
    lastTick = now;
    broadcastStatus();
  }
}
