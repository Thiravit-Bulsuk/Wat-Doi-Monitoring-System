#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

#include <Arduino.h>

// สถานะการแจ้งเตือน
enum AlertState {
    STATE_NORMAL,
    STATE_LOW_WATER,
    STATE_COMM_ERROR
};

// ประเภทของข้อมูลที่ส่ง
#define MSG_TYPE_WATER 1 // ข้อมูลระดับน้ำ
#define MSG_TYPE_SPD   2 // ข้อมูลสถานะกันฟ้าผ่า

// โครงสร้างก้อนข้อมูลสำหรับส่งผ่าน LoRa (ใช้ __attribute__((packed)) เพื่อป้องกัน padding)
struct __attribute__((packed)) LoRaPacket {
    uint8_t messageType;  // ประเภทข้อความ (WATER/SPD)
    uint8_t receiverID;   // ID ผู้รับ
    uint8_t senderID;     // ID ผู้ส่ง (Repeater หรือ Node)
    uint8_t sourceNodeID; // ID ของ Node ต้นทางจริงๆ
    uint8_t value;        // ค่าที่วัดได้ (%)
    float battery;        // แรงดันแบตเตอรี่ (V) - เผื่อไว้ใช้ในอนาคต
};

// โครงสร้างข้อมูลสำหรับแต่ละ Node
struct Node {
    uint8_t id;             // ID ของ Node (1, 2, 3, 4, 7, 8...)
    String name;           // ชื่อเรียก (JD, PhraNon, etc.)
    const char* mqttTopic;  // Topic สำหรับ MQTT
    int threshold;         // ระดับน้ำที่ให้เตือน (%)
    uint8_t relayPin;      // ขา Relay ที่ควบคุม (0 ถ้าไม่มี)
    uint8_t msgType;       // ประเภทเซ็นเซอร์ (WATER หรือ SPD)
    
    // สถานะภายใน (ใช้จัดการ Notification)
    volatile int lastValue;
    volatile unsigned long lastSeen;
    volatile float battery;         // เพิ่มฟิลด์เก็บค่าแรงดันแบตเตอรี่
    volatile AlertState alertState; // สถานะแจ้งเตือน (Normal / Alert)
    volatile AlertState commState;  // สถานะการสื่อสาร (Normal / Error)
    volatile int lowCount;      // ตัวนับสำหรับ Debounce
};

#endif