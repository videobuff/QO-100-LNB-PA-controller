// File amended on: 2025-05-29 09:16 AM CEST

#include <WiFi.h>
#include <WiFiManager.h> 
#define WEBSERVER_H
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <SPIFFS.h>
#define ARDUINOJSON_USE_DEPRECATED 0
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>

const char* FIRMWARE_VERSION = "1.0.6";

const int relayPin = 14;
const int paRelayPin = 26;
const int forwardAdcPin = 34;
const int reflectedAdcPin = 35;

const float forwardCalibrationFactor = 1.0;
const float reflectedCalibrationFactor = 1.0;

char wifi_ssid[32] = "";
char wifi_password[64] = "";

float swrThreshold = 3.0;
bool isVertical = true;
bool isPaOn = true; // Local PA state (SWR/temperature controlled)
bool mainPaEnabled = true; // Main PA switch state
bool swrLatched = false;
unsigned long lastResetTime = 0;
unsigned long swrLatchTime = 0;
float lastSwr = 1.0;
float paTemperature = 0.0; // Simulated temperature
const float tempThreshold = 70.0; // Shutdown PA if temp exceeds 70°C
uint8_t needleThickness = 2; // Changed to uint8_t for space optimization
String needleColor = "#000000"; // Added to store needle color persistently

Preferences prefs;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

const char* http_username = "admin";
const char* http_password = "password";

String previousFirmwareUrl = "";

// Debouncing variables
unsigned long lastRelayToggle = 0;
unsigned long lastPaRelayToggle = 0;
const unsigned long debounceDelay = 50; // 50ms debounce

bool checkAuth(AsyncWebServerRequest *request) {
  if (!request->authenticate(http_username, http_password)) {
    request->requestAuthentication();
    return false;
  }
  return true;
}

void saveConfigCallback() {
  Serial.println("Should save config");
}

float readPowerFiltered(int adcPin, float maxPower, float calibrationFactor) {
  const int samples = 10;
  int readings[samples];
  for (int i = 0; i < samples; i++) {
    readings[i] = analogRead(adcPin);
    delay(1);
  }
  std::sort(readings, readings + samples);
  int median = readings[samples / 2];
  float power = (median / 4095.0) * maxPower * calibrationFactor;
  return max(power, 0.0f);
}

float readTemperature() {
  // Simulate temperature reading (replace with actual sensor reading, e.g., using a DallasTemperature sensor)
  return 25.0 + (random(0, 500) / 10.0); // Random temp between 25°C and 75°C
}

float calculateSWR(float forward, float reflected) {
  const float minForward = 1.0;
  if (forward < minForward || reflected < 0) return 1.0;
  float reflectionCoefficient = reflected / forward;
  if (reflectionCoefficient >= 1.0) return 9.0;
  return (1 + sqrt(reflectionCoefficient)) / (1 - sqrt(reflectionCoefficient));
}

bool validateThreshold(float value) {
  return value >= 1.5 && value <= 10.0;
}

bool validateNeedleThickness(uint8_t value) {
  return value > 0 && value <= 255; // Adjust range as needed, e.g., 1-10
}

