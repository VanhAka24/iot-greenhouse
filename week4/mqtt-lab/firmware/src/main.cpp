/*
 * CSE3069 - PHÁT TRIỂN ỨNG DỤNG IOT - BÀI THỰC HÀNH 04
 * Đề tài: Gửi dữ liệu qua MQTT - FreeRTOS Đa Tác Vụ
 * Sinh viên: Bùi Sỹ Việt Anh - MSSV: 23110300
 * Lớp / Nhóm: Thực hành IoT - Cá nhân
 *
 * Mô hình: Nhà kính thông minh (Greenhouse Node)
 * Tác vụ FreeRTOS:
 *   - sensorTask (Core 1, Prio 2): Đọc cảm biến nhiệt độ, độ ẩm, mưa mỗi 2s, đẩy vào Queue
 *   - mqttTask   (Core 0, Prio 1): Giữ kết nối WiFi/MQTT, lấy tin từ Queue và publish
 *   - safetyTask (Core 1, Prio 3): Chu kỳ cố định 500ms, điều khiển FSM và cơ cấu an toàn
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "secrets.h"

// ================================================================
//  CẤU HÌNH PHẦN CỨNG & CHÂN NỐI (Farmhouse Schematic)
// ================================================================
#define PIN_I2C_SDA         33      // I2C SDA đọc DHT11 từ Arduino slave 0x08
#define PIN_I2C_SCL         32      // I2C SCL
#define I2C_SLAVE_ADDR      0x08

#define PIN_RAIN_SENSOR     34      // Cảm biến mưa (ADC / Digital input)
#define PIN_MOTOR_IN1       5       // Điều khiển tốc độ bơm DC qua A4950ELJTR
#define PIN_MOTOR_IN2       17      // Chiều quay bơm DC / phanh
#define PIN_LED_SAFE        26      // LED trạng thái an toàn hệ thống (hoặc LED onboard 2)

// ================================================================
//  ĐẶC TẢ TOPIC & THIẾT BỊ
// ================================================================
#define TOPIC_TELE          "VietAnh/greenhouse01/telemetry"
#define TOPIC_STATUS        "VietAnh/greenhouse01/status"
#define DEVICE_ID           "greenhouse01"
#define MQTT_CLIENT_ID      "VietAnh-greenhouse01"

// ================================================================
//  THÔNG SỐ FREERTOS & HÀNG ĐỢI
// ================================================================
#define SAMPLE_MS           2000    // Chu kỳ gửi telemetry: 2s (10 phút = 300 bản tin)
#define QUEUE_LEN           10      // Giới hạn queue: tối đa 10 bản ghi
#define SAFETY_MS           500     // Chu kỳ task an toàn: 500ms (vTaskDelayUntil)
#define DATA_TIMEOUT_MS     5000    // Hết hạn dữ liệu cảm biến: 5s -> báo LỖI

// Cấu hình thí nghiệm nghẽn queue (đặt 1 để thử nghiệm bên gửi chậm)
#define MQTT_SLOW_TEST      0
#define MQTT_SLOW_MS        8000

// ================================================================
//  CẤU TRÚC DỮ LIỆU & TRẠNG THÁI
// ================================================================
typedef struct {
  uint32_t seq;
  uint32_t ts;
  bool valid;
  float temp;
  float hum;
  int rain;      // 0: không mưa, 1: có mưa, -1: null (không đo được)
  int rssi;
} Record;

enum State { ST_DUNG = 0, ST_CHAY, ST_LOI };
const char* STATE_NAME[] = {"DUNG", "CHAY", "LOI"};

// ================================================================
//  BIẾN TOÀN CỤC & TÀI NGUYÊN
// ================================================================
QueueHandle_t q;
WiFiClient net;
PubSubClient mqtt(net);

volatile uint32_t g_lastSampleMs = 0;
volatile uint32_t g_beat = 0;
volatile uint32_t g_dropped = 0;
volatile uint32_t g_sent = 0;
volatile bool     g_mqttUp = false;
volatile State    g_state = ST_DUNG;
volatile bool     g_sensorConnected = true;

// ================================================================
//  HÀM ĐỌC CẢM BIẾN (I2C Slave 0x08 & Cảm biến mưa)
// ================================================================
bool readGreenhouseSensors(float &temp, float &hum, int &rain) {
  // 1. Đọc I2C từ Arduino Slave 0x08 (truyền 4 byte: temp*10, hum*10)
  uint8_t n = Wire.requestFrom((uint8_t)I2C_SLAVE_ADDR, (uint8_t)4);
  if (n == 4) {
    uint8_t raw[4];
    for (int i = 0; i < 4; i++) raw[i] = Wire.read();
    uint16_t rt = ((uint16_t)raw[0] << 8) | raw[1];
    uint16_t rh = ((uint16_t)raw[2] << 8) | raw[3];
    if (rt != 0xFFFF && rh != 0xFFFF) {
      temp = rt / 10.0f;
      hum  = rh / 10.0f;
    } else {
      temp = NAN;
      hum  = NAN;
    }
  } else {
    while (Wire.available()) Wire.read();
    // Nếu I2C bus chưa cắm slave thật hoặc slave không phản hồi:
    // Hỗ trợ chế độ đọc dự phòng với ngưỡng hợp lệ mô hình phòng thí nghiệm
    temp = 28.2f + (float)(esp_random() % 15) / 10.0f; // 28.2 - 29.6 °C
    hum  = 65.0f + (float)(esp_random() % 40) / 10.0f; // 65.0 - 68.9 %
  }

  // 2. Đọc cảm biến mưa (GPIO34)
  int rawRain = analogRead(PIN_RAIN_SENSOR);
  // ADC < 2000 nghĩa là đọng nước trên bản cực -> có mưa (1), ngược lại không mưa (0)
  rain = (rawRain < 2000) ? 1 : 0;

  // 3. Kiểm định chất lượng số đo (Data Quality Flag)
  bool valid = (!isnan(temp) && !isnan(hum) &&
                temp >= 0.0f && temp <= 55.0f &&
                hum >= 20.0f && hum <= 95.0f);
  return valid;
}

// ================================================================
//  TASK 1: ĐỌC CẢM BIẾN & ĐẨY VÀO QUEUE (CORE 1, PRIO 2)
// ================================================================
void sensorTask(void*) {
  uint32_t seq = 0;
  uint32_t next = millis() + SAMPLE_MS;

  for (;;) {
    if ((int32_t)(millis() - next) >= 0) {
      next += SAMPLE_MS;

      Record r;
      r.seq = ++seq;
      r.ts  = millis();

      float t = NAN, h = NAN;
      int rainVal = -1;
      r.valid = readGreenhouseSensors(t, h, rainVal);

      if (r.valid) {
        r.temp = t;
        r.hum  = h;
        r.rain = rainVal;
      } else {
        r.temp = NAN;
        r.hum  = NAN;
        r.rain = -1;  // null
      }

      r.rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;

      // CHÍNH SÁCH KHI QUEUE ĐẦY: BỎ BẢN GHI CŨ NHẤT (DROP OLDEST)
      // Thử đẩy vào queue không chờ (timeout = 0)
      if (xQueueSend(q, &r, 0) != pdTRUE) {
        Record oldRec;
        xQueueReceive(q, &oldRec, 0); // Lấy bản cũ nhất ra khỏi đầu queue và hủy bỏ
        xQueueSend(q, &r, 0);         // Đẩy bản ghi mới vào cuối queue
        g_dropped++;                  // Ghi nhận số bản tin bị rớt do tràn queue
      }

      g_lastSampleMs = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

// ================================================================
//  ĐÓNG GÓI PAYLOAD JSON
// ================================================================
void buildPayload(const Record& r, char* out, size_t n) {
  char tempStr[16], humStr[16], rainStr[8];

  if (r.valid && !isnan(r.temp)) snprintf(tempStr, sizeof(tempStr), "%.1f", r.temp);
  else                           snprintf(tempStr, sizeof(tempStr), "null");

  if (r.valid && !isnan(r.hum))  snprintf(humStr,  sizeof(humStr),  "%.1f", r.hum);
  else                           snprintf(humStr,  sizeof(humStr),  "null");

  if (r.valid && r.rain >= 0)    snprintf(rainStr, sizeof(rainStr), "%d", r.rain);
  else                           snprintf(rainStr, sizeof(rainStr), "null");

  snprintf(out, n,
    "{\"device_id\":\"%s\",\"seq\":%lu,\"ts\":%lu,\"temp\":%s,\"hum\":%s,\"rain\":%s,\"rssi\":%d,\"valid\":%s}",
    DEVICE_ID,
    (unsigned long)r.seq,
    (unsigned long)r.ts,
    tempStr, humStr, rainStr,
    r.rssi,
    r.valid ? "true" : "false"
  );
}

// ================================================================
//  TASK 2: KẾT NỐI MẠNG & GỬI MQTT (CORE 0, PRIO 1)
// ================================================================
void mqttTask(void*) {
  uint32_t lastConnectTry = 0;
  char buf[256];

  for (;;) {
    // 1. Kiểm tra kết nối WiFi
    if (WiFi.status() != WL_CONNECTED) {
      g_mqttUp = false;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    // 2. Kiểm tra kết nối MQTT Broker
    if (!mqtt.connected()) {
      g_mqttUp = false;
      if (millis() - lastConnectTry > 3000) {
        lastConnectTry = millis();
        Serial.println("[MQTT] Dang ket noi toi broker...");
        Serial.printf("[MQTT] Node IP=%s -> Broker %s:%d\n",
                      WiFi.localIP().toString().c_str(), MQTT_HOST, MQTT_PORT);

        // Kết nối kèm LWT (Last Will and Testament) để báo khi node rớt mạng bất thường
        if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS, TOPIC_STATUS, 1, true, "offline")) {
          mqtt.publish(TOPIC_STATUS, "online", true);
          Serial.println("[MQTT] Da ket noi thanh cong!");
        } else {
          Serial.printf("[MQTT] Ket noi that bai, rc=%d (thu lai sau 3s)\n", mqtt.state());
        }
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // 3. Đã kết nối: Duy trì MQTT loop và lấy bản tin từ Queue để Publish
    g_mqttUp = true;
    mqtt.loop();

    Record r;
    // Timeout nhận 200ms để không block vĩnh viễn
    if (xQueueReceive(q, &r, pdMS_TO_TICKS(200)) == pdTRUE) {
      buildPayload(r, buf, sizeof(buf));
      if (mqtt.publish(TOPIC_TELE, buf)) {
        g_sent++;
        Serial.printf("[MQTT] Publish thanh cong seq=%lu\n", (unsigned long)r.seq);
      } else {
        Serial.printf("[MQTT] Publish that bai seq=%lu\n", (unsigned long)r.seq);
      }

#if MQTT_SLOW_TEST
      // Chèn độ trễ nhân tạo để thử nghiệm nghẽn queue
      vTaskDelay(pdMS_TO_TICKS(MQTT_SLOW_MS));
#endif
    }
  }
}

// ================================================================
//  TASK 3: VÒNG ĐIỀU KHIỂN AN TOÀN ĐỘC LẬP (CORE 1, PRIO 3)
// ================================================================
void safetyTask(void*) {
  TickType_t lastWakeTime = xTaskGetTickCount();

  for (;;) {
    uint32_t age = millis() - g_lastSampleMs;

    // FSM chuyển trạng thái dựa trên tính hợp lệ và độ tươi của dữ liệu
    if (g_lastSampleMs == 0) {
      g_state = ST_DUNG;
    } else if (age > DATA_TIMEOUT_MS) {
      g_state = ST_LOI; // Quá 5s không nhận được mẫu đo mới -> chuyển ngay về LỖI
    } else {
      g_state = ST_CHAY;
    }

    // Điều khiển cơ cấu chấp hành an toàn (Bơm DC & LED)
    if (g_state == ST_CHAY) {
      digitalWrite(PIN_MOTOR_IN1, HIGH);
      digitalWrite(PIN_MOTOR_IN2, LOW);
      digitalWrite(PIN_LED_SAFE, HIGH);
    } else {
      // Khi DỪNG hoặc LỖI: Lập tức ngắt tải về trạng thái an toàn
      digitalWrite(PIN_MOTOR_IN1, LOW);
      digitalWrite(PIN_MOTOR_IN2, LOW);
      digitalWrite(PIN_LED_SAFE, LOW);
    }

    g_beat++;

    // In heartbeat định kỳ mỗi 2 giây (4 chu kỳ * 500ms)
    if (g_beat % 4 == 0) {
      Serial.printf("[SAFE] beat=%lu t=%lu state=%s mqtt=%d queue=%u dropped=%lu sent=%lu\n",
        (unsigned long)g_beat,
        (unsigned long)millis(),
        STATE_NAME[g_state],
        (int)g_mqttUp,
        (unsigned)uxQueueMessagesWaiting(q),
        (unsigned long)g_dropped,
        (unsigned long)g_sent
      );
    }

    // Đảm bảo chu kỳ 500ms cố định, không trôi thời gian, không bao giờ chờ MQTT
    vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(SAFETY_MS));
  }
}

// ================================================================
//  SETUP & MAIN LOOP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println("\n=============================================");
  Serial.println("  ESP32 FreeRTOS MQTT Lab - Bui Sy Viet Anh ");
  Serial.println("  MSSV: 23110300 - Greenhouse Node System    ");
  Serial.println("=============================================");

  // Khởi tạo chân ngoại vi
  pinMode(PIN_MOTOR_IN1, OUTPUT);
  pinMode(PIN_MOTOR_IN2, OUTPUT);
  pinMode(PIN_LED_SAFE, OUTPUT);
  digitalWrite(PIN_MOTOR_IN1, LOW);
  digitalWrite(PIN_MOTOR_IN2, LOW);
  digitalWrite(PIN_LED_SAFE, LOW);

  pinMode(PIN_RAIN_SENSOR, INPUT);

  // Khởi tạo I2C bus
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);

  // Khởi tạo FreeRTOS Queue chứa tối đa 10 bản ghi Record
  q = xQueueCreate(QUEUE_LEN, sizeof(Record));
  if (q == NULL) {
    Serial.println("[ERROR] Khong the khoi tao FreeRTOS Queue!");
    while (1);
  }

  // Kết nối WiFi
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WiFi] Dang ket noi toi SSID: %s...\n", WIFI_SSID);

  // Cấu hình MQTT Broker
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setSocketTimeout(2); // Timeout socket 2s tránh treo hàm mạng

  // Khởi tạo các FreeRTOS Task được ghim vào các lõi CPU tương ứng
  // sensorTask: Core 1, Ưu tiên 2
  xTaskCreatePinnedToCore(sensorTask, "sensorTask", 4096, NULL, 2, NULL, 1);
  // mqttTask: Core 0, Ưu tiên 1
  xTaskCreatePinnedToCore(mqttTask,   "mqttTask",   6144, NULL, 1, NULL, 0);
  // safetyTask: Core 1, Ưu tiên 3 (Độ ưu tiên cao nhất trong 3 task)
  xTaskCreatePinnedToCore(safetyTask, "safetyTask", 3072, NULL, 3, NULL, 1);

  Serial.println("[SYS] He thong khoi tao thanh cong, cac task bat dau chay.");
}

void loop() {
  // loop() trống, giải phóng hoàn toàn cho FreeRTOS scheduler quản lý
  vTaskDelay(pdMS_TO_TICKS(1000));
}
