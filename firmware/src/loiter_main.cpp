// loiter_main.cpp — Loiter Sprint 1 正式固件
//
// 终端 UI + WiFi + MQTT + 文字聊天 + 频道切换。
// 把 Sprint 0 echo 升级成真正能用的客户端：
//   - 终端三段式 UI：顶部状态栏 / 中间消息流 / 底部输入行
//   - 非阻塞 WiFi + MQTT 重连（broker 挂了也不冻屏）— 小龙虾 review #1
//   - ArduinoJson 构造/解析 payload（避免引号/反斜杠注入坏包）— 小龙虾 review #2
//   - Tab 切频道（main / fishing / help），Enter 发送，Backspace 删字
//   - /nick <name> 改昵称（单机模拟多人，方便 demo）
//
// 协议契约见 docs/mqtt-protocol.md：
//   pub  loiter/hall/join          {uid,nick,ts}
//   pub  loiter/hall/leave (LWT)   {uid,reason}
//   pub  loiter/hall/msg/<channel> {uid,nick,text,ts}
//   sub  loiter/hall/msg/#         别人的消息
//   sub  loiter/hall/status        在线人数心跳
//   sub  loiter/hall/sys/notice    系统通告

#include <M5Cardputer.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "config.h"

// ============================================================
// 全局状态
// ============================================================
static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);

static String   gUid;                       // card-<mac>，绑设备持久
static String   gNick   = DEV_NICK;
static const char* CHANNELS[] = {"main", "fishing", "help"};
static int      gChannel = 0;               // 当前频道索引
static int      gOnline  = 0;               // 在线人数（来自 status 心跳）
static bool     gWifiUp  = false;
static bool     gMqttUp  = false;

static String   gInput   = "";              // 当前输入行
static uint32_t gLastMqttTry = 0;           // 非阻塞重连节流
static uint32_t gLastWifiTry = 0;

// ---- 屏幕布局（240×135, rotation 1, textSize 1 ≈ 40 列 × 16 行）----
static const int CHAR_W   = 6;
static const int LINE_H   = 8;
static const int STATUS_Y = 0;
static const int LOG_Y0   = 12;
static const int LOG_ROWS = 13;             // 中间消息区行数
static const int INPUT_Y  = LOG_Y0 + LOG_ROWS * LINE_H + 2;   // ≈ 118

// ---- 消息流环形缓冲 ----
struct LogRow { String text; uint16_t color; };
static LogRow gLog[LOG_ROWS];
static int    gLogCount = 0;

// ============================================================
// UI 绘制
// ============================================================
static void pushLog(const String& s, uint16_t color) {
    Serial.println(s);
    if (gLogCount < LOG_ROWS) {
        gLog[gLogCount++] = {s, color};
    } else {
        for (int i = 1; i < LOG_ROWS; i++) gLog[i - 1] = gLog[i];
        gLog[LOG_ROWS - 1] = {s, color};
    }
}

static void drawStatus() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, STATUS_Y, 240, LINE_H + 2, TFT_DARKGREEN);
    d.setTextColor(TFT_BLACK, TFT_DARKGREEN);
    d.setCursor(2, STATUS_Y + 1);
    // 左：昵称@频道   右：人数 + 连接灯
    String left = gNick + "@" + CHANNELS[gChannel];
    d.print(left);
    String right = String(gMqttUp ? "●" : "○") + " " + String(gOnline);
    int rx = 240 - (right.length() * CHAR_W) - 2;
    d.setCursor(rx, STATUS_Y + 1);
    d.print(right);
}

static void drawLog() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, LOG_Y0, 240, LOG_ROWS * LINE_H, TFT_BLACK);
    for (int i = 0; i < gLogCount; i++) {
        d.setTextColor(gLog[i].color, TFT_BLACK);
        d.setCursor(0, LOG_Y0 + i * LINE_H);
        // 截断到 40 列，避免换行打乱布局
        String t = gLog[i].text;
        if (t.length() > 40) t = t.substring(0, 39) + "~";
        d.print(t);
    }
}

static void drawInput() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, INPUT_Y, 240, LINE_H + 2, TFT_BLACK);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setCursor(0, INPUT_Y + 1);
    String shown = "> " + gInput + "_";
    if (shown.length() > 40) shown = "> ~" + gInput.substring(gInput.length() - 36) + "_";
    d.print(shown);
}

static void redrawAll() {
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    drawStatus();
    drawLog();
    drawInput();
}

// ============================================================
// MQTT
// ============================================================
static String topic(const char* sub) {
    return String("loiter/hall/") + sub;
}
static String msgTopic(int ch) {
    return String("loiter/hall/msg/") + CHANNELS[ch];
}

static void publishJoin() {
    JsonDocument doc;
    doc["uid"] = gUid;
    doc["nick"] = gNick;
    doc["ts"] = (uint32_t)millis();
    char buf[160];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(topic("join").c_str(), (const uint8_t*)buf, n, false);
}

static void publishMsg(const String& text) {
    JsonDocument doc;
    doc["uid"] = gUid;
    doc["nick"] = gNick;
    doc["text"] = text;                 // ArduinoJson 自动转义引号/反斜杠
    doc["ts"] = (uint32_t)millis();
    char buf[320];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(msgTopic(gChannel).c_str(), (const uint8_t*)buf, n, false);
}

