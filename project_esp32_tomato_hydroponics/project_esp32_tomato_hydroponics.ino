/*
  🍅 Tomato Hydroponics Controller - ESP32
  EC/pH + Pump Control with Growth Stage Presets
  /อ่านก่อน ultrasonic เราก็ต้องแก้ระยะใหม่ด้วยเพราะตัดโครงออกไปส่วนนึง
  /lcd ยังไม่ได้เชื่อมเพราะว่าจะเทสระบบกับเว็บก่อน
  //ปุ่มยังไม่ได้ใส่หรือต่อเพราะว่ายังวางระบบไม่ได้ว่าจะบังคับอะไรบ้างแบบmanual
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <U8g2lib.h>          // LCD ST7920
//#include <esp_wifi.h>
//#include "esp_eap_client.h"  // WPA2-Enterprise (eduroam) — Core 3.x
//#include <WebServer.h>
//#include <WebSocketsServer.h> libraryเดิมที่เชื่อมกับ websocket

// ---- Sensor Pins ----
#define PH_PIN            35        // GPIO35 (ADC1_CH7) — pH sensor
#define EC_PIN            34        // GPIO34 (ADC1_CH6) — EC sensor (DFR0300)
#define SAMPLES           20        // ADC averaging samples

// ---- Ultrasonic HC-SR04 ----
#define TRIG_PIN          25        // GPIO25 — Trigger
#define ECHO_PIN          26        // GPIO26 — Echo (ใช้ HC-SR04P 3.3V!)
#define WATER_FULL_DIST   20.0    // cm จาก sensor → ผิวน้ำเมื่อเต็ม (100%)
#define WATER_EMPTY_DIST  42.0    // cm จาก sensor → ผิวน้ำเมื่อถังว่าง (0%)

// ---- Relays (Active-LOW, Wired NO) ----
#define RELAY_PUMP_A      13        // Fertilizer A
#define RELAY_PUMP_B      12        // Fertilizer B
#define RELAY_MAIN_PUMP   14        // pH Down / Main Pump

// ---- LCD ST7920 128x64 (SPI) ----
// Wiring: VCC=Vin(5V), BLA=3.3V, BLK=GND, R6=GND
#define LCD_CLK           18        // GPIO18 — E (SCLK)
#define LCD_DATA          23        // GPIO23 — R/W (SID)
#define LCD_CS            5         // GPIO5  — RS (CS)
#define LCD_RST           22        // GPIO22 — RST

U8G2_ST7920_128X64_F_SW_SPI u8g2(U8G2_R0, LCD_CLK, LCD_DATA, LCD_CS, LCD_RST);

// ---- Auto Dosing Button (momentary push, wired to GND) ----
#define BTN_AUTO_PIN      32        // GPIO32 — INPUT_PULLUP
#define BTN_DEBOUNCE_MS   50        // debounce time (ms)

// ---- pH EEPROM (address 0x00-0x08) ----
#define PH_ADDR_SLOPE     0
#define PH_ADDR_INTERCEPT 4
#define PH_ADDR_VALID     8
#define PH_VALID_MAGIC    0xAB

// ---- EC EEPROM (address 0x0A-0x12) ----
#define EC_KVALUEADDR     0x0A
#define EC_ADDR_VALID     0x12
#define EC_VALID_MAGIC    0xEC
#define RES2              820.0
#define ECREF             200.0
#define VREF              3300.0    // 3.3V = 3300mV
#define ADC_RES           4095.0    // ESP32 12-bit
#define EC_DEFAULT_TEMP   25.0

// ---- WiFi ----
const char* WIFI_SSID = "Yumgaizap";
const char* WIFI_PASS = "0625321533";

// ---- MQTT (HiveMQ Cloud Private Broker) ----
const char* MQTT_SERVER = "8218cf51f5de4ac5a776aa0efb931888.s1.eu.hivemq.cloud";
const int MQTT_PORT = 8883;
const char* MQTT_USER = "tomato-esp32";
const char* MQTT_PASS = "tomato_project_Y3";

// ---- MQTT Topics (per-sensor cloud architecture) ----
// Publish (ESP32 → Cloud)
const char* T_SENSOR_EC    = "hydroponics/sensor/ec";
const char* T_SENSOR_PH    = "hydroponics/sensor/ph";
const char* T_SENSOR_WATER = "hydroponics/sensor/water";
const char* T_PUMP_A       = "hydroponics/pump/a";
const char* T_PUMP_B       = "hydroponics/pump/b";
const char* T_PUMP_PH      = "hydroponics/pump/ph";
const char* T_PUMP_MAIN    = "hydroponics/pump/main";
const char* T_SYS_STATUS   = "hydroponics/system/status";
const char* T_SYS_AUTO     = "hydroponics/system/auto";
// Subscribe (Cloud → ESP32)
const char* T_CTRL_EC      = "hydroponics/control/ec_target";
const char* T_CTRL_PH      = "hydroponics/control/ph_target";
const char* T_SYS_EMERGENCY = "hydroponics/system/emergency";
const char* T_CTRL_CALIBRATE = "hydroponics/control/calibrate";

WiFiClientSecure espClient;
PubSubClient mqtt(espClient);

bool pumpA_on = false, pumpB_on = false, mainPump_on = false;
float targetEc = 2.0, targetPh = 6.2, ecValue = 0.0, phValue = 0.0;
float waterLevel = 0.0;   // ระดับน้ำ (%)
unsigned long lastDataSend = 0;
const unsigned long DATA_INTERVAL = 2000;

// ---- Non-blocking Pump B delay ----
bool waitingForPumpB = false;
unsigned long pumpBStartTime = 0;
const unsigned long PUMP_B_DELAY = 2000; // 2 seconds

// ---- pH Calibration ----
float phSlope     = -0.004593f;   // default: -5.70 * (3.3/4095)
float phIntercept = 21.00f;

// ---- EC Calibration ----
float ecKvalueLow  = 1.0f;
float ecKvalueHigh = 1.0f;
float ecKvalue     = 1.0f;
float ecTemperature = EC_DEFAULT_TEMP;

// ---- WiFi / MQTT reconnect timing ----
const unsigned long WIFI_CONNECT_TIMEOUT = 10000;      // ms
const unsigned long MQTT_RECONNECT_INTERVAL = 3000;    // ms
unsigned long lastMqttReconnectAttempt = 0;

// ---- Auto dosing state ----
bool autoMode = false;
unsigned long ecStableSince = 0;

// ---- Button debounce state ----
bool btnLastState = HIGH;           // INPUT_PULLUP → idle = HIGH
bool btnStableState = HIGH;
unsigned long btnLastDebounceTime = 0;
const float EC_TOLERANCE = 0.05f;
const unsigned long EC_STABLE_TIME = 10000; // ms

void sendSensorData();
void sendPumpState(const char* pump, bool state);
void handleCalibration(char* cmd);
void reconnectMQTT();
void stopAllPumps();

// ============================================================
//  EEPROM: Load Calibration Values
// ============================================================
void loadPhCalibration() {
    if (EEPROM.read(PH_ADDR_VALID) == PH_VALID_MAGIC) {
        EEPROM.get(PH_ADDR_SLOPE, phSlope);
        EEPROM.get(PH_ADDR_INTERCEPT, phIntercept);
        Serial.printf("🍅 pH calibration loaded: slope=%.6f intercept=%.4f\n", phSlope, phIntercept);
    } else {
        Serial.println("🍅 pH: no calibration found, using defaults");
    }
}

void loadEcCalibration() {
    if (EEPROM.read(EC_ADDR_VALID) == EC_VALID_MAGIC) {
        EEPROM.get(EC_KVALUEADDR, ecKvalueLow);
        EEPROM.get(EC_KVALUEADDR + 4, ecKvalueHigh);
        if (ecKvalueLow < 0.5 || ecKvalueLow > 1.5 || ecKvalueHigh < 0.5 || ecKvalueHigh > 1.5) {
            ecKvalueLow = 1.0; ecKvalueHigh = 1.0;
            Serial.println("🍅 EC: calibration out of range, reset to defaults");
        } else {
            Serial.printf("🍅 EC calibration loaded: Klow=%.4f Khigh=%.4f\n", ecKvalueLow, ecKvalueHigh);
        }
        ecKvalue = ecKvalueLow;
    } else {
        Serial.println("🍅 EC: no calibration found, using defaults (K=1.0)");
    }
}

// ============================================================
//  Sensor Reading Functions
// ============================================================
float readPH() {
    long sum = 0;
    for (int i = 0; i < SAMPLES; i++) {
        sum += analogRead(PH_PIN);
        delay(5);
    }
    float avg = (float)sum / SAMPLES;
    float ph = phSlope * avg + phIntercept;
    return constrain(ph, 0.0f, 14.0f);
}

float readEC() {
    long sum = 0;
    for (int i = 0; i < SAMPLES; i++) {
        sum += analogRead(EC_PIN);
        delay(5);
    }
    float avg = (float)sum / SAMPLES;
    float voltage = avg / ADC_RES * VREF;
    float rawEC = 1000.0 * voltage / RES2 / ECREF;

    // Auto-select K value based on EC range
    if (rawEC * ecKvalue > 2.5)      ecKvalue = ecKvalueHigh;
    else if (rawEC * ecKvalue < 2.0)  ecKvalue = ecKvalueLow;

    float ec = rawEC * ecKvalue;
    // Temperature compensation
    ec = ec / (1.0 + 0.0185 * (ecTemperature - 25.0));
    return max(ec, 0.0f);
}

float readWaterLevelOnce() {
    // ส่ง pulse 10μs
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(5);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    // วัดเวลาที่ ECHO เป็น HIGH (timeout 50ms ≈ ~850cm)
    long duration = pulseIn(ECHO_PIN, HIGH, 50000);
    if (duration == 0) return -1.0;  // no echo

    float distance = duration * 0.034 / 2.0;  // cm
    return distance;
}

float readWaterLevel() {
    // Try 3 times, take first valid reading
    float distance = -1.0;
    for (int attempt = 0; attempt < 3; attempt++) {
        distance = readWaterLevelOnce();
        if (distance > 0) break;
        delay(60);  // wait between retries (sensor needs ~60ms between pings)
    }

    if (distance < 0) {
        Serial.println("🍅 Water: TIMEOUT x3 (no echo! check wiring TRIG=GPIO25 ECHO=GPIO26)");
        return waterLevel;  // ใช้ค่าเดิม
    }

    // 10cm = 100%, ยิ่งระยะห่างมาก → น้ำยิ่งน้อย (ลดลงเป็นสัดส่วน)
    float level = (WATER_EMPTY_DIST - distance) / (WATER_EMPTY_DIST - WATER_FULL_DIST) * 100.0;
    level = constrain(level, 0.0f, 100.0f);
    Serial.printf("🍅 Water: dist=%.1fcm level=%.0f%%\n", distance, level);
    return level;
}

void updateSensors() {
    phValue = readPH();
    ecValue = readEC();
    waterLevel = readWaterLevel();
}

// ============================================================
//  LCD: แสดง EC, pH, Water Level, Cloud Status, Target EC/pH
// ============================================================
void updateLCD() {
    u8g2.clearBuffer();

    // ---- Title bar ----
    u8g2.setFont(u8g2_font_7x14B_tr);
    u8g2.drawStr(2, 12, "TOMATO HYDRO");
    u8g2.drawHLine(0, 14, 128);

    // ---- Sensor values + Targets ----
    u8g2.setFont(u8g2_font_6x12_tr);
    char buf[24];

    snprintf(buf, sizeof(buf), "EC:%.2f  T:%.1f", ecValue, targetEc);
    u8g2.drawStr(2, 26, buf);

    snprintf(buf, sizeof(buf), "pH:%.2f  T:%.1f", phValue, targetPh);
    u8g2.drawStr(2, 38, buf);

    // ---- Water level + bar ----
    int wl = (int)waterLevel;
    snprintf(buf, sizeof(buf), "H2O:%d%%", wl);
    u8g2.drawStr(2, 50, buf);

    // Progress bar (right side, same row as H2O)
    u8g2.drawFrame(56, 42, 68, 8);
    int barW = (int)(66.0 * waterLevel / 100.0);
    if (barW > 0) u8g2.drawBox(57, 43, barW, 6);

    // ---- Cloud Status ----
    u8g2.drawHLine(0, 53, 128);
    u8g2.setFont(u8g2_font_5x7_tr);
    if (mqtt.connected()) {
        u8g2.drawStr(2, 63, "CLOUD: CONNECTED");
    } else if (WiFi.status() == WL_CONNECTED) {
        u8g2.drawStr(2, 63, "WiFi OK | MQTT: ...");
    } else {
        u8g2.drawStr(2, 63, "OFFLINE");
    }

    u8g2.sendBuffer();
}

// ============================================================
//  MQTT: Per-Topic Publish & Subscribe
// ============================================================
void sendPumpState(const char* pump, bool state) {
    if (!mqtt.connected()) return;
    // Map pump name to topic
    const char* topic = NULL;
    if (strcmp(pump, "pumpA") == 0)       topic = T_PUMP_A;
    else if (strcmp(pump, "pumpB") == 0)  topic = T_PUMP_B;
    else if (strcmp(pump, "pumpPh") == 0) topic = T_PUMP_PH;
    else if (strcmp(pump, "mainPump") == 0) { topic = T_PUMP_MAIN; mqtt.publish(T_PUMP_PH, state ? "1" : "0"); }
    if (topic) mqtt.publish(topic, state ? "1" : "0");
    Serial.printf("🍅 %s: %s\n", pump, state ? "ON" : "OFF");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    char msg[length + 1];
    for (unsigned int i = 0; i < length; i++) msg[i] = (char)payload[i];
    msg[length] = '\0';
    Serial.printf("🍅 [%s]: %s\n", topic, msg);

    // ---- Pump control topics (value: "1" or "0") ----
    if (strcmp(topic, T_PUMP_A) == 0) {
        bool on = (msg[0] == '1');
        if (on && ecValue >= targetEc) { Serial.println("🍅 Pump A blocked: EC >= target"); return; }
        pumpA_on = on;
        digitalWrite(RELAY_PUMP_A, on ? HIGH : LOW);
        sendPumpState("pumpA", on);
    }
    else if (strcmp(topic, T_PUMP_B) == 0) {
        bool on = (msg[0] == '1');
        if (on && ecValue >= targetEc) { Serial.println("🍅 Pump B blocked: EC >= target"); return; }
        pumpB_on = on;
        digitalWrite(RELAY_PUMP_B, on ? HIGH : LOW);
        sendPumpState("pumpB", on);
    }
    else if (strcmp(topic, T_PUMP_MAIN) == 0 || strcmp(topic, T_PUMP_PH) == 0) {
        bool on = (msg[0] == '1');
        if (on && phValue <= targetPh) { Serial.println("🍅 Main pump blocked: pH <= target"); return; }
        mainPump_on = on;
        digitalWrite(RELAY_MAIN_PUMP, on ? HIGH : LOW);
        sendPumpState("mainPump", on);
    }
    // ---- Target setpoints ----
    else if (strcmp(topic, T_CTRL_EC) == 0) {
        targetEc = atof(msg);
        Serial.printf("🍅 Target EC set to: %.2f\n", targetEc);
    }
    else if (strcmp(topic, T_CTRL_PH) == 0) {
        targetPh = atof(msg);
        Serial.printf("🍅 Target pH set to: %.2f\n", targetPh);
    }
    // ---- System: Auto mode ----
    else if (strcmp(topic, T_SYS_AUTO) == 0) {
        bool on = (msg[0] == '1');
        autoMode = on;
        ecStableSince = 0;
        Serial.printf("🍅 Auto mode: %s\n", on ? "ON" : "OFF");
        if (!on) stopAllPumps();
    }
    // ---- System: Emergency stop ----
    else if (strcmp(topic, T_SYS_EMERGENCY) == 0) {
        Serial.println("🍅 EMERGENCY STOP!");
        autoMode = false;
        stopAllPumps();
        mqtt.publish(T_SYS_AUTO, "0");
    }
    // ---- Calibration commands ----
    else if (strcmp(topic, T_CTRL_CALIBRATE) == 0) {
        handleCalibration(msg);
    }
}

void handleCalibration(char* cmd) {
    // Placeholder: extend with your calibration logic
    Serial.printf("🍅 Calibration command: %s\n", cmd);
}

void reconnectMQTT() {
    Serial.print("🍅 Attempting MQTT connection...");
    String clientId = "ESP32Client-Tomato-";
    clientId += String(random(0xffff), HEX);

    if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS, T_SYS_STATUS, 1, true, "offline")) {
        Serial.println("connected to HiveMQ Cloud!");
        mqtt.publish(T_SYS_STATUS, "online", true);

        // Subscribe to all control & pump topics
        mqtt.subscribe(T_PUMP_A);
        mqtt.subscribe(T_PUMP_B);
        mqtt.subscribe(T_PUMP_PH);
        mqtt.subscribe(T_PUMP_MAIN);
        mqtt.subscribe(T_CTRL_EC);
        mqtt.subscribe(T_CTRL_PH);
        mqtt.subscribe(T_SYS_AUTO);
        mqtt.subscribe(T_SYS_EMERGENCY);
        mqtt.subscribe(T_CTRL_CALIBRATE);
        Serial.println("🍅 Subscribed to all control topics");
    } else {
        int rc = mqtt.state();
        Serial.printf("failed, rc=%d ", rc);
        if (rc == -2) Serial.println("(TLS/TCP failed)");
        else if (rc == -4) Serial.println("(timeout)");
        else if (rc == 4) Serial.println("(bad credentials)");
        else if (rc == 5) Serial.println("(unauthorized)");
        else Serial.println("(will retry)");
    }
}

void sendSensorData() {
    if (!mqtt.connected()) return;
    // Publish each sensor to its own topic
    char buf[16];
    dtostrf(ecValue, 1, 2, buf);
    mqtt.publish(T_SENSOR_EC, buf);

    dtostrf(phValue, 1, 2, buf);
    mqtt.publish(T_SENSOR_PH, buf);

    dtostrf(waterLevel, 1, 1, buf);
    mqtt.publish(T_SENSOR_WATER, buf);

    // Publish current targets + auto mode state
    dtostrf(targetEc, 1, 2, buf);
    mqtt.publish(T_CTRL_EC, buf);

    dtostrf(targetPh, 1, 2, buf);
    mqtt.publish(T_CTRL_PH, buf);

    mqtt.publish(T_SYS_AUTO, autoMode ? "1" : "0");
}

// ============================================================
//  SETUP & LOOP
// ============================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n🍅 Tomato Hydroponics Controller - CLOUD EDITION");

    // ---- EEPROM + ADC init ----
    EEPROM.begin(32);
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    loadPhCalibration();
    loadEcCalibration();
    
    // ---- Relays Init (Active-HIGH) ----
    digitalWrite(RELAY_PUMP_A, LOW);
    pinMode(RELAY_PUMP_A, OUTPUT);
    digitalWrite(RELAY_PUMP_B, LOW);
    pinMode(RELAY_PUMP_B, OUTPUT);
    digitalWrite(RELAY_MAIN_PUMP, LOW);
    pinMode(RELAY_MAIN_PUMP, OUTPUT);

    // ---- Ultrasonic HC-SR04 ----
    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);

    // ---- Auto Dosing Button ----
    pinMode(BTN_AUTO_PIN, INPUT_PULLUP);

    // ---- Ultrasonic self-test (before WiFi) ----
    Serial.println("🍅 Ultrasonic test (3 reads before WiFi):");
    for (int i = 0; i < 3; i++) {
        float d = readWaterLevelOnce();
        if (d > 0) Serial.printf("  [%d] dist=%.1f cm  OK\n", i+1, d);
        else       Serial.printf("  [%d] NO ECHO - check wiring!\n", i+1);
        delay(100);
    }

    // ---- LCD init (matched from working demo) ----
    u8g2.begin();
    u8g2.setPowerSave(0);       // ensure display is ON
    delay(150);                 // ST7920 needs time after power-on
    u8g2.setDrawColor(1);       // foreground color
    u8g2.setFontDirection(0);   // left-to-right
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_7x14B_tr);
    u8g2.drawStr(10, 35, "TOMATO CLOUD");
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(20, 50, "Connecting Wi-Fi");
    u8g2.sendBuffer();

    // ---- WiFi ----
    WiFi.disconnect(true);
    delay(300);
    WiFi.mode(WIFI_STA);
    Serial.printf("🍅 Connecting to %s ", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < WIFI_CONNECT_TIMEOUT) {
        delay(200);
        Serial.print(".");
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\n🍅 WiFi connect failed (timeout). Restarting...");
        delay(1000);
        ESP.restart();
    }

    Serial.println(" OK!");
    Serial.printf("🍅 IP: %s\n", WiFi.localIP().toString().c_str());

    // ---- MQTT Init ----
    espClient.setInsecure();                // skip cert verification
    espClient.setHandshakeTimeout(5);       // 5 sec TLS timeout
    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setBufferSize(512);                // default 256 is too small
    mqtt.setCallback(mqttCallback);

    Serial.println("🍅 Ready to connect to HiveMQ Cloud!");
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        // Handle WiFi Disconnect
        Serial.println("WiFi Disconnected. Waiting for reconnection...");
        delay(1000);
        return;
    }

    if (!mqtt.connected()) {
        if (millis() - lastMqttReconnectAttempt > MQTT_RECONNECT_INTERVAL) {
            lastMqttReconnectAttempt = millis();
            reconnectMQTT();
        }
    } else {
        mqtt.loop();
    }

    // เช็ค Timer เปิดปั๊ม B (ไม่ Block)
    if (waitingForPumpB && millis() - pumpBStartTime >= PUMP_B_DELAY) {
        waitingForPumpB = false;
        pumpB_on = true;
        digitalWrite(RELAY_PUMP_B, HIGH);
        sendPumpState("pumpB", true);
        Serial.println("🍅 Pump B started (after 2s delay)");
    }
    
    // ---- Debounced button read ----
    bool btnReading = digitalRead(BTN_AUTO_PIN);
    if (btnReading != btnLastState) {
        btnLastDebounceTime = millis();
    }
    btnLastState = btnReading;

    if ((millis() - btnLastDebounceTime) > BTN_DEBOUNCE_MS) {
        if (btnReading != btnStableState) {
            btnStableState = btnReading;
            // Trigger on press (HIGH→LOW because INPUT_PULLUP)
            if (btnStableState == LOW) {
                autoMode = !autoMode;
                Serial.printf("\xF0\x9F\x8D\x85 Button: autoMode=%s\n", autoMode ? "ON" : "OFF");
                if (!autoMode) {
                    stopAllPumps();
                }
                ecStableSince = 0;
                // Notify web UI immediately
                sendSensorData();
            }
        }
    }

    if (millis() - lastDataSend >= DATA_INTERVAL) {
        lastDataSend = millis();
        updateSensors();
        processAutoMode();
        sendSensorData();
        updateLCD();
    }
}

void stopAllPumps() {
    pumpA_on = false;
    pumpB_on = false;
    mainPump_on = false;
    waitingForPumpB = false;

    digitalWrite(RELAY_PUMP_A, LOW);
    digitalWrite(RELAY_PUMP_B, LOW);
    digitalWrite(RELAY_MAIN_PUMP, LOW);

    sendPumpState("pumpA", false);
    sendPumpState("pumpB", false);
    sendPumpState("mainPump", false);
}

void processAutoMode() {
    if (!autoMode) return;

    // ---- EC dosing (Pump A/B) ----
    if (ecValue < targetEc - EC_TOLERANCE) {
        // เปิด pump A ถ้ายังปิด
        if (!pumpA_on) {
            pumpA_on = true;
            digitalWrite(RELAY_PUMP_A, HIGH);
            sendPumpState("pumpA", true);

            // เริ่มนับ 2 วิ สำหรับ Pump B
            waitingForPumpB = true;
            pumpBStartTime = millis();
        }
    } else {
        // EC ถึงหรือเกินเป้า -> ปิด pumps A/B
        if (pumpA_on || pumpB_on) {
            pumpA_on = false;
            pumpB_on = false;
            waitingForPumpB = false;
            digitalWrite(RELAY_PUMP_A, LOW);
            digitalWrite(RELAY_PUMP_B, LOW);
            sendPumpState("pumpA", false);
            sendPumpState("pumpB", false);
        }
    }

    // ---- EC stable ตรวจสอบ 10 วินาที ----
    if (fabs(ecValue - targetEc) <= EC_TOLERANCE) {
        if (ecStableSince == 0) ecStableSince = millis();
    } else {
        ecStableSince = 0;
    }

    // ---- เมื่อ EC เสถียร 10 วิ ให้ควบคุม pH (main pump) ----
    if (ecStableSince != 0 && millis() - ecStableSince >= EC_STABLE_TIME) {
        if (phValue > targetPh + 0.05f) {
            if (!mainPump_on) {
                mainPump_on = true;
                digitalWrite(RELAY_MAIN_PUMP, HIGH);
                sendPumpState("mainPump", true);
            }
        } else {
            if (mainPump_on) {
                mainPump_on = false;
                digitalWrite(RELAY_MAIN_PUMP, LOW);
                sendPumpState("mainPump", false);
            }
        }
    }
}

