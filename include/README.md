# Wat Doi Monitoring System 🌊⚡

ระบบตรวจสอบระดับน้ำและสถานะกันฟ้าผ่า (SPD) ระยะไกลผ่านเครือข่าย LoRa และแจ้งเตือนผ่าน LINE/MQTT สำหรับวัดดอย

## 📋 ภาพรวมระบบ (System Overview)

โปรเจกต์นี้พัฒนาขึ้นเพื่อแก้ปัญหาการตรวจสอบระดับน้ำในแท็งค์ที่กระจายตัวอยู่ตามจุดต่างๆ บนภูเขา และตรวจสอบสถานะของอุปกรณ์ป้องกันฟ้าผ่า (SPD) โดยใช้เทคโนโลยี **LoRa (Long Range)** ในการสื่อสารระยะไกลที่ประหยัดพลังงาน

### โครงสร้างการทำงาน (Architecture)
1.  **Data Nodes (Nodes 1, 2, 3, 4, 7):**
    *   ติดตั้ง ณ จุดวัดต่างๆ (แท็งค์น้ำ, ตู้ไฟ)
    *   อ่านค่าจากเซ็นเซอร์ (Ultrasonic หรือ Dry Contact)
    *   ส่งข้อมูลผ่าน LoRa ไปยัง Repeater
    *   ทำงานในโหมด **Deep Sleep** เพื่อประหยัดพลังงาน (ตื่นมาส่งข้อมูลทุก 5 นาที)
2.  **Repeater Node (Node 5):**
    *   ติดตั้ง ณ จุดกึ่งกลางที่รับสัญญาณได้ดี
    *   ทำหน้าที่รับช่วงต่อสัญญาณ (Relay) จาก Data Nodes ส่งไปยัง Master Node
    *   ทำงานตลอดเวลา (Always On)
3.  **Master Node (Node 6):**
    *   ติดตั้ง ณ สำนักงาน/กุฏิ
    *   รับข้อมูลจาก Repeater แสดงผลบนหน้าจอ OLED
    *   เชื่อมต่อ WiFi เพื่อส่งข้อมูลขึ้น **MQTT Broker** (HiveMQ)
    *   แจ้งเตือนผ่าน **LINE Notify** เมื่อเกิดเหตุผิดปกติ (น้ำลดต่ำ, SPD เสีย, หรือขาดการติดต่อ)
    *   ควบคุม Relay เพื่อสั่งงานปั๊มน้ำหรือวาล์วตามเงื่อนไข

---

## 🛠️ ฮาร์ดแวร์ที่ต้องใช้ (Hardware Requirements)

*   **Microcontroller:** Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262) จำนวน 7 บอร์ด
*   **Sensors:**
    *   **Ultrasonic JSN-SR04T (Waterproof):** สำหรับวัดระดับน้ำ (Node 1, 2, 3, 4)
    *   **Dry Contact / Switch:** สำหรับเช็คสถานะ SPD (Node 7)
*   **Display:** OLED 0.96" (Built-in บนบอร์ด Heltec)
*   **Actuators:** Relay Module 4 Channels (Active Low) ต่อกับ Master Node
*   **Power Supply:**
    *   Battery Li-Po / 18650 (สำหรับ Data Nodes)
    *   Adapter 5V USB-C (สำหรับ Repeater และ Master Node)

### 🔌 การต่อวงจร (Pinout Configuration)

#### 📍 Data Nodes (1, 3, 4) - Ultrasonic เดี่ยว
*   **Trigger Pin:** GPIO 6
*   **Echo Pin:** GPIO 7
*   **Wakeup Button:** GPIO 0 (ปุ่ม PRG บนบอร์ด)

#### 📍 Data Node 2 (PhraNon) - Ultrasonic คู่
*   **Sensor 1 (Tank 2):** Trigger GPIO 6, Echo GPIO 7
*   **Sensor 2 (Tank 8):** Trigger GPIO 4, Echo GPIO 5

#### 📍 Data Node 7 (SPD)
*   **SPD Status Pin:** GPIO 4 (ต่อแบบ Input Pull-up)
    *   *Note:* สามารถตั้งค่า Logic (NO/NC) ได้ในไฟล์ `Data_Node_7_SPD.cpp`

#### 📍 Master Node 6
*   **Relays:**
    *   Relay 1: GPIO 7
    *   Relay 2: GPIO 6
    *   Relay 3: GPIO 5
    *   Relay 4: GPIO 4
*   **Reset Button:** GPIO 26 (กดค้าง 2 วินาทีเพื่อรีเซ็ตการแจ้งเตือน)

---

## 💻 ซอฟต์แวร์และการติดตั้ง (Software & Installation)

โปรเจกต์นี้ใช้ **PlatformIO** ในการพัฒนา

### 1. การเตรียม Environment
*   ติดตั้ง **VS Code**
*   ติดตั้ง Extension **PlatformIO IDE** ใน VS Code
*   ติดตั้ง **Git**

### 2. Library ที่ใช้ (Included in `lib/`)
Library ทั้งหมดถูกรวมอยู่ในโฟลเดอร์ `lib/` ของโปรเจกต์แล้ว ไม่ต้องติดตั้งเพิ่ม:
*   `SX126XLT` (LoRa Communication)
*   `Heltec_ESP32_Dev-Boards` (OLED Display & Board Support)
*   `PubSubClient` (MQTT)
*   `SimpleTimer` (Task Scheduling)

