// ใช้ได้ดี ทดสอบ linemessaging API แล้วใช้ได้ 
//แก้ไขหน้าจอทับซ้อนเวลาเน็ตหลุด แก้ไขหน้าจอแสดงผลตลอดเวลาทำให้หน้าจอหมอง
// แก้ไข การแจ้งเตือนเมื่อเซ็นเซอร์ทำงานผิดพลาด โดยให้มีการนับ หากน้ำต่ำกว่าครึ่งแทงค์ถึงสามครั้งจึงจะแจ้งเตือน 
// แก้ไขไฟลล์ให้เป็นไปตามมาตรฐาน C++11 เปลี่ยนนามสกุล .ino เป็น .cpp เพิ่มการ declare ฟังก์ชันข้างบน main code
// disable auto reset function for testing purpose


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
void displaySPDStatus();
void CeckCommError();
void DisplayTimerTask(void *pvParameters);
void reconnect();
void callback(char* topic, byte* payload, unsigned int length);
void publishMessage(const char* topic, String payload , boolean retained);
void sendMQTTData();
void MainTask(void *parameter);
void MqttTimerTask(void *parameter);

// Task handles
TaskHandle_t TaskMain;
TaskHandle_t TaskMqttTimer;
TaskHandle_t TaskResetTimer; // Task Handle สำหรับรีเซ็ตบอร์ด
TaskHandle_t TaskDisplay;

// Timer สำหรับการอัปเดตหน้าจอ และ MQTT
SimpleTimer displayTimer;
SimpleTimer mqttTimer;

// ตัวแปรสำหรับสถานะการแสดงผล (วนรอบระหว่าง 0-12)
// ตัวแปร global track state และพักหน้าจอ
volatile int displayState = 0;         // 0..12 คือข้อมูลแต่ละหน้า

const TickType_t displayInterval = 3500 / portTICK_PERIOD_MS; // 3500 ms
const TickType_t pauseInterval   = 500 / portTICK_PERIOD_MS;  // 100 ms

unsigned long lastStateChange = 0;  // เก็บเวลาที่เปลี่ยนสถานะครั้งล่าสุด

// ประกาศตัวแปรเก็บสถานะเน็ต (ต้องประกาศให้ถูกต้องตามโปรเจคคุณ)
volatile bool internetConnected = false;  // กำหนดค่าเริ่มต้นตามสถานะจริงของเน็ต

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
unsigned long lastMsg = 0;

#define MSG_BUFFER_SIZE (50)
char msg[MSG_BUFFER_SIZE];

// Node topics
const char* sensor1_topic = "JD";
const char* sensor2_topic = "PhraNon";
const char* sensor3_topic = "ThamYai";
const char* sensor4_topic = "RongPui";  // New Node RongPui
const char* sensor5_topic = "NongPog";      // SPD topic

const unsigned long mqttSendInterval = 600000;
// Flag to track when new data is received
bool newDataReceived = false;

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

#define RESET_BUTTON_PIN 48
#define LONG_PRESS_DURATION 2000  // กำหนดเวลาการกดค้าง (2 วินาที)

WiFiMulti wifiMulti;

#define WIFI_SSID "SolarCell" // "iPhone" // WiFi Name
#define WIFI_PASSWORD "12345678" // WiFi Password


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
bool notification1Sent = false;
bool notification2Sent = false;
bool notification3Sent = false;
bool notification4Sent = false;
bool notification5Sent = false;

bool notification12Sent = false;
bool notification22Sent = false;
bool notification32Sent = false;
bool notification42Sent = false;
bool notification52Sent = false; 

int waterLevelJD = 0;
int waterLevelPhraNon = 0;
int waterLevelThamYai = 0;
int waterLevelRongPui = 0;  // Water level for RongPui
int spdStatus = 0;          // 0 for Good, 1 for Broken

// เพิ่มตัวนับสำหรับป้องกัน false trigger (debounce แบบซอฟต์แวร์)
int lowCountJD = 0;
int lowCountPhraNon = 0;
int lowCountThamYai = 0;
int lowCountRongPui = 0;

const int LOW_THRESHOLD = 3; // ต้องต่ำติดกันกี่ครั้งถึงจะแจ้งเตือน


