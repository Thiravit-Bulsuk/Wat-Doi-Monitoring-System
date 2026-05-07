# Wat Doi Monitoring System 🌊⚡

ระบบตรวจสอบระดับน้ำและสถานะกันฟ้าผ่า (SPD) ระยะไกลผ่านเครือข่าย LoRa และแจ้งเตือนผ่าน LINE/MQTT สำหรับวัดดอย

## 📋 ภาพรวมระบบ (System Overview)

โปรเจกต์นี้พัฒนาขึ้นเพื่อแก้ปัญหาการตรวจสอบระดับน้ำในแท็งค์ที่กระจายตัวอยู่ตามจุดต่างๆ บนภูเขา และตรวจสอบสถานะของอุปกรณ์ป้องกันฟ้าผ่า (SPD) โดยใช้เทคโนโลยี **LoRa (Long Range)** ในการสื่อสารระยะไกลที่ประหยัดพลังงาน

### โครงสร้างการทำงาน (Architecture)
1.  **Data Nodes (Nodes 1, 2, 4, 5, 6):**
    *   ติดตั้ง ณ จุดวัดต่างๆ (แท็งค์น้ำ, ตู้ไฟ)
    *   อ่านค่าจากเซ็นเซอร์ (Ultrasonic หรือ Dry Contact)
    *   ส่งข้อมูลผ่าน LoRa ไปยัง Repeater
    *   ทำงานในโหมด **Deep Sleep** เพื่อประหยัดพลังงาน (ตื่นมาส่งข้อมูลทุก 5 นาที)
2.  **Repeater Node (Node 10):**
    *   ติดตั้ง ณ จุดกึ่งกลางที่รับสัญญาณได้ดี
    *   ทำหน้าที่รับช่วงต่อสัญญาณ (Relay) จาก Data Nodes ส่งไปยัง Master Node
    *   ทำงานตลอดเวลา (Always On)
3.  **Master Node (Node 100):**
    *   ติดตั้ง ณ สำนักงาน/กุฏิ
    *   รับข้อมูลจาก Repeater แสดงผลบนหน้าจอ OLED
    *   เชื่อมต่อ WiFi เพื่อส่งข้อมูลขึ้น **MQTT Broker** (HiveMQ)
    *   แจ้งเตือนผ่าน **LINE Notify** เมื่อเกิดเหตุผิดปกติ (น้ำลดต่ำ, SPD เสีย, หรือขาดการติดต่อ)
    *   ควบคุม Relay เพื่อสั่งงานปั๊มน้ำหรือวาล์วตามเงื่อนไข

---

## 🛠️ ฮาร์ดแวร์ที่ต้องใช้ (Hardware Requirements)

*   **Microcontroller:** Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262) จำนวน 7 ชุด
*   **Sensors:**
    *   **Ultrasonic JSN-SR04T (Waterproof):** สำหรับวัดระดับน้ำ (Node 1, 2, 3, 4)
    *   **Dry Contact / Switch:** สำหรับเช็คสถานะ SPD (Node 6)
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

#### 📍 Data Node 6 (NongPog - SPD)
*   **SPD Status Pin:** GPIO 4 (ต่อแบบ Input Pull-up)
    *   Note: สามารถตั้งค่า Logic (NO/NC) ได้ในไฟล์ `Node_06_SPD_NongPog.cpp`

#### 📍 Master Node 100
*   **Relays:**
    *   Relay 1: GPIO 7
    *   Relay 2: GPIO 6
    *   Relay 3: GPIO 5
    *   Relay 4: GPIO 4
*   **Reset Button:** GPIO 47 (กดค้าง 2 วินาทีเพื่อรีเซ็ตการแจ้งเตือน)

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
แก้ไขไฟล์ src/Node_100_Master_MainPannel.cpp เพื่อตั้งค่าเครือข่าย:
```cpp
// WiFi Settings
#define WIFI_SSID "SolarCell"
#define WIFI_PASSWORD "12345678"

// MQTT Broker Settings
const char* mqtt_server = "aa9e03df167b40b086069a385503cf2f.s1.eu.hivemq.cloud";
const char* mqtt_username = "thiravit";
const char* mqtt_password = "@16Jan2563";
const int mqtt_port = 8883;

// LINE Notify Token
const char* LINE_ACCESS_TOKEN = "Vv9JoO8Af3WbpW3iozJEmzXj0IdGRVHruYM1TE6umoFn5OWqTDelb05Pm0oah8FmHV/lDx0pmmLDm5HZ6umXSWJV8zrmh4c8wtpZcCLObqdUWqPdOYN1XKG+GoPN0A6BBW+fNXY/s2AEJA1HQR1HkQdB04t89/1O/w1cDnyilFU=";
const char* LINE_USER_ID = "C193952f60a4c2951926955db63570e03";
```

#### MQTT Root CA Certificate (ISRG Root X1)
สำหรับการเชื่อมต่อผ่าน TLS (Port 8883) จำเป็นต้องระบุ Root CA ในโค้ด เพื่อใช้ตรวจสอบความปลอดภัยของ Broker:

```cpp
static const char *root_ca PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)EOF";

```

### 4. การอัปโหลด (Upload)
เลือก Environment ที่ต้องการ Upload ผ่านแถบสถานะด้านล่างของ VS Code หรือเมนู Project Tasks:
*   `env:node_1_jd`
*   `env:node_2_3_phranon_plastic`
*   `env:node_4_thamyai`
*   `env:node_5_rongpui`
*   `env:node_6_nongpog`
*   `env:repeater_node_10`
*   `env:master_node_100`
*   `env:node_test_repeater_receiver` (สำหรับทดสอบ LoRa Receiver ของ Repeater Node 10)
*   `env:node_test_thamyai_sender` (สำหรับทดสอบ LoRa Sender ของ Node 4 - ThamYai)

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
| `Plastic` | ระดับน้ำแท็งค์พลาสติก | `0` - `100` (%) |
| `ThamYai` | ระดับน้ำแท็งค์ถ้ำใหญ่ | `0` - `100` (%) |
| `JD/battery` | แรงดันแบตเตอรี่แท็งค์เจดีย์ | `0.00` - `4.20` (V) |
| `PhraNon/battery` | แรงดันแบตเตอรี่แท็งค์พระนอน | `0.00` - `4.20` (V) |
| `Plastic/battery` | แรงดันแบตเตอรี่แท็งค์พลาสติก | `0.00` - `4.20` (V) |
| `ThamYai/battery` | แรงดันแบตเตอรี่แท็งค์ถ้ำใหญ่ | `0.00` - `4.20` (V) |
| `RongPui` | ระดับน้ำแท็งค์โรงปุ๋ย | `0` - `100` (%) |
| `RongPui/battery` | แรงดันแบตเตอรี่แท็งค์โรงปุ๋ย | `0.00` - `4.20` (V) |
| `NongPog` | สถานะกันฟ้าผ่า (SPD) | `0` = ปกติ, `1` = เสีย |
| `NongPog/battery` | แรงดันแบตเตอรี่ SPD หนองพอก | `0.00` - `4.20` (V) |
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
*   **SPD แจ้งเตือนผิดพลาด:** ตรวจสอบการตั้งค่า `SPD_IS_NC` ในโค้ด Node 6 ให้ตรงกับชนิดของอุปกรณ์ (NO/NC)

---

**Developed for Wat Doi Monitoring Project**