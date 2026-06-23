#include <Arduino.h>
#include <TFT_eSPI.h>
#include <DHT.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <time.h>
#include <math.h>

// ========== WiFi ==========
const char* WIFI_SSID     = "Friday";
const char* WIFI_PASSWORD = "0987654321";

// ========== 巴法云 (Bemfa) ==========
const char* BEMFA_SERVER   = "bemfa.com";
const int   BEMFA_PORT     = 9501;
const char* BEMFA_KEY      = "ff2aba976550475080b2c399fad134a0"; // UID/私钥
const char* TOPIC_TEMP     = "temp004";                     // 温湿度主题（小爱查询）
const char* TOPIC_DATA     = "data004";                     // 光照+设备状态主题（网页用）
const char* TOPIC_FAN1     = "fan003";                      // 风扇主题
const char* TOPIC_FAN2     = "humi006";                    // 加湿器主题
const char* TOPIC_MUTE     = "mute006";                     // 静音控制主题

// ========== 硬件引脚 ==========
#define DHTPIN     12    // D6
#define DHTTYPE    DHT11
#define FAN1_PIN   4     // D2（风扇）
#define FAN2_PIN   5     // D1（加湿器）
#define LIGHT_AO   A0    // A0（光敏模拟输出，读取光照强度）
#define SR602_PIN  15    // D8（人体存在传感器，HIGH=有人）
#define BUZZER_PIN 16    // D0（蜂鸣器）

// ========== 全局对象 ==========
DHT dht(DHTPIN, DHTTYPE);
TFT_eSPI tft = TFT_eSPI();

// 巴法云客户端（明文 TCP，端口 9501，不走 TLS）
WiFiClient bemfaClient;
PubSubClient bemfa(bemfaClient);

// ========== 时间控制 ==========
unsigned long lastReadTime  = 0;
bool fan1State = false;  // 风扇状态
bool fan2State = false;  // 加湿器状态
bool screenOn  = true;   // 屏幕状态
bool presenceState = false;  // SR602 人体存在状态
int  lightPct  = 0;      // 光照百分比（0-100）
int  lightLux  = 0;      // 光照估算值（lux）

// ========== 模拟量屏幕熄屏阈值 ==========
const int LIGHT_OFF_THRESHOLD = 50;   // 低于50%熄屏（手遮≈45%，真暗<5%）
const int LIGHT_ON_THRESHOLD  = 65;   // 高于65%亮屏（迟滞15%）

// ========== 蜂鸣器报警 ==========
float TEMP_ALARM = 29.0;   // 温度报警阈值（℃）
float HUMI_ALARM = 96.0;   // 湿度报警阈值（%）
const int  BEEP_MS  = 1000; // 蜂鸣时长（ms）
unsigned long buzzerUntil = 0;  // 蜂鸣结束时刻（0=停）
bool tempAlerted = false;  // 防止反复触发
bool humiAlerted = false;
bool darkAlerted = false;
bool muted = false;        // 静音状态

void beep() {
  if (muted) {
    Serial.println("Buzzer muted, skip");
    return;
  }
  digitalWrite(BUZZER_PIN, LOW);
  buzzerUntil = millis() + BEEP_MS;
  Serial.println("Buzzer ON");
}

void buzzerLoop() {
  if (buzzerUntil > 0 && millis() >= buzzerUntil) {
    digitalWrite(BUZZER_PIN, HIGH);
    buzzerUntil = 0;
    Serial.println("Buzzer OFF");
  }
}

// NTP 配置
const char* NTP_SERVER1 = "ntp.aliyun.com";
const char* NTP_SERVER2 = "pool.ntp.org";
const long GMT_OFFSET = 8 * 3600;  // 中国时区 UTC+8
const int DST_OFFSET = 0;          // 夏令时偏移

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

