// Data Node 4 (SPD)
#include <SPI.h>  
#include <SX126XLT.h>
#include <ProgramLT_Definitions.h>

// Define LoRa settings
#define LORA_DEVICE DEVICE_SX1262
#define NSS 8
#define NRESET 12
#define RFBUSY 13
#define DIO1 14

// Define SPD status pin
#define SPD_PIN 40

const uint32_t Frequency = 923000000;           // Frequency of transmissions
const uint32_t Offset = 0;                      // Offset frequency for calibration purposes

const uint8_t Bandwidth = LORA_BW_125;          // LoRa bandwidth
const uint8_t SpreadingFactor = LORA_SF12;      // LoRa spreading factor
const uint8_t CodeRate = LORA_CR_4_5;           // LoRa coding rate
const uint8_t Optimisation = LDRO_AUTO;         // Low data rate optimisation setting

const int8_t TXpower = 14;                      // LoRa transmit power in dBm

#define MESSAGE_TYPE_SPD 2
#define REPEATER_NODE 5
#define NODE_ID 4

SX126XLT LT;

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
    return digitalRead(SPD_PIN) == LOW;  // SPD in bad state if pin reads LOW
}

// Function to send data
uint8_t sendData(uint8_t MessageType, uint8_t RepeaterNode, uint8_t NodeID, uint8_t status) {
    delay(1000); // Delay before sending
    uint8_t len;

    Serial.println(status == 1 ? "SPD Bad" : "SPD Good"); // Print SPD status to be sent
    LT.startWriteSXBuffer(0);       // Start writing to buffer

    LT.writeUint8(MessageType);     // Packet type (SPD Status)
    LT.writeUint8(RepeaterNode);    // ID ของ Repeater Node
    LT.writeUint8(NodeID);          // Node ID
    LT.writeUint8(status);          // SPD status: 1 for bad, 0 for good

    len = LT.endWriteSXBuffer();    // Close the packet and get length
    uint8_t TXPacketL = LT.transmitSXBuffer(0, len + 2, 5000, TXpower, WAIT_TX); // Send packet with TXpower
    if (TXPacketL == 0) {
        Serial.println("Error: Failed to send data");
    } else {
        Serial.println("Data sent successfully");
    }
    return TXPacketL;
}

void setup() {
    Serial.begin(115200);
    Serial.println("Data Node 4 (SPD) Initializing...");

    SPI.begin();
    delay(500);

    pinMode(SPD_PIN, INPUT_PULLUP);  // Use internal pull-up resistor

    if (!initLoRa()) {
        Serial.println("Critical Error: LoRa initialization failed. System halting.");
        while (1);  // Stop if LoRa initialization fails
    }
    LT.setupLoRa(Frequency, Offset, SpreadingFactor, Bandwidth, CodeRate, Optimisation);
    int randomDelay = random(15000, 20000);  // สุ่มหน่วงเวลาระหว่าง 1 ถึง 5 วินาที
    delay(randomDelay);
}

void loop() {
    bool isBad = readSPDStatus();
    uint8_t status = isBad ? 1 : 0;  // Convert bool to uint8_t for transmission
    sendData(MESSAGE_TYPE_SPD, REPEATER_NODE, NODE_ID, status);  // Send SPD status to Repeater Node
    int randomDelay1 = random(1000, 10000);  
    delay(randomDelay1);
    sendData(MESSAGE_TYPE_SPD, REPEATER_NODE, NODE_ID, status);  // Send SPD status to Repeater Node

    delay(2000);  
    LT.setSleep(CONFIGURATION_RETENTION);   // Preserve register settings in sleep.
    Serial.println("Sleeping zzzzz....");
    Serial.flush();
    
    // คำนวณเวลาหลับ: 5 นาที (300,000 ms) + สุ่ม 1-30 วินาที
    // หน่วยเป็น microseconds (us) จึงต้องคูณ 1000
    uint64_t sleepDuration = 300000ULL * 1000;
    sleepDuration += (uint64_t)random(1000, 30000) * 1000;

    esp_sleep_enable_timer_wakeup(sleepDuration);
    esp_deep_sleep_start();
}