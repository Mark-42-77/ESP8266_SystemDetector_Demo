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

// ========== WiFi 连接 ==========
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(10, 100);
  tft.print("Connecting WiFi...");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    tft.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(10, 100);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.printf("WiFi OK");
    tft.setCursor(10, 130);
    tft.printf("IP:%s", WiFi.localIP().toString().c_str());
  } else {
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(10, 100);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.print("WiFi FAIL!");
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
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("System Init...", 10, 10);

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
