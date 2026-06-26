// net.cpp — Loiter v2 联网层实现（移植自 v1 loiter_main.cpp，改 v2 协议）
#include "net.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "config.h"

#ifndef LOITER_FW_VERSION
#define LOITER_FW_VERSION "0.0.0-dev"
#endif
static const char* FW_VERSION = LOITER_FW_VERSION;

// user_profile.h 由 loiter-flash skill 每人生成（gitignored），只定义 LOITER_PROFILE_ID
// （一次性烧录身份）。缺失时 fallback 空串，编译不炸（review Codex #1）——
// 空 profile_id 时 server 会走 adopt 兜底（轮转一个新岛）。
#if defined(__has_include)
#  if __has_include("user_profile.h")
#    include "user_profile.h"
#  endif
#endif
#ifndef LOITER_PROFILE_ID
#define LOITER_PROFILE_ID ""
#endif

// ── 全局状态 ──────────────────────────────────────────────
static Preferences  prefs;
static String       gSavedSSID, gSavedPass;
static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);
static String       gUid;
static String       gNick = "anon";
static bool         gWifiUp = false, gMqttUp = false;
static uint32_t     gLastMqttTry = 0, gLastWifiTry = 0;
static NetCallbacks gCb = {};
static int          gProfileShape[5] = {0, 0, 0, 1, 0};
static int          gProfileColor[5] = {0, 0, 0, 0, 0};
static int          gProfileSigParticle = -1;
static int          gProfileSigAction = -1;

// 在线名册缓存（服务端 loiter/hall/roster retain 推送，仅已分岛成员）
#define ROSTER_MAX 32
struct RosterEntry { char nick[13]; int8_t island; };
static RosterEntry  gRoster[ROSTER_MAX];
static int          gRosterCount = 0;

static String tp(const char* sub) { return String("loiter/hall/") + sub; }

static void fillProfile(JsonDocument& doc) {
    doc["uid"] = gUid;
    doc["nick"] = gNick;
    JsonObject avatar = doc["avatar"].to<JsonObject>();
    JsonArray sh = avatar["shape"].to<JsonArray>();
    JsonArray co = avatar["color"].to<JsonArray>();
    for (int i = 0; i < 5; ++i) {
        sh.add(gProfileShape[i]);
        co.add(gProfileColor[i]);
    }
    JsonObject sig = doc["sig"].to<JsonObject>();
    sig["particle"] = gProfileSigParticle;
    sig["action"] = gProfileSigAction;
    doc["ts"] = (uint32_t)millis();
}

// ── 前向声明 ──
static bool runWifiSetup();

// ── 发布 ──────────────────────────────────────────────────
void net_set_nick(const char* nick) { gNick = nick; }

void net_set_profile(const int shape[5], const int color[5], int sig_particle, int sig_action) {
    for (int i = 0; i < 5; ++i) {
        gProfileShape[i] = shape[i];
        gProfileColor[i] = color[i];
    }
    gProfileSigParticle = sig_particle;
    gProfileSigAction = sig_action;
}

void net_publish_join(bool fresh) {
    if (!gMqttUp) return;
    JsonDocument doc;
    fillProfile(doc);
    doc["fw_ver"] = FW_VERSION;
    doc["profile_id"] = LOITER_PROFILE_ID;  // v3'：baked 一次性身份，server 据此分岛
    if (fresh) doc["fresh"] = true;          // Reset/重开旅程 → server 归一 spectrum（心跳不带）
    char buf[360];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(tp("join").c_str(), (const uint8_t*)buf, n, false);
}

void net_publish_profile() {
    if (!gMqttUp) return;
    JsonDocument doc;
    fillProfile(doc);
    char buf[300];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(tp("profile").c_str(), (const uint8_t*)buf, n, false);
}

void net_publish_leave() {
    if (!gMqttUp) return;
    JsonDocument doc;
    doc["uid"] = gUid;
    doc["reason"] = "reset";
    char buf[96];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(tp("leave").c_str(), (const uint8_t*)buf, n, false);
}

