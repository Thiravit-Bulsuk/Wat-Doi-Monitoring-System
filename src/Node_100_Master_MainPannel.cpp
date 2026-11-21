#include <SPI.h>  
#include <SimpleTimer.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <SX126XLT.h>
#include <HT_SSD1306Wire.h>
#include <Arduino.h>
#include <ProgramLT_Definitions.h>
#include "NodeConfig.h"
#include <time.h>
#include <freertos/queue.h>
#include <HTTPClient.h>
#if __has_include("secrets.h")
#include "secrets.h"
#endif
// Web app history upload (logs + levels kept 3 days); leave HISTORY_TOKEN empty to disable.
#ifndef HISTORY_URL
#define HISTORY_URL ""
#endif
#ifndef HISTORY_TOKEN
#define HISTORY_TOKEN ""
#endif
#if !defined(WIFI_SSID) || !defined(WIFI_PASSWORD) || !defined(MQTT_SERVER) || !defined(MQTT_USERNAME) || !defined(MQTT_PASSWORD) || !defined(LINE_CHANNEL_ACCESS_TOKEN) || !defined(LINE_USER_ID_SECRET) || !defined(LINE_ADMIN_USER_ID_SECRET)
#error "Copy include/secrets.example.h to include/secrets.h and fill in the credentials"
#endif

void sendLineMessage(String message);
void sendAdminMessage(String message);
void resetSystem();
void ResetTimerTask(void *parameter);
void IRAM_ATTR handleButtonPress();
bool initLoRa();
void processReceivedPacket(const LoRaPacket &packet);
bool pollSingleNode(int nodeIdx);
bool pollNodeWithRetries(int nodeIdx);
void pollAllNodes();
void activateRelay(uint8_t nodeID);
void resetNotification();
void VextON(void);
void VextOFF(void);
void displayData(String tankName, String message);
void DisplayTimerTask(void *pvParameters);
void reconnect();
void callback(char* topic, byte* payload, unsigned int length);
void publishMessage(const char* topic, String payload , boolean retained);
void sendMQTTData();
bool checkInternetConnection();
void MainTask(void *parameter);
void DisplayControlButtonTask(void *parameter);
void handleNodeRecovery(int nodeIdx);
void enqueueLineTextMessage(const String &message);

// Task handles
TaskHandle_t TaskMain;
TaskHandle_t TaskResetTimer; // Task Handle สำหรับรีเซ็ตบอร์ด
TaskHandle_t TaskDisplay;
TaskHandle_t TaskButton;

// Timer สำหรับการอัปเดตหน้าจอ
SimpleTimer displayTimer;

// ตัวแปรสำหรับสถานะการแสดงผล (วนรอบระหว่าง 0-12)
// ตัวแปร global track state และพักหน้าจอ
volatile int displayPageState = 0;     // 0..12 คือข้อมูลแต่ละหน้า
volatile int displayNodeIndex = 0;     // ตัวชี้ Node ที่กำลังแสดงผล
const TickType_t displayInterval = 3500 / portTICK_PERIOD_MS; // 3500 ms
const TickType_t pauseInterval   = 500 / portTICK_PERIOD_MS;  // 100 ms

unsigned long lastStateChange = 0;  // เก็บเวลาที่เปลี่ยนสถานะครั้งล่าสุด

// ประกาศตัวแปรเก็บสถานะเน็ต (ต้องประกาศให้ถูกต้องตามโปรเจคคุณ)
volatile bool internetConnected = false;  // กำหนดค่าเริ่มต้นตามสถานะจริงของเน็ต

// ==== System Timing Constants ====
const unsigned long RECOVERY_GRACE_PERIOD = 900000; // 15 นาที (งดแจ้งเตือนราย Node หลัง Repeater ฟื้น)
const float BATTERY_LOW_THRESHOLD = 2.40;           // แจ้งเตือนเมื่อแบตเตอรี่ต่ำกว่า 2.4V
const float BATTERY_RECOVER_THRESHOLD = 3.50;        // ต้องขึ้นเกิน 3.5V ก่อนถึงจะถือว่ากลับมาปกติ (กันแจ้งเตือนรัวๆ ที่ค่าแกว่งใกล้ threshold)

// ==== LINE Messaging API Settings ====
const char* LINE_ACCESS_TOKEN = LINE_CHANNEL_ACCESS_TOKEN;
const char* LINE_API_URL = "https://api.line.me/v2/bot/message/push";
const char* LINE_USER_ID = LINE_USER_ID_SECRET; // ผู้ใช้งานทั่วไป: รับแจ้งเตือนระดับน้ำ/สถานะ SPD
const char* LINE_ADMIN_USER_ID = LINE_ADMIN_USER_ID_SECRET; // Admin: รับแจ้งเตือนการทำงานของระบบ (Comm Error, Repeater, แบตเตอรี่)

//---- MQTT Broker settings
const char* mqtt_server = MQTT_SERVER; // EMQX Serverless (deployment-a7821161)
const char* mqtt_username = MQTT_USERNAME;
const char* mqtt_password = MQTT_PASSWORD;
const int mqtt_port = 8883;

WiFiClientSecure espClient;
PubSubClient client(espClient); // เข้าถึง client/espClient ได้จาก MainTask เท่านั้น ที่อื่นต้องส่งผ่านคิว/แฟล็กด้านล่าง

// คิวสำหรับข้อความ LINE ที่ต้องประกาศต่อขึ้น MQTT (ผลิตจากหลาย Task, บริโภคโดย MainTask เท่านั้น)
#define MQTT_MSG_MAX_LEN 200
struct MqttTextMessage {
    char text[MQTT_MSG_MAX_LEN];
};
QueueHandle_t mqttMessageQueue = NULL;
QueueHandle_t displayWakeQueue = NULL;

// คิว log ที่จะส่งขึ้นเว็บแอป (ผลิตจากหลาย Task, MainTask เป็นผู้ส่ง)
#define HISTORY_UPLOAD_INTERVAL_MS 60000
#define HISTORY_LOG_BUFFER 30
#define HISTORY_SAMPLE_BUFFER 20
struct HistoryLog {
    uint32_t time;
    char text[MQTT_MSG_MAX_LEN];
};
QueueHandle_t historyLogQueue = NULL;
void queueHistoryLog(const String &message);

// แฟล็กขอให้ MainTask เรียก sendMQTTData() รอบถัดไป (ตั้งจาก Task อื่นได้ อ่าน/เคลียร์โดย MainTask เท่านั้น)
volatile bool pendingMQTTDataSync = false;

bool repeaterErrorActive = false;    // สถานะแจ้งเตือน Repeater ขัดข้อง
uint8_t repeaterFailureCount = 0;    // จำนวนรอบที่ไม่มี Node ใดตอบติดต่อกัน
unsigned long lastRepeaterRecoveryTime = 0; // เวลาที่ Repeater กลับมาปกติครั้งล่าสุด
unsigned long lastMsg = 0;
unsigned long lastRepeaterSeen = 0; // เวลาที่ได้รับข้อมูลล่าสุดผ่าน Repeater (ใช้ตรวจจับ Repeater ไฟดับ)

#define MSG_BUFFER_SIZE (50)
char msg[MSG_BUFFER_SIZE];

const char* message_topic = "message";
const char* notification_control_topic = "notification_control";
const char* notification_status_topic = "notification_status";
const char* request_data_topic = "request_data";
const char* master_status_topic = "master_status";           // LWT: "online"/"offline" สำหรับให้แอปเช็คว่า Master ยังออนไลน์อยู่จริงไหม
const char* master_last_update_topic = "master_last_update";  // เวลาจริง (NTP) ที่ publish ชุดข้อมูลล่าสุด เอาไว้เทียบกับเวลาปัจจุบันในแอปเอง
const char* link_quality_topic_prefix = "link_quality/";      // RSSI/SNR จาก Node ถึง Repeater

// Flag to track when new data is received
volatile bool newDataReceived = false;
volatile bool mqttResetNotification = false;
volatile bool mqttResetSystem = false;
volatile bool notificationsEnabled = true;
volatile bool mqttDisableNotifications = false;
volatile bool mqttEnableNotifications = false;
volatile bool mqttRequestData = false;

// DigiCert Global Root G2 (EMQX Serverless *.emqxsl.com), หมดอายุ 2038-01-15
static const char *root_ca PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH
MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI
2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx
1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ
q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz
tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ
vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP
BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV
5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY
1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4
NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG
Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91
8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe
pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl
MrY=
-----END CERTIFICATE-----
)EOF";

// GTS Root R1 (Cloud Run *.run.app) สำหรับส่ง history ขึ้นเว็บแอป, หมดอายุ 2036-06-22
static const char *gts_root_r1 PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw
MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp
Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA
A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo
27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w
Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw
TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl
qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH
szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8
Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk
MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92
wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p
aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN
VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID
AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E
FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb
C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe
QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy
h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4
7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J
ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef
MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/
Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT
6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ
0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm
2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb
bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c
-----END CERTIFICATE-----
)EOF";

