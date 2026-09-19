// Data Node 2 (PhraNon & Plastic)
// รองรับเซ็นเซอร์ 2 ชุด: ชุดที่ 1 คือแทงก์พระนอน (ID 2) และชุดที่ 2 คือแทงก์พลาสติก (ID 3)
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

// Define sensor pin
#define TRIGGER_PIN_1 46
#define ECHO_PIN_1 45

// Define second sensor pin
#define TRIGGER_PIN_2 47 // กรุณาเลือก GPIO ที่เหมาะสม
#define ECHO_PIN_2 48    // กรุณาเลือก GPIO ที่เหมาะสม

// Define Tank Constants (Calibration)
// Tank 2: สูงจริง 5m, ระยะเต็ม 25cm, ระยะน้ำหมดอ้างอิงที่ 450cm (เพื่อให้ค่า % ตรงกับความเป็นจริง)
const int TANK2_ID = 2;
const int TANK2_FULL_CM = 25;
const int TANK2_EMPTY_CM = 450;

const int TANK3_ID = 3;
const int TANK3_FULL_CM = 48; // ปรับค่าใหม่จาก 35 (คำนวณจาก 95%)
const int TANK3_EMPTY_CM = 240;

const uint32_t Frequency = 923000000;           // Frequency of transmissions
const uint32_t Offset = 0;                      // Offset frequency for calibration purposes

const uint8_t Bandwidth = LORA_BW_125;          // LoRa bandwidth
const uint8_t SpreadingFactor = LORA_SF12;      // LoRa spreading factor
const uint8_t CodeRate = LORA_CR_4_5;           // LoRa coding rate
const uint8_t Optimisation = LDRO_AUTO;         // Low data rate optimisation setting

const int8_t TXpower = 14;                      // LoRa transmit power in dBm

#define MESSAGE_TYPE 1
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

// ตัวแปรสำหรับ Logic ใหม่ (Layer 2: Sanity Check) แยกตามถัง
RTC_DATA_ATTR float lastValidDistance1 = 25.0; // Tank 2 (เริ่มที่น้ำเต็ม)
RTC_DATA_ATTR bool isTankEmpty1 = false;
RTC_DATA_ATTR int refillCount1 = 0;

RTC_DATA_ATTR float lastValidDistance2 = 48.0; // Tank 8 (เริ่มที่น้ำเต็ม)
RTC_DATA_ATTR bool isTankEmpty2 = false;
RTC_DATA_ATTR int refillCount2 = 0;

const int REFILL_THRESHOLD = 3;  // ต้องอ่านค่าได้กี่ครั้งถึงจะเชื่อว่าน้ำมาจริง

// Function to initialize LoRa module
bool initLoRa() {
    if (!LT.begin(NSS, NRESET, RFBUSY, DIO1, LORA_DEVICE)) {
        Serial.println("LoRa init failed!");
        return false;
    }
    Serial.println("LoRa init succeeded.");
    return true;
}