void setupWiFi() {
  prefs.begin("settings", true);
  String savedSettings = prefs.getString("all_settings", "");
  if (savedSettings.length() > 0) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, savedSettings);
    if (!error) {
      swrThreshold = doc["swrThreshold"] | 3.0;
      mainPaEnabled = doc["mainPaEnabled"] | true;
      previousFirmwareUrl = doc["prevFirmware"] | "";
      needleThickness = doc["needleThickness"] | 2;
      needleColor = doc["needleColor"] | "#000000"; // Load needleColor
      Serial.printf("[Timestamp: %lu] Loaded settings: needleThickness=%d, swrThreshold=%.2f, needleColor=%s\n", millis(), needleThickness, swrThreshold, needleColor.c_str());
    } else {
      Serial.printf("[Timestamp: %lu] Deserialization failed: %s\n", millis(), error.c_str());
    }
  } else {
    Serial.printf("[Timestamp: %lu] No saved settings found\n", millis());
  }
  prefs.end();

  WiFiManager wifiManager;
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setConfigPortalTimeout(180);
  
  String apName = "QO100-Controller-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  
  if (!wifiManager.autoConnect(apName.c_str())) {
    Serial.println("Failed to connect and hit timeout");
    delay(3000);
    ESP.restart();
  }
  
  Serial.print("WiFi connected. IP: ");
  Serial.println(WiFi.localIP());

  if (saveConfigCallback) {
    prefs.begin("settings", false);
    prefs.putString("wifi_ssid", WiFi.SSID());
    prefs.putString("wifi_pass", WiFi.psk());
    prefs.end();
    Serial.println("WiFi credentials saved");
  }
}

void saveSettings() {
  JsonDocument doc;
  doc["swrThreshold"] = swrThreshold;
  doc["mainPaEnabled"] = mainPaEnabled;
  doc["prevFirmware"] = previousFirmwareUrl;
  doc["needleThickness"] = needleThickness;
  doc["forwardMax"] = 50.0; // Default values, update as needed
  doc["reflectedMax"] = 25.0;
  doc["needleColor"] = needleColor; // Save the current needleColor

  String json;
  serializeJson(doc, json);
  prefs.begin("settings", false);
  bool success = prefs.putString("all_settings", json);
  prefs.end();
  Serial.printf("[Timestamp: %lu] Settings saved, Success: %d, Data: %s\n", millis(), success, json.c_str());
}

void broadcastData() {
  float forward = readPowerFiltered(forwardAdcPin, 50.0, forwardCalibrationFactor);
  float reflected = readPowerFiltered(reflectedAdcPin, 25.0, reflectedCalibrationFactor);
  float swr = calculateSWR(forward, reflected);
  paTemperature = readTemperature();

  // Only apply SWR/temperature protection if main PA is enabled
  if (mainPaEnabled) {
    if (swr > swrThreshold && isPaOn && !swrLatched) {
      swrLatched = true;
      swrLatchTime = millis();
      isPaOn = false;
      if (millis() - lastPaRelayToggle > debounceDelay) {
        digitalWrite(paRelayPin, HIGH); // Turn PA off
        lastPaRelayToggle = millis();
      }
      Serial.printf("[Timestamp: %lu] SWR > %.1f, PA turned OFF, pin %d set to %d\n", millis(), swrThreshold, paRelayPin, digitalRead(paRelayPin));
    }

    if (paTemperature > tempThreshold && isPaOn && !swrLatched) {
      swrLatched = true;
      swrLatchTime = millis();
      isPaOn = false;
      if (millis() - lastPaRelayToggle > debounceDelay) {
        digitalWrite(paRelayPin, HIGH); // Turn PA off
        lastPaRelayToggle = millis();
      }
      Serial.printf("[Timestamp: %lu] PA temperature > %.1f°C, PA turned OFF\n", millis(), tempThreshold);
    }
  } else {
    // If main PA is disabled, ensure the relay is off
    if (isPaOn) {
      isPaOn = false;
      if (millis() - lastPaRelayToggle > debounceDelay) {
        digitalWrite(paRelayPin, HIGH); // Turn PA off
        lastPaRelayToggle = millis();
      }
    }
  }

  // Debug: Log PA state
  Serial.printf("[Timestamp: %lu] PA State: mainPaEnabled=%d, isPaOn=%d, pin=%d\n", millis(), mainPaEnabled, isPaOn, digitalRead(paRelayPin));

  JsonDocument doc;
  doc["type"] = "data";
  doc["forward"] = round(forward * 10) / 10.0;
  doc["reflected"] = round(reflected * 10) / 10.0;
  doc["swr"] = round(swr * 10) / 10.0;
  doc["temperature"] = round(paTemperature * 10) / 10.0;
  doc["state"] = isVertical ? "VERTICAL" : "HORIZONTAL";
  doc["paState"] = isPaOn ? "ON" : "OFF";
  doc["mainPaState"] = mainPaEnabled ? "ON" : "OFF";
  doc["swrThreshold"] = round(swrThreshold * 10) / 10.0;
  doc["version"] = FIRMWARE_VERSION;
  
  String json;
  serializeJson(doc, json);
  ws.textAll(json);

  lastSwr = swr;
}

