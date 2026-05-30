// sprint0_echo.cpp — Loiter Sprint 0 链路验证固件
//
// 目标：用一台 Cardputer 把 [WiFi → MQTT broker → 收发] 整条链路打通，
//       消除 80% 技术风险。不含任何业务逻辑。
//
// 行为：
//   1. 连 WiFi (config.h 里的热点)
//   2. 连 Mac 上的 Mosquitto broker
//   3. 订阅 loiter/hall/echo，收到的消息打印到屏幕
//   4. 每 3s 往 loiter/hall/echo 发一条心跳 {nick, n, ts}
//   5. 按键盘任意键，把当前输入行作为消息发出去
//
// 验证方法（在 Mac 上）：
//   mosquitto_sub -t 'loiter/#' -v          ← 应能看到 Cardputer 的心跳
//   mosquitto_pub -t 'loiter/hall/echo' -m '{"hi":"mac"}'   ← Cardputer 屏幕应显示

#include <M5Cardputer.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "config.h"

static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);

static const char* TOPIC_ECHO = "loiter/hall/echo";

static String inputLine = "";
static uint32_t lastBeat = 0;
static uint32_t beatCount = 0;

// 屏幕滚动日志
static void logLine(const String& s) {
    auto& d = M5Cardputer.Display;
    Serial.println(s);
    d.print(s);
    d.println();
}

static void onMqttMessage(char* topic, byte* payload, unsigned int len) {
    String msg;
    for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
    M5Cardputer.Display.setTextColor(TFT_CYAN);
    logLine("< " + msg);
    M5Cardputer.Display.setTextColor(TFT_GREEN);
}

static void connectWifi() {
    logLine("WiFi: " WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(300);
        M5Cardputer.Display.print(".");
        if (millis() - t0 > 20000) { logLine("\nWiFi TIMEOUT"); return; }
    }
    logLine("\nIP: " + WiFi.localIP().toString());
}

static void connectMqtt() {
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(onMqttMessage);
    mqtt.setKeepAlive(60);
    String cid = String("cardputer-") + String((uint32_t)ESP.getEfuseMac(), HEX);
    logLine("MQTT: " MQTT_HOST);
    while (!mqtt.connected()) {
        if (mqtt.connect(cid.c_str())) {
            logLine("MQTT OK");
            mqtt.subscribe(TOPIC_ECHO);
        } else {
            logLine("MQTT rc=" + String(mqtt.state()) + " retry");
            delay(1500);
        }
    }
}

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    auto& d = M5Cardputer.Display;
    d.setRotation(1);
    d.setTextSize(1);
    d.fillScreen(TFT_BLACK);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setTextScroll(true);
    logLine("== LOITER sprint0 ==");

    connectWifi();
    if (WiFi.status() == WL_CONNECTED) connectMqtt();
}

void loop() {
    M5Cardputer.update();
    if (!mqtt.connected() && WiFi.status() == WL_CONNECTED) connectMqtt();
    mqtt.loop();

    // 3s 心跳
    if (millis() - lastBeat > 3000) {
        lastBeat = millis();
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "{\"nick\":\"%s\",\"n\":%u,\"ts\":%lu}",
                 DEV_NICK, (unsigned)beatCount++, (unsigned long)millis());
        mqtt.publish(TOPIC_ECHO, buf);
    }

    // 键盘输入：回车发送，其它键累积
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        auto ks = M5Cardputer.Keyboard.keysState();
        for (auto c : ks.word) inputLine += c;
        if (ks.enter && inputLine.length()) {
            String out = String("{\"nick\":\"") + DEV_NICK +
                         "\",\"text\":\"" + inputLine + "\"}";
            mqtt.publish(TOPIC_ECHO, out.c_str());
            M5Cardputer.Display.setTextColor(TFT_YELLOW);
            logLine("> " + inputLine);
            M5Cardputer.Display.setTextColor(TFT_GREEN);
            inputLine = "";
        }
    }
}
