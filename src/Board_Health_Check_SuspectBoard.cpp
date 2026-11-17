#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <HT_SSD1306Wire.h>
#include <SPI.h>
#include <SX126XLT.h>
#include <ProgramLT_Definitions.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_system.h>

#define LED_PIN 35
#define LORA_DEVICE DEVICE_SX1262
#define NSS 8
#define NRESET 12
#define RFBUSY 13
#define DIO1 14
#define LORA_FREQUENCY 923000000

SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);
SX126XLT LT;
BLECharacteristic *healthCharacteristic = nullptr;

RTC_NOINIT_ATTR uint32_t stageBeforeReset;

static int testsRun = 0;
static int testsPassed = 0;
static bool overallPass = true;
static bool oledReady = false;

void showOLED(const String &line1, const String &line2, const String &line3 = "") {
  if (!oledReady) return;
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 0, line1);
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 18, line2);
  if (line3.length() > 0) {
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 44, line3);
  }
  display.display();
}

void recordResult(const String &name, bool ok) {
  testsRun++;
  if (ok) {
    testsPassed++;
  } else {
    overallPass = false;
  }

  Serial.print("[");
  Serial.print(name);
  Serial.print("] ");
  Serial.println(ok ? "PASS" : "FAIL");
}

bool testCoreLED() {
  pinMode(LED_PIN, OUTPUT);
  for (int i = 0; i < 2; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(120);
    digitalWrite(LED_PIN, LOW);
    delay(120);
  }
  Serial.println("Core LED OK");
  return true;
}

bool testI2C() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW); // เปิดไฟเลี้ยง OLED (Active LOW)
  delay(100);

  if (!display.init()) {
    Serial.println("OLED init failed");
    return false;
  }

  Wire.beginTransmission(0x3C);
  uint8_t err = Wire.endTransmission();
  Serial.printf("I2C scan error code: %u\n", err);
  if (err == 0) {
    oledReady = true;
    display.displayOn();
    showOLED("SUSPECT CHECK", "OLED OK", "Testing LoRa...");
  }
  return err == 0;
}

bool testLoRa() {
  SPI.begin(9, 11, 10, NSS);

  if (!LT.begin(NSS, NRESET, RFBUSY, DIO1, LORA_DEVICE)) {
    Serial.println("LoRa SX1262 begin failed");
    return false;
  }

  LT.setupLoRa(LORA_FREQUENCY, 0, LORA_SF7, LORA_BW_125, LORA_CR_4_5, LDRO_AUTO);

  const uint8_t payload[] = "HELTEC_LORA_SUSPECT_TEST";
  int txResult = LT.transmit((uint8_t *)payload, sizeof(payload) - 1, 2000, 14, WAIT_TX);
  Serial.printf("LoRa TX packet length (14 dBm): %d\n", txResult);

  if (txResult == 0) {
    return false;
  }

  uint8_t rxBuf[64];
  int rxLen = LT.receive(rxBuf, sizeof(rxBuf) - 1, 900, WAIT_RX);
  if (rxLen > 0) {
    rxBuf[rxLen] = '\0';
    Serial.printf("LoRa RX packet: %s\n", (char *)rxBuf);
  } else {
    Serial.println("LoRa TX passed; no peer packet received during brief listen window");
  }

  return true;
}

bool testWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.disconnect(true);
  delay(150);

  int found = WiFi.scanNetworks(false, false);
  Serial.printf("WiFi scan found: %d network(s)\n", found);
  WiFi.mode(WIFI_OFF);
  delay(150);
  return found >= 0;
}

bool testBluetooth() {
  BLEDevice::init("Heltec_BT_Suspect");
  BLEServer *server = BLEDevice::createServer();
  BLEService *service = server->createService("12345678-1234-1234-1234-123456789abc");
  healthCharacteristic = service->createCharacteristic(
      "12345678-1234-1234-1234-123456789abd",
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_NOTIFY);

  healthCharacteristic->setValue("HELTEC_BLE_TEST");
  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(service->getUUID());
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  BLEDevice::startAdvertising();

  if (healthCharacteristic != nullptr) {
    healthCharacteristic->notify();
  }

  delay(150);
  Serial.println("BLE service initialized and advertising");
  return true;
}

void runSuspectCheck() {
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.println("\n=== Heltec suspect board health check ===");
  Serial.printf("Last reset reason: %d (stageBeforeReset=0x%08X)\n", (int)reason, stageBeforeReset);

  bool brownoutFromLoRa = (reason == ESP_RST_BROWNOUT && stageBeforeReset == 0xABCD0000);
  bool brownoutFromWiFi = (reason == ESP_RST_BROWNOUT && stageBeforeReset == 0xABCD0001);
  bool brownoutFromBLE  = (reason == ESP_RST_BROWNOUT && stageBeforeReset == 0xABCD0002);

  recordResult("Core LED", testCoreLED());
  recordResult("I2C+OLED", testI2C());

  bool loraOk = false;
  if (brownoutFromLoRa) {
    Serial.println("[LoRa SX1262] FAIL (Triggered BROWNOUT_RST during LoRa TX on previous boot!)");
    recordResult("LoRa SX1262", false);
  } else {
    stageBeforeReset = 0xABCD0000;
    loraOk = testLoRa();
    recordResult("LoRa SX1262", loraOk);
  }

  if (brownoutFromLoRa || brownoutFromWiFi) {
    Serial.println("[WiFi] FAIL/SKIPPED (Board 3.3V rail collapses on RF transmit)");
    recordResult("WiFi", false);
  } else {
    showOLED("SUSPECT CHECK", "LoRa PASS", "Testing WiFi...");
    stageBeforeReset = 0xABCD0001;
    Serial.println("Testing WiFi...");
    Serial.flush();
    recordResult("WiFi", testWiFi());

    if (brownoutFromBLE) {
      Serial.println("[Bluetooth] FAIL (Triggered BROWNOUT_RST on previous boot!)");
      recordResult("Bluetooth", false);
    } else {
      stageBeforeReset = 0xABCD0002;
      recordResult("Bluetooth", testBluetooth());
    }
  }

  stageBeforeReset = 0;

  Serial.println("\n=== Diagnostic Summary ===");
  Serial.printf("Passed: %d / %d\n", testsPassed, testsRun);

  if (overallPass) {
    showOLED("DIAG RESULT", "ALL PASS", "Board Healthy");
    Serial.println("RESULT: All modules healthy.");
  } else if (brownoutFromLoRa) {
    showOLED("BOARD FAULTY", "3.3V BROWNOUT", "DO NOT USE IN FIELD");
    Serial.println("RESULT: UNUSABLE FOR NODE 1 (JD)!");
    Serial.println("FAULT: Even LoRa TX (14 dBm) causes immediate 3.3V BROWNOUT_RST.");
  } else {
    showOLED("DIAG RESULT", "BOARD FAIL", "Check Serial");
    Serial.println("RESULT: Critical module failed.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("Heltec suspect board start");
  runSuspectCheck();
}

void loop() {
  delay(5000);
}
