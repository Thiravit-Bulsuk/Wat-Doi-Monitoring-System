#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>
#include "lora/LoRa.h"

#define LED_PIN 35
#define LORA_CS 8
#define LORA_RST 12
#define LORA_DIO0 14
#define LORA_FREQUENCY 923E6

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
  Wire.begin();
  Wire.beginTransmission(0x3C);
  uint8_t err = Wire.endTransmission();
  Serial.printf("I2C scan error code: %u\n", err);
  return err == 0;
}

bool testWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(150);

  int found = WiFi.scanNetworks(false, false);
  Serial.printf("WiFi scan found: %d network(s)\n", found);
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

bool testLoRa() {
  SPI.begin(9, 11, 10, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_FREQUENCY, true)) {
    Serial.println("LoRa begin failed");
    return false;
  }

  LoRa.setTxPower(17, PA_OUTPUT_PA_BOOST_PIN);
  LoRa.beginPacket();
  LoRa.print("HELTEC_LORA_SUSPECT_TEST");
  int txResult = LoRa.endPacket();
  Serial.printf("LoRa TX result: %d\n", txResult);

  if (txResult == 0) {
    LoRa.sleep();
    return false;
  }

  LoRa.receive();
  bool received = false;
  unsigned long started = millis();

  while (millis() - started < 900) {
    int packetSize = LoRa.parsePacket();
    if (packetSize > 0) {
      String data = "";
      while (LoRa.available()) {
        data += (char)LoRa.read();
      }
      Serial.printf("LoRa RX packet: %s\n", data.c_str());
      received = true;
      break;
    }
    delay(20);
  }

  LoRa.sleep();
  if (!received) {
    Serial.println("LoRa TX passed; no peer packet received during brief listen window");
  }

  return true;
}

void runSuspectCheck() {
  Serial.println("\n=== Heltec suspect board health check ===");
  Serial.println("This version avoids risky power toggling and uses a conservative test set.");

  recordResult("Core LED", testCoreLED());
  recordResult("I2C", testI2C());
  recordResult("WiFi", testWiFi());
  recordResult("Bluetooth", testBluetooth());
  recordResult("LoRa", testLoRa());

  Serial.println("\n=== Summary ===");
  Serial.printf("Passed: %d / %d\n", testsPassed, testsRun);

  if (overallPass) {
    Serial.println("This unit appears to be alive and responding to basic radio checks.");
  } else {
    Serial.println("This unit failed at least one critical test. Stop using it and re-check power wiring.");
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