// Variables to track last received time
unsigned long lastReceivedJD = 0;
unsigned long lastReceivedPhraNon = 0;
unsigned long lastReceivedThamYai = 0;
unsigned long lastReceivedRongPui = 0;  // Last received time for RongPui
unsigned long lastReceivedNongPog = 0;
const unsigned long commTimeout = 1500000; // 25 minutes in milliseconds

volatile bool buttonPressed = false;     // สถานะว่าปุ่มถูกกดหรือไม่
volatile unsigned long buttonPressTime = 0;  // เวลาที่ปุ่มถูกกด
bool resetTriggered = false;            // ตรวจสอบการทำงานของรีเซ็ต

// ==== ฟังก์ชันส่งข้อความไปที่ LINE ====
void sendLineMessage(String message) {
    WiFiClientSecure client;
    client.setInsecure(); // ข้ามการตรวจสอบใบรับรอง SSL

    if (!client.connect("api.line.me", 443)) {
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

    client.print(request);

    Serial.println("LINE Message Sent: " + message);
    delay(200);
    client.stop();
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

void IRAM_ATTR handleButtonPress() {
    if (digitalRead(RESET_BUTTON_PIN) == LOW) {
        buttonPressed = true;               // ปุ่มถูกกด
        buttonPressTime = millis();         // บันทึกเวลาที่กดปุ่ม
    } else {
        buttonPressed = false;              // ปุ่มถูกปล่อย
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
        Serial.println("Received packet");

        LT.startReadSXBuffer(0);
        
        uint8_t messageType = LT.readUint8();
        uint8_t receiverID = LT.readUint8();
        uint8_t senderId = LT.readUint8();
        uint8_t nodeID = LT.readUint8();
        uint8_t message = LT.readUint8();

        LT.endReadSXBuffer();
        if ((messageType == 1 || messageType == 2 || messageType == 3) && receiverID == 6) {
            
            Serial.print("MessageType: "); Serial.println(messageType);
            Serial.print("ReceiverID: "); Serial.println(receiverID);
            Serial.print("SenderID: "); Serial.println(senderId);
            Serial.print("nodeID: "); Serial.println(nodeID);
            Serial.print("Message: "); Serial.println(message);

            // Check if sender is Node 5 (Repeater Node)
            if (senderId == 5) {
              if (messageType == 1) {
                int waterLevel = (int)message;
                if (nodeID == 1) {
                    waterLevelJD = waterLevel; // Store JD water level
                    lastReceivedJD = millis(); // Update last received time
                    // Debounce / require LOW_THRESHOLD consecutive low readings
                    if (waterLevel < 50) {
                        lowCountJD++;
                        if (lowCountJD >= LOW_THRESHOLD && !notification1Sent) {
                            sendLineMessage("แท็งค์เจดีย์น้ำเหลือครึ่งแท็งค์!!!");
                            Serial.println("Notification Sent JD");
                            activateRelay(nodeID);
                            notification1Sent = true;
                            lowCountJD = 0; // reset counter after notifying
                        }
                    } else {
                        // reset counter when reading is back to normal
                        lowCountJD = 0;
                    }
                } else if (nodeID == 2) {
                    waterLevelPhraNon = waterLevel; // Store PhraNon water level
                    lastReceivedPhraNon = millis(); // Update last received time
                    if (waterLevel < 50) {
                        lowCountPhraNon++;
                        if (lowCountPhraNon >= LOW_THRESHOLD && !notification2Sent) {
                            sendLineMessage("แท็งค์พระนอนน้ำเหลือครึ่งแท็งค์!!!");
                            Serial.println("Notification Sent PhraNon");
                            activateRelay(nodeID);
                            notification2Sent = true;
                            lowCountPhraNon = 0;
                        }
                    } else {
                        lowCountPhraNon = 0;
                    }
                } else if (nodeID == 3) {
                    waterLevelThamYai = waterLevel; // Store ThamYai water level
                    lastReceivedThamYai = millis(); // Update last received time
                    if (waterLevel < 50) {
                        lowCountThamYai++;
                        if (lowCountThamYai >= LOW_THRESHOLD && !notification3Sent) {
                            sendLineMessage("แท็งค์ถ้ำใหญ่น้ำเหลือครึ่งแท็งค์!!!");
                            Serial.println("Notification Sent ThamYai");
                            activateRelay(nodeID);
                            notification3Sent = true;
                            lowCountThamYai = 0;
                        }
                    } else {
                        lowCountThamYai = 0;
                    }
                } else if (nodeID == 4) {
                    waterLevelRongPui = waterLevel; // Store RongPui water level
                    lastReceivedRongPui = millis(); // Update last received time
                    if (waterLevel < 50) {
                        lowCountRongPui++;
                        if (lowCountRongPui >= LOW_THRESHOLD && !notification4Sent) {
                            sendLineMessage("แท็งค์โรงปุ๋ยน้ำเหลือครึ่งแท็งค์!!!");
                            Serial.println("Notification Sent RongPui");
                            activateRelay(nodeID);
                            notification4Sent = true;
                            lowCountRongPui = 0;
                        }
                    } else {
                        lowCountRongPui = 0;
                    }
                }
              } else if (messageType == 2 && nodeID == 7) {
                spdStatus = (int)message;
                lastReceivedNongPog = millis(); // Update last received time
                if (spdStatus == 1 && !notification5Sent) {
                    sendLineMessage("SPD หมดสภาพแล้ว ตรวจสอบด่วน!!!");
                    Serial.print("Notification Sent SPD");
                    notification5Sent = true;
                    // No relay activation for SPD, just notify via LINE.
                }
              }
            newDataReceived = true; // Set flag to indicate new data was received
            }
        
        } else {
            Serial.println("Message does not meet criteria, ignoring.");
        }
        
  }

}


// Function to activate relay based on tank ID
void activateRelay(uint8_t nodeID) {
    switch (nodeID) {
        case 1:
            digitalWrite(RELAY_PIN_1, LOW);  // JD
            break;
        case 2:
            digitalWrite(RELAY_PIN_2, LOW);  // Phranon
            break;
        case 3:
            digitalWrite(RELAY_PIN_3, LOW);  // ThamYai
            break;
        case 4:
            digitalWrite(RELAY_PIN_4, LOW);  // RongPui
            break;
        default:
            break;
    }
}

// Function to reset notifications and relays
void resetNotification() {
    // หยุดการทำงานของ Task แสดงผลชั่วคราว เพื่อป้องกันการเขียนทับหน้าจอ
    vTaskSuspend(TaskDisplay);

    notification1Sent = false;
    notification2Sent = false;
    notification3Sent = false;
    notification4Sent = false;  
    notification5Sent = false;  

    notification12Sent = false;
    notification22Sent = false;
    notification32Sent = false;
    notification42Sent = false;  
    notification52Sent = false;  

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

    vTaskDelay(5000 / portTICK_PERIOD_MS); // ใช้ vTaskDelay แทน delay()
    display.clear();
    display.display();

    vTaskResume(TaskDisplay); // ให้ Task แสดงผลกลับมาทำงานตามปกติ

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

// Function to display SPD status on OLED
void displaySPDStatus() {
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setFont(ArialMT_Plain_24);
    display.drawString(64, 5, "SPD");
    display.drawString(64, 30, spdStatus == 0 ? "Working" : "Failed");
    display.display();
}

void CeckCommError() {
  unsigned long currentMillis = millis();  // อัปเดต currentMillis ที่นี่

  if (currentMillis - lastReceivedJD >= commTimeout && !notification12Sent) {
      sendLineMessage("การสื่อสารขัดข้อง!!! ตรวจสอบ DataNode แท็งค์เจดีย์!");
      notification12Sent = true;
    }
  if (currentMillis - lastReceivedPhraNon >= commTimeout && !notification22Sent) {
      sendLineMessage("การสื่อสารขัดข้อง!!! ตรวจสอบ DataNode แท็งค์พระนอน!");
      notification22Sent = true;
    }
  if (currentMillis - lastReceivedThamYai >= commTimeout && !notification32Sent) {
      sendLineMessage("การสื่อสารขัดข้อง!!! ตรวจสอบ DataNode แท็งค์ถ้ำใหญ่!");
      notification32Sent = true;
    }
  if (currentMillis - lastReceivedRongPui >= commTimeout && !notification42Sent) {
      sendLineMessage("การสื่อสารขัดข้อง!!! ตรวจสอบ DataNode โรงปุ๋ย!");
      notification42Sent = true;
    }
  if (currentMillis - lastReceivedNongPog >= commTimeout && !notification52Sent) {
      sendLineMessage("การสื่อสารขัดข้อง!!! ตรวจสอบ DataNode หนองพอก!");
      notification52Sent = true;
    }
}


void DisplayTimerTask(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();

    for (;;) {
        // แสดงผลตาม state
        switch (displayState) {
            case 0:
                if (notification12Sent) displayData("JD", "CommError");
                else displayData("JD", String(waterLevelJD) + " %");
                break;

            case 1:
                display.clear();
                display.display();
                break;

            case 2:
                if (notification22Sent) displayData("PhraNon", "CommError");
                else displayData("PhraNon", String(waterLevelPhraNon) + " %");
                break;

            case 3:
                display.clear();
                display.display();
                break;

            case 4:
                if (notification32Sent) displayData("ThamYai", "CommError");
                else displayData("ThamYai", String(waterLevelThamYai) + " %");
                break;

            case 5:
                display.clear();
                display.display();
                break;

            case 6:
                if (notification42Sent) displayData("RongPui", "CommError");
                else displayData("RongPui", String(waterLevelRongPui) + " %");
                break;

            case 7:
                display.clear();
                display.display();
                break;

            case 8:
                if (notification52Sent) displayData("SPD", "CommError");
                else displaySPDStatus();
                break;

            case 9:
                display.clear();
                display.display();
                break;

            case 10:
                displayData("Internet", "Error");
                break;

            case 11:
                display.clear();
                display.display();
                break;
        }

        // คำนวณจำนวน state รวมพักหน้าจอ
        int maxState = internetConnected ? 10 : 12;

        // รอเวลาตามประเภท state
        TickType_t delayTime = (displayState % 2 == 0) ? displayInterval : pauseInterval;

        // ไป state ถัดไป
        displayState = (displayState + 1) % maxState;

        // delay แบบ vTaskDelayUntil เพื่อให้เวลา delay คงที่แม่นยำ
        vTaskDelayUntil(&xLastWakeTime, delayTime);
    }
}

void reconnect() {
// Loop until we’re reconnected
while (!client.connected()) {
Serial.print( "Attempting MQTT connection..." );
String clientId = "ESP32Client-"; // Create a random client ID
clientId += String(random(0xffff), HEX);
// Attempt to connect
if (client.connect(clientId.c_str(), mqtt_username, mqtt_password)) {
Serial.println("connected");

  client.subscribe("reset_notification");
  client.subscribe("reset_system");
  
     // subscribe the topics here
  //client.subscribe(command2_topic);   // subscribe the topics here
} else {
  Serial.print("failed, rc=");
  Serial.print(client.state());
  Serial.println(" try again in 5 seconds");   // Wait 5 seconds before retrying
  delay(5000);
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
            resetNotification();
        }
    }

    if (strcmp(topic, "reset_system") == 0) {
        if (incommingMessage.equals("1")) {
            resetSystem();
        }
    }
}

//======================================= publising as string
void publishMessage(const char* topic, String payload , boolean retained) {
if (client.publish(topic, payload.c_str(), true))
Serial.println("Message publised ["+String(topic)+"]: "+payload);
}

// Function to send MQTT data every 10 minutes
void sendMQTTData() {
    if (newDataReceived) {
        // Publish the latest water levels and SPD status to MQTT topics
        publishMessage(sensor1_topic, String(waterLevelJD), true);
        publishMessage(sensor2_topic, String(waterLevelPhraNon), true);
        publishMessage(sensor3_topic, String(waterLevelThamYai), true);
        publishMessage(sensor4_topic, String(waterLevelRongPui), true);
        publishMessage(sensor5_topic, String(spdStatus), true);

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
    display.setContrast(255);
    display.display();   
    
    pinMode(RELAY_PIN_1, OUTPUT);
    pinMode(RELAY_PIN_2, OUTPUT);
    pinMode(RELAY_PIN_3, OUTPUT);
    pinMode(RELAY_PIN_4, OUTPUT);

    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);  // ตั้งค่าปุ่มรีเซ็ตเป็น INPUT_PULLUP

    // ตั้งค่า Interrupt เมื่อปุ่มมีการเปลี่ยนแปลงสถานะ
    attachInterrupt(digitalPinToInterrupt(RESET_BUTTON_PIN), handleButtonPress, CHANGE);

    digitalWrite(RELAY_PIN_1, HIGH);
    digitalWrite(RELAY_PIN_2, HIGH);
    digitalWrite(RELAY_PIN_3, HIGH);
    digitalWrite(RELAY_PIN_4, HIGH);

    Serial.println("Resetting All Values...");
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 10, "VALUES");
    display.drawString(64, 40, "RESET");
    display.display();

    delay(5000);

    // กำหนดค่าเริ่มต้น
    waterLevelJD = 0;
    waterLevelPhraNon = 0;
    waterLevelThamYai = 0;
    waterLevelRongPui = 0;
    spdStatus = 0;

    // เรียก resetNotification() เพื่อล้างค่าทั้งหมด
    resetNotification();

    // เชื่อมต่อ Wi-Fi
    WiFi.mode(WIFI_STA);
    wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);

    espClient.setCACert(root_ca);
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(callback);
    
    // ตรวจสอบการเชื่อมต่อ Wi-Fi
    if (wifiMulti.run() == WL_CONNECTED) {
        Serial.println("Wi-Fi connected");
        internetConnected = true;
    } else {
        Serial.println("unable to connect Wi-Fi");
        internetConnected = false;
    }

    if (!initLoRa()) {
        while (1);  // Stop if LoRa initialization fails
    }
    LT.setupLoRa(Frequency, Offset, SpreadingFactor, Bandwidth, CodeRate, Optimisation);
    delay(1000);

    // Initialize last received times to current time
    lastReceivedJD = millis();
    lastReceivedPhraNon = millis();
    lastReceivedThamYai = millis();
    lastReceivedRongPui = millis();
    lastReceivedNongPog = millis();

    // Set up the MQTT Timer to call `sendMQTTData` every 10 minutes
    mqttTimer.setInterval(mqttSendInterval, sendMQTTData);
    // Create tasks
    xTaskCreate(MainTask, "MainTask", 10000, NULL, 1, &TaskMain);
    // Create MQTT Timer Task
    xTaskCreate(MqttTimerTask, "MqttTimerTask", 10000, NULL, 1, &TaskMqttTimer);
    xTaskCreate(DisplayTimerTask, "DisplayTimerTask", 5000, NULL, 1, &TaskDisplay);

    // สร้าง Task สำหรับรีเซ็ตอัตโนมัติ
    // disableชั่วคราวเพื่อทดสอบ
    //xTaskCreate(ResetTimerTask, "ResetTimerTask", 1000, NULL, 1, &TaskResetTimer);


}

// Task for main logic
void MainTask(void *parameter) {
  
  while (true) {
    // ตรวจสอบว่าปุ่มถูกกดและค้างไว้นานกว่า LONG_PRESS_DURATION หรือไม่
    if (buttonPressed && (millis() - buttonPressTime >= LONG_PRESS_DURATION)) {
        if (!resetTriggered) {
            Serial.println("Long press detected! Resetting notifications.");
            resetNotification();            // เรียกใช้ฟังก์ชันรีเซ็ตการแจ้งเตือน
            resetTriggered = true;          // ป้องกันการเรียกซ้ำ
        }
    }

    // ถ้าปุ่มถูกปล่อยแล้วรีเซ็ตการตรวจจับ
    if (!buttonPressed && resetTriggered) {
        resetTriggered = false;  // รีเซ็ตสถานะการทำงานของรีเซ็ต
    }

    CeckCommError();

    // เรียก wifiMulti.run() ทุกครั้งเพื่อตรวจสอบและพยายามเชื่อมต่อใหม่
    if (wifiMulti.run() == WL_CONNECTED) {
        if (!internetConnected) {
            Serial.println("Wi-Fi reconnected");
            internetConnected = true;
        }
    } else {
        if (internetConnected) {
            Serial.println("Wi-Fi disconnected. Reconnecting...");
        }
        internetConnected = false;
    }

    // MQTT
    if (internetConnected) {
        if (!client.connected()) reconnect();
        client.loop();
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);  // ตรวจทุก 100ms
  }
}

// Task to run the MQTT Timer
void MqttTimerTask(void *parameter) {
    while (true) {
        mqttTimer.run(); // Call mqttTimer.run() to manage the scheduled tasks

        // Sleep the task for a short time to prevent high CPU usage
        vTaskDelay(60000 / portTICK_PERIOD_MS); // Run the timer every 1 minute
    }
}

void loop() {

    // Continuously receive data
    receiveData();  // Call your function to receive data
    delay(10);
    yield();
}