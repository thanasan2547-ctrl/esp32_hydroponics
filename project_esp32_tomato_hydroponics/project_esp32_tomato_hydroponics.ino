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
#define PH_PIN            34
#define EC_PIN            35
#define SAMPLES           20

// ---- Ultrasonic HC-SR04 ----
#define TRIG_PIN          25
#define ECHO_PIN          33
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
#define PH_VALID_MAGIC    0xAC // Changed from 0xAB to force EEPROM reset after new math

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
//const char* WIFI_SSID = "Pew";
//const char* WIFI_PASS = "88888888";
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
const char* T_PUMP_SOLENOID = "hydroponics/pump/solenoid1";
const char* T_SYS_STATUS    = "hydroponics/system/status";
const char* T_SYS_AUTO      = "hydroponics/system/auto";
const char* T_SYS_STATE     = "hydroponics/system/state";

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

// Track the last output state written to the relays to avoid repeated toggles
bool relayPumpA_out = false;
bool relayPumpB_out = false;
bool relayPumpPh_out = false;
bool relayMainPump_out = false;
bool relaySolenoid1_out = false;

float targetEc = 2.0f;
float targetPh = 6.2f;

// Keep a small sliding window of recent readings to stabilize sensor noise.
// This smooths values used for auto dosing and MQTT reports.
const int SENSOR_SMOOTH_SAMPLES = 10;
float ecSamples[SENSOR_SMOOTH_SAMPLES] = {0};
float phSamples[SENSOR_SMOOTH_SAMPLES] = {0};
int ecSampleIndex = 0;
int phSampleIndex = 0;
int ecSampleCount = 0;
int phSampleCount = 0;

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
// When a user toggles a pump manually (button or MQTT), we suspend automatic dosing until
// auto mode is explicitly re-enabled. This prevents auto logic from fighting manual input.
bool manualOverride = false;
bool webEmergencyStop = false; // Persistent emergency stop from web

const float EC_TOLERANCE = 0.05f;
const float PH_TOLERANCE = 0.05f;

enum AutoState { AUTO_CHECK, DOSING_A, DOSING_WAIT_B, DOSING_B, DOSING_MIX_EC, DOSING_PH, DOSING_MIX_PH, FEEDING_PLANTS, DRAINING_WATER };
AutoState autoState = AUTO_CHECK;
unsigned long dosingStateStartTime = 0;
unsigned long dosingAStartTime = 0;
unsigned long dosingBStartTime = 0;

const unsigned long PUMP_A_DOSE_MS = 1000;      // 1s
const unsigned long PUMP_B_DELAY_MS = 1000;     // 1s wait
const unsigned long PUMP_B_DOSE_MS = 1000;      // 1s
const unsigned long PUMP_MIX_EC_MS = 7000;      // 7s mix/wait before next check
const unsigned long PH_DOSE_MS = 1500;          // Dose pH for 1.5s
const unsigned long PH_MIXING_MS = 20000;       // Mix pH for 20s
const float WATER_TARGET_PCT = 80.0f;

// ---- Emergency ----
bool emergencyStopActive = false;

// ---- Calibration ----
// We use Explicit Voltage-Based 2-Point Interpolation
float phVoltage7 = 2.0f; // Typical DFRobot neutral voltage
float phVoltage4 = 1.419f; // Calculated from your ADC 1761 at pH 4 buffer
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
float getRawADC(int pin);

int getCurrentHour() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return -1; // Time not set
  }
  return timeinfo.tm_hour;
}

void writeRelay(uint8_t relayPin, bool on) {
  if (RELAY_ACTIVE_HIGH) {
    digitalWrite(relayPin, on ? HIGH : LOW);
  } else {
    digitalWrite(relayPin, on ? LOW : HIGH);
  }
}

void publishSystemState() {
  if (!mqtt.connected()) return;

  char buf[128];
  int len = snprintf(
    buf, sizeof(buf),
    "{\"pumpA\":%d,\"pumpB\":%d,\"pumpPh\":%d,\"mainPump\":%d,\"solenoid1\":%d,\"auto\":%d,\"emergency\":%d,\"hw_emergency\":%d,\"web_emergency\":%d}",
    pumpA_on ? 1 : 0,
    pumpB_on ? 1 : 0,
    pumpPh_on ? 1 : 0,
    mainPump_on ? 1 : 0,
    solenoid1_on ? 1 : 0,
    autoMode ? 1 : 0,
    (emergencyStopActive || webEmergencyStop) ? 1 : 0,
    emergencyStopActive ? 1 : 0,
    webEmergencyStop ? 1 : 0
  );

  if (len > 0) {
    mqtt.publish(T_SYS_STATE, buf, true);
  }
}