// Function to read distance from sensor (Density-based Selection from Node 3)
float readDistance(uint8_t triggerPin, uint8_t echoPin) {
    const int BURST_COUNT = 20;     // อ่าน 20 ครั้ง
    const int MAX_RETRIES = 3;      // ลองใหม่ได้ 3 รอบถ้าผลไม่ชัดเจน
    const float CLUSTER_RANGE = 5.0;// ระยะห่างที่ถือว่าเป็นกลุ่มเดียวกัน (cm)
    const int MIN_CLUSTER_SIZE = 5; // ต้องเกาะกลุ่มกันอย่างน้อย 5 ค่า

    for (int retry = 0; retry < MAX_RETRIES; retry++) {
        float validReadings[BURST_COUNT];
        int validCount = 0;
        int errorCount = 0;

        // 1. Burst Sampling
        for (int i = 0; i < BURST_COUNT; i++) {
            digitalWrite(triggerPin, LOW);
            delayMicroseconds(2);
            digitalWrite(triggerPin, HIGH);
            delayMicroseconds(10);
            digitalWrite(triggerPin, LOW);
            
            long duration = pulseIn(echoPin, HIGH, 30000); // 30ms timeout
            float dist = (duration * 0.0343) / 2;
            
            // ปรับระยะการกรอง Noise ให้เหมาะสมกับระยะเต็มจริงที่ 25cm
            // ใช้ 22.0 เพื่อให้มีระยะเผื่อนิดหน่อยก่อนถึงระยะเต็ม (25.0)
            if (duration == 0 || dist < 22.0) { 
                errorCount++;
            } else {
                validReadings[validCount++] = dist;
            }
            delay(20); // หน่วงเวลาเล็กน้อยระหว่างยิง
        }

        // 2. Analysis
        if (validCount < MIN_CLUSTER_SIZE) {
             if (retry == MAX_RETRIES - 1) return -1.0; // ยอมแพ้
             continue; // ลองใหม่
        }

        // Find Best Cluster (Density)
        int bestClusterSize = 0;
        float bestClusterAvg = 0;

        for (int i = 0; i < validCount; i++) {
            int count = 0;
            float sum = 0;
            for (int j = i; j < validCount; j++) {
                if (validReadings[j] - validReadings[i] <= CLUSTER_RANGE) {
                    count++;
                    sum += validReadings[j];
                } else {
                    break;
                }
            }
            if (count > bestClusterSize) {
                bestClusterSize = count;
                bestClusterAvg = sum / count;
            }
        }

        // 3. Decision
        if (bestClusterSize >= MIN_CLUSTER_SIZE) {
            if (bestClusterSize >= errorCount) {
                return bestClusterAvg;
            }
        }
    }

    return -1.0;
}

// Function to send data
uint8_t sendData(uint8_t MessageType, uint8_t RepeaterNode, uint8_t tankID, uint8_t percentage) {
    LoRaPacket packet;
    packet.messageType = MessageType;
    packet.receiverID = RepeaterNode;
    packet.senderID = 2; // Node ID หลักคือ 2
    packet.sourceNodeID = tankID; // แต่ sourceNodeID เปลี่ยนตามแทงก์ (2 หรือ 3)
    packet.value = percentage;
    packet.battery = 0.0;

    uint8_t txBuffer[sizeof(LoRaPacket)];
    memcpy(txBuffer, &packet, sizeof(LoRaPacket));

    Serial.print("Sending Data: TankID="); Serial.print(tankID); Serial.print(", %="); Serial.println(percentage);

    LT.startWriteSXBuffer(0);
    for (int i = 0; i < sizeof(LoRaPacket); i++) {
        LT.writeUint8(txBuffer[i]);
    }
    uint8_t len = LT.endWriteSXBuffer();

    return LT.transmitSXBuffer(0, (len + 2), 5000, TXpower, WAIT_TX);
}

// Function to measure and compute percentage for Tank 2 (PhraNon)
uint8_t measureTank2() {
    VextON();
    float distance1 = readDistance(TRIGGER_PIN_1, ECHO_PIN_1);
    VextOFF();

    uint8_t percentage1 = 0;

    if (distance1 < 0) {
        Serial.println("Error reading distance from Sensor 1 (Tank 2).");
        if (lastValidDistance1 > (TANK2_EMPTY_CM - 40)) { 
            isTankEmpty1 = true;
            percentage1 = 0;
        } else if (lastValidDistance1 < (TANK2_FULL_CM + 5)) {
            Serial.println("Likely Blind Spot for Tank 2. Sending 100%.");
            percentage1 = 100;
        }
        refillCount1 = 0;
    } else {
        if (isTankEmpty1) {
            refillCount1++;
            if (refillCount1 >= REFILL_THRESHOLD) {
                isTankEmpty1 = false;
                lastValidDistance1 = distance1;
                long calc = map(lastValidDistance1, TANK2_FULL_CM, TANK2_EMPTY_CM, 100, 0);
                percentage1 = (uint8_t)constrain(calc, 0, 100);
                refillCount1 = 0;
            } else {
                percentage1 = 0;
            }
        } else {
            lastValidDistance1 = distance1;
            long calc = map(lastValidDistance1, TANK2_FULL_CM, TANK2_EMPTY_CM, 100, 0);
            percentage1 = (uint8_t)constrain(calc, 0, 100);
            refillCount1 = 0;
        }
    }
    return percentage1;
}

