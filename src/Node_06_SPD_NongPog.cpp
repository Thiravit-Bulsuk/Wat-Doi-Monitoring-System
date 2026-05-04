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

// Define SPD status pin
#define SPD_PIN 4

// Configuration for SPD Type
#define SPD_IS_NC true // ตั้งเป็น true ถ้าเปลี่ยนอุปกรณ์เป็นแบบ NC (Normally Closed) เพื่อความปลอดภัย (Fail-Safe)

const uint32_t Frequency = 923000000;           // Frequency of transmissions
const uint32_t Offset = 0;                      // Offset frequency for calibration purposes

const uint8_t Bandwidth = LORA_BW_125;          // LoRa bandwidth
const uint8_t SpreadingFactor = LORA_SF12;      // LoRa spreading factor
const uint8_t CodeRate = LORA_CR_4_5;           // LoRa coding rate
const uint8_t Optimisation = LDRO_AUTO;         // Low data rate optimisation setting

const int8_t TXpower = 14;                      // LoRa transmit power in dBm

#define MESSAGE_TYPE_SPD 2
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

// Forward declaration
bool readSPDStatus();

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
    display.drawString(64, 0, "Node 6: SPD");
    
    bool failed = readSPDStatus();
    display.setFont(ArialMT_Plain_24);
    if (failed) {
        display.drawString(64, 25, "FAILED!");
    } else {
        display.drawString(64, 25, "Working");
    }

    display.display();
    delay(3000); // Show for 3 seconds
    display.displayOff();
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

// Function to read SPD status
bool readSPDStatus() {
    bool pinState = digitalRead(SPD_PIN);
    
    if (SPD_IS_NC) {
        // NC Logic (Fail-Safe):
        // - ปกติ (Good): หน้าสัมผัสปิด (Closed) -> ลง Ground -> Pin LOW
        // - เสีย/สายขาด (Bad): หน้าสัมผัสเปิด (Open) -> Pull-up -> Pin HIGH
        return pinState == HIGH; 
    } else {
        // NO Logic (Current):
        // - ปกติ (Good): หน้าสัมผัสเปิด (Open) -> Pull-up -> Pin HIGH
        // - เสีย (Bad): หน้าสัมผัสปิด (Closed) -> ลง Ground -> Pin LOW
        return pinState == LOW;
    }
}

// Function to send data
uint8_t sendData(uint8_t MessageType, uint8_t RepeaterNode, uint8_t NodeID, uint8_t status) {
    delay(1000);

    LoRaPacket packet;
    packet.messageType = MessageType;
    packet.receiverID = RepeaterNode;
    packet.senderID = NodeID;
    packet.sourceNodeID = NodeID;
    packet.value = status; // ส่งค่าสถานะ (0 หรือ 1)
    packet.battery = 0.0;

    uint8_t txBuffer[sizeof(LoRaPacket)];
    memcpy(txBuffer, &packet, sizeof(LoRaPacket)); // Copy struct to byte array

    Serial.println(status == 1 ? "SPD Failed" : "SPD Working");

    LT.startWriteSXBuffer(0);
    for (int i = 0; i < sizeof(LoRaPacket); i++) {
        LT.writeUint8(txBuffer[i]);
    }
    uint8_t len = LT.endWriteSXBuffer();

    return LT.transmitSXBuffer(0, (len + 2), 5000, TXpower, WAIT_TX);
}

void setup() {
    Serial.begin(115200);
    Serial.println("Data Node 6 (NongPog) Initializing...");

    SPI.begin();
    delay(500);

    pinMode(PRG_BUTTON, INPUT_PULLUP);
    pinMode(SPD_PIN, INPUT_PULLUP);  // Use internal pull-up resistor

    // ตรวจสอบว่าตื่นเพราะกดปุ่มหรือไม่
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
        showStatus(); // แสดงสถานะแล้วทำงานต่อ
    }

    if (!initLoRa()) {
        Serial.println("Critical Error: LoRa initialization failed. System halting.");
        // ถ้า LoRa พัง ให้หลับไปก่อนแล้วค่อยตื่นมาลองใหม่
    } else {
        LT.setupLoRa(Frequency, Offset, SpreadingFactor, Bandwidth, CodeRate, Optimisation);
        
        // Random delay to prevent communication collisions (เฉพาะตอนทำงานปกติ ไม่ใช่ตอนกดปุ่มดู)
        if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT0) {
            int randomDelay = random(1000, 30000);  
            delay(randomDelay);
        }

        bool failed = readSPDStatus();
        uint8_t status = failed ? 1 : 0;  // Convert bool to uint8_t for transmission
        
        // Send SPD status twice (to ensure successful transmission)
        sendData(MESSAGE_TYPE_SPD, REPEATER_NODE, 6, status);  
        delay(random(1000, 5000));  
        sendData(MESSAGE_TYPE_SPD, REPEATER_NODE, 6, status);  
    }

    VextOFF();
    
    // Put LoRa module to sleep to save power
    LT.setSleep(CONFIGURATION_RETENTION);   
    Serial.println("LoRa Sleeping zzzzz....");
    
    // Put ESP32 into deep sleep for 5 minutes (300000 ms)
    uint64_t sleepTime = 300000ULL * 1000ULL; // 5 นาที
    sleepTime += random(1000, 30000) * 1000ULL; // Random Jitter
    esp_sleep_enable_timer_wakeup(sleepTime);
    
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); // ตื่นด้วยปุ่มกด

    Serial.println("ESP32 Sleeping zzzzz....");
    
    Serial.flush();
    esp_deep_sleep_start();  // Enter deep sleep
}

void loop() {}