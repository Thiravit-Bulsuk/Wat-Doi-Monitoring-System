// โค้ดทดสอบฮาร์ดแวร์ของบอร์ด Master เท่านั้น: วน เปิด/ปิด Relay ทั้ง 4 ตัว + ไฟ LED (pin 35) สลับกันไปเรื่อยๆ
// ไม่มี LoRa/WiFi/MQTT ใดๆ เพื่อตัดปัจจัยอื่นออก ใช้ตรวจเช็คว่า Relay/สาย/ไฟ LED ต่อถูกต้องหรือไม่
#include <Arduino.h>

#define RELAY_PIN_1 7
#define RELAY_PIN_2 6
#define RELAY_PIN_3 5
#define RELAY_PIN_4 4
#define LED_PIN 35 // ไฟ LED สีขาวบนบอร์ด Heltec V3

const uint8_t relayPins[4] = {RELAY_PIN_1, RELAY_PIN_2, RELAY_PIN_3, RELAY_PIN_4};
const int STEP_DELAY_MS = 1000; // หน่วงเวลาต่อ 1 จังหวะ

void setup() {
    Serial.begin(115200);
    Serial.println("Master Relay/LED Hardware Test Starting...");

    for (int i = 0; i < 4; i++) {
        pinMode(relayPins[i], OUTPUT);
        digitalWrite(relayPins[i], HIGH); // Relay Active LOW: HIGH = ปิด (เริ่มต้นให้ปิดทั้งหมด)
    }
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
}

void loop() {
    // วนเปิดทีละตัวเรียงกัน 1 -> 2 -> 3 -> 4 แล้ววนซ้ำ พร้อมกับกะพริบ LED ตามจังหวะ
    for (int i = 0; i < 4; i++) {
        Serial.printf("Relay %d ON (Pin %d)\n", i + 1, relayPins[i]);
        digitalWrite(relayPins[i], LOW);  // Active LOW = เปิด
        digitalWrite(LED_PIN, HIGH);
        delay(STEP_DELAY_MS);

        digitalWrite(relayPins[i], HIGH); // ปิดก่อนไปตัวถัดไป
        digitalWrite(LED_PIN, LOW);
        Serial.printf("Relay %d OFF\n", i + 1);
        delay(STEP_DELAY_MS);
    }
}
