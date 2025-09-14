/*
  QO-100 Controller – ESP32 (Async) – v023
  - WiFiManager + SPIFFS + ESPAsyncWebServer + AsyncWebSocket
  - JSON endpoints met AsyncCallbackJsonWebHandler (geen 400 Bad Request meer)
  - Settings in Preferences (namespace "settings")
  - SIM mode toggle (Preferences key "sim")
  - SWR latch: PA OFF wanneer SWR > threshold
  - WebSocket "hello" en periodieke "data"
*/

#include <WiFi.h>
#include <WiFiManager.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <Update.h>

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>

// -------------------- Pins --------------------
const int mainPaPin   = 14;   // PA relais
const int polPin      = 26;   // SSB/DATV
const int RECOVERY_PIN= 4;    // bij LOW op boot -> login reset

// -------------------- Globals --------------------
Preferences preferences;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

String firmwareVersion = "v016";

// auth
String httpUser;
String httpPass;

// status
bool mainPaState  = false;      // OFF
bool swrLatched   = false;
bool simMode      = true;

// settings (defaults)
float swrThreshold   = 3.0f;
float forwardMax     = 50.0f;
float reflectedMax   = 25.0f;
int   needleThickness= 2;
String needleColor   = "#000000";

// rail schaalfactoren (ADC->V): V = adc * scale
float v5Scale  = 1.6818f;
float v12Scale = 4.0909f;
float v18Scale = 6.4545f;
float v28Scale = 10.0909f;

// runtime samples
float forwardPower   = 0.0f;
float reflectedPower = 0.0f;
float temperature    = 25.0f;

unsigned long lastTick = 0;

// -------------------- Helpers --------------------
void saveAuth(const String& u, const String& p){
  preferences.begin("settings", false);
  preferences.putString("httpUser", u);
  preferences.putString("httpPass", p);
  preferences.end();
}
void loadAuth(){
  preferences.begin("settings", true);
  httpUser = preferences.getString("httpUser", "admin");
  httpPass = preferences.getString("httpPass", "password");
  preferences.end();
}
bool requireAuth(AsyncWebServerRequest* request){
  if (!request->authenticate(httpUser.c_str(), httpPass.c_str())) {
    request->requestAuthentication();
    return false;
  }
  return true;
}

void loadAllPrefs(){
  preferences.begin("settings", true);
  forwardMax      = preferences.getFloat("forwardMax",     50.0f);
  reflectedMax    = preferences.getFloat("reflectedMax",   25.0f);
  needleThickness = preferences.getInt  ("needleThickness",2);
  needleColor     = preferences.getString("needleColor",   "#000000");
  swrThreshold    = preferences.getFloat("swrThreshold",   3.0f);

  v5Scale  = preferences.getFloat("v5Scale",  1.6818f);
  v12Scale = preferences.getFloat("v12Scale", 4.0909f);
  v18Scale = preferences.getFloat("v18Scale", 6.4545f);
  v28Scale = preferences.getFloat("v28Scale", 10.0909f);

  simMode  = preferences.getBool("sim", true);
  preferences.end();
}
void saveSimPref(bool v){
  simMode = v;
  preferences.begin("settings", false);
  preferences.putBool("sim", simMode);
  preferences.end();
}

float computeSWR(float fwd, float refl){
  if (fwd <= 0.01f) return 1.0f;
  float r = refl / (fwd + 1e-6f);
  r = constrain(r, 0.0f, 0.9999f);
  float gamma = sqrtf(r);
  float swr = (1 + gamma) / (1 - gamma);
  if (!isfinite(swr)) swr = 99.9f;
  return swr;
}

// simulatie data, gereflecteerd max 40% van forward
void simulateSamples(){
  forwardPower   = random(0, (int)(forwardMax * 10)) / 10.0f; // in stapjes 0.1
  float reflCap  = min(reflectedMax, forwardPower * 0.4f);
  reflectedPower = random(0, (int)(reflCap * 10)) / 10.0f;
  temperature    = 20.0f + (random(0, 120) / 10.0f); // 20.0..32.0
}

void sendHello(AsyncWebSocketClient* client){
  JsonDocument hello;
  hello["type"]    = "hello";
  hello["version"] = firmwareVersion;
  hello["sim"]     = simMode;
  String s; serializeJson(hello, s);
  client->text(s);
}

