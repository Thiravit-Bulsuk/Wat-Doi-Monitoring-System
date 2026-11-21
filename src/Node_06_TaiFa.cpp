#include <SPI.h>
#include <Wire.h>
#include <HT_SSD1306Wire.h>
#include <SX126XLT.h>
#include <ProgramLT_Definitions.h>
#include <esp_sleep.h>
#include "NodeConfig.h"
#include "Arduino.h"

// Define LoRa settings
#define LORA_DEVICE DEVICE_SX1262
#define NSS 8
#define NRESET 12
#define RFBUSY 13
#define DIO1 14

// Define sensor pin
#define TRIGGER_PIN 6
#define ECHO_PIN 7

// Define Tank Constants (Calibration)
const int TANK_FULL_CM = 36;   // ระยะห่างระหว่างเซ็นเซอร์กับน้ำเต็ม
const int TANK_EMPTY_CM = 100; // ระยะห่างระหว่างเซ็นเซอร์กับก้นแทงก์

const uint32_t Frequency = 923000000;           // Frequency of transmissions
const uint32_t Offset = 0;                      // Offset frequency for calibration purposes

const uint8_t Bandwidth = LORA_BW_125;          // LoRa bandwidth
const uint8_t SpreadingFactor = LORA_SF12;      // LoRa spreading factor
const uint8_t CodeRate = LORA_CR_4_5;           // LoRa coding rate
const uint8_t Optimisation = LDRO_AUTO;         // Low data rate optimisation setting

const int8_t TXpower = 14;                      // ปรับเป็น 14 dBm เพื่อความเสถียรในการส่ง

#define MESSAGE_TYPE 1
#define REPEATER_NODE 101
#define AUTO_RESET_INTERVAL_MS 43200000UL // 12 ชั่วโมง ป้องกัน heap fragmentation สะสมจากการรันต่อเนื่องเป็นเวลานาน
#define NODE_ID 6       // หมายเลข Node TaiFa

SX126XLT LT;

// Initialize OLED Display and Button
static SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);
#define PRG_BUTTON 0

void VextON() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
  Serial.println("Vext ON");
}

void VextOFF() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, HIGH);
  Serial.println("Vext OFF");
}

// Logic สำหรับความต่อเนื่องของข้อมูล (Layer 2: Sanity Check)
RTC_DATA_ATTR float lastValidDistance = TANK_FULL_CM; 
RTC_DATA_ATTR bool isTankEmpty = false;       
RTC_DATA_ATTR int refillConfirmationCount = 0; 
const int REFILL_THRESHOLD = 3;  

RTC_DATA_ATTR uint8_t lastPercentage = 100; 

// Forward declaration
float readDistance();

bool initLoRa() {
    if (!LT.begin(NSS, NRESET, RFBUSY, DIO1, LORA_DEVICE)) {
        Serial.println("LoRa init failed!");
        return false;
    }
    Serial.println("LoRa init succeeded.");
    return true;
}

void showStatus() {
    Serial.println("Button Pressed: Showing Status");
    VextON();
    delay(50);
    display.init();
    display.displayOn();
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 20, "Measuring...");
    display.display();

    float distance = readDistance();
    
    display.clear();
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 0, "Node 6: TaiFa");
    display.setFont(ArialMT_Plain_24);

    if (distance < 0) {
        display.drawString(64, 20, "Error");
    } else {
        long calc_percentage = map(distance, TANK_FULL_CM, TANK_EMPTY_CM, 100, 0);
        calc_percentage = constrain(calc_percentage, 0, 100);
        display.drawString(64, 20, String(calc_percentage) + " %");
    }

    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 50, "System Working");
    display.display();
    delay(3000); 
    display.displayOff();
    VextOFF();
}

void smartDelay(unsigned long ms) {
    unsigned long start = millis();
    while (millis() - start < ms) {
        if (digitalRead(PRG_BUTTON) == LOW) {
            showStatus();
            while(digitalRead(PRG_BUTTON) == LOW) delay(10);
        }
        delay(50);
    }
}