void net_publish_profile_request() {
    if (!gMqttUp) return;
    JsonDocument doc;
    doc["uid"] = gUid;
    char buf[96];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    // C→S：请 server 重推本机 island/<uid>（揭晓屏首发 + 重连兜底 + Reset 重取）。
    mqtt.publish(tp("profile/request").c_str(), (const uint8_t*)buf, n, false);
}

void net_publish_hi_request(const char* responder_uid, const char* msg) {
    if (!gMqttUp) return;
    JsonDocument doc;
    doc["requester"] = gUid;
    doc["responder"] = responder_uid;
    if (msg && msg[0]) doc["msg"] = msg;
    char buf[192];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(tp("hi/request").c_str(), (const uint8_t*)buf, n, false);
}

void net_publish_hi_request_nick(const char* responder_nick, const char* msg) {
    if (!gMqttUp) return;
    JsonDocument doc;
    doc["requester"] = gUid;
    doc["responder_nick"] = responder_nick;   // 服务端权威解析 nick→uid（仅已分岛成员）
    if (msg && msg[0]) doc["msg"] = msg;
    char buf[192];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(tp("hi/request").c_str(), (const uint8_t*)buf, n, false);
}

void net_publish_hi_respond(const char* requester_uid, bool accept) {
    if (!gMqttUp) return;
    JsonDocument doc;
    doc["requester"] = requester_uid;
    doc["responder"] = gUid;
    doc["accept"] = accept;
    char buf[160];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(tp("hi/respond").c_str(), (const uint8_t*)buf, n, false);
}

void net_publish_hi_cancel() {
    if (!gMqttUp) return;
    JsonDocument doc;
    doc["requester"] = gUid;
    char buf[96];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(tp("hi/cancel").c_str(), (const uint8_t*)buf, n, false);
}

void net_publish_jump() {
    if (!gMqttUp) return;
    JsonDocument doc;
    doc["uid"] = gUid;
    doc["ts"] = (uint32_t)millis();
    char buf[96];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(tp("jump").c_str(), (const uint8_t*)buf, n, false);
}

void net_publish_shake() {
    if (!gMqttUp) return;
    JsonDocument doc;
    doc["uid"] = gUid;
    doc["ts"] = (uint32_t)millis();
    char buf[96];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(tp("shake").c_str(), (const uint8_t*)buf, n, false);
}

void net_publish_anon(const char* text) {
    if (!gMqttUp) return;
    JsonDocument doc;
    doc["uid"] = gUid;
    doc["text"] = text;
    char buf[160];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(tp("anon").c_str(), (const uint8_t*)buf, n, false);
}

void net_publish_move(float dx, float dy) {
    if (!gMqttUp) return;
    JsonDocument doc;
    doc["uid"] = gUid;
    doc["dx"] = dx;
    doc["dy"] = dy;
    char buf[128];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(tp("move").c_str(), (const uint8_t*)buf, n, false);
}

void net_publish_sig(int particle, int action) {
    if (!gMqttUp) return;
    JsonDocument doc;
    doc["uid"] = gUid;
    doc["particle"] = particle;
    doc["action"] = action;
    char buf[96];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(tp("sig").c_str(), (const uint8_t*)buf, n, false);
}

bool net_request_reading() {
    if (!gMqttUp) return false;
    JsonDocument doc;
    doc["uid"] = gUid;
    char buf[96];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    return mqtt.publish(tp("reading/request").c_str(), (const uint8_t*)buf, n, false);
}

// ── 在线名册访问器 ──
int net_roster_count() { return gRosterCount; }
const char* net_roster_nick(int i) {
    return (i >= 0 && i < gRosterCount) ? gRoster[i].nick : "";
}
int net_roster_island(int i) {
    return (i >= 0 && i < gRosterCount) ? gRoster[i].island : -1;
}