void applyRelayStates(bool publishState = true) {
  if (emergencyStopActive || webEmergencyStop) {
    // Emergency stop overrides all other control paths
    if (relayPumpA_out || relayPumpB_out || relayPumpPh_out || relayMainPump_out || relaySolenoid1_out) {
      writeRelay(RELAY_PUMP_A, false);
      writeRelay(RELAY_PUMP_B, false);
      writeRelay(RELAY_PUMP_PH, false);
      writeRelay(RELAY_MAIN_PUMP, false);
      writeRelay(RELAY_SOLENOID_1, false);
      relayPumpA_out = relayPumpB_out = relayPumpPh_out = relayMainPump_out = relaySolenoid1_out = false;
      if (publishState) {
        sendPumpState("pumpA", false);
        sendPumpState("pumpB", false);
        sendPumpState("pumpPh", false);
        sendPumpState("mainPump", false);
        publishSystemState();
      }
    }
    return;
  }

  bool stateChanged = false;

  if (pumpA_on != relayPumpA_out) {
    writeRelay(RELAY_PUMP_A, pumpA_on);
    relayPumpA_out = pumpA_on;
    stateChanged = true;
    if (publishState) sendPumpState("pumpA", pumpA_on);
  }
  if (pumpB_on != relayPumpB_out) {
    writeRelay(RELAY_PUMP_B, pumpB_on);
    relayPumpB_out = pumpB_on;
    stateChanged = true;
    if (publishState) sendPumpState("pumpB", pumpB_on);
  }
  if (pumpPh_on != relayPumpPh_out) {
    writeRelay(RELAY_PUMP_PH, pumpPh_on);
    relayPumpPh_out = pumpPh_on;
    stateChanged = true;
    if (publishState) sendPumpState("pumpPh", pumpPh_on);
  }
  if (mainPump_on != relayMainPump_out) {
    writeRelay(RELAY_MAIN_PUMP, mainPump_on);
    relayMainPump_out = mainPump_on;
    stateChanged = true;
    if (publishState) sendPumpState("mainPump", mainPump_on);
  }
  if (solenoid1_on != relaySolenoid1_out) {
    writeRelay(RELAY_SOLENOID_1, solenoid1_on);
    relaySolenoid1_out = solenoid1_on;
    stateChanged = true;
    // solenoid state is tied to mainPump, so no topic publish here
  }

  if (publishState && stateChanged) {
    publishSystemState();
  }
}

void forceRelaysOff() {
  // Force all relays off immediately (used during startup or in emergency conditions)
  writeRelay(RELAY_PUMP_A, false);
  writeRelay(RELAY_PUMP_B, false);
  writeRelay(RELAY_PUMP_PH, false);
  writeRelay(RELAY_MAIN_PUMP, false);
  writeRelay(RELAY_SOLENOID_1, false);

  relayPumpA_out = relayPumpB_out = relayPumpPh_out = relayMainPump_out = relaySolenoid1_out = false;
}

void setMainFlow(bool on, bool publishState = true) {
  mainPump_on = on;
  applyRelayStates(publishState);
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
    EEPROM.get(PH_ADDR_SLOPE, phVoltage7);
    EEPROM.get(PH_ADDR_INTERCEPT, phVoltage4);
    Serial.printf("[pH] Calibration loaded: v7=%.3fV v4=%.3fV\n", phVoltage7, phVoltage4);
  } else {
    Serial.println("[pH] No calibration found or format changed, using defaults");
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
  float avg = getRawADC(PH_PIN);
  float voltage = avg / ADC_RES * VREF / 1000.0f;
  
  // Explicit 2-point calculation (handles both positive and negative slope sensors correctly)
  float slope = (7.0f - 4.0f) / (phVoltage7 - phVoltage4);
  float intercept = 7.0f - slope * phVoltage7;
  
  float ph = slope * voltage + intercept;
  
  // Debug to Serial Monitor to verify ADC health
  Serial.printf("[pH Sensor] Voltage: %.3fV, Slope: %.2f, pH: %.2f\n", voltage, slope, ph);
  
  return constrain(ph, 0.0f, 14.0f);
}