float readDistance() {
    const int BURST_COUNT = 20;     
    const int MAX_RETRIES = 3;      
    const float CLUSTER_RANGE = 5.0;
    const int MIN_CLUSTER_SIZE = 5; 

    for (int retry = 0; retry < MAX_RETRIES; retry++) {
        float validReadings[BURST_COUNT];
        int validCount = 0;
        int errorCount = 0;

        for (int i = 0; i < BURST_COUNT; i++) {
            digitalWrite(TRIGGER_PIN, LOW);
            delayMicroseconds(2);
            digitalWrite(TRIGGER_PIN, HIGH);
            delayMicroseconds(10);
            digitalWrite(TRIGGER_PIN, LOW);
            
            long duration = pulseIn(ECHO_PIN, HIGH, 30000); 
            float dist = (duration * 0.0343) / 2;
            
            if (duration == 0 || dist < 15.0) { // ปรับระยะ Blind spot ให้สั้นลงตามความสูงแทงก์
                errorCount++;
            } else {
                validReadings[validCount++] = dist;
            }
            delay(20);
        }

        if (validCount < MIN_CLUSTER_SIZE) {
             if (retry == MAX_RETRIES - 1) return -1.0;
             continue;
        }

        // Bubble Sort
        for (int i = 0; i < validCount - 1; i++) {
            for (int j = 0; j < validCount - i - 1; j++) {
                if (validReadings[j] > validReadings[j + 1]) {
                    float temp = validReadings[j];
                    validReadings[j] = validReadings[j + 1];
                    validReadings[j + 1] = temp;
                }
            }
        }

        int bestClusterSize = 0;
        float bestClusterAvg = 0;
        for (int i = 0; i < validCount; i++) {
            int count = 0;
            float sum = 0;
            for (int j = i; j < validCount; j++) {
                if (validReadings[j] - validReadings[i] <= CLUSTER_RANGE) {
                    count++;
                    sum += validReadings[j];
                } else { break; }
            }
            if (count > bestClusterSize) {
                bestClusterSize = count;
                bestClusterAvg = sum / count;
            }
        }

        if (bestClusterSize >= MIN_CLUSTER_SIZE && bestClusterSize >= errorCount) {
            return bestClusterAvg;
        }
    }
    return -1.0;
}

// Function to measure water level and compute percentage
uint8_t measureAndCalculatePercentage() {
    VextON();
    float distance = readDistance();
    VextOFF();

    uint8_t percentage = 0;

    if (distance < 0) {
        if (lastValidDistance > (TANK_EMPTY_CM - 20)) {
            isTankEmpty = true;
            percentage = 0;
        } else {
            percentage = lastPercentage;
        }
        refillConfirmationCount = 0;
    } else {
        if (isTankEmpty) {
            if (++refillConfirmationCount >= REFILL_THRESHOLD) {
                isTankEmpty = false;
                lastValidDistance = distance;
                long p = map(lastValidDistance, TANK_FULL_CM, TANK_EMPTY_CM, 100, 0);
                percentage = (uint8_t)constrain(p, 0, 100);
                refillConfirmationCount = 0;
            } else {
                percentage = 0;
            }
        } else {
            lastValidDistance = distance;
            long p = map(lastValidDistance, TANK_FULL_CM, TANK_EMPTY_CM, 100, 0);
            percentage = (uint8_t)constrain(p, 0, 100);
            refillConfirmationCount = 0;
        }
    }
    lastPercentage = percentage;
    return percentage;
}

uint8_t sendData(uint8_t MessageType, uint8_t RepeaterNode, uint8_t tankID, uint8_t percentage) {
    LoRaPacket packet;
    packet.messageType = MessageType;
    packet.receiverID = RepeaterNode;
    packet.senderID = tankID;    
    packet.sourceNodeID = tankID;
    packet.value = percentage;
    packet.battery = 0.0;        

    uint8_t txBuffer[sizeof(LoRaPacket)];
    memcpy(txBuffer, &packet, sizeof(LoRaPacket));

    Serial.printf("Sending Data: Node=%d, %=%d\n", tankID, percentage);

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
    Serial.println("Data Node 6 (TaiFa) - Polling Mode Initializing...");

    SPI.begin();
    delay(500);

    pinMode(TRIGGER_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);
    pinMode(PRG_BUTTON, INPUT_PULLUP);
    
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

            // เช็คเฉพาะ receiverID เพราะ field นี้ถูก Repeater เขียนใหม่ตอน forward เท่านั้น
            if (packet.messageType == MSG_TYPE_POLL_REQ && packet.receiverID == NODE_ID) {
                Serial.println("Received POLL_REQ for Node 6 (TaiFa). Measuring...");
                uint8_t pct = measureAndCalculatePercentage();
                delay(50);
                sendData(MESSAGE_TYPE, REPEATER_NODE, NODE_ID, pct);
            }
        } else {
            LT.endReadSXBuffer();
        }
    }
}