#include "DHT.h"
#include "WiFi.h"
#include "WebServer.h"
#include "ArduinoJson.h"
#include "Preferences.h"
#include "driver/ledc.h"
#include "HTTPClient.h"

#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// Define GPIO pins (ESP32-C3 safe mapping)
const int fanrelay = 6;         // Relay / Fan control
const int lightsensor = 2;      // LDR (analog input, ADC1_CH2)
const int nightled = 7;         // PWM brightness control (LDR LED)
const int mainledsw = 0;        // Touch sensor
const int proximitysw = 3;      // Proximity sensor
const int mainled = 1;          // Main LED PWM control

int currentTouchValue = 0;
int lastTouchValue = 0;
int currentProximityValue = 0;
int lastProximityValue = 0;

int lightThreshold = 2350;

const ledc_mode_t ledcMode = LEDC_LOW_SPEED_MODE;
const ledc_timer_bit_t ledcResolution = LEDC_TIMER_13_BIT;
const int ledcMaxValue = 8191; // 2^13 - 1

const ledc_timer_t ledcTimer_ldr = LEDC_TIMER_0;
const ledc_channel_t ledcChannel_ldr = LEDC_CHANNEL_0;
int ledcBaseFreq = 5000;
int currentBrightnessDutyCycle = 4096; // Default 50% brightness for LDR LED (8192 / 2)

const ledc_timer_t ledcTimer_mainLed = LEDC_TIMER_1;
const ledc_channel_t ledcChannel_mainLed = LEDC_CHANNEL_1;
int mainLedBaseFreq = 5000;
int mainLedBrightnessDutyCycle = 8191; // Main LED full brightness

const float tempOffset = -0.1;
const float humidityOffset = 6.0;

float tempOn = 29.0;
float tempOff = 28.5;

const char* ssid = "wifi_slow";
IPAddress staticIP(192, 168, 1, 4);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

WebServer server(80);
Preferences preferences;

unsigned long previousSensorMillis = 0;
const long sensorInterval = 10000; // Sensor read interval (10 seconds)
unsigned long lastLDRChangeTime = 0;
long debounceDelay = 50; // Reduced to make the night LED react faster

float lastHumidity = 0;
float lastTemperature = 0;
int lastLight = 0;
bool lastLDRState;
bool mainLedManualState = false;
bool proximityManualState = false; // This variable controls the master switch state
bool lastFanState = false;
int lastNightLedDuty = 0;

enum ControlMode { AUTOMATED, MANUAL_ON_PERMANENT, MANUAL_ON_TIMED };
ControlMode currentMode = AUTOMATED;
unsigned long manualTimerEnd = 0;

void readSensors() {
  lastHumidity = dht.readHumidity() + humidityOffset;
  lastTemperature = dht.readTemperature() + tempOffset;
  lastLight = analogRead(lightsensor);
}

void sendEventLogToPi(String event_message) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin("http://192.168.1.3:5001/log");
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<512> jsonDoc;
    jsonDoc["event_message"] = event_message;

    String jsonPayload;
    serializeJson(jsonDoc, jsonPayload);

    int httpResponseCode = http.POST(jsonPayload);

    if (httpResponseCode > 0) {
      String response = http.getString();
    } else {
      // Handle error
    }
    http.end();
  }
}

/**
 * @brief Sets the fan relay to a permanent ON state.
 */
void handleOn() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  currentMode = MANUAL_ON_PERMANENT;
  digitalWrite(fanrelay, HIGH);
  sendEventLogToPi("Fan is now manually ON (permanent).");
  server.send(200, "text/plain", "GPIO 6 is now manually ON (permanent).");
}

/**
 * @brief Sets the fan relay to OFF and resumes automated control.
 */
void handleOff() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  currentMode = AUTOMATED;
  digitalWrite(fanrelay, LOW);
  sendEventLogToPi("Fan is now manually OFF. Automated control resumed.");
  server.send(200, "text/plain", "GPIO 6 is now OFF. Automated control resumed.");
}

/**
 * @brief Sets the fan relay ON for 1 hour.
 */
void handleOn1h() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  currentMode = MANUAL_ON_TIMED;
  digitalWrite(fanrelay, HIGH);
  manualTimerEnd = millis() + 3600000;
  sendEventLogToPi("Fan is now manually ON for 1 hour.");
  server.send(200, "text/plain", "GPIO 6 is now manually ON for 1 hour.");
}

