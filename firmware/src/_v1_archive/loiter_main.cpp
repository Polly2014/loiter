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
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "config.h"

// Phase 7.6 OTA: FW_VERSION 来自 platformio.ini -DLOITER_FW_VERSION
// 兜底防止单独编 .cpp 时缺定义
#ifndef LOITER_FW_VERSION
#define LOITER_FW_VERSION "0.0.0-dev"
#endif
static const char* FW_VERSION = LOITER_FW_VERSION;

// Phase 7.6 OTA: 前向声明（实现见文件靠后），onMqttMessage 要先调它
static void handleOtaMessage(JsonDocument& doc);

// ============================================================
// NVS WiFi 持久化
// ============================================================
static Preferences prefs;
static String gSavedSSID;
static String gSavedPass;

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

// ---- IMU 体感移动 ----
static uint32_t gLastIMU     = 0;
static const uint32_t IMU_INTERVAL = 500;   // 每 500ms 采样一次
static const float DEAD_ZONE = 0.15f;       // 倾斜死区 (g)
static float    gLastSentDx  = 0;
static float    gLastSentDy  = 0;

// ---- 摇一摇随机 emote ----
static uint32_t gLastShake   = 0;
static const uint32_t SHAKE_COOLDOWN = 3000; // 3s 冷却（与服务端 emote 冷却对齐）
static const float SHAKE_THRESHOLD = 2.5f;   // 加速度阈值 (g)
static const char* EMOTES[] = {"bloom", "spark", "wind", "fox", "rain"};

// ---- Sprint 7 Phase 7.4: 技能系统（16 个常规 + 4 大招 + omni）----
// 4 系 × 4，必须与服务端 skills.py SKILLS_BY_ELEMENT 顺序一致
static const char* ALL_SKILLS[16] = {
    // nature
    "bloom", "wind", "rain", "leaf",
    // fire
    "spark", "fox", "flame", "comet",
    // water
    "wave", "bubble", "mist", "tide",
    // light
    "star", "aurora", "lightning", "halo",
};
// 每个 skill 对应的 element 索引（0=nature/绿, 1=fire/橙, 2=water/蓝, 3=light/金）
static const uint16_t ELEMENT_COLORS[4] = {
    0x37E0,  // nature: sage green ~#3DFD30
    0xDB23,  // fire: ember orange ~#DC4818
    0x5D5F,  // water: dusk blue ~#5BABFF
    0xE564,  // light: pollen gold ~#E5AC22
};
static const uint16_t ELEMENT_COLORS_DARK[4] = {
    0x29A4,  // dark sage
    0x4A03,  // dark ember
    0x1A8B,  // dark dusk
    0x39A2,  // dark gold
};
static bool gMySkills[16] = {false};     // 我拥有哪些 skill（与 ALL_SKILLS index 对齐）
static bool gMyStarter[16] = {false};    // 我入场的 starter（永不变）
static int gSkillStack = 0;              // 已收集数量
static uint32_t gSkillStateTs = 0;       // 上次状态推送时刻（用于 chip 行闪烁新得）
static int gLastGainedIdx[2] = {-1, -1}; // 最近 fused 新得的 2 个 skill idx，用于亮起特效

// ---- Sprint 7: 双机 shake 配对（PAIRING_MODE）----
// `/pair` 后 3s 求偶模式。第一次检测到剧烈 shake → 进入 1.2s **采样窗口**：
// 持续记录 |a| > PEAK_DETECT_THRESHOLD 的局部峰值，窗口结束时打包 fingerprint
// {peak_g, peaks, rhythm_ms, energy} 一次上报，让服务端比对双方曲线是否相似。
// 物理意义：两台贴一起摇 → 峰值数 / 节奏 / 能量都同步；远程独立摇 → 差很多 → 拒绝。
static const float PAIR_SHAKE_THRESHOLD = 3.0f;       // 进入采样窗口的触发阈值 (g)
static const float PEAK_DETECT_THRESHOLD = 2.0f;      // 采样期间认定为"一次摇"的局部峰值阈值
static const uint32_t PAIRING_WINDOW_MS = 3000;       // 与服务端 PAIRING_WINDOW_S 对齐
static const uint32_t SAMPLE_WINDOW_MS = 1200;        // shake 触发后采样 1.2s
static const uint32_t MIN_PEAK_GAP_MS = 80;           // 两次 peak 间最小间隔，去抖
static uint32_t gPairingUntil = 0;                    // 0=非求偶；>0=求偶截止 millis
// 采样状态机
static bool gSampling = false;
static uint32_t gSampleStartMs = 0;
static float gSamplePeakG = 0.0f;
static float gSampleEnergy = 0.0f;
static int gSamplePeaks = 0;
static uint32_t gLastPeakMs = 0;
static uint32_t gFirstPeakMs = 0;

// ---- 频道颜色（和大屏对齐）----
static const uint16_t CH_COLORS[] = {
    0xC560,  // main: warm gold (approx #b08830)
    0x0EC6,  // fishing: teal (#2a8a70)
    0x7A5F,  // help: purple (#7c5cbf)
};
static const uint16_t COL_NPC    = 0xFDA0;  // Vix 狐灵: bright amber
static const uint16_t COL_ANON   = 0x7BEF;  // anonymous: light gray
static const uint16_t COL_SELF   = TFT_YELLOW;
static const uint16_t COL_SYSTEM = 0x4208;  // dark gray