// GTS Root R4 (live Cloud Run certificate chain)
static const char *gts_root_r4 PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDejCCAmKgAwIBAgIQf+UwvzMTQ77dghYQST2KGzANBgkqhkiG9w0BAQsFADBX
MQswCQYDVQQGEwJCRTEZMBcGA1UEChMQR2xvYmFsU2lnbiBudi1zYTEQMA4GA1UE
CxMHUm9vdCBDQTEbMBkGA1UEAxMSR2xvYmFsU2lnbiBSb290IENBMB4XDTIzMTEx
NTAzNDMyMVoXDTI4MDEyODAwMDA0MlowRzELMAkGA1UEBhMCVVMxIjAgBgNVBAoT
GUdvb2dsZSBUcnVzdCBTZXJ2aWNlcyBMTEMxFDASBgNVBAMTC0dUUyBSb290IFI0
MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAE83Rzp2iLYK5DuDXFgTB7S0md+8Fhzube
Rr1r1WEYNa5A3XP3iZEwWus87oV8okB2O6nGuEfYKueSkWpz6bFyOZ8pn6KY019e
WIZlD6GEZQbR3IvJx3PIjGov5cSr0R2Ko4H/MIH8MA4GA1UdDwEB/wQEAwIBhjAd
BgNVHSUEFjAUBggrBgEFBQcDAQYIKwYBBQUHAwIwDwYDVR0TAQH/BAUwAwEB/zAd
BgNVHQ4EFgQUgEzW63T/STaj1dj8tT7FavCUHYwwHwYDVR0jBBgwFoAUYHtmGkUN
l8qJUC99BM00qP/8/UswNgYIKwYBBQUHAQEEKjAoMCYGCCsGAQUFBzAChhpodHRw
Oi8vaS5wa2kuZ29vZy9nc3IxLmNydDAtBgNVHR8EJjAkMCKgIKAehhxodHRwOi8v
Yy5wa2kuZ29vZy9yL2dzcjEuY3JsMBMGA1UdIAQMMAowCAYGZ4EMAQIBMA0GCSqG
SIb3DQEBCwUAA4IBAQAYQrsPBtYDh5bjP2OBDwmkoWhIDDkic574y04tfzHpn+cJ
odI2D4SseesQ6bDrarZ7C30ddLibZatoKiws3UL9xnELz4ct92vID24FfVbiI1hY
+SW6FoVHkNeWIP0GCbaM4C6uVdF5dTUsMVs/ZbzNnIdCp5Gxmx5ejvEau8otR/Cs
kGN+hr/W5GvT1tMBjgWKZ1i4//emhA1JG1BbPzoLJQvyEotc03lXjTaCzv8mEbep
8RqZ7a2CPsgRbuvTPBwcOMBBmuFeU88+FSBX6+7iP0il8b4Z0QFqIwwMHfs/L6K1
vepuoxtGzi4CZ68zJpiq1UvSqTbFJjtbD4seiMHl
-----END CERTIFICATE-----
)EOF";

// Define LoRa settings
#define LORA_DEVICE DEVICE_SX1262
#define NSS 8
#define NRESET 12
#define RFBUSY 13
#define DIO1 14

// Define relay and reset pin
#define RELAY_PIN_1 7
#define RELAY_PIN_2 6
#define RELAY_PIN_3 5
#define RELAY_PIN_4 4

#define RESET_BUTTON_PIN 1  // ย้ายจาก 47 มาขา 1 เนื่องจากขา 47 เสีย (ตอนนี้ใช้เพิ่มความสว่างจอชั่วคราวเท่านั้น reset notification ย้ายไปสั่งผ่าน MQTT อย่างเดียว)
const uint8_t DISPLAY_BRIGHTNESS_IDLE = 6;     // ความสว่างขณะพัก ~10% (0-255)
const uint8_t DISPLAY_BRIGHTNESS_ACTIVE = 255; // ความสว่างระหว่างแสดงข้อมูล ~100%
const unsigned long DISPLAY_BOOST_DURATION_MS = 120000; // ระยะเวลาความสว่างสูงหลังกดปุ่ม (2 นาที)
const unsigned long LED_HEARTBEAT_INTERVAL_MS = 20000; // กระพริบ LED ทุก 20 วินาทีเมื่อจอปิด
const unsigned long LED_HEARTBEAT_ON_MS = 250;          // ระยะเวลาที่ LED ติดในแต่ละรอบ

WiFiMulti wifiMulti;

#define AUTO_RESET_INTERVAL_MS 43200000 // 12 ชั่วโมงใน milliseconds (12 * 60 * 60 * 1000) เพื่อล้าง heap fragmentation สะสมเป็นระยะ

static SSD1306Wire  display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED); // addr , freq , i2c group , resolution , rst

const uint32_t Frequency = 923000000;           // frequency of transmissions
const uint32_t Offset = 0;                      // offset frequency for calibration purposes

const uint8_t Bandwidth = LORA_BW_125;          // LoRa bandwidth
const uint8_t SpreadingFactor = LORA_SF12;      // LoRa spreading factor
const uint8_t CodeRate = LORA_CR_4_5;           // LoRa coding rate
const uint8_t Optimisation = LDRO_AUTO;         // low data rate optimisation setting

const int8_t TXpower = 14;                      // LoRa transmit power in dBm

SX126XLT LT;

// การตั้งค่า Node ทั้งหมดในระบบ (เพิ่ม Node 8, 9, 10 ได้ที่นี่)
Node nodes[] = { // กำหนด Node ทั้งหมดในระบบ
    {1, "JD",       "JD",       50, RELAY_PIN_1, MSG_TYPE_WATER, -1, 0, 0.0, STATE_NORMAL, STATE_NORMAL, 0, false, 0},
    {2, "PhraNon",  "PhraNon",  50, RELAY_PIN_2, MSG_TYPE_WATER, -1, 0, 0.0, STATE_NORMAL, STATE_NORMAL, 0, false, 0},
    {3, "Plastic",  "Plastic",  10, 0,           MSG_TYPE_WATER, -1, 0, 0.0, STATE_NORMAL, STATE_NORMAL, 0, false, 0},
    {4, "ThamYai",  "ThamYai",  50, RELAY_PIN_3, MSG_TYPE_WATER, -1, 0, 0.0, STATE_NORMAL, STATE_NORMAL, 0, false, 0},
    {5, "RongPui",  "RongPui",  50, RELAY_PIN_4, MSG_TYPE_WATER, -1, 0, 0.0, STATE_NORMAL, STATE_NORMAL, 0, false, 0},
    {6, "TaiFa",    "TaiFa",    10, 0,           MSG_TYPE_WATER, -1, 0, 0.0, STATE_NORMAL, STATE_NORMAL, 0, false, 0},
    {7, "KhonPD",   "KhonPD",   10, 0,           MSG_TYPE_WATER, -1, 0, 0.0, STATE_NORMAL, STATE_NORMAL, 0, false, 0},
    {8, "SuanMM",   "SuanMM",   10, 0,           MSG_TYPE_WATER, -1, 0, 0.0, STATE_NORMAL, STATE_NORMAL, 0, false, 0},
    {20, "NongPog", "NongPog",  0,  0,           MSG_TYPE_SPD,   -1, 0, 0.0, STATE_NORMAL, STATE_NORMAL, 0, false, 0}
    // สามารถเพิ่ม Node ใหม่ได้ที่นี่ เช่น {9, "Tank9", "Tank9", 50, 0, MSG_TYPE_WATER, 0, 0, STATE_NORMAL, STATE_NORMAL, 0, false}
};

const int numNodes = sizeof(nodes) / sizeof(nodes[0]);
volatile int16_t nodeToRepeaterRssi[numNodes];
volatile int8_t nodeToRepeaterSnr[numNodes];
unsigned long nodeLowWaterStartTime[numNodes] = {0}; // จับเวลาสำหรับเงื่อนไขพิเศษของ TaiFa, KhonPD, SuanMM
const int LOW_THRESHOLD = 10; // จำนวนครั้งที่ต้องอ่านค่าต่ำกว่า Threshold ติดกันเพื่อแจ้งเตือน
const uint8_t COMM_FAILURE_THRESHOLD = 3; // ต้องไม่ตอบติดต่อกันกี่รอบจึงแจ้งเตือน Comm Error
const uint8_t REPEATER_FAIL_NODE_THRESHOLD = 3; // หากมีปัญหาการสื่อสารพร้อมกันตั้งแต่ 3 Node ขึ้นไป ให้ถือเป็นปัญหาที่ Repeater (>=3 ป้องกันกรณีบอร์ดคู่ PhraNon+Plastic ขัดข้อง)

