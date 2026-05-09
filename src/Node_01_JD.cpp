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
const int TANK_FULL_CM = 40;
const int TANK_EMPTY_CM = 390;

const uint32_t Frequency = 923000000;           // Frequency of transmissions
const uint32_t Offset = 0;                      // Offset frequency for calibration purposes

const uint8_t Bandwidth = LORA_BW_125;          // LoRa bandwidth
const uint8_t SpreadingFactor = LORA_SF12;      // LoRa spreading factor
const uint8_t CodeRate = LORA_CR_4_5;           // LoRa coding rate
const uint8_t Optimisation = LDRO_AUTO;         // Low data rate optimisation setting

const int8_t TXpower = 2;                      // LoRa transmit power in dBm

#define MESSAGE_TYPE 1
#define REPEATER_NODE 101

SX126XLT LT;

// Initialize OLED Display and Button
static SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);
#define PRG_BUTTON 0

void VextON() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW); // Active LOW เพื่อเปิดไฟ
  Serial.println("Vext ON");
}

void VextOFF() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, HIGH); // Active LOW (HIGH = OFF)
  Serial.println("Vext OFF");
}

// ตัวแปรสำหรับ Logic ใหม่ (Layer 2: Sanity Check)
RTC_DATA_ATTR float lastValidDistance = TANK_FULL_CM; // เก็บค่าระยะทางที่ถูกต้องล่าสุด (เริ่มที่น้ำเต็ม)
RTC_DATA_ATTR bool isTankEmpty = false;       // สถานะว่าน้ำหมดหรือไม่
RTC_DATA_ATTR int refillConfirmationCount = 0; // ตัวนับเพื่อยืนยันเมื่อน้ำกลับมา
const int REFILL_THRESHOLD = 3;  // ต้องอ่านค่าได้กี่ครั้งถึงจะเชื่อว่าน้ำมาจริง

RTC_DATA_ATTR uint8_t lastPercentage = 100; // เก็บค่าเปอร์เซ็นต์ล่าสุด

// Forward declaration
float readDistance();

// Function to initialize LoRa module
bool initLoRa() {
    if (!LT.begin(NSS, NRESET, RFBUSY, DIO1, LORA_DEVICE)) {
        Serial.println("LoRa init failed!");
        return false;
    }
    Serial.println("LoRa init succeeded.");
    return true;
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

    float distance = readDistance();
    Serial.print("Manual Read Distance: "); Serial.println(distance);

    display.clear();
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 0, "Node 1: JD");
    display.setFont(ArialMT_Plain_24);

    if (distance < 0) {
        display.drawString(64, 20, "Error");
    } else {
        long calc_percentage = map(distance, TANK_FULL_CM, TANK_EMPTY_CM, 100, 0);
        calc_percentage = constrain(calc_percentage, 0, 100);
        uint8_t percentage = (uint8_t)calc_percentage;
        lastPercentage = percentage;
        display.drawString(64, 20, String(percentage) + " %");
    }

    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 50, "System Working");
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