### 3. การตั้งค่าก่อน Upload (Configuration)
แก้ไขไฟล์ `src/Master_Node_6_MainPannel.cpp` เพื่อตั้งค่าเครือข่าย:
```cpp
// WiFi Settings
#define WIFI_SSID "ชื่อ WiFi ของคุณ"
#define WIFI_PASSWORD "รหัสผ่าน WiFi"

// MQTT Broker Settings
const char* mqtt_server = "your.mqtt.broker.com";
const char* mqtt_username = "your_username";
const char* mqtt_password = "your_password";

// LINE Notify Token
const char* LINE_ACCESS_TOKEN = "YOUR_LINE_NOTIFY_TOKEN";
```

### 4. การอัปโหลด (Upload)
เลือก Environment ที่ต้องการ Upload ผ่านแถบสถานะด้านล่างของ VS Code หรือเมนู Project Tasks:
*   `env:node_1_jd`
*   `env:node_2_phranon`
*   `env:node_3_thamyai`
*   `env:node_4_rongpui`
*   `env:repeater_node_5`
*   `env:master_node_6`
*   `env:node_7_SPD`

---

## 📡 MQTT Topics & API

### ⚙️ Broker Settings
*   **Server:** `aa9e03df167b40b086069a385503cf2f.s1.eu.hivemq.cloud`
*   **Port:** `8883` (Secure/TLS)

### 📤 Publish Topics (Board ➔ Dashboard/App)
หัวข้อที่ Master Node ส่งข้อมูลออกมาแสดงผล

| Topic | Description | Payload / Value |
| :--- | :--- | :--- |
| `JD` | ระดับน้ำแท็งค์เจดีย์ | `0` - `100` (%) |
| `PhraNon` | ระดับน้ำแท็งค์พระนอน | `0` - `100` (%) |
| `ThamYai` | ระดับน้ำแท็งค์ถ้ำใหญ่ | `0` - `100` (%) |
| `RongPui` | ระดับน้ำแท็งค์โรงปุ๋ย | `0` - `100` (%) |
| `Plastic` | ระดับน้ำแท็งค์พลาสติก | `0` - `100` (%) |
| `NongPog` | สถานะกันฟ้าผ่า (SPD) | `0` = ปกติ, `1` = เสีย |
| `notification_status` | สถานะระบบแจ้งเตือน | `1` = เปิด, `0` = ปิด |
| `message` | ข้อความสถานะระบบ/Log | String Message |

### 📥 Subscribe Topics (Dashboard/App ➔ Board)
หัวข้อที่ Master Node รอรับคำสั่งจากผู้ใช้

| Topic | Description | Payload / Command |
| :--- | :--- | :--- |
| `request_data` | ขอข้อมูลล่าสุดทันที (Manual Request) | ส่ง `1` เพื่อเรียกดูข้อมูลเซ็นเซอร์ทั้งหมดทันที |
| `notification_control` | ควบคุมการเปิด/ปิดแจ้งเตือน | `1` = เปิด, `0` = ปิด |
| `reset_notification` | รีเซ็ตสถานะการแจ้งเตือนและ Relay | ส่ง `1` เพื่อเคลียร์สถานะ |
| `reset_system` | รีสตาร์ทบอร์ด Master Node | ส่ง `1` เพื่อสั่ง Reboot |

---

## 🔋 Power Management (Deep Sleep Logic)

เพื่อให้ Data Nodes สามารถทำงานด้วยแบตเตอรี่ได้ยาวนาน ระบบจึงใช้ Logic ดังนี้:

1.  **Wake Up:** ตื่นขึ้นมาเมื่อครบกำหนดเวลา (Timer) หรือเมื่อกดปุ่ม (Ext0 Wakeup)
2.  **Sensor Reading:**
    *   อ่านค่าแบบ Burst (20 ครั้ง)
    *   กรองค่ารบกวนด้วยอัลกอริทึม **Density-based Clustering**
    *   ตรวจสอบความสมเหตุสมผล (Sanity Check) เทียบกับค่าเดิมใน RTC Memory
3.  **Transmission:**
    *   ส่งข้อมูลผ่าน LoRa ไปยัง Repeater (ส่งซ้ำเพื่อความชัวร์)
4.  **Deep Sleep:**
    *   จำค่าตัวแปรสำคัญไว้ใน `RTC_DATA_ATTR`
    *   เข้าสู่โหมดหลับลึก 5 นาที (กินกระแสต่ำมาก ~10-20uA)

---

## ⚠️ Troubleshooting

*   **ค่าระดับน้ำเป็น -1 หรือ Error:** เซ็นเซอร์อาจมีปัญหา หรือระยะวัดอยู่นอกช่วง (20cm - 450cm)
*   **ไม่ได้รับแจ้งเตือน LINE:** ตรวจสอบการเชื่อมต่อ WiFi ของ Master Node และตรวจสอบว่า `notification_status` เป็น ON
*   **SPD แจ้งเตือนผิดพลาด:** ตรวจสอบการตั้งค่า `SPD_IS_NC` ในโค้ด Node 7 ให้ตรงกับชนิดของอุปกรณ์ (NO/NC)

---

**Developed for Wat Doi Monitoring Project**