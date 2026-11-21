#include <Arduino.h>

// ขาที่ต้องการทดสอบ
#define TEST_BUTTON_PIN 1
#define ONBOARD_LED_PIN 35

void setup() {
    // เริ่มต้น Serial Monitor
    Serial.begin(115200);
    while (!Serial); // รอจนกว่า Serial จะพร้อม
    
    Serial.println("");
    Serial.println("========================================");
    Serial.println("  HARDWARE PIN 1 + PROTECTION CIRCUIT   ");
    Serial.println("  Components: 2k Ohm & 4.7uF Cap        ");
    Serial.println("========================================");
    
    // ตั้งค่า Pin 1 เป็น Input Pullup (ถ้าไม่กดต้องเป็น HIGH)
    pinMode(TEST_BUTTON_PIN, INPUT_PULLUP);
    
    // ตั้งค่า LED บนบอร์ดเป็น Output
    pinMode(ONBOARD_LED_PIN, OUTPUT);
    
    Serial.println("Status: Initialized.");
    Serial.println("Observation: LED 35 will fade in/out slightly due to 4.7uF Cap.");
}

void loop() {
    // อ่านค่าดิบจาก Pin 1
    int buttonValue = digitalRead(TEST_BUTTON_PIN);
    
    // แสดงผลผ่าน Serial Monitor ทุกๆ 250ms เพื่อไม่ให้จอลายเกินไป
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 250) {
        Serial.print("Pin 1 Value: ");
        if (buttonValue == LOW) {
            Serial.println("[LOW]  <--- Pressed (Safe via 2k Resistor)");
        } else {
            Serial.println("[HIGH] Released (Protected)");
        }
        lastPrint = millis();
    }
    
    // ควบคุม LED บนบอร์ดตามสถานะปุ่ม
    // บน Heltec V3: ขา 35 HIGH คือไฟติด, LOW คือไฟดับ
    if (buttonValue == LOW) {
        digitalWrite(ONBOARD_LED_PIN, HIGH); 
    } else {
        digitalWrite(ONBOARD_LED_PIN, LOW);
    }
}