// Function to measure and compute percentage for Tank 3 (Plastic)
uint8_t measureTank3() {
    VextON();
    float distance2 = readDistance(TRIGGER_PIN_2, ECHO_PIN_2);
    VextOFF();

    uint8_t percentage2 = 0;

    if (distance2 < 0) {
        Serial.println("Error reading distance from Sensor 2 (Tank 3).");
        if (lastValidDistance2 > (TANK3_EMPTY_CM - 10)) {
            isTankEmpty2 = true;
            percentage2 = 0;
        }
        refillCount2 = 0;
    } else {
        if (isTankEmpty2) {
            refillCount2++;
            if (refillCount2 >= REFILL_THRESHOLD) {
                isTankEmpty2 = false;
                lastValidDistance2 = distance2;
                long calc = map(lastValidDistance2, TANK3_FULL_CM, TANK3_EMPTY_CM, 100, 0);
                percentage2 = (uint8_t)constrain(calc, 0, 100);
                refillCount2 = 0;
            } else {
                percentage2 = 0;
            }
        } else {
            lastValidDistance2 = distance2;
            long calc = map(lastValidDistance2, TANK3_FULL_CM, TANK3_EMPTY_CM, 100, 0);
            percentage2 = (uint8_t)constrain(calc, 0, 100);
            refillCount2 = 0;
        }
    }
    return percentage2;
}

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
    display.drawString(64, 20, "Measuring...");
    display.display();

    float d1 = readDistance(TRIGGER_PIN_1, ECHO_PIN_1);
    float d2 = readDistance(TRIGGER_PIN_2, ECHO_PIN_2);

    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 0, "Node 2: PhraNon");
    
    // Tank 2 Logic
    String t2Str = "T2: Err";
    if (d1 >= 0) {
        long p1 = map(d1, TANK2_FULL_CM, TANK2_EMPTY_CM, 100, 0);
        p1 = constrain(p1, 0, 100);
        t2Str = "T2: " + String(p1) + "%";
    }
    
    // Tank 3 Logic
    String t3Str = "T3: Err";
    if (d2 >= 0) {
        long p2 = map(d2, TANK3_FULL_CM, TANK3_EMPTY_CM, 100, 0);
        p2 = constrain(p2, 0, 100);
        t3Str = "T3: " + String(p2) + "%";
    }

    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 20, t2Str);
    display.drawString(64, 45, t3Str);

    display.display();
    delay(3000); // Show for 3 seconds
    display.clear();
    display.display();
    display.displayOff();
    VextOFF();
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
    Serial.println("Data Node 2 (PhraNon & Plastic) - Polling Mode Initializing...");

    SPI.begin();
    delay(500);

    pinMode(TRIGGER_PIN_1, OUTPUT);
    pinMode(ECHO_PIN_1, INPUT);

    pinMode(TRIGGER_PIN_2, OUTPUT);
    pinMode(ECHO_PIN_2, INPUT);
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
            if (packet.messageType == MSG_TYPE_POLL_REQ) {
                // Request for Tank 2 (PhraNon)
                if (packet.receiverID == TANK2_ID) {
                    Serial.println("Received POLL_REQ for Tank 2 (PhraNon). Measuring...");
                    uint8_t pct = measureTank2();
                    delay(50);
                    sendData(MESSAGE_TYPE, REPEATER_NODE, TANK2_ID, pct);
                }
                // Request for Tank 3 (Plastic)
                else if (packet.receiverID == TANK3_ID) {
                    Serial.println("Received POLL_REQ for Tank 3 (Plastic). Measuring...");
                    uint8_t pct = measureTank3();
                    delay(50);
                    sendData(MESSAGE_TYPE, REPEATER_NODE, TANK3_ID, pct);
                }
            }
        } else {
            LT.endReadSXBuffer();
        }
    }
}