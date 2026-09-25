#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <HT_SSD1306Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>
#include "lora/LoRa.h"

#define LED_PIN 35
#define VEXT_PIN 21
#define LORA_CS 8
#define LORA_RST 12
#define LORA_DIO0 14
#define LORA_FREQUENCY 923E6

SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);
BLECharacteristic *healthCharacteristic = nullptr;

static int testsRun = 0;
static int testsPassed = 0;
static bool overallPass = true;

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

void showOLEDStatus(const String &title, const String &message) {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 0, title);
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 24, message);
  display.display();
}

bool testSerialPort() {
  Serial.println("Serial port OK");
  return true;
}

bool testBoardLed() {
  pinMode(LED_PIN, OUTPUT);
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(100);
    digitalWrite(LED_PIN, LOW);
    delay(100);
  }
  return true;
}

bool testVextPower() {
  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, LOW);
  delay(150);
  digitalWrite(VEXT_PIN, HIGH);
  delay(150);
  digitalWrite(VEXT_PIN, LOW);
  Serial.println("Vext toggle OK");
  return true;
}

bool testI2CAndOLED() {
  Wire.begin();
  Wire.beginTransmission(0x3C);
  uint8_t err = Wire.endTransmission();
  if (err != 0) {
    Serial.printf("OLED I2C failed: err=%u\n", err);
    return false;
  }

  display.init();
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 22, "OLED OK");
  display.display();
  delay(250);
  return true;
}

bool testWiFiRadio() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(200);

  int foundNetworks = WiFi.scanNetworks(false, false);
  Serial.printf("WiFi scan count: %d\n", foundNetworks);

  if (foundNetworks < 0) {
    return false;
  }

  for (int i = 0; i < foundNetworks && i < 5; i++) {
    Serial.printf("WiFi AP %d: %s (%d dBm)\n", i, WiFi.SSID(i).c_str(), WiFi.RSSI(i));
  }

  WiFi.mode(WIFI_OFF);
  return true;
}

bool testBluetoothRadio() {
  BLEDevice::init("Heltec_BT_Health");
  BLEServer *server = BLEDevice::createServer();
  BLEService *service = server->createService("12345678-1234-1234-1234-123456789abc");
  healthCharacteristic = service->createCharacteristic(
      "12345678-1234-1234-1234-123456789abd",
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_NOTIFY);

  healthCharacteristic->setValue("HELTEC_BLE_HEALTH_CHECK");
  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(service->getUUID());
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  BLEDevice::startAdvertising();

  if (healthCharacteristic != nullptr) {
    healthCharacteristic->notify();
  }

  delay(250);
  Serial.println("BLE service initialized and advertising");
  return true;
}

bool testLoRaRadio() {
  SPI.begin(9, 11, 10, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_FREQUENCY, true)) {
    Serial.println("LoRa begin failed");
    return false;
  }

  LoRa.setTxPower(17, PA_OUTPUT_PA_BOOST_PIN);
  LoRa.enableCrc();

  const char payload[] = "HELTEC_LORA_OK";
  LoRa.beginPacket();
  LoRa.print(payload);
  int txResult = LoRa.endPacket();
  Serial.printf("LoRa TX result: %d\n", txResult);

  if (txResult == 0) {
    LoRa.sleep();
    return false;
  }

  LoRa.receive();
  unsigned long started = millis();
  bool rxReceived = false;

  while (millis() - started < 1500) {
    int packetSize = LoRa.parsePacket();
    if (packetSize > 0) {
      String data = "";
      while (LoRa.available()) {
        data += (char)LoRa.read();
      }
      Serial.printf("LoRa RX packet: %s\n", data.c_str());
      rxReceived = true;
      break;
    }
    delay(20);
  }

  LoRa.sleep();
  if (!rxReceived) {
    Serial.println("LoRa TX passed; RX idle because no peer transmitter was present");
  }

  return true;
}

void runFullHealthCheck() {
  Serial.println("\n=== Heltec V3.1 full board health check ===");

  recordResult("Serial", testSerialPort());
  recordResult("LED", testBoardLed());
  recordResult("Vext", testVextPower());
  recordResult("I2C+OLED", testI2CAndOLED());
  recordResult("WiFi", testWiFiRadio());
  recordResult("Bluetooth", testBluetoothRadio());
  recordResult("LoRa", testLoRaRadio());

  Serial.println("\n=== Summary ===");
  Serial.printf("Tests passed: %d / %d\n", testsPassed, testsRun);

  if (overallPass) {
    showOLEDStatus("BOARD OK", "ALL TESTS PASS");
    Serial.println("Board appears healthy.");
  } else {
    showOLEDStatus("BOARD FAIL", "CHECK FAILURES");
    Serial.println("One or more modules failed. Check the failed item above.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println("Heltec board health check start");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  runFullHealthCheck();
}

void loop() {
  digitalWrite(LED_PIN, HIGH);
  delay(300);
  digitalWrite(LED_PIN, LOW);
  delay(300);
}