static void onMqttMessage(char* topicC, byte* payload, unsigned int len) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len)) return;   // 坏 JSON 直接丢

    String tp = topicC;
    if (tp == "loiter/hall/status") {
        gOnline = doc["count"] | gOnline;
        drawStatus();
        return;
    }
    if (tp.startsWith("loiter/hall/sys/")) {
        String text = doc["text"] | "";
        if (text.length()) { pushLog("* " + text, TFT_MAGENTA); drawLog(); }
        return;
    }
    if (tp.startsWith("loiter/hall/msg/")) {
        String uid  = doc["uid"]  | "";
        if (uid == gUid) return;          // 自己的消息本地已回显
        String nick = doc["nick"] | "?";
        String text = doc["text"] | "";
        String ch   = tp.substring(strlen("loiter/hall/msg/"));
        String line = (ch == CHANNELS[gChannel] ? "" : ("#" + ch + " ")) + nick + ": " + text;
        pushLog(line, TFT_CYAN);
        drawLog();
    }
}

// 非阻塞 MQTT 重连 — review #1：绝不在 loop() 里 while/delay 死等
static void mqttEnsure() {
    if (mqtt.connected()) { gMqttUp = true; return; }
    if (gMqttUp) {                        // 刚掉线
        gMqttUp = false;
        pushLog("* MQTT lost, retrying", TFT_ORANGE);
        drawLog(); drawStatus();
    }
    if (millis() - gLastMqttTry < 3000) return;   // 每 3s 试一次，不阻塞
    gLastMqttTry = millis();

    // LWT：异常掉线时 broker 替我们发 leave
    JsonDocument will;
    will["uid"] = gUid;
    will["reason"] = "lwt";
    char wbuf[96];
    size_t wn = serializeJson(will, wbuf, sizeof(wbuf));
    String willTopic = topic("leave");

    // willQos=1：与 docs/mqtt-protocol.md 对齐（LWT QoS=1，掉线 leave 不丢）
    if (mqtt.connect(gUid.c_str(), willTopic.c_str(), 1, false, wbuf)) {
        (void)wn;
        gMqttUp = true;
        mqtt.subscribe("loiter/hall/msg/#", 1);   // QoS 1：broker 重启瞬间不丢消息
        mqtt.subscribe("loiter/hall/status", 0);  // 心跳丢一拍无所谓
        mqtt.subscribe("loiter/hall/sys/#", 1);
        publishJoin();
        pushLog("* connected as " + gNick, TFT_GREEN);
        drawLog(); drawStatus();
    }
}

// 非阻塞 WiFi 维持
static void wifiEnsure() {
    if (WiFi.status() == WL_CONNECTED) { gWifiUp = true; return; }
    gWifiUp = false;
    if (millis() - gLastWifiTry < 5000) return;
    gLastWifiTry = millis();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// ============================================================
// 输入处理
// ============================================================
static void cycleChannel() {
    gChannel = (gChannel + 1) % (sizeof(CHANNELS) / sizeof(CHANNELS[0]));
    pushLog("* 切到频道 #" + String(CHANNELS[gChannel]), TFT_DARKGREY);
    drawLog(); drawStatus();
}

static void handleEnter() {
    String line = gInput;
    gInput = "";
    line.trim();
    if (!line.length()) { drawInput(); return; }

    // 命令：/nick <name>
    if (line.startsWith("/nick ")) {
        String nn = line.substring(6);
        nn.trim();
        if (nn.length()) {
            gNick = nn;
            if (gMqttUp) publishJoin();   // 重新声明身份
            pushLog("* 昵称改为 " + gNick, TFT_DARKGREY);
            drawLog(); drawStatus();
        }
        drawInput();
        return;
    }
    // 命令：/ch <name>
    if (line.startsWith("/ch ")) {
        String c = line.substring(4); c.trim();
        for (int i = 0; i < 3; i++) if (c == CHANNELS[i]) gChannel = i;
        drawStatus(); drawInput();
        return;
    }

    // 普通消息
    if (gMqttUp) {
        publishMsg(line);
        pushLog(gNick + ": " + line, TFT_YELLOW);   // 本地回显
    } else {
        pushLog("! 离线，未发送", TFT_RED);
    }
    drawLog(); drawInput();
}

static void handleKeyboard() {
    if (!(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())) return;
    auto ks = M5Cardputer.Keyboard.keysState();

    if (ks.tab)   { cycleChannel(); return; }
    if (ks.enter) { handleEnter();  return; }
    if (ks.del) {                                 // Backspace
        if (gInput.length()) gInput.remove(gInput.length() - 1);
        drawInput();
        return;
    }
    bool changed = false;
    for (auto c : ks.word) { gInput += c; changed = true; }
    if (changed) drawInput();
}

// ============================================================
// setup / loop
// ============================================================
void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    auto& d = M5Cardputer.Display;
    d.setRotation(1);
    d.setTextSize(1);
    d.setTextFont(1);

    // uid 绑设备 MAC（持久身份）
    gUid = String("card-") + String((uint32_t)ESP.getEfuseMac(), HEX);

    redrawAll();
    pushLog("== LOITER ==  uid=" + gUid, TFT_GREEN);
    pushLog("Tab=频道 Enter=发送 /nick改名", TFT_DARKGREY);
    drawLog();

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(onMqttMessage);
    mqtt.setKeepAlive(30);
    mqtt.setBufferSize(1024);
}

void loop() {
    M5Cardputer.update();
    wifiEnsure();
    if (gWifiUp) mqttEnsure();
    mqtt.loop();
    handleKeyboard();
}