volatile bool buttonPressed = false;     // สถานะว่าปุ่มถูกกดหรือไม่
volatile bool displaySessionActive = false;    // สถานะว่ากำลังแสดงข้อมูลบนจออยู่หรือไม่
volatile unsigned long displaySessionStartTime = 0; // เวลาเริ่ม session แสดงข้อมูลล่าสุด
unsigned long lastLedHeartbeatTime = 0;
unsigned long ledHeartbeatStartTime = 0;
bool ledHeartbeatOn = false;

// ฟังก์ชันเช็คว่าออกเน็ตได้จริงหรือไม่ โดยการลอง Connect ไปที่ Google DNS (8.8.8.8)
bool checkInternetConnection() {
    WiFiClient testClient; // เปลี่ยนชื่อตัวแปรเพื่อไม่ให้สับสนกับ client ของ MQTT
    testClient.setTimeout(2); // Set timeout to 2 seconds to avoid blocking MainTask
    // ลองเชื่อมต่อ port 53 (DNS) ของ Google
    if (testClient.connect("8.8.8.8", 53)) {
        testClient.stop();
        return true;
    }
    return false;
}

// คืนค่า วันที่-เวลาปัจจุบันตาม NTP เพื่อให้แอปมือถือเทียบกับเวลาปัจจุบันได้เอง (คืนสตริงว่างถ้ายังไม่ sync)
String getFormattedTime() {
    time_t now;
    time(&now);
    if (now < 1700000000) { // ยังไม่ sync NTP (epoch เริ่มต้นจะเป็นค่าเล็กมาก)
        return "";
    }
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char buf[25];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(buf);
}

// ==== ฟังก์ชันส่งข้อความไปที่ LINE (ใช้ภายในเท่านั้น ระบุปลายทางได้ทั้ง user/admin) ====
void sendLineMessageTo(const char* userId, String message) {
    if (!notificationsEnabled) {
        Serial.println("Notifications disabled. Skipping LINE message.");
        queueHistoryLog(message + " (ไม่ส่ง LINE: ปิดการแจ้งเตือน)");
        return;
    }

    if (!internetConnected) {
        Serial.println("No Internet. Skipping LINE message.");
        queueHistoryLog(message + " (ไม่ส่ง LINE: ไม่มีอินเทอร์เน็ต)");
        return;
    }

    WiFiClientSecure lineClient;
    lineClient.setInsecure(); // ข้ามการตรวจสอบใบรับรอง SSL

    if (!lineClient.connect("api.line.me", 443)) {
        Serial.println("LINE API Connection Failed");
        queueHistoryLog(message + " (ส่ง LINE ไม่สำเร็จ)");
        return;
    }

    String payload = "{";
    payload += "\"to\": \"" + String(userId) + "\",";
    payload += "\"messages\": [{";
    payload += "\"type\": \"text\",";
    payload += "\"text\": \"" + message + "\"";
    payload += "}]}";

    String request = "POST /v2/bot/message/push HTTP/1.1\r\n";
    request += "Host: api.line.me\r\n";
    request += "Authorization: Bearer " + String(LINE_ACCESS_TOKEN) + "\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + String(payload.length()) + "\r\n";
    request += "\r\n";
    request += payload;

    lineClient.print(request);

    // อ่าน Response จาก Server เพื่อตรวจสอบสถานะ (เช่น กรณีโควต้าหมด)
    String statusLine = "";
    while (lineClient.connected()) {
        String line = lineClient.readStringUntil('\n');
        if (statusLine == "") statusLine = line; // เก็บ HTTP Status Line เช่น HTTP/1.1 429 Too Many Requests
        if (line == "\r") {
            break;
        }
    }

    if (statusLine.indexOf("200") != -1) { // 200 คือ HTTP Status OK (ส่งสำเร็จ) ไม่ใช่จำนวนข้อความ
        Serial.println("LINE Message Sent: " + message);
        enqueueLineTextMessage(message);
    } else {
        Serial.println("LINE API Error: " + statusLine);
        queueHistoryLog(message + " (ส่ง LINE ไม่สำเร็จ)");
        String errorAlert = "ระบบ: ส่ง LINE ไม่สำเร็จ (" + statusLine.substring(9) + ")";
        if (statusLine.indexOf("429") != -1) { // 429 คือ HTTP Status Too Many Requests (โควต้าเต็ม)
            errorAlert = "แจ้งเตือน: โควต้า LINE Messaging API เต็มแล้ว (HTTP 429)!";
        }
        enqueueLineTextMessage(errorAlert);
    }

    lineClient.stop();
}

// แจ้งเตือนผู้ใช้งานทั่วไป: ระดับน้ำ, สถานะ SPD ฯลฯ
void sendLineMessage(String message) {
    sendLineMessageTo(LINE_USER_ID, message);
}

// แจ้งเตือน Admin: สถานะการทำงานของระบบ (Comm Error, Repeater, แบตเตอรี่)
void sendAdminMessage(String message) {
    sendLineMessageTo(LINE_ADMIN_USER_ID, message);
}

// ส่งข้อความเข้าคิวให้ MainTask เป็นผู้ publish ขึ้น MQTT แต่เพียงผู้เดียว (ปลอดภัยเมื่อเรียกจากหลาย Task)
void enqueueLineTextMessage(const String &message) {
    if (mqttMessageQueue == NULL) return;
    MqttTextMessage item;
    message.toCharArray(item.text, MQTT_MSG_MAX_LEN);
    if (xQueueSend(mqttMessageQueue, &item, 0) != pdTRUE) {
        Serial.println("WARNING: mqttMessageQueue full, dropping message: " + message);
    }
}


// ฟังก์ชันสำหรับรีเซ็ตระบบ
void resetSystem() {
    Serial.println("System resetting...");
    delay(1000); // หน่วงเวลาเพื่อให้ข้อมูลถูกส่งออกก่อน
    ESP.restart(); // คำสั่งสำหรับรีเซ็ตบอร์ด
}