// ── 入站 ──────────────────────────────────────────────────
static void onMqttMessage(char* topicC, byte* payload, unsigned int len) {
    String t = topicC;
    JsonDocument doc;
    DeserializationError jerr = deserializeJson(doc, payload, len);
    if (jerr) {
        return;
    }

    if (t == "loiter/hall/island/" + gUid) {
        if (gCb.on_island) {
            gCb.on_island(doc["island"] | -1,
                          doc["name"] | "",
                          doc["color"] | "#888888",
                          doc["reason_en"] | "",
                          doc["reason_cn"] | "");
        }
        return;
    }
    if (t == "loiter/hall/hi/result/" + gUid) {
        String ev = doc["event"] | "";
        if (ev == "incoming") {
            if (gCb.on_hi_incoming)
                gCb.on_hi_incoming(doc["requester"] | "", doc["requester_nick"] | "",
                                   doc["color"] | "#888888", doc["msg"] | "");
        } else {
            if (gCb.on_hi_result)
                gCb.on_hi_result(ev.c_str(), doc["partner"] | "",
                                 doc["color"] | "#888888", doc["slot"] | -1);
        }
        return;
    }
    if (t == "loiter/hall/roster") {
        gRosterCount = 0;
        JsonArray arr = doc["members"].as<JsonArray>();
        for (JsonObject m : arr) {
            if (gRosterCount >= ROSTER_MAX) break;
            const char* uid = m["uid"] | "";
            if (gUid == uid) continue;   // 跳过自己（HI 名册只列别人）
            const char* nk = m["nick"] | "";
            strncpy(gRoster[gRosterCount].nick, nk, sizeof(gRoster[0].nick) - 1);
            gRoster[gRosterCount].nick[sizeof(gRoster[0].nick) - 1] = '\0';
            gRoster[gRosterCount].island = m["island"] | -1;
            gRosterCount++;
        }
        if (gCb.redraw) gCb.redraw();
        return;
    }
    if (t == "loiter/hall/phase") {
        if (gCb.on_phase) gCb.on_phase(doc["phase"] | 1);
        return;
    }
    if (t == "loiter/hall/jump/progress") {
        // 集体 JUMP 实时进度 → P2-07 跳跃屏显真 N/need（替换本地假倒计时）
        if (gCb.on_jump_progress)
            gCb.on_jump_progress(doc["count"] | 0, doc["need"] | 5);
        return;
    }
    if (t == "loiter/hall/reading/" + gUid) {
        if (gCb.on_reading) {
            const char* en[9];
            const char* cn[9];
            for (int i = 0; i < 9; ++i) en[i] = doc["lines"][i] | "";
            for (int i = 0; i < 9; ++i) cn[i] = doc["lines_cn"][i] | "";
            gCb.on_reading(doc["title"] | "", doc["title_cn"] | "", doc["core_cn"] | "", en, cn);
        }
        return;
    }
    if (t == "loiter/hall/sig/" + gUid) {
        // 近距 shake 交换成功 → 服务端推“复制到的对方 sig”
        if (gCb.on_sig_recv)
            gCb.on_sig_recv(doc["particle"] | -1, doc["action"] | -1, doc["from"] | "");
        return;
    }
    // 注：不订阅 raw loiter/hall/anon——那是 C→S 上行（未剥离身份/未限流）。
    // 匿名公屏是服务端剥身份后只走大屏 WS（review Codex #4）。
}

// ── 连接维持 ──────────────────────────────────────────────
static void mqttEnsure() {
    if (mqtt.connected()) { gMqttUp = true; return; }
    gMqttUp = false;
    if (millis() - gLastMqttTry < 5000) return;
    gLastMqttTry = millis();
    mqtt.disconnect();  // 避免 broker "already connected" 循环踢

    JsonDocument will;
    will["uid"] = gUid;
    will["reason"] = "lwt";
    char wbuf[96];
    serializeJson(will, wbuf, sizeof(wbuf));
    String willTopic = tp("leave");

    if (mqtt.connect(gUid.c_str(), MQTT_USER, MQTT_PASS,
                     willTopic.c_str(), 1, false, wbuf)) {
        gMqttUp = true;
        mqtt.subscribe(("loiter/hall/island/" + gUid).c_str(), 1);
        mqtt.subscribe(("loiter/hall/hi/result/" + gUid).c_str(), 1);
        mqtt.subscribe(("loiter/hall/reading/" + gUid).c_str(), 1);
        mqtt.subscribe(("loiter/hall/sig/" + gUid).c_str(), 1);
        mqtt.subscribe("loiter/hall/roster", 1);
        mqtt.subscribe("loiter/hall/phase", 1);
        mqtt.subscribe("loiter/hall/jump/progress", 0);
        // 仅已输名（g_joined）后才发 auto-join；未输名时 gNick=="anon"，不发（防大屏出现幽灵 anon 小人）
        if (gNick != "anon") {
            net_publish_join();
            net_publish_profile();
        }
    }
}

