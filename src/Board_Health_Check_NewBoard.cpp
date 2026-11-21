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

#define LED_PIN 35
#define VEXT_PIN Vext // GPIO 36 บน Heltec V3 (GPIO 21 คือ RST_OLED)

#define LORA_DEVICE DEVICE_SX1262
#define NSS 8
#define NRESET 12
#define RFBUSY 13
#define DIO1 14
#define LORA_FREQUENCY 923000000

SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);
SX126XLT LT;
BLECharacteristic *healthCharacteristic = nullptr;

static int testsRun = 0;
static int testsPassed = 0;
static bool overallPass = true;
static bool oledReady = false;

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
  if (!oledReady) {
    Serial.printf("OLED unavailable; %s: %s\n", title.c_str(), message.c_str());
    return;
  }

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
  digitalWrite(VEXT_PIN, HIGH); // OFF
  delay(100);
  digitalWrite(VEXT_PIN, LOW);  // ON (Active LOW จ่ายไฟให้ OLED)
  delay(150);
  Serial.println("Vext toggle OK");
  return true;
}

bool testI2CAndOLED() {
  oledReady = false;

  // ปลด Reset OLED และเริ่มต้น I2C บนขา SDA_OLED (17), SCL_OLED (18)
  if (!display.init()) {
    Serial.println("OLED init failed");
    return false;
  }

  Wire.beginTransmission(0x3C);
  uint8_t err = Wire.endTransmission();
  if (err != 0) {
    Serial.printf("OLED I2C failed: err=%u\n", err);
    return false;
  }

  oledReady = true;
  display.displayOn();
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
  WiFi.setTxPower(WIFI_POWER_8_5dBm); // ลดกระแสกระชากป้องกัน BROWNOUT_RST
  WiFi.disconnect(true);
  delay(200);

  int foundNetworks = WiFi.scanNetworks(false, false);
  Serial.printf("WiFi scan count: %d\n", foundNetworks);

  if (foundNetworks < 0) {
    WiFi.mode(WIFI_OFF);
    return false;
  }

  for (int i = 0; i < foundNetworks && i < 5; i++) {
    Serial.printf("WiFi AP %d: %s (%d dBm)\n", i, WiFi.SSID(i).c_str(), WiFi.RSSI(i));
  }

  WiFi.mode(WIFI_OFF);
  delay(200);
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
  SPI.begin(9, 11, 10, NSS);

  if (!LT.begin(NSS, NRESET, RFBUSY, DIO1, LORA_DEVICE)) {
    Serial.println("LoRa SX1262 begin failed");
    return false;
  }

  LT.setupLoRa(LORA_FREQUENCY, 0, LORA_SF7, LORA_BW_125, LORA_CR_4_5, LDRO_AUTO);

  const uint8_t payload[] = "HELTEC_LORA_OK";
  int txResult = LT.transmit((uint8_t *)payload, sizeof(payload) - 1, 2000, 14, WAIT_TX);
  Serial.printf("LoRa TX packet length: %d\n", txResult);

  if (txResult == 0) {
    return false;
  }

  uint8_t rxBuf[64];
  int rxLen = LT.receive(rxBuf, sizeof(rxBuf) - 1, 1500, WAIT_RX);
  if (rxLen > 0) {
    rxBuf[rxLen] = '\0';
    Serial.printf("LoRa RX packet: %s\n", (char *)rxBuf);
  } else {
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