// ========== 巴法云回调（接收控制指令） ==========
void bemfaCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  Serial.printf("Bemfa Recv [%s]: %s\n", topic, msg.c_str());

  // 风扇1控制（fan003 主题）
  if (String(topic) == TOPIC_FAN1) {
    if (msg == "on") {
      fan1State = true;
      digitalWrite(FAN1_PIN, HIGH);
      Serial.println("Fan1 ON");
    } else if (msg == "off") {
      fan1State = false;
      digitalWrite(FAN1_PIN, LOW);
      Serial.println("Fan1 OFF");
    }
  }

  // 加湿器控制（humi006 主题）
  if (String(topic) == TOPIC_FAN2) {
    if (msg == "on") {
      fan2State = true;
      digitalWrite(FAN2_PIN, HIGH);
      Serial.println("Humidifier ON");
    } else if (msg == "off") {
      fan2State = false;
      digitalWrite(FAN2_PIN, LOW);
      Serial.println("Humidifier OFF");
    }
  }

  // 蜂鸣器开关（mute006 主题，on=开启 off=静音）
  if (String(topic) == TOPIC_MUTE) {
    if (msg == "on") {
      muted = false;
      Serial.println("Buzzer enabled");
    } else if (msg == "off") {
      muted = true;
      digitalWrite(BUZZER_PIN, HIGH);  // 立即停止
      Serial.println("Buzzer muted");
    }
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
    bemfa.subscribe(TOPIC_TEMP);
    bemfa.subscribe(TOPIC_DATA);
    bemfa.subscribe(TOPIC_FAN1);
    bemfa.subscribe(TOPIC_FAN2);
    bemfa.subscribe(TOPIC_MUTE);
    Serial.printf("Bemfa subscribed: %s, %s, %s, %s, %s\n", TOPIC_TEMP, TOPIC_DATA, TOPIC_FAN1, TOPIC_FAN2, TOPIC_MUTE);
  } else {
    Serial.printf("Bemfa failed, rc=%d\n", bemfa.state());
  }
}

// ========== 巴法云上报 ==========
void publishBemfa(float temp, float humi) {
  // temp004：标准温湿度格式（小爱查询）
  String msgTH = "#" + String((int)temp) + "#" + String((int)humi);
  bool ok1 = bemfa.publish(TOPIC_TEMP, msgTH.c_str());
  Serial.printf("Bemfa Pub [%s]: %s -> %s\n", TOPIC_TEMP, msgTH.c_str(), ok1 ? "OK" : "FAIL");

  // data004：光照+设备状态+人体存在（网页用）
  String msgData = "#" + String(lightPct) + "#" + String(lightLux) + "#" +
                   (fan1State ? "on" : "off") + "#" + (fan2State ? "on" : "off") + "#" +
                   (presenceState ? "on" : "off");
  bool ok2 = bemfa.publish(TOPIC_DATA, msgData.c_str());
  Serial.printf("Bemfa Pub [%s]: %s -> %s\n", TOPIC_DATA, msgData.c_str(), ok2 ? "OK" : "FAIL");
}

// ========== 获取星期字符串 ==========
const char* getWeekDay(int wday) {
  const char* weekDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  return weekDays[wday];
}

// ========== 屏幕显示时间 ==========
void drawTime() {
  time_t now = time(nullptr);
  struct tm* timeInfo = localtime(&now);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.printf("%04d-%02d-%02d",
             timeInfo->tm_year + 1900,
             timeInfo->tm_mon + 1,
             timeInfo->tm_mday);

  tft.setCursor(10, 35);
  tft.printf("%02d:%02d:%02d %s",
             timeInfo->tm_hour,
             timeInfo->tm_min,
             timeInfo->tm_sec,
             getWeekDay(timeInfo->tm_wday));
}

// ========== 屏幕显示数据 ==========
void drawScreen(float temp, float humi) {
  // 显示时间
  drawTime();

  // 显示温湿度
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 70);
  tft.printf("Temp: %.1f C   ", temp);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(10, 100);
  tft.printf("Humi: %.1f %%   ", humi);

  // 显示风扇/加湿器状态
  tft.setTextColor(fan1State ? TFT_RED : TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 130);
  tft.printf("Fan : %s   ", fan1State ? "ON " : "OFF");

  tft.setTextColor(fan2State ? TFT_BLUE : TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 160);
  tft.printf("Humi: %s   ", fan2State ? "ON " : "OFF");

  // 显示人体存在 + 光照强度
  tft.setTextColor(presenceState ? TFT_GREEN : TFT_MAGENTA, TFT_BLACK);
  tft.setCursor(10, 190);
  tft.printf("P:%s L:%3d%%  ", presenceState ? "YES" : "NO", lightPct);

  // 显示蜂鸣器状态
  tft.setTextColor(muted ? TFT_WHITE : TFT_YELLOW, TFT_BLACK);
  tft.setCursor(10, 220);
  tft.printf("Buzz: %s   ", muted ? "OFF" : "ON ");
}

