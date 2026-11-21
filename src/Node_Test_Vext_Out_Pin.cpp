#include <Arduino.h>

// หากบอร์ดไม่ได้ Define ค่า Vext ไว้ใน Board Support Package
// สามารถระบุพินโดยตรงได้ (Heltec ESP32 V3 ใช้ GPIO 36)
#ifndef Vext
  #define Vext 36
#endif

void setup() {
  Serial.begin(115200);
  delay(1000);

  // ตั้งค่าพิน Vext ให้เป็น OUTPUT
  pinMode(Vext, OUTPUT);
  Serial.println("=== เริ่มต้นการทดสอบพิน Vext ===");
}

void loop() {
  // 1. เปิดการจ่ายไฟ Vext (Active LOW -> สั่ง LOW เพื่อเปิด)
  digitalWrite(Vext, LOW);
  Serial.println("[Vext STATUS] ON  (LOW)  - จ่ายไฟ 3.3V ออกพิน Vext");
  delay(6000);

  // 2. ปิดการจ่ายไฟ Vext (Active LOW -> สั่ง HIGH เพื่อปิด)
  digitalWrite(Vext, HIGH);
  Serial.println("[Vext STATUS] OFF (HIGH) - ตัดการจ่ายไฟพิน Vext");
  delay(6000);
}