static void wifiEnsure() {
    if (WiFi.status() == WL_CONNECTED) { gWifiUp = true; return; }
    gWifiUp = false;
    if (millis() - gLastWifiTry < 5000) return;
    gLastWifiTry = millis();
    if (gSavedSSID.length()) WiFi.begin(gSavedSSID.c_str(), gSavedPass.c_str());
    else                     WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// ── WiFi 配网门户（移植 v1 runWifiSetup，v2 视觉对齐 Designer Pride 风格）──

// Pride 6-band colors (same as ISLANDS in main.cpp, duplicated to keep net.cpp self-contained)
static const uint16_t PRIDE_BAND[6] = {
    0xE266, // EMBER  #E84D3C
    0xFCE6, // HEARTH #FF9F43
    0xFE47, // SPARK  #F9CA24
    0x6D86, // GROVE  #6AB04C
    0x4B74, // TIDE   #4A6FA5
    0x5137, // MIST   #5F27CD
};
static const uint16_t NET_COL_BG   = 0x18A4;  // COL_FRAME_BG #1a1726
static const uint16_t NET_COL_GOLD = 0xFF14;  // COL_GOLD #f8e1a0
static const uint16_t NET_COL_LAV  = 0xDE5F;  // COL_LAVENDER #d8c8ff

// Draw Pride rainbow stripes as background
static void drawPrideBg() {
    auto& d = M5Cardputer.Display;
    int bh = 135 / 6;
    for (int i = 0; i < 6; ++i) {
        int y = i * bh;
        int h = (i == 5) ? 135 - y : bh;
        d.fillRect(0, y, 240, h, PRIDE_BAND[i]);
    }
}

// Draw a centered dark overlay box with gold text (Designer Welcome style)
static void drawCenteredBox(const char* text, int textSize, int bx, int by, int bw, int bh, uint16_t fg = 0xFF14) {
    auto& d = M5Cardputer.Display;
    d.fillRect(bx, by, bw, bh, NET_COL_BG);
    d.drawRect(bx, by, bw, bh, 0x0000);
    d.setFont(&fonts::Font0);
    d.setTextDatum(middle_center);
    d.setTextSize(textSize);
    d.setTextColor(fg, NET_COL_BG);
    d.drawString(text, 120, by + bh / 2 + 1);
}

static bool runWifiSetup() {
    auto& d = M5Cardputer.Display;

    // ── Scanning screen ──
    drawPrideBg();
    drawCenteredBox("WIFI SETUP", 2, 30, 40, 180, 26);
    d.setTextSize(1); d.setTextColor(NET_COL_LAV, NET_COL_BG);
    d.drawString("scanning...", 120, 80);

    WiFi.mode(WIFI_STA); WiFi.disconnect(); delay(100);
    int n = WiFi.scanNetworks();
    if (n <= 0) {
        drawPrideBg();
        drawCenteredBox("NO WIFI FOUND", 2, 20, 45, 200, 26, TFT_RED);
        d.setTextSize(1); d.setTextColor(NET_COL_LAV, NET_COL_BG);
        d.drawString("check router", 120, 85);
        delay(2000); return false;
    }

    // ── Network selection screen ──
    int sel = 0, maxShow = min(n, 10); bool selecting = true;
    while (selecting) {
        d.fillScreen(NET_COL_BG);
        // Header banner (thin Pride stripe at top)
        for (int i = 0; i < 6; ++i) d.fillRect(i * 40, 0, 40, 3, PRIDE_BAND[i]);
        d.setFont(&fonts::Font0); d.setTextDatum(top_left); d.setTextSize(1);
        d.setTextColor(NET_COL_GOLD, NET_COL_BG);
        d.setCursor(4, 6);
        char hdr[32]; snprintf(hdr, sizeof(hdr), "SELECT WIFI  (%d found)", n);
        d.print(hdr);
        d.drawFastHLine(0, 16, 240, NET_COL_GOLD);

        // Network list
        for (int i = 0; i < maxShow; i++) {
            int y = 19 + i * 11;
            bool isSel = (i == sel);
            uint16_t rowBg = isSel ? PRIDE_BAND[i % 6] : NET_COL_BG;
            uint16_t rowFg = isSel ? 0x0000 : NET_COL_LAV;
            d.fillRect(0, y, 240, 11, rowBg);
            d.setTextColor(rowFg, rowBg);
            d.setCursor(6, y + 2);
            String label = WiFi.SSID(i);
            if (label.length() > 26) label = label.substring(0, 25) + "~";
            char buf[40]; snprintf(buf, sizeof(buf), "%-26s %ddB", label.c_str(), WiFi.RSSI(i));
            d.print(buf);
        }

        // Footer hints
        d.setTextColor(0x4208, NET_COL_BG);
        d.setCursor(4, 126); d.print("TAB=next  DEL=prev  ENTER=select");

        while (true) {
            M5Cardputer.update();
            if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
                auto ks = M5Cardputer.Keyboard.keysState();
                if (ks.enter) { selecting = false; break; }
                if (ks.del)  { sel = (sel + maxShow - 1) % maxShow; break; }
                if (ks.tab)  { sel = (sel + 1) % maxShow; break; }
                if (ks.word.size() > 0) { sel = (sel + 1) % maxShow; break; }
            }
            delay(10);
        }
    }
    String ssid = WiFi.SSID(sel); WiFi.scanDelete();

    // ── Password entry screen ──
    d.fillScreen(NET_COL_BG);
    for (int i = 0; i < 6; ++i) d.fillRect(i * 40, 0, 40, 3, PRIDE_BAND[i]);
    d.setFont(&fonts::Font0); d.setTextDatum(top_left); d.setTextSize(1);
    d.setTextColor(NET_COL_GOLD, NET_COL_BG); d.setCursor(4, 6);
    char ssidHdr[40]; snprintf(ssidHdr, sizeof(ssidHdr), "WIFI: %.28s", ssid.c_str());
    d.print(ssidHdr);
    d.drawFastHLine(0, 16, 240, NET_COL_GOLD);
    d.setTextColor(NET_COL_LAV, NET_COL_BG); d.setCursor(4, 22);
    d.print("enter password:");

    // Input area dark box
    d.fillRect(4, 36, 232, 16, 0x0000);
    d.drawRect(4, 36, 232, 16, 0x4208);

    String pass = ""; bool entering = true;
    while (entering) {
        d.fillRect(5, 37, 230, 14, 0x0000);
        d.setTextColor(TFT_GREEN, 0x0000); d.setCursor(8, 40);
        String shown = "> " + pass + "_";
        if (shown.length() > 36) shown = "> ~" + pass.substring(pass.length() - 32) + "_";
        d.print(shown);
        while (true) {
            M5Cardputer.update();
            if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
                auto ks = M5Cardputer.Keyboard.keysState();
                if (ks.enter) { entering = false; break; }
                if (ks.del) { if (pass.length()) pass.remove(pass.length() - 1); break; }
                for (auto c : ks.word) if (c >= 32 && c < 127 && pass.length() < 63) pass += c;
                break;
            }
            delay(10);
        }
    }

    // ── Connecting screen ──
    drawPrideBg();
    drawCenteredBox("CONNECTING...", 2, 20, 50, 200, 26);

    WiFi.begin(ssid.c_str(), pass.c_str());
    unsigned long start = millis();
    int dots = 0;
    while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
        delay(300);
        dots++;
        d.setTextSize(1); d.setTextColor(NET_COL_LAV, NET_COL_BG);
        d.setCursor(80 + dots * 8, 88); d.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        prefs.begin("loiter", false);
        prefs.putString("ssid", ssid); prefs.putString("pass", pass);
        prefs.end();
        gSavedSSID = ssid; gSavedPass = pass;
        drawPrideBg();
        drawCenteredBox("CONNECTED!", 2, 30, 45, 180, 26, TFT_GREEN);
        d.setTextSize(1); d.setTextColor(NET_COL_LAV, NET_COL_BG);
        d.drawString(WiFi.localIP().toString().c_str(), 120, 85);
        delay(1500); return true;
    }
    drawPrideBg();
    drawCenteredBox("FAILED!", 2, 50, 45, 140, 26, TFT_RED);
    d.setTextSize(1); d.setTextColor(NET_COL_LAV, NET_COL_BG);
    d.drawString("check password", 120, 85);
    delay(2000); return false;
}