float readEC() {
  // อ่าน 40 ครั้ง (ตาม reference snippet)
  int buf[40];
  for (int i = 0; i < 40; i++) {
    buf[i] = analogRead(EC_PIN);
    delay(10);
  }

  // Sort ascending
  for (int i = 0; i < 39; i++)
    for (int j = i + 1; j < 40; j++)
      if (buf[i] > buf[j]) { int t = buf[i]; buf[i] = buf[j]; buf[j] = t; }

  // ตัด outlier 25% บน-ล่าง ใช้ค่ากลาง 20 ตัว — คำนวณ mV บน 3.3V
  long sum = 0;
  for (int i = 10; i < 30; i++) sum += buf[i];
  float mV = (sum / 20.0f) / 4095.0f * 3300.0f;

  // probe ไม่ได้จุ่มน้ำ
  float v = mV / 1000.0f;
  if (v < 0.1f) {
    Serial.printf("[EC Sensor] mV: %.2f -> probe not in water\n", mV);
    return 0.0f;
  }

  // DFRobot source: rawEC = 1000 * mV / 820 / 200
  // บน 3.3V ค่าจะเกิน 10x เทียบกับ 5V → หาร 10 เพื่อแก้ scale
  float rawEC  = 1000.0f * mV / RES2 / ECREF;
  float ecComp = rawEC / (1.0f + 0.0185f * (ecTemperature - 25.0f));
  float ec     = (ecComp / 4.1f) * ecKvalue;  // 4.1 = empirical scale for this 3.3V sensor

  Serial.printf("[EC Sensor] mV: %.2f, EC: %.3f mS/cm\n", mV, ec);
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

static float computeAverage(const float* samples, int count) {
  if (count <= 0) return 0.0f;
  float sum = 0.0f;
  for (int i = 0; i < count; i++) {
    sum += samples[i];
  }
  return sum / count;
}

void updateSensors() {
  // Take raw readings, then smooth over a sliding window to reduce jitter.
  float newPh = readPH();
  float newEc = readEC();

  phSamples[phSampleIndex] = newPh;
  phSampleIndex = (phSampleIndex + 1) % SENSOR_SMOOTH_SAMPLES;
  if (phSampleCount < SENSOR_SMOOTH_SAMPLES) phSampleCount++;

  ecSamples[ecSampleIndex] = newEc;
  ecSampleIndex = (ecSampleIndex + 1) % SENSOR_SMOOTH_SAMPLES;
  if (ecSampleCount < SENSOR_SMOOTH_SAMPLES) ecSampleCount++;

  phValue = computeAverage(phSamples, phSampleCount);
  ecValue = computeAverage(ecSamples, ecSampleCount);

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
  autoState = AUTO_CHECK;

  applyRelayStates(publishState);
}

void handleHardwareButtons(unsigned long nowMs) {
  updateButton(btnMain, nowMs);
  updateButton(btnStage, nowMs);
  updateButton(btnEstop, nowMs);

  bool estopNow = (btnEstop.stableState == HIGH); // NC: High when pressed/open
  if (estopNow) {
    if (!emergencyStopActive) {
      emergencyStopActive = true;
      manualOverride = false;
      autoMode = false;
      stopAllPumps(true);
      if (mqtt.connected()) {
        mqtt.publish(T_SYS_AUTO, "0");
        mqtt.publish(T_SYS_STATUS, "hardware_estop", false);
      }
      publishSystemState();
      Serial.println("[E-STOP] Hardware ACTIVE - all relays OFF");
    } else {
      forceRelaysOff();
    }
    // REMOVED: return; so that loop() continues to call mqtt.loop()
  } else {
    if (emergencyStopActive) {
      emergencyStopActive = false;
      manualOverride = false;
      publishSystemState();
      Serial.println("[E-STOP] Hardware Released");
    }
  }

  if (btnMain.pressedEvent) {
    manualOverride = true;
    autoMode = false;
    bool next = !mainPump_on;
    setMainFlow(next, true);
    publishSystemState();
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
    bool active = (msg[0] == '1');
    webEmergencyStop = active;
    manualOverride = false;
    autoMode = false;
    stopAllPumps(true);
    mqtt.publish(T_SYS_AUTO, "0");
    publishSystemState();
    Serial.printf("[E-STOP] Web %s\n", active ? "ACTIVE" : "CLEARED");
    return;
  }

  if (emergencyStopActive || webEmergencyStop) {
    return;
  }

  if (strcmp(topic, T_PUMP_A) == 0) {
    bool on = (msg[0] == '1');
    if (pumpA_on != on) {
      manualOverride = true;
      autoMode = false;
      pumpA_on = on;
      applyRelayStates(true);
      sendSensorData(); // keep UI in sync
    }
  } else if (strcmp(topic, T_PUMP_B) == 0) {
    bool on = (msg[0] == '1');
    if (pumpB_on != on) {
      manualOverride = true;
      autoMode = false;
      pumpB_on = on;
      applyRelayStates(true);
      sendSensorData();
    }
  } else if (strcmp(topic, T_PUMP_PH) == 0) {
    bool on = (msg[0] == '1');
    if (pumpPh_on != on) {
      manualOverride = true;
      autoMode = false;
      pumpPh_on = on;
      applyRelayStates(true);
      sendSensorData();
    }
  } else if (strcmp(topic, T_PUMP_MAIN) == 0) {
    bool on = (msg[0] == '1');
    if (mainPump_on != on) {
      manualOverride = true;
      autoMode = false;
      mainPump_on = on; // Directly control mainPump_on
      applyRelayStates(true); // Apply changes
      sendSensorData();
    }
  } else if (strcmp(topic, T_PUMP_SOLENOID) == 0) {
    bool on = (msg[0] == '1');
    if (solenoid1_on != on) {
      manualOverride = true;
      autoMode = false;
      solenoid1_on = on;
      applyRelayStates(true);
      sendSensorData();
    }
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
      manualOverride = false;
      autoState = AUTO_CHECK;
      if (!autoMode) {
        stopAllPumps(true);
      }
      publishSystemState();
    }
  } else if (strcmp(topic, T_CTRL_CALIBRATE) == 0) {
    handleCalibration(msg);
  }
}

