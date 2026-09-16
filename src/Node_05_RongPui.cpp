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
#define TRIGGER_PIN 47
#define ECHO_PIN 48

// Battery Measurement Pin (Heltec V3)
#define ADC_BATTERY_PIN 1
#define BAT_ADC_CTRL 37 // ขาควบคุมวงจรวัดแบตเตอรี่สำหรับ V3 (ตาม Pinout Map)

// Define Tank Constants (Calibration)
const int TANK_FULL_CM = 82;
const int TANK_EMPTY_CM = 500;   // ความสูงจริงของแทงก์ (เซ็นเซอร์ถึงก้นแทงก์ 5 เมตร)
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
#define AUTO_RESET_INTERVAL_MS 43200000UL // 12 ชั่วโมง ป้องกัน heap fragmentation สะสมจากการรันต่อเนื่องเป็นเวลานาน

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
float readDistance();
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

    float d = readDistance();
    Serial.print("Manual Read Distance: "); Serial.println(d);

    display.clear();
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 0, "Node 5: RongPui");

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
            
            // กรอง Noise ระยะสั้น (<25cm) หรือไกลเกินขีดจำกัด (450cm) หรือ Timeout (0) ให้เป็น Error
            if (duration == 0 || dist < 25.0 || dist > SENSOR_LIMIT_CM) { 
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

// Function to measure water level and compute percentage
uint8_t measureAndCalculatePercentage() {
    VextON();
    float distance = readDistance();
    VextOFF();

    uint8_t percentage = 0;

    // Layer 2: Sanity Check & History
    if (distance < 0) {
        Serial.println("Result: Error (-1)");
        if (isTankEmpty) {
            Serial.println("Previously marked empty — keep sending 0%.");
            percentage = 0;
        } else if (!hasSentInitialZero && lastValidDistance == TANK_FULL_CM) {
            Serial.println("No history and first read error. Sending initial 0%.");
            isTankEmpty = true;
            percentage = 0;
            hasSentInitialZero = true;
        } else if (lastValidDistance > (SENSOR_LIMIT_CM - 30)) { 
            Serial.println("Water below sensor range (> 4.5m). Reporting 0%.");
            isTankEmpty = true;
            percentage = 0;
        } else {
            Serial.println("History says Full. Using last valid percentage.");
            percentage = lastPercentage;
        }
        refillConfirmationCount = 0;
    } else {
        Serial.print("Result: Valid ("); Serial.print(distance); Serial.println(" cm)");
        
        if (isTankEmpty) {
            refillConfirmationCount++;
            Serial.print("Potential Refill Detected. Count: "); Serial.println(refillConfirmationCount);
            
            if (refillConfirmationCount >= REFILL_THRESHOLD) {
                Serial.println("Refill Confirmed!");
                isTankEmpty = false;
                lastValidDistance = distance;
                long calc_percentage = map(lastValidDistance, TANK_FULL_CM, TANK_EMPTY_CM, 100, 0); 
                percentage = (uint8_t)constrain(calc_percentage, 0, 100);
                refillConfirmationCount = 0;
            } else {
                Serial.println("Waiting for confirmation...");
                percentage = 0;
            }
        } else {
            lastValidDistance = distance;
            long calc_percentage = map(lastValidDistance, TANK_FULL_CM, TANK_EMPTY_CM, 100, 0); 
            percentage = (uint8_t)constrain(calc_percentage, 0, 100);
            refillConfirmationCount = 0;
        }
    }

    lastPercentage = percentage;
    return percentage;
}

// Function to send data
uint8_t sendData(uint8_t MessageType, uint8_t RepeaterNode, uint8_t tankID, uint8_t percentage) {
    LoRaPacket packet;
    packet.messageType = MessageType;
    packet.receiverID = RepeaterNode;
    packet.senderID = tankID;
    packet.sourceNodeID = tankID;
    packet.value = percentage;
    packet.battery = globalBatteryVoltage;

    uint8_t txBuffer[sizeof(LoRaPacket)];
    memcpy(txBuffer, &packet, sizeof(LoRaPacket));

    Serial.printf("Sending Data: TankID=%d, %=%d, Bat=%.2fV\n", tankID, percentage, globalBatteryVoltage);
    
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
            globalBatteryVoltage = readBatteryVoltage();
            showStatus();
            while (digitalRead(PRG_BUTTON) == LOW) vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("Data Node 5 (RongPui) - Polling Mode Initializing...");

    SPI.begin();
    delay(500);

    pinMode(TRIGGER_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);
    pinMode(PRG_BUTTON, INPUT_PULLUP);
    
    VextOFF();

    globalBatteryVoltage = readBatteryVoltage();
    Serial.printf("Initial Battery: %.2fV\n", globalBatteryVoltage);

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

            if (packet.messageType == MSG_TYPE_POLL_REQ && (packet.receiverID == 5 || packet.sourceNodeID == 5)) {
                Serial.println("Received POLL_REQ for Node 5 (RongPui). Measuring...");
                globalBatteryVoltage = readBatteryVoltage();
                uint8_t pct = measureAndCalculatePercentage();
                delay(50);
                sendData(MESSAGE_TYPE, REPEATER_NODE, 5, pct);
            }
        } else {
            LT.endReadSXBuffer();
        }
    }
}
