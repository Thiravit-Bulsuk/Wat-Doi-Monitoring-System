// LoRa Test Repeater Receiver
// Simulates a Repeater Node (ID 10) that only receives LoRa packets
// from Data Nodes and prints them to Serial Monitor, without forwarding to Master.

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

const int8_t TXpower = 14;  // LoRa transmit power in dBm (not used for receiving, but for setup)

SX126XLT LT;

// Initialize OLED Display and Button (optional, can be removed if not needed for test)
static SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);
#define PRG_BUTTON 0

// Global variables for display status (optional, can be removed if not needed for test)
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

// Task to handle Display and Button (optional, can be simplified or removed)
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
                display.drawString(0, 0, "Test Repeater 10 (Live)");
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

// Function to receive data (without forwarding)
void onReceive(uint8_t ThisNode) { // MasterNode parameter is not needed here
    int packetSize = LT.receiveSXBuffer(0, 0, WAIT_RX); // รับข้อมูลเข้า Buffer
        
    if (packetSize > 0) { // ตรวจสอบว่ามีแพ็กเก็ตเข้ามา
        if (packetSize == sizeof(LoRaPacket) || packetSize == (sizeof(LoRaPacket) + 2)) {
            Serial.println("Received packet (size OK)");
            logStatus = "RX " + String(packetSize) + " bytes";

            LoRaPacket packet;
            uint8_t rxBuffer[sizeof(LoRaPacket)];

            LT.startReadSXBuffer(0);
            for (int i = 0; i < sizeof(LoRaPacket); i++) {
                rxBuffer[i] = LT.readUint8();
            }
            if (packetSize == (sizeof(LoRaPacket) + 2)) { // อ่าน CRC ทิ้งไป
                LT.readUint8(); 
                LT.readUint8(); 
            }
            LT.endReadSXBuffer(); 

            memcpy(&packet, rxBuffer, sizeof(LoRaPacket)); 

            logLastPacket = "N" + String(packet.sourceNodeID) + " -> N" + String(packet.receiverID);

            Serial.print("Test Repeater: Raw packet data (hex - payload only): ");
            for (int i = 0; i < sizeof(LoRaPacket); i++) {
                Serial.printf("%02X ", rxBuffer[i]);
            }
            Serial.println();

            Serial.print("Test Repeater: Received packet.receiverID = "); Serial.println(packet.receiverID);
            Serial.print("Test Repeater: ThisNode = "); Serial.println(ThisNode);
            Serial.print("Test Repeater: sizeof(LoRaPacket) = "); Serial.println(sizeof(LoRaPacket));

            if (packet.receiverID == ThisNode) {
                Serial.print("Message from Node "); Serial.print(packet.sourceNodeID);
                Serial.print(": "); Serial.println(packet.value);
                Serial.print("Battery: "); Serial.println(packet.battery, 2);
                logAction = "Received OK";
            } else {
                Serial.println("Message does not meet criteria (ID mismatch), ignoring.");
                logAction = "Ignored (ID)";
            }
        } else {
            Serial.print("Received packet with unexpected size: "); Serial.print(packetSize); Serial.print(" bytes. Expected "); Serial.print(sizeof(LoRaPacket)); Serial.print(" or "); Serial.print(sizeof(LoRaPacket) + 2); Serial.println(" bytes. Ignoring.");
            LT.endReadSXBuffer(); 
            logAction = "Ignored (Size)";
        }
    } else if (packetSize < 0) { // Error in reception
        Serial.println("LoRa RX Error!");
        logStatus = "RX Error";
        logAction = "Error";
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("Test Repeater Receiver (Node 10) Initializing...");

    SPI.begin();
    delay(500);

    xTaskCreate(DisplayTask, "DisplayTask", 4096, NULL, 1, NULL);
    VextOFF(); 

    if (!initLoRa()) {
        Serial.println("LoRa initialization failed. Halting.");
        while (1); 
    }

    LT.setupLoRa(Frequency, Offset, SpreadingFactor, Bandwidth, CodeRate, Optimisation);
    delay(1000);
}

void loop() {
    logStatus = "Listening...";
    onReceive(10); // ThisNode is 10
}