// Task สำหรับรีเซ็ตระบบอัตโนมัติ
void ResetTimerTask(void *parameter) {
    unsigned long lastResetTime = millis();
    while (true) {
        // ตรวจสอบว่าถึงเวลาที่จะรีเซ็ตหรือยัง
        if (millis() - lastResetTime >= AUTO_RESET_INTERVAL_MS) {
            resetSystem(); // เรียกใช้ฟังก์ชันรีเซ็ตระบบ
            lastResetTime = millis(); // อัปเดตเวลาล่าสุด
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS); // หน่วงเวลา 1 วินาที
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

// Function to process a received data packet
void processReceivedPacket(const LoRaPacket &packet) {
    Serial.print("RX Data -> MessageType: "); Serial.println(packet.messageType);
    Serial.print("nodeID: "); Serial.println(packet.sourceNodeID);
    Serial.print("Value: "); Serial.println(packet.value);
    Serial.print("Battery: "); Serial.println(packet.battery);

    lastRepeaterSeen = millis();

    for (int i = 0; i < numNodes; i++) {
        if (nodes[i].id == packet.sourceNodeID) {
            nodes[i].lastValue = (int)packet.value;
            nodes[i].lastSeen = millis();
            nodes[i].battery = packet.battery; // บันทึกค่าแบตเตอรี่ที่ได้รับ
            nodes[i].commFailureCount = 0;

            // แจ้งเตือนแบตเตอรี่ต่ำ (เฉพาะ ThamYai ID:4 และ RongPui ID:5) - ส่งครั้งเดียวจนกว่าแบตจะกลับมาปกติ
            if ((nodes[i].id == 4 || nodes[i].id == 5) &&
                nodes[i].battery > 0 &&
                nodes[i].battery < BATTERY_LOW_THRESHOLD &&
                !nodes[i].batteryAlertSent) {
                sendAdminMessage("แจ้งเตือน: DataNode " + nodes[i].name + " แบตเตอรี่ต่ำ (" + String(nodes[i].battery, 2) + "V)");
                nodes[i].batteryAlertSent = true;
            } else if ((nodes[i].id == 4 || nodes[i].id == 5) &&
                       nodes[i].battery >= BATTERY_RECOVER_THRESHOLD &&
                       nodes[i].batteryAlertSent) {
                sendAdminMessage("แจ้งเตือน: DataNode " + nodes[i].name + " แบตเตอรี่กลับมาปกติ (" + String(nodes[i].battery, 2) + "V)");
                nodes[i].batteryAlertSent = false;
            }

            // กรณีกลับมาทำงานปกติหลังจาก Comm Error
            if (nodes[i].commState == STATE_COMM_ERROR) {
                handleNodeRecovery(i);
            }

            // Logic สำหรับระดับน้ำ
            if (nodes[i].msgType == MSG_TYPE_WATER) {
                // เงื่อนไขพิเศษสำหรับ TaiFa, KhonPD, SuanMM (Node ID 6, 7, 8)
                if (nodes[i].id == 6 || nodes[i].id == 7 || nodes[i].id == 8) {
                    if (nodes[i].lastValue < 10) {
                        if (nodeLowWaterStartTime[i] == 0) {
                            nodeLowWaterStartTime[i] = millis();
                            if (nodeLowWaterStartTime[i] == 0) nodeLowWaterStartTime[i] = 1;
                        } else if (millis() - nodeLowWaterStartTime[i] >= 7200000) { // นานเกิน 2 ชั่วโมง
                            if (nodes[i].alertState == STATE_NORMAL) {
                                sendLineMessage("แจ้งเตือน: แท็งค์ " + nodes[i].name + " น้ำหมดแท็งก์นานผิดปกติ");
                                nodes[i].alertState = STATE_LOW_WATER;
                            }
                        }
                    } else {
                        nodeLowWaterStartTime[i] = 0;
                        if (nodes[i].alertState == STATE_LOW_WATER) nodes[i].alertState = STATE_NORMAL;
                    }
                } 
                // เงื่อนไขปกติสำหรับ Node อื่นๆ
                else if (nodes[i].lastValue < nodes[i].threshold) {
                    nodes[i].lowCount++;
                    if (nodes[i].lowCount >= LOW_THRESHOLD && nodes[i].alertState == STATE_NORMAL) {
                        sendLineMessage("แจ้งเตือน: แท็งค์ " + nodes[i].name + " น้ำเหลือต่ำกว่า " + String(nodes[i].threshold) + "%");
                        activateRelay(nodes[i].id);
                        nodes[i].alertState = STATE_LOW_WATER;
                    }
                } else {
                    nodes[i].lowCount = 0;
                    if (nodes[i].alertState == STATE_LOW_WATER) {
                        nodes[i].alertState = STATE_NORMAL;
                        if (nodes[i].relayPin != 0) {
                            digitalWrite(nodes[i].relayPin, HIGH);
                            Serial.println("DEBUG: [RELAY OFF] " + nodes[i].name + " water level recovered.");
                        }
                    }
                }
            } 
            // Logic สำหรับ SPD
            else if (nodes[i].msgType == MSG_TYPE_SPD) {
                if (nodes[i].lastValue == 1 && nodes[i].alertState == STATE_NORMAL) {
                    sendLineMessage("แจ้งเตือน: SPD " + nodes[i].name + " หมดสภาพแล้ว โปรดตรวจสอบ");
                    nodes[i].alertState = STATE_LOW_WATER;
                }
            }
            break;
        }
    }
    newDataReceived = true;
}

// Function to send a single poll request and wait for a response
bool pollSingleNode(int nodeIdx) {
    uint8_t targetID = nodes[nodeIdx].id;

    LoRaPacket pollPacket;
    pollPacket.messageType = MSG_TYPE_POLL_REQ;
    pollPacket.receiverID = 101;  // ส่งหา Repeater
    pollPacket.senderID = 100;    // จาก Master
    pollPacket.sourceNodeID = targetID; // ระบุ Node ปลายทางที่ต้องการให้อ่านค่า
    pollPacket.value = 0;
    pollPacket.battery = 0.0;

    uint8_t txBuffer[sizeof(LoRaPacket)];
    memcpy(txBuffer, &pollPacket, sizeof(LoRaPacket));

    LT.startWriteSXBuffer(0);
    for (int i = 0; i < sizeof(LoRaPacket); i++) {
        LT.writeUint8(txBuffer[i]);
    }
    uint8_t len = LT.endWriteSXBuffer();

    uint8_t txResult = LT.transmitSXBuffer(0, len + 2, 5000, TXpower, WAIT_TX);
    if (txResult == 0) {
        Serial.printf("Error: Failed to transmit POLL_REQ to Node %s (ID %d)\n", nodes[nodeIdx].name.c_str(), targetID);
        return false;
    }

    // รอรับคำตอบกลับจาก Repeater (Downlink -> Node -> Uplink -> Master)
    // หมายเหตุสำคัญ: receiveSXBuffer(..., NO_WAIT) จะ return 0 ทันทีเสมอโดยไม่ตรวจสอบแพ็กเก็ตเลย (ดู SX126XLT.cpp)
    // จึงต้องใช้ WAIT_RX พร้อม timeout จริงเป็นช่วงๆ (slice) แทน เพื่อให้ยังเช็ค mqttRequestData เพื่อยกเลิกได้ระหว่างรอ
    // slice ต้องยาวกว่า time-on-air ของแพ็กเก็ต (~1.2-1.5s ที่ SF12/BW125) ไม่งั้นการ re-arm ระหว่าง slice
    // จะไปตัดกลางคันขณะกำลังรับแพ็กเก็ตที่ยาวกว่า slice พอดี ทำให้พลาดคำตอบทุกครั้ง
    const uint32_t POLL_RX_SLICE_MS = 3000;
    unsigned long startTime = millis();
    while (millis() - startTime < POLL_TIMEOUT_MS) {
        if (mqttRequestData) {
            Serial.println("DEBUG: Poll wait aborted - a new manual poll request is pending.");
            return false;
        }

        unsigned long elapsed = millis() - startTime;
        uint32_t remaining = POLL_TIMEOUT_MS - elapsed;
        uint32_t rxSlice = (remaining < POLL_RX_SLICE_MS) ? remaining : POLL_RX_SLICE_MS;

        int packetSize = LT.receiveSXBuffer(0, rxSlice, WAIT_RX); // บล็อกจริงจนกว่าจะได้แพ็กเก็ตหรือครบ rxSlice
        if (packetSize > 0) {
            if (packetSize == sizeof(LoRaPacket) || packetSize == (sizeof(LoRaPacket) + 2)) {
                int16_t rssi = LT.readPacketRSSI();
                int8_t snr = LT.readPacketSNR();
                LoRaPacket rxPacket;
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

                memcpy(&rxPacket, rxBuffer, sizeof(LoRaPacket));

                // Log สัญญาณทุกแพ็กเก็ตที่ถอดรหัสได้ เพื่อเปรียบเทียบความแรงสัญญาณ Master<->Repeater ก่อน/หลังปรับเสาอากาศ
                Serial.printf("Master RX: Type=%d, Sender=%d, Recv=%d, Source=%d, RSSI=%d dBm, SNR=%d dB\n",
                              rxPacket.messageType, rxPacket.senderID, rxPacket.receiverID, rxPacket.sourceNodeID, rssi, snr);

                if (rxPacket.messageType == MSG_TYPE_LINK_QUALITY &&
                    rxPacket.receiverID == 100 && rxPacket.senderID == 101 &&
                    rxPacket.sourceNodeID == targetID) {
                    int16_t measuredRssi = (int16_t)rxPacket.battery;
                    int8_t measuredSnr = (int8_t)rxPacket.value - 128;
                    nodeToRepeaterRssi[nodeIdx] = measuredRssi;
                    nodeToRepeaterSnr[nodeIdx] = measuredSnr;
                    // Do not publish here; wait until the full polling cycle ends so the MQTT payload
                    // reflects the complete batch for all nodes, not an intermediate partial update.
                    Serial.printf("Node %d -> Repeater link: RSSI=%d dBm, SNR=%d dB\n",
                                  targetID, measuredRssi, measuredSnr);
                    continue;
                }

                // ตรวจสอบว่าเป็นคำตอบที่ส่งถึง Master (100) จาก Repeater (101) สำหรับ Node ที่กำลัง Poll อยู่
                if (rxPacket.receiverID == 100 && rxPacket.senderID == 101 && rxPacket.sourceNodeID == targetID) {
                    processReceivedPacket(rxPacket);
                    return true;
                }
                // ไม่ตรงเงื่อนไข: วนลูปต่อเพื่อรอในเวลาที่เหลือ
            } else {
                LT.endReadSXBuffer();
            }
        }
    }

    return false; // หมดเวลารอ
}

// Function to poll a node with retries if no response
bool pollNodeWithRetries(int nodeIdx) {
    int maxAttempts = 1 + MAX_POLL_RETRIES;
    for (int attempt = 1; attempt <= maxAttempts; attempt++) {
        if (mqttRequestData) {
            Serial.println("DEBUG: Retry loop aborted - a new manual poll request is pending.");
            return false;
        }
        Serial.printf("Polling Node %s (ID %d) - Attempt %d/%d...\n", nodes[nodeIdx].name.c_str(), nodes[nodeIdx].id, attempt, maxAttempts);
        
        if (pollSingleNode(nodeIdx)) {
            Serial.printf("-> Node %s (ID %d) responded OK on attempt %d.\n", nodes[nodeIdx].name.c_str(), nodes[nodeIdx].id, attempt);
            return true;
        }

        Serial.printf("-> Node %s (ID %d) no response on attempt %d.\n", nodes[nodeIdx].name.c_str(), nodes[nodeIdx].id, attempt);
        if (attempt < maxAttempts) {
            delay(300); // หน่วงเวลาก่อนลองส่งซ้ำ
        }
    }

    // หากลองครบตามจำนวนแล้วยังไม่ตอบ
    Serial.printf("-> CRITICAL: Node %s (ID %d) FAILED to respond after %d attempts!\n", nodes[nodeIdx].name.c_str(), nodes[nodeIdx].id, maxAttempts);
    
    if (nodes[nodeIdx].commFailureCount < COMM_FAILURE_THRESHOLD) {
        nodes[nodeIdx].commFailureCount++;
    }
    Serial.printf("-> Node %s communication failure count: %d/%d\n",
                  nodes[nodeIdx].name.c_str(), nodes[nodeIdx].commFailureCount, COMM_FAILURE_THRESHOLD);

    // หมายเหตุ: การแจ้งเตือนจะถูกประเมินภาพรวมใน pollAllNodes() หลังจาก Poll ครบทุก Node ในรอบนี้แล้ว
    // เพื่อรอดูว่ามีปัญหาพร้อมกันหลาย Node หรือไม่ หากใช่จะแจ้งเตือนว่า Repeater ขัดข้องทีเดียว ไม่ยิงเตือนทีละ Node

    return false;
}

// Function to sequentially poll all configured nodes
void pollAllNodes() {
    Serial.println("========================================");
    Serial.println("Starting Master Polling Cycle...");
    Serial.println("========================================");

    int successCount = 0;
    bool cycleAborted = false;

    for (int i = 0; i < numNodes; i++) {
        if (mqttRequestData) {
            Serial.println("Polling Cycle Aborted: a new manual poll request is pending, restarting fresh.");
            cycleAborted = true;
            break;
        }
        if (pollNodeWithRetries(i)) {
            successCount++;
        }
        delay(300); // เว้นระยะระหว่าง Node เพื่อความสะอาดของความถี่
        // หมายเหตุ: ไม่เรียก client.loop() ที่นี่ เพราะ loop()/pollAllNodes() รันคนละ Task กับ MainTask ซึ่งเรียก client.loop() อยู่แล้ว
    }

    // รอบที่ถูก interrupt ยังไม่ได้ข้อมูลครบทุก Node จึงข้ามการประเมินผล/แจ้งเตือน แล้วปล่อยให้ loop() เริ่มรอบใหม่ทันที
    if (cycleAborted) {
        return;
    }

    Serial.printf("Polling Cycle Completed: %d/%d nodes responded.\n", successCount, numNodes);

    // ประเมินสถานะการสื่อสารหลังครบรอบ Polling เพื่อแยกแยะระหว่างปัญหาเฉพาะ Node กับปัญหาที่ Repeater
    int failingCount = 0;       // จำนวน Node ทั้งหมดที่มีปัญหาการสื่อสารติดต่อกันจนครบ Threshold
    int newlyFailingCount = 0;  // จำนวน Node ที่เพิ่งขัดข้องครบ Threshold ในรอบนี้ (commState ยังเป็น STATE_NORMAL)

    for (int i = 0; i < numNodes; i++) {
        if (nodes[i].commFailureCount >= COMM_FAILURE_THRESHOLD) {
            failingCount++;
            if (nodes[i].commState == STATE_NORMAL) {
                newlyFailingCount++;
            }
        }
    }

    Serial.printf("-> Comm Status: %d/%d nodes failing (newly failing: %d, responded: %d)\n",
                  failingCount, numNodes, newlyFailingCount, successCount);

    if (repeaterErrorActive) {
        // หาก Repeater ขัดข้องอยู่แล้ว และมี Node ตอบกลับมาได้ในรอบนี้ (กรณีหลุดรอดจาก handleNodeRecovery)
        if (successCount > 0) {
            sendAdminMessage("แจ้งเตือน: ระบบการสื่อสาร (Repeater) กลับมาทำงานปกติแล้ว");
            repeaterErrorActive = false;
            repeaterFailureCount = 0;
            lastRepeaterRecoveryTime = millis();
            Serial.println("DEBUG: Repeater recovered in pollAllNodes");
        }
    } else {
        // เงื่อนไขตรวจสอบปัญหาที่ Repeater:
        // 1. ไม่มี Node ใดตอบกลับเลย (successCount == 0) ติดต่อกันจนครบ Threshold (failingCount >= numNodes)
        // 2. หรือมีหลาย Node (>= REPEATER_FAIL_NODE_THRESHOLD) ที่มีปัญหาการสื่อสารติดต่อกัน และมี Node ขัดข้องใหม่ในรอบนี้
        bool isRepeaterIssue = (failingCount >= REPEATER_FAIL_NODE_THRESHOLD && newlyFailingCount > 0) ||
                               (successCount == 0 && numNodes > 0 && failingCount >= numNodes && newlyFailingCount > 0);

        if (isRepeaterIssue) {
            sendAdminMessage("แจ้งเตือน: พบความผิดปกติที่ Repeater (ไม่สามารถติดต่อ Data Nodes ผ่าน Repeater ได้)");
            repeaterErrorActive = true;
            repeaterFailureCount = COMM_FAILURE_THRESHOLD;
            for (int i = 0; i < numNodes; i++) {
                if (nodes[i].commFailureCount >= COMM_FAILURE_THRESHOLD) {
                    nodes[i].commState = STATE_COMM_ERROR;
                }
            }
            Serial.println("-> ALERT: Repeater communication error notified!");
        } else if (newlyFailingCount > 0) {
            // กรณีขัดข้องเฉพาะบาง Node (ไม่ใช่ปัญหาที่ Repeater) แจ้งเตือนเฉพาะ Node ที่เพิ่งขัดข้อง
            for (int i = 0; i < numNodes; i++) {
                if (nodes[i].commFailureCount >= COMM_FAILURE_THRESHOLD &&
                    nodes[i].commState == STATE_NORMAL) {
                    nodes[i].commState = STATE_COMM_ERROR;
                    sendAdminMessage("แจ้งเตือน: การสื่อสารขัดข้องที่ DataNode " + nodes[i].name + " (ไม่ตอบกลับคำขอ)");
                    Serial.printf("-> ALERT: DataNode %s communication error notified!\n", nodes[i].name.c_str());
                }
            }
        }
    }

    // ส่งข้อมูลขึ้น MQTT ครั้งเดียวเมื่อ polling cycle จบสมบูรณ์
    pendingMQTTDataSync = true;
}

// ฟังก์ชันจัดการเมื่อ Node กลับมาออนไลน์ เพื่อให้โค้ดหลักสะอาดขึ้น
void handleNodeRecovery(int nodeIdx) {
    if (repeaterErrorActive) {
        sendAdminMessage("แจ้งเตือน: ระบบการสื่อสาร (Repeater) กลับมาทำงานปกติแล้ว");
        repeaterErrorActive = false;
        repeaterFailureCount = 0;
        lastRepeaterRecoveryTime = millis();
    } else {
        unsigned long timeSinceLastRecovery = millis() - lastRepeaterRecoveryTime;
        if (timeSinceLastRecovery > RECOVERY_GRACE_PERIOD) {
            sendAdminMessage("แจ้งเตือน: DataNode " + nodes[nodeIdx].name + " กลับมาทำงานปกติแล้ว");
        } else {
            Serial.println("DEBUG: Silent recovery for " + nodes[nodeIdx].name + " during grace period.");
        }
    }
    nodes[nodeIdx].commState = STATE_NORMAL;
}

// Function to activate relay based on tank ID
void activateRelay(uint8_t nodeID) {
    if (!notificationsEnabled) {
        Serial.println("Notifications disabled. Relay activation skipped.");
        return;
    }

    String tankName = "Unknown";
    for (int i = 0; i < numNodes; i++) {
        if (nodes[i].id == nodeID) {
            tankName = nodes[i].name;
            break;
        }
    }

    switch (nodeID) {
        case 1:
            Serial.println("DEBUG: [RELAY ON] Pin " + String(RELAY_PIN_1) + " (JD) - Reason: " + tankName + " water level < threshold");
            digitalWrite(RELAY_PIN_1, LOW);  // JD
            break;
        case 2:
            Serial.println("DEBUG: [RELAY ON] Pin " + String(RELAY_PIN_2) + " (PhraNon) - Reason: " + tankName + " water level < threshold");
            digitalWrite(RELAY_PIN_2, LOW);  // Phranon
            break;
        case 4:
            Serial.println("DEBUG: [RELAY ON] Pin " + String(RELAY_PIN_3) + " (ThamYai) - Reason: " + tankName + " water level < threshold");
            digitalWrite(RELAY_PIN_3, LOW);  // ThamYai
            break;
        case 5:
            Serial.println("DEBUG: [RELAY ON] Pin " + String(RELAY_PIN_4) + " (RongPui) - Reason: " + tankName + " water level < threshold");
            digitalWrite(RELAY_PIN_4, LOW);  // RongPui
            break;
        default:
            Serial.println("DEBUG: [RELAY SKIP] Node ID " + String(nodeID) + " has no assigned relay.");
            break;
    }
}

// Function to reset notifications and relays
void resetNotification() {
    for (int i = 0; i < numNodes; i++) {
        nodes[i].alertState = STATE_NORMAL;
        nodes[i].commState = STATE_NORMAL;
        nodes[i].lowCount = 0;
        nodes[i].commFailureCount = 0;
        nodeLowWaterStartTime[i] = 0;
    }
    repeaterErrorActive = false;
    repeaterFailureCount = 0;
    lastRepeaterRecoveryTime = 0;  // ล้างช่วงเวลาพักการแจ้งเตือน (Grace Period) เพื่อให้พร้อมรับข้อมูลใหม่ทันที
    lastRepeaterSeen = millis();   // ป้องกันการตรวจจับ Repeater ลงทันทีหลังรีเซ็ต

    Serial.println("DEBUG: [RELAY OFF] All Pins (1-4) - Reason: Manual Reset or System Init");
    digitalWrite(RELAY_PIN_1, HIGH);
    digitalWrite(RELAY_PIN_2, HIGH);
    digitalWrite(RELAY_PIN_3, HIGH);
    digitalWrite(RELAY_PIN_4, HIGH);
    Serial.println("Notifications and relays reset.");

    display.clear();
    display.display();
}

void VextON(void) {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
}

void VextOFF(void) { //Vext default OFF
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, HIGH);
}

// Function to display information on OLED
void displayData(String tankName, String message) {
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setFont(ArialMT_Plain_24);
    display.drawString(64, 5, tankName);
    display.drawString(64, 30, message);
    display.display();
}

// หมายเหตุ: เดิมมี CheckCommError() ที่ตรวจจับ comm error/repeater down แบบ timeout ยาว (50 นาที)
// สำหรับสถาปัตยกรรม push-based เดิม ตอนนี้ pollNodeWithRetries()/pollAllNodes()/handleNodeRecovery()
// ตรวจจับและแจ้งเตือนได้เร็วและแม่นยำกว่าอยู่แล้วในทุกรอบ poll จึงลบออกเพื่อไม่ให้ตรรกะทับซ้อน/ขัดแย้งกัน

void DisplayTimerTask(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    bool wasDisplaySessionActive = false;

    for (;;) {
        uint8_t wakeRequest = 0;
        if (displayWakeQueue != NULL &&
            xQueueReceive(displayWakeQueue, &wakeRequest, 0) == pdTRUE &&
            wakeRequest == 1) {
            displaySessionActive = true;
            displaySessionStartTime = millis();
            displayPageState = 0;
            displayNodeIndex = 0;
        }

        if (displaySessionActive &&
            (millis() - displaySessionStartTime >= DISPLAY_BOOST_DURATION_MS)) {
            displaySessionActive = false;
        }

        if (!displaySessionActive) {
            if (wasDisplaySessionActive) {
                display.setBrightness(DISPLAY_BRIGHTNESS_IDLE);
                display.clear();
                display.display();
                wasDisplaySessionActive = false;
                lastLedHeartbeatTime = millis();
                Serial.println("DEBUG: Display window ended; OLED off");
            } else {
                display.clear();
                display.display();
            }
            xLastWakeTime = xTaskGetTickCount();
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }

        if (!wasDisplaySessionActive) {
            display.setBrightness(DISPLAY_BRIGHTNESS_ACTIVE);
            wasDisplaySessionActive = true;
        }

        int currentTotalDisplayItems = numNodes;
        if (!internetConnected) {
            currentTotalDisplayItems++; // เพิ่ม 1 ช่องสำหรับ "Internet Error"
        }

        // สลับแสดงผล: ข้อมูล Node -> หน้าว่าง (Flash) หรือ Internet Error -> หน้าว่าง
        if (displayPageState % 2 == 0) {
            if (displayNodeIndex < numNodes) {
                // แสดงข้อมูล Node ปัจจุบัน
                Node &n = nodes[displayNodeIndex];
                if (n.commState == STATE_COMM_ERROR) {
                    displayData(n.name, "CommError");
                } else if (n.lastValue == -1) {
                    displayData(n.name, "Waiting...");
                } else if (n.msgType == MSG_TYPE_SPD) {
                    displayData(n.name, n.lastValue == 0 ? "SPD OK" : "SPD XX");
                } else {
                    displayData(n.name, String(n.lastValue) + " %");
                }
            } else if (displayNodeIndex == numNodes && !internetConnected) {
                // แสดง Internet Error หาก displayNodeIndex ชี้ไปที่ช่องสุดท้ายและไม่มีเน็ต
                displayData("Internet", "Error");
            } else {
                // กรณี displayNodeIndex ชี้ไปที่ช่อง Internet Error แต่เน็ตต่ออยู่ ให้แสดงหน้าว่างแทน
                display.clear();
                display.display();
            }
        } else {
            // หน้าว่าง
            display.clear();
            display.display();
            // ขยับไป Node ถัดไปหลังจากหน้าว่าง
            displayNodeIndex = (displayNodeIndex + 1) % currentTotalDisplayItems;
        }

        TickType_t delayTime = (displayPageState % 2 == 0) ? displayInterval : pauseInterval;
        displayPageState = (displayPageState + 1) % (currentTotalDisplayItems * 2); // วนรอบตามจำนวนรายการทั้งหมด (ข้อมูล + หน้าว่าง)

        // delay แบบ vTaskDelayUntil เพื่อให้เวลา delay คงที่แม่นยำ
        vTaskDelayUntil(&xLastWakeTime, delayTime);
    }
}

void reconnect() {
    // ลบ Loop while ออก เพื่อไม่ให้บล็อกการทำงานของปุ่มกด
    if (!client.connected()) {
        Serial.print("Attempting MQTT connection...");
        String clientId = "ESP32Client-";
        clientId += String(WiFi.macAddress()); // Use MAC Address for stable Client ID
        
        // (TAG) ตั้งค่า LWT ไว้ด้วย: ถ้า Master หลุดการเชื่อมต่อแบบไม่บอกลา (ไฟดับ/แฮงก์จน TCP ขาด) broker จะประกาศ offline ให้เอง
        if (client.connect(clientId.c_str(), mqtt_username, mqtt_password, master_status_topic, 1, true, "offline")) {
            Serial.println("connected");
            client.subscribe("reset_notification");
            client.subscribe("reset_system");
            client.subscribe(notification_control_topic);
            client.subscribe(request_data_topic);
            publishMessage(master_status_topic, "online", true);
        } else {
            Serial.print("failed, rc=");
            Serial.print(client.state());
            Serial.println(" try again later");
        }
    }
}

//=======================================
// This void is called every time we have a message from the broker

void callback(char* topic, byte* payload, unsigned int length) {
    String incommingMessage = "";
    for (int i = 0; i < length; i++) {
        incommingMessage += (char)payload[i];
    }

    Serial.println("Message arrived [" + String(topic) + "]: " + incommingMessage);

    if (strcmp(topic, "reset_notification") == 0) {
        if (incommingMessage.equals("1")) {
            mqttResetNotification = true;
        }
    }

    if (strcmp(topic, "reset_system") == 0) {
        if (incommingMessage.equals("1")) {
            mqttResetSystem = true;
        }
    }

    if (strcmp(topic, notification_control_topic) == 0) {
        if (incommingMessage.equals("0")) {
            mqttDisableNotifications = true;
        } else if (incommingMessage.equals("1")) {
            mqttEnableNotifications = true;
        }
    }

    if (strcmp(topic, request_data_topic) == 0) {
        if (incommingMessage.equals("1")) {
            mqttRequestData = true;
        }
    }
}

//======================================= publising as string
void publishMessage(const char* topic, String payload , boolean retained) {
    if (topic == message_topic) queueHistoryLog(payload);
    if (client.publish(topic, payload.c_str(), retained)) {
        Serial.println("Message publised ["+String(topic)+"]: "+payload);
    }
}

// ==== History upload (MainTask only) ====

// เรียกได้จากทุก Task: เก็บเวลาตอนเกิดเหตุการณ์ไว้ (0 = ยังไม่ sync NTP เซิร์ฟเวอร์จะใช้เวลาที่ได้รับแทน)
void queueHistoryLog(const String &message) {
    if (historyLogQueue == NULL || !HISTORY_URL[0] || !HISTORY_TOKEN[0]) return;
    HistoryLog item;
    time_t now = time(nullptr);
    item.time = now > 1700000000 ? (uint32_t)now : 0;
    message.toCharArray(item.text, MQTT_MSG_MAX_LEN);
    xQueueSend(historyLogQueue, &item, 0);
}

struct LevelSample {
    uint32_t time;
    int16_t values[numNodes];
};
HistoryLog pendingLogs[HISTORY_LOG_BUFFER];
int pendingLogCount = 0;
LevelSample pendingSamples[HISTORY_SAMPLE_BUFFER];
int pendingSampleCount = 0;
uint32_t historyLogSeq = 0;

// เก็บค่าระดับน้ำทุกรอบ poll ไว้ส่งขึ้นเว็บแอป (เต็มแล้วทิ้งค่าเก่าสุด)
void recordLevelSample() {
    if (!HISTORY_URL[0] || !HISTORY_TOKEN[0]) return;
    if (pendingSampleCount == HISTORY_SAMPLE_BUFFER) {
        memmove(pendingSamples, pendingSamples + 1, sizeof(LevelSample) * (HISTORY_SAMPLE_BUFFER - 1));
        pendingSampleCount--;
    }
    LevelSample &s = pendingSamples[pendingSampleCount++];
    time_t now = time(nullptr);
    s.time = now > 1700000000 ? (uint32_t)now : 0;
    for (int i = 0; i < numNodes; i++) {
        s.values[i] = nodes[i].commState == STATE_COMM_ERROR ? -1 : nodes[i].lastValue;
    }
}

void appendJsonString(String &out, const char *text) {
    out += '"';
    for (const char *p = text; *p; p++) {
        char c = *p;
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((uint8_t)c < 0x20) { char esc[8]; snprintf(esc, sizeof(esc), "\\u%04x", c); out += esc; }
        else out += c;
    }
    out += '"';
}

void uploadHistory() {
    static unsigned long lastAttempt = 0;
    HistoryLog item;
    while (historyLogQueue != NULL && xQueueReceive(historyLogQueue, &item, 0) == pdTRUE) {
        if (pendingLogCount == HISTORY_LOG_BUFFER) {
            memmove(pendingLogs, pendingLogs + 1, sizeof(HistoryLog) * (HISTORY_LOG_BUFFER - 1));
            pendingLogCount--;
        }
        pendingLogs[pendingLogCount++] = item;
    }
    if (!internetConnected || (pendingLogCount == 0 && pendingSampleCount == 0)) return;
    if (lastAttempt != 0 && millis() - lastAttempt < HISTORY_UPLOAD_INTERVAL_MS) return;
    lastAttempt = millis();

    int logCount = pendingLogCount;
    int sampleCount = pendingSampleCount;
    String body = "{\"logs\":[";
    for (int i = 0; i < logCount; i++) {
        if (i) body += ',';
        body += "{\"t\":" + String(pendingLogs[i].time) + ",\"s\":" + String(++historyLogSeq) + ",\"m\":";
        appendJsonString(body, pendingLogs[i].text);
        body += '}';
    }
    body += "],\"samples\":[";
    for (int i = 0; i < sampleCount; i++) {
        if (i) body += ',';
        body += "{\"t\":" + String(pendingSamples[i].time) + ",\"v\":{";
        bool first = true;
        for (int n = 0; n < numNodes; n++) {
            if (nodes[n].msgType != MSG_TYPE_WATER) continue;
            if (!first) body += ',';
            first = false;
            body += "\"" + String(nodes[n].mqttTopic) + "\":" + String(pendingSamples[i].values[n]);
        }
        body += "}}";
    }
    body += "]}";

    WiFiClientSecure httpsClient;
    httpsClient.setCACert(gts_root_r4);
    HTTPClient http;
    http.setTimeout(10000);
    if (!http.begin(httpsClient, HISTORY_URL)) return;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("x-log-token", HISTORY_TOKEN);
    int code = http.POST(body);
    http.end();
    if (code < 200 || code >= 300) {
        Serial.printf("History upload failed: %d\n", code);
        return;
    }
    pendingLogCount -= logCount;
    memmove(pendingLogs, pendingLogs + logCount, sizeof(HistoryLog) * pendingLogCount);
    pendingSampleCount -= sampleCount;
    memmove(pendingSamples, pendingSamples + sampleCount, sizeof(LevelSample) * pendingSampleCount);
}

// Function to send MQTT data every 10 minutes
void sendMQTTData() {
    publishMessage(notification_status_topic, notificationsEnabled ? "1" : "0", true);

    // ส่งเวลาปัจจุบันจริง (NTP) แนบมากับทุกชุดข้อมูลที่ publish จริง เพื่อให้แอปมือถือเทียบกับเวลาปัจจุบันเองได้
    String ts = getFormattedTime();
    if (ts.length() > 0) publishMessage(master_last_update_topic, ts, true);

    // วน Loop ส่งข้อมูลของทุก Node ขึ้น MQTT ตาม topic ที่กำหนดใน Struct
    for (int i = 0; i < numNodes; i++) {
        if (nodes[i].commState == STATE_COMM_ERROR) {
            // หากเกิดข้อผิดพลาดในการสื่อสาร ให้ส่งค่า -1 และแบตเตอรี่ 0.00 เพื่อแจ้งแอปมือถือ
            publishMessage(nodes[i].mqttTopic, "-1", true);
            String batTopic = String(nodes[i].mqttTopic) + "/battery";
            publishMessage(batTopic.c_str(), "0.00", true);
        } else {
            // ส่งเฉพาะค่าล่าสุดที่มีในสถานะปกติ
            publishMessage(nodes[i].mqttTopic, String(nodes[i].lastValue), true);
            // ส่งค่าแบตเตอรี่ขึ้น MQTT (เฉพาะ Node 4 และ 5 ที่มีแบตเตอรี่จริง)
            if (nodes[i].id == 4 || nodes[i].id == 5) {
                String batTopic = String(nodes[i].mqttTopic) + "/battery";
                publishMessage(batTopic.c_str(), String(nodes[i].battery, 2), true);
            }
        }

        if (nodeToRepeaterRssi[i] != -127) {
            String linkTopic = String(link_quality_topic_prefix) + nodes[i].mqttTopic;
            String linkPayload = "{\"rssi\":" + String(nodeToRepeaterRssi[i]) +
                                 ",\"snr\":" + String(nodeToRepeaterSnr[i]) + "}";
            publishMessage(linkTopic.c_str(), linkPayload, true);
        }
    }

    if (newDataReceived) {
        // Reset the newDataReceived flag
        newDataReceived = false;
    }
    recordLevelSample();
}


void setup() {
    Serial.begin(115200);
    Serial.println("Master Node 6 Initializing...");

    // ต้องสร้างก่อนเริ่ม Task ใดๆ เพื่อให้ loop()/MainTask ส่งข้อความผ่าน Queue ได้ตั้งแต่ต้น
    mqttMessageQueue = xQueueCreate(20, sizeof(MqttTextMessage));
    historyLogQueue = xQueueCreate(HISTORY_LOG_BUFFER, sizeof(HistoryLog));
    displayWakeQueue = xQueueCreate(1, sizeof(uint8_t));

    SPI.begin();
    delay(500);

    VextON();
    delay(100);

    // Initialize OLED Display
    display.init();
    display.clear();
    display.setBrightness(DISPLAY_BRIGHTNESS_IDLE); // เริ่มต้นที่ความสว่าง ~10%
    display.display();   
    
    pinMode(RELAY_PIN_1, OUTPUT);
    pinMode(RELAY_PIN_2, OUTPUT);
    pinMode(RELAY_PIN_3, OUTPUT);
    pinMode(RELAY_PIN_4, OUTPUT);

    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);  // ตั้งค่าปุ่มรีเซ็ตเป็น INPUT_PULLUP
    pinMode(35, OUTPUT);                      // LED สีขาวบนบอร์ด Heltec V3 (Visual Feedback)

    digitalWrite(RELAY_PIN_1, HIGH);
    digitalWrite(RELAY_PIN_2, HIGH);
    digitalWrite(RELAY_PIN_3, HIGH);
    digitalWrite(RELAY_PIN_4, HIGH);

    // เริ่ม Task ปุ่มกดก่อนเพื่อให้กดค้างรีเซ็ตได้ทันทีหากจำเป็น
    xTaskCreate(DisplayControlButtonTask, "DisplayControlButtonTask", 2048, NULL, 1, &TaskButton);

    // 1. เคลียร์สถานะแจ้งเตือนและ Relay โดยไม่แสดงผลบนหน้าจอ
    resetNotification();

    // 2. เริ่ม Task แสดงข้อมูลเมื่อผู้ใช้กดปุ่มเท่านั้น
    xTaskCreate(DisplayTimerTask, "DisplayTimerTask", 5000, NULL, 1, &TaskDisplay);

    // เชื่อมต่อ Wi-Fi
    WiFi.mode(WIFI_STA);
    wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);

    espClient.setCACert(root_ca);
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(callback);
    client.setKeepAlive(60); // เพิ่มจาก default 15s เพื่อลด PINGREQ/PINGRESP ที่กินโควต้าเน็ตรายปีโดยไม่จำเป็น

    internetConnected = false; // เริ่มต้นเป็น false จนกว่าจะเช็คผ่าน

    if (!initLoRa()) {
        while (1);  // Stop if LoRa initialization fails
    }
    LT.setupLoRa(Frequency, Offset, SpreadingFactor, Bandwidth, CodeRate, Optimisation);
    delay(1000);

    // เริ่มต้นเวลาสื่อสารสำหรับทุก Node
    for (int i = 0; i < numNodes; i++) {
        nodes[i].lastSeen = millis();
        nodes[i].commFailureCount = 0;
        nodeToRepeaterRssi[i] = -127;
        nodeToRepeaterSnr[i] = -128;
    }
    lastRepeaterSeen = millis(); // เริ่มนับเวลา Repeater ตั้งแต่เปิดเครื่อง

    // Create tasks
    xTaskCreate(MainTask, "MainTask", 10000, NULL, 1, &TaskMain);

    // สร้าง Task สำหรับรีเซ็ตอัตโนมัติทุก 12 ชั่วโมง เพื่อป้องกัน heap fragmentation สะสมจากการใช้ String ต่อเนื่อง
    xTaskCreate(ResetTimerTask, "ResetTimerTask", 1000, NULL, 1, &TaskResetTimer);


}

