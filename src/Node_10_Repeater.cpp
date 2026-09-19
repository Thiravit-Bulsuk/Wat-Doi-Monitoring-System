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
#define AUTO_RESET_INTERVAL_MS 43200000UL // 12 ชั่วโมง ป้องกัน heap fragmentation สะสมจากการ forward packet ต่อเนื่องเป็นเวลานาน

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
                display.drawString(0, 0, "Repeater Node 101 (Live)");
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

    if (packetSize <= 0) {
        // ไม่ได้แพ็กเก็ต: พิมพ์เหตุผล (Timeout / CRC error / Header error) เพื่อ debug
        uint16_t irq = LT.readIrqStatus();
        Serial.printf("Repeater RX fail, IRQ status: 0x%04X", irq);
        if (irq & IRQ_CRC_ERROR) Serial.print(" [CRC_ERROR]");
        if (irq & IRQ_HEADER_ERROR) Serial.print(" [HEADER_ERROR]");
        if (irq & IRQ_RX_TX_TIMEOUT) Serial.print(" [TIMEOUT]");
        Serial.println();
        logStatus = "RX fail 0x" + String(irq, HEX);
    }

    if (packetSize > 0) { // ตรวจสอบว่ามีแพ็กเก็ตเข้ามา
        if (packetSize == sizeof(LoRaPacket) || packetSize == (sizeof(LoRaPacket) + 2)) {
            int16_t rssi = LT.readPacketRSSI();
            int8_t snr = LT.readPacketSNR();
            Serial.printf("Received packet (size OK), RSSI=%d dBm, SNR=%d dB\n", rssi, snr);
            logStatus = "RX " + String(packetSize) + "B R" + String(rssi);

            LoRaPacket packet;
            uint8_t rxBuffer[sizeof(LoRaPacket)];

            LT.startReadSXBuffer(0);
            for (int i = 0; i < sizeof(LoRaPacket); i++) {
                rxBuffer[i] = LT.readUint8();
            }
            if (packetSize == (sizeof(LoRaPacket) + 2)) {
                LT.readUint8(); // อ่าน CRC byte 1 ทิ้ง
                LT.readUint8(); // อ่าน CRC byte 2 ทิ้ง
            }
            LT.endReadSXBuffer();

            memcpy(&packet, rxBuffer, sizeof(LoRaPacket));

            logLastPacket = "Type " + String(packet.messageType) + " | N" + String(packet.senderID) + " -> N" + String(packet.receiverID);

            Serial.printf("Repeater RX: Type=%d, Sender=%d, Recv=%d, Target/Source=%d, Val=%d, Bat=%.2fV\n",
                          packet.messageType, packet.senderID, packet.receiverID, packet.sourceNodeID, packet.value, packet.battery);

            bool shouldForward = false;
            bool isNodeUplink = false;
            LoRaPacket forwardPacket = packet;
            forwardPacket.senderID = ThisNode; // 101

            // กรณีที่ 1: Downlink (Master ขอข้อมูลไปยัง Data Node)
            if (packet.messageType == MSG_TYPE_POLL_REQ && packet.receiverID == ThisNode) {
                forwardPacket.receiverID = packet.sourceNodeID; // ส่งต่อไปยัง Node ปลายทาง
                logAction = "Fwd Poll -> N" + String(packet.sourceNodeID);
                Serial.println("Repeater: Forwarding POLL_REQ to Node " + String(packet.sourceNodeID));
                shouldForward = true;
            }
            // กรณีที่ 2: Uplink (Data Node ส่งข้อมูลกลับมาให้ Repeater เพื่อส่งต่อให้ Master)
            else if ((packet.messageType == MSG_TYPE_WATER || packet.messageType == MSG_TYPE_SPD) && packet.receiverID == ThisNode) {
                forwardPacket.receiverID = MasterNode; // ส่งต่อไปยัง Master (100)
                logAction = "Fwd Data N" + String(packet.sourceNodeID) + " -> Master";
                Serial.println("Repeater: Forwarding Data from Node " + String(packet.sourceNodeID) + " to Master");
                shouldForward = true;
                isNodeUplink = true;
            } else {
                Serial.println("Repeater: Packet ignored (not addressed to this repeater).");
                logAction = "Ignored";
            }

            if (shouldForward) {
                if (isNodeUplink) {
                    LoRaPacket linkQualityPacket = {};
                    linkQualityPacket.messageType = MSG_TYPE_LINK_QUALITY;
                    linkQualityPacket.receiverID = MasterNode;
                    linkQualityPacket.senderID = ThisNode;
                    linkQualityPacket.sourceNodeID = packet.sourceNodeID;
                    linkQualityPacket.value = (uint8_t)(snr + 128);
                    linkQualityPacket.battery = (float)rssi;

                    uint8_t txQualityBuffer[sizeof(LoRaPacket)];
                    memcpy(txQualityBuffer, &linkQualityPacket, sizeof(LoRaPacket));
                    LT.startWriteSXBuffer(0);
                    for (int i = 0; i < sizeof(LoRaPacket); i++) {
                        LT.writeUint8(txQualityBuffer[i]);
                    }
                    uint8_t qualityLen = LT.endWriteSXBuffer();
                    uint8_t qualityResult = LT.transmitSXBuffer(0, qualityLen + 2, 5000, TXpower, WAIT_TX);
                    if (qualityResult == 0) {
                        Serial.println("Error: Failed to forward Node link quality");
                    } else {
                        Serial.printf("Forwarded Node %d link quality to Master: RSSI=%d dBm, SNR=%d dB\n",
                                      packet.sourceNodeID, rssi, snr);
                    }
                }

                delay(100); // หน่วงเวลาเล็กน้อยเพื่อความเสถียรของช่องสัญญาณ

                uint8_t txForwardBuffer[sizeof(LoRaPacket)];
                memcpy(txForwardBuffer, &forwardPacket, sizeof(LoRaPacket));

                LT.startWriteSXBuffer(0);
                for (int i = 0; i < sizeof(LoRaPacket); i++) {
                    LT.writeUint8(txForwardBuffer[i]);
                }
                uint8_t len = LT.endWriteSXBuffer();

                uint8_t TXPacketL = LT.transmitSXBuffer(0, (len + 2), 5000, TXpower, WAIT_TX);

                if (TXPacketL == 0) {
                    Serial.println("Error: Failed to forward packet");
                    logAction = "Fwd Failed";
                } else {
                    Serial.println("Packet forwarded successfully.");
                    logAction = "Fwd OK";
                }
            }
        } else {
            Serial.print("Received packet with unexpected size: "); Serial.println(packetSize);
            LT.endReadSXBuffer();
        }
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("Repeater Node 101 Initializing...");

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

    // Debug: ตรวจสอบว่าตั้งค่า LoRa ตรงกับที่ตั้งใจไว้จริงหรือไม่
    Serial.println(F("--- LoRa Modem Settings ---"));
    LT.printModemSettings();
    Serial.println();
    Serial.println(F("--- LoRa Operating Settings ---"));
    LT.printOperatingSettings();
    Serial.println();
}

void loop() {
    if (millis() >= AUTO_RESET_INTERVAL_MS) {
        Serial.println("Auto-reset: 12h uptime reached, restarting...");
        ESP.restart();
    }

    logStatus = "Listening...";
    onReceive(100, 101);
}