// Data Node 3 (ThamYai)
#include <Arduino.h>
#include <SPI.h>  
#include <Wire.h>
#include <HT_SSD1306Wire.h>
#include <SX126XLT.h>
#include <ProgramLT_Definitions.h>
#include <esp_sleep.h> // เรียกใช้ Library สำหรับ Deep Sleep
#include "NodeConfig.h"

// Define LoRa settings
#define LORA_DEVICE DEVICE_SX1262
#define NSS 8
#define NRESET 12
#define RFBUSY 13
#define DIO1 14

// Define sensor pin
#define TRIGGER_PIN 6
#define ECHO_PIN 7

// Battery Measurement Pin (Heltec V3)
#define ADC_BATTERY_PIN 1
#define BAT_ADC_CTRL 37 // ขาควบคุมวงจรวัดแบตเตอรี่สำหรับ V3 (ตาม Pinout Map)

// Define Tank Constants (Calibration)
const int TANK_FULL_CM = 42;
const int TANK_EMPTY_CM = 700;   // ความสูงจริงของแทงก์ (เซ็นเซอร์ถึงก้นแทงก์ 7 เมตร)
const int SENSOR_LIMIT_CM = 450; // ขีดจำกัดทางกายภาพของเซ็นเซอร์ JSN-SR04T (4.5 เมตร)

const uint32_t Frequency = 923000000;           // Frequency of transmissions
const uint32_t Offset = 0;                      // Offset frequency for calibration purposes

const uint8_t Bandwidth = LORA_BW_125;          // LoRa bandwidth
const uint8_t SpreadingFactor = LORA_SF12;      // LoRa spreading factor
const uint8_t CodeRate = LORA_CR_4_5;           // LoRa coding rate
const uint8_t Optimisation = LDRO_AUTO;         // Low data rate optimisation setting

const int8_t TXpower = 14;                      // ปรับความแรงสูงสุด (22 dBm) เนื่องจากอยู่ไกลมาก

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
RTC_DATA_ATTR float lastValidDistance = TANK_FULL_CM; // ใช้ RTC_DATA_ATTR เพื่อจำค่าตอน Deep Sleep
RTC_DATA_ATTR bool isTankEmpty = false;       
RTC_DATA_ATTR int refillConfirmationCount = 0; 
const int REFILL_THRESHOLD = 3;  // ต้องอ่านค่าได้กี่ครั้งถึงจะเชื่อว่าน้ำมาจริง

RTC_DATA_ATTR uint8_t lastPercentage = 100; // ใช้ RTC_DATA_ATTR เพื่อจำค่าตอน Deep Sleep
RTC_DATA_ATTR bool hasSentInitialZero = false; // ส่ง 0% ครั้งแรกเมื่อยังไม่มีประวัติ (ป้องกัน Comm Error)
float globalBatteryVoltage = 0.0; // เก็บค่าแบตเตอรี่ที่วัดได้ตอนเริ่มระบบ

