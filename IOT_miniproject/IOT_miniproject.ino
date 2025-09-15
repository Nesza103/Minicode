#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>

// ========= WIFI =========
const char* WIFI_SSID = "JoJuJim B1";
const char* WIFI_PASS = "64972882";

// ========= TELEGRAM =========
const char* BOT_TOKEN = "8303681023:AAF3WC74sH2Dqc7xE_Jn4Oyjm_KUE0pt-hc";
const char* CHAT_ID   = "8076007996";   // ตัวเลข id ของคุณ

// ========= SENSORS =========
// XKC-Y25 (NPN, Active-Low) ต่อ DO -> D5
const int WATER_SENSOR_PIN = D5;  // LOW=เจอน้ำ, HIGH=ไม่เจอน้ำ
// SW-420 DO -> D2
const int VIB_SENSOR_PIN   = D2;  // HIGH=สั่น, LOW=เงียบ

// ========= TIMING =========
const unsigned long STABLE_MS = 30UL * 1000UL;        // ต้องนิ่ง 30s ก่อนแจ้งครั้งแรก
const unsigned long REPEAT_MS = 5UL * 60UL * 1000UL;  // แจ้งซ้ำทุก 5 นาที
const unsigned long POLL_UPDATES_MS = 5000UL;         // ดึง getUpdates ทุก 5 วินาที

// ========= STATE =========
bool trackingCondition = false;     // กำลังอยู่ในสภาพ "ไม่เจอน้ำ + ไม่สั่น" ต่อเนื่องหรือไม่
unsigned long conditionStartMs = 0; // เวลาเริ่มเข้าเงื่อนไข
unsigned long lastNotifyMs = 0;     // เวลาแจ้งล่าสุด
bool muted = false;                 // ผู้ใช้กดปุ่มหยุดแจ้งเตือนแล้วหรือยัง (สำหรับรอบนี้)
unsigned long lastPollMs = 0;       // เวลา poll getUpdates ล่าสุด
unsigned long updatesOffset = 0;    // offset สำหรับ getUpdates เพื่อไม่อ่านซ้ำ

WiFiClientSecure secureClient;

void sendTelegram(const String& text) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient https;
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/sendMessage";
  if (https.begin(secureClient, url)) {
    https.addHeader("Content-Type", "application/json");
    String payload = "{\"chat_id\":\"" + String(CHAT_ID) + "\",\"text\":\"" + text + "\"}";
    https.POST(payload);
    https.end();
  }
}

void sendTelegramWithButton(const String& text) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient https;
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/sendMessage";
  if (https.begin(secureClient, url)) {
    https.addHeader("Content-Type", "application/json");
    // ปุ่ม inline: กดแล้วส่ง callback_data="stop"
    String keyboard = "{\"inline_keyboard\":[[{\"text\":\"✅ เอาผ้าออกแล้ว\",\"callback_data\":\"stop\"}]]}";
    String payload  = "{\"chat_id\":\"" + String(CHAT_ID) + "\",\"text\":\"" + text + "\",\"reply_markup\":" + keyboard + "}";
    https.POST(payload);
    https.end();
  }
}

