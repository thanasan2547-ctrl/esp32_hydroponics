/*
  ESP32 Nutrient Control System (Non-Blocking)
  Hardware:
  - 4-Channel Relay: GPIO 13, 12, 14, 26
  - 1-Channel Relay: GPIO 27
  - NO Buttons: GPIO 16 (Start), GPIO 17 (Stage Toggle)
  - NC Button: GPIO 4 (Emergency Kill, fail-safe)

  Notes:
  - Buttons use INPUT_PULLUP.
  - NO button press: HIGH -> LOW.
  - NC emergency (wired to GND through NC contact):
    normal = LOW, pressed/broken wire = HIGH (emergency active).
*/

#include <Arduino.h>

// ---------------- Pin Mapping ----------------
static const uint8_t RELAY_NUTRIENT_A = 13;  // 4CH relay
static const uint8_t RELAY_NUTRIENT_B = 12;  // 4CH relay
static const uint8_t RELAY_PH_DOWN    = 14;  // 4CH relay
static const uint8_t RELAY_SOLENOID_1 = 26;  // 4CH relay
static const uint8_t RELAY_MAIN_PUMP  = 27;  // 1CH relay

static const uint8_t BTN_START_PIN    = 16;  // NO
static const uint8_t BTN_STAGE_PIN    = 17;  // NO
static const uint8_t BTN_ESTOP_PIN    = 4;   // NC

// Relay modules are often active LOW. Change to false if your board is active HIGH.
static const bool RELAY_ACTIVE_LOW = true;

// ---------------- Timing ----------------
static const unsigned long DEBOUNCE_MS = 35;
static const unsigned long STATUS_PRINT_MS = 1000;

// ---------------- Growth Stages ----------------
struct GrowthStage {
  const char* name;
  float ecTarget;
  float phTarget;
};

GrowthStage stages[] = {
  {"Seedling",   1.2f, 6.2f},
  {"Vegetative", 1.8f, 6.0f},
  {"Flowering",  2.3f, 5.8f},
  {"Ripening",   2.0f, 6.1f}
};

static const uint8_t STAGE_COUNT = sizeof(stages) / sizeof(stages[0]);
uint8_t currentStageIndex = 0;
float targetEC = stages[0].ecTarget;
float targetPH = stages[0].phTarget;

// ---------------- State ----------------
bool circulationEnabled = false;
bool emergencyActive = false;
unsigned long lastStatusPrint = 0;

struct DebouncedButton {
  uint8_t pin;
  bool stableState;
  bool lastReading;
  unsigned long lastChangeMs;
  bool pressedEvent;
};

DebouncedButton btnStart{BTN_START_PIN, HIGH, HIGH, 0, false};
DebouncedButton btnStage{BTN_STAGE_PIN, HIGH, HIGH, 0, false};
DebouncedButton btnEstop{BTN_ESTOP_PIN, HIGH, HIGH, 0, false};

void relayWrite(uint8_t pin, bool on) {
  if (RELAY_ACTIVE_LOW) {
    digitalWrite(pin, on ? LOW : HIGH);
  } else {
    digitalWrite(pin, on ? HIGH : LOW);
  }
}

void stopAllPumps() {
  relayWrite(RELAY_SOLENOID_1, false);
  relayWrite(RELAY_NUTRIENT_A, false);
  relayWrite(RELAY_NUTRIENT_B, false);
  relayWrite(RELAY_PH_PUMP, false);
  relayWrite(RELAY_MAIN_PUMP, false);
}

void applyCirculationOutputs() {
  relayWrite(RELAY_MAIN_PUMP, circulationEnabled);
  relayWrite(RELAY_SOLENOID_1, circulationEnabled);
}

void applyStage(uint8_t idx) {
  currentStageIndex = idx % STAGE_COUNT;
  targetEC = stages[currentStageIndex].ecTarget;
  targetPH = stages[currentStageIndex].phTarget;
}

void updateButton(DebouncedButton& b, bool activeLow, unsigned long nowMs) {
  b.pressedEvent = false;
  bool reading = digitalRead(b.pin);

  if (reading != b.lastReading) {
    b.lastChangeMs = nowMs;
    b.lastReading = reading;
  }

  if ((nowMs - b.lastChangeMs) >= DEBOUNCE_MS && reading != b.stableState) {
    b.stableState = reading;
    bool pressed = activeLow ? (b.stableState == LOW) : (b.stableState == HIGH);
    if (pressed) {
      b.pressedEvent = true;
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(RELAY_SOLENOID_1, OUTPUT);
  pinMode(RELAY_NUTRIENT_A, OUTPUT);
  pinMode(RELAY_NUTRIENT_B, OUTPUT);
  pinMode(RELAY_PH_PUMP, OUTPUT);
  pinMode(RELAY_MAIN_PUMP, OUTPUT);
  stopAllPumps();

  pinMode(BTN_START_PIN, INPUT_PULLUP);
  pinMode(BTN_STAGE_PIN, INPUT_PULLUP);
  pinMode(BTN_ESTOP_PIN, INPUT_PULLUP);

  // Initialize debouncer states with current readings.
  btnStart.stableState = btnStart.lastReading = digitalRead(BTN_START_PIN);
  btnStage.stableState = btnStage.lastReading = digitalRead(BTN_STAGE_PIN);
  btnEstop.stableState = btnEstop.lastReading = digitalRead(BTN_ESTOP_PIN);

  applyStage(0);
  Serial.println("Nutrient controller ready.");
  Serial.printf("Stage: %s | EC: %.2f | pH: %.2f\n", stages[currentStageIndex].name, targetEC, targetPH);
}

void loop() {
  unsigned long now = millis();

  // NO buttons: active low (press = LOW)
  updateButton(btnStart, true, now);
  updateButton(btnStage, true, now);
  // NC emergency: with INPUT_PULLUP in this wiring, emergency active = HIGH
  updateButton(btnEstop, false, now);

  emergencyActive = (btnEstop.stableState == HIGH);
  if (emergencyActive) {
    circulationEnabled = false;
    stopAllPumps();
  } else {
    if (btnStart.pressedEvent) {
      // Start circulation (main pump + solenoid 1)
      circulationEnabled = true;
      applyCirculationOutputs();
      Serial.println("Start button pressed -> Main pump and Solenoid 1 ON");
    }

    if (btnStage.pressedEvent) {
      uint8_t nextStage = (currentStageIndex + 1) % STAGE_COUNT;
      applyStage(nextStage);
      Serial.printf("Stage changed -> %s | EC: %.2f | pH: %.2f\n",
                    stages[currentStageIndex].name, targetEC, targetPH);
    }
  }

  // Non-blocking status output
  if (now - lastStatusPrint >= STATUS_PRINT_MS) {
    lastStatusPrint = now;
    Serial.printf("E-STOP:%s | Circulation:%s | Stage:%s | EC:%.2f | pH:%.2f\n",
                  emergencyActive ? "ACTIVE" : "OK",
                  circulationEnabled ? "ON" : "OFF",
                  stages[currentStageIndex].name,
                  targetEC,
                  targetPH);
  }
}
