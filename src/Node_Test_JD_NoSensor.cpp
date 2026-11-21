#include <SPI.h>
#include <SX126XLT.h>
#include <ProgramLT_Definitions.h>
#include "NodeConfig.h"
#include "Arduino.h"

// Define LoRa settings (เหมือน Node 1 ทุกอย่าง)
#define LORA_DEVICE DEVICE_SX1262
#define NSS 8
#define NRESET 12
#define RFBUSY 13
#define DIO1 14

const uint32_t Frequency = 923000000;
const uint32_t Offset = 0;
const uint8_t Bandwidth = LORA_BW_125;
const uint8_t SpreadingFactor = LORA_SF12;
const uint8_t CodeRate = LORA_CR_4_5;
const uint8_t Optimisation = LDRO_AUTO;
const int8_t TXpower = 14;

#define MESSAGE_TYPE 1
#define REPEATER_NODE 101
#define NODE_ID 1  // ปลอมเป็น Node 1 (JD)

SX126XLT LT;

bool initLoRa() {
    if (!LT.begin(NSS, NRESET, RFBUSY, DIO1, LORA_DEVICE)) {
        Serial.println("LoRa init failed!");
        return false;
    }
    Serial.println("LoRa init succeeded.");
    return true;
}

// ส่งค่าคงที่กลับไปแทนการอ่านเซนเซอร์จริง
uint8_t sendData(uint8_t MessageType, uint8_t RepeaterNode, uint8_t tankID, uint8_t percentage) {
    LoRaPacket packet;
    packet.messageType = MessageType;
    packet.receiverID = RepeaterNode;
    packet.senderID = tankID;
    packet.sourceNodeID = tankID;
    packet.value = percentage;
    packet.battery = 0.0;

    uint8_t txBuffer[sizeof(LoRaPacket)];
    memcpy(txBuffer, &packet, sizeof(LoRaPacket));

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
    Serial.println("Fake Node 1 (No Sensor) - Test Mode Initializing...");

    SPI.begin();
    delay(500);

    if (!initLoRa()) {
        Serial.println("Critical Error: LoRa initialization failed.");
        while (1);
    }

    LT.setupLoRa(Frequency, Offset, SpreadingFactor, Bandwidth, CodeRate, Optimisation);
    Serial.println("LoRa setup complete. Listening for poll requests...");
}

void loop() {
    int packetSize = LT.receiveSXBuffer(0, 0, WAIT_RX);

    if (packetSize <= 0) {
        uint16_t irq = LT.readIrqStatus();
        Serial.printf("FakeNode1 RX fail, IRQ status: 0x%04X", irq);
        if (irq & IRQ_CRC_ERROR) Serial.print(" [CRC_ERROR]");
        if (irq & IRQ_HEADER_ERROR) Serial.print(" [HEADER_ERROR]");
        if (irq & IRQ_RX_TX_TIMEOUT) Serial.print(" [TIMEOUT]");
        Serial.println();
    }

    if (packetSize > 0) {
        if (packetSize == sizeof(LoRaPacket) || packetSize == (sizeof(LoRaPacket) + 2)) {
            int16_t rssi = LT.readPacketRSSI();
            int8_t snr = LT.readPacketSNR();
            Serial.printf("FakeNode1 received packet (size OK), RSSI=%d dBm, SNR=%d dB\n", rssi, snr);

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

            Serial.printf("FakeNode1 RX: Type=%d, Recv=%d, Source=%d (expecting NODE_ID=%d)\n",
                          packet.messageType, packet.receiverID, packet.sourceNodeID, NODE_ID);

            // เช็คเฉพาะ receiverID เพราะ field นี้ถูก Repeater เขียนใหม่ตอน forward เท่านั้น
            if (packet.messageType == MSG_TYPE_POLL_REQ && packet.receiverID == NODE_ID) {
                Serial.println("Received POLL_REQ for fake Node 1. Sending dummy value...");
                delay(50);
                uint8_t txResult = sendData(MESSAGE_TYPE, REPEATER_NODE, NODE_ID, 88); // ค่าคงที่ 88%
                Serial.printf("FakeNode1 sendData result: %d (0 = failed)\n", txResult);
            } else {
                Serial.println("FakeNode1: packet not addressed to this node, ignoring.");
            }
        } else {
            Serial.printf("FakeNode1 received packet with unexpected size: %d\n", packetSize);
            LT.endReadSXBuffer();
        }
    }
}