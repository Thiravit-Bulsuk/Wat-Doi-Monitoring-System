// LoRa Test Sender for ThamYai (Node 4)
// Simulates sending water level data every 30 seconds.
// This code is for testing LoRa transmission without a physical sensor.

#include <Arduino.h>
#include <SPI.h>  
#include <SX126XLT.h>
#include <ProgramLT_Definitions.h> // For common LoRa definitions if any
#include "NodeConfig.h" // For LoRaPacket struct

// Define LoRa settings
#define LORA_DEVICE DEVICE_SX1262
#define NSS 8
#define NRESET 12
#define RFBUSY 13
#define DIO1 14

const uint32_t Frequency = 923000000;           // Frequency of transmissions
const uint32_t Offset = 0;                      // Offset frequency for calibration purposes

const uint8_t Bandwidth = LORA_BW_125;          // LoRa bandwidth
const uint8_t SpreadingFactor = LORA_SF12;      // LoRa spreading factor
const uint8_t CodeRate = LORA_CR_4_5;           // LoRa coding rate
const uint8_t Optimisation = LDRO_AUTO;         // Low data rate optimisation setting

const int8_t TXpower = 14;                      // LoRa transmit power in dBm

#define MESSAGE_TYPE 1 // Water level message type
#define NODE_ID 4      // ThamYai's Node ID
#define REPEATER_NODE 101 // Repeater's Node ID

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

// Function to send data using LoRaPacket structure
uint8_t sendData(uint8_t MessageType, uint8_t RepeaterNode, uint8_t tankID, uint8_t percentage) {
    LoRaPacket packet;
    packet.messageType = MessageType;
    packet.receiverID = RepeaterNode;
    packet.senderID = tankID;    // For Data Node, senderID and sourceNodeID are the same
    packet.sourceNodeID = tankID;
    packet.value = percentage;
    packet.battery = 0.0;        // No battery sensor on this test board, send 0.0V

    Serial.print("Sending Data: TankID="); Serial.print(tankID); Serial.print(", %="); Serial.println(percentage);
    
    uint8_t txBuffer[sizeof(LoRaPacket)];
    memcpy(txBuffer, &packet, sizeof(LoRaPacket)); // Copy struct to byte array

    Serial.print("Sender: Raw packet data (hex): ");
    for (int i = 0; i < sizeof(LoRaPacket); i++) {
        Serial.printf("%02X ", txBuffer[i]);
    }
    Serial.println();
    Serial.print("Sender: sizeof(LoRaPacket) = "); Serial.println(sizeof(LoRaPacket));
    
    LT.startWriteSXBuffer(0);
    for (int i = 0; i < sizeof(LoRaPacket); i++) {
        LT.writeUint8(txBuffer[i]);
    }
    uint8_t len = LT.endWriteSXBuffer(); // This 'len' should be sizeof(LoRaPacket)

    // The crucial part: adding 2 for CRC, as per user's original working code
    uint8_t TXPacketL = LT.transmitSXBuffer(0, (len + 2), 5000, TXpower, WAIT_TX);

    if (TXPacketL == 0) {
        Serial.println("Error: Failed to send data");
    } else {
        Serial.println("Data sent successfully");
    }
    return TXPacketL;
}

void setup() {
    Serial.begin(115200);
    Serial.println("LoRa Test Sender (ThamYai - Node 4) Initializing...");

    SPI.begin();
    delay(500);

    // Initialize LoRa
    if (!initLoRa()) {
        Serial.println("Critical Error: LoRa initialization failed. Halting.");
        while (1); // Stop if LoRa initialization fails
    }
    LT.setupLoRa(Frequency, Offset, SpreadingFactor, Bandwidth, CodeRate, Optimisation);
    delay(1000); // Give LoRa module some time to settle
}

void loop() {
    static uint8_t simulatedWaterLevel = 100; // Start at 100%
    static bool decreasing = true;

    // Simulate water level changing from 100% down to 0% and back up
    if (decreasing) {
        simulatedWaterLevel--;
        if (simulatedWaterLevel == 0) decreasing = false;
    } else {
        simulatedWaterLevel++;
        if (simulatedWaterLevel == 100) decreasing = true;
    }

    Serial.print("Simulating water level: "); Serial.print(simulatedWaterLevel); Serial.println(" %");

    sendData(MESSAGE_TYPE, REPEATER_NODE, NODE_ID, simulatedWaterLevel);

    delay(30000); // Send every 30 seconds (1/2 minute)
}