float getRawADC(int pin) {
  int buf[SAMPLES];
  for (int i = 0; i < SAMPLES; i++) {
    buf[i] = analogRead(pin);
  }
  
  // Median filter: Sort the array
  for (int i = 0; i < SAMPLES - 1; i++) {
    for (int j = i + 1; j < SAMPLES; j++) {
      if (buf[i] > buf[j]) {
        int temp = buf[i];
        buf[i] = buf[j];
        buf[j] = temp;
      }
    }
  }
  
  // Average the middle 50%
  long sum = 0;
  int count = 0;
  for (int i = SAMPLES / 4; i < SAMPLES - SAMPLES / 4; i++) {
    sum += buf[i];
    count++;
  }
  return (float)sum / count;
}

void handleCalibration(const char* cmd) {
  Serial.printf("[Calibrate] %s\n", cmd);
  
  if (strcmp(cmd, "ph_cal_7") == 0) {
    float avg = getRawADC(PH_PIN);
    phVoltage7 = avg / ADC_RES * VREF / 1000.0f;
    Serial.printf("pH 7 Calibrated. Voltage at pH 7: %.3fV\n", phVoltage7);
  } else if (strcmp(cmd, "ph_cal_4") == 0) {
    float avg = getRawADC(PH_PIN);
    phVoltage4 = avg / ADC_RES * VREF / 1000.0f;
    Serial.printf("pH 4 Calibrated. Voltage at pH 4: %.3fV\n", phVoltage4);
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
    EEPROM.put(PH_ADDR_SLOPE, phVoltage7);
    EEPROM.put(PH_ADDR_INTERCEPT, phVoltage4);
    EEPROM.write(PH_ADDR_VALID, PH_VALID_MAGIC);
    EEPROM.commit();
    Serial.println("pH Calibration Saved to EEPROM");
  } else if (strcmp(cmd, "ec_save") == 0) {
    EEPROM.put(EC_KVALUEADDR, ecKvalueLow);
    EEPROM.put(EC_KVALUEADDR + 4, ecKvalueHigh);
    EEPROM.write(EC_ADDR_VALID, EC_VALID_MAGIC);
    EEPROM.commit();
    Serial.println("EC Saved");
  }
}

// Assuming T_PUMP_SOLENOID is defined globally like other T_PUMP_ topics
// For example: const char* T_PUMP_SOLENOID = "hydro/pump/solenoid1";
// And setMainFlow is modified to only affect mainPump_on.
// Example setMainFlow:
// void setMainFlow(bool on, bool publishState) {
//   mainPump_on = on;
//   applyRelayStates(publishState);
//   sendSensorData();
// }

