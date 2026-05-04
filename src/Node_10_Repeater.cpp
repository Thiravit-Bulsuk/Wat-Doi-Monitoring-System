#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <HT_SSD1306Wire.h>
#include <SX126XLT.h>
#include <ProgramLT_Definitions.h>
#include "NodeConfig.h"

// Define LoRa settings
#define LORA_DEVICE DEVICE_SX1262
#define NSS 8
#define NRESET 12
#define RFBUSY 13
#define DIO1 14

const uint32_t Frequency = 923000000;  // Frequency of transmissions
const uint32_t Offset = 0;             // Offset frequency for calibration purposes

const uint8_t Bandwidth = LORA_BW_125;  // LoRa bandwidth
const uint8_t SpreadingFactor = LORA_SF12;  // LoRa spreading factor
const uint8_t CodeRate = LORA_CR_4_5;  // LoRa coding rate
const uint8_t Optimisation = LDRO_AUTO;  // Low data rate optimisation setting

const int8_t TXpower = 14;  // LoRa transmit power in dBm

SX126XLT LT;

// Initialize OLED Display and Button
static SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);
#define PRG_BUTTON 0

// Global variables for display status
volatile bool displayActive = false;
unsigned long displayTimeout = 0;
String logStatus = "Waiting...";
String logLastPacket = "-";
String logAction = "-";

void VextON() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
}

void VextOFF() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, HIGH);
}