// ---- 屏幕布局（240×135, rotation 1, textSize 1 ≈ 40 列 × 16 行）----
static const int CHAR_W   = 6;
static const int LINE_H   = 8;
static const int STATUS_Y = 0;
// Phase 7.4: 状态栏下方加 8px chip 行（16 个 4×6 色块 + 进度数字）
static const int CHIP_Y   = 11;          // 紧接 status bar
static const int CHIP_H   = 9;
static const int LOG_Y0   = CHIP_Y + CHIP_H + 1;  // 21
static const int LOG_ROWS = 12;          // 砍 1 行让位给 chip 行
static const int INPUT_Y  = LOG_Y0 + LOG_ROWS * LINE_H + 2;   // ≈ 119

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
    // 深色底条 + 频道对应颜色的细线
    d.fillRect(0, STATUS_Y, 240, LINE_H + 3, TFT_BLACK);
    // Sprint 7: 求偶模式下用粉色线 + 剩余秒数
    bool pairing = (gPairingUntil != 0) && ((int32_t)(millis() - gPairingUntil) < 0);
    uint16_t lineCol = pairing ? TFT_MAGENTA : CH_COLORS[gChannel];
    d.drawFastHLine(0, LINE_H + 2, 240, lineCol);
    // 左：昵称@频道（频道名用频道色，求偶模式覆盖为粉色）
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(2, STATUS_Y + 1);
    d.print(gNick);
    if (pairing) {
        d.setTextColor(TFT_MAGENTA, TFT_BLACK);
        int remain = (int)((gPairingUntil - millis() + 999) / 1000);
        if (remain < 1) remain = 1;
        d.print(" PAIR ");
        d.print(remain);
        d.print("s");
    } else {
        d.setTextColor(CH_COLORS[gChannel], TFT_BLACK);
        d.print("@" + String(CHANNELS[gChannel]));
    }
    // 右：连接状态圆点 + 在线人数
    String right = String(gOnline);
    int rx = 240 - (right.length() * CHAR_W) - 14;
    // 绿/红圆点表示连接状态
    d.fillCircle(rx, STATUS_Y + 4, 3, gMqttUp ? TFT_GREEN : TFT_RED);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.setCursor(rx + 8, STATUS_Y + 1);
    d.print(right);
}

// Phase 7.4: chip 行 — 16 个小色块 + 进度数字 "x/16"
// 布局：x=0 → 16 个 4×6 chip (按 4 系分 4 组，每组 4 chip)，组间 2px 间隔
// x=128 起：进度数字 "x/16"，再后是 ⚡(大招) ✦(omni) 标签
static void drawChips() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, CHIP_Y, 240, CHIP_H, TFT_BLACK);
    int x = 2;
    for (int el = 0; el < 4; el++) {
        for (int i = 0; i < 4; i++) {
            int idx = el * 4 + i;
            bool owned = gMySkills[idx];
            uint16_t col = owned ? ELEMENT_COLORS[el] : ELEMENT_COLORS_DARK[el];
            d.fillRect(x, CHIP_Y + 1, 5, 6, col);
            // starter 加白色边框做区分
            if (gMyStarter[idx]) {
                d.drawRect(x - 1, CHIP_Y, 7, 8, TFT_WHITE);
            }
            x += 6;
        }
        x += 3;  // 组间距
    }
    // 进度数字
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.setCursor(x + 4, CHIP_Y + 1);
    char buf[12];
    snprintf(buf, sizeof(buf), "%d/16", gSkillStack);
    d.print(buf);
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
    d.drawFastHLine(0, INPUT_Y - 1, 240, 0x2104);  // subtle separator
    d.setTextColor(CH_COLORS[gChannel], TFT_BLACK);
    d.setCursor(0, INPUT_Y + 1);
    String shown = "> " + gInput + "_";
    if (shown.length() > 40) shown = "> ~" + gInput.substring(gInput.length() - 36) + "_";
    d.print(shown);
}

static void redrawAll() {
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    drawStatus();
    drawChips();
    drawLog();
    drawInput();
}

// ============================================================
// WiFi Setup UI — scan, select, enter password
// ============================================================
static bool gWifiSetupMode = false;

