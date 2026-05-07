// ใช้ได้ดี ทดสอบ linemessaging API แล้วใช้ได้ 
//แก้ไขหน้าจอทับซ้อนเวลาเน็ตหลุด แก้ไขหน้าจอแสดงผลตลอดเวลาทำให้หน้าจอหมอง
// แก้ไข การแจ้งเตือนเมื่อเซ็นเซอร์ทำงานผิดพลาด โดยให้มีการนับ หากน้ำต่ำกว่าครึ่งแทงค์ถึงสามครั้งจึงจะแจ้งเตือน 
// แก้ไขไฟลล์ให้เป็นไปตามมาตรฐาน C++11 เปลี่ยนนามสกุล .ino เป็น .cpp เพิ่มการ declare ฟังก์ชันข้างบน main code
// disable auto reset function for testing purpose
//  ลดความสว่างหน้าจอเหลือ 150

/*

เพิ่ม Topic ชื่อ request_data
เมื่อคุณกดปุ่มบนมือถือให้ส่งค่า 1 มาที่ Topic นี้
Master Node จะตรวจจับได้และทำการ Publish ข้อมูลเซ็นเซอร์ทุกตัวออกมาทันทีครับ  

*/


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

void sendLineMessage(String message);
void resetSystem();
void ResetTimerTask(void *parameter);
void IRAM_ATTR handleButtonPress();
bool initLoRa();
void receiveData();
void activateRelay(uint8_t nodeID);
void resetNotification();
void VextON(void);
void VextOFF(void);
void displayData(String tankName, String message);
bool initLoRa(); // ย้ายมาไว้ตรงนี้
void CheckCommError();
void DisplayTimerTask(void *pvParameters);
void reconnect();
void callback(char* topic, byte* payload, unsigned int length);
void publishMessage(const char* topic, String payload , boolean retained);
void sendMQTTData();
bool checkInternetConnection();
void MainTask(void *parameter);
void ButtonTask(void *parameter);
void handleNodeRecovery(int nodeIdx); // ฟังก์ชันช่วยจัดการเมื่อ Node กลับมาออนไลน์

// Task handles
TaskHandle_t TaskMain;
TaskHandle_t TaskResetTimer; // Task Handle สำหรับรีเซ็ตบอร์ด
TaskHandle_t TaskDisplay;
TaskHandle_t TaskButton;

// Timer สำหรับการอัปเดตหน้าจอ และ MQTT
SimpleTimer displayTimer;
SimpleTimer mqttTimer;

// ตัวแปรสำหรับสถานะการแสดงผล (วนรอบระหว่าง 0-12)
// ตัวแปร global track state และพักหน้าจอ
volatile int displayState = 0;         // 0..12 คือข้อมูลแต่ละหน้า
volatile int activeNodeIdx = 0;        // ตัวชี้ Node ที่กำลังแสดงผล
const TickType_t displayInterval = 3500 / portTICK_PERIOD_MS; // 3500 ms
const TickType_t pauseInterval   = 500 / portTICK_PERIOD_MS;  // 100 ms

unsigned long lastStateChange = 0;  // เก็บเวลาที่เปลี่ยนสถานะครั้งล่าสุด

// ประกาศตัวแปรเก็บสถานะเน็ต (ต้องประกาศให้ถูกต้องตามโปรเจคคุณ)
volatile bool internetConnected = false;  // กำหนดค่าเริ่มต้นตามสถานะจริงของเน็ต

// ==== System Timing Constants ====
const unsigned long OBSERVATION_WINDOW = 300000;    // 5 นาที (รอเช็คจำนวน Node ที่เสียพร้อมกัน)
const unsigned long RECOVERY_GRACE_PERIOD = 900000; // 15 นาที (งดแจ้งเตือนราย Node หลัง Repeater ฟื้น)
const unsigned long mqttSendInterval = 600000;      // 10 นาที (รอบการส่งข้อมูลขึ้น MQTT)
const unsigned long commTimeout = 1500000;          // 25 นาที (Timeout สำหรับตรวจการขาดการติดต่อ)

