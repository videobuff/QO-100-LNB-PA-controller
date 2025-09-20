/*
  QO-100 Controller – ESP32 (Async)
  Option B (ArduinoJson v7) + REAL ADC + Frontend compatibility + Auth challenge

  - SIM/REAL switch (Preferences key "sim")
  - ADC1 pins for FOR/REF and rails (5/12/18/28V)
  - WebSocket /ws → "hello" + periodieke "data" (incl. rails)
  - Legacy routes: /api/main_pa_toggle, /api/latch_swr, /api/reset_swr
  - Basic-Auth met WWW-Authenticate challenge (request->requestAuthentication)
  - ⚡ Snel: TELEMETRY_PERIOD_MS = 150 ms (~6–7 Hz) + agressiever EMA
  - 🛡️ SWR-latch guard: min forward, N opeenvolgende samples, 2s hold-off

  WiFi credentials via WiFiManager (opgeslagen in ESP32 systeem-NVS)
*/

#include <WiFi.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Update.h>

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <vector>
#include <driver/adc.h>

// -------------------- Pins --------------------
const int mainPaPin    = 14;   // PA relais
const int polPin       = 26;   // SSB/DATV
const int RECOVERY_PIN = 4;    // bij LOW op boot -> login reset

// Alleen ADC1 pins gebruiken (ADC2 stoort met WiFi)
const int PIN_FORWARD   = 34;  // ADC1_CH6  (FOR)
const int PIN_REFLECTED = 35;  // ADC1_CH7  (REF)
const int PIN_V5        = 32;  // ADC1_CH4
const int PIN_V12       = 33;  // ADC1_CH5
const int PIN_V18       = 36;  // ADC1_CH0
const int PIN_V28       = 39;  // ADC1_CH3
const int PIN_TEMP      = -1;  // NTC optioneel (ADC1-pin), -1 = uit

// -------------------- Tempo & smoothing --------------------
const uint16_t TELEMETRY_PERIOD_MS = 150;    // ~6–7 Hz
const float    EMA_POWER_ALPHA      = 0.75f; // snel
const float    EMA_TEMP_ALPHA       = 0.20f; // rustiger

// -------------------- SWR latch guard --------------------
const float   MIN_FWD_W_FOR_LATCH = 1.0f;  // pas SWR-latch bekijken als fwd ≥ 1 W
const uint8_t SWR_OVER_N_SAMPLES  = 5;     // N opeenvolgende samples boven threshold
volatile uint8_t swrOverCount = 0;
unsigned long  latchIgnoreUntil = 0;       // guard-time (ms) waarin we NIET latschen

// -------------------- Globals --------------------
Preferences preferences;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

String firmwareVersion = "2025-v025-guard";

// auth
String httpUser;
String httpPass;

// status
bool mainPaState  = false;      // OFF
bool swrLatched   = false;
bool simMode      = true;

// settings (defaults)
float swrThreshold    = 3.0f;
float forwardMax      = 50.0f;
float reflectedMax    = 25.0f;
int   needleThickness = 2;
String needleColor    = "#000000";

// rails (V = adc_raw * scale) — jouw model
float v5Scale  = 1.6818f;
float v12Scale = 4.0909f;
float v18Scale = 6.4545f;
float v28Scale = 10.0909f;

// runtime
float forwardPower   = 0.0f;
float reflectedPower = 0.0f;
float temperature    = 25.0f;
float rail5V=0, rail12V=0, rail18V=0, rail28V=0;

unsigned long lastTick = 0;

// -------------------- BodyBuffer --------------------
class BodyBuffer {
public:
  void begin(size_t total){ buf.clear(); buf.reserve(total>0?total:1024); }
  void append(const uint8_t* d, size_t n){ buf.insert(buf.end(), d, d+n); }
  const char* c_str(){ tmp.assign(buf.begin(), buf.end()); tmp.push_back('\0'); return tmp.data(); }
  size_t size() const { return buf.size(); }
private:
  std::vector<uint8_t> buf; std::vector<char> tmp;
};