// Task for Button Logic (Separated for responsiveness)
// หน้าที่เดียวตอนนี้: เพิ่มความสว่างจอชั่วคราวเมื่อกด (reset notification ย้ายไปสั่งผ่าน MQTT เท่านั้น)
void DisplayControlButtonTask(void *parameter) {
    for (;;) {
        bool isPressed = (digitalRead(RESET_BUTTON_PIN) == LOW); // ทำงานร่วมกับ RC Filter 2k/4.7uF

        if (isPressed) {
            if (!buttonPressed) {
                vTaskDelay(50 / portTICK_PERIOD_MS); // Software Debounce ซ้ำอีกชั้น
                if (digitalRead(RESET_BUTTON_PIN) == LOW) {
                    buttonPressed = true;
                    Serial.println("DEBUG: Button Pressed - Showing display at full brightness for 2 minutes");
                    uint8_t wakeRequest = 1;
                    if (displayWakeQueue != NULL) {
                        xQueueOverwrite(displayWakeQueue, &wakeRequest);
                    }
                    digitalWrite(35, LOW);
                    ledHeartbeatOn = false;
                }
            }
        } else {
            buttonPressed = false;
        }

        if (!displaySessionActive) {
            if (!ledHeartbeatOn && millis() - lastLedHeartbeatTime >= LED_HEARTBEAT_INTERVAL_MS) {
                digitalWrite(35, HIGH);
                ledHeartbeatOn = true;
                ledHeartbeatStartTime = millis();
                lastLedHeartbeatTime = millis();
            } else if (ledHeartbeatOn && millis() - ledHeartbeatStartTime >= LED_HEARTBEAT_ON_MS) {
                digitalWrite(35, LOW);
                ledHeartbeatOn = false;
            }
        }

        vTaskDelay(50 / portTICK_PERIOD_MS); 
    }
}

