#pragma once
// คัดลอกไฟล์นี้เป็น include/secrets.h แล้วใส่ค่าจริง (secrets.h ไม่ถูก commit ขึ้น git)

// ส่ง log/ระดับน้ำขึ้นเว็บแอป (HISTORY_TOKEN ต้องตรงกับใน dashboard/.env) เว้นว่างไว้ถ้าไม่ใช้
#define HISTORY_URL "https://your-service.a.run.app/api/history"
#define HISTORY_TOKEN ""

#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"

#define MQTT_SERVER "your-mqtt-server"
#define MQTT_USERNAME "your-mqtt-user"
#define MQTT_PASSWORD "your-mqtt-password"

#define LINE_CHANNEL_ACCESS_TOKEN "your-line-channel-access-token"
#define LINE_USER_ID_SECRET "your-line-user-id"
#define LINE_ADMIN_USER_ID_SECRET "your-line-admin-user-id"