/**
 * @brief Sets the fan relay ON for 30 minutes.
 */
void handleOn30m() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  currentMode = MANUAL_ON_TIMED;
  digitalWrite(fanrelay, HIGH);
  manualTimerEnd = millis() + 1800000;
  sendEventLogToPi("Fan is now manually ON for 30 minutes.");
  server.send(200, "text/plain", "GPIO 6 is now manually ON for 30 minutes.");
}

/**
 * @brief Sets the temperature ON threshold.
 */
void handleSetTempOn() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.hasArg("value")) {
    tempOn = server.arg("value").toFloat();
    preferences.putFloat("tempOn", tempOn);
    sendEventLogToPi("Temperature ON threshold set to " + String(tempOn) + "C.");
    server.send(200, "text/plain", "Temperature ON threshold set to " + String(tempOn) + "C.");
  } else {
    server.send(400, "text/plain", "Missing 'value' parameter.");
  }
}

/**
 * @brief Sets the temperature OFF threshold.
 */
void handleSetTempOff() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.hasArg("value")) {
    tempOff = server.arg("value").toFloat();
    preferences.putFloat("tempOff", tempOff);
    sendEventLogToPi("Temperature OFF threshold set to " + String(tempOff) + "C.");
    server.send(200, "text/plain", "Temperature OFF threshold set to " + String(tempOff) + "C.");
  } else {
    server.send(400, "text/plain", "Missing 'value' parameter.");
  }
}

/**
 * @brief Sets the night LED brightness.
 */
void handleSetBrightness() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.hasArg("value")) {
    int requestedValue = server.arg("value").toInt();
    if (requestedValue >= 0 && requestedValue <= ledcMaxValue) {
      currentBrightnessDutyCycle = requestedValue;
      preferences.putUInt("brightness", currentBrightnessDutyCycle);

      ledc_set_duty(ledcMode, ledcChannel_ldr, currentBrightnessDutyCycle);
      ledc_update_duty(ledcMode, ledcChannel_ldr);
      float percent = (float)currentBrightnessDutyCycle / ledcMaxValue * 100.0;
      sendEventLogToPi("Night LED brightness level set to " + String(percent, 1) + "%.");
      server.send(200, "text/plain", "LED brightness level set to " + String(percent, 1) + "%..");
    } else {
      server.send(400, "text/plain", "Invalid 'value' parameter. Must be between 0 and 8191.");
    }
  } else {
    server.send(400, "text/plain", "Missing 'value' parameter.");
  }
}

/**
 * @brief Sets the light sensor threshold.
 */
void handleSetLightThreshold() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.hasArg("value")) {
    lightThreshold = server.arg("value").toInt();
    preferences.putUInt("lightThreshold", lightThreshold);
    sendEventLogToPi("Light sensor threshold set to " + String(lightThreshold) + ".");
    server.send(200, "text/plain", "Light sensor threshold set to " + String(lightThreshold) + ".");
  } else {
    server.send(400, "text/plain", "Missing 'value' parameter.");
  }
}

/**
 * @brief Sets the main LED brightness.
 */
void handleSetMainLedBrightness() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.hasArg("value")) {
    int requestedValue = server.arg("value").toInt();
    if (requestedValue >= 0 && requestedValue <= ledcMaxValue) {
      mainLedBrightnessDutyCycle = requestedValue;
      preferences.putUInt("mainLedBrightness", mainLedBrightnessDutyCycle);
      float percent = (float)mainLedBrightnessDutyCycle / ledcMaxValue * 100.0;
      sendEventLogToPi("Main LED brightness level set to " + String(percent, 1) + "%.");
      server.send(200, "text/plain", "Main LED brightness level set to " + String(percent, 1) + "%..");
    } else {
      server.send(400, "text/plain", "Invalid 'value' parameter. Must be between 0 and 8191.");
    }
  } else {
    server.send(400, "text/plain", "Missing 'value' parameter.");
  }
}

/**
 * @brief Sets the debounce delay for the night LED.
 */