// -------------------- Prefs/Auth --------------------
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

// UNIFIED auth met challenge → browser vraagt credentials & onthoudt ze
bool requireAuth(AsyncWebServerRequest* request){
  if (request->authenticate(httpUser.c_str(), httpPass.c_str())) return true;
  request->requestAuthentication("QO100-Controller");  // stuurt WWW-Authenticate
  return false;
}

void loadAllPrefs(){
  preferences.begin("settings", true);
  forwardMax      = preferences.getFloat("forwardMax",     50.0f);
  reflectedMax    = preferences.getFloat("reflectedMax",   25.0f);
  needleThickness = preferences.getInt  ("needleThickness",2);
  needleColor     = preferences.getString("needleColor",   "#000000");
  swrThreshold    = preferences.getFloat("swrThreshold",   3.0f);

  v5Scale  = preferences.getFloat("v5Scale",  v5Scale);
  v12Scale = preferences.getFloat("v12Scale", v12Scale);
  v18Scale = preferences.getFloat("v18Scale", v18Scale);
  v28Scale = preferences.getFloat("v28Scale", v28Scale);

  simMode  = preferences.getBool("sim", true);
  preferences.end();
}
void saveSimPref(bool v){
  simMode = v;
  preferences.begin("settings", false);
  preferences.putBool("sim", simMode);
  preferences.end();
}

// -------------------- Helpers --------------------
float computeSWR(float fwd, float refl){
  if (fwd <= 0.01f) return 1.0f;
  float r = refl / (fwd + 1e-6f);
  r = constrain(r, 0.0f, 0.9999f);
  float gamma = sqrtf(r);
  float swr = (1 + gamma) / (1 - gamma);
  if (!isfinite(swr)) swr = 99.9f;
  return swr;
}

void simulateSamples(){
  forwardPower   = random(0, (int)(forwardMax * 10)) / 10.0f;
  float reflCap  = min(reflectedMax, forwardPower * 0.4f);
  reflectedPower = random(0, (int)(reflCap * 10)) / 10.0f;
  temperature    = 20.0f + (random(0, 120) / 10.0f);

  rail5V  = 4.9f + (random(-5,6)/100.0f);
  rail12V = 12.1f+ (random(-10,11)/100.0f);
  rail18V = 18.0f+ (random(-10,11)/100.0f);
  rail28V = 27.8f+ (random(-20,21)/100.0f);
}

float ema(float prev, float cur, float a=0.35f){ return prev*(1-a) + cur*a; }

void readRealSensors(){
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_FORWARD,   ADC_11db);
  analogSetPinAttenuation(PIN_REFLECTED, ADC_11db);
  analogSetPinAttenuation(PIN_V5,  ADC_11db);
  analogSetPinAttenuation(PIN_V12, ADC_11db);
  analogSetPinAttenuation(PIN_V18, ADC_11db);
  analogSetPinAttenuation(PIN_V28, ADC_11db);
  if (PIN_TEMP >= 0) analogSetPinAttenuation(PIN_TEMP, ADC_11db);

  int rawF  = analogRead(PIN_FORWARD);
  int rawR  = analogRead(PIN_REFLECTED);
  int raw5  = analogRead(PIN_V5);
  int raw12 = analogRead(PIN_V12);
  int raw18 = analogRead(PIN_V18);
  int raw28 = analogRead(PIN_V28);

  // FOR/REF voorlopig lineair naar W → vervang later met jouw echte kalibratie
  float fwdW = (rawF  / 4095.0f) * forwardMax;
  float refW = (rawR  / 4095.0f) * reflectedMax;
  forwardPower   = ema(forwardPower,   fwdW, EMA_POWER_ALPHA);
  reflectedPower = ema(reflectedPower, refW, EMA_POWER_ALPHA);

  // rails met jouw model
  rail5V  = raw5  * v5Scale;
  rail12V = raw12 * v12Scale;
  rail18V = raw18 * v18Scale;
  rail28V = raw28 * v28Scale;

  if (PIN_TEMP >= 0) {
    int rawT = analogRead(PIN_TEMP);
    float tC = (rawT / 4095.0f) * 50.0f; // placeholder
    temperature = ema(temperature, tC, EMA_TEMP_ALPHA);
  }
}