// ==== LINE Messaging API Settings ====
const char* LINE_ACCESS_TOKEN = "Vv9JoO8Af3WbpW3iozJEmzXj0IdGRVHruYM1TE6umoFn5OWqTDelb05Pm0oah8FmHV/lDx0pmmLDm5HZ6umXSWJV8zrmh4c8wtpZcCLObqdUWqPdOYN1XKG+GoPN0A6BBW+fNXY/s2AEJA1HQR1HkQdB04t89/1O/w1cDnyilFU="; // ใส่ Channel Access Token ของคุณ
const char* LINE_API_URL = "https://api.line.me/v2/bot/message/push";
const char* LINE_USER_ID = "C193952f60a4c2951926955db63570e03"; // ใส่ LINE User ID ที่ต้องการให้ส่งข้อความหา

//---- MQTT Broker settings
const char* mqtt_server = "aa9e03df167b40b086069a385503cf2f.s1.eu.hivemq.cloud"; // replace with your broker url
const char* mqtt_username = "thiravit";
const char* mqtt_password = "@16Jan2563";
const int mqtt_port = 8883;

WiFiClientSecure espClient;
PubSubClient client(espClient);
unsigned long suspectedCommTime = 0; // ตัวแปรสำหรับเก็บเวลาที่เริ่มพบปัญหาการสื่อสารครั้งแรก
bool repeaterErrorActive = false;    // สถานะแจ้งเตือน Repeater ขัดข้อง
unsigned long lastRepeaterRecoveryTime = 0; // เวลาที่ Repeater กลับมาปกติครั้งล่าสุด
unsigned long lastMsg = 0;

#define MSG_BUFFER_SIZE (50)
char msg[MSG_BUFFER_SIZE];

const char* message_topic = "message";
const char* notification_control_topic = "notification_control";
const char* notification_status_topic = "notification_status";
const char* request_data_topic = "request_data";

// Flag to track when new data is received
volatile bool newDataReceived = false;
volatile bool mqttResetNotification = false;
volatile bool mqttResetSystem = false;
volatile bool notificationsEnabled = true;
volatile bool mqttDisableNotifications = false;
volatile bool mqttEnableNotifications = false;
volatile bool mqttRequestData = false;

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

#define RESET_BUTTON_PIN 1  // ย้ายจาก 47 มาขา 1 เนื่องจากขา 47 เสีย
#define LONG_PRESS_DURATION 2000  // กำหนดเวลาการกดค้าง (2 วินาที)

WiFiMulti wifiMulti;

#define WIFI_SSID   "SolarCell" //  "iPhone" // WiFi Name
#define WIFI_PASSWORD  "12345678" // WiFi Password

#define ONE_DAY_INTERVAL 43200000 // 1/2 วันใน milliseconds (12 * 60 * 60 * 1000)

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
    {1, "JD",       "JD",       50, RELAY_PIN_1, MSG_TYPE_WATER, -1, 0, 0.0, STATE_NORMAL, STATE_NORMAL, 0},
    {2, "PhraNon",  "PhraNon",  50, RELAY_PIN_2, MSG_TYPE_WATER, -1, 0, 0.0, STATE_NORMAL, STATE_NORMAL, 0},
    {3, "Plastic",  "Plastic",  10, 0,           MSG_TYPE_WATER, -1, 0, 0.0, STATE_NORMAL, STATE_NORMAL, 0},
    {4, "ThamYai",  "ThamYai",  50, RELAY_PIN_3, MSG_TYPE_WATER, -1, 0, 0.0, STATE_NORMAL, STATE_NORMAL, 0},
    {5, "RongPui",  "RongPui",  50, RELAY_PIN_4, MSG_TYPE_WATER, -1, 0, 0.0, STATE_NORMAL, STATE_NORMAL, 0},
    {6, "NongPog",  "NongPog",  0,  0,           MSG_TYPE_SPD,   -1, 0, 0.0, STATE_NORMAL, STATE_NORMAL, 0}
    // สามารถเพิ่ม Node ใหม่ได้ที่นี่ เช่น {9, "Tank9", "Tank9", 50, 0, MSG_TYPE_WATER, 0, 0, STATE_NORMAL, STATE_NORMAL, 0}
};
const int numNodes = sizeof(nodes) / sizeof(nodes[0]);
const int LOW_THRESHOLD = 5; // จำนวนครั้งที่ต้องอ่านค่าต่ำกว่า Threshold ติดกันเพื่อแจ้งเตือน

