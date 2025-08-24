#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "time.h"
#include <driver/ledc.h>
#include <ArduinoJson.h>
#include "DHT.h"
#include <Preferences.h>
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <HTTPClient.h>

#define MAINLED_PIN 1
#define NIGHTLED_PIN 7
#define MAINLEDSW_PIN 2
#define MASTERSW_PIN 3
#define DHT_PIN 4
#define FAN_PIN 6

#define PWM_FREQUENCY 5000
#define PWM_RESOLUTION 8
#define LED_CHANNEL 0

const char* ssid = "wifi_slow";

IPAddress staticIP(192, 168, 1, 4);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(192, 168, 1, 3);

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 8 * 3600;
const int daylightOffset_sec = 0;

int mainledswState = 0;
int lastMainledswReading = 0;
int masterswState = 0;
int lastMasterswReading = 0;
int nightledPWMValue = 0;

#define DHTTYPE DHT22
DHT dht(DHT_PIN, DHTTYPE);
float tempThresholdOn = 28.9;
float tempThresholdOff = 28.7;
float currentTemperature = 0.0;
unsigned long lastDhtReadTime = 0;
const unsigned long dhtReadInterval = 4000;

bool fanOverride = false;
unsigned long fanOverrideStartTime = 0;
const unsigned long fanOverrideDuration = 30 * 60 * 1000;
unsigned long lastFanStateChange = 0;
const unsigned long fanCooldownDelay = 5000;

Preferences preferences;
WebServer server(80);

bool isTimeInRange(int startHour, int startMin, int endHour, int endMin) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return false;
  }
  
  int nowInMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int startInMinutes = startHour * 60 + startMin;
  int endInMinutes = endHour * 60 + endMin;

  if (startInMinutes > endInMinutes) {
    return (nowInMinutes >= startInMinutes || nowInMinutes < endInMinutes);
  } else {
    return (nowInMinutes >= startInMinutes && nowInMinutes < endInMinutes);
  }
}

void handleMainToggle() {
  if (masterswState == 1) {
    mainledswState = 1 - mainledswState;
  }
  String response = "Main LED Switch state is now: " + String(mainledswState);
  server.send(200, "text/plain", response);
}

void handleMasterToggle() {
  masterswState = 1 - masterswState;
  String response = "Master Switch state is now: " + String(masterswState);
  server.send(200, "text/plain", response);
}

void handleSetTempOn() {
  if (!server.hasArg("tempOn")) {
    server.send(400, "text/plain", "Missing 'tempOn' parameter.");
    return;
  }
  
  float newTempOn = server.arg("tempOn").toFloat();
  if (newTempOn > tempThresholdOff) {
    tempThresholdOn = newTempOn;
    preferences.begin("bilik-config", false);
    preferences.putFloat("tempOn", tempThresholdOn);
    preferences.end();
    server.send(200, "text/plain", "Fan 'tempOn' threshold set successfully.");
  } else {
    server.send(400, "text/plain", "New 'tempOn' must be greater than current 'tempOff'.");
  }
}

void handleSetTempOff() {
  if (!server.hasArg("tempOff")) {
    server.send(400, "text/plain", "Missing 'tempOff' parameter.");
    return;
  }
  
  float newTempOff = server.arg("tempOff").toFloat();
  if (tempThresholdOn > newTempOff) {
    tempThresholdOff = newTempOff;
    preferences.begin("bilik-config", false);
    preferences.putFloat("tempOff", tempThresholdOff);
    preferences.end();
    server.send(200, "text/plain", "Fan 'tempOff' threshold set successfully.");
  } else {
    server.send(400, "text/plain", "New 'tempOff' must be less than current 'tempOn'.");
  }
}

void handleFanOn30m() {
  if (masterswState == 1 && fanOverride == false) {
    fanOverride = true;
    fanOverrideStartTime = millis();
    digitalWrite(FAN_PIN, HIGH);
    server.send(200, "text/plain", "Fan turned on for 30 minutes.");
  } else {
    server.send(400, "text/plain", "Cannot turn fan on. Master switch is off or fan is already in override mode.");
  }
}

void handleFanOff() {
  digitalWrite(FAN_PIN, LOW);
  fanOverride = false;
  server.send(200, "text/plain", "Fan turned off.");
}

void handleSetNightledPWM() {
  if (!server.hasArg("pwmValue")) {
    server.send(400, "text/plain", "Missing 'pwmValue' parameter.");
    return;
  }
  
  int newPWM = server.arg("pwmValue").toInt();
  if (newPWM >= 0 && newPWM <= 255) {
    nightledPWMValue = newPWM;
    preferences.begin("bilik-config", false);
    preferences.putInt("nightledPWM", nightledPWMValue);
    preferences.end();
    server.send(200, "text/plain", "Night LED PWM value set successfully.");
  } else {
    server.send(400, "text/plain", "PWM value must be between 0 and 255.");
  }
}

bool shouldNightLedBeOn() {
  return isTimeInRange(19, 15, 7, 15) && (mainledswState == 0 || masterswState == 0);
}

