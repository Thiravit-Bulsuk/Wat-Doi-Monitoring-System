#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <HT_SSD1306Wire.h>
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

// Initialize OLED Display and Button
static SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);
#define PRG_BUTTON 0

// Global variables for display status
volatile bool displayActive = false;
unsigned long displayTimeout = 0;
String logStatus = "Waiting...";
String logLastPacket = "-";
String logAction = "-";

void VextON() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
}

void VextOFF() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, HIGH);
}

// Task to handle Display and Button
void DisplayTask(void *parameter) {
    pinMode(PRG_BUTTON, INPUT_PULLUP);
    
    for(;;) {
        if (digitalRead(PRG_BUTTON) == LOW) {
            if (!displayActive) {
                VextON();
                delay(50);
                display.init();
                display.displayOn();
                display.setFont(ArialMT_Plain_10);
                display.setTextAlignment(TEXT_ALIGN_LEFT);
            }
            displayActive = true;
            displayTimeout = millis() + 180000; // 3 minutes
            vTaskDelay(200 / portTICK_PERIOD_MS); // Debounce
        }

        if (displayActive) {
            if (millis() > displayTimeout) {
                displayActive = false;
                display.clear();
                display.display();
                display.displayOff();
                VextOFF();
            } else {
                display.clear();
                display.drawString(0, 0, "Repeater Node 5 (Live)");
                display.drawString(0, 13, "St: " + logStatus);
                display.drawString(0, 26, "Pkt: " + logLastPacket);
                display.drawString(0, 39, "Act: " + logAction);
                display.drawString(0, 52, "Off in: " + String((displayTimeout - millis())/1000) + "s");
                display.display();
            }
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
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

// Function to receive data
void onReceive(uint8_t MasterNode, uint8_t ThisNode) {
    int packetSize = LT.receiveSXBuffer(0, 0, WAIT_RX);
        
    if (packetSize > 0) {
        Serial.println("Received packet");
        logStatus = "RX " + String(packetSize) + " bytes";

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
        
        logLastPacket = "N" + String(senderId) + " -> N" + String(receiverID) + " (T" + String(messageType) + ")";

        if ((messageType == 1 || messageType == 2 || messageType == 3) && receiverID == 5) {
            Serial.print("Message from Node ");
            Serial.print(senderId);
            Serial.print(": ");
            Serial.println(message);
            logAction = "Forwarding...";

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
              logAction = "Send Failed";
            } else {
                Serial.println("Data sent successfully");
                Serial.println("Data forwarded to Master Node.");
                logAction = "Forwarded OK";
                
            }
        } else {
            Serial.println("Message does not meet criteria, ignoring.");
            logAction = "Ignored";
            if (displayActive) delay(500); // Keep message on screen briefly if display is on
        }
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("Repeater Node 5 Initializing...");

    SPI.begin();
    delay(500);

    // Start Display Task
    xTaskCreate(DisplayTask, "DisplayTask", 4096, NULL, 1, NULL);
    VextOFF(); // Ensure display is off initially

    if (!initLoRa()) {
        Serial.println("LoRa initialization failed. Halting.");
        while (1); 
    }

    LT.setupLoRa(Frequency, Offset, SpreadingFactor, Bandwidth, CodeRate, Optimisation);
    delay(1000);
}

void loop() {
    logStatus = "Listening...";
    onReceive(6, 5);
}