void handleSetDebounceDelay() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.hasArg("value")) {
    debounceDelay = server.arg("value").toInt();
    preferences.putUInt("debounceDelay", debounceDelay);
    sendEventLogToPi("Night LED debounce delay set to " + String(debounceDelay) + "ms.");
    server.send(200, "text/plain", "Night LED debounce delay set to " + String(debounceDelay) + "ms.");
  } else {
    server.send(400, "text/plain", "Missing 'value' parameter.");
  }
}

/**
 * @brief Toggles the main LED state and saves it.
 */
void handleToggleMainLed() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (proximityManualState) {
    mainLedManualState = !mainLedManualState;
    preferences.putBool("mainLedManualState", mainLedManualState);
    sendEventLogToPi("Main LED state toggled to " + String(mainLedManualState ? "ON" : "OFF") + ".");
    server.send(200, "text/plain", "Main LED state toggled to " + String(mainLedManualState ? "ON" : "OFF") + ".");
  } else {
    server.send(200, "text/plain", "Master switch is OFF. Cannot toggle main LED.");
  }
}

/**
 * @brief Toggles the master switch state and saves it.
 */
void handleToggleMasterSwitch() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  proximityManualState = !proximityManualState;
  preferences.putBool("proximityManualState", proximityManualState);
  sendEventLogToPi("Master switch state toggled to " + String(proximityManualState ? "ON" : "OFF") + ".");
  server.send(200, "text/plain", "Master switch state toggled to " + String(proximityManualState ? "ON" : "OFF") + ".");
}

/**
 * @brief Provides sensor data and current status in JSON format.
 */
void handleData() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  StaticJsonDocument<512> jsonDoc;
  char tempStr[6];
  sprintf(tempStr, "%.1f", lastTemperature);

  jsonDoc["temperature"] = tempStr;
  jsonDoc["humidity"] = (int)lastHumidity;
  jsonDoc["light"] = lastLight;
  jsonDoc["gpio_status"] = digitalRead(fanrelay) == HIGH ? "on" : "off";
  jsonDoc["master_switch_state"] = proximityManualState ? "on" : "off";
  jsonDoc["main_led_status"] = mainLedManualState ? "on" : "off";
  jsonDoc["control_mode"] = currentMode == AUTOMATED ? "Automated" : (currentMode == MANUAL_ON_PERMANENT ? "Manual (Permanent)" : "Manual (Timed)");
  float nightLedPercent = (float)ledc_get_duty(ledcMode, ledcChannel_ldr) / ledcMaxValue * 100.0;
  float mainLedPercent = (float)ledc_get_duty(ledcMode, ledcChannel_mainLed) / ledcMaxValue * 100.0;
  jsonDoc["ldr_brightness_level"] = nightLedPercent;
  jsonDoc["main_led_brightness_level"] = mainLedPercent;
  jsonDoc["temp_on_threshold"] = tempOn;
  jsonDoc["temp_off_threshold"] = tempOff;
  jsonDoc["light_threshold"] = lightThreshold;
  jsonDoc["debounce_delay_ms"] = debounceDelay;

  String response;
  serializeJson(jsonDoc, response);
  server.send(200, "application/json", response);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not Found");
}

