// net.h — Loiter v2 联网层（WiFi + MQTT + UID + 配网门户）
//
// 从 v1 loiter_main.cpp 抽出、模块化、改 v2 协议。自包含：
//   - WiFi STA + NVS 凭据 + 全屏配网门户（扫描/选网/输密码/保存）
//   - MQTT（PubSubClient）+ 非阻塞重连 + LWT + 2048B buffer
//   - UID = card-<efuseMac>
//   （v3' Phase D 删 OTA：每人独特二进制，统一 OTA 失去意义；git 留底）
//
// UI 无关：配网会短暂全屏接管，结束后回调 redraw 让 Designer 重绘。
// 协议契约见 docs/mqtt-protocol.md（v2）。
#pragma once
#include <Arduino.h>

// 来自服务端的入站事件 → main.cpp 注册这些回调驱动 16 屏状态机。
struct NetCallbacks {
    // 服务端据 baked profile_id 分配的岛屿 + 文艺双语解读（island/<uid>，v3'）。
    // reason 异步生成：先空后到 → 二次 push 再调一次本回调填 reason（揭晓屏二次刷新）。
    void (*on_island)(int island, const char* name, const char* color,
                      const char* reason_en, const char* reason_cn);
    // 有人 HI 我（hi/result/<uid> event=incoming）—— 携发起者昵称 + 岛色供屏显
    void (*on_hi_incoming)(const char* requester, const char* nick, const char* color, const char* msg);
    // 我发起的 HI 有结果（event=matched/declined/expired；matched 带 partner/color/slot）
    void (*on_hi_result)(const char* event, const char* partner, const char* color, int slot);
    // 全场阶段切换（phase）
    void (*on_phase)(int phase);
    // Phase 3 个人 reading（reading/<uid>）—— 双语：EN title + lines[9] + CN title_cn/core_cn/lines_cn[9]（3 页×3 行）
    void (*on_reading)(const char* title, const char* title_cn, const char* core_cn,
                       const char* lines[9], const char* lines_cn[9]);
    // 匿名公屏（anon → 大屏为主，设备可选显示）
    void (*on_anon)(const char* text);    // 集体 JUMP 实时进度（jump/progress 广播）——窗口内正在跳的人数 count / 阈值 need
    void (*on_jump_progress)(int count, int need);    // 近距 shake 交换成功 → 复制到的对方 sig（particle/action）+ 对方昵称
    void (*on_sig_recv)(int particle, int action, const char* from_nick);
    // 配网全屏接管结束后，请 UI 重绘当前屏
    void (*redraw)();
};

// 生命周期
void net_begin(const NetCallbacks& cb);  // WiFi（splash+门户）+ MQTT 初始化，填 uid
void net_loop();                          // wifiEnsure + mqttEnsure + mqtt.loop
bool net_online();                        // wifi && mqtt 都 up
const char* net_uid();
const char* net_fw_version();

// 身份
void net_set_nick(const char* nick);
void net_set_profile(const int shape[5], const int color[5], int sig_particle, int sig_action);

// 发布（v2 topic，requester/responder 语义见 docs/mqtt-protocol.md）
void net_publish_join();
void net_publish_leave();          // C->S 主动下线/重置（服务端移除 member）
void net_publish_profile();        // C->S profile sync (avatar/sig/nick)
void net_publish_profile_request(); // C->S 拉取本机岛屿+reason（揭晓屏首发/重连兜底/Reset 重取）
void net_publish_hi_request(const char* responder_uid, const char* msg);       // 直接给 uid
void net_publish_hi_request_nick(const char* responder_nick, const char* msg); // 键入 nick，服务端解析
void net_publish_hi_respond(const char* requester_uid, bool accept);
void net_publish_hi_cancel();   // 发起方撤销未决 HI（服务端 cancel pending）
void net_publish_jump();
void net_publish_shake();    // C->S 近距 shake（move 模式晃动）—— 与集体 JUMP 是两个场景
void net_publish_anon(const char* text);
void net_publish_move(float dx, float dy);
void net_publish_sig(int particle, int action);
bool net_request_reading();   // C→S 进 Phase 3 按需请求个人 reading；返回是否真发出（MQTT up）

// 在线名册（订阅 loiter/<room>/roster 缓存，供 HI 选人/补全）
int  net_roster_count();
const char* net_roster_nick(int i);
int  net_roster_island(int i);

// /wifi 重新配网（全屏门户）
void net_open_wifi_setup();