// Function to read distance from sensor with timeout
float readDistance() {
    // Layer 1: Density-based Selection
    const int BURST_COUNT = 20;     // อ่าน 20 ครั้ง
    const int MAX_RETRIES = 3;      // ลองใหม่ได้ 3 รอบถ้าผลไม่ชัดเจน
    const float CLUSTER_RANGE = 5.0;// ระยะห่างที่ถือว่าเป็นกลุ่มเดียวกัน (cm)
    const int MIN_CLUSTER_SIZE = 5; // ต้องเกาะกลุ่มกันอย่างน้อย 5 ค่า

    Serial.println("Start reading distance (Density-based)...");

    for (int retry = 0; retry < MAX_RETRIES; retry++) {
        float validReadings[BURST_COUNT];
        int validCount = 0;
        int errorCount = 0;

        // 1. Burst Sampling
        for (int i = 0; i < BURST_COUNT; i++) {
            digitalWrite(TRIGGER_PIN, LOW);
            delayMicroseconds(2);
            digitalWrite(TRIGGER_PIN, HIGH);
            delayMicroseconds(10);
            digitalWrite(TRIGGER_PIN, LOW);
            
            long duration = pulseIn(ECHO_PIN, HIGH, 30000); // 30ms timeout
            float dist = (duration * 0.0343) / 2;
            
            // กรอง Noise ระยะสั้น (<25cm) หรือ Timeout (0) ให้เป็น Error
            if (duration == 0 || dist < 25.0) { 
                errorCount++;
            } else {
                validReadings[validCount++] = dist;
            }
            delay(20); // หน่วงเวลาเล็กน้อยระหว่างยิง
        }

        // 2. Analysis (ถ้า Error เยอะเกินไป และ Valid น้อยกว่าขั้นต่ำ)
        if (validCount < MIN_CLUSTER_SIZE) {
             Serial.print("Retry "); Serial.print(retry+1); 
             Serial.println(": Too many errors or not enough valid data.");
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

        Serial.print("Retry "); Serial.print(retry+1);
        Serial.print(" -> Error: "); Serial.print(errorCount);
        Serial.print(", ClusterSize: "); Serial.print(bestClusterSize);
        Serial.print(", Avg: "); Serial.println(bestClusterAvg);

        // 3. Decision
        // กลุ่มตัวเลขต้องมีขนาดใหญ่พอ และ ต้องชนะ Error (หรือสูสี)
        if (bestClusterSize >= MIN_CLUSTER_SIZE) {
            if (bestClusterSize >= errorCount) {
                return bestClusterAvg; // เชื่อมั่นในกลุ่มตัวเลข
            } else {
                Serial.println(" -> Error count dominates. Retrying...");
            }
        } else {
            Serial.println(" -> Cluster too small. Retrying...");
        }
    }

    return -1.0; // ถ้าลองครบแล้วยังไม่ได้ผล ให้คืนค่า Error
}

// Function to send data
uint8_t sendData(uint8_t MessageType, uint8_t RepeaterNode, uint8_t tankID, uint8_t percentage) {
    smartDelay(1000); // Use smartDelay to allow button check

    LoRaPacket packet;
    packet.messageType = MessageType;
    packet.receiverID = RepeaterNode;
    packet.senderID = tankID;    // สำหรับ Data Node, senderID กับ sourceNodeID คือตัวเดียวกัน
    packet.sourceNodeID = tankID;
    packet.value = percentage;
    packet.battery = 0.0;        // หากยังไม่ได้ต่อเซ็นเซอร์วัดแบต ให้ใส่ 0 ไว้ก่อน

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

void setup() {
    Serial.begin(115200);
    Serial.println("Data Node 1 (JD) Initializing...");

    SPI.begin();
    delay(500);

    pinMode(TRIGGER_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);
    pinMode(PRG_BUTTON, INPUT_PULLUP);
    
    // ตรวจสอบว่าตื่นเพราะกดปุ่มหรือไม่
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
        showStatus(); // แสดงสถานะแล้วทำงานต่อ
    }

    // เริ่มต้นกระบวนการวัดค่า (ย้ายจาก loop มาไว้ setup)
    VextON();
    float distance = readDistance();
    
    bool shouldSendZero = false;
    bool shouldSendValue = false;

    // Layer 2: Sanity Check & History
    if (distance < 0) {
        Serial.println("Result: Error (-1)");
        // ถ้าอ่านค่าไม่ได้ ให้ดูประวัติ (JD Empty is 390)
        if (lastValidDistance > (TANK_EMPTY_CM - 40)) {
            Serial.println("History says Empty. Confirming 0%.");
            isTankEmpty = true;
            shouldSendZero = true;
        } else {
            Serial.println("History says Full. Ignoring Error (Sensor Fault/Noise).");
        }
        refillConfirmationCount = 0; // รีเซ็ตตัวนับยืนยัน
    } else {
        Serial.print("Result: Valid ("); Serial.print(distance); Serial.println(" cm)");
        
        if (isTankEmpty) {
            // กรณีเดิมน้ำหมด ตอนนี้เริ่มอ่านค่าได้ (Refilling?)
            refillConfirmationCount++;
            Serial.print("Potential Refill Detected. Count: "); Serial.println(refillConfirmationCount);
            
            if (refillConfirmationCount >= REFILL_THRESHOLD) {
                Serial.println("Refill Confirmed!");
                isTankEmpty = false;
                lastValidDistance = distance;
                shouldSendValue = true;
                refillConfirmationCount = 0;
            } else {
                Serial.println("Waiting for confirmation...");
                shouldSendZero = true; // ยังคงส่ง 0% จนกว่าจะยืนยันครบ
            }
        } else {
            // กรณีปกติ (Normal Operation)
            lastValidDistance = distance;
            shouldSendValue = true;
            refillConfirmationCount = 0;
        }
    }

    // เริ่มต้น LoRa เฉพาะตอนจะส่งข้อมูล
    if (!initLoRa()) {
        Serial.println("Critical Error: LoRa initialization failed.");
    } else {
        LT.setupLoRa(Frequency, Offset, SpreadingFactor, Bandwidth, CodeRate, Optimisation);

    // Action based on Logic
    if (shouldSendZero) {
        lastPercentage = 0;
        sendData(MESSAGE_TYPE, REPEATER_NODE, 1, 0);
        int randomDelay1 = random(10000, 20000);  
        smartDelay(randomDelay1);
        sendData(MESSAGE_TYPE, REPEATER_NODE, 1, 0);
    } else if (shouldSendValue) {
        long calc_percentage = map(lastValidDistance, TANK_FULL_CM, TANK_EMPTY_CM, 100, 0); 
        calc_percentage = constrain(calc_percentage, 0, 100);
        uint8_t percentage = (uint8_t)calc_percentage;
        Serial.print("Calculated Percentage: "); Serial.println(percentage);
        lastPercentage = percentage;
        Serial.println("Sending Packet 1...");
        sendData(MESSAGE_TYPE, REPEATER_NODE, 1, percentage);
        int randomDelay1 = random(10000, 20000);  
        Serial.print("Waiting random delay: "); Serial.println(randomDelay1);
        smartDelay(randomDelay1);
        Serial.println("Sending Packet 2...");
        sendData(MESSAGE_TYPE, REPEATER_NODE, 1, percentage);
    }
    } // End LoRa block

    VextOFF();
    
    // เตรียมตัวหลับ (Deep Sleep)
    LT.setSleep(CONFIGURATION_RETENTION);   // Preserve register settings in sleep.
    Serial.println("LoRa set to Sleep");
    Serial.println("Entering Deep Sleep (5 mins)...");
    Serial.flush();

    // ตั้งค่าการตื่น
    // 1. ตื่นด้วยปุ่มกด (GPIO 0 เป็น LOW)
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); 
    
    // 2. ตื่นด้วยเวลา (5 นาที + สุ่มเล็กน้อยเพื่อกันชนกัน)
    uint64_t sleepTime = 300000ULL * 1000ULL; // 5 นาที (หน่วยไมโครวินาที)
    sleepTime += random(1000, 30000) * 1000ULL; // เพิ่ม Random Jitter 1-30 วินาที
    esp_sleep_enable_timer_wakeup(sleepTime);

    esp_deep_sleep_start(); // หลับยาว
}

void loop() {}