void broadcastStatus(){
  if (simMode) {
    simulateSamples();
  } else {
    // TODO: echte ADC-metingen i.c.m. je schaalfactoren
    simulateSamples(); // tijdelijk, tot de echte ingangen zijn aangesloten
  }

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

  String json; serializeJson(doc, json);
  ws.textAll(json);
}

// -------------------- REST handlers --------------------
void onToggle(AsyncWebServerRequest* request){
  if (!requireAuth(request)) return;
  // toggle pol pin
  digitalWrite(polPin, !digitalRead(polPin));
  bool ssb = (digitalRead(polPin) == HIGH);
  JsonDocument doc;
  doc["mode"] = ssb ? "SSB" : "DATV";
  String s; serializeJson(doc, s);
  request->send(200, "application/json", s);
}

void onMainPaToggle(AsyncWebServerRequest* request){
  if (!requireAuth(request)) return;
  if (!swrLatched) {
    mainPaState = !mainPaState;
    digitalWrite(mainPaPin, mainPaState ? HIGH : LOW);
  }
  JsonDocument doc;
  doc["state"] = mainPaState ? "ON" : "OFF";
  String s; serializeJson(doc, s);
  request->send(200, "application/json", s);
}

void onLatchSWR(AsyncWebServerRequest* request){
  if (!requireAuth(request)) return;
  if (!swrLatched) {
    swrLatched = true;
    mainPaState = false;
    digitalWrite(mainPaPin, LOW);
    JsonDocument doc;
    doc["swrThreshold"] = swrThreshold;
    String s; serializeJson(doc, s);
    request->send(200, "application/json", s);
  } else {
    request->send(400, "text/plain", "SWR already latched");
  }
}

void onResetSWR(AsyncWebServerRequest* request){
  if (!requireAuth(request)) return;
  if (swrLatched) {
    swrLatched = false;
    request->send(200, "text/plain", "SWR_RESET");
  } else {
    request->send(200, "text/plain", "WAIT_TO_RESET");
  }
}

