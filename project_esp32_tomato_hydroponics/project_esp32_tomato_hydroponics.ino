/*
  Tomato Hydroponics Controller - ESP32
  Refactored for:
  - 3 hardware buttons (GPIO16, GPIO17, GPIO4 NC)
  - 4CH relay + 1CH relay mapping
  - LCD 12864 real-time status
  - Non-blocking control flow with millis()
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include <U8g2lib.h>
#include <math.h>
#include <string.h>
#include <time.h>

// ---- Sensor Pins ----
#define PH_PIN            35
#define EC_PIN            34
#define SAMPLES           20

// ---- Ultrasonic HC-SR04 ----
#define TRIG_PIN          25
#define ECHO_PIN          32
#define WATER_FULL_DIST   20.0f
#define WATER_EMPTY_DIST  42.0f

// ---- Relay Pins ----
// 4CH relay: 13, 12, 14, 26
// 1CH relay: 27
#define RELAY_PUMP_A      13
#define RELAY_PUMP_B      12
#define RELAY_PUMP_PH     14
#define RELAY_SOLENOID_1  26
#define RELAY_MAIN_PUMP   27
const bool RELAY_ACTIVE_HIGH = true; // Reverted back to true for Active-High Relays

// ---- Buttons ----
#define BTN_MAIN_PIN      16   // NO: main pump + solenoid toggle
#define BTN_STAGE_PIN     17   // NO: growth-stage toggle
#define BTN_ESTOP_PIN      4   // NC: emergency stop (active HIGH with INPUT_PULLUP)
#define BTN_DEBOUNCE_MS   50

// ---- LCD ST7920 128x64 ----
#define LCD_CLK           18
#define LCD_DATA          23
#define LCD_CS            5
#define LCD_RST           22
U8G2_ST7920_128X64_F_SW_SPI u8g2(U8G2_R0, LCD_CLK, LCD_DATA, LCD_CS, LCD_RST);

// ---- pH EEPROM ----
#define PH_ADDR_SLOPE     0
#define PH_ADDR_INTERCEPT 4
#define PH_ADDR_VALID     8
#define PH_VALID_MAGIC    0xAB

// ---- EC EEPROM ----
#define EC_KVALUEADDR     0x0A
#define EC_ADDR_VALID     0x12
#define EC_VALID_MAGIC    0xEC
#define RES2              820.0f
#define ECREF             200.0f
#define VREF              3300.0f
#define ADC_RES           4095.0f
#define EC_DEFAULT_TEMP   25.0f

// ---- WiFi ----
const char* WIFI_SSID = "Yumgaizap";
const char* WIFI_PASS = "0625321533";

// ---- MQTT ----
const char* MQTT_SERVER = "8218cf51f5de4ac5a776aa0efb931888.s1.eu.hivemq.cloud";
const int MQTT_PORT = 8883;
const char* MQTT_USER = "tomato-esp32";
const char* MQTT_PASS = "tomato_project_Y3";

// Publish topics
const char* T_SENSOR_EC     = "hydroponics/sensor/ec";
const char* T_SENSOR_PH     = "hydroponics/sensor/ph";
const char* T_SENSOR_WATER  = "hydroponics/sensor/water";
const char* T_PUMP_A        = "hydroponics/pump/a";
const char* T_PUMP_B        = "hydroponics/pump/b";
const char* T_PUMP_PH       = "hydroponics/pump/ph";
const char* T_PUMP_MAIN     = "hydroponics/pump/main";
const char* T_SYS_STATUS    = "hydroponics/system/status";
const char* T_SYS_AUTO      = "hydroponics/system/auto";

// Subscribe topics
const char* T_CTRL_EC       = "hydroponics/control/ec_target";
const char* T_CTRL_PH       = "hydroponics/control/ph_target";
const char* T_SYS_EMERGENCY = "hydroponics/system/emergency";
const char* T_CTRL_CALIBRATE = "hydroponics/control/calibrate";

WiFiClientSecure espClient;
PubSubClient mqtt(espClient);

// ---- Runtime State ----
bool pumpA_on = false;
bool pumpB_on = false;
bool pumpPh_on = false;
bool mainPump_on = false;
bool solenoid1_on = false;

float targetEc = 2.0f;
float targetPh = 6.2f;
float ecValue = 0.0f;
float phValue = 0.0f;
float waterLevel = 0.0f;

unsigned long lastDataSend = 0;
const unsigned long DATA_INTERVAL = 2000;
unsigned long lastLcdUpdate = 0;
const unsigned long LCD_UPDATE_INTERVAL = 250;
unsigned long lastMqttReconnectAttempt = 0;
const unsigned long MQTT_RECONNECT_INTERVAL = 3000;
unsigned long lastWifiReconnectAttempt = 0;
const unsigned long WIFI_RECONNECT_INTERVAL = 5000;
unsigned long lastWifiLog = 0;
const unsigned long WIFI_LOG_INTERVAL = 1500;

// ---- Auto dosing ----
bool autoMode = false;
const float EC_TOLERANCE = 0.05f;
const float PH_TOLERANCE = 0.05f;

enum AutoState { DOSING_IDLE, DOSING_A, DOSING_WAIT_B, DOSING_B, DOSING_MIX_EC, DOSING_PH, DOSING_MIX_PH };
AutoState autoState = DOSING_IDLE;
unsigned long dosingStateStartTime = 0;

const unsigned long PUMP_DOSE_MS = 2000;        // Dose for 2s
const unsigned long PUMP_B_DELAY = 2000;        // Wait 2s before A -> B
const unsigned long PUMP_MIXING_MS = 30000;     // Mix for 30s before reading again
const unsigned long PH_DOSE_MS = 1500;          // Dose pH for 1.5s
const unsigned long PH_MIXING_MS = 20000;       // Mix pH for 20s

// ---- Daily Schedule ----
bool dailyScheduleActive = false;
bool dailyMixingDone = false;

// ---- Emergency ----
bool emergencyStopActive = false;

// ---- Calibration ----
float phSlope = -0.004593f;
float phIntercept = 21.0f;
float ecKvalueLow = 1.0f;
float ecKvalueHigh = 1.0f;
float ecKvalue = 1.0f;
float ecTemperature = EC_DEFAULT_TEMP;

// ---- Growth Stage ----
struct GrowthStage {
  const char* name;
  float ecTarget;
  float phTarget;
};

GrowthStage growthStages[] = {
  {"Seedling",   1.20f, 6.20f},
  {"Vegetative", 1.80f, 6.00f},
  {"Flowering",  2.30f, 5.80f},
  {"Ripening",   2.00f, 6.10f}
};

const int GROWTH_STAGE_COUNT = sizeof(growthStages) / sizeof(growthStages[0]);
int activeStageIndex = 0; // -1 means custom target

// ---- Debounced Button ----
struct DebouncedButton {
  uint8_t pin;
  bool activeLow;
  bool stableState;
  bool lastReading;
  unsigned long lastDebounceTime;
  bool pressedEvent;
};

DebouncedButton btnMain  = {BTN_MAIN_PIN, true,  HIGH, HIGH, 0, false};
DebouncedButton btnStage = {BTN_STAGE_PIN, true, HIGH, HIGH, 0, false};
DebouncedButton btnEstop = {BTN_ESTOP_PIN, false, HIGH, HIGH, 0, false};

// ---- Forward declarations ----
void sendSensorData();
void sendPumpState(const char* pump, bool state);
void reconnectMQTT();
void stopAllPumps(bool publishState = true);
void updateSensors();
void updateLCD();
void processAutoMode();
void handleCalibration(const char* cmd);

void writeRelay(uint8_t relayPin, bool on) {
  if (RELAY_ACTIVE_HIGH) {
    digitalWrite(relayPin, on ? HIGH : LOW);
  } else {
    digitalWrite(relayPin, on ? LOW : HIGH);
  }
}

void forceRelaysOff() {
  writeRelay(RELAY_PUMP_A, false);
  writeRelay(RELAY_PUMP_B, false);
  writeRelay(RELAY_PUMP_PH, false);
  writeRelay(RELAY_MAIN_PUMP, false);
  writeRelay(RELAY_SOLENOID_1, false);
}

void setMainFlow(bool on, bool publishState = true) {
  mainPump_on = on;
  solenoid1_on = on;
  writeRelay(RELAY_MAIN_PUMP, mainPump_on);
  writeRelay(RELAY_SOLENOID_1, solenoid1_on);
  if (publishState) {
    sendPumpState("mainPump", mainPump_on);
  }
}

const char* getStageName() {
  if (activeStageIndex >= 0 && activeStageIndex < GROWTH_STAGE_COUNT) {
    return growthStages[activeStageIndex].name;
  }
  return "Custom";
}

void applyGrowthStage(int nextIndex, bool publishTargets) {
  if (nextIndex < 0) {
    nextIndex = 0;
  }
  activeStageIndex = nextIndex % GROWTH_STAGE_COUNT;
  targetEc = growthStages[activeStageIndex].ecTarget;
  targetPh = growthStages[activeStageIndex].phTarget;

  Serial.printf("[Stage] %s -> EC %.2f, pH %.2f\n", getStageName(), targetEc, targetPh);
  if (publishTargets) {
    sendSensorData();
  }
}

void initButton(DebouncedButton& b) {
  pinMode(b.pin, INPUT_PULLUP);
  b.stableState = digitalRead(b.pin);
  b.lastReading = b.stableState;
  b.lastDebounceTime = millis();
  b.pressedEvent = false;
}

void updateButton(DebouncedButton& b, unsigned long nowMs) {
  b.pressedEvent = false;
  bool reading = digitalRead(b.pin);

  if (reading != b.lastReading) {
    b.lastDebounceTime = nowMs;
    b.lastReading = reading;
  }

  if ((nowMs - b.lastDebounceTime) >= BTN_DEBOUNCE_MS && reading != b.stableState) {
    b.stableState = reading;
    bool pressed = b.activeLow ? (b.stableState == LOW) : (b.stableState == HIGH);
    if (pressed) {
      b.pressedEvent = true;
    }
  }
}

void loadPhCalibration() {
  if (EEPROM.read(PH_ADDR_VALID) == PH_VALID_MAGIC) {
    EEPROM.get(PH_ADDR_SLOPE, phSlope);
    EEPROM.get(PH_ADDR_INTERCEPT, phIntercept);
    Serial.printf("[pH] Calibration loaded: slope=%.6f intercept=%.4f\n", phSlope, phIntercept);
  } else {
    Serial.println("[pH] No calibration found, using defaults");
  }
}

void loadEcCalibration() {
  if (EEPROM.read(EC_ADDR_VALID) == EC_VALID_MAGIC) {
    EEPROM.get(EC_KVALUEADDR, ecKvalueLow);
    EEPROM.get(EC_KVALUEADDR + 4, ecKvalueHigh);
    if (ecKvalueLow < 0.5f || ecKvalueLow > 1.5f || ecKvalueHigh < 0.5f || ecKvalueHigh > 1.5f) {
      ecKvalueLow = 1.0f;
      ecKvalueHigh = 1.0f;
      Serial.println("[EC] Calibration out of range, reset to defaults");
    } else {
      Serial.printf("[EC] Calibration loaded: Klow=%.4f Khigh=%.4f\n", ecKvalueLow, ecKvalueHigh);
    }
    ecKvalue = ecKvalueLow;
  } else {
    Serial.println("[EC] No calibration found, using defaults (K=1.0)");
  }
}

float readPH() {
  long sum = 0;
  for (int i = 0; i < SAMPLES; i++) {
    sum += analogRead(PH_PIN);
  }
  float avg = (float)sum / SAMPLES;
  float ph = phSlope * avg + phIntercept;
  return constrain(ph, 0.0f, 14.0f);
}

float readEC() {
  long sum = 0;
  for (int i = 0; i < SAMPLES; i++) {
    sum += analogRead(EC_PIN);
  }

  float avg = (float)sum / SAMPLES;
  float voltage = avg / ADC_RES * VREF;
  float rawEC = 1000.0f * voltage / RES2 / ECREF;

  if (rawEC * ecKvalue > 2.5f) {
    ecKvalue = ecKvalueHigh;
  } else if (rawEC * ecKvalue < 2.0f) {
    ecKvalue = ecKvalueLow;
  }

  float ec = rawEC * ecKvalue;
  ec = ec / (1.0f + 0.0185f * (ecTemperature - 25.0f));
  return max(ec, 0.0f);
}

float readWaterLevelOnce() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(5);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 50000);
  if (duration == 0) {
    return -1.0f;
  }

  return duration * 0.034f / 2.0f;
}

float readWaterLevel() {
  float distance = readWaterLevelOnce();
  if (distance < 0) {
    return waterLevel;
  }

  float level = (WATER_EMPTY_DIST - distance) / (WATER_EMPTY_DIST - WATER_FULL_DIST) * 100.0f;
  return constrain(level, 0.0f, 100.0f);
}

void updateSensors() {
  phValue = readPH();
  ecValue = readEC();
  waterLevel = readWaterLevel();
}

void updateLCD() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);

  char line[32];

  snprintf(line, sizeof(line), "STG:%s A:%d E:%d", getStageName(), autoMode ? 1 : 0, emergencyStopActive ? 1 : 0);
  u8g2.drawStr(0, 8, line);

  snprintf(line, sizeof(line), "EC %.2f / %.2f", ecValue, targetEc);
  u8g2.drawStr(0, 18, line);

  snprintf(line, sizeof(line), "pH %.2f / %.2f", phValue, targetPh);
  u8g2.drawStr(0, 28, line);

  snprintf(line, sizeof(line), "R A:%d B:%d PH:%d", pumpA_on ? 1 : 0, pumpB_on ? 1 : 0, pumpPh_on ? 1 : 0);
  u8g2.drawStr(0, 38, line);

  snprintf(line, sizeof(line), "R M:%d S1:%d H2O:%d%%", mainPump_on ? 1 : 0, solenoid1_on ? 1 : 0, (int)waterLevel);
  u8g2.drawStr(0, 48, line);

  snprintf(line, sizeof(line), "WiFi:%s MQTT:%s", WiFi.status() == WL_CONNECTED ? "ON" : "OFF", mqtt.connected() ? "ON" : "OFF");
  u8g2.drawStr(0, 58, line);

  u8g2.sendBuffer();
}

void sendPumpState(const char* pump, bool state) {
  if (!mqtt.connected()) {
    return;
  }

  const char* topic = nullptr;
  if (strcmp(pump, "pumpA") == 0) {
    topic = T_PUMP_A;
  } else if (strcmp(pump, "pumpB") == 0) {
    topic = T_PUMP_B;
  } else if (strcmp(pump, "pumpPh") == 0) {
    topic = T_PUMP_PH;
  } else if (strcmp(pump, "mainPump") == 0) {
    topic = T_PUMP_MAIN;
  }

  if (topic != nullptr) {
    mqtt.publish(topic, state ? "1" : "0");
  }
}

void stopAllPumps(bool publishState) {
  pumpA_on = false;
  pumpB_on = false;
  pumpPh_on = false;
  mainPump_on = false;
  solenoid1_on = false;
  autoState = DOSING_IDLE;

  forceRelaysOff();

  if (publishState) {
    sendPumpState("pumpA", false);
    sendPumpState("pumpB", false);
    sendPumpState("pumpPh", false);
    sendPumpState("mainPump", false);
  }
}

void handleHardwareButtons(unsigned long nowMs) {
  updateButton(btnMain, nowMs);
  updateButton(btnStage, nowMs);
  updateButton(btnEstop, nowMs);

  bool estopNow = (btnEstop.stableState == HIGH);
  if (estopNow) {
    if (!emergencyStopActive) {
      emergencyStopActive = true;
      autoMode = false;
      stopAllPumps(true);
      if (mqtt.connected()) {
        mqtt.publish(T_SYS_AUTO, "0");
        mqtt.publish(T_SYS_STATUS, "hardware_estop", false);
      }
      Serial.println("[E-STOP] ACTIVE - all relays OFF");
    } else {
      forceRelaysOff();
    }
    return;
  }

  if (emergencyStopActive) {
    emergencyStopActive = false;
    Serial.println("[E-STOP] Released");
  }

  if (btnMain.pressedEvent) {
    bool next = !mainPump_on;
    setMainFlow(next, true);
    Serial.printf("[Button] Main flow %s\n", next ? "ON" : "OFF");
  }

  if (btnStage.pressedEvent) {
    int nextStage = (activeStageIndex < 0) ? 0 : (activeStageIndex + 1) % GROWTH_STAGE_COUNT;
    applyGrowthStage(nextStage, true);
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[128];
  unsigned int copyLen = length;
  if (copyLen >= sizeof(msg)) {
    copyLen = sizeof(msg) - 1;
  }
  memcpy(msg, payload, copyLen);
  msg[copyLen] = '\0';

  Serial.printf("[MQTT] %s => %s\n", topic, msg);

  if (strcmp(topic, T_SYS_EMERGENCY) == 0) {
    emergencyStopActive = true;
    autoMode = false;
    stopAllPumps(true);
    mqtt.publish(T_SYS_AUTO, "0");
    return;
  }

  if (emergencyStopActive) {
    return;
  }

  if (strcmp(topic, T_PUMP_A) == 0) {
    bool on = (msg[0] == '1');
    pumpA_on = on;
    writeRelay(RELAY_PUMP_A, pumpA_on);
    sendPumpState("pumpA", pumpA_on);
    if (autoMode) { autoMode = false; sendSensorData(); } // Break out of auto if manual override
  } else if (strcmp(topic, T_PUMP_B) == 0) {
    bool on = (msg[0] == '1');
    pumpB_on = on;
    writeRelay(RELAY_PUMP_B, pumpB_on);
    sendPumpState("pumpB", pumpB_on);
    if (autoMode) { autoMode = false; sendSensorData(); }
  } else if (strcmp(topic, T_PUMP_PH) == 0) {
    bool on = (msg[0] == '1');
    pumpPh_on = on;
    writeRelay(RELAY_PUMP_PH, pumpPh_on);
    sendPumpState("pumpPh", pumpPh_on);
    if (autoMode) { autoMode = false; sendSensorData(); }
  } else if (strcmp(topic, T_PUMP_MAIN) == 0) {
    bool on = (msg[0] == '1');
    setMainFlow(on, true);
  } else if (strcmp(topic, T_CTRL_EC) == 0) {
    targetEc = atof(msg);
    activeStageIndex = -1;
  } else if (strcmp(topic, T_CTRL_PH) == 0) {
    targetPh = atof(msg);
    activeStageIndex = -1;
  } else if (strcmp(topic, T_SYS_AUTO) == 0) {
    bool newAuto = (msg[0] == '1');
    if (autoMode != newAuto) {
      autoMode = newAuto;
      autoState = DOSING_IDLE;
      if (!autoMode) {
        stopAllPumps(true);
      }
    }
  } else if (strcmp(topic, T_CTRL_CALIBRATE) == 0) {
    handleCalibration(msg);
  }
}

float getRawADC(int pin) {
  long sum = 0;
  for (int i = 0; i < SAMPLES; i++) sum += analogRead(pin);
  return (float)sum / SAMPLES;
}

void handleCalibration(const char* cmd) {
  Serial.printf("[Calibrate] %s\n", cmd);
  
  if (strcmp(cmd, "ph_cal_7") == 0) {
    float avg = getRawADC(PH_PIN);
    phIntercept = 7.0f - (phSlope * avg);
    Serial.println("pH 7 Calibrated");
  } else if (strcmp(cmd, "ph_cal_4") == 0) {
    float avg = getRawADC(PH_PIN);
    phSlope = (4.0f - phIntercept) / avg;
    Serial.println("pH 4 Calibrated");
  } else if (strcmp(cmd, "ec_cal_low") == 0) {
    float avg = getRawADC(EC_PIN);
    float voltage = avg / ADC_RES * VREF;
    float rawEC = 1000.0f * voltage / RES2 / ECREF;
    if (rawEC > 0) ecKvalueLow = 1.41f / rawEC;
    Serial.println("EC Low Calibrated");
  } else if (strcmp(cmd, "ec_cal_high") == 0) {
    float avg = getRawADC(EC_PIN);
    float voltage = avg / ADC_RES * VREF;
    float rawEC = 1000.0f * voltage / RES2 / ECREF;
    if (rawEC > 0) ecKvalueHigh = 2.76f / rawEC;
    Serial.println("EC High Calibrated");
  } else if (strcmp(cmd, "ph_save") == 0) {
    EEPROM.put(PH_ADDR_SLOPE, phSlope);
    EEPROM.put(PH_ADDR_INTERCEPT, phIntercept);
    EEPROM.write(PH_ADDR_VALID, PH_VALID_MAGIC);
    EEPROM.commit();
    Serial.println("pH Saved");
  } else if (strcmp(cmd, "ec_save") == 0) {
    EEPROM.put(EC_KVALUEADDR, ecKvalueLow);
    EEPROM.put(EC_KVALUEADDR + 4, ecKvalueHigh);
    EEPROM.write(EC_ADDR_VALID, EC_VALID_MAGIC);
    EEPROM.commit();
    Serial.println("EC Saved");
  }
}

void reconnectMQTT() {
  String clientId = "ESP32Client-Tomato-";
  clientId += String(random(0xffff), HEX);

  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS, T_SYS_STATUS, 1, true, "offline")) {
    mqtt.publish(T_SYS_STATUS, "online", true);
    mqtt.subscribe(T_PUMP_A);
    mqtt.subscribe(T_PUMP_B);
    mqtt.subscribe(T_PUMP_PH);
    mqtt.subscribe(T_PUMP_MAIN);
    mqtt.subscribe(T_CTRL_EC);
    mqtt.subscribe(T_CTRL_PH);
    mqtt.subscribe(T_SYS_AUTO);
    mqtt.subscribe(T_SYS_EMERGENCY);
    mqtt.subscribe(T_CTRL_CALIBRATE);
    sendSensorData();
  }
}

void sendSensorData() {
  if (!mqtt.connected()) {
    return;
  }

  char buf[16];
  dtostrf(ecValue, 1, 2, buf);
  mqtt.publish(T_SENSOR_EC, buf);

  dtostrf(phValue, 1, 2, buf);
  mqtt.publish(T_SENSOR_PH, buf);

  dtostrf(waterLevel, 1, 1, buf);
  mqtt.publish(T_SENSOR_WATER, buf);

  dtostrf(targetEc, 1, 2, buf);
  mqtt.publish(T_CTRL_EC, buf);

  dtostrf(targetPh, 1, 2, buf);
  mqtt.publish(T_CTRL_PH, buf);

  mqtt.publish(T_SYS_AUTO, autoMode ? "1" : "0");
}

void processAutoMode() {
  if (!autoMode || emergencyStopActive) {
    return;
  }

  unsigned long elapsed = millis() - dosingStateStartTime;

  switch (autoState) {
    case DOSING_IDLE:
      if (ecValue < targetEc - EC_TOLERANCE) {
        autoState = DOSING_A;
        dosingStateStartTime = millis();
        pumpA_on = true;
        writeRelay(RELAY_PUMP_A, true);
        sendPumpState("pumpA", true);
      } else if (phValue > targetPh + PH_TOLERANCE) {
        autoState = DOSING_PH;
        dosingStateStartTime = millis();
        pumpPh_on = true;
        writeRelay(RELAY_PUMP_PH, true);
        sendPumpState("pumpPh", true);
      } else {
        // Safe defaults when resting
        if (pumpA_on || pumpB_on || pumpPh_on) {
          stopAllPumps(true);
          autoMode = true; // prevent stopAllPumps from breaking overall UI toggle expectation
        }
      }
      break;

    case DOSING_A:
      if (elapsed >= PUMP_DOSE_MS) {
        pumpA_on = false;
        writeRelay(RELAY_PUMP_A, false);
        sendPumpState("pumpA", false);
        autoState = DOSING_WAIT_B;
        dosingStateStartTime = millis();
      }
      break;

    case DOSING_WAIT_B:
      if (elapsed >= PUMP_B_DELAY) {
        pumpB_on = true;
        writeRelay(RELAY_PUMP_B, true);
        sendPumpState("pumpB", true);
        autoState = DOSING_B;
        dosingStateStartTime = millis();
      }
      break;

    case DOSING_B:
      if (elapsed >= PUMP_DOSE_MS) {
        pumpB_on = false;
        writeRelay(RELAY_PUMP_B, false);
        sendPumpState("pumpB", false);
        autoState = DOSING_MIX_EC;
        dosingStateStartTime = millis();
      }
      break;

    case DOSING_MIX_EC:
      if (elapsed >= PUMP_MIXING_MS) {
        autoState = DOSING_IDLE;
      }
      break;

    case DOSING_PH:
      if (elapsed >= PH_DOSE_MS) {
        pumpPh_on = false;
        writeRelay(RELAY_PUMP_PH, false);
        sendPumpState("pumpPh", false);
        autoState = DOSING_MIX_PH;
        dosingStateStartTime = millis();
      }
      break;

    case DOSING_MIX_PH:
      if (elapsed >= PH_MIXING_MS) {
        autoState = DOSING_IDLE;
      }
      break;
  }
}

void processDailySchedule() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10)) return;

  int hour = timeinfo.tm_hour;

  if (hour >= 8 && hour < 10) {
    if (!dailyScheduleActive) {
      dailyScheduleActive = true;
      dailyMixingDone = false;
      autoMode = true; 
      sendSensorData();
      Serial.println("[Schedule] 08:00 -> Phase 1: Mixing");
    }
    if (!dailyMixingDone) {
      if (autoMode && autoState == DOSING_IDLE && 
          fabs(ecValue - targetEc) <= EC_TOLERANCE && 
          fabs(phValue - targetPh) <= PH_TOLERANCE) {
        dailyMixingDone = true;
        setMainFlow(true, true);
        Serial.println("[Schedule] Mixed -> Phase 2: Flow to Plants");
      }
    }
  } else {
    if (dailyScheduleActive) {
      dailyScheduleActive = false;
      dailyMixingDone = false;
      autoMode = false;
      stopAllPumps(true);
      sendSensorData();
      Serial.println("[Schedule] 10:00 -> Phase 3: Idle");
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nTomato Hydroponics Controller - Refactored");

  EEPROM.begin(32);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  loadPhCalibration();
  loadEcCalibration();

  // Set HIGH before pinMode to prevent Active-Low relays from turning on instantly
  digitalWrite(RELAY_PUMP_A, RELAY_ACTIVE_HIGH ? LOW : HIGH);
  pinMode(RELAY_PUMP_A, OUTPUT);
  digitalWrite(RELAY_PUMP_B, RELAY_ACTIVE_HIGH ? LOW : HIGH);
  pinMode(RELAY_PUMP_B, OUTPUT);
  digitalWrite(RELAY_PUMP_PH, RELAY_ACTIVE_HIGH ? LOW : HIGH);
  pinMode(RELAY_PUMP_PH, OUTPUT);
  digitalWrite(RELAY_SOLENOID_1, RELAY_ACTIVE_HIGH ? LOW : HIGH);
  pinMode(RELAY_SOLENOID_1, OUTPUT);
  digitalWrite(RELAY_MAIN_PUMP, RELAY_ACTIVE_HIGH ? LOW : HIGH);
  pinMode(RELAY_MAIN_PUMP, OUTPUT);
  forceRelaysOff();

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  initButton(btnMain);
  initButton(btnStage);
  initButton(btnEstop);

  applyGrowthStage(0, false);

  u8g2.begin();
  u8g2.setPowerSave(0);
  u8g2.setDrawColor(1);
  u8g2.setFontDirection(0);
  updateLCD();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  espClient.setInsecure();
  espClient.setHandshakeTimeout(5);
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setCallback(mqttCallback);
}

void loop() {
  unsigned long now = millis();

  // High-priority hardware inputs
  handleHardwareButtons(now);

  // Non-blocking WiFi maintenance
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiLog >= WIFI_LOG_INTERVAL) {
      lastWifiLog = now;
      Serial.println("[WiFi] Disconnected");
    }
    if (now - lastWifiReconnectAttempt >= WIFI_RECONNECT_INTERVAL) {
      lastWifiReconnectAttempt = now;
      WiFi.reconnect();
    }
  }

  // Non-blocking MQTT maintenance
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) {
      if (now - lastMqttReconnectAttempt >= MQTT_RECONNECT_INTERVAL) {
        lastMqttReconnectAttempt = now;
        reconnectMQTT();
      }
    } else {
      mqtt.loop();
    }
  }

  // Periodic data task
  if (now - lastDataSend >= DATA_INTERVAL) {
    lastDataSend = now;
    updateSensors();
    sendSensorData();
  }

  processAutoMode(); // Call rapidly to evaluate state machine delays against millis()
  processDailySchedule(); // Check daily timing

  // Faster UI refresh
  if (now - lastLcdUpdate >= LCD_UPDATE_INTERVAL) {
    lastLcdUpdate = now;
    updateLCD();
  }
}