// Forward declaration
float readDistance(bool enforceSensorLimit = true);
float readBatteryVoltage();

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

    float d = readDistance(false);
    Serial.print("Manual Read Distance: "); Serial.println(d);

    display.clear();
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 0, "Node 4: ThamYai");

    // ใช้ค่าแบตเตอรี่ที่วัดไว้ตอนเริ่ม setup
    display.setFont(ArialMT_Plain_16); // เปลี่ยน Font สำหรับแสดงแบตเตอรี่
    display.drawString(64, 40, "Bat: " + String(globalBatteryVoltage, 2) + "V"); 

    // แสดงสถานะระดับน้ำ
    if (d < 0) {
        display.drawString(64, 20, "Error");
    } else {
        long calc_percentage = map(d, TANK_FULL_CM, TANK_EMPTY_CM, 100, 0);
        calc_percentage = constrain(calc_percentage, 0, 100);
        uint8_t percentage = (uint8_t)calc_percentage;
        lastPercentage = percentage;
        display.drawString(64, 20, String(percentage) + " %"); // แสดงเปอร์เซ็นต์น้ำ
    }

    display.setFont(ArialMT_Plain_10); // กลับไปใช้ Font เล็กสำหรับข้อความสถานะ
    display.drawString(64, 55, "System Working"); // แสดง System Working ด้านล่างสุด
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
float readDistance(bool enforceSensorLimit) {
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
            
            // กรอง Noise ระยะสั้น (<25cm) หรือไกลเกินขีดจำกัด (450cm) หรือ Timeout (0) ให้เป็น Error
            if (duration == 0 || dist < 25.0 || (enforceSensorLimit && dist > SENSOR_LIMIT_CM)) { 
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

// Function to read battery voltage
float readBatteryVoltage() {
    // เปิดวงจรวัดแบตเตอรี่ (GPIO 37 ต้องเป็น HIGH เพื่อให้กระแสไหลผ่านวงจรแบ่งแรงดัน)
    pinMode(BAT_ADC_CTRL, OUTPUT);
    digitalWrite(BAT_ADC_CTRL, HIGH); 
    delay(10); // รอให้แรงดันไฟนิ่ง

    uint16_t raw = analogRead(ADC_BATTERY_PIN);

    // ปิดวงจรวัดแบตเตอรี่ทันทีเพื่อประหยัดไฟ
    digitalWrite(BAT_ADC_CTRL, LOW); 

    Serial.print("Raw ADC: "); Serial.println(raw);
    float batteryVoltage = raw * (3.3 / 4095.0) * 4.9; 
    Serial.print("Calculated Battery Voltage: "); Serial.println(batteryVoltage, 2);

    // สูตรคำนวณ: ค่าดิบ * (แรงดันอ้างอิง / ความละเอียด) * อัตราส่วนตัวต้านทาน (4.9 สำหรับ V3)
    return batteryVoltage;
}

// Function to send data
uint8_t sendData(uint8_t MessageType, uint8_t RepeaterNode, uint8_t tankID, uint8_t percentage) {
    smartDelay(1000); // Use smartDelay to allow button check

    LoRaPacket packet;
    packet.messageType = MessageType;
    packet.receiverID = RepeaterNode;
    packet.senderID = tankID;
    packet.sourceNodeID = tankID;
    packet.value = percentage;
    packet.battery = globalBatteryVoltage; // ใช้ค่าแบตเตอรี่ที่วัดไว้ตอนเริ่ม

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
    Serial.println("Data Node 3 (ThamYai) Initializing...");

    // 1. วัดแบตเตอรี่ทันทีที่ตื่น (ก่อนโหลดหนักอย่าง OLED/LoRa จะทำงาน)
    globalBatteryVoltage = readBatteryVoltage();
    Serial.print("Initial Battery Voltage: "); Serial.println(globalBatteryVoltage, 2);

    SPI.begin();
    delay(500);

    pinMode(TRIGGER_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);
    pinMode(PRG_BUTTON, INPUT_PULLUP);
    
    // ตรวจสอบว่าตื่นเพราะกดปุ่มหรือไม่
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
        showStatus(); // แสดงสถานะแล้วทำงานต่อ (วัดค่าและส่งข้อมูลอัปเดตทันที)
    }

    // เริ่มต้นกระบวนการวัดค่า (ย้ายจาก loop มาไว้ setup)
    VextON();
    float distance = readDistance();
    
    bool shouldSendZero = false;
    bool shouldSendValue = false;

    // Layer 2: Sanity Check & History
    if (distance < 0) {
        Serial.println("Result: Error (-1)");
        // ถ้าเดิมถูกมาร์คว่า Empty อยู่แล้ว ให้ยังส่ง 0% ต่อไป
        if (isTankEmpty) {
            Serial.println("Previously marked empty — keep sending 0%.");
            shouldSendZero = true;
        }
        // ถ้าอ่านค่าไม่ได้ ให้พิจารณาตามประวัติ
        // กรณีที่ยังไม่มีประวัติจริง (ค่าเริ่มต้นยังเป็น TANK_FULL_CM) และเป็น Error ครั้งแรก
        // ให้ส่ง 0% ครั้งแรกเพื่อไม่ให้ Master ตีเป็น Comm Error
        else if (!hasSentInitialZero && lastValidDistance == TANK_FULL_CM) {
            Serial.println("No history and first read error. Sending initial 0% to indicate empty.");
            isTankEmpty = true;
            shouldSendZero = true;
            hasSentInitialZero = true;
        }
        // กรณีที่ประวัติระบุว่าน้ำอยู่ต่ำกว่าช่วงเซ็นเซอร์ -> ส่ง 0%
        else if (lastValidDistance > (SENSOR_LIMIT_CM - 30)) { 
            Serial.println("Water below sensor range (> 4.5m). Reporting 0% for safety.");
            isTankEmpty = true;
            shouldSendZero = true;
        } else {
            Serial.println("History says Full. Ignoring Error (Sensor Fault/Noise).");
            // ไม่ส่งค่า ปล่อยให้ Master แจ้งเตือน Comm Error ถ้าเป็นนานๆ
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
        // ถ้า LoRa พัง ให้หลับไปก่อนแล้วค่อยตื่นมาลองใหม่
    } else {
        LT.setupLoRa(Frequency, Offset, SpreadingFactor, Bandwidth, CodeRate, Optimisation);

    // Action based on Logic
    if (shouldSendZero) {
        lastPercentage = 0;
        sendData(MESSAGE_TYPE, REPEATER_NODE, 4, 0);
        int randomDelay1 = random(10000, 20000);
        smartDelay(randomDelay1);
        sendData(MESSAGE_TYPE, REPEATER_NODE, 4, 0);
    } else if (shouldSendValue) {
        long calc_percentage = map(lastValidDistance, TANK_FULL_CM, TANK_EMPTY_CM, 100, 0); 
        calc_percentage = constrain(calc_percentage, 0, 100);   // Constrain between 0 and 100
        uint8_t percentage = (uint8_t)calc_percentage;          // Cast to uint8_t
        Serial.print("Calculated Percentage: "); Serial.println(percentage);
        lastPercentage = percentage;
        Serial.println("Sending Packet 1...");
        sendData(MESSAGE_TYPE, REPEATER_NODE, 4, percentage);
        int randomDelay1 = random(10000, 20000);  
        Serial.print("Waiting random delay: "); Serial.println(randomDelay1);
        smartDelay(randomDelay1);
        Serial.println("Sending Packet 2...");
        sendData(MESSAGE_TYPE, REPEATER_NODE, 4, percentage);
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

void loop() {
    // ไม่ต้องทำอะไรใน loop เพราะ Deep Sleep จะรีเซ็ตกลับไป setup ใหม่เสมอ
}
