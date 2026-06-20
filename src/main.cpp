#include <Arduino.h>
#include <TFT_eSPI.h>
#include <DHT.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>

// ========== WiFi ==========
const char* WIFI_SSID     = "TP-LINK_E14B";
const char* WIFI_PASSWORD = "2kbctn2m";

// ========== MQTT (EMQX Cloud) ==========
const char* MQTT_BROKER   = "ac813c9c.ala.cn-shenzhen.emqxsl.cn";
const int   MQTT_PORT     = 8883;
const char* MQTT_USER     = "esp8266_device";
const char* MQTT_PASS     = "device123";
const char* MQTT_CLIENT_ID = "esp8266_001";

// ========== MQTT Topics ==========
const char* TOPIC_DATA   = "device/esp8266/data";    // 设备 → 网页（传感器数据）
const char* TOPIC_FAN    = "device/esp8266/fan";     // 网页 → 设备（风扇控制）
const char* TOPIC_STATUS = "device/esp8266/status";  // 设备在线状态

// ========== 硬件引脚 ==========
#define DHTPIN     12    // D6
#define DHTTYPE    DHT11
#define FAN_PIN    4     // D2（风扇控制引脚，可改）

// ========== 全局对象 ==========
DHT dht(DHTPIN, DHTTYPE);
TFT_eSPI tft = TFT_eSPI();
BearSSL::WiFiClientSecure secureClient;
PubSubClient mqtt(secureClient);

// ========== 时间控制 ==========
unsigned long lastReadTime  = 0;
unsigned long lastMqttTime  = 0;
bool fanState = false;

// ========== 启动旋转 Logo 动画 ==========
void showBootAnimation() {
  // 80x80 sprite，16-bit = 12.8KB，ESP8266 可承受
  TFT_eSprite spr = TFT_eSprite(&tft);
  spr.createSprite(80, 80);

  const int cx = 40, cy = 40;
  const int r  = 30;

  // 下方显示启动文字
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawCentreString("WIFI", 120, 170, 2);

  // 旋转 3 圈（360°/6° × 3 = 180 帧）
  for (int round = 0; round < 3; round++) {
    for (int angle = 0; angle < 360; angle += 6) {
      spr.fillSprite(TFT_BLACK);

      // 5 条线形成拖尾效果，亮度递减
      for (int i = 0; i < 5; i++) {
        int a = (angle - i * 12 + 360) % 360;
        float rad = a * PI / 180.0f;
        int x = cx + (int)(r * cos(rad));
        int y = cy + (int)(r * sin(rad));
        uint16_t color = (i == 0) ? TFT_CYAN
                       : tft.color565(0, (uint8_t)(200 - i * 40), (uint8_t)(200 - i * 40));
        spr.drawLine(cx, cy, x, y, color);
      }

      // 中心圆点
      spr.fillCircle(cx, cy, 5, TFT_WHITE);

      // 推送到屏幕中央偏上（x=80, y=60 → 左上角坐标）
      spr.pushSprite(80, 60);
      delay(15);
    }
  }

  spr.deleteSprite();

  // 清除文字残留
  tft.fillRect(0, 160, 240, 30, TFT_BLACK);
}

// ========== 画粗线辅助函数 ==========
// 沿垂直于线段的方向绘制多条平行线，模拟粗线效果
void drawThickLine(int x1, int y1, int x2, int y2, int width, uint16_t color) {
  float dx = x2 - x1;
  float dy = y2 - y1;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.01f) return;
  // 单位法向量
  float nx = -dy / len;
  float ny =  dx / len;
  for (int i = -width / 2; i <= width / 2; i++) {
    tft.drawLine(x1 + (int)(nx * i), y1 + (int)(ny * i),
                 x2 + (int)(nx * i), y2 + (int)(ny * i), color);
  }
}

// ========== WiFi 连接动画 ==========
// 在 sprite 内画 WiFi 信号图标，level = 0~3 层弧
void drawWiFiSignal(TFT_eSprite &s, int level) {
  s.fillSprite(TFT_BLACK);
  const int cx = 30, cy = 48;
  // 底部信号点（始终显示，level=0 时只看到它）
  s.fillCircle(cx, cy, 3, TFT_CYAN);
  if (level >= 1) {
    for (int a = 225; a <= 315; a++) {
      float rad = a * PI / 180.0f;
      int x = cx + (int)(14 * cos(rad));
      int y = cy + (int)(14 * sin(rad));
      s.drawLine(cx, cy, x, y, TFT_CYAN);
    }
    s.fillCircle(cx, cy, 10, TFT_BLACK);
  }
  if (level >= 2) {
    for (int a = 225; a <= 315; a++) {
      float rad = a * PI / 180.0f;
      int x = cx + (int)(22 * cos(rad));
      int y = cy + (int)(22 * sin(rad));
      s.drawLine(cx, cy, x, y, TFT_CYAN);
    }
    s.fillCircle(cx, cy, 18, TFT_BLACK);
  }
  if (level >= 3) {
    for (int a = 225; a <= 315; a++) {
      float rad = a * PI / 180.0f;
      int x = cx + (int)(30 * cos(rad));
      int y = cy + (int)(30 * sin(rad));
      s.drawLine(cx, cy, x, y, TFT_CYAN);
    }
    s.fillCircle(cx, cy, 26, TFT_BLACK);
  }
}

