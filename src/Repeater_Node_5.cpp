#include <Arduino.h>
#include <SPI.h>
#include <SX126XLT.h>
#include <ProgramLT_Definitions.h>

// Define LoRa settings
#define LORA_DEVICE DEVICE_SX1262
#define NSS 8
#define NRESET 12
#define RFBUSY 13
#define DIO1 14

const uint32_t Frequency = 923000000;  // Frequency of transmissions
const uint32_t Offset = 0;             // Offset frequency for calibration purposes

const uint8_t Bandwidth = LORA_BW_125;  // LoRa bandwidth
const uint8_t SpreadingFactor = LORA_SF12;  // LoRa spreading factor
const uint8_t CodeRate = LORA_CR_4_5;  // LoRa coding rate
const uint8_t Optimisation = LDRO_AUTO;  // Low data rate optimisation setting

const int8_t TXpower = 14;  // LoRa transmit power in dBm

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

// Function to receive data
void onReceive(uint8_t MasterNode, uint8_t ThisNode) {
    int packetSize = LT.receiveSXBuffer(0, 0, WAIT_RX);
        
    if (packetSize > 0) {
        Serial.println("Received packet");

        LT.startReadSXBuffer(0);

        uint8_t messageType = LT.readUint8();
        uint8_t receiverID = LT.readUint8();  // should be Receiver Node (5)
        uint8_t senderId = LT.readUint8();    // should be Data Nodes (1 or 2 or 3 or 4)
        uint8_t message = LT.readUint8();
        LT.endReadSXBuffer();

        Serial.print("MessageType: "); Serial.println(messageType);
        Serial.print("ReceiverID: "); Serial.println(receiverID);
        Serial.print("SenderID: "); Serial.println(senderId);
        Serial.print("Message: "); Serial.println(message);

        if ((messageType == 1 || messageType == 2 || messageType == 3) && receiverID == 5) {
            Serial.print("Message from Node ");
            Serial.print(senderId);
            Serial.print(": ");
            Serial.println(message);

            delay(1000);

            uint8_t len;

            // Send data to Master Node
            LT.startWriteSXBuffer(0);

            LT.writeUint8(messageType);
            LT.writeUint8(MasterNode);   // Master Node
            LT.writeUint8(ThisNode);   // This Node
            LT.writeUint8(senderId);  // Data Node
            LT.writeUint8(message);

            len = LT.endWriteSXBuffer();
            uint8_t TXPacketL = LT.transmitSXBuffer(0, (len + 3), 5000, TXpower, WAIT_TX); // Send packet with TXpower
            if (TXPacketL == 0) {
              Serial.println("Error: Failed to send data");
            } else {
                Serial.println("Data sent successfully");
                Serial.println("Data forwarded to Master Node.");
                
            }
        } else {
            Serial.println("Message does not meet criteria, ignoring.");
        }
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("Repeater Node 5 Initializing...");

    SPI.begin();
    delay(500);

    if (!initLoRa()) {
        Serial.println("LoRa initialization failed. Halting.");
        while (1); 
    }

    LT.setupLoRa(Frequency, Offset, SpreadingFactor, Bandwidth, CodeRate, Optimisation);
    delay(1000);
}

void loop() {
    onReceive(6, 5);
}