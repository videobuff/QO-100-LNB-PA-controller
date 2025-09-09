/*
 * Project: QO-100 LNB/PA Controller
 * Author: PA0ESH (Erik Schott)
 * Year: 2025
 * License: MIT
 * Version: 015
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

#define FSYS LittleFS
#define LOG(...) do{ if (Serial) Serial.printf(__VA_ARGS__); }while(0)

// ===== Pins =====
const int mainPaPin = 14;
const int polPin    = 26;

// ADC1 (werkt met Wi-Fi)
const int ADC_FOR   = 34;  // forward power
const int ADC_REF   = 35;  // reflected power
const int ADC_5V    = 32;  // 5V rail
const int ADC_12V   = 33;  // 12V rail
const int ADC_18V   = 36;  // 18V rail
const int ADC_28V   = 39;  // 28V rail

// ===== Globals =====
bool  mainPaState     = false;
bool  swrLatched      = false;
float swrThreshold    = 3.0f;

float forwardPower    = 0.0f;
float reflectedPower  = 0.0f;
float temperature     = 25.0f;

bool  simMode         = true;      // start SIM

// Power schaling (W = Vadc * WperV)
float fwdWperV        = 10.0f;
float refWperV        = 10.0f;

// Rail-schaalfactoren (Vin = Vadc * K) — defaults voor voorgestelde delers
float v5Scale   = 1.6818f;  // 15k/22k
float v12Scale  = 4.0909f;  // 68k/22k
float v18Scale  = 6.4545f;  // 120k/22k
float v28Scale  = 10.0909f; // 200k/22k

float rail5V    = 0.0f;
float rail12V   = 0.0f;
float rail18V   = 0.0f;
float rail28V   = 0.0f;

// === Versie ===
String firmwareVersion = "11/09/2025-015";

unsigned long lastTick = 0;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
Preferences preferences;

// ===== Helpers =====
float computeSWR(float fwd, float refl) {
  if (fwd <= 0.01f) return 1.0f;
  float r = refl / (fwd + 1e-6f);
  r = constrain(r, 0.0f, 0.9999f);
  float g = sqrtf(r);
  float s = (1 + g) / (1 - g);
  if (!isfinite(s)) s = 99.9f;
  return s;
}

bool readJsonBody(AsyncWebServerRequest* req, JsonDocument& doc, String& err) {
  if (!req->hasParam("plain", true)) { err = "empty body"; return false; }
  String body = req->getParam("plain", true)->value();
  if (body.length() == 0) { err = "empty body"; return false; }
  DeserializationError e = deserializeJson(doc, body);
  if (e) { err = e.f_str(); return false; }
  return true;
}

void sendJson(AsyncWebServerRequest* req, JsonDocument& doc, int code=200) {
  String out; serializeJson(doc, out);
  auto* resp = req->beginResponse(code, "application/json", out);
  resp->addHeader("Cache-Control", "no-store, max-age=0, must-revalidate");
  req->send(resp);
}

float adcToVolt(uint16_t raw) {
  return raw * (3.3f / 4095.0f);
}

uint16_t readADCavg(int pin, int N=8) {
  uint32_t acc = 0;
  for (int i=0;i<N;i++){ acc += analogRead(pin); delayMicroseconds(200); }
  return (uint16_t)(acc / (uint32_t)N);
}

void readSensors() {
  if (simMode) {
    // Forward tussen 0…50 W
    forwardPower = random(0, 500) / 10.0f;         // 0.0 … 50.0
    // Reflected max 50% van Forward
    float reflMax = 0.5f * forwardPower;
    reflectedPower = (random(0, 1000) / 1000.0f) * reflMax;
    // dummy temp
    temperature = 22.0f + 3.0f * sinf((millis()%10000)/1000.0f);
    // demo rails (redelijk stabiel, kleine jitter)
    rail5V  = 5.00f  + ((random(-5,6))/100.0f);   // ±0.05V
    rail12V = 12.0f  + ((random(-10,11))/100.0f); // ±0.10V
    rail18V = 18.0f  + ((random(-15,16))/100.0f); // ±0.15V
    rail28V = 28.0f  + ((random(-20,21))/100.0f); // ±0.20V
    return;
  }

  // Power kanalen
  uint16_t rF = readADCavg(ADC_FOR);
  uint16_t rR = readADCavg(ADC_REF);
  float vF = adcToVolt(rF);
  float vR = adcToVolt(rR);
  forwardPower   = vF * fwdWperV;
  reflectedPower = vR * refWperV;

  // Rail metingen
  analogSetPinAttenuation(ADC_5V,  ADC_11db);
  analogSetPinAttenuation(ADC_12V, ADC_11db);
  analogSetPinAttenuation(ADC_18V, ADC_11db);
  analogSetPinAttenuation(ADC_28V, ADC_11db);

  float v5adc   = adcToVolt(readADCavg(ADC_5V));
  float v12adc  = adcToVolt(readADCavg(ADC_12V));
  float v18adc  = adcToVolt(readADCavg(ADC_18V));
  float v28adc  = adcToVolt(readADCavg(ADC_28V));

  rail5V  = v5adc  * v5Scale;
  rail12V = v12adc * v12Scale;
  rail18V = v18adc * v18Scale;
  rail28V = v28adc * v28Scale;

  // temp placeholder
  temperature = 25.0f + 0.2f * sinf((millis()%10000)/1000.0f);
}

void broadcastStatus() {
  readSensors();
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
  doc["simMode"]      = simMode;

  // nieuwe rails
  doc["v5"]   = rail5V;
  doc["v12"]  = rail12V;
  doc["v18"]  = rail18V;
  doc["v28"]  = rail28V;

  String json; serializeJson(doc, json);
  ws.textAll(json);
}

// ===== Handlers =====
void onStatus(AsyncWebServerRequest* req){
  JsonDocument d;
  d["ok"]          = true;
  d["mainPaState"] = mainPaState ? "ON" : "OFF";
  d["pol"]         = digitalRead(polPin) ? "VERTICAL" : "HORIZONTAL";
  d["swrLatched"]  = swrLatched;
  d["swrThreshold"]= swrThreshold;
  d["version"]     = firmwareVersion;
  d["simMode"]     = simMode;

  d["v5"]  = rail5V;  d["v12"] = rail12V;
  d["v18"] = rail18V; d["v28"] = rail28V;

  sendJson(req, d);
}

void onToggle(AsyncWebServerRequest* request) {
  bool old = digitalRead(polPin);
  digitalWrite(polPin, !old);
  bool vertical = digitalRead(polPin);
  LOG("[toggle] pol %s -> %s\n", old?"VERTICAL":"HORIZONTAL", vertical?"VERTICAL":"HORIZONTAL");
  JsonDocument doc; doc["ok"]=true; doc["mode"]= vertical ? "VERTICAL" : "HORIZONTAL";
  sendJson(request, doc);
}

void onMainPaToggle(AsyncWebServerRequest* request) {
  JsonDocument doc;
  if (!swrLatched) {
    mainPaState = !mainPaState;
    digitalWrite(mainPaPin, mainPaState ? HIGH : LOW);
    doc["ok"]=true;
    LOG("[pa] toggled -> %s\n", mainPaState?"ON":"OFF");
  } else {
    doc["ok"]=false; doc["error"]="swr_latched";
  }
  doc["state"]= mainPaState ? "ON" : "OFF";
  sendJson(request, doc);
}

void onLatchSWR(AsyncWebServerRequest* request) {
  if (!swrLatched) { swrLatched=true; mainPaState=false; digitalWrite(mainPaPin, LOW); }
  JsonDocument doc; doc["ok"]=true; doc["latched"]=true; doc["swrThreshold"]=swrThreshold; doc["mainPaState"]="OFF";
  sendJson(request, doc);
}

void onResetSWR(AsyncWebServerRequest* request) {
  bool changed=false; if (swrLatched){ swrLatched=false; changed=true; }
  JsonDocument doc; doc["ok"]=true; doc["changed"]=changed; doc["latched"]=swrLatched;
  sendJson(request, doc);
}

void onSetThreshold(AsyncWebServerRequest* request) {
  float val = NAN;
  if (request->hasParam("plain", true)) {
    String body = request->getParam("plain", true)->value();
    JsonDocument d; if (!deserializeJson(d, body)) {
      if (d["value"].is<float>() || d["value"].is<int>()) val = d["value"].as<float>();
    }
    if (isnan(val)) val = body.toFloat();
  }
  if (isnan(val) && request->hasParam("value")) val = request->getParam("value")->value().toFloat();

  JsonDocument r;
  if (isnan(val) || val<=0) { r["ok"]=false; r["error"]="invalid"; }
  else {
    swrThreshold = val;
    preferences.begin("settings", false);
    preferences.putFloat("swrThreshold", swrThreshold);
    preferences.end();
    r["ok"]=true; r["swrThreshold"]=swrThreshold;
  }
  sendJson(request, r);
}

void onSimToggle(AsyncWebServerRequest* request){
  simMode = !simMode;
  preferences.begin("settings", false);
  preferences.putBool("simMode", simMode);
  preferences.end();
  JsonDocument d; d["ok"]=true; d["simMode"]=simMode;
  sendJson(request, d);
}

void onSaveSettings(AsyncWebServerRequest* request) {
  JsonDocument body; String perr; bool has = readJsonBody(request, body, perr);

  preferences.begin("settings", false);
  int wrote = 0;

  auto putF = [&](const char* k, float &ref){
    if (has && (body[k].is<float>() || body[k].is<int>())) { ref = body[k].as<float>(); preferences.putFloat(k, ref); wrote++; }
    else if (request->hasParam(k)) { ref = request->getParam(k)->value().toFloat(); preferences.putFloat(k, ref); wrote++; }
  };
  auto putI = [&](const char* k){
    if (has && (body[k].is<int>() || body[k].is<float>())) { int v=(int)round(body[k].as<float>()); preferences.putInt(k, v); wrote++; }
    else if (request->hasParam(k)) { int v=(int)round(request->getParam(k)->value().toFloat()); preferences.putInt(k, v); wrote++; }
  };
  auto putS = [&](const char* k){
    if (has && body[k].is<const char*>()) { preferences.putString(k, body[k].as<const char*>()); wrote++; }
    else if (request->hasParam(k)) { preferences.putString(k, request->getParam(k)->value()); wrote++; }
  };

  // bestaande settings
  float dummy;
  putF("forwardMax", dummy);
  putF("reflectedMax", dummy);
  putI("needleThickness");
  putS("needleColor");
  putF("swrThreshold", swrThreshold);
  putF("fwdWperV", fwdWperV);
  putF("refWperV", refWperV);

  // NIEUW: schaalfactors rails
  putF("v5Scale",  v5Scale);
  putF("v12Scale", v12Scale);
  putF("v18Scale", v18Scale);
  putF("v28Scale", v28Scale);

  if (has && body["simMode"].is<bool>()) { simMode = body["simMode"].as<bool>(); preferences.putBool("simMode", simMode); wrote++; }

  JsonDocument out;
  out["ok"]              = wrote > 0;
  out["forwardMax"]      = preferences.getFloat("forwardMax", 50);
  out["reflectedMax"]    = preferences.getFloat("reflectedMax", 25);
  out["needleThickness"] = preferences.getInt  ("needleThickness", 2);
  out["needleColor"]     = preferences.getString("needleColor", "#000000");
  out["swrThreshold"]    = preferences.getFloat("swrThreshold", 3.0f);
  out["simMode"]         = preferences.getBool ("simMode", true);
  out["fwdWperV"]        = preferences.getFloat("fwdWperV", fwdWperV);
  out["refWperV"]        = preferences.getFloat("refWperV", refWperV);
  out["v5Scale"]         = preferences.getFloat("v5Scale",  v5Scale);
  out["v12Scale"]        = preferences.getFloat("v12Scale", v12Scale);
  out["v18Scale"]        = preferences.getFloat("v18Scale", v18Scale);
  out["v28Scale"]        = preferences.getFloat("v28Scale", v28Scale);

  preferences.end();

  sendJson(request, out);
}

void onLoadSettings(AsyncWebServerRequest* request) {
  preferences.begin("settings", true);
  JsonDocument doc;
  doc["forwardMax"]      = preferences.getFloat("forwardMax", 50);
  doc["reflectedMax"]    = preferences.getFloat("reflectedMax", 25);
  doc["needleThickness"] = preferences.getInt  ("needleThickness", 2);
  doc["needleColor"]     = preferences.getString("needleColor", "#000000");
  doc["swrThreshold"]    = preferences.getFloat("swrThreshold", 3.0f);
  doc["simMode"]         = preferences.getBool ("simMode", true);
  doc["fwdWperV"]        = preferences.getFloat("fwdWperV", fwdWperV);
  doc["refWperV"]        = preferences.getFloat("refWperV", refWperV);
  doc["v5Scale"]         = preferences.getFloat("v5Scale",  v5Scale);
  doc["v12Scale"]        = preferences.getFloat("v12Scale", v12Scale);
  doc["v18Scale"]        = preferences.getFloat("v18Scale", v18Scale);
  doc["v28Scale"]        = preferences.getFloat("v28Scale", v28Scale);
  preferences.end();
  sendJson(request, doc);
}

void onCheckUpdate(AsyncWebServerRequest* request) {
  JsonDocument d; d["updateAvailable"]=false; d["currentVersion"]=firmwareVersion; d["latestVersion"]=firmwareVersion; d["updateUrl"]="";
  sendJson(request, d);
}

void onUpdateUpload(AsyncWebServerRequest *request, String, size_t index, uint8_t *data, size_t len, bool final){
  if (index == 0) { Update.begin(); }
  Update.write(data, len);
  if (final) {
    JsonDocument d; if (Update.end(true)) { ws.textAll("{\"type\":\"update_progress\",\"status\":\"success\",\"progress\":100}"); d["ok"]=true; }
    else { d["ok"]=false; }
    sendJson(request, d);
  }
}

void onUpdate(AsyncWebServerRequest* request){
  JsonDocument d; d["ok"]=true; d["hint"]="use multipart upload";
  sendJson(request, d);
}

// ===== WS =====
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *, uint8_t *, size_t) {
  if (type == WS_EVT_CONNECT) {
    JsonDocument hello; hello["type"]="hello"; hello["version"]=firmwareVersion; hello["simMode"]=simMode;
    String s; serializeJson(hello, s); client->text(s);
    LOG("[ws] client connected\n");
  }
}

// ===== setup/loop =====
void setup() {
  Serial.begin(115200);
  delay(100);
  LOG("\n[boot] QO-100 controller v015 …\n");

// Mount LittleFS op de partitie met label "spiffs"
// basePath "/fs" is hoe het in het VFS hangt; maxOpenFiles = 10
if (!FSYS.begin(true, "/fs", 10, "spiffs")) {
  Serial.println("LittleFS mount failed");
}


  pinMode(mainPaPin, OUTPUT);
  pinMode(polPin, OUTPUT);
  mainPaState = false;
  digitalWrite(mainPaPin, LOW);
  digitalWrite(polPin, HIGH);
  // ADC setup
  analogReadResolution(12);
  analogSetPinAttenuation(ADC_FOR, ADC_11db);
  analogSetPinAttenuation(ADC_REF, ADC_11db);
  analogSetPinAttenuation(ADC_5V,  ADC_11db);
  analogSetPinAttenuation(ADC_12V, ADC_11db);
  analogSetPinAttenuation(ADC_18V, ADC_11db);
  analogSetPinAttenuation(ADC_28V, ADC_11db);

  // Load settings
  preferences.begin("settings", false);
  swrThreshold = preferences.getFloat("swrThreshold", 3.0f);
  simMode      = preferences.getBool ("simMode", true);
  fwdWperV     = preferences.getFloat("fwdWperV", fwdWperV);
  refWperV     = preferences.getFloat("refWperV", refWperV);
  v5Scale      = preferences.getFloat("v5Scale",  v5Scale);
  v12Scale     = preferences.getFloat("v12Scale", v12Scale);
  v18Scale     = preferences.getFloat("v18Scale", v18Scale);
  v28Scale     = preferences.getFloat("v28Scale", v28Scale);
  preferences.end();

  // WiFi
  WiFiManager wm;
  bool ok = wm.autoConnect("QO100-Controller");
  if (!ok) { LOG("WiFi connect failed, reboot in 5s\n"); delay(5000); ESP.restart(); }
  LOG("WiFi connected. IP: %s\n", WiFi.localIP().toString().c_str());

  auto &st = server.serveStatic("/", FSYS, "/").setDefaultFile("index.html");

  st.setCacheControl("no-store, max-age=0, must-revalidate");

  // API
  server.on("/api/status",          HTTP_GET,  onStatus);
  server.on("/api/toggle",          HTTP_POST, onToggle);
  server.on("/api/main_pa_toggle",  HTTP_POST, onMainPaToggle);
  server.on("/api/latch_swr",       HTTP_POST, onLatchSWR);
  server.on("/api/reset_swr",       HTTP_POST, onResetSWR);
  server.on("/api/set_threshold",   HTTP_POST, onSetThreshold);
  server.on("/api/sim_toggle",      HTTP_POST, onSimToggle);
  server.on("/api/save_settings",   HTTP_ANY,  onSaveSettings);
  server.on("/api/load_settings",   HTTP_GET,  onLoadSettings);
  server.on("/api/check_update",    HTTP_GET,  onCheckUpdate);
  server.on("/api/update",          HTTP_POST, onUpdate, onUpdateUpload);

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.begin();
  LOG("[server] started on %s\n", WiFi.localIP().toString().c_str());
}

void loop() {
  unsigned long now = millis();
  if (now - lastTick >= 1000) { lastTick = now; broadcastStatus(); }
}
