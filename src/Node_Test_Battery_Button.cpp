#include <Arduino.h>
#include <HT_SSD1306Wire.h>
#include <Wire.h>

// Heltec WiFi LoRa 32 V3
#ifndef Vext
#define Vext 36
#endif

#define ADC_BATTERY_PIN 1
#define BTN_PIN 0

const unsigned long LONG_PRESS_MS = 1500;

float gLastAdcVoltage = 0.0f;

SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64,
                   RST_OLED);

struct BatteryReading {
  float avgMv;
  float avgVoltage;
  float minVoltage;
  float maxVoltage;
};

void showOledScreen(const String &line1, const String &line2,
                    const String &line3 = "", const String &line4 = "") {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 0, line1);
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 16, line2);

  if (line3.length() > 0) {
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 40, line3);
  }
  if (line4.length() > 0) {
    display.drawString(64, 52, line4);
  }
  display.display();
}

float readAverageRawAdcMv(uint8_t sampleCount) {
  pinMode(ADC_BATTERY_PIN, INPUT);
  analogSetPinAttenuation(ADC_BATTERY_PIN, ADC_11db);

  uint32_t sumMv = 0;
  for (uint8_t i = 0; i < sampleCount; ++i) {
    sumMv += analogReadMilliVolts(ADC_BATTERY_PIN);
    delay(15);
  }

  return (float)sumMv / (float)sampleCount;
}

void showRawAdcLive() {
  digitalWrite(Vext, LOW);
  delay(500);

  showOledScreen("RAW ADC", "Settling...", "wait 0.5s", "then read");
  delay(500);

  float avgMv = readAverageRawAdcMv(20);
  gLastAdcVoltage = avgMv / 1000.0f;
  digitalWrite(Vext, HIGH);

  char line2[24];
  snprintf(line2, sizeof(line2), "ADC %.0f mV", avgMv);

  char line3[24];
  snprintf(line3, sizeof(line3), "RAW V %.3f", gLastAdcVoltage);

  showOledScreen("RAW ADC", line2, line3, "Press to refresh");

  Serial.printf("RAW ADC avg = %.0f mV | ADC V = %.3f V\n", avgMv,
                gLastAdcVoltage);
}

void runCalibrationMode() {
  showRawAdcLive();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(Vext, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);

  display.init();
  display.displayOn();
  display.clear();
  display.display();

  digitalWrite(Vext, HIGH);
  analogSetPinAttenuation(ADC_BATTERY_PIN, ADC_11db);

  showOledScreen("RAW ADC TEST", "Press button", "Shows ADC mV", "No ratio yet");

  Serial.println("=== RAW ADC TEST ===");
  Serial.println("Board: Heltec WiFi LoRa 32 V3");
  Serial.println("ADC pin: GPIO 1 (A0)");
  Serial.println("Vext: GPIO 36 (active LOW)");
  Serial.println("Button press = refresh raw ADC value");
}

void loop() {
  if (digitalRead(BTN_PIN) == LOW) {
    unsigned long pressStart = millis();

    while (digitalRead(BTN_PIN) == LOW) {
      delay(20);
      if ((millis() - pressStart) >= LONG_PRESS_MS) {
        showRawAdcLive();
        while (digitalRead(BTN_PIN) == LOW) {
          delay(20);
        }
        return;
      }
    }

    if ((millis() - pressStart) < LONG_PRESS_MS) {
      showRawAdcLive();
    }

    while (digitalRead(BTN_PIN) == LOW) {
      delay(20);
    }
  }

  delay(20);
}