// Task to handle Display and Button
void DisplayTask(void *parameter) {
    pinMode(PRG_BUTTON, INPUT_PULLUP);
    
    for(;;) {
        if (digitalRead(PRG_BUTTON) == LOW) {
            if (!displayActive) {
                VextON();
                delay(50);
                display.init();
                display.displayOn();
                display.setFont(ArialMT_Plain_10);
                display.setTextAlignment(TEXT_ALIGN_LEFT);
            }
            displayActive = true;
            displayTimeout = millis() + 180000; // 3 minutes
            vTaskDelay(200 / portTICK_PERIOD_MS); // Debounce
        }

        if (displayActive) {
            if (millis() > displayTimeout) {
                displayActive = false;
                display.clear();
                display.display();
                display.displayOff();
                VextOFF();
            } else {
                display.clear();
                display.drawString(0, 0, "Repeater Node 10 (Live)");
                display.drawString(0, 13, "St: " + logStatus);
                display.drawString(0, 26, "Pkt: " + logLastPacket);
                display.drawString(0, 39, "Act: " + logAction);
                display.drawString(0, 52, "Off in: " + String((displayTimeout - millis())/1000) + "s");
                display.display();
            }
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
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

// Function to receive data
void onReceive(uint8_t MasterNode, uint8_t ThisNode) {
    int packetSize = LT.receiveSXBuffer(0, 0, WAIT_RX); // รับข้อมูลเข้า Buffer
        
    if (packetSize > 0) { // ตรวจสอบว่ามีแพ็กเก็ตเข้ามา
        // ตรวจสอบขนาดแพ็กเก็ตที่ได้รับ
        // กรณีที่ 1: packetSize เท่ากับขนาดของ LoRaPacket (อาจจะไม่มี CRC ในการรายงาน)
        // กรณีที่ 2: packetSize เท่ากับขนาดของ LoRaPacket + 2 (รวม CRC)
        if (packetSize == sizeof(LoRaPacket) || packetSize == (sizeof(LoRaPacket) + 2)) {
            Serial.println("Received packet (size OK)");
            logStatus = "RX " + String(packetSize) + " bytes";

            LoRaPacket packet;
            uint8_t rxBuffer[sizeof(LoRaPacket)];

            LT.startReadSXBuffer(0);
            for (int i = 0; i < sizeof(LoRaPacket); i++) {
                rxBuffer[i] = LT.readUint8();
            }
            // ถ้า packetSize รวม CRC มาด้วย เราต้องอ่าน 2 ไบต์นั้นทิ้งไป
            if (packetSize == (sizeof(LoRaPacket) + 2)) {
                LT.readUint8(); // อ่าน CRC byte 1 ทิ้ง
                LT.readUint8(); // อ่าน CRC byte 2 ทิ้ง
            }
            LT.endReadSXBuffer(); // End reading from SXBuffer

            memcpy(&packet, rxBuffer, sizeof(LoRaPacket)); // Copy byte array to struct

            logLastPacket = "N" + String(packet.sourceNodeID) + " -> N" + String(packet.receiverID);

            Serial.print("Repeater: Raw packet data (hex - payload only): ");
            for (int i = 0; i < sizeof(LoRaPacket); i++) { // Print only payload bytes
                Serial.printf("%02X ", rxBuffer[i]);
            }
            Serial.println();

            Serial.print("Repeater: Received packet.receiverID = "); Serial.println(packet.receiverID);
            Serial.print("Repeater: ThisNode = "); Serial.println(ThisNode);
            Serial.print("Repeater: sizeof(LoRaPacket) = "); Serial.println(sizeof(LoRaPacket));

            if (packet.receiverID == ThisNode) {
                Serial.print("Message from Node "); Serial.print(packet.sourceNodeID);
                Serial.print(": "); Serial.println(packet.value);
                logAction = "Forwarding...";

                delay(1000);

                // Prepare packet for forwarding
                LoRaPacket forwardPacket = packet; // Copy received packet
                forwardPacket.receiverID = MasterNode; // เปลี่ยนผู้รับเป็น Master (100)
                forwardPacket.senderID = ThisNode;     // เปลี่ยนผู้ส่งเป็นตัวมันเอง (10)

                uint8_t txForwardBuffer[sizeof(LoRaPacket)];
                memcpy(txForwardBuffer, &forwardPacket, sizeof(LoRaPacket));

                LT.startWriteSXBuffer(0);
                for (int i = 0; i < sizeof(LoRaPacket); i++) {
                    LT.writeUint8(txForwardBuffer[i]);
                }
                uint8_t len = LT.endWriteSXBuffer();

                uint8_t TXPacketL = LT.transmitSXBuffer(0, (len + 2), 5000, TXpower, WAIT_TX);

                if (TXPacketL == 0) {
                  Serial.println("Error: Failed to send data");
                  logAction = "Send Failed";
                } else {
                    Serial.println("Data sent successfully");
                    Serial.println("Data forwarded to Master Node.");
                    logAction = "Forwarded OK";
                    
                }
            } else {
                Serial.println("Message does not meet criteria, ignoring.");
                logAction = "Ignored";
                Serial.print("Repeater: Received packet.receiverID = "); Serial.print(packet.receiverID);
                Serial.print(", Expected ThisNode = "); Serial.println(ThisNode);
                Serial.print("Repeater: sizeof(LoRaPacket) = "); Serial.println(sizeof(LoRaPacket));
            }
        } else {
            Serial.print("Received packet with unexpected size: "); Serial.print(packetSize); Serial.print(" bytes. Expected "); Serial.print(sizeof(LoRaPacket)); Serial.print(" or "); Serial.print(sizeof(LoRaPacket) + 2); Serial.println(" bytes. Ignoring.");
            LT.endReadSXBuffer(); // Clear any pending read operations
        }
    } else if (packetSize > 0) {
        Serial.print("Received packet with unexpected size: "); Serial.print(packetSize); Serial.print(" bytes. Expected "); Serial.print(sizeof(LoRaPacket)); Serial.print(" or "); Serial.print(sizeof(LoRaPacket) + 2); Serial.println(" bytes. Ignoring.");
        LT.endReadSXBuffer(); // Clear any pending read operations
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("Repeater Node 10 Initializing...");

    SPI.begin();
    delay(500);

    // Start Display Task
    xTaskCreate(DisplayTask, "DisplayTask", 4096, NULL, 1, NULL);
    VextOFF(); // Ensure display is off initially

    if (!initLoRa()) {
        Serial.println("LoRa initialization failed. Halting.");
        while (1); 
    }

    LT.setupLoRa(Frequency, Offset, SpreadingFactor, Bandwidth, CodeRate, Optimisation);
    delay(1000);
}

void loop() {
    logStatus = "Listening...";
    onReceive(100, 10);
}