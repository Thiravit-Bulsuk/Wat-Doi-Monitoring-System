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
const int TANK_FULL_CM = 20;   // ระยะห่างระหว่างเซ็นเซอร์กับน้ำเต็ม
const int TANK_EMPTY_CM = 120; // ระยะห่างระหว่างเซ็นเซอร์กับก้นแทงก์

const uint32_t Frequency = 923000000;           // Frequency of transmissions
const uint32_t Offset = 0;                      // Offset frequency for calibration purposes

const uint8_t Bandwidth = LORA_BW_125;          // LoRa bandwidth
const uint8_t SpreadingFactor = LORA_SF12;      // LoRa spreading factor
const uint8_t CodeRate = LORA_CR_4_5;           // LoRa coding rate
const uint8_t Optimisation = LDRO_AUTO;         // Low data rate optimisation setting

const int8_t TXpower = 14;                      // ปรับเป็น 14 dBm เพื่อความเสถียรในการส่ง

#define MESSAGE_TYPE 1
#define REPEATER_NODE 101
#define NODE_ID 7       // หมายเลข Node KhonPD

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
    display.drawString(64, 0, "Node 7: KhonPD");
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

uint8_t sendData(uint8_t MessageType, uint8_t RepeaterNode, uint8_t tankID, uint8_t percentage) {
    smartDelay(1000);
    LoRaPacket packet;
    packet.messageType = MessageType;
    packet.receiverID = RepeaterNode;
    packet.senderID = tankID;    
    packet.sourceNodeID = tankID;
    packet.value = percentage;
    packet.battery = 0.0;        

    uint8_t txBuffer[sizeof(LoRaPacket)];
    memcpy(txBuffer, &packet, sizeof(LoRaPacket));

    LT.startWriteSXBuffer(0);
    for (int i = 0; i < sizeof(LoRaPacket); i++) {
        LT.writeUint8(txBuffer[i]);
    }
    uint8_t len = LT.endWriteSXBuffer();
    return LT.transmitSXBuffer(0, (len + 2), 5000, TXpower, WAIT_TX);
}

void setup() {
    Serial.begin(115200);
    SPI.begin();
    pinMode(TRIGGER_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);
    pinMode(PRG_BUTTON, INPUT_PULLUP);
    
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
        showStatus();
    }

    VextON();
    float distance = readDistance();
    bool shouldSendZero = false;
    bool shouldSendValue = false;

    if (distance < 0) {
        if (lastValidDistance > (TANK_EMPTY_CM - 20)) {
            isTankEmpty = true;
            shouldSendZero = true;
        }
        refillConfirmationCount = 0;
    } else {
        if (isTankEmpty) {
            if (++refillConfirmationCount >= REFILL_THRESHOLD) {
                isTankEmpty = false;
                lastValidDistance = distance;
                shouldSendValue = true;
                refillConfirmationCount = 0;
            } else { shouldSendZero = true; }
        } else {
            lastValidDistance = distance;
            shouldSendValue = true;
            refillConfirmationCount = 0;
        }
    }

    if (initLoRa()) {
        LT.setupLoRa(Frequency, Offset, SpreadingFactor, Bandwidth, CodeRate, Optimisation);
        uint8_t p = shouldSendZero ? 0 : map(lastValidDistance, TANK_FULL_CM, TANK_EMPTY_CM, 100, 0);
        p = constrain(p, 0, 100);
        sendData(MESSAGE_TYPE, REPEATER_NODE, NODE_ID, p);
        smartDelay(random(10000, 20000));
        sendData(MESSAGE_TYPE, REPEATER_NODE, NODE_ID, p);
    }

    VextOFF();
    LT.setSleep(CONFIGURATION_RETENTION);
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); 
    esp_sleep_enable_timer_wakeup(300000ULL * 1000ULL + random(1000, 30000) * 1000ULL);
    esp_deep_sleep_start();
}

void loop() {}