static bool runWifiSetup() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(TFT_BLACK);
    d.setTextSize(1);

    // --- Phase 1: Scan ---
    d.setTextColor(0xC560, TFT_BLACK);
    d.setCursor(4, 4);
    d.print("WIFI SETUP - scanning...");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    int n = WiFi.scanNetworks();
    if (n <= 0) {
        d.setCursor(4, 20);
        d.setTextColor(TFT_RED, TFT_BLACK);
        d.print("No networks found!");
        delay(2000);
        return false;
    }

    // --- Phase 2: Select SSID (up/down = fn+;/fn+. , Enter = select) ---
    int sel = 0;
    int maxShow = min(n, 13);  // fit in 135px screen
    bool selecting = true;

    while (selecting) {
        d.fillScreen(TFT_BLACK);
        d.setTextColor(0xC560, TFT_BLACK);
        d.setCursor(4, 0);
        d.printf("SELECT WIFI (%d found)", n);
        d.drawFastHLine(0, 9, 240, 0x4208);

        for (int i = 0; i < maxShow; i++) {
            int idx = i;  // simple: show first maxShow networks sorted by RSSI
            int y = 12 + i * 9;
            if (i == sel) {
                d.fillRect(0, y, 240, 9, 0xC560);
                d.setTextColor(TFT_BLACK, 0xC560);
            } else {
                d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
            }
            d.setCursor(4, y + 1);
            String label = WiFi.SSID(idx);
            if (label.length() > 30) label = label.substring(0, 29) + "~";
            int rssi = WiFi.RSSI(idx);
            char buf[40];
            snprintf(buf, sizeof(buf), "%-30s %ddB", label.c_str(), rssi);
            d.print(buf);
        }

        // Wait for key
        while (true) {
            M5Cardputer.update();
            if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
                auto ks = M5Cardputer.Keyboard.keysState();
                if (ks.enter) { selecting = false; break; }
                // Navigate: Tab/Space = down, Backspace = up
                if (ks.del) {
                    sel = (sel + maxShow - 1) % maxShow;  // up
                    break;
                }
                if (ks.tab) {
                    sel = (sel + 1) % maxShow;  // down
                    break;
                }
                // Space or any printable = down
                if (ks.word.size() > 0) {
                    sel = (sel + 1) % maxShow;
                }
                break;
            }
            delay(10);
        }
    }

    String selectedSSID = WiFi.SSID(sel);
    WiFi.scanDelete();

    // --- Phase 3: Enter password ---
    d.fillScreen(TFT_BLACK);
    d.setTextColor(0xC560, TFT_BLACK);
    d.setCursor(4, 4);
    d.print("WIFI: " + selectedSSID);
    d.drawFastHLine(0, 14, 240, 0x4208);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.setCursor(4, 20);
    d.print("Enter password (Enter=connect):");

    String password = "";
    bool entering = true;

    while (entering) {
        // Draw password field
        d.fillRect(0, 34, 240, 12, TFT_BLACK);
        d.setTextColor(TFT_GREEN, TFT_BLACK);
        d.setCursor(4, 36);
        String shown = "> " + password + "_";
        if (shown.length() > 38) shown = "> ~" + password.substring(password.length() - 34) + "_";
        d.print(shown);

        // Show connecting hint
        d.fillRect(0, 54, 240, 10, TFT_BLACK);

        while (true) {
            M5Cardputer.update();
            if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
                auto ks = M5Cardputer.Keyboard.keysState();
                if (ks.enter) { entering = false; break; }
                if (ks.del) {
                    if (password.length()) password.remove(password.length() - 1);
                    break;
                }
                // Regular character input
                for (auto c : ks.word) {
                    if (c >= 32 && c < 127 && password.length() < 63) {
                        password += c;
                    }
                }
                break;
            }
            delay(10);
        }
    }

    // --- Phase 4: Try connecting ---
    d.fillRect(0, 54, 240, 20, TFT_BLACK);
    d.setTextColor(0xC560, TFT_BLACK);
    d.setCursor(4, 56);
    d.print("Connecting...");

    WiFi.begin(selectedSSID.c_str(), password.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
        delay(200);
        d.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        // Save to NVS
        prefs.begin("loiter", false);
        prefs.putString("ssid", selectedSSID);
        prefs.putString("pass", password);
        prefs.end();
        gSavedSSID = selectedSSID;
        gSavedPass = password;

        d.fillRect(0, 70, 240, 12, TFT_BLACK);
        d.setTextColor(TFT_GREEN, TFT_BLACK);
        d.setCursor(4, 72);
        d.print("Connected! IP: " + WiFi.localIP().toString());
        delay(1500);
        return true;
    } else {
        d.fillRect(0, 70, 240, 12, TFT_BLACK);
        d.setTextColor(TFT_RED, TFT_BLACK);
        d.setCursor(4, 72);
        d.print("Failed! Check password.");
        delay(2000);
        return false;
    }
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
    doc["fw_ver"] = FW_VERSION;  // Phase 7.6 OTA: 让服务端/大屏能看到当前版本，方便针对性推 OTA
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

// /emote <type> → 发送表情动作
static void publishEmote(const String& emote) {
    JsonDocument doc;
    doc["uid"] = gUid;
    doc["nick"] = gNick;
    doc["emote"] = emote;
    doc["ts"] = (uint32_t)millis();
    char buf[160];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(topic("emote").c_str(), (const uint8_t*)buf, n, false);
}

// Sprint 7: /pair → 求偶意向上报
static void publishPairIntent() {
    JsonDocument doc;
    doc["uid"] = gUid;
    doc["nick"] = gNick;
    doc["ts"] = (uint32_t)millis();
    char buf[128];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(topic("pair/intent").c_str(), (const uint8_t*)buf, n, false);
}