// -------------------- WebSocket --------------------
void sendHello(AsyncWebSocketClient* client){
  JsonDocument hello;
  hello["type"]    = "hello";
  hello["version"] = firmwareVersion;
  hello["sim"]     = simMode;
  String s; serializeJson(hello, s);
  client->text(s);
}

void broadcastStatus(){
  if (simMode) simulateSamples(); else readRealSensors();
  float swr = computeSWR(forwardPower, reflectedPower);

  // --- SWR latch logica met filtering/guards ---
  bool over      = (swr > swrThreshold);
  bool enoughFwd = (forwardPower >= MIN_FWD_W_FOR_LATCH);
  unsigned long now = millis();

  if (over && enoughFwd) {
    if (swrOverCount < 255) swrOverCount++;
  } else {
    swrOverCount = 0;
  }

  if (mainPaState && (now >= latchIgnoreUntil) && (swrOverCount >= SWR_OVER_N_SAMPLES)) {
    swrLatched   = true;
    mainPaState  = false;
    digitalWrite(mainPaPin, LOW);
    Serial.printf("[SWR] LATCH: swr=%.2f thr=%.2f fwd=%.1f (count=%u)\n",
                  swr, swrThreshold, forwardPower, swrOverCount);
  }

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
  doc["latched"]      = swrLatched;

  JsonObject rails = doc["rails"].to<JsonObject>();
  rails["v5"]  = rail5V;
  rails["v12"] = rail12V;
  rails["v18"] = rail18V;
  rails["v28"] = rail28V;

  String json; serializeJson(doc, json);
  ws.textAll(json);
}

// -------------------- REST core --------------------
void onToggle(AsyncWebServerRequest* request){
  if (!requireAuth(request)) return;
  digitalWrite(polPin, !digitalRead(polPin));
  bool ssb = (digitalRead(polPin) == HIGH);
  JsonDocument doc; doc["ok"]=true; doc["mode"]= ssb ? "SSB" : "DATV";
  String s; serializeJson(doc, s);
  request->send(200, "application/json", s);
}

void onMainPaToggle(AsyncWebServerRequest* request){
  if (!requireAuth(request)) return;
  if (!swrLatched) {
    mainPaState = !mainPaState;
    digitalWrite(mainPaPin, mainPaState ? HIGH : LOW);

    if (mainPaState) {
      // Net aangezet → even niet latschen, en teller resetten
      latchIgnoreUntil = millis() + 2000; // 2s settle time
      swrOverCount = 0;
    }
  }
  JsonDocument doc; doc["ok"]=true; doc["state"]= mainPaState ? "ON" : "OFF"; doc["latched"]=swrLatched;
  String s; serializeJson(doc, s);
  request->send(200, "application/json", s);
}

void onLatchSWR(AsyncWebServerRequest* request){
  if (!requireAuth(request)) return;
  if (!swrLatched) {
    swrLatched = true;
    mainPaState = false;
    digitalWrite(mainPaPin, LOW);
    request->send(200, "application/json", "{\"ok\":true}");
  } else {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"already latched\"}");
  }
}

void onResetSWR(AsyncWebServerRequest* request){
  if (!requireAuth(request)) return;
  if (swrLatched) {
    swrLatched = false;
    swrOverCount = 0;
    latchIgnoreUntil = millis() + 2000; // kort niet latschen
    request->send(200, "application/json", "{\"ok\":true,\"result\":\"SWR_RESET\"}");
  } else {
    request->send(200, "application/json", "{\"ok\":true,\"result\":\"WAIT_TO_RESET\"}");
  }
}