void reconnectMQTT() {
  String clientId = "ESP32Client-Tomato-";
  clientId += String(random(0xffff), HEX);

  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS, T_SYS_STATUS, 1, true, "offline")) {
    mqtt.publish(T_SYS_STATUS, "online", true);
    mqtt.subscribe(T_PUMP_A);
    mqtt.subscribe(T_PUMP_B);
    mqtt.subscribe(T_PUMP_PH);
    mqtt.subscribe(T_PUMP_MAIN);
    mqtt.subscribe(T_PUMP_SOLENOID);
    mqtt.subscribe(T_CTRL_EC);
    mqtt.subscribe(T_CTRL_PH);
    mqtt.subscribe(T_SYS_AUTO);
    mqtt.subscribe(T_SYS_EMERGENCY);
    mqtt.subscribe(T_CTRL_CALIBRATE);
    sendSensorData();
    publishSystemState();
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
  if (!autoMode || emergencyStopActive || webEmergencyStop || manualOverride) {
    return;
  }

  unsigned long now = millis();
  int currentHr = getCurrentHour();

  switch (autoState) {
    case AUTO_CHECK:
      // Check if we are in the 8-10 AM window OR if the user just enabled auto
      // (The system will stay in AUTO_CHECK until it reaches target)
      
      if (ecValue < targetEc - EC_TOLERANCE) {
        autoState = DOSING_A;
        dosingAStartTime = now;
        pumpA_on = true;
        pumpB_on = false;
        pumpPh_on = false;
        // mainPump off during dosing injection
        mainPump_on = false; 
        solenoid1_on = false;
        applyRelayStates(true);
        Serial.println("[Auto] EC low: Pump A ON (1s)");
      } else {
        // Target EC reached! 
        // Now turn on Main Pump to circulate (DFT system)
        autoState = FEEDING_PLANTS;
        dosingStateStartTime = now;
        pumpA_on = false;
        pumpB_on = false;
        pumpPh_on = false;
        solenoid1_on = false;
        mainPump_on = true;
        applyRelayStates(true);
        Serial.println("[Auto] Target EC reached. Main Pump ON for circulation.");
      }
      break;

    case DOSING_A:
      if (now - dosingAStartTime >= PUMP_A_DOSE_MS) {
        pumpA_on = false;
        applyRelayStates(true);
        autoState = DOSING_WAIT_B;
        dosingStateStartTime = now;
        Serial.println("[Auto] Pump A OFF, waiting 1s");
      }
      break;

    case DOSING_WAIT_B:
      if (now - dosingStateStartTime >= PUMP_B_DELAY_MS) {
        autoState = DOSING_B;
        dosingBStartTime = now;
        pumpB_on = true;
        applyRelayStates(true);
        Serial.println("[Auto] Pump B ON (1s)");
      }
      break;

    case DOSING_B:
      if (now - dosingBStartTime >= PUMP_B_DOSE_MS) {
        pumpB_on = false;
        applyRelayStates(true);
        autoState = DOSING_MIX_EC;
        dosingStateStartTime = now;
        Serial.println("[Auto] Pump B OFF, mixing 7s");
      }
      break;

    case DOSING_MIX_EC:
      if (now - dosingStateStartTime >= PUMP_MIX_EC_MS) {
        autoState = AUTO_CHECK; // Loop back to check EC again
      }
      break;

    case FEEDING_PLANTS:
      // Safety: Stop Main Pump if reservoir is empty (0-5%)
      if (mainPump_on && waterLevel <= 5.0f && waterLevel >= 0.0f) {
          Serial.println("[Auto] Reservoir empty (5%). Stopping Main Pump safety.");
          mainPump_on = false;
          applyRelayStates(true);
      }

      // Automatically end autoMode if it's past 10 AM or before 8 AM
      if (currentHr != -1 && (currentHr >= 10 || currentHr < 8)) {
          Serial.println("[Auto] Window ended or outside 8-10 AM. Stopping cycle.");
          autoMode = false;
          stopAllPumps(true);
      }
      break;

    case DRAINING_WATER:
      if (waterLevel >= WATER_TARGET_PCT || waterLevel < 0.0f) {
        solenoid1_on = false;
        applyRelayStates(true);
        autoState = AUTO_CHECK;
      }
      break;
    
    default:
      autoState = AUTO_CHECK;
      break;
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

  // Faster UI refresh
  if (now - lastLcdUpdate >= LCD_UPDATE_INTERVAL) {
    lastLcdUpdate = now;
    updateLCD();
  }
}