void connectWiFi() {
  tft.fillScreen(TFT_BLACK);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("Connecting WiFi", 120, 130, 2);

  TFT_eSprite s = TFT_eSprite(&tft);
  s.createSprite(60, 60);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    int level = attempts % 3;   // 0→1→2 循环，搜索感更强
    drawWiFiSignal(s, level);
    s.pushSprite(90, 60, TFT_BLACK);
    delay(500);
    Serial.print(".");
    attempts++;
  }

  s.deleteSprite();
  // 清除图标 + 标题区域
  tft.fillRect(0, 55, 240, 110, TFT_BLACK);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    // 绿色对勾（两条线段组成 V 形）
    drawThickLine(75, 95, 105, 120, 4, TFT_GREEN);
    drawThickLine(105, 120, 160, 75, 4, TFT_GREEN);
    tft.setTextSize(2);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawCentreString("WiFi OK", 120, 140, 2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString(WiFi.localIP().toString().c_str(), 120, 170, 2);
    delay(1500);
    tft.fillScreen(TFT_BLACK);
  } else {
    // 红色叉号
    drawThickLine(85, 85, 155, 115, 4, TFT_RED);
    drawThickLine(85, 115, 155, 85, 4, TFT_RED);
    tft.setTextSize(2);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawCentreString("WiFi FAIL", 120, 140, 2);
    ESP.restart();
  }
}

// ========== MQTT 消息回调（接收风扇控制指令） ==========
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  Serial.printf("MQTT Recv [%s]: %s\n", topic, msg.c_str());

  if (String(topic) == TOPIC_FAN) {
    fanState = (msg == "ON" || msg == "1");
    digitalWrite(FAN_PIN, fanState ? HIGH : LOW);
    Serial.printf("Fan -> %s\n", fanState ? "ON" : "OFF");
  }
}

// ========== MQTT 连接 ==========
void connectMQTT() {
  if (mqtt.connected()) return;

  tft.setCursor(10, 160);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.print("MQTT connecting...");

  if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
    Serial.println("MQTT connected!");
    tft.setCursor(10, 160);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.print("MQTT OK!         ");

    // 发布在线状态
    mqtt.publish(TOPIC_STATUS, "online", true);

    // 订阅风扇控制主题
    mqtt.subscribe(TOPIC_FAN);
  } else {
    Serial.printf("MQTT failed, rc=%d\n", mqtt.state());
    tft.setCursor(10, 160);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.printf("MQTT FAIL rc=%d ", mqtt.state());
  }
}

// ========== 发布传感器数据 ==========
void publishData(float temp, float humi) {
  StaticJsonDocument<256> doc;
  doc["temp"]  = temp;
  doc["humi"]  = humi;
  doc["fan"]   = fanState ? "ON" : "OFF";
  doc["ts"]    = millis();

  char buf[256];
  serializeJson(doc, buf);

  mqtt.publish(TOPIC_DATA, buf);
  Serial.printf("MQTT Pub: %s\n", buf);
}

// ========== 屏幕显示数据 ==========
void drawScreen(float temp, float humi) {
  // 温度
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(10, 30);
  tft.printf("Temp: %.1f C   ", temp);

  // 湿度
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(10, 60);
  tft.printf("Humi: %.1f %%   ", humi);

  // 风扇状态
  tft.setTextColor(fanState ? TFT_RED : TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 90);
  tft.printf("Fan:  %s   ", fanState ? "ON " : "OFF");
}

// ========== Setup ==========
void setup() {
  Serial.begin(115200);
  Serial.println("System Starting...");

  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, LOW);

  dht.begin();

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  showBootAnimation();

  // TLS 跳过证书校验（适合个人项目，生产环境应使用证书）
  secureClient.setInsecure();

  connectWiFi();
  delay(500);

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  connectMQTT();

  delay(1000);
  tft.fillScreen(TFT_BLACK);
}

// ========== Loop ==========
void loop() {
  // WiFi 断线重连
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    connectWiFi();
  }

  // MQTT 断线重连
  if (!mqtt.connected()) {
    connectMQTT();
    delay(100);
  }
  mqtt.loop();

  // 传感器采样（2秒间隔）
  if (millis() - lastReadTime > 2000) {
    lastReadTime = millis();

    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (isnan(h) || isnan(t)) {
      Serial.println("DHT read error!");
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.setCursor(10, 30);
      tft.print("DHT Error!   ");
      return;
    }

    Serial.printf("Temp: %.1f C  Humi: %.1f %%\n", t, h);
    drawScreen(t, h);

    // MQTT 上报（5秒间隔）
    if (millis() - lastMqttTime > 5000) {
      lastMqttTime = millis();
      publishData(t, h);
    }
  }
}