// Sprint 7: 求偶模式期间剧烈 shake → 上报（含 fingerprint）
static void publishPairShake(float peakG, int peaks, int rhythmMs, float energy) {
    JsonDocument doc;
    doc["uid"] = gUid;
    doc["peak_g"] = peakG;
    doc["peaks"] = peaks;
    doc["rhythm_ms"] = rhythmMs;
    doc["energy"] = energy;
    doc["ts"] = (uint32_t)millis();
    char buf[160];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(topic("pair/shake").c_str(), (const uint8_t*)buf, n, false);
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
    // Phase 7.6 OTA: 必须放在通用 sys/ 之前，否则会被当 notice 文本吃掉
    if (tp == "loiter/hall/sys/ota") {
        handleOtaMessage(doc);
        return;
    }
    if (tp.startsWith("loiter/hall/sys/")) {
        String text = doc["text"] | "";
        if (text.length()) { pushLog("* " + text, TFT_MAGENTA); drawLog(); }
        return;
    }
    if (tp == "loiter/hall/emote") {
        String uid   = doc["uid"]   | "";
        if (uid == gUid) return;          // 自己的已本地回显
        String nick  = doc["nick"]  | "?";
        String emote = doc["emote"] | "?";
        pushLog(nick + " [" + emote + "]", TFT_MAGENTA);
        drawLog();
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
            pushLog("[avatar] new look!", TFT_GREEN);
            drawAvatarToast();
        } else {
            pushLog("! avatar decode failed", TFT_RED);
            drawLog();
        }
        return;
    }
    if (tp.startsWith("loiter/hall/pair/result/")) {
        // Sprint 7: pair/result/<uid> → phase=armed/fused/rejected/state
        String phase = doc["phase"] | "";
        if (phase == "state") {
            // Phase 7.4: 服务端推全量 skill state（入场 / 配对后 / reset 后）
            for (int i = 0; i < 16; i++) gMySkills[i] = false;
            JsonArray skills = doc["skills"].as<JsonArray>();
            for (JsonVariant v : skills) {
                String sk = v.as<String>();
                for (int i = 0; i < 16; i++) {
                    if (sk == ALL_SKILLS[i]) { gMySkills[i] = true; break; }
                }
            }
            // starter（永不变，但每次 state 都同步以防漂移）
            for (int i = 0; i < 16; i++) gMyStarter[i] = false;
            JsonArray starter = doc["starter"].as<JsonArray>();
            for (JsonVariant v : starter) {
                String sk = v.as<String>();
                for (int i = 0; i < 16; i++) {
                    if (sk == ALL_SKILLS[i]) { gMyStarter[i] = true; break; }
                }
            }
            gSkillStack = (int)(doc["stack"] | 0);
            gSkillStateTs = millis();
            drawChips();
            return;
        }
        if (phase == "armed") {
            uint32_t winSec = (uint32_t)(doc["window_s"] | 3);
            gPairingUntil = millis() + winSec * 1000;
            pushLog("* PAIRING armed — SHAKE TOGETHER!", TFT_MAGENTA);
            drawLog();
            drawStatus();
        } else if (phase == "fused") {
            gPairingUntil = 0;
            String partner  = doc["partner_nick"] | "?";
            int total       = doc["stack"]        | 0;
            int totalAll    = doc["total"]        | 16;
            bool omni       = doc["omni"]         | false;
            JsonArray gained = doc["gained"].as<JsonArray>();
            JsonArray ults   = doc["ultimates"].as<JsonArray>();
            String gainedStr = "";
            for (JsonVariant v : gained) {
                if (gainedStr.length()) gainedStr += " ";
                gainedStr += v.as<String>();
            }
            if (gainedStr.length() == 0) gainedStr = "(no new)";
            String title = "+ " + gainedStr;
            String desc  = "paired " + partner + "  " +
                           String(total) + "/" + String(totalAll);
            if (ults.size() > 0) {
                desc += "  ULT!";
            }
            if (omni) {
                desc = "OMNISCIENT! " + desc;
            }
            pushLog("[pair] " + title, TFT_MAGENTA);
            pushLog("       " + desc, TFT_DARKGREY);
            drawToast(title, desc);
            drawStatus();
        } else if (phase == "rejected") {
            // Sprint 7: 配对被拒（指纹不匹配 / 距离太远）
            // 不清 gPairingUntil → 用户可以在剩余窗口里再摇一次
            String partner = doc["partner_nick"] | "?";
            String reason  = doc["reason"]       | "";
            if (reason == "too_far") {
                int dist = (int)(doc["distance"] | 0);
                pushLog("[pair] too far from " + partner, TFT_RED);
                pushLog("       walk closer (" + String(dist) + "px)", TFT_DARKGREY);
                drawToast(String("WALK CLOSER"),
                          String("near ") + partner + "  " + String(dist) + "px");
            } else {
                float sim = (float)(doc["similarity"] | 0.0);
                pushLog("[pair] sync but no grip!", TFT_RED);
                pushLog("       try again sim=" + String(sim, 2), TFT_DARKGREY);
                drawToast(String("STICK TOGETHER"),
                          String("synced ") + partner + "  sim " + String(sim, 2));
            }
        }
        return;
    }
    if (tp.startsWith("loiter/hall/msg/")) {
        String uid  = doc["uid"]  | "";
        if (uid == gUid) return;          // 自己的消息本地已回显
        String nick = doc["nick"] | "?";
        String text = doc["text"] | "";
        // 匿名告白: gray color
        if (uid == "anon" && text.startsWith("\xf0\x9f\x92\x8c")) {
            pushLog("??? " + text, COL_ANON);
            drawLog();
            return;
        }
        // Determine message color by source
        uint16_t msgColor = TFT_CYAN;
        if (nick == "Vix" || uid == "npc-vix") {
            msgColor = COL_NPC;      // NPC: amber
        } else {
            // Match channel color
            String ch = tp.substring(strlen("loiter/hall/msg/"));
            for (int i = 0; i < 3; i++) {
                if (ch == CHANNELS[i]) { msgColor = CH_COLORS[i]; break; }
            }
        }
        String ch   = tp.substring(strlen("loiter/hall/msg/"));
        String line = (ch == CHANNELS[gChannel] ? "" : ("#" + ch + " ")) + nick + ": " + text;
        pushLog(line, msgColor);
        drawLog();
    }
}

// ============================================================
// Phase 7.6 OTA — sys/ota 触发的固件升级
// ============================================================
// 流程: MQTT 收到 sys/ota retain message → 校验 version + targets → 设 pending flag
//       → loop() 跳出 callback 后调 performOTA() → 全屏进度 UI → HTTPClient 流式拉
//       → 边写 Update.h 边算 SHA256 → 校验通过则 commit + ESP.restart()
// 安全: SHA256 不匹配/写入失败 → Update.abort() + 显示错误 + 不重启
//       ESP32 双 partition 天然回滚（commit 前老 partition 保留）

struct OtaPending {
    bool      active = false;     // true = main loop 该执行 OTA
    String    url;                // http(s):// 都支持
    String    sha256;             // 64 char hex 小写
    uint32_t  size = 0;           // 字节数（manifest 提供，预分配 partition 用）
    String    version;            // remote 版本，仅用于日志/UI
};
static OtaPending gOta;
static bool gOtaInProgress = false;  // performOTA 重入保护

// 解析 "0.2.0" → [0, 2, 0]；缺位补 0；非数字片段当 0。
static void parseSemver(const String& s, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    int idx = 0, start = 0;
    for (int i = 0; i <= (int)s.length() && idx < 3; i++) {
        if (i == (int)s.length() || s[i] == '.') {
            String part = s.substring(start, i);
            out[idx++] = part.toInt();
            start = i + 1;
        }
    }
}

// remote > current? 三段 semver 比较；"0.10.0" > "0.2.5" 不会踩 lexical 陷阱。
static bool versionNewer(const String& remote, const String& current) {
    int r[3], c[3];
    parseSemver(remote, r);
    parseSemver(current, c);
    for (int i = 0; i < 3; i++) {
        if (r[i] != c[i]) return r[i] > c[i];
    }
    return false;
}

// targets: "all" 或 "card-abc,card-def"（含我 uid）→ true
static bool targetsMe(const String& targets) {
    String t = targets;
    t.trim();
    if (t.length() == 0 || t == "all" || t == "*") return true;
    // 简单包含检查（用 "," 包裹避免 card-ab 匹配 card-abc）
    String haystack = "," + t + ",";
    haystack.replace(" ", "");
    String needle = "," + gUid + ",";
    return haystack.indexOf(needle) >= 0;
}