void onLoadSettings(AsyncWebServerRequest* request){
  if (!requireAuth(request)) return;
  loadAllPrefs();
  JsonDocument doc;
  doc["ok"]              = true;
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

// ---------- POST bodies (Option B) ----------
void handleSaveSettingsBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
  static BodyBuffer body;
  if (index == 0) body.begin(total);
  body.append(data, len);
  if (index + len != total) return;
  if (!requireAuth(request)) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, (const char*)body.c_str(), body.size());
  if (err) { request->send(400, "application/json", String("{\"ok\":false,\"error\":\"") + err.c_str() + "\"}"); return; }

  preferences.begin("settings", false);

  if (doc["forwardMax"].is<float>()){ forwardMax = doc["forwardMax"].as<float>(); preferences.putFloat("forwardMax", forwardMax); }
  if (doc["reflectedMax"].is<float>()){ reflectedMax = doc["reflectedMax"].as<float>(); preferences.putFloat("reflectedMax", reflectedMax); }
  if (doc["needleThickness"].is<int>()){ needleThickness = doc["needleThickness"].as<int>(); preferences.putInt("needleThickness", needleThickness); }
  if (doc["needleColor"].is<const char*>()){ needleColor = String(doc["needleColor"].as<const char*>()); preferences.putString("needleColor", needleColor); }
  if (doc["swrThreshold"].is<float>()){ swrThreshold = doc["swrThreshold"].as<float>(); preferences.putFloat("swrThreshold", swrThreshold); }

  if (doc["v5Scale"].is<float>()){  v5Scale  = doc["v5Scale"].as<float>();  preferences.putFloat("v5Scale", v5Scale); }
  if (doc["v12Scale"].is<float>()){ v12Scale = doc["v12Scale"].as<float>(); preferences.putFloat("v12Scale", v12Scale); }
  if (doc["v18Scale"].is<float>()){ v18Scale = doc["v18Scale"].as<float>(); preferences.putFloat("v18Scale", v18Scale); }
  if (doc["v28Scale"].is<float>()){ v28Scale = doc["v28Scale"].as<float>(); preferences.putFloat("v28Scale", v28Scale); }

  if (doc["sim"].is<bool>()){ saveSimPref(doc["sim"].as<bool>()); }

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

void handleSetThresholdBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
  static BodyBuffer body;
  if (index == 0) body.begin(total);
  body.append(data, len);
  if (index + len != total) return;
  if (!requireAuth(request)) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, (const char*)body.c_str(), body.size());
  if (err) { request->send(400, "application/json", String("{\"ok\":false,\"error\":\"JSON: ") + err.c_str() + "\"}"); return; }

  if (doc["threshold"].is<float>()){
    swrThreshold = doc["threshold"].as<float>();
    preferences.begin("settings", false);
    preferences.putFloat("swrThreshold", swrThreshold);
    preferences.end();
    request->send(200, "application/json", "{\"ok\":true,\"result\":\"Threshold set\"}");
  } else {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"Missing 'threshold' (float)\"}");
  }
}

// -------------------- WebSocket event --------------------
void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t * data, size_t len) {
  if (type == WS_EVT_CONNECT) sendHello(client);
}

