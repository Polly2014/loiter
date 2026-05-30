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
#include "mbedtls/base64.h"
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
static uint32_t gToastUntil  = 0;           // 成就 toast 到期时间（0=无）

static uint8_t  gAvatar[32];                // 16×16 1-bit 头像位图（行 MSB 在前）
static bool     gHasAvatar = false;

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

// 成就 toast — 非阻塞横幅，3s 后由 loop() 还原界面
static void drawToast(const String& title, const String& desc) {
    auto& d = M5Cardputer.Display;
    const int y = 38, h = 54;
    d.fillRect(0, y, 240, h, TFT_YELLOW);
    d.drawRect(3, y + 3, 234, h - 6, TFT_BLACK);
    d.setTextColor(TFT_BLACK, TFT_YELLOW);
    d.setCursor(10, y + 9);  d.print("*  ACHIEVEMENT  *");
    d.setCursor(10, y + 24); d.print(title);
    d.setCursor(10, y + 38); d.print(desc);
    gToastUntil = millis() + 3000;
}

// AI 头像 toast — 把 16×16 1-bit 放大 ×4（64×64）居中预览，3s 后还原
static void drawAvatarToast() {
    auto& d = M5Cardputer.Display;
    const int scale = 4, side = 16 * scale;          // 64
    const int x = (240 - side) / 2, y = (135 - side) / 2 - 2;
    d.fillRect(x - 8, y - 14, side + 16, side + 22, TFT_BLACK);
    d.drawRect(x - 8, y - 14, side + 16, side + 22, TFT_GREEN);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setCursor(x - 4, y - 11); d.print("YOUR FACE");
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 16; col++) {
            // 服务端已反色：bit 1 = 主体（亮白前景）、bit 0 = 背景（纯黑融入 toast）
            int bit = (gAvatar[row * 2 + (col >> 3)] >> (7 - (col & 7))) & 1;
            uint16_t c = bit ? TFT_WHITE : TFT_BLACK;
            d.fillRect(x + col * scale, y + row * scale, scale, scale, c);
        }
    }
    gToastUntil = millis() + 4000;
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

// /face <keywords> → 请求服务端生成 AI 头像
static void publishAvatarRequest(const String& kw) {
    JsonDocument doc;
    doc["uid"] = gUid;
    JsonArray arr = doc["keywords"].to<JsonArray>();
    int start = 0;
    for (int i = 0; i <= (int)kw.length(); i++) {
        if (i == (int)kw.length() || kw[i] == ' ') {
            if (i > start) arr.add(kw.substring(start, i));
            start = i + 1;
        }
    }
    char buf[256];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(topic("avatar/request").c_str(), (const uint8_t*)buf, n, false);
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
    if (tp.startsWith("loiter/hall/achievement/")) {
        String title = doc["title"] | "";
        String desc  = doc["desc"]  | "";
        pushLog("[\u6210\u5c31] " + title, TFT_YELLOW);
        drawToast(title, desc);
        return;
    }
    if (tp.startsWith("loiter/hall/avatar/")) {
        const char* b64 = doc["bitmap_b64"] | "";
        size_t olen = 0;
        if (strlen(b64) &&
            mbedtls_base64_decode(gAvatar, sizeof(gAvatar), &olen,
                                  (const uint8_t*)b64, strlen(b64)) == 0 &&
            olen == sizeof(gAvatar)) {
            gHasAvatar = true;
            pushLog("[\u5934\u50cf] \u65b0\u8138\u5df2\u751f\u6210", TFT_GREEN);
            drawAvatarToast();
        } else {
            pushLog("! \u5934\u50cf\u89e3\u7801\u5931\u8d25", TFT_RED);
            drawLog();
        }
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
        mqtt.subscribe(("loiter/hall/achievement/" + gUid).c_str(), 1);  // QoS 1：成就解锁不丢
        mqtt.subscribe(("loiter/hall/avatar/" + gUid).c_str(), 1);       // QoS 1：头像位图不丢
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
    // 命令：/face <keywords> 生成 AI 头像
    if (line.startsWith("/face ")) {
        String kw = line.substring(6); kw.trim();
        if (kw.length() && gMqttUp) {
            publishAvatarRequest(kw);
            pushLog("* 生成头像中… " + kw, TFT_DARKGREY);
        } else if (!gMqttUp) {
            pushLog("! 离线，无法生成", TFT_RED);
        }
        drawLog(); drawInput();
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
    pushLog("Tab=频道 Enter=发送 /nick改名 /face头像", TFT_DARKGREY);
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
    // 成就 toast 到期 → 还原界面（非阻塞）
    if (gToastUntil && (int32_t)(millis() - gToastUntil) >= 0) {
        gToastUntil = 0;
        redrawAll();
    }
}