// ========== Setup ==========
void setup() {
  Serial.begin(115200);
  Serial.println("System Starting...");

  pinMode(FAN1_PIN, OUTPUT);
  pinMode(FAN2_PIN, OUTPUT);
  pinMode(SR602_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(FAN1_PIN, LOW);
  digitalWrite(FAN2_PIN, LOW);
  digitalWrite(BUZZER_PIN, HIGH);  // 低电平触发，默认 HIGH 不响

  dht.begin();

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  showBootAnimation();

  connectWiFi();
  delay(500);

  // NTP 时间同步
  configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER1, NTP_SERVER2);
  Serial.println("Waiting for NTP time sync...");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("\nNTP time synced!");

  // 巴法云
  bemfa.setServer(BEMFA_SERVER, BEMFA_PORT);
  bemfa.setKeepAlive(60);
  bemfa.setCallback(bemfaCallback);  // 设置回调函数
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

  // 蜂鸣器非阻塞关断
  buzzerLoop();

  // 传感器采样（2秒间隔）
  if (millis() - lastReadTime > 2000) {
    lastReadTime = millis();

    // 读取光照强度（电路：3.3V→LDR→A0→10K→GND，亮=ADC高，暗=ADC低）
    int adc = analogRead(LIGHT_AO);
    lightPct = constrain(adc * 100 / 1023, 0, 100);
    // R_ldr = 10K × (1024 - adc) / adc （LDR在上，10K在下分压）
    float R_ldr = 10000.0f * (1024 - adc) / (adc + 1.0f);
    float lux = pow(50120.0f / R_ldr, 1.4286f);
    lightLux = constrain((int)lux, 0, 100000);

    // SR602 人体存在检测 + 光照联合控制屏幕
    bool present = digitalRead(SR602_PIN) == HIGH;
    if (present && !presenceState) {
      Serial.println("Presence detected");
    } else if (!present && presenceState) {
      Serial.println("Presence lost");
    }
    presenceState = present;

    // 有人 + 光照足够 → 亮屏；没人或光照不足 → 熄屏
    if (presenceState && lightPct > LIGHT_ON_THRESHOLD && !screenOn) {
      screenOn = true;
      Serial.printf("Presence + Light %d%% -> Screen ON\n", lightPct);
      darkAlerted = false;
    } else if ((!presenceState || lightPct < LIGHT_OFF_THRESHOLD) && screenOn) {
      tft.fillScreen(TFT_BLACK);
      screenOn = false;
      if (!presenceState) {
        Serial.println("No presence -> Screen OFF");
      } else {
        Serial.printf("Light %d%% -> Screen OFF\n", lightPct);
      }
      if (!darkAlerted) {
        beep();
        darkAlerted = true;
      }
    }

    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (isnan(h) || isnan(t)) {
      Serial.println("DHT read error!");
      if (screenOn) {
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.setCursor(10, 30);
        tft.print("DHT Error!   ");
      }
      return;
    }

    Serial.printf("Temp: %.1f C  Humi: %.1f %%  Light: %d%%/%d lx  Presence: %s\n",
                  t, h, lightPct, lightLux, presenceState ? "YES" : "NO");
    if (screenOn) {
      drawScreen(t, h);
    }

    // 温度报警（迟滞，防止反复触发）
    if (t > TEMP_ALARM) {
      if (!tempAlerted) {
        beep();
        tempAlerted = true;
      }
    } else if (t < TEMP_ALARM - 1.0) {
      tempAlerted = false;
    }

    // 湿度报警
    if (h > HUMI_ALARM) {
      if (!humiAlerted) {
        beep();
        humiAlerted = true;
      }
    } else if (h < HUMI_ALARM - 2.0) {
      humiAlerted = false;
    }

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