void net_open_wifi_setup() {
    WiFi.disconnect();
    gWifiUp = false; gMqttUp = false;
    runWifiSetup();
    if (gCb.redraw) gCb.redraw();
}

// ── 生命周期 ──────────────────────────────────────────────
static void drawNetSplash() {
    drawPrideBg();
    // Title — same layout as P1-01 Welcome
    const char* title = "ISLANDS OF COLOR";
    int tw = 16 * 12;
    int bw = tw + 16, bh = 26;
    int bx = (240 - bw) / 2, by = 40;
    drawCenteredBox(title, 2, bx, by, bw, bh);
    // Version + connecting
    int sbw = 180, sbh = 14;
    int sbx = (240 - sbw) / 2, sby = by + bh + 6;
    auto& d = M5Cardputer.Display;
    d.fillRect(sbx, sby, sbw, sbh, NET_COL_BG);
    d.setFont(&fonts::Font0); d.setTextDatum(middle_center); d.setTextSize(1);
    d.setTextColor(0x4208, NET_COL_BG);
    char vbuf[40]; snprintf(vbuf, sizeof(vbuf), "v%s  connecting...", FW_VERSION);
    d.drawString(vbuf, 120, sby + sbh / 2);
}

void net_begin(const NetCallbacks& cb) {
    gCb = cb;
    // uid 用全 48 位 efuse MAC（高 16 位 = 厂商序列号是设备唯一标识，绝不能丢）。
    // 旧版 `(uint32_t)getEfuseMac()` 只取低 32 位 = MAC 前 4 字节，前 3 字节是 Espressif/M5
    // 厂商 OUI（所有设备相同）→ 同厂商设备极易撞 uid（实测两台烧成同一个 card-ce7d7850，
    // 大屏同 uid 共用一个 DOM、昵称/位置乱跳像两人重叠 — 2026-06-26 P0）。
    uint64_t mac = ESP.getEfuseMac();
    char ubuf[24];
    snprintf(ubuf, sizeof(ubuf), "card-%04x%08x",
             (uint16_t)(mac >> 32), (uint32_t)mac);   // 高16位(序列号) + 低32位
    gUid = String(ubuf);

    prefs.begin("loiter", true);
    gSavedSSID = prefs.getString("ssid", "");
    gSavedPass = prefs.getString("pass", "");
    prefs.end();

    drawNetSplash();
    WiFi.mode(WIFI_STA);
    if (gSavedSSID.length()) WiFi.begin(gSavedSSID.c_str(), gSavedPass.c_str());
    else                     WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 5000) delay(100);
    if (WiFi.status() != WL_CONNECTED) { while (!runWifiSetup()) {} }

    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(onMqttMessage);
    mqtt.setKeepAlive(30);  // 30s keepalive → broker 45s 判定断连 → LWT leave（缩短关机后小人残留时间）
    mqtt.setBufferSize(2048);
}

void net_loop() {
    wifiEnsure();
    if (gWifiUp) mqttEnsure();
    mqtt.loop();
}

bool net_online() { return gWifiUp && gMqttUp; }
const char* net_uid() { return gUid.c_str(); }
const char* net_fw_version() { return FW_VERSION; }