void onLoadSettings(AsyncWebServerRequest* request){
  if (!requireAuth(request)) return;

  // read fresh from NVS to reflect latest saved values
  loadAllPrefs();

  JsonDocument doc;
  doc["forwardMax"]      = forwardMax;
  doc["reflectedMax"]    = reflectedMax;
  doc["needleThickness"] = needleThickness;
  doc["needleColor"]     = needleColor;
  doc["swrThreshold"]    = swrThreshold;

  doc["v5Scale"]  = v5Scale;
  doc["v12Scale"] = v12Scale;
  doc["v18Scale"] = v18Scale;
  doc["v28Scale"] = v28Scale;

  doc["sim"]      = simMode;

  String s; serializeJson(doc, s);
  request->send(200, "application/json", s);
}

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.printf("[boot] QO-100 controller %s …\n", firmwareVersion.c_str());

  // SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed");
  }

  // Pins
  pinMode(mainPaPin, OUTPUT);
  pinMode(polPin, OUTPUT);
  pinMode(RECOVERY_PIN, INPUT_PULLUP);

  // Beginstand
  digitalWrite(mainPaPin, LOW);  mainPaState = false;   // OFF
  digitalWrite(polPin, HIGH);    // SSB

  // recovery auth?
  if (digitalRead(RECOVERY_PIN) == LOW) {
    saveAuth("admin","password");
    Serial.println("[recovery] login reset to admin/password");
  }
  loadAuth();

  // Load settings
  loadAllPrefs();

  // WiFi via WiFiManager
  WiFiManager wm;
  bool ok = wm.autoConnect("QO100-Controller");
  if (!ok) {
    Serial.println("WiFi connect failed, rebooting in 5s");
    delay(5000);
    ESP.restart();
  }
  Serial.print("WiFi connected. IP: ");
  Serial.println(WiFi.localIP());
  Serial.printf("[server] started on %s\n", WiFi.localIP().toString().c_str());

  // Static files
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");

  // --------- JSON save_settings handler ----------
  auto saveHandler = new AsyncCallbackJsonWebHandler("/api/save_settings",
    [](AsyncWebServerRequest* request, JsonVariant &json){
      if (!requireAuth(request)) { request->requestAuthentication(); return; }
      JsonObject doc = json.as<JsonObject>();

      // Update globals + Preferences (met defensieve checks)
      preferences.begin("settings", false);

      if (doc["forwardMax"].is<float>()){
        forwardMax = doc["forwardMax"].as<float>();
        preferences.putFloat("forwardMax", forwardMax);
      }
      if (doc["reflectedMax"].is<float>()){
        reflectedMax = doc["reflectedMax"].as<float>();
        preferences.putFloat("reflectedMax", reflectedMax);
      }
      if (doc["needleThickness"].is<int>()){
        needleThickness = doc["needleThickness"].as<int>();
        preferences.putInt("needleThickness", needleThickness);
      }
      if (doc["needleColor"].is<const char*>()){
        needleColor = String(doc["needleColor"].as<const char*>());
        preferences.putString("needleColor", needleColor);
      }
      if (doc["swrThreshold"].is<float>()){
        swrThreshold = doc["swrThreshold"].as<float>();
        preferences.putFloat("swrThreshold", swrThreshold);
      }

      if (doc["v5Scale"].is<float>()){
        v5Scale = doc["v5Scale"].as<float>();
        preferences.putFloat("v5Scale", v5Scale);
      }
      if (doc["v12Scale"].is<float>()){
        v12Scale = doc["v12Scale"].as<float>();
        preferences.putFloat("v12Scale", v12Scale);
      }
      if (doc["v18Scale"].is<float>()){
        v18Scale = doc["v18Scale"].as<float>();
        preferences.putFloat("v18Scale", v18Scale);
      }
      if (doc["v28Scale"].is<float>()){
        v28Scale = doc["v28Scale"].as<float>();
        preferences.putFloat("v28Scale", v28Scale);
      }

      if (doc["sim"].is<bool>()){
        saveSimPref(doc["sim"].as<bool>());
      }

      // optioneel: httpUser/httpPass
      if (doc["httpUser"].is<const char*>() && doc["httpPass"].is<const char*>()){
        String u = String(doc["httpUser"].as<const char*>());
        String p = String(doc["httpPass"].as<const char*>());
        preferences.putString("httpUser", u);
        preferences.putString("httpPass", p);
        httpUser=u; httpPass=p;
      }

      preferences.end();

      request->send(200, "application/json", "{\"ok\":true}");
    }
  );
  saveHandler->setMethod(HTTP_POST);
  server.addHandler(saveHandler);

  // JSON set_threshold (achterwaarts compatibel met UI die dit los zet)
  auto thrHandler = new AsyncCallbackJsonWebHandler("/api/set_threshold",
    [](AsyncWebServerRequest* request, JsonVariant &json){
      if (!requireAuth(request)) { request->requestAuthentication(); return; }
      JsonObject doc = json.as<JsonObject>();
      if (doc["threshold"].is<float>()){
        swrThreshold = doc["threshold"].as<float>();
        preferences.begin("settings", false);
        preferences.putFloat("swrThreshold", swrThreshold);
        preferences.end();
        request->send(200, "text/plain", "Threshold set");
      } else {
        request->send(400, "text/plain", "Missing 'threshold' (float)");
      }
    }
  );
  thrHandler->setMethod(HTTP_POST);
  server.addHandler(thrHandler);

  // overige endpoints
  server.on("/api/toggle",         HTTP_POST, onToggle);
  server.on("/api/main_pa_toggle", HTTP_POST, onMainPaToggle);
  server.on("/api/latch_swr",      HTTP_POST, onLatchSWR);
  server.on("/api/reset_swr",      HTTP_POST, onResetSWR);
  server.on("/api/load_settings",  HTTP_GET,  onLoadSettings);

  server.on("/api/check_update",   HTTP_GET, [](AsyncWebServerRequest* req){
    if (!requireAuth(req)) return;
    req->send(200, "application/json",
      "{\"updateAvailable\":false,\"currentVersion\":\"v023\",\"latestVersion\":\"v023\",\"updateUrl\":\"\"}");
  });

  // dummy update endpoint (URL-based); upload-variant kan later
  server.on("/api/update", HTTP_POST, [](AsyncWebServerRequest* req){
    if (!requireAuth(req)) return;
    req->send(200, "text/plain", "UPDATE_SUCCESS");
  });

  // WebSocket
  ws.onEvent([](AsyncWebSocket* server, AsyncWebSocketClient* client,
                AwsEventType type, void* arg, uint8_t* data, size_t len){
    if (type == WS_EVT_CONNECT) {
      sendHello(client);
    }
  });
  server.addHandler(&ws);

  // start
  server.begin();
}

void loop(){
  unsigned long now = millis();
  if (now - lastTick >= 1000) {
    lastTick = now;
    broadcastStatus();
  }
}
