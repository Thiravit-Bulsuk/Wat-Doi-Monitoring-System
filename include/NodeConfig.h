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
#define MSG_TYPE_WATER     1  // ข้อมูลระดับน้ำ
#define MSG_TYPE_SPD       2  // ข้อมูลสถานะกันฟ้าผ่า
#define MSG_TYPE_LINK_QUALITY  3  // ข้อมูลคุณภาพลิงก์
#define MSG_TYPE_POLL_REQ  10 // คำขออ่านข้อมูลจาก Master (Request / Polling)

// ค่าคงที่สำหรับการ Polling & Retry
// หมายเหตุ: ที่ SF12/BW125 การส่งข้อมูล 1 ครั้ง (LoRaPacket 8 ไบต์) ใช้เวลา on-air ~1 วินาที
// รอบ Poll หนึ่งครั้งต้องส่งถึง 4 hop (Master->Repeater->Node->Repeater->Master) รวมเวลาวัดค่าเซนเซอร์ที่ Node
// จึงต้องตั้ง timeout ให้เผื่อเวลาไว้มากพอ ไม่เช่นนั้นจะ timeout ทุกครั้งแม้สัญญาณจะดีก็ตาม
#define POLL_TIMEOUT_MS         9000  // ระยะเวลารอตอบกลับต่อครั้ง (9 วินาที)
#define MAX_POLL_RETRIES        2     // จำนวนครั้งที่ลองส่งซ้ำหากไม่ตอบกลับ (รวมเป็น 3 ครั้ง)
#define POLL_CYCLE_INTERVAL_MS  180000 // รอบเวลาการ Polling ทุกโหนด (3 นาที)

// โครงสร้างก้อนข้อมูลสำหรับส่งผ่าน LoRa (ใช้ __attribute__((packed)) เพื่อป้องกัน padding)
struct __attribute__((packed)) LoRaPacket {
    uint8_t messageType;  // ประเภทข้อความ (WATER/SPD/POLL_REQ)
    uint8_t receiverID;   // ID ผู้รับ
    uint8_t senderID;     // ID ผู้ส่ง (Repeater, Master หรือ Node)
    uint8_t sourceNodeID; // ID ของ Node ต้นทาง (หรือ ID เป้าหมายกรณี POLL_REQ)
    uint8_t value;        // ค่าที่วัดได้ (%) หรือสถานะ SPD
    float battery;        // แรงดันแบตเตอรี่ (V)
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
    volatile bool batteryAlertSent; // กันแจ้งเตือนแบตเตอรี่ต่ำซ้ำๆ (ส่งครั้งเดียวจนกว่าแบตจะกลับมาปกติ)
    volatile uint8_t commFailureCount; // จำนวนรอบ Poll ที่ไม่ตอบติดต่อกัน
};

#endif