volatile bool buttonPressed = false;     // สถานะว่าปุ่มถูกกดหรือไม่
volatile unsigned long buttonPressTime = 0;  // เวลาที่ปุ่มถูกกด
bool resetTriggered = false;            // ตรวจสอบการทำงานของรีเซ็ต

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

// ==== ฟังก์ชันส่งข้อความไปที่ LINE ====
void sendLineMessage(String message) {
    if (!notificationsEnabled) {
        Serial.println("Notifications disabled. Skipping LINE message.");
        return;
    }

    if (!internetConnected) {
        Serial.println("No Internet. Skipping LINE message.");
        return;
    }

    WiFiClientSecure lineClient;
    lineClient.setInsecure(); // ข้ามการตรวจสอบใบรับรอง SSL

    if (!lineClient.connect("api.line.me", 443)) {
        Serial.println("LINE API Connection Failed");
        return;
    }

    String payload = "{";
    payload += "\"to\": \"" + String(LINE_USER_ID) + "\",";
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
        publishMessage(message_topic, message, true);
    } else {
        Serial.println("LINE API Error: " + statusLine);
        String errorAlert = "ระบบ: ส่ง LINE ไม่สำเร็จ (" + statusLine.substring(9) + ")";
        if (statusLine.indexOf("429") != -1) { // 429 คือ HTTP Status Too Many Requests (โควต้าเต็ม)
            errorAlert = "แจ้งเตือน: โควต้า LINE Messaging API เต็มแล้ว (HTTP 429)!";
        }
        publishMessage(message_topic, errorAlert, false);
    }
    
    // วนลูปเพื่อให้ MQTT Client ทำงานและส่งข้อความออกไปจริงๆ ก่อนที่จะทำอย่างอื่นต่อ
    unsigned long start = millis();
    while (millis() - start < 1000) {
        client.loop();
        delay(10);
    }
    lineClient.stop();
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
        if (millis() - lastResetTime >= ONE_DAY_INTERVAL) {
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

// Function to receive and validate data
void receiveData() {
    int packetSize = LT.receiveSXBuffer(0, 0, WAIT_RX);

    if (packetSize > 0) {
        if (packetSize == sizeof(LoRaPacket) || packetSize == (sizeof(LoRaPacket) + 2)) {
            Serial.println("Received packet (size OK)");
        
            LoRaPacket packet;
            uint8_t rxBuffer[sizeof(LoRaPacket)];

            LT.startReadSXBuffer(0);
            for (int i = 0; i < sizeof(LoRaPacket); i++) {
                rxBuffer[i] = LT.readUint8();
            }
            // ถ้ามี CRC ติดมาด้วย 2 ไบต์ ให้อ่านทิ้งเพื่อให้ Buffer ว่าง
            if (packetSize == (sizeof(LoRaPacket) + 2)) {
                LT.readUint8(); // CRC 1
                LT.readUint8(); // CRC 2
            }
            LT.endReadSXBuffer();
            
            memcpy(&packet, rxBuffer, sizeof(LoRaPacket));

            if (packet.receiverID == 100 && packet.senderID == 10) {
            
            Serial.print("MessageType: "); Serial.println(packet.messageType);
            Serial.print("nodeID: "); Serial.println(packet.sourceNodeID);
            Serial.print("Value: "); Serial.println(packet.value);
            Serial.print("Battery: "); Serial.println(packet.battery);

            for (int i = 0; i < numNodes; i++) {
                if (nodes[i].id == packet.sourceNodeID) {
                    nodes[i].lastValue = (int)packet.value;
                    nodes[i].lastSeen = millis();
                    nodes[i].battery = packet.battery; // บันทึกค่าแบตเตอรี่ที่ได้รับ

                    // กรณีกลับมาทำงานปกติหลังจาก Comm Error
                    if (nodes[i].commState == STATE_COMM_ERROR) {
                        handleNodeRecovery(i);
                    }

                    // Logic สำหรับระดับน้ำ
                    if (nodes[i].msgType == MSG_TYPE_WATER) {
                        if (nodes[i].lastValue < nodes[i].threshold) {
                            nodes[i].lowCount++;
                            if (nodes[i].lowCount >= LOW_THRESHOLD && nodes[i].alertState == STATE_NORMAL) {
                                sendLineMessage("แจ้งเตือน: แท็งค์ " + nodes[i].name + " น้ำเหลือต่ำกว่า " + String(nodes[i].threshold) + "%");
                                activateRelay(nodes[i].id);
                                nodes[i].alertState = STATE_LOW_WATER;
                            }
                        } else {
                            nodes[i].lowCount = 0;
                            // ถ้าน้ำกลับมาปกติ (เกิน Threshold) และเคยเตือนไปแล้ว ให้ Reset State
                            // ในที่นี้สมมติว่าให้ Reset อัตโนมัติเมื่อน้ำเกิน Threshold
                            if (nodes[i].alertState == STATE_LOW_WATER) nodes[i].alertState = STATE_NORMAL;
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
        } else {
                Serial.println("Message does not meet criteria (ID mismatch), ignoring.");
            }
        } else {
            Serial.print("Received packet with unexpected size: "); Serial.print(packetSize); 
            Serial.print(" bytes. Expected "); Serial.print(sizeof(LoRaPacket)); Serial.println(" or +2. Ignoring.");
            LT.endReadSXBuffer();
        }
    }
}

// ฟังก์ชันจัดการเมื่อ Node กลับมาออนไลน์ เพื่อให้โค้ดหลักสะอาดขึ้น
void handleNodeRecovery(int nodeIdx) {
    if (repeaterErrorActive) {
        sendLineMessage("แจ้งเตือน: ระบบการสื่อสาร (Repeater) กลับมาทำงานปกติแล้ว");
        repeaterErrorActive = false;
        lastRepeaterRecoveryTime = millis();
    } else {
        // แจ้งเตือนรายโหนดเฉพาะเมื่อไม่อยู่ในช่วงพัก (Grace Period) หลัง Repeater ฟื้น
        unsigned long timeSinceLastRecovery = millis() - lastRepeaterRecoveryTime;
        if (timeSinceLastRecovery > RECOVERY_GRACE_PERIOD) {
            sendLineMessage("แจ้งเตือน: DataNode " + nodes[nodeIdx].name + " กลับมาทำงานปกติแล้ว");
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
    }
    repeaterErrorActive = false;

    Serial.println("DEBUG: [RELAY OFF] All Pins (1-4) - Reason: Manual Reset or System Init");
    digitalWrite(RELAY_PIN_1, HIGH);
    digitalWrite(RELAY_PIN_2, HIGH);
    digitalWrite(RELAY_PIN_3, HIGH);
    digitalWrite(RELAY_PIN_4, HIGH);
    Serial.println("Notifications and relays reset.");

    display.clear();
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 10, "NOTIFICATION");
    display.drawString(64, 40, "RESET");
    display.display();

    // หน่วงเวลาเพื่อให้ข้อความแสดงผลค้างไว้
    vTaskDelay(5000 / portTICK_PERIOD_MS); // ใช้ vTaskDelay เพื่อให้ Task อื่นยังทำงานได้บ้าง
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

void CheckCommError() {
    unsigned long currentMillis = millis();
    int currentFailingCount = 0;

    // 1. ตรวจสอบจำนวน Node ที่ขาดการติดต่อนานเกินกำหนด (และยังไม่ได้แจ้งเตือน Error)
    for (int i = 0; i < numNodes; i++) {
        if (currentMillis - nodes[i].lastSeen >= commTimeout && nodes[i].commState == STATE_NORMAL) {
            currentFailingCount++;
        }
    }

    // 2. หากเริ่มพบปัญหา ให้เริ่มจับเวลาสังเกตการณ์
    if (currentFailingCount > 0) {
        if (suspectedCommTime == 0) {
            suspectedCommTime = currentMillis;
            Serial.printf("DEBUG: Found %d failing node(s). Starting 5-min observation...\n", currentFailingCount);
        }
    } else {
        suspectedCommTime = 0; // รีเซ็ตหากทุก Node กลับมาปกติก่อนครบเวลา
    }

    // 3. เมื่อครบกำหนดการสังเกตการณ์ ให้ตัดสินใจว่าจะแจ้งเตือนแบบไหน
    if (suspectedCommTime != 0 && (currentMillis - suspectedCommTime >= OBSERVATION_WINDOW)) {
        bool isRepeaterDown = (currentFailingCount >= 3);

        if (isRepeaterDown) {
            sendLineMessage("แจ้งเตือน: พบความผิดปกติที่ Repeater (อาจเกิดจากไฟดับหรืออุปกรณ์ขัดข้อง)");
            repeaterErrorActive = true;
        }

        for (int i = 0; i < numNodes; i++) {
            if (currentMillis - nodes[i].lastSeen >= commTimeout && nodes[i].commState == STATE_NORMAL) {
                if (!isRepeaterDown) {
                    sendLineMessage("แจ้งเตือน: การสื่อสารขัดข้องที่ DataNode " + nodes[i].name);
                }
                nodes[i].commState = STATE_COMM_ERROR;
            }
        }
        suspectedCommTime = 0;
    }
}


void DisplayTimerTask(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();

    for (;;) {
        int currentTotalDisplayItems = numNodes;
        if (!internetConnected) {
            currentTotalDisplayItems++; // เพิ่ม 1 ช่องสำหรับ "Internet Error"
        }

        // สลับแสดงผล: ข้อมูล Node -> หน้าว่าง (Flash) หรือ Internet Error -> หน้าว่าง
        if (displayState % 2 == 0) {
            if (activeNodeIdx < numNodes) {
                // แสดงข้อมูล Node ปัจจุบัน
                Node &n = nodes[activeNodeIdx];
                if (n.commState == STATE_COMM_ERROR) {
                    displayData(n.name, "CommError");
                } else if (n.lastValue == -1) {
                    displayData(n.name, "Waiting...");
                } else if (n.msgType == MSG_TYPE_SPD) {
                    displayData(n.name, n.lastValue == 0 ? "SPD OK" : "SPD XX");
                } else {
                    displayData(n.name, String(n.lastValue) + " %");
                }
            } else if (activeNodeIdx == numNodes && !internetConnected) {
                // แสดง Internet Error หาก activeNodeIdx ชี้ไปที่ช่องสุดท้ายและไม่มีเน็ต
                displayData("Internet", "Error");
            } else {
                // กรณี activeNodeIdx ชี้ไปที่ช่อง Internet Error แต่เน็ตต่ออยู่ ให้แสดงหน้าว่างแทน
                display.clear();
                display.display();
            }
        } else {
            // หน้าว่าง
            display.clear();
            display.display();
            // ขยับไป Node ถัดไปหลังจากหน้าว่าง
            activeNodeIdx = (activeNodeIdx + 1) % currentTotalDisplayItems;
        }

        TickType_t delayTime = (displayState % 2 == 0) ? displayInterval : pauseInterval;
        displayState = (displayState + 1) % (currentTotalDisplayItems * 2); // วนรอบตามจำนวนรายการทั้งหมด (ข้อมูล + หน้าว่าง)

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
        
        if (client.connect(clientId.c_str(), mqtt_username, mqtt_password)) {
            Serial.println("connected");
            client.subscribe("reset_notification");
            client.subscribe("reset_system");
            client.subscribe(notification_control_topic);
            client.subscribe(request_data_topic);
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
    if (client.publish(topic, payload.c_str(), retained)) {
        Serial.println("Message publised ["+String(topic)+"]: "+payload);
    }
}

// Function to send MQTT data every 10 minutes
void sendMQTTData() {
    publishMessage(notification_status_topic, notificationsEnabled ? "1" : "0", true);

    // วน Loop ส่งข้อมูลของทุก Node ขึ้น MQTT ตาม topic ที่กำหนดใน Struct
    for (int i = 0; i < numNodes; i++) {
        // ส่งเฉพาะค่าล่าสุดที่มี
        publishMessage(nodes[i].mqttTopic, String(nodes[i].lastValue), true);
        
        // ส่งค่าแบตเตอรี่ขึ้น MQTT (เฉพาะ Node ที่มีค่าแบตเตอรี่ > 0)
        String batTopic = String(nodes[i].mqttTopic) + "/battery";
        publishMessage(batTopic.c_str(), String(nodes[i].battery, 2), true);
    }

    if (newDataReceived) {
        // Reset the newDataReceived flag
        newDataReceived = false;
    }
}


void setup() {
    Serial.begin(115200);
    Serial.println("Master Node 6 Initializing...");

    SPI.begin();
    delay(500);

    VextON();
    delay(100);

    // Initialize OLED Display
    display.init();
    display.clear();
    display.setContrast(150);
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
    xTaskCreate(ButtonTask, "ButtonTask", 2048, NULL, 1, &TaskButton);

    // 1. เคลียร์ค่าและแสดงหน้าจอ RESET ให้จบกระบวนการ (5 วินาที)
    resetNotification();

    // แสดงหน้าจอ Waiting สำหรับแต่ละโหนดเมื่อเปิดเครื่อง เพื่อให้ทราบสถานะเริ่มต้น
    for (int i = 0; i < numNodes; i++) {
        displayData(nodes[i].name, "Waiting...");
        delay(1000);
    }

    // 2. หลังจาก Reset จบแล้ว ค่อยเริ่ม Task วนแสดงผลระดับน้ำ เพื่อไม่ให้หน้าจอซ้อนกัน
    xTaskCreate(DisplayTimerTask, "DisplayTimerTask", 5000, NULL, 1, &TaskDisplay);

    // เชื่อมต่อ Wi-Fi
    WiFi.mode(WIFI_STA);
    wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);

    espClient.setCACert(root_ca);
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(callback);
    
    internetConnected = false; // เริ่มต้นเป็น false จนกว่าจะเช็คผ่าน

    if (!initLoRa()) {
        while (1);  // Stop if LoRa initialization fails
    }
    LT.setupLoRa(Frequency, Offset, SpreadingFactor, Bandwidth, CodeRate, Optimisation);
    delay(1000);

    // เริ่มต้นเวลาสื่อสารสำหรับทุก Node
    for (int i = 0; i < numNodes; i++) {
        nodes[i].lastSeen = millis();
    }

    // Set up the MQTT Timer to call `sendMQTTData` every 10 minutes
    mqttTimer.setInterval(mqttSendInterval, sendMQTTData);
    // Create tasks
    xTaskCreate(MainTask, "MainTask", 10000, NULL, 1, &TaskMain);

    // สร้าง Task สำหรับรีเซ็ตอัตโนมัติ
    // disableชั่วคราวเพื่อทดสอบ
    //xTaskCreate(ResetTimerTask, "ResetTimerTask", 1000, NULL, 1, &TaskResetTimer);


}

// Task for Button Logic (Separated for responsiveness)
void ButtonTask(void *parameter) {
    for (;;) {
        bool isPressed = (digitalRead(RESET_BUTTON_PIN) == LOW); // ทำงานร่วมกับ RC Filter 2k/4.7uF

        // ให้ไฟขาว (Pin 35) ติดเวลากดปุ่ม และดับเมื่อปล่อย
        digitalWrite(35, isPressed ? HIGH : LOW);

        if (isPressed) {
            if (!buttonPressed) {
                vTaskDelay(50 / portTICK_PERIOD_MS); // Software Debounce ซ้ำอีกชั้น
                if (digitalRead(RESET_BUTTON_PIN) == LOW) {
                    buttonPressed = true;
                    buttonPressTime = millis();
                    Serial.println("DEBUG: Button Pressed (Pin 1 - Protected)");
                }
            } else {
                // ตรวจสอบว่ากดค้างนานพอหรือยัง
                if (!resetTriggered && (millis() - buttonPressTime >= LONG_PRESS_DURATION)) {
                    Serial.println("DEBUG: Long press confirmed! Executing reset...");
                    vTaskSuspend(TaskDisplay);      
                    resetNotification();            
                    vTaskResume(TaskDisplay);       
                    resetTriggered = true;          
                }
            }
        } else {
            if (buttonPressed) {
                Serial.println("DEBUG: Button Released");
            }
            buttonPressed = false;
            resetTriggered = false;
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
    CheckCommError();

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
        if (wifiMulti.run() == WL_CONNECTED) {
             Serial.println("Wi-Fi reconnected");
             lastInternetCheck = 0; // บังคับให้เช็คเน็ตทันทีในรอบถัดไป
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
            mqttTimer.run();

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
                publishMessage(message_topic, "Manual data update sent", false);
                
                sendMQTTData(); // เรียกฟังก์ชันเดิมที่วน loop ให้อยู่แล้ว
                
                mqttRequestData = false;
                unsigned long start = millis();
                while (millis() - start < 1000) {
                    client.loop();
                    delay(10);
                }
            }
        }
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);  // ตรวจทุก 100ms
  }
}

void loop() {

    // Continuously receive data
    receiveData();  // Call your function to receive data
    delay(10);
    yield();
}