// 把 32 字节 hash 转 64 char 小写 hex
static String hashToHex(const uint8_t hash[32]) {
    String out;
    out.reserve(64);
    char buf[3];
    for (int i = 0; i < 32; i++) {
        snprintf(buf, sizeof(buf), "%02x", hash[i]);
        out += buf;
    }
    return out;
}

// 全屏 OTA UI：渲染进度条 + 状态文字
static void drawOtaUi(const String& title, const String& detail, int pct, uint16_t color) {
    auto& d = M5Cardputer.Display;
    d.fillScreen(TFT_BLACK);
    // header
    d.setTextSize(2);
    d.setTextColor(0xC560, TFT_BLACK);  // warm gold
    d.setCursor(8, 8);
    d.print("OTA");
    d.setTextSize(1);
    d.setTextColor(color, TFT_BLACK);
    d.setCursor(48, 14);
    d.print(title);
    d.drawFastHLine(0, 30, 240, 0x4208);
    // detail line（version / size / error）
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.setCursor(8, 40);
    d.print(detail);
    // progress bar
    if (pct >= 0) {
        int barX = 8, barY = 70, barW = 224, barH = 18;
        d.drawRect(barX, barY, barW, barH, 0x7BEF);
        int fill = (barW - 2) * pct / 100;
        d.fillRect(barX + 1, barY + 1, fill, barH - 2, color);
        char buf[16];
        snprintf(buf, sizeof(buf), "%3d%%", pct);
        d.setTextColor(TFT_WHITE, TFT_BLACK);
        d.setCursor(105, barY + barH + 8);
        d.print(buf);
    }
    // footer hint
    d.setTextColor(0x4208, TFT_BLACK);
    d.setCursor(8, 120);
    d.print("do not unplug");
}

// 真干活：HTTPClient 流式拉 → Update.write → sha256 verify → restart
static void performOTA(const OtaPending& p) {
    gOtaInProgress = true;
    drawOtaUi("starting", "v" + p.version, 0, TFT_CYAN);

    // 离线/无 WiFi 直接放弃
    if (WiFi.status() != WL_CONNECTED) {
        drawOtaUi("no wifi", "abort", -1, TFT_RED);
        delay(2500);
        gOtaInProgress = false;
        redrawAll();
        return;
    }

    // HTTPS 用 WiFiClientSecure + setInsecure（cert 校验依赖 cloudflare CA 包很麻烦，
    // 关键防护是后面的 SHA256 校验：哪怕 TLS 被 MITM，bin 改一字节就 hash 不上）
    WiFiClient* client = nullptr;
    WiFiClientSecure secure;
    WiFiClient plain;
    if (p.url.startsWith("https://")) {
        secure.setInsecure();
        client = &secure;
    } else if (p.url.startsWith("http://")) {
        client = &plain;
    } else {
        drawOtaUi("bad url", p.url.substring(0, 32), -1, TFT_RED);
        delay(3000);
        gOtaInProgress = false;
        redrawAll();
        return;
    }

    HTTPClient http;
    http.setTimeout(15000);
    if (!http.begin(*client, p.url)) {
        drawOtaUi("begin failed", "url err", -1, TFT_RED);
        delay(2500);
        gOtaInProgress = false;
        redrawAll();
        return;
    }
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        drawOtaUi("http " + String(code), "abort", -1, TFT_RED);
        http.end();
        delay(2500);
        gOtaInProgress = false;
        redrawAll();
        return;
    }
    int contentLen = http.getSize();
    uint32_t expectSize = (contentLen > 0) ? (uint32_t)contentLen : p.size;
    if (expectSize == 0) {
        drawOtaUi("no size", "abort", -1, TFT_RED);
        http.end();
        delay(2500);
        gOtaInProgress = false;
        redrawAll();
        return;
    }

    // Update.h 预分配 inactive partition
    if (!Update.begin(expectSize, U_FLASH)) {
        drawOtaUi("flash full?", String(Update.errorString()), -1, TFT_RED);
        http.end();
        delay(3000);
        gOtaInProgress = false;
        redrawAll();
        return;
    }

    drawOtaUi("downloading", String(expectSize / 1024) + " KB", 0, TFT_CYAN);

    mbedtls_sha256_context shaCtx;
    mbedtls_sha256_init(&shaCtx);
    mbedtls_sha256_starts(&shaCtx, 0);

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[1024];
    uint32_t written = 0;
    int lastPct = -1;
    uint32_t lastDataMs = millis();
    while (http.connected() && (written < expectSize || expectSize == 0)) {
        size_t avail = stream->available();
        if (avail == 0) {
            if (millis() - lastDataMs > 8000) {
                // stuck
                Update.abort();
                mbedtls_sha256_free(&shaCtx);
                http.end();
                drawOtaUi("stalled", "abort", -1, TFT_RED);
                delay(2500);
                gOtaInProgress = false;
                redrawAll();
                return;
            }
            delay(2);
            continue;
        }
        size_t toRead = avail > sizeof(buf) ? sizeof(buf) : avail;
        int n = stream->readBytes(buf, toRead);
        if (n <= 0) { delay(2); continue; }
        if ((int)Update.write(buf, n) != n) {
            Update.abort();
            mbedtls_sha256_free(&shaCtx);
            http.end();
            drawOtaUi("write fail", String(Update.errorString()), -1, TFT_RED);
            delay(3000);
            gOtaInProgress = false;
            redrawAll();
            return;
        }
        mbedtls_sha256_update(&shaCtx, buf, n);
        written += n;
        lastDataMs = millis();
        int pct = (int)((written * 100ULL) / expectSize);
        if (pct != lastPct) {
            lastPct = pct;
            drawOtaUi("downloading",
                      String(written / 1024) + "/" + String(expectSize / 1024) + " KB",
                      pct, TFT_CYAN);
        }
    }
    http.end();

    if (written != expectSize) {
        Update.abort();
        mbedtls_sha256_free(&shaCtx);
        drawOtaUi("short read",
                  String(written) + "/" + String(expectSize),
                  -1, TFT_RED);
        delay(3000);
        gOtaInProgress = false;
        redrawAll();
        return;
    }

    // verify sha256
    uint8_t got[32];
    mbedtls_sha256_finish(&shaCtx, got);
    mbedtls_sha256_free(&shaCtx);
    String gotHex = hashToHex(got);
    String want = p.sha256;
    want.toLowerCase();
    if (want.length() == 64 && gotHex != want) {
        Update.abort();
        drawOtaUi("sha256 fail", gotHex.substring(0, 16) + "...", -1, TFT_RED);
        delay(4000);
        gOtaInProgress = false;
        redrawAll();
        return;
    }

    drawOtaUi("finalizing", "v" + p.version, 100, TFT_GREEN);
    if (!Update.end(true)) {
        drawOtaUi("commit fail", String(Update.errorString()), -1, TFT_RED);
        delay(3000);
        gOtaInProgress = false;
        redrawAll();
        return;
    }

    drawOtaUi("restarting", "v" + p.version, 100, TFT_GREEN);
    delay(1500);
    ESP.restart();
}