// -------------------- Setup --------------------
void setup() {
  Serial.begin(115200); delay(50);
  Serial.printf("[boot] QO-100 controller %s …\n", firmwareVersion.c_str());

  // LittleFS
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed → formatting…");
    LittleFS.format();
    if (!LittleFS.begin()) { Serial.println("LittleFS mount failed after formatting"); while(1) delay(1000); }
  }

  // Pins
  pinMode(mainPaPin, OUTPUT);
  pinMode(polPin, OUTPUT);
  pinMode(RECOVERY_PIN, INPUT_PULLUP);
  digitalWrite(mainPaPin, LOW);  mainPaState = false; // OFF
  digitalWrite(polPin, HIGH);    // SSB

  // Recovery login?
  if (digitalRead(RECOVERY_PIN) == LOW) {
    saveAuth("admin","password");
    Serial.println("[recovery] login reset to admin/password");
  }
  loadAuth();
  loadAllPrefs();

  // WiFi (WiFiManager → systeem-NVS)
  WiFiManager wm;
  bool ok = wm.autoConnect("QO100-Controller");
  if (!ok) { Serial.println("WiFi connect failed, rebooting in 5s"); delay(5000); ESP.restart(); }

  Serial.print("WiFi connected. IP: "); Serial.println(WiFi.localIP());

  // Static files
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  // WebSocket
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // ---------- Core routes ----------
  server.on("/api/toggle", HTTP_POST, onToggle);
  server.on("/api/mainpa", HTTP_POST, onMainPaToggle);
  server.on("/api/latch",  HTTP_POST, onLatchSWR);
  server.on("/api/reset",  HTTP_POST, onResetSWR);

  server.on("/api/load_settings", HTTP_GET, onLoadSettings);

  server.on("/api/save_settings", HTTP_POST,
    [](AsyncWebServerRequest *request){}, nullptr, handleSaveSettingsBody);
  server.on("/api/set_threshold", HTTP_POST,
    [](AsyncWebServerRequest *request){}, nullptr, handleSetThresholdBody);

  // ---------- Compatibility aliases (oude frontend) ----------
  server.on("/api/main_pa_toggle", HTTP_POST, onMainPaToggle); // {state:"ON"/"OFF"}
  server.on("/api/latch_swr", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
    if (!swrLatched) { swrLatched = true; mainPaState=false; digitalWrite(mainPaPin, LOW); }
    request->send(200, "text/plain", "OK");
  });
  server.on("/api/reset_swr", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
    if (swrLatched) {
      swrLatched=false;
      swrOverCount = 0;
      latchIgnoreUntil = millis() + 2000;
      request->send(200, "text/plain", "SWR_RESET");
    } else {
      request->send(200, "text/plain", "WAIT_TO_RESET");
    }
  });

  // (optioneel) Login-trigger om popup te forceren
  server.on("/login", HTTP_GET, [](AsyncWebServerRequest* r){
    r->requestAuthentication("QO100-Controller");
  });

  // (optioneel) LittleFS diagnostiek
  server.on("/fs/list", HTTP_GET, [](AsyncWebServerRequest *request){
    String out = "[\n";
    File root = LittleFS.open("/");
    File f = root.openNextFile();
    bool first = true;
    while (f) {
      if (!first) out += ",\n";
      first = false;
      out += "  {\"name\":\"" + String(f.name()) + "\",\"size\":" + String(f.size()) + "}";
      f = root.openNextFile();
    }
    out += "\n]\n";
    request->send(200, "application/json", out);
  });
  server.on("/fs/exists", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->hasParam("p")) { request->send(400, "text/plain", "usage: /fs/exists?p=/index.html"); return; }
    String p = request->getParam("p")->value();
    bool ok = LittleFS.exists(p);
    request->send(200, "application/json", String("{\"path\":\"")+p+"\",\"exists\":"+(ok?"true":"false")+"}");
  });

  // 404 → geen SPA-fallback voor /api/*
  server.onNotFound([](AsyncWebServerRequest *request){
    String url = request->url();
    if (url.startsWith("/api/")) {
      request->send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
      return;
    }
    if (LittleFS.exists(url)) request->send(LittleFS, url, String(), false);
    else                      request->send(LittleFS, "/index.html", "text/html");
  });

  server.begin();
  Serial.println("HTTP server started");
}

// -------------------- Loop --------------------
void loop(){
  unsigned long now = millis();
  if (now - lastTick >= TELEMETRY_PERIOD_MS) {
    lastTick = now;
    broadcastStatus();
  }
}