unsigned long getBroadcastInterval() {
  float swrChange = abs(lastSwr - calculateSWR(
    readPowerFiltered(forwardAdcPin, 50.0, forwardCalibrationFactor),
    readPowerFiltered(reflectedAdcPin, 25.0, reflectedCalibrationFactor)
  ));
  if (swrChange < 0.1 && lastSwr < swrThreshold - 0.5) {
    return 500;
  }
  return 200;
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("[Timestamp: %lu] WebSocket client connected\n", millis());

    float forward = readPowerFiltered(forwardAdcPin, 50.0, forwardCalibrationFactor);
    float reflected = readPowerFiltered(reflectedAdcPin, 25.0, reflectedCalibrationFactor);
    float swr = calculateSWR(forward, reflected);
    paTemperature = readTemperature();

    JsonDocument doc;
    doc["type"] = "data";
    doc["forward"] = round(forward * 10) / 10.0;
    doc["reflected"] = round(reflected * 10) / 10.0;
    doc["swr"] = round(swr * 10) / 10.0;
    doc["temperature"] = round(paTemperature * 10) / 10.0;
    doc["state"] = isVertical ? "VERTICAL" : "HORIZONTAL";
    doc["paState"] = isPaOn ? "ON" : "OFF";
    doc["mainPaState"] = mainPaEnabled ? "ON" : "OFF";
    doc["swrThreshold"] = round(swrThreshold * 10) / 10.0;
    doc["version"] = FIRMWARE_VERSION;
    
    String json;
    serializeJson(doc, json);
    client->text(json);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.printf("[Timestamp: %lu] Booting...\n", millis());

  // Load settings from Preferences without clearing
  prefs.begin("settings", true);
  String savedSettings = prefs.getString("all_settings", "");
  Serial.printf("[Timestamp: %lu] Raw Preferences 'all_settings': %s\n", millis(), savedSettings.c_str());
  
  if (savedSettings.length() > 0) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, savedSettings);
    if (!error) {
      swrThreshold = doc["swrThreshold"] | 3.0;
      mainPaEnabled = doc["mainPaEnabled"] | true;
      previousFirmwareUrl = doc["prevFirmware"] | "";
      needleThickness = doc["needleThickness"] | 2;
      needleColor = doc["needleColor"] | "#000000"; // Load needleColor
      Serial.printf("[Timestamp: %lu] Loaded from Preferences: needleThickness=%d, swrThreshold=%.2f, mainPaEnabled=%d, prevFirmware='%s', needleColor='%s'\n", 
                    millis(), needleThickness, swrThreshold, mainPaEnabled, previousFirmwareUrl.c_str(), needleColor.c_str());
    } else {
      Serial.printf("[Timestamp: %lu] Deserialization failed: %s\n", millis(), error.c_str());
      needleThickness = 2; // Fallback to default
      swrThreshold = 3.0;
      mainPaEnabled = true;
      previousFirmwareUrl = "";
      needleColor = "#000000"; // Default needleColor
      Serial.printf("[Timestamp: %lu] Applied defaults due to deserialization failure: needleThickness=%d, swrThreshold=%.2f, needleColor=%s\n", 
                    millis(), needleThickness, swrThreshold, needleColor.c_str());
    }
  } else {
    Serial.printf("[Timestamp: %lu] No saved settings found, using defaults\n", millis());
    needleThickness = 2; // Default value
    swrThreshold = 3.0;
    mainPaEnabled = true;
    previousFirmwareUrl = "";
    needleColor = "#000000"; // Default needleColor
    Serial.printf("[Timestamp: %lu] Applied defaults: needleThickness=%d, swrThreshold=%.2f, needleColor=%s\n", 
                  millis(), needleThickness, swrThreshold, needleColor.c_str());
  }
  prefs.end();

  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, HIGH);
  pinMode(paRelayPin, OUTPUT);
  digitalWrite(paRelayPin, mainPaEnabled ? LOW : HIGH); // Initialize based on mainPaEnabled only

  pinMode(forwardAdcPin, INPUT);
  pinMode(reflectedAdcPin, INPUT);
  analogSetAttenuation(ADC_11db);
  analogSetWidth(12);

  if (!SPIFFS.begin(true)) {
    Serial.printf("[Timestamp: %lu] Failed to mount SPIFFS\n", millis());
    return;
  }

  setupWiFi();

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html").setAuthentication(http_username, http_password);

  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    request->send(SPIFFS, "/script.js", "application/javascript");
  });

  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    request->send(SPIFFS, "/style.css", "text/css");
  });

  server.on("/meter-satpwr.png", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    if (SPIFFS.exists("/meter-satpwr.png")) {
      request->send(SPIFFS, "/meter-satpwr.png", "image/png");
    } else {
      request->send(404, "text/plain", "Image not found");
    }
  });

  server.on("/api/toggle", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    isVertical = !isVertical;
    if (millis() - lastRelayToggle > debounceDelay) {
      digitalWrite(relayPin, isVertical ? HIGH : LOW);
      lastRelayToggle = millis();
    }
    Serial.printf("[Timestamp: %lu] Mode toggled to %s\n", millis(), isVertical ? "VERTICAL" : "HORIZONTAL");
    request->send(200, "text/plain", isVertical ? "VERTICAL" : "HORIZONTAL");
  });

  server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    if (!isPaOn && millis() - lastResetTime > 1000) {
      isPaOn = true;
      if (millis() - lastPaRelayToggle > debounceDelay && mainPaEnabled) {
        digitalWrite(paRelayPin, LOW);
        lastPaRelayToggle = millis();
      }
      lastResetTime = millis();
      Serial.printf("[Timestamp: %lu] PA reset to ON\n", millis());
      request->send(200, "text/plain", "PA_ON");
    } else {
      Serial.printf("[Timestamp: %lu] PA reset ignored due to cooldown\n", millis());
      request->send(400, "text/plain", "RESET_IGNORED");
    }
  });

  server.on("/api/latch_swr", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    swrLatched = true;
    swrLatchTime = millis();
    Serial.printf("[Timestamp: %lu] SWR latch triggered\n", millis());
    request->send(200, "text/plain", "SWR_LATCHED");
  });

  server.on("/api/reset_swr", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    if (swrLatched && millis() - swrLatchTime > 3000) {
      swrLatched = false;
      isPaOn = true;
      if (millis() - lastPaRelayToggle > debounceDelay && mainPaEnabled) {
        digitalWrite(paRelayPin, LOW);
        lastPaRelayToggle = millis();
      }
      Serial.printf("[Timestamp: %lu] SWR latch reset, PA turned back ON\n", millis());
      request->send(200, "text/plain", "SWR_RESET");
    } else if (!swrLatched) {
      Serial.printf("[Timestamp: %lu] SWR reset ignored: not latched\n", millis());
      request->send(200, "text/plain", "SWR_NOT_LATCHED");
    } else {
      Serial.printf("[Timestamp: %lu] SWR reset ignored: waiting for cooldown\n", millis());
      request->send(429, "text/plain", "WAIT_TO_RESET");
    }
  });

  server.on("/api/set_threshold", HTTP_POST, [](AsyncWebServerRequest *request) {},
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (!checkAuth(request)) return;
      Serial.printf("[Timestamp: %lu] Received set_threshold request\n", millis());
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, data, len);

      if (error) {
        String errorMsg = "JSON parsing error: ";
        errorMsg += error.c_str();
        Serial.printf("[Timestamp: %lu] JSON parsing error: %s\n", millis(), errorMsg.c_str());
        request->send(400, "application/json", "{\"error\":\"" + errorMsg + "\"}");
        return;
      }
      
      if (doc["threshold"].isNull() || !doc["threshold"].is<float>()) {
        Serial.printf("[Timestamp: %lu] Invalid threshold field\n", millis());
        request->send(400, "application/json", "{\"error\":\"Missing or invalid 'threshold' field\"}");
        return;
      }
      
      float newThreshold = doc["threshold"];
      
      if (!validateThreshold(newThreshold)) {
        Serial.printf("[Timestamp: %lu] Invalid threshold value: %.2f\n", millis(), newThreshold);
        request->send(400, "application/json", "{\"error\":\"Threshold must be between 1.5 and 10.0\"}");
        return;
      }

      swrThreshold = newThreshold;
      saveSettings();
      JsonDocument responseDoc;
      responseDoc["success"] = true;
      responseDoc["threshold"] = swrThreshold;
      String response;
      serializeJson(responseDoc, response);
      request->send(200, "application/json", response);
  });

  server.on("/api/get_threshold", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    Serial.printf("[Timestamp: %lu] Received get_threshold request\n", millis());
    JsonDocument doc;
    doc["threshold"] = swrThreshold;
    String response;
    serializeJson(doc, response);
    Serial.printf("[Timestamp: %lu] SWR threshold queried: %.2f\n", millis(), swrThreshold);
    request->send(200, "application/json", response);
  });

  server.on("/api/version", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    Serial.printf("[Timestamp: %lu] Received version request\n", millis());
    JsonDocument doc;
    doc["version"] = FIRMWARE_VERSION;
    String response;
    serializeJson(doc, response);
    Serial.printf("[Timestamp: %lu] Firmware version queried: %s\n", millis(), FIRMWARE_VERSION);
    request->send(200, "application/json", response);
  });

  server.on("/api/check_update", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    Serial.printf("[Timestamp: %lu] Received check_update request\n", millis());
    const char* latestVersion = "1.0.7"; // Updated to reflect new version
    const char* updateUrl = "http://example.com/firmware.bin";
    JsonDocument doc;
    doc["currentVersion"] = FIRMWARE_VERSION;
    doc["latestVersion"] = latestVersion;
    doc["updateAvailable"] = strcmp(FIRMWARE_VERSION, latestVersion) < 0;
    doc["updateUrl"] = updateUrl;
    String response;
    serializeJson(doc, response);
    Serial.printf("[Timestamp: %lu] Update check: current=%s, latest=%s\n", millis(), FIRMWARE_VERSION, latestVersion);
    request->send(200, "application/json", response);
  });

  server.on("/api/update", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    Serial.printf("[Timestamp: %lu] Received update request\n", millis());
    if (Update.hasError()) {
      request->send(500, "text/plain", "UPDATE_FAILED");
    } else {
      request->send(200, "text/plain", "UPDATE_SUCCESS");
      delay(1000);
      ESP.restart();
    }
  }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    if (!checkAuth(request)) return;
    Serial.printf("[Timestamp: %lu] Processing update data, index=%zu, len=%zu\n", millis(), index, len);

    if (!index) {
      Serial.printf("[Timestamp: %lu] Starting OTA update\n", millis());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
        return;
      }
    }

    if (Update.write(data, len) != len) {
      Update.printError(Serial);
      return;
    }

    if (final) {
      if (Update.end(true)) {
        Serial.printf("[Timestamp: %lu] OTA update finished\n", millis());
        previousFirmwareUrl = request->getParam("updateUrl", true)->value();
        saveSettings();
      } else {
        Update.printError(Serial);
      }
    }
  });

  server.on("/api/rollback", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    Serial.printf("[Timestamp: %lu] Received rollback request\n", millis());
    if (previousFirmwareUrl.length() > 0) {
      Serial.printf("[Timestamp: %lu] Initiating rollback to previous firmware\n", millis());
      request->send(200, "text/plain", "ROLLBACK_INITIATED");
    } else {
      Serial.printf("[Timestamp: %lu] Rollback failed: No previous firmware available\n", millis());
      request->send(400, "text/plain", "NO_PREVIOUS_FIRMWARE");
    }
  });

  server.on("/api/main_pa_toggle", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    Serial.printf("[Timestamp: %lu] Received main_pa_toggle request\n", millis());
    mainPaEnabled = !mainPaEnabled;
    if (millis() - lastPaRelayToggle > debounceDelay) {
      digitalWrite(paRelayPin, mainPaEnabled ? LOW : HIGH); // Directly control pin based on mainPaEnabled
      lastPaRelayToggle = millis();
    }
    if (!mainPaEnabled) {
      isPaOn = false; // Ensure PA state reflects the toggle
    }
    saveSettings();
    Serial.printf("[Timestamp: %lu] Main PA toggled to %s, pin %d set to %d\n", millis(), mainPaEnabled ? "ON" : "OFF", paRelayPin, digitalRead(paRelayPin));

    // Broadcast updated state
    JsonDocument doc;
    doc["type"] = "data";
    doc["mainPaState"] = mainPaEnabled ? "ON" : "OFF";
    doc["paState"] = isPaOn ? "ON" : "OFF";
    String json;
    serializeJson(doc, json);
    ws.textAll(json);

    request->send(200, "text/plain", mainPaEnabled ? "ON" : "OFF");
  });

  server.on("/api/save_settings", HTTP_POST, [](AsyncWebServerRequest *request) {},
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      Serial.printf("[Timestamp: %lu] Received save_settings request, len=%zu\n", millis(), len);
      if (!checkAuth(request)) {
        Serial.printf("[Timestamp: %lu] Authentication failed for save_settings\n", millis());
        return;
      }
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, data, len);

      if (error) {
        String errorMsg = "JSON parsing error: ";
        errorMsg += error.c_str();
        Serial.printf("[Timestamp: %lu] JSON parsing error: %s\n", millis(), errorMsg.c_str());
        request->send(400, "application/json", "{\"error\":\"" + errorMsg + "\"}");
        return;
      }

      if (doc["forwardMax"].is<float>()) swrThreshold = doc["forwardMax"].as<float>(); // Note: Using forwardMax as temp holder
      if (doc["reflectedMax"].is<float>()) swrThreshold = doc["reflectedMax"].as<float>(); // Note: Using reflectedMax as temp holder
      if (doc["needleThickness"].is<int>()) {
        uint8_t thickness = static_cast<uint8_t>(doc["needleThickness"].as<int>());
        if (validateNeedleThickness(thickness)) {
          needleThickness = thickness;
          Serial.printf("[Timestamp: %lu] Updated needleThickness to %d\n", millis(), needleThickness);
        }
      }
      if (doc["needleColor"].is<String>()) {
        needleColor = doc["needleColor"].as<String>(); // Correctly save needleColor
        Serial.printf("[Timestamp: %lu] Updated needleColor to %s\n", millis(), needleColor.c_str());
      }
      if (doc["swrThreshold"].is<float>()) {
        swrThreshold = doc["swrThreshold"].as<float>();
        Serial.printf("[Timestamp: %lu] Updated swrThreshold to %.2f\n", millis(), swrThreshold);
      }

      saveSettings();
      request->send(200, "application/json", "{\"success\":true}");
    });

  server.on("/api/load_settings", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.printf("[Timestamp: %lu] Received load_settings request\n", millis());
    if (!checkAuth(request)) {
      Serial.printf("[Timestamp: %lu] Authentication failed for load_settings\n", millis());
      return;
    }
    JsonDocument doc;
    doc["forwardMax"] = 50.0;
    doc["reflectedMax"] = 25.0;
    doc["needleThickness"] = needleThickness; // Use current value
    doc["needleColor"] = needleColor; // Use stored needleColor
    doc["swrThreshold"] = swrThreshold; // Use current value

    String response;
    serializeJson(doc, response);
    Serial.printf("[Timestamp: %lu] Loaded from Preferences: needleThickness=%d, swrThreshold=%.2f, needleColor=%s, Response: %s\n", millis(), needleThickness, swrThreshold, needleColor.c_str(), response.c_str());
    request->send(200, "application/json", response);
  });

  server.on("/api", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!checkAuth(request)) return;
    String html = "<html><head><title>QO-100 Controller API</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{font-family:Arial,sans-serif;max-width:800px;margin:0 auto;padding:20px;} "
            "table{width:100%;border-collapse:collapse;margin:20px 0;} "
            "th,td{border:1px solid #ddd;padding:8px;text-align:left;} "
            "th{background-color:#f2f2f2;} "
            "h1,h2{color:#007bff;}</style></head><body>";
    html += "<h1>QO-100 Controller API Documentation</h1>";
    html += "<p>Firmware Version: ";
    html += FIRMWARE_VERSION;
    html += "</p><table><tr><th>Endpoint</th><th>Method</th><th>Description</th><th>Parameters</th></tr>";
    html += "<tr><td>/api/toggle</td><td>POST</td><td>Toggle mode</td><td>None</td></tr>";
    html += "<tr><td>/api/reset</td><td>POST</td><td>Reset PA after shutdown</td><td>None</td></tr>";
    html += "<tr><td>/api/latch_swr</td><td>POST</td><td>Manually latch SWR protection</td><td>None</td></tr>";
    html += "<tr><td>/api/reset_swr</td><td>POST</td><td>Reset SWR latch</td><td>None</td></tr>";
    html += "<tr><td>/api/set_threshold</td><td>POST</td><td>Set SWR threshold</td><td>JSON: {\"threshold\": float}</td></tr>";
    html += "<tr><td>/api/get_threshold</td><td>GET</td><td>Get current SWR threshold</td><td>None</td></tr>";
    html += "<tr><td>/api/version</td><td>GET</td><td>Get firmware version</td><td>None</td></tr>";
    html += "<tr><td>/api/check_update</td><td>GET</td><td>Check for firmware updates</td><td>None</td></tr>";
    html += "<tr><td>/api/update</td><td>POST</td><td>Perform OTA update</td><td>updateUrl (query param)</td></tr>";
    html += "<tr><td>/api/rollback</td><td>POST</td><td>Rollback to previous firmware</td><td>None</td></tr>";
    html += "<tr><td>/api/main_pa_toggle</td><td>POST</td><td>Toggle main PA on/off</td><td>None</td></tr>";
    html += "<tr><td>/api/save_settings</td><td>POST</td><td>Save gauge settings</td><td>JSON: {\"forwardMax\": float, \"reflectedMax\": float, \"needleThickness\": int, \"needleColor\": string, \"swrThreshold\": float}</td></tr>";
    html += "<tr><td>/api/load_settings</td><td>GET</td><td>Load saved gauge settings</td><td>None</td></tr>";
    html += "</table>";
    html += "<h2>WebSocket API</h2>";
    html += "<p>Connect to <code>ws://[device-ip]/ws</code> for real-time data updates.</p>";
    html += "</body></html>";
    request->send(200, "text/html", html);
  });

  server.begin();
}

void loop() {
  ws.cleanupClients();
  static unsigned long lastBroadcast = 0;
  if (millis() - lastBroadcast >= getBroadcastInterval()) {
    broadcastData();
    lastBroadcast = millis();
  }
}

// Note: PA control pin has been changed to D14 - update server-side firmware accordingly
