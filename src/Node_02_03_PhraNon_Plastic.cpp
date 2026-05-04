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
#define TRIGGER_PIN_1 6
#define ECHO_PIN_1 7

// Define second sensor pin
#define TRIGGER_PIN_2 4 // กรุณาเลือก GPIO ที่เหมาะสม
#define ECHO_PIN_2 5    // กรุณาเลือก GPIO ที่เหมาะสม

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
#define REPEATER_NODE 10

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

        // Sort valid readings (Bubble Sort)
        for (int i = 0; i < validCount - 1; i++) {
            for (int j = 0; j < validCount - i - 1; j++) {
                if (validReadings[j] > validReadings[j + 1]) {
                    float temp = validReadings[j];
                    validReadings[j] = validReadings[j + 1];
                    validReadings[j + 1] = temp;
                }
            }
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
                    break; // หลุดช่วงแล้ว หยุดเช็ค
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
                Serial.print("Valid Dist: "); Serial.println(bestClusterAvg);
                return bestClusterAvg; // เชื่อมั่นในกลุ่มตัวเลข
            }
        }
    }

    return -1.0; // ถ้าลองครบแล้วยังไม่ได้ผล ให้คืนค่า Error
}

// Function to send data
uint8_t sendData(uint8_t MessageType, uint8_t RepeaterNode, uint8_t tankID, uint8_t percentage) {
    delay(1000);

    LoRaPacket packet;
    packet.messageType = MessageType;
    packet.receiverID = RepeaterNode;
    packet.senderID = 2; // Node ID หลักคือ 2
    packet.sourceNodeID = tankID; // แต่ sourceNodeID เปลี่ยนตามแทงก์ (2 หรือ 3)
    packet.value = percentage;
    packet.battery = 0.0;

    uint8_t txBuffer[sizeof(LoRaPacket)];
    memcpy(txBuffer, &packet, sizeof(LoRaPacket)); // Copy struct to byte array

    Serial.print("Sending Data: TankID="); Serial.print(tankID); Serial.print(", %="); Serial.println(percentage);

    LT.startWriteSXBuffer(0);
    for (int i = 0; i < sizeof(LoRaPacket); i++) {
        LT.writeUint8(txBuffer[i]);
    }
    uint8_t len = LT.endWriteSXBuffer();

    return LT.transmitSXBuffer(0, (len + 2), 5000, TXpower, WAIT_TX);
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

// Smart delay function that checks for button press
void smartDelay(unsigned long ms) {
    unsigned long start = millis();
    while (millis() - start < ms) {
        if (digitalRead(PRG_BUTTON) == LOW) { // Button pressed (Active Low)
            showStatus();
            while(digitalRead(PRG_BUTTON) == LOW) delay(10); // Wait for release
        }
        delay(50);
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("Data Node 2 (PhraNon) Initializing...");

    SPI.begin();
    delay(500);

    pinMode(TRIGGER_PIN_1, OUTPUT);
    pinMode(ECHO_PIN_1, INPUT);

    pinMode(TRIGGER_PIN_2, OUTPUT);
    pinMode(ECHO_PIN_2, INPUT);
    pinMode(PRG_BUTTON, INPUT_PULLUP);
    
    // ตรวจสอบว่าตื่นเพราะกดปุ่มหรือไม่
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
        showStatus(); // แสดงสถานะแล้วทำงานต่อ
    }

    VextON();
    delay(50);
    
    // ==========================================
    // --- Sensor 1 (Tank ID 2) Logic ---
    // ==========================================
    float distance1 = readDistance(TRIGGER_PIN_1, ECHO_PIN_1);
    bool shouldSendZero1 = false;
    bool shouldSendValue1 = false;

    if (distance1 < 0) {
        Serial.println("Error reading distance from Sensor 1.");
        
        // กรณีน้ำหมด: หากระยะล่าสุดเกิน 410cm แล้ววัดไม่ได้ ให้ถือว่าน้ำหมดจริง (หลุดระยะเซนเซอร์)
        if (lastValidDistance1 > (TANK2_EMPTY_CM - 40)) { 
            isTankEmpty1 = true;
            shouldSendZero1 = true;
        }
        // กรณีน้ำเต็ม (Blind Spot): หากระยะล่าสุดน้อยกว่า 30cm แล้ววัดไม่ได้ ให้ถือว่าน้ำเต็มจนบังเซนเซอร์
        else if (lastValidDistance1 < (TANK2_FULL_CM + 5)) {
            Serial.println("Likely Blind Spot for Tank 2. Sending 100%.");
            shouldSendValue1 = true; 
        }

        refillCount1 = 0;
    } else {
        if (isTankEmpty1) {
            refillCount1++;
            if (refillCount1 >= REFILL_THRESHOLD) {
                isTankEmpty1 = false;
                lastValidDistance1 = distance1;
                shouldSendValue1 = true;
                refillCount1 = 0;
            } else {
                shouldSendZero1 = true; // ยังไม่ยืนยัน ส่ง 0 ไปก่อน
            }
        } else {
            lastValidDistance1 = distance1;
            shouldSendValue1 = true;
            refillCount1 = 0;
        }
    }


    // ==========================================
    // --- Sensor 2 (Tank ID 3) Logic ---
    // ==========================================
    float distance2 = readDistance(TRIGGER_PIN_2, ECHO_PIN_2);
    bool shouldSendZero2 = false;
    bool shouldSendValue2 = false;

    if (distance2 < 0) {
        Serial.println("Error reading distance from Sensor 2.");
        // ถ้าอ่านค่าไม่ได้ ให้ดูประวัติ (Tank 3 Empty ~240)
        if (lastValidDistance2 > (TANK3_EMPTY_CM - 10)) { // > 230cm
            isTankEmpty2 = true;
            shouldSendZero2 = true;
        }
        refillCount2 = 0;
    } else {
        if (isTankEmpty2) {
            refillCount2++;
            if (refillCount2 >= REFILL_THRESHOLD) {
                isTankEmpty2 = false;
                lastValidDistance2 = distance2;
                shouldSendValue2 = true;
                refillCount2 = 0;
            } else {
                shouldSendZero2 = true;
            }
        } else {
            lastValidDistance2 = distance2;
            shouldSendValue2 = true;
            refillCount2 = 0;
        }
    }

    // เริ่มต้น LoRa เฉพาะตอนจะส่งข้อมูล
    if (!initLoRa()) {
        Serial.println("Critical Error: LoRa initialization failed.");
    } else {
        LT.setupLoRa(Frequency, Offset, SpreadingFactor, Bandwidth, CodeRate, Optimisation);

        // --- Sending Data for Sensor 1 ---
        if (shouldSendZero1) {
            sendData(MESSAGE_TYPE, REPEATER_NODE, TANK2_ID, 0);
            smartDelay(random(10000, 20000));
            sendData(MESSAGE_TYPE, REPEATER_NODE, TANK2_ID, 0);
        } else if (shouldSendValue1) {
            uint8_t percentage1 = map(lastValidDistance1, TANK2_FULL_CM, TANK2_EMPTY_CM, 100, 0);
            percentage1 = constrain(percentage1, 0, 100);
            sendData(MESSAGE_TYPE, REPEATER_NODE, TANK2_ID, percentage1);
            smartDelay(random(10000, 20000));
            sendData(MESSAGE_TYPE, REPEATER_NODE, TANK2_ID, percentage1);
        }

        if (shouldSendZero1 || shouldSendValue1) {
             smartDelay(2000); // เว้นระยะนิดหน่อยถ้ามีการส่งข้อมูลชุดแรก
        }

        // --- Sending Data for Sensor 2 ---
        if (shouldSendZero2) {
            sendData(MESSAGE_TYPE, REPEATER_NODE, TANK3_ID, 0);
            smartDelay(random(10000, 20000));
            sendData(MESSAGE_TYPE, REPEATER_NODE, TANK3_ID, 0);
        } else if (shouldSendValue2) {
            uint8_t percentage2 = map(lastValidDistance2, TANK3_FULL_CM, TANK3_EMPTY_CM, 100, 0);
            percentage2 = constrain(percentage2, 0, 100);
            sendData(MESSAGE_TYPE, REPEATER_NODE, TANK3_ID, percentage2);
            smartDelay(random(10000, 20000));
            sendData(MESSAGE_TYPE, REPEATER_NODE, TANK3_ID, percentage2);
        }
    }

    VextOFF();
    
    // เตรียมตัวหลับ (Deep Sleep)
    LT.setSleep(CONFIGURATION_RETENTION);   // Preserve register settings in sleep.
    Serial.println("Entering Deep Sleep (5 mins)...");
    Serial.flush();

    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); // ตื่นด้วยปุ่มกด
    
    uint64_t sleepTime = 300000ULL * 1000ULL; // 5 นาที
    sleepTime += random(1000, 30000) * 1000ULL; // Random Jitter
    esp_sleep_enable_timer_wakeup(sleepTime);

    esp_deep_sleep_start(); // หลับยาว
}

void loop() {}