#include <Arduino.h>
#include <SPI.h>  
#include <Wire.h>
#include <HT_SSD1306Wire.h>
#include <SX126XLT.h>
#include <ProgramLT_Definitions.h>
#include <esp_sleep.h>
#include "NodeConfig.h"

// Define LoRa settings
#define LORA_DEVICE DEVICE_SX1262
#define NSS 8
#define NRESET 12
#define RFBUSY 13
#define DIO1 14

// Define SPD status pin
#define SPD_PIN 4

// Configuration for SPD Type
#define SPD_IS_NC true // ตั้งเป็น true ถ้าเปลี่ยนอุปกรณ์เป็นแบบ NC (Normally Closed) เพื่อความปลอดภัย (Fail-Safe)

const uint32_t Frequency = 923000000;           // Frequency of transmissions
const uint32_t Offset = 0;                      // Offset frequency for calibration purposes

const uint8_t Bandwidth = LORA_BW_125;          // LoRa bandwidth
const uint8_t SpreadingFactor = LORA_SF12;      // LoRa spreading factor
const uint8_t CodeRate = LORA_CR_4_5;           // LoRa coding rate
const uint8_t Optimisation = LDRO_AUTO;         // Low data rate optimisation setting

const int8_t TXpower = 14;                      // LoRa transmit power in dBm

#define MESSAGE_TYPE_SPD 2
#define REPEATER_NODE 101
#define AUTO_RESET_INTERVAL_MS 43200000UL // 12 ชั่วโมง ป้องกัน heap fragmentation สะสมจากการรันต่อเนื่องเป็นเวลานาน

SX126XLT LT;

// Initialize OLED Display and Button
static SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);
#define PRG_BUTTON 0

void VextON() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW); // Active LOW เพื่อเปิดไฟ
}

void VextOFF() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, HIGH); // Active LOW (HIGH = OFF)
}

// Forward declaration
bool readSPDStatus();

// Function to display status on OLED when button is pressed
void showStatus() {
    Serial.println("Button Pressed: Showing Status");
    VextON();
    delay(50);
    display.init();

    display.displayOn();
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 0, "Node 20: SPD");
    
    bool failed = readSPDStatus();
    display.setFont(ArialMT_Plain_24);
    if (failed) {
        display.drawString(64, 25, "FAILED!");
    } else {
        display.drawString(64, 25, "Working");
    }

    display.display();
    delay(3000); // Show for 3 seconds
    display.displayOff();
}

// Function to initialize LoRa module
bool initLoRa() {
    if (!LT.begin(NSS, NRESET, RFBUSY, DIO1, LORA_DEVICE)) {
        Serial.println("LoRa init failed!");
        return false;
    }
    Serial.println("LoRa init succeeded.");
    return true;
}

// Function to read SPD status
bool readSPDStatus() {
    bool pinState = digitalRead(SPD_PIN);
    
    if (SPD_IS_NC) {
        // NC Logic (Fail-Safe):
        // - ปกติ (Good): หน้าสัมผัสปิด (Closed) -> ลง Ground -> Pin LOW
        // - เสีย/สายขาด (Bad): หน้าสัมผัสเปิด (Open) -> Pull-up -> Pin HIGH
        return pinState == HIGH; 
    } else {
        // NO Logic (Current):
        // - ปกติ (Good): หน้าสัมผัสเปิด (Open) -> Pull-up -> Pin HIGH
        // - เสีย (Bad): หน้าสัมผัสปิด (Closed) -> ลง Ground -> Pin LOW
        return pinState == LOW;
    }
}

// Function to send data
uint8_t sendData(uint8_t MessageType, uint8_t RepeaterNode, uint8_t NodeID, uint8_t status) {
    LoRaPacket packet;
    packet.messageType = MessageType;
    packet.receiverID = RepeaterNode;
    packet.senderID = NodeID;
    packet.sourceNodeID = NodeID;
    packet.value = status; // ส่งค่าสถานะ (0 = ปกติ, 1 = เสีย)
    packet.battery = 0.0;

    uint8_t txBuffer[sizeof(LoRaPacket)];
    memcpy(txBuffer, &packet, sizeof(LoRaPacket));

    Serial.printf("Sending Data: Node=%d, SPD=%s\n", NodeID, status == 1 ? "FAILED" : "OK");

    LT.startWriteSXBuffer(0);
    for (int i = 0; i < sizeof(LoRaPacket); i++) {
        LT.writeUint8(txBuffer[i]);
    }
    uint8_t len = LT.endWriteSXBuffer();

    return LT.transmitSXBuffer(0, (len + 2), 5000, TXpower, WAIT_TX);
}

// Task แยกต่างหากจาก loop() เพื่อให้ปุ่มกดตอบสนองได้แม้ขณะ loop() จะติดอยู่ใน receiveSXBuffer(WAIT_RX)
void ButtonTask(void *parameter) {
    for (;;) {
        if (digitalRead(PRG_BUTTON) == LOW) {
            showStatus();
            while (digitalRead(PRG_BUTTON) == LOW) vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("Data Node 20 (NongPog) - Polling Mode Initializing...");

    SPI.begin();
    delay(500);

    pinMode(PRG_BUTTON, INPUT_PULLUP);
    pinMode(SPD_PIN, INPUT_PULLUP);
    
    VextOFF();

    if (!initLoRa()) {
        Serial.println("Critical Error: LoRa initialization failed.");
        while (1);
    }

    LT.setupLoRa(Frequency, Offset, SpreadingFactor, Bandwidth, CodeRate, Optimisation);
    Serial.println("LoRa setup complete. Listening for poll requests...");

    xTaskCreate(ButtonTask, "ButtonTask", 4096, NULL, 1, NULL);
}

void loop() {
    if (millis() >= AUTO_RESET_INTERVAL_MS) {
        Serial.println("Auto-reset: 12h uptime reached, restarting...");
        ESP.restart();
    }

    // Listen for Master poll requests
    int packetSize = LT.receiveSXBuffer(0, 0, WAIT_RX);
    if (packetSize > 0) {
        if (packetSize == sizeof(LoRaPacket) || packetSize == (sizeof(LoRaPacket) + 2)) {
            LoRaPacket packet;
            uint8_t rxBuffer[sizeof(LoRaPacket)];

            LT.startReadSXBuffer(0);
            for (int i = 0; i < sizeof(LoRaPacket); i++) {
                rxBuffer[i] = LT.readUint8();
            }
            if (packetSize == (sizeof(LoRaPacket) + 2)) {
                LT.readUint8();
                LT.readUint8();
            }
            LT.endReadSXBuffer();

            memcpy(&packet, rxBuffer, sizeof(LoRaPacket));

            if (packet.messageType == MSG_TYPE_POLL_REQ && (packet.receiverID == 20 || packet.sourceNodeID == 20)) {
                Serial.println("Received POLL_REQ for Node 20 (SPD). Reading status...");
                bool failed = readSPDStatus();
                uint8_t status = failed ? 1 : 0;
                delay(50);
                sendData(MESSAGE_TYPE_SPD, REPEATER_NODE, 20, status);
            }
        } else {
            LT.endReadSXBuffer();
        }
    }
}