// MQTT callback 里调用：解析 + 过滤 + 写 pending flag。
// 真正执行放 loop()，避免在 callback 里阻塞 30 秒导致 keepalive 超时 + LWT 误发。
static void handleOtaMessage(JsonDocument& doc) {
    String version = doc["version"] | "";
    String url     = doc["url"]     | "";
    String sha     = doc["sha256"]  | "";
    String targets = doc["targets"] | "all";
    uint32_t size  = (uint32_t)(doc["size"] | 0);
    if (!version.length() || !url.length()) {
        pushLog("! ota: bad manifest", TFT_RED);
        drawLog();
        return;
    }
    if (!targetsMe(targets)) {
        pushLog("* ota " + version + " not for me", COL_SYSTEM);
        drawLog();
        return;
    }
    if (!versionNewer(version, String(FW_VERSION))) {
        pushLog("* ota " + version + " <= mine", COL_SYSTEM);
        drawLog();
        return;
    }
    if (gOta.active || gOtaInProgress) return;  // 已经排队/在跑
    gOta.url     = url;
    gOta.sha256  = sha;
    gOta.size    = size;
    gOta.version = version;
    gOta.active  = true;
    pushLog("* ota -> v" + version + " (" + String(size / 1024) + "KB)", TFT_YELLOW);
    drawLog();
}