// ดึงอีเวนต์จาก Telegram เพื่อเช็คว่ามีการกดปุ่ม "stop" หรือยัง
void pollTelegramUpdates() {
  if (WiFi.status() != WL_CONNECTED) return;

  // จำกัดความถี่การดึง
  unsigned long now = millis();
  if (now - lastPollMs < POLL_UPDATES_MS) return;
  lastPollMs = now;

  HTTPClient https;
  // ใช้ offset เพื่อไม่ให้ข้อความเดิมถูกประมวลผลซ้ำ
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/getUpdates?timeout=0";
  if (updatesOffset > 0) {
    url += "&offset=" + String(updatesOffset);
  }

  if (https.begin(secureClient, url)) {
    int code = https.GET();
    if (code == HTTP_CODE_OK) {
      String body = https.getString();

      // หา update_id ล่าสุดเพื่อตั้ง offset
      // (พาร์สแบบง่ายด้วย string find; สำหรับงานจริงควรใช้ JSON parser)
      // อัปเดต offset: ค้นหา "update_id":xxxx จากท้ายสุด
      int lastIdx = body.lastIndexOf("\"update_id\":");
      if (lastIdx != -1) {
        int start = lastIdx + String("\"update_id\":").length();
        // อ่านเลขจนกว่าจะเจอ non-digit
        unsigned long id = 0;
        while (start < (int)body.length() && isDigit(body[start])) {
          id = id * 10 + (body[start] - '0');
          start++;
        }
        if (id > 0) updatesOffset = id + 1;
      }

      // ตรวจว่ามี callback_data = "stop" ไหม
      if (body.indexOf("\"callback_data\":\"stop\"") != -1) {
        muted = true;
        sendTelegram("หยุดการส่งแจ้งเตือน");
      }
    }
    https.end();
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(WATER_SENSOR_PIN, INPUT_PULLUP); // XKC-Y25: LOW=เจอน้ำ, HIGH=ไม่เจอน้ำ
  pinMode(VIB_SENSOR_PIN,   INPUT);        // SW-420: HIGH=สั่น, LOW=เงียบ (ถ้าลอย -> INPUT_PULLUP)

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print('.');
    delay(500);
  }
  Serial.printf("\nWiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());

  secureClient.setInsecure(); // ง่ายสุด

  sendTelegram("เริ่มการทำงาน");
}

void loop() {
  // อ่านสถานะเซนเซอร์
  int waterRaw = digitalRead(WATER_SENSOR_PIN); // LOW=เจอน้ำ, HIGH=ไม่เจอน้ำ
  int vibRaw   = digitalRead(VIB_SENSOR_PIN);   // HIGH=สั่น,  LOW=เงียบ

  bool noWater = (waterRaw == HIGH);
  bool noVib   = (vibRaw   == LOW);

  Serial.printf("[WATER raw=%d -> %s]  [VIB raw=%d -> %s]\n",
                waterRaw, noWater ? "ไม่เจอน้ำ" : "เจอน้ำ",
                vibRaw,   noVib   ? "เงียบ"     : "สั่น");

  unsigned long now = millis();

  if (noWater && noVib) {
    if (!trackingCondition) {
      // เพิ่งเข้าเงื่อนไข → เริ่มจับเวลา & เคลียร์การแจ้งเตือนในรอบนี้
      trackingCondition = true;
      conditionStartMs  = now;
      lastNotifyMs      = 0;
      muted             = false;  // เริ่มรอบใหม่ ปลด mute
      Serial.println("Condition started: no water + no vibration (tracking 30s)");
    } else {
      // อยู่ในเงื่อนไขต่อเนื่อง
      if ((now - conditionStartMs) >= STABLE_MS) {
        // ครบ 30s แล้ว
        if (!muted) {
          // แจ้งครั้งแรก หรือแจ้งซ้ำทุก 5 นาที
          if (lastNotifyMs == 0 || (now - lastNotifyMs) >= REPEAT_MS) {
            sendTelegramWithButton("✅ ผ้าซักเสร็จแล้ว \nกดปุ่มนี้เมื่อเอาผ้าออกแล้วเพื่อหยุดแจ้งเตือน");
            lastNotifyMs = now;
          }
        }
      }
    }
  } else {
    // เงื่อนไขแตก (มีน้ำกลับมา หรือมีการสั่น)
    if (trackingCondition) {
      Serial.println("ตรวจพบการสั่น หรือมีการเติมน้ำในถัง");
    }
    trackingCondition = false;
    conditionStartMs  = 0;
    lastNotifyMs      = 0;
    muted             = false; // เตรียมพร้อมสำหรับรอบใหม่
  }

  // เช็กการกดปุ่มจาก Telegram เพื่อตั้ง mute
  pollTelegramUpdates();

  delay(500);
}