void handleState() {
  StaticJsonDocument<300> doc;
  
  struct tm timeinfo;
  char timeString[64];
  getLocalTime(&timeinfo);
  strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
  doc["currentTime"] = timeString;

  long uptimeSeconds = millis() / 1000;
  doc["uptimeSeconds"] = uptimeSeconds;

  doc["mainledswState"] = mainledswState;
  doc["masterswState"] = masterswState;
  doc["mainledState"] = digitalRead(MAINLED_PIN);
  
  doc["nightledState"] = shouldNightLedBeOn();
  doc["nightledPWMValue"] = nightledPWMValue;

  doc["dhtTemp"] = currentTemperature;
  doc["fanState"] = digitalRead(FAN_PIN);
  doc["tempThresholdOn"] = tempThresholdOn;
  doc["tempThresholdOff"] = tempThresholdOff;
  doc["fanOverride"] = fanOverride;

  doc["freeHeap"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  doc["minFreeHeap"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

  String jsonResponse;
  serializeJson(doc, jsonResponse);

  server.send(200, "application/json", jsonResponse);
}

void controlNightLED() {
  if (shouldNightLedBeOn()) {
    ledcWrite(NIGHTLED_PIN, nightledPWMValue);
  } else {
    ledcWrite(NIGHTLED_PIN, 0);
  }
}

void controlFan() {
  if (fanOverride) {
    return;
  }
  
  if (masterswState == 1) {
    if (millis() - lastFanStateChange < fanCooldownDelay) {
      return;
    }
    
    if (digitalRead(FAN_PIN) == LOW && currentTemperature >= tempThresholdOn) {
      digitalWrite(FAN_PIN, HIGH);
      lastFanStateChange = millis();
    }
    else if (digitalRead(FAN_PIN) == HIGH && currentTemperature <= tempThresholdOff) {
      digitalWrite(FAN_PIN, LOW);
      lastFanStateChange = millis();
    }
  } else {
    digitalWrite(FAN_PIN, LOW);
  }
}

void sendWdtTrace() {
  // Check the reason for the last reset
  esp_reset_reason_t reason = esp_reset_reason();
  
  // If the reason is a watchdog timer reset, send a trace
  if (reason == ESP_RST_WDT || reason == ESP_RST_TASK_WDT) {
    HTTPClient http;
    // Update the URL to match the Python server's /log endpoint
    String url = "http://192.168.1.3:5001/log";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<200> doc;
    // Create a single log message with all relevant information
    String logMessage = "Watchdog Timer Reset. Uptime: " + String(millis() / 1000) + "s. Free Heap: " + String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) + " bytes.";
    doc["event_message"] = logMessage;
    
    String requestBody;
    serializeJson(doc, requestBody);

    int httpResponseCode = http.POST(requestBody);

    if (httpResponseCode > 0) {
      Serial.print("HTTP Response code: ");
      Serial.println(httpResponseCode);
    } else {
      Serial.print("Error sending trace: ");
      Serial.println(http.errorToString(httpResponseCode).c_str());
    }
    http.end();
  }
}

void setup() {
  setCpuFrequencyMhz(80);

  // Call the new trace function at the start of setup
  sendWdtTrace();

  WiFi.config(staticIP, gateway, subnet, primaryDNS);
  
  preferences.begin("bilik-config", false);
  tempThresholdOn = preferences.getFloat("tempOn", 28.9);
  tempThresholdOff = preferences.getFloat("tempOff", 28.7);
  nightledPWMValue = preferences.getInt("nightledPWM", 0);
  preferences.end();

  pinMode(MAINLED_PIN, OUTPUT);
  pinMode(NIGHTLED_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  pinMode(MAINLEDSW_PIN, INPUT_PULLUP);
  pinMode(MASTERSW_PIN, INPUT_PULLUP);

  dht.begin();

  ledcAttach(NIGHTLED_PIN, PWM_FREQUENCY, PWM_RESOLUTION);

  server.on("/mainledswState", handleMainToggle);
  server.on("/masterswState", handleMasterToggle);
  server.on("/set_temp_on", handleSetTempOn);
  server.on("/set_temp_off", handleSetTempOff);
  server.on("/on-30m", handleFanOn30m);
  server.on("/off", handleFanOff);
  server.on("/set_nightled_pwm", handleSetNightledPWM);
  server.on("/state", handleState);
  server.begin();

  Serial.begin(115200);
  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid);

  int wifiTimeout = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
    wifiTimeout++;
    if (wifiTimeout > 20) {
      Serial.println("\nFailed to connect to WiFi. Restarting...");
      ESP.restart();
    }
  }
  Serial.println("\nWiFi connected.");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

void loop() {
  server.handleClient();

  if (millis() - lastDhtReadTime >= dhtReadInterval) {
    float temp = dht.readTemperature();
    if (!isnan(temp)) {
      currentTemperature = temp;
    }
    lastDhtReadTime = millis();
  }

  if (fanOverride && (millis() - fanOverrideStartTime) >= fanOverrideDuration) {
    fanOverride = false;
    digitalWrite(FAN_PIN, LOW);
  }

  int currentMainledswReading = digitalRead(MAINLEDSW_PIN);
  if (currentMainledswReading != lastMainledswReading && currentMainledswReading == LOW) {
    if (masterswState == 1) {
      mainledswState = 1 - mainledswState;
    }
  }
  lastMainledswReading = currentMainledswReading;

  int currentMasterswReading = digitalRead(MASTERSW_PIN);
  if (currentMasterswReading != lastMasterswReading && currentMasterswReading == LOW) {
    masterswState = 1 - masterswState;
  }
  lastMasterswReading = currentMasterswReading;

  if (mainledswState == 1 && masterswState == 1) {
    digitalWrite(MAINLED_PIN, HIGH);
  } else {
    digitalWrite(MAINLED_PIN, LOW);
  }

  controlNightLED();
  controlFan();

  yield();
}