void setup() {
  setCpuFrequencyMhz(80);
  Serial.begin(9600);
  sendEventLogToPi("System started.");
  dht.begin();
  preferences.begin("my-app", false);

  tempOn = preferences.getFloat("tempOn", 29.0);
  tempOff = preferences.getFloat("tempOff", 28.5);
  currentBrightnessDutyCycle = preferences.getUInt("brightness", 4096);
  lightThreshold = preferences.getUInt("lightThreshold", 2350);
  mainLedBrightnessDutyCycle = preferences.getUInt("mainLedBrightness", 8191);
  ledcBaseFreq = preferences.getUInt("pwmFrequency", 5000);
  mainLedBaseFreq = preferences.getUInt("mainLedPwmFrequency", 5000);
  proximityManualState = preferences.getBool("proximityManualState", false);
  mainLedManualState = preferences.getBool("mainLedManualState", false);
  debounceDelay = preferences.getUInt("debounceDelay", 50);

  pinMode(fanrelay, OUTPUT);
  pinMode(mainledsw, INPUT);
  pinMode(proximitysw, INPUT);
  pinMode(mainled, OUTPUT);
  digitalWrite(fanrelay, LOW);

  // Initialize LEDC timers
  ledc_timer_config_t ledcTimerConfig_ldr = {
    .speed_mode = ledcMode,
    .duty_resolution = ledcResolution,
    .timer_num = ledcTimer_ldr,
    .freq_hz = ledcBaseFreq,
    .clk_cfg = LEDC_AUTO_CLK
  };
  ledc_timer_config(&ledcTimerConfig_ldr);

  ledc_timer_config_t ledcTimerConfig_mainLed = {
    .speed_mode = ledcMode,
    .duty_resolution = ledcResolution,
    .timer_num = ledcTimer_mainLed,
    .freq_hz = mainLedBaseFreq,
    .clk_cfg = LEDC_AUTO_CLK
  };
  ledc_timer_config(&ledcTimerConfig_mainLed);

  // Initialize LEDC channels
  ledc_channel_config_t ledcChannelConfig_ldr = {
    .gpio_num = nightled,
    .speed_mode = ledcMode,
    .channel = ledcChannel_ldr,
    .intr_type = LEDC_INTR_DISABLE,
    .timer_sel = ledcTimer_ldr,
    .duty = 0,
    .hpoint = 0
  };
  ledc_channel_config(&ledcChannelConfig_ldr);

  ledc_channel_config_t ledcChannelConfig_mainLed = {
    .gpio_num = mainled,
    .speed_mode = ledcMode,
    .channel = ledcChannel_mainLed,
    .intr_type = LEDC_INTR_DISABLE,
    .timer_sel = ledcTimer_mainLed,
    .duty = 0,
    .hpoint = 0
  };
  ledc_channel_config(&ledcChannelConfig_mainLed);

  // Now set the duties after initialization
  ledc_set_duty(ledcMode, ledcChannel_mainLed, 0);
  ledc_update_duty(ledcMode, ledcChannel_mainLed);
  ledc_set_duty(ledcMode, ledcChannel_ldr, currentBrightnessDutyCycle);
  ledc_update_duty(ledcMode, ledcChannel_ldr);
  sendEventLogToPi("Initial fan state: OFF, initial main LED state: OFF.");

  WiFi.config(staticIP, gateway, subnet);
  WiFi.begin(ssid);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  sendEventLogToPi("WiFi connected. IP address: " + WiFi.localIP().toString() + ".");

  server.on("/on-perm", handleOn);
  server.on("/off", handleOff);
  server.on("/on-1h", handleOn1h);
  server.on("/on-30m", handleOn30m);
  server.on("/set-temp-on", handleSetTempOn);
  server.on("/set-temp-off", handleSetTempOff);
  server.on("/set-brightness", handleSetBrightness);
  server.on("/set-light-threshold", handleSetLightThreshold);
  server.on("/set-main-led-brightness", handleSetMainLedBrightness);
  server.on("/set-debounce-delay", handleSetDebounceDelay);
  server.on("/toggle-main-led", handleToggleMainLed);
  server.on("/toggle-master-switch", handleToggleMasterSwitch);
  server.on("/data", handleData);
  server.onNotFound(handleNotFound);
  server.begin();

  readSensors();
  lastLDRState = lastLight < lightThreshold;
  if (lastLDRState) {
    ledc_set_duty(ledcMode, ledcChannel_ldr, currentBrightnessDutyCycle);
    ledc_update_duty(ledcMode, ledcChannel_ldr);
  } else {
    ledc_set_duty(ledcMode, ledcChannel_ldr, 0);
    ledc_update_duty(ledcMode, ledcChannel_ldr);
  }

  // Fix: Initialize lastProximityValue with the current state of the pin
  lastProximityValue = digitalRead(proximitysw);
  sendEventLogToPi("Initial master switch state: " + String(proximityManualState ? "ON" : "OFF"));
  sendEventLogToPi("Initial main LED state: " + String(mainLedManualState ? "ON" : "OFF"));


  lastFanState = digitalRead(fanrelay);
  lastNightLedDuty = ledc_get_duty(ledcMode, ledcChannel_ldr);
}