// Task for main logic
void MainTask(void *parameter) {

  static unsigned long lastInternetCheck = 0;
  static unsigned long lastReconnectAttempt = 0; // ตัวแปรจับเวลาสำหรับ Reconnect
  static bool ntpSynced = false;
  
  while (true) {
    // จัดการ NTP เมื่อเชื่อมต่อได้ครั้งแรก
    if (WiFi.status() == WL_CONNECTED && !ntpSynced) {
        configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");
        ntpSynced = true;
    }

    // ตรวจสอบสถานะ WiFi ก่อน ถ้าหลุดค่อยเรียก wifiMulti.run()
    // ตรวจสอบสถานะ WiFi และ Internet จริงๆ
    if (WiFi.status() == WL_CONNECTED) {
        // ถ้าต่อ WiFi อยู่ ให้เช็คเน็ตทุกๆ 60 วินาที (หรือทุก 5 วินาทีถ้าสถานะเดิมคือหลุด)
        unsigned long checkInterval = internetConnected ? 60000 : 5000;
        if (millis() - lastInternetCheck >= checkInterval) {
            lastInternetCheck = millis();
            bool hasInternet = checkInternetConnection();
            if (hasInternet != internetConnected) {
                internetConnected = hasInternet;
                Serial.println(internetConnected ? "Internet Access: OK" : "Internet Access: Lost");
            }
        }
    } else {
        internetConnected = false;
        ntpSynced = false;

        // เพิ่มการหน่วงเวลาการพยายามเชื่อมต่อใหม่ เพื่อลด Log รก และลดการสแกนที่บ่อยเกินไป
        static unsigned long lastWiFiRetry = 0;
        if (millis() - lastWiFiRetry >= 30000) { // พยายามเชื่อมต่อใหม่ทุกๆ 30 วินาที
            lastWiFiRetry = millis();
            if (wifiMulti.run() == WL_CONNECTED) {
                 Serial.println("Wi-Fi reconnected");
                 lastInternetCheck = 0; // บังคับให้เช็คเน็ตทันทีในรอบถัดไป
            }
        }
    }

    // MQTT
    if (internetConnected) {
        if (!client.connected()) {
            // ลองเชื่อมต่อใหม่ทุกๆ 5 วินาที (แบบไม่บล็อกปุ่ม)
            if (millis() - lastReconnectAttempt > 5000) {
                lastReconnectAttempt = millis();
                reconnect();
            }
        } else {
            client.loop();
            // MainTask เป็นเจ้าของ client แต่เพียงผู้เดียว จึงมาไล่ประมวลผลคิว/แฟล็กที่ Task อื่นฝากไว้ที่นี่
            MqttTextMessage queuedMsg;
            while (mqttMessageQueue != NULL && xQueueReceive(mqttMessageQueue, &queuedMsg, 0) == pdTRUE) {
                publishMessage(message_topic, String(queuedMsg.text), true);
            }
            if (pendingMQTTDataSync) {
                pendingMQTTDataSync = false;
                sendMQTTData();
            }

            if (mqttResetNotification) {
                publishMessage(message_topic, "Reset Notification via MQTT", false);
                mqttResetNotification = false;
                // Give time for message to be sent
                unsigned long start = millis();
                while (millis() - start < 1000) {
                    client.loop();
                    delay(10);
                }
                resetNotification();
            }

            if (mqttResetSystem) {
                publishMessage(message_topic, "System Reset via MQTT", false);
                mqttResetSystem = false;
                // Give time for message to be sent
                unsigned long start = millis();
                while (millis() - start < 2000) {
                    client.loop();
                    delay(10);
                }
                resetSystem();
            }

            if (mqttDisableNotifications) {
                notificationsEnabled = false;
                
                Serial.println("DEBUG: [RELAY OFF] All Pins (1-4) - Reason: MQTT Command (Disable Notifications)");
                // Turn off all relays immediately
                digitalWrite(RELAY_PIN_1, HIGH);
                digitalWrite(RELAY_PIN_2, HIGH);
                digitalWrite(RELAY_PIN_3, HIGH);
                digitalWrite(RELAY_PIN_4, HIGH);

                Serial.println("Notifications Disabled via MQTT");
                publishMessage(message_topic, "notification is set to off", false);
                publishMessage(notification_status_topic, notificationsEnabled ? "1" : "0", true);
                mqttDisableNotifications = false;
                unsigned long start = millis();
                while (millis() - start < 1000) {
                    client.loop();
                    delay(10);
                }
            }

            if (mqttEnableNotifications) {
                notificationsEnabled = true;
                Serial.println("Notifications Enabled via MQTT");
                publishMessage(message_topic, "notification is set to on", false);
                publishMessage(notification_status_topic, notificationsEnabled ? "1" : "0", true);
                mqttEnableNotifications = false;
                unsigned long start = millis();
                while (millis() - start < 1000) {
                    client.loop();
                    delay(10);
                }
            }

            if (mqttRequestData) {
                Serial.println("Manual Data Request via MQTT");
                publishMessage(message_topic, "Manual data update requested. Polling nodes...", false);
                // mqttRequestData remains true and will be processed immediately by loop()
                unsigned long start = millis();
                while (millis() - start < 300) {
                    client.loop();
                    delay(10);
                }
            }
        }
    }
    uploadHistory();
    vTaskDelay(100 / portTICK_PERIOD_MS);  // ตรวจทุก 100ms
  }
}

void loop() {
    static unsigned long lastPollCycleTime = 0;

    // ทำการ Poll ทุกๆ POLL_CYCLE_INTERVAL_MS หรือเมื่อเปิดเครื่อง หรือเมื่อถูกสั่งผ่าน MQTT
    if (lastPollCycleTime == 0 || (millis() - lastPollCycleTime >= POLL_CYCLE_INTERVAL_MS) || mqttRequestData) {
        if (mqttRequestData) {
            Serial.println("Polling triggered immediately via MQTT request_data command!");
            mqttRequestData = false;
        }
        lastPollCycleTime = millis();
        pollAllNodes();
    }

    delay(100);
    yield();
}