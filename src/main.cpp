#include <Arduino.h>
#include <TFT_eSPI.h>
#include <DHT.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>

// ========== WiFi ==========
const char* WIFI_SSID     = "TP-LINK_E14B";
const char* WIFI_PASSWORD = "2kbctn2m";

// ========== 巴法云 (Bemfa) ==========
const char* BEMFA_SERVER   = "bemfa.com";
const int   BEMFA_PORT     = 9501;
const char* BEMFA_KEY      = "ff2aba976550475080b2c399fad134a0"; // UID/私钥
const char* BEMFA_TOPIC    = "temp004";                     // 主题名

// ========== 硬件引脚 ==========
#define DHTPIN     12    // D6
#define DHTTYPE    DHT11
#define FAN_PIN    4     // D2（风扇控制引脚，可改）

// ========== 全局对象 ==========
DHT dht(DHTPIN, DHTTYPE);
TFT_eSPI tft = TFT_eSPI();

// 巴法云客户端（明文 TCP，端口 9501，不走 TLS）
WiFiClient bemfaClient;
PubSubClient bemfa(bemfaClient);

// ========== 时间控制 ==========
unsigned long lastReadTime  = 0;
bool fanState = false;

// ========== 启动旋转 Logo 动画 ==========
void showBootAnimation() {
  TFT_eSprite spr = TFT_eSprite(&tft);
  spr.createSprite(80, 80);

  const int cx = 40, cy = 40;
  const int r  = 30;

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawCentreString("WIFI", 120, 170, 2);

  for (int round = 0; round < 3; round++) {
    for (int angle = 0; angle < 360; angle += 6) {
      spr.fillSprite(TFT_BLACK);

      for (int i = 0; i < 5; i++) {
        int a = (angle - i * 12 + 360) % 360;
        float rad = a * PI / 180.0f;
        int x = cx + (int)(r * cos(rad));
        int y = cy + (int)(r * sin(rad));
        uint16_t color = (i == 0) ? TFT_CYAN
                       : tft.color565(0, (uint8_t)(200 - i * 40), (uint8_t)(200 - i * 40));
        spr.drawLine(cx, cy, x, y, color);
      }

      spr.fillCircle(cx, cy, 5, TFT_WHITE);
      spr.pushSprite(80, 60);
      delay(15);
    }
  }

  spr.deleteSprite();
  tft.fillRect(0, 160, 240, 30, TFT_BLACK);
}

// ========== 画粗线辅助函数 ==========
void drawThickLine(int x1, int y1, int x2, int y2, int width, uint16_t color) {
  float dx = x2 - x1;
  float dy = y2 - y1;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.01f) return;
  float nx = -dy / len;
  float ny =  dx / len;
  for (int i = -width / 2; i <= width / 2; i++) {
    tft.drawLine(x1 + (int)(nx * i), y1 + (int)(ny * i),
                 x2 + (int)(nx * i), y2 + (int)(ny * i), color);
  }
}

// ========== WiFi 连接动画 ==========
void drawWiFiSignal(TFT_eSprite &s, int level) {
  s.fillSprite(TFT_BLACK);
  const int cx = 30, cy = 48;
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
    int level = attempts % 3;
    drawWiFiSignal(s, level);
    s.pushSprite(90, 60, TFT_BLACK);
    delay(500);
    Serial.print(".");
    attempts++;
  }

  s.deleteSprite();
  tft.fillRect(0, 55, 240, 110, TFT_BLACK);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
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
    drawThickLine(85, 85, 155, 115, 4, TFT_RED);
    drawThickLine(85, 115, 155, 85, 4, TFT_RED);
    tft.setTextSize(2);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawCentreString("WiFi FAIL", 120, 140, 2);
    ESP.restart();
  }
}

// ========== 巴法云连接 ==========
void connectBemfa() {
  if (bemfa.connected()) return;

  Serial.printf("Bemfa connecting to %s:%d ...\n", BEMFA_SERVER, BEMFA_PORT);
  Serial.printf("  clientId=%s\n", BEMFA_KEY);

  bool ok = bemfa.connect(BEMFA_KEY);
  if (ok) {
    Serial.println("Bemfa connected!");
    bemfa.subscribe(BEMFA_TOPIC);
    Serial.printf("Bemfa subscribed: %s\n", BEMFA_TOPIC);
  } else {
    Serial.printf("Bemfa failed, rc=%d\n", bemfa.state());
  }
}

// ========== 巴法云上报 ==========
void publishBemfa(float temp, float humi) {
  String msg = "#" + String((int)temp) + "#" + String((int)humi) + "#" + (fanState ? "on" : "off");

  bool ok = bemfa.publish(BEMFA_TOPIC, msg.c_str());
  Serial.printf("Bemfa Pub: %s -> %s\n", msg.c_str(), ok ? "OK" : "FAIL");
}

// ========== 屏幕显示数据 ==========
void drawScreen(float temp, float humi) {
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(10, 30);
  tft.printf("Temp: %.1f C   ", temp);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(10, 60);
  tft.printf("Humi: %.1f %%   ", humi);

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

  connectWiFi();
  delay(500);

  // 巴法云
  bemfa.setServer(BEMFA_SERVER, BEMFA_PORT);
  bemfa.setKeepAlive(60);
  connectBemfa();

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

  // 巴法云断线重连
  if (!bemfa.connected()) {
    connectBemfa();
  }
  bemfa.loop();

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

    // 巴法云上报（5秒间隔）
    static unsigned long lastBemfaTime = 0;
    if (millis() - lastBemfaTime > 5000) {
      lastBemfaTime = millis();
      if (bemfa.connected()) {
        publishBemfa(t, h);
      }
    }
  }
}