void loop() {
  server.handleClient();
  unsigned long currentMillis = millis();

  // Handle sensor reading on a timed interval
  if (currentMillis - previousSensorMillis >= sensorInterval) {
    previousSensorMillis = currentMillis;
    readSensors();
    if (isnan(lastHumidity) || isnan(lastTemperature)) {
      sendEventLogToPi("Failed to read from DHT sensor!");
    }
  }

  // Handle latching for proximitysw (master switch toggle)
  currentProximityValue = digitalRead(proximitysw);
  if (currentProximityValue == HIGH && lastProximityValue == LOW) {
    proximityManualState = !proximityManualState;
    preferences.putBool("proximityManualState", proximityManualState);
    sendEventLogToPi("Master switch state toggled to " + String(proximityManualState ? "ON" : "OFF") + ".");
  }
  lastProximityValue = currentProximityValue;

  // Master Control: proximityManualState dictates main operations
  if (proximityManualState) {
    // Handle latching for mainledsw (main LED toggle)
    currentTouchValue = digitalRead(mainledsw);
    if (currentTouchValue == HIGH && lastTouchValue == LOW) {
      mainLedManualState = !mainLedManualState;
      preferences.putBool("mainLedManualState", mainLedManualState);
      sendEventLogToPi("Main LED state toggled to " + String(mainLedManualState ? "ON" : "OFF") + ".");
    }
    lastTouchValue = currentTouchValue;

    // Control Main LED
    if (mainLedManualState) {
      ledc_set_duty(ledcMode, ledcChannel_mainLed, mainLedBrightnessDutyCycle);
    } else {
      ledc_set_duty(ledcMode, ledcChannel_mainLed, 0);
    }
    ledc_update_duty(ledcMode, ledcChannel_mainLed);

    // Control Fan
    // Check for manual timed mode timeout first
    if (currentMode == MANUAL_ON_TIMED && currentMillis >= manualTimerEnd) {
      currentMode = AUTOMATED;
      digitalWrite(fanrelay, LOW);
      sendEventLogToPi("Manual timer ended. Fan is now OFF.");
    }
    
    if (currentMode == AUTOMATED) {
      if (lastTemperature >= tempOn && digitalRead(fanrelay) == LOW) {
        digitalWrite(fanrelay, HIGH);
        sendEventLogToPi("Fan is now ON due to high temperature (" + String(lastTemperature, 1) + "C).");
      } else if (lastTemperature <= tempOff && digitalRead(fanrelay) == HIGH) {
        digitalWrite(fanrelay, LOW);
        sendEventLogToPi("Fan is now OFF due to low temperature (" + String(lastTemperature, 1) + "C).");
      }
    }
  } else {
    // If master switch is OFF, turn off both fan and main LED
    if (digitalRead(fanrelay) == HIGH) {
      digitalWrite(fanrelay, LOW);
      sendEventLogToPi("Master switch OFF. Fan is now OFF.");
    }
    if (ledc_get_duty(ledcMode, ledcChannel_mainLed) > 0) {
      ledc_set_duty(ledcMode, ledcChannel_mainLed, 0);
      ledc_update_duty(ledcMode, ledcChannel_mainLed);
      sendEventLogToPi("Master switch OFF. Main LED is now OFF.");
    }
  }

  // LDR and Brightness Control Logic
  int currentLight = analogRead(lightsensor);
  bool currentLDRState = currentLight < lightThreshold;

  // Use a simple debounce logic for LDR
  if (currentLDRState != lastLDRState) {
    lastLDRChangeTime = currentMillis;
  }

  if ((currentMillis - lastLDRChangeTime) >= debounceDelay) {
    if (currentLDRState) {
      if (ledc_get_duty(ledcMode, ledcChannel_ldr) != currentBrightnessDutyCycle) {
        ledc_set_duty(ledcMode, ledcChannel_ldr, currentBrightnessDutyCycle);
        ledc_update_duty(ledcMode, ledcChannel_ldr);
        sendEventLogToPi("Night LED ON. Light level is " + String(currentLight) + ".");
      }
    } else {
      if (ledc_get_duty(ledcMode, ledcChannel_ldr) > 0) {
        ledc_set_duty(ledcMode, ledcChannel_ldr, 0);
        ledc_update_duty(ledcMode, ledcChannel_ldr);
        sendEventLogToPi("Night LED OFF. Light level is " + String(currentLight) + ".");
      }
    }
  }
  lastLDRState = currentLDRState;
  
  // This delay() gives the watchdog timer a chance to be reset.
  delay(1);
}