// 非阻塞 MQTT 重连 — review #1：绝不在 loop() 里 while/delay 死等
static void mqttEnsure() {
    if (mqtt.connected()) { gMqttUp = true; return; }
    if (gMqttUp) {                        // 刚掉线
        gMqttUp = false;
        pushLog("* MQTT lost, retrying", TFT_ORANGE);
        drawLog(); drawStatus();
    }
    if (millis() - gLastMqttTry < 5000) return;   // 每 5s 试一次（给 broker 清理旧连接）
    gLastMqttTry = millis();

    // 重连前先断开，避免 broker "already connected, closing old" 循环
    mqtt.disconnect();

    // LWT：异常掉线时 broker 替我们发 leave
    JsonDocument will;
    will["uid"] = gUid;
    will["reason"] = "lwt";
    char wbuf[96];
    size_t wn = serializeJson(will, wbuf, sizeof(wbuf));
    String willTopic = topic("leave");

    // willQos=1：与 docs/mqtt-protocol.md 对齐（LWT QoS=1，掉线 leave 不丢）
    // 7-arg connect: id + user + pass + LWT (topic, qos, retain, payload)
    if (mqtt.connect(gUid.c_str(), MQTT_USER, MQTT_PASS,
                     willTopic.c_str(), 1, false, wbuf)) {
        (void)wn;
        gMqttUp = true;
        mqtt.subscribe("loiter/hall/msg/#", 1);   // QoS 1：broker 重启瞬间不丢消息
        mqtt.subscribe("loiter/hall/status", 0);  // 心跳丢一拍无所谓
        mqtt.subscribe("loiter/hall/sys/#", 1);
        mqtt.subscribe("loiter/hall/emote", 1);   // 表情动作
        mqtt.subscribe(("loiter/hall/achievement/" + gUid).c_str(), 1);  // QoS 1：成就解锁不丢
        mqtt.subscribe(("loiter/hall/avatar/" + gUid).c_str(), 1);       // QoS 1：头像位图不丢
        mqtt.subscribe(("loiter/hall/pair/result/" + gUid).c_str(), 1);  // Sprint 7：配对结果不丢
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
    // Use NVS credentials if available, else fallback to config.h
    if (gSavedSSID.length()) {
        WiFi.begin(gSavedSSID.c_str(), gSavedPass.c_str());
    } else {
        WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
}

// ============================================================
// 输入处理
// ============================================================
static void cycleChannel() {
    gChannel = (gChannel + 1) % (sizeof(CHANNELS) / sizeof(CHANNELS[0]));
    pushLog("* channel #" + String(CHANNELS[gChannel]), COL_SYSTEM);
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
            pushLog("* nick -> " + gNick, COL_SYSTEM);
            drawLog(); drawStatus();
        }
        drawInput();
        return;
    }
    // 命令：/wifi 重新配网
    if (line == "/wifi") {
        WiFi.disconnect();
        gWifiUp = false;
        gMqttUp = false;
        if (runWifiSetup()) {
            pushLog("* WiFi: " + gSavedSSID, TFT_GREEN);
        } else {
            pushLog("! WiFi setup cancelled", TFT_RED);
        }
        redrawAll();
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
            pushLog("* generating... " + kw, COL_SYSTEM);
        } else if (!gMqttUp) {
            pushLog("! offline", TFT_RED);
        }
        drawLog(); drawInput();
        return;
    }
    // 命令：/emote <type> 或简写 /e <type>
    // 可用：bloom spark wind fox rain
    if (line.startsWith("/emote ") || line.startsWith("/e ")) {
        String em = line.startsWith("/e ") ? line.substring(3) : line.substring(7);
        em.trim(); em.toLowerCase();
        if (em.length() && gMqttUp) {
            publishEmote(em);
            pushLog("* " + gNick + " [" + em + "]", TFT_MAGENTA);
        } else if (!gMqttUp) {
            pushLog("! offline", TFT_RED);
        }
        drawLog(); drawInput();
        return;
    }
    // 命令：/anon <message> 匿名告白
    if (line.startsWith("/anon ")) {
        String msg = line.substring(6); msg.trim();
        if (msg.length() && gMqttUp) {
            publishMsg("/anon " + msg);  // 服务端拦截处理
            pushLog("* sent anonymously", TFT_DARKGREY);
        } else if (!gMqttUp) {
            pushLog("! offline", TFT_RED);
        }
        drawLog(); drawInput();
        return;
    }
    // 命令：/quiz 发起问答赛
    if (line == "/quiz") {
        if (gMqttUp) {
            publishMsg("/quiz");  // 服务端拦截处理
            pushLog("* starting quiz...", TFT_CYAN);
        } else {
            pushLog("! offline", TFT_RED);
        }
        drawLog(); drawInput();
        return;
    }
    // 命令：/ans <answer> 问答赛抢答
    if (line.startsWith("/ans ")) {
        String ans = line.substring(5); ans.trim();
        if (ans.length() && gMqttUp) {
            publishMsg("/ans " + ans);  // 服务端拦截处理
            pushLog("* answer: " + ans, TFT_CYAN);
        } else if (!gMqttUp) {
            pushLog("! offline", TFT_RED);
        }
        drawLog(); drawInput();
        return;
    }
    // 命令：/pair 进入 3s 求偶模式（Sprint 7）
    if (line == "/pair") {
        if (gMqttUp) {
            publishPairIntent();
            pushLog("* /pair sent — find a partner!", TFT_MAGENTA);
        } else {
            pushLog("! offline", TFT_RED);
        }
        drawLog(); drawInput();
        return;
    }
    // 命令：/skills 查看自己的技能（Sprint 7 Phase 7.4 — 本地渲染，无需服务端往返）
    if (line == "/skills") {
        // 按 4 系打印，每系一行
        const char* ELEMENT_NAMES[4] = {"nature", "fire", "water", "light"};
        const uint16_t ELEMENT_TXT_COLORS[4] = {0x57E0, 0xFC03, 0x9D7F, 0xFE64};
        for (int el = 0; el < 4; el++) {
            String line2 = String(ELEMENT_NAMES[el]) + ":";
            for (int i = 0; i < 4; i++) {
                int idx = el * 4 + i;
                if (gMySkills[idx]) {
                    line2 += " ";
                    line2 += ALL_SKILLS[idx];
                    if (gMyStarter[idx]) line2 += "*";  // starter 标记 *
                }
            }
            pushLog(line2, ELEMENT_TXT_COLORS[el]);
        }
        pushLog("Skills " + String(gSkillStack) + "/16  * = starter", TFT_LIGHTGREY);
        drawLog(); drawInput();
        return;
    }

    // 普通消息
    if (gMqttUp) {
        publishMsg(line);
        pushLog(gNick + ": " + line, COL_SELF);
    } else {
        pushLog("! offline", TFT_RED);
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
// IMU 体感移动 — BMI270 加速度计 → 倾斜方向 → MQTT
// ============================================================
static void handleIMU() {
    if (!gMqttUp) return;

    float ax, ay, az;
    M5.Imu.getAccelData(&ax, &ay, &az);

    // ---- 摇一摇检测（每帧都检查，不受 IMU_INTERVAL 限制）----
    float mag = sqrtf(ax * ax + ay * ay + az * az);

    // Sprint 7: 求偶模式
    if (gPairingUntil != 0) {
        if ((int32_t)(millis() - gPairingUntil) >= 0) {
            // 求偶窗口超时
            gPairingUntil = 0;
            if (gSampling) {
                // 已经在采样中：窗口结束（或服务端窗口超时），强制收尾
                gSampling = false;
            }
            pushLog("* pairing timeout", TFT_DARKGREY);
            drawLog(); drawStatus();
        } else if (!gSampling) {
            // 还没采样：等第一个强 shake 触发 → 进入 1.2s 采样窗口
            if (mag > PAIR_SHAKE_THRESHOLD) {
                gSampling = true;
                gSampleStartMs = millis();
                gSamplePeakG = mag;
                gSampleEnergy = mag;
                gSamplePeaks = 1;
                gFirstPeakMs = gSampleStartMs;
                gLastPeakMs = gSampleStartMs;
                pushLog("* shake! (" + String(mag, 1) + "g) sampling...", TFT_MAGENTA);
                drawLog();
            }
        } else {
            // 采样中：累计能量 + 检测局部峰值
            gSampleEnergy += mag;
            if (mag > PEAK_DETECT_THRESHOLD &&
                millis() - gLastPeakMs > MIN_PEAK_GAP_MS) {
                gSamplePeaks++;
                gLastPeakMs = millis();
                if (mag > gSamplePeakG) gSamplePeakG = mag;
            }
            // 采样窗口结束 → 打包上报
            if (millis() - gSampleStartMs >= SAMPLE_WINDOW_MS) {
                int rhythmMs = 0;
                if (gSamplePeaks >= 2) {
                    rhythmMs = (int)((gLastPeakMs - gFirstPeakMs) / (gSamplePeaks - 1));
                }
                publishPairShake(gSamplePeakG, gSamplePeaks, rhythmMs, gSampleEnergy);
                pushLog("* fp: " + String(gSamplePeaks) + " peaks, " +
                        String(rhythmMs) + "ms, E=" + String(gSampleEnergy, 0),
                        TFT_MAGENTA);
                drawLog();
                gSampling = false;
                // 不清 gPairingUntil — 结果由服务端 pair/result 决定
            }
        }
    } else if (mag > SHAKE_THRESHOLD && millis() - gLastShake > SHAKE_COOLDOWN) {
        // 普通单人 shake → 随机放一个**自己拥有的** skill（A+ 改进，Phase 7.4）
        gLastShake = millis();
        // 收集所有 owned skill 的 index
        int ownedIdx[16], ownedCount = 0;
        for (int i = 0; i < 16; i++) if (gMySkills[i]) ownedIdx[ownedCount++] = i;
        String em;
        if (ownedCount > 0) {
            em = ALL_SKILLS[ownedIdx[esp_random() % ownedCount]];
        } else {
            // 兜底：还没拿到 state 推送，用老 EMOTES
            em = EMOTES[esp_random() % 5];
        }
        publishEmote(em);
        pushLog("* SHAKE! -> [" + em + "]", TFT_MAGENTA);
        drawLog();
    }

    // ---- 倾斜移动（节流到 IMU_INTERVAL）----
    // Sprint 7: 求偶模式 / shake 采样期间禁用倾斜移动，避免"摇出去"
    if (gPairingUntil != 0 || gSampling) {
        // 顺手把"上次发送方向"重置，避免退出 pair 时一直按 0 发 stop
        gLastSentDx = 0;
        gLastSentDy = 0;
        return;
    }
    if (millis() - gLastIMU < IMU_INTERVAL) return;
    gLastIMU = millis();

    // 死区过滤 + 钳位到 [-1, 1]
    // Cardputer rotation=1 (横屏键盘朝自己)：左倾=ax+，前倾=ay-，需取反适配大屏坐标
    float dx = (fabsf(ax) > DEAD_ZONE) ? constrain(-ax, -1.0f, 1.0f) : 0.0f;
    float dy = (fabsf(ay) > DEAD_ZONE) ? constrain(ay, -1.0f, 1.0f) : 0.0f;

    // 量化到 0.05 步进，减少无意义更新
    dx = roundf(dx * 20.0f) / 20.0f;
    dy = roundf(dy * 20.0f) / 20.0f;

    // 只在方向变化时发送（或从非零回到零时发一次 stop）
    if (dx == gLastSentDx && dy == gLastSentDy) return;
    gLastSentDx = dx;
    gLastSentDy = dy;

    JsonDocument doc;
    doc["uid"] = gUid;
    doc["dx"] = dx;
    doc["dy"] = dy;
    char buf[96];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish("loiter/hall/move", (const uint8_t*)buf, n, false);
}

// ============================================================
// setup / loop
// ============================================================
// Startup splash screen
static void drawSplash() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(TFT_BLACK);
    // LOITER logo — large centered text
    d.setTextSize(3);
    d.setTextColor(0xC560);  // warm gold
    const char* logo = "LOITER";
    int lw = strlen(logo) * 18;  // textSize 3 ≈ 18px/char
    d.setCursor((240 - lw) / 2, 30);
    d.print(logo);
    // Tagline
    d.setTextSize(1);
    d.setTextColor(0x7BEF);  // light gray
    const char* tag = "A pocket-sized social lobby";
    int tw = strlen(tag) * 6;
    d.setCursor((240 - tw) / 2, 75);
    d.print(tag);
    // Decorative line
    d.drawFastHLine(40, 68, 160, 0x4208);
    // Version / uid hint
    d.setTextColor(0x4208);
    d.setCursor(60, 100);
    d.print("v");
    d.print(FW_VERSION);
    d.print(" - connecting...");
}

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    auto& d = M5Cardputer.Display;
    d.setRotation(1);
    d.setTextSize(1);
    d.setTextFont(1);

    // uid 绑设备 MAC（持久身份）
    gUid = String("card-") + String((uint32_t)ESP.getEfuseMac(), HEX);

    // Load saved WiFi from NVS
    prefs.begin("loiter", true);  // read-only
    gSavedSSID = prefs.getString("ssid", "");
    gSavedPass = prefs.getString("pass", "");
    prefs.end();

    // Show splash screen
    drawSplash();

    // Try connecting with saved or config.h WiFi
    WiFi.mode(WIFI_STA);
    if (gSavedSSID.length()) {
        WiFi.begin(gSavedSSID.c_str(), gSavedPass.c_str());
    } else {
        WiFi.begin(WIFI_SSID, WIFI_PASS);
    }

    // Wait up to 5s for WiFi during splash
    unsigned long splashStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - splashStart < 5000) {
        delay(100);
    }

    // If no WiFi, enter setup mode
    if (WiFi.status() != WL_CONNECTED) {
        while (!runWifiSetup()) {
            // Keep retrying until connected
        }
    }

    redrawAll();
    pushLog("LOITER  v" + String(FW_VERSION), 0xC560);
    pushLog("uid=" + gUid, COL_SYSTEM);
    String wifiInfo = "WiFi: " + (gSavedSSID.length() ? gSavedSSID : String(WIFI_SSID));
    pushLog(wifiInfo, COL_SYSTEM);
    pushLog("/wifi /nick /face /e /anon /quiz", COL_SYSTEM);
    drawLog();
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(onMqttMessage);
    mqtt.setKeepAlive(60);
    mqtt.setBufferSize(1024);
}

void loop() {
    M5Cardputer.update();
    wifiEnsure();
    if (gWifiUp) mqttEnsure();
    mqtt.loop();
    // Phase 7.6 OTA: pending flag 由 MQTT callback 写入，这里跳出 callback 后才执行
    // 避免在 callback 内部跑 30s 阻塞下载，导致 keepalive 超时被 broker 踢
    if (gOta.active && !gOtaInProgress) {
        gOta.active = false;
        performOTA(gOta);
        // 若 OTA 失败 redrawAll 已恢复界面；若成功这行不会执行（已 reboot）
    }
    handleIMU();
    handleKeyboard();
    // 成就 toast 到期 → 还原界面（非阻塞）
    if (gToastUntil && (int32_t)(millis() - gToastUntil) >= 0) {
        gToastUntil = 0;
        redrawAll();
    }
    // Sprint 7: 求偶模式期间每秒刷一次状态栏倒计时
    static uint32_t gLastPairTick = 0;
    if (gPairingUntil != 0 && millis() - gLastPairTick >= 1000) {
        gLastPairTick = millis();
        drawStatus();
    }
}
