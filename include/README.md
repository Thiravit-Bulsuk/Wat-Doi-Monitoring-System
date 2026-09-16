# Wat Doi Monitoring System 🌊⚡

ระบบตรวจวัดระดับน้ำในแท็งก์และสถานะอุปกรณ์ป้องกันฟ้าผ่า (SPD) ระยะไกลผ่านเครือข่าย LoRa พร้อมระบบแจ้งเตือนผ่าน LINE Notify และเชื่อมต่อแดชบอร์ดผ่าน Secure MQTT (TLS) สำหรับวัดดอย

---

## 📋 สารบัญ (Table of Contents)
- [ภาพรวมสถาปัตยกรรมระบบ (System Architecture)](#-ภาพรวมสถาปัตยกรรมระบบ-system-architecture)
- [หลักการทำงานของระบบ Master Polling (Request-Response Protocol)](#-หลักการทำงานของระบบ-master-polling-request-response-protocol)
- [รายการโหนดและฟังก์ชันการทำงาน (Nodes Specification)](#-รายการโหนดและฟังก์ชันการทำงาน-nodes-specification)
- [ฮาร์ดแวร์และการต่อวงจร (Hardware & Pinouts)](#-ฮาร์ดแวร์และการต่อวงจร-hardware--pinouts)
- [การตั้งค่าและการคอมไพล์ (Configuration & Build)](#-การตั้งค่าและการคอมไพล์-configuration--build)
- [MQTT Topics & API Specification](#-mqtt-topics--api-specification)
- [การแจ้งเตือนและการกู้คืนสถานะ (Alerts & Recovery Logic)](#-การแจ้งเตือนและการกู้คืนสถานะ-alerts--recovery-logic)
- [การแก้ปัญหาเบื้องต้น (Troubleshooting)](#-การแก้ปัญหาเบื้องต้น-troubleshooting)

---

## 🏗️ ภาพรวมสถาปัตยกรรมระบบ (System Architecture)

ระบบประกอบด้วยอุปกรณ์ **Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262)** ทำงานร่วมกัน 3 ระดับ:

```mermaid
graph TD
    subgraph Cloud["Cloud Services"]
        LINE["LINE Messaging / Notify API"]
        MQTT["HiveMQ Cloud Broker (TLS 8883)"]
    end

    subgraph Master["Control Center"]
        M100["Master Node 100<br/>(ESP32-S3 + SX1262)<br/>- OLED Status<br/>- 4-Channel Relays<br/>- WiFi Multi"]
    end

    subgraph Relaying["Signal Relay"]
        R101["Repeater Node 101<br/>(Bi-Directional Forwarder)"]
    end

    subgraph Nodes["Data Acquisition Nodes"]
        N1["Node 1: JD (เจดีย์)"]
        N2["Node 2/3: PhraNon & Plastic"]
        N4["Node 4: ThamYai (ถ้ำใหญ่ - Solar/Batt)"]
        N5["Node 5: RongPui (โรงปุ๋ย - Solar/Batt)"]
        N6["Node 6: TaiFa (ท้ายฝาย)"]
        N7["Node 7: KhonPD (ขอนผาดำ)"]
        N8["Node 8: SuanMM (สวน ม.ม.)"]
        N20["Node 20: NongPog (SPD กันฟ้าผ่า)"]
    end

    M100 <-->|WiFi / Internet| LINE
    M100 <-->|MQTT over TLS| MQTT
    M100 <-->|LoRa Downlink (Poll) & Uplink (Data)| R101
    R101 <-->|LoRa 923MHz SF12| N1
    R101 <-->|LoRa 923MHz SF12| N2
    R101 <-->|LoRa 923MHz SF12| N4
    R101 <-->|LoRa 923MHz SF12| N5
    R101 <-->|LoRa 923MHz SF12| N6
    R101 <-->|LoRa 923MHz SF12| N7
    R101 <-->|LoRa 923MHz SF12| N8
    R101 <-->|LoRa 923MHz SF12| N20
```

---

## 🔄 หลักการทำงานของระบบ Master Polling (Request-Response Protocol)

ระบบใช้สถาปัตยกรรม **Master-Initiated Polling** เพื่อให้ Master ทราบสถานะออนไลน์/ออฟไลน์ของแต่ละโหนดได้ทันที:

1. **รอบการส่งคำขอ (Polling Cycle):**
   * Master Node (100) จะส่งแพ็กเก็ตคำขอ `MSG_TYPE_POLL_REQ` วนหาโหนดปลายทางทีละโหนดผ่าน Repeater (101) ทุกๆ **3 นาที** (`POLL_CYCLE_INTERVAL_MS`) หรือทันทีเมื่อผู้ใช้สั่งผ่าน MQTT (`request_data`)
2. **การตอบกลับของ Data Nodes (Active Listeners):**
   * Data Nodes ทุกตัวจะเปิดรับสัญญาณ LoRa RX ตลอดเวลา
   * เมื่อได้รับ `POLL_REQ` ที่ตรงกับ Node ID ของตนเอง จะทำการอ่านค่าเซ็นเซอร์ (วัดระยะทางอัลตราโซนิก + คำนวณเปอร์เซ็นต์ หรืออ่านค่าสถานะ SPD และแบตเตอรี่) แล้วส่งกลับหา Master ทันที
3. **ระบบ Auto-Retry & Real-Time Fault Detection:**
   * Master รอคำตอบจากแต่ละโหนดเป็นเวลา **4.5 วินาที** (`POLL_TIMEOUT_MS`)
   * หากสัญญาณขาดหายหรือถูกรบกวน Master จะ **ส่งคำขอซ้ำทันทีสูงสุด 2 ครั้ง** (รวมส่งทั้งหมด 3 ครั้ง)
   * หากโหนดไม่ตอบกลับครบทั้ง 3 ครั้ง:
     * Master จะเปลี่ยนสถานะของโหนดนั้นเป็น `STATE_COMM_ERROR` ทันที
     * ส่งข้อความแจ้งเตือนเข้า LINE ทันที (`"แจ้งเตือน: การสื่อสารขัดข้องที่ DataNode ... (ไม่ตอบกลับคำขอ)"`)
     * อัปเดตสถานะขึ้น MQTT ทันทีโดยไม่ต้องรอนาน
4. **การตรวจจับ Repeater ขัดข้อง (Repeater Outage Detection):**
   * หากในรอบการ Poll ไม่มีโหนดใดตอบกลับเลย (0 โหนด) Master จะระบุว่า **Repeater 101 ขัดข้องหรือไฟดับ** และแจ้งเตือน LINE/MQTT ให้ทราบทันที

---

## 📊 รายการโหนดและฟังก์ชันการทำงาน (Nodes Specification)

| Node ID | ชื่องาน / สถานที่ | ประเภทเซ็นเซอร์ | แหล่งจ่ายไฟ | Relay ควบคุม (Master) | รายละเอียด |
| :---: | :--- | :--- | :--- | :---: | :--- |
| **1** | **JD (เจดีย์)** | JSN-SR04T (Ultrasonic) | ไฟหลวง (AC Continuous) | Relay 1 (Pin 7) | วัดระดับน้ำแท็งก์เจดีย์ |
| **2** | **PhraNon (พระนอน)** | JSN-SR04T #1 | ไฟหลวง (AC Continuous) | Relay 2 (Pin 6) | บอร์ดเดียวควบคุม 2 แท็งก์ |
| **3** | **Plastic (พลาสติก)** | JSN-SR04T #2 | ไฟหลวง (AC Continuous) | - | (แชร์บอร์ดกับ Node 2) |
| **4** | **ThamYai (ถ้ำใหญ่)** | JSN-SR04T + ADC Batt | Solar Panel + Battery | Relay 3 (Pin 5) | ตรวจวัดระดับน้ำ + แรงดันแบตเตอรี่ |
| **5** | **RongPui (โรงปุ๋ย)** | JSN-SR04T + ADC Batt | Solar Panel + Battery | Relay 4 (Pin 4) | ตรวจวัดระดับน้ำ + แรงดันแบตเตอรี่ |
| **6** | **TaiFa (ท้ายฝาย)** | JSN-SR04T | ไฟหลวง (AC Continuous) | - | เฝ้าระวังน้ำหมดแท็งก์นานผิดปกติ |
| **7** | **KhonPD (ขอนผาดำ)** | JSN-SR04T | ไฟหลวง (AC Continuous) | - | เฝ้าระวังน้ำหมดแท็งก์นานผิดปกติ |
| **8** | **SuanMM (สวน ม.ม.)** | JSN-SR04T | ไฟหลวง (AC Continuous) | - | เฝ้าระวังน้ำหมดแท็งก์นานผิดปกติ |
| **20** | **NongPog (หนองป๊อก)** | Dry Contact (SPD) | ไฟหลวง (AC Continuous) | - | ตรวจสอบสถานะ Surge Protection |
| **101** | **Repeater** | - | ไฟหลวง (AC Continuous) | - | ทวนสัญญาณ 2 ทิศทาง (Downlink/Uplink) |
| **100** | **Master Panel** | 4x Relays + OLED + WiFi | ไฟหลวง (AC Continuous) | - | ศูนย์ควบคุม, แสดงผล, LINE & MQTT Gateway |

---

## 🔌 ฮาร์ดแวร์และการต่อวงจร (Hardware & Pinouts)

### 1. Data Nodes (Single Ultrasonic: Node 1, 4, 6, 7, 8)
* **Trigger Pin:** GPIO `6`
* **Echo Pin:** GPIO `7`
* **Vext Control (Sensor Power):** GPIO `21` (Active Low)
* **Manual OLED Inspection Button:** GPIO `0` (PRG Button)
* **Battery ADC Reading (เฉพาะ Node 4 & 5):**
  * ADC Input: GPIO `1`
  * ADC Control: GPIO `37` (เปิดวงจร Voltage Divider ก่อนอ่าน)

### 2. Dual Ultrasonic Node (Node 2 & 3: PhraNon & Plastic)
* **Tank 2 (PhraNon):** Trigger = GPIO `46`, Echo = GPIO `45`
* **Tank 3 (Plastic):** Trigger = GPIO `47`, Echo = GPIO `48`
* **Vext Control:** GPIO `21`

### 3. SPD Node (Node 20: NongPog)
* **SPD Status Pin:** GPIO `4` (Input Pull-up, รองรับคอนฟิกแบบ Fail-Safe NC/NO)

### 4. Master Node (Node 100)
* **Relay Outputs (Active Low):**
  * Relay 1 (JD): GPIO `7`
  * Relay 2 (PhraNon): GPIO `6`
  * Relay 3 (ThamYai): GPIO `5`
  * Relay 4 (RongPui): GPIO `4`
* **Protected Reset Button:** GPIO `1` (พร้อมวงจร Hardware RC Filter 2kΩ / 4.7µF, กดค้าง 2 วินาทีเพื่อรีเซ็ต)
* **Status LED (Feedback):** GPIO `35` (White LED บนบอร์ด)
* **OLED Display:** Built-in SSD1306 (SDA = GPIO `17`, SCL = GPIO `18`, RST = GPIO `21`)

---

## ⚙️ การตั้งค่าและการคอมไพล์ (Configuration & Build)

โปรเจกต์นี้จัดการซอร์สโค้ดและไลบรารีผ่าน **PlatformIO**

### โครงสร้าง PlatformIO Environments ใน `platformio.ini`:
```ini
# โหนดตรวจวัดข้อมูล (Data Nodes)
[env:node_1_jd]
[env:node_2_3_phranon_plastic]
[env:node_4_thamyai]
[env:node_5_rongpui]
[env:node_6_taifa]
[env:node_7_khonpd]
[env:node_8_suanmm]
[env:node_20_nongpog]

# โหนดศูนย์กลางและทวนสัญญาณ
[env:repeater_node_101]
[env:master_node_100]

# เครื่องมือทดสอบฮาร์ดแวร์และสัญญาณ
[env:node_test_thamyai_sender]
[env:node_test_repeater_receiver]
[env:Hardware_Test_Pin47]
```

### การตั้งค่าการเชื่อมต่อ (ใน `src/Node_100_Master_MainPannel.cpp`):
* **WiFi Multi-AP:** กำหนด SSID และ Password
* **MQTT Settings:** กำหนด Broker URL, Port 8883 (TLS), Username, Password และ Root CA
* **LINE Messaging API:** กำหนด Channel Access Token และ Target Group/User ID

### คำสั่งคอมไพล์ผ่าน PlatformIO CLI:
```bash
# คอมไพล์ทุก Environment พร้อมกัน
pio run

# คอมไพล์และอัปโหลดเฉพาะ Master Node
pio run -e master_node_100 -t upload

# คอมไพล์และอัปโหลดเฉพาะ Repeater
pio run -e repeater_node_101 -t upload
```

---

## 📡 MQTT Topics & API Specification

### ⚙️ การเชื่อมต่อ Broker
* **Server:** HiveMQ Cloud (Secure TLS Port 8883)
* **Protocol:** MQTTS with ISRG Root X1 Certificate

### 📤 Telemetry Publish Topics (Master ➔ Dashboard / Node-RED)

| Topic | Description | Payload Format |
| :--- | :--- | :--- |
| `JD` | ระดับน้ำแท็งก์เจดีย์ | `0` - `100` (%) (หรือ `-1` เมื่อ Error) |
| `PhraNon` | ระดับน้ำแท็งก์พระนอน | `0` - `100` (%) |
| `Plastic` | ระดับน้ำแท็งก์พลาสติก | `0` - `100` (%) |
| `ThamYai` | ระดับน้ำแท็งก์ถ้ำใหญ่ | `0` - `100` (%) |
| `RongPui` | ระดับน้ำแท็งก์โรงปุ๋ย | `0` - `100` (%) |
| `TaiFa` | ระดับน้ำแท็งก์ท้ายฝาย | `0` - `100` (%) |
| `KhonPD` | ระดับน้ำแท็งก์ขอนผาดำ | `0` - `100` (%) |
| `SuanMM` | ระดับน้ำแท็งก์สวน ม.ม. | `0` - `100` (%) |
| `NongPog` | สถานะกันฟ้าผ่า (SPD) | `0` = ปกติ, `1` = เสีย/ชำรุด |
| `ThamYai/battery` | แรงดันแบตเตอรี่ถ้ำใหญ่ | `0.00` - `4.20` (V) |
| `RongPui/battery` | แรงดันแบตเตอรี่โรงปุ๋ย | `0.00` - `4.20` (V) |
| `notification_status` | สถานะระบบแจ้งเตือน | `1` = เปิดใช้งาน, `0` = ปิด |
| `message` | บันทึกข้อความและ Log สถานะ | String ข้อความอธิบายสถานะ |

### 📥 Remote Control Subscribe Topics (Dashboard / App ➔ Master)

| Topic | Description | Payload |
| :--- | :--- | :--- |
| `request_data` | สั่งให้ Master ส่ง Poll หาเซ็นเซอร์ทุกตัวและอัปเดตข้อมูลทันที | ส่ง `1` |
| `notification_control` | เปิด/ปิด ระบบแจ้งเตือนและการทำงานของ Relay | `1` = เปิด, `0` = ปิด |
| `reset_notification` | รีเซ็ตสถานะแจ้งเตือนและปลด Relay ทุกตัว | ส่ง `1` |
| `reset_system` | สั่ง Reboot บอร์ด Master จากระยะไกล | ส่ง `1` |

---

## 🔔 การแจ้งเตือนและการกู้คืนสถานะ (Alerts & Recovery Logic)

1. **ระดับน้ำต่ำกว่าเกณฑ์ (Low Water Threshold):**
   * ตรวจสอบค่าต่ำกว่า Threshold ต่อเนื่อง 3 ครั้ง
   * สั่งเปิด Relay ประจำแท็งก์ และส่งข้อความเตือนเข้า LINE
2. **น้ำหมดแท็งก์ผิดปกติ (TaiFa, KhonPD, SuanMM):**
   * หากระดับน้ำต่ำกว่า 10% ต่อเนื่องเกิน 2 ชั่วโมง จะส่งแจ้งเตือนพิเศษ
3. **แบตเตอรี่ต่ำ (Low Battery Alert):**
   * สำหรับ Node 4 และ Node 5 หากแบตเตอรี่ต่ำกว่า `3.40V` ระบบจะส่งแจ้งเตือน
4. **การกู้คืนระบบ (Auto-Recovery & Grace Period):**
   * เมื่อโหนดที่เคยขาดการติดต่อตอบกลับสำเร็จ Master จะเปลี่ยนสถานะกลับเป็น `STATE_NORMAL` ทันที พร้อมส่ง LINE แจ้งเตือนว่าโหนดกลับมาออนไลน์ และอัปเดตสถานะบน MQTT ทันที

---

## 🛠️ การแก้ปัญหาเบื้องต้น (Troubleshooting)

* **ระดับน้ำแสดง -1 หรือค่าคงที่ผิดปกติ:**
  * ตรวจสอบสิ่งกีดขวางหน้าเซ็นเซอร์ Ultrasonic JSN-SR04T (ระยะบอด Dead Zone คือต่ำกว่า 22 cm)
  * ตรวจสอบสาย Trigger/Echo และไฟเลี้ยง Vext
* **ขึ้นแจ้งเตือนการสื่อสารขัดข้องพร้อมกันทุกโหนด:**
  * แสดงว่า **Repeater 101** ไฟดับหรือมีปัญหา ให้ตรวจสอบจุดติดตั้ง Repeater
* **LINE ไม่แจ้งเตือน:**
  * ตรวจสอบการเชื่อมต่ออินเทอร์เน็ตของ Master Node บนหน้าจอ OLED
  * ตรวจสอบอายุและสิทธิ์ของ LINE Access Token
* **ปุ่มกด Reset บน Master ไม่ทำงาน:**
  * ปุ่มต้อง **กดค้างอย่างน้อย 2 วินาที** (เพื่อป้องกัน Noise ทางไฟฟ้าจากสายยาว) ไฟ LED สีขาว (Pin 35) จะติดสว่างระหว่างกดเพื่อยืนยันการกดปุ่ม

---

**Wat Doi Water & SPD Monitoring System**  
*Built for robustness, remote mountain terrain reliability, and real-time fault tolerance.*