# AGENTS.md — Loiter

> 卡片机社交厅 · A pocket-sized social lobby for hardware hackers.

**Status**: 🟢 Sprint 7 Skill Fusion 灵魂玩法上线 ✅ (2026-06-08) · 双机 shake 配对 + fingerprint + 16 技能 + 4 大招 + omni + 固件 chip 行 + KING 皇冠 · 下次开 Phase 7.6 OTA
**Reviewers**: Polly + 小龙虾（均已批准）
**Developer**: Codex (Copilot)
**Scope 决策**: P0+P1+P2 全做；设备发参与者带走（账号持久化）；回放战报升 P1；**服务端全上云，本地 Mac hub 留为现场离线兜底**

---

## Project Overview

**Loiter** 是一套"口袋级"虚拟社交厅引擎。每位参会者拿一台 **M5Stack Cardputer-Adv**（卡片大小的 ESP32-S3 终端），连上任何 WiFi 后直接接入云端房间，在 240×135 的屏幕上以**复古绿底终端风**创建虚拟形象、收发消息、解锁成就；大屏（`loiter.polly.wang`，任何浏览器都能看）上实时展示所有人的虚拟形象、聊天气泡和入场动画，作为"上帝视角"。

名字 **Loiter**（闲逛 / 游荡）精准命名了这个产品的灵魂动作：*在一个空间里无所事事地晃悠、撞见人、随便聊两句*——这正是社交厅的本质，而不是某个功能。

**核心张力**：
- 现场的人是物理上共处一室的（社交本身已经在发生）
- Loiter 不是"取代社交"，而是**在物理空间之上叠加一层"游戏化交互层"**——让安静的人有出口、让活泼的人有舞台、让所有人有共同的话题

### 引擎 vs 实例（重要架构观）

参照 SoulPort 的"格式 vs 消费者"思路：

- **Loiter** = 通用引擎（repo 名 / 技术内部名 / MQTT namespace / 域名），**永久不变**
- **每场活动 = 一个 instance**，有自己的展示名。**首个实例叫 "GLEAM Hall"**（公司 GLEAM 组织的线下活动）
- 未来内部/外部任何 hackathon、年会、读书会都复用同一个 Loiter 引擎，只换 instance 配置（房间名 / 大屏标题 / 成就集）

> 即：Loiter 是引擎，"GLEAM Hall" 只是它的第一次部署。

---

## 背景

| 维度 | 说明 |
|------|------|
| 触发事件 | 公司 GLEAM 组织活动，已采购一批 Cardputer-Adv（首个 Loiter 实例 = "GLEAM Hall"） |
| 我的角色 | 技术开发（被 GLEAM 聘为技术人员） |
| 硬件 | M5Stack Cardputer-Adv (SKU: K132-Adv)，ESP32-S3FN8 / 8MB Flash / 240×135 LCD / 56 键键盘 / WiFi 2.4G / 1750mAh / **IR emitter only (GPIO44, 无 RX)** / ES8311 音频 / BMI270 IMU / microSD。⚠️ IR 是单向发射器（遥控器），不是收发器——见 Sprint 7 设计决策 |
| Hub | **Azure VM `20.51.201.85`**（Ubuntu 20.04，与 xhsx / SoulArena / CopilotX 同机）。本地 Mac 仅作现场离线兜底 |
| 网络 | **云端模式（当前）**：Cardputer 连任何 WiFi → 公网 1883 → VM mosquitto；大屏走 `https://loiter.polly.wang`。**现场离线兜底**：Mac 自建热点 + 本地 broker/server，改 `firmware/src/config.h` 的 `MQTT_HOST` 切回 |
| 大屏 | `loiter.polly.wang` → Cloudflare Tunnel（跑在 VM）→ `localhost:8080`。任何手机/电视/浏览器随时可访问 |
| AI 服务 | 图像：Azure AI Foundry gpt-image-1.5（dream-painter 同源凭据，存 VM `/etc/loiter/loiter.env` chmod 600）；文本：CopilotX Codex |
| 设备归属 | **活动后发参与者带走** → 账号持久化；带回家照样能玩，反正服务端 7×24 在线 |
| 设备到手 | Polly 手边 1 台 Cardputer-Adv，云端真机验证通过 ✅ |
| 时间线 | 充裕（P0+P1+P2 全做） |

---

## Architecture

```
                    ┌──────────────────────────────────────────────┐
                    │     Azure VM 20.51.201.85 (Ubuntu 20.04)      │
                    │                                                │
                    │  ┌──────────────────────────────────────────┐ │
                    │  │  mosquitto :1883 (0.0.0.0)               │ │  ← MQTT broker
                    │  │  systemd: mosquitto.service               │ │     公网入口
                    │  └──────────────────────────────────────────┘ │
                    │                                                │
                    │  ┌──────────────────────────────────────────┐ │
                    │  │  loiter.service → uvicorn :8080 (127.0.0.1)│ │  ← FastAPI
                    │  │  ├ paho-mqtt bridge ←→ 127.0.0.1:1883     │ │     房间状态权威
                    │  │  ├ WebSocket /ws (for 大屏)               │ │     成就/AI头像
                    │  │  ├ Static / (web/index.html 大屏)         │ │
                    │  │  └ /etc/loiter/loiter.env (Azure key)     │ │
                    │  └──────────────────────────────────────────┘ │
                    │                                                │
                    │  ┌──────────────────────────────────────────┐ │
                    │  │  cloudflared-loiter.service               │ │  ← named tunnel
                    │  │  tunnel `loiter` (id 9c3f05c8...) → :8080 │ │     9c3f05c8
                    │  └──────────────────────────────────────────┘ │
                    └────────────┬────────────────────┬─────────────┘
                                 │ HTTPS (443)        │ MQTT TCP (1883)
                                 │ via Cloudflare     │ 直连公网 IP
                                 ▼                    │
                ┌────────────────────────────┐        │
                │  https://loiter.polly.wang │        │
                │  (任何浏览器/手机/电视)     │        │
                └────────────────────────────┘        │
                                                       │
              ┌────────────────────────────────────────┼──────────────────────┐
              │                                        │                      │
         ┌────▼────┐                              ┌────▼────┐            ┌────▼────┐
         │Cardputer│ 家庭 WiFi / 4G / 公司 WiFi   │Cardputer│            │Cardputer│
         │  Alice  │ → 公网                       │  Bob    │            │ Carol   │
         └─────────┘                              └─────────┘            └─────────┘

  现场离线兜底（broker 上不去时）：
    改 firmware/src/config.h MQTT_HOST = Mac 局域网 IP，
    本地起 mosquitto + uvicorn，Mac 自建热点当 AP。
```

### 数据流职责

| 组件 | 输入 | 输出 | 职责 |
|------|------|------|------|
| Cardputer 固件 | 键盘 / WiFi | LCD / Speaker / MQTT | 终端 UI + 收发消息 + 显示头像 bitmap |
| Mosquitto | MQTT pub | MQTT sub fanout | 纯消息中继，不存状态 |
| Loiter Server | MQTT msg / WS / REST | MQTT msg / WS push | **唯一的房间状态权威**；持久化；成就判定；AI 头像生成 |
| Web 大屏 | WebSocket | Canvas 2D 渲染 | 在线人渲染 + 聊天气泡 + 入场动画 |
| 手机观众 | URL | 同上 | 只读模式 |

---

## Scope（已与小龙虾共识）

### 🟥 P0 — 必做 (MVP 活动当天能用)

| # | Item | 说明 |
|---|------|------|
| P0.1 | Cardputer 固件：WiFi + MQTT + 终端 UI + 文字聊天 | Arduino 框架，`PubSubClient` + `M5Cardputer` 库 |
| P0.2 | Mac 端 FastAPI：MQTT bridge + WebSocket bridge + 房间状态 | 单文件 ≤300 行 |
| P0.3 | `loiter.polly.wang` 大屏：HTML Canvas 2D + 所有头像 + 实时气泡 | 单页应用，原生 JS |
| P0.4 | Cloudflare Tunnel 配置 + 文档 | 5 分钟事 |

### 🟧 P1 — 强烈建议（时间允许）

| # | Item | 说明 |
|---|------|------|
| P1.1 | AI 头像生成（双版本：大屏 256×256 PNG + 小屏 16×16 mono） | Azure gpt-image-1.5 `/images/generations`；服务端 Pillow Floyd–Steinberg dither 成 1-bit bitmap |
| P1.2 | 成就系统（5 个内置成就） | 🥇首位加入 / 💯破百消息 / 🦉夜猫子 / 🦋社交达人 / 🧭频道环游 |
| P1.3 | 多频道切换（主厅 / 摸鱼 / 求助） | ←→ 键切换 |
| P1.4 | OTA 固件升级（`loiter.polly.wang/firmware.bin`） | 活动当天发现 bug 可推修复 |
| **P1.5** | **回放页 + 战报**（原 P2.4，Polly 提升） | SQLite 重放；现场结束生成"今日金句 TOP10"海报 |

### 🟨 P2 — 锦上添花（活动前一周有空再做）

| # | Item | 说明 |
|---|------|------|
| P2.1 | 简化版破冰任务 | 从服务器已有状态推任务卡，无需额外采集数据 |
| ~~P2.2~~ | ~~IR "碰一碰加好友"~~ | **已替换** → Sprint 7 双机同步 shake 配对（Cardputer-Adv 只有 IR 发射器，无接收器，纯 IR 双机互通硬件不可能） |
| P2.3 | 现场 AI NPC | 房间里有只 Codex，每人都能找它聊一句 |
| ~~P2.4~~ | 已升级为 **P1.5**（见上）：Polly 决定要回放/战报 | |

### 🟫 P3 — 已砍掉（write-down rationale）

| Item | 砍掉原因 |
|------|---------|
| 位置模拟 / 靠近才能聊 | Cardputer 没摇杆只有键盘，30 人挤在 30×17 格子里体验差；现场社交本身已是空间的 |
| Three.js / 2.5D 大屏 | 投入产出比低，Canvas 2D 像素风已足够帅；留到 Phase 3 |
| 实时语音 | 30 人同厅广播 WiFi 必崩；如果真要做就改成 walkie-talkie 模式 |
| 视频 | 显然不可能 |
| 单独 `avatar.polly.wang` 子域名 | 统一到 `loiter.polly.wang/create` path 即可 |
| 纯 IR 双机互碰 / IR “碰一碰加好友” | **Cardputer-Adv 只有 IR emitter（GPIO44 TX），无 IR receiver**——官方手册明确 `IR \| 1x IR emitter`，官方 `ir_nec.ino` 首行 `#define DISABLE_CODE_FOR_RECEIVER`。硬件死结 → Sprint 7 改用**双机同步 shake 配对**（仪式感反而更强：两人一起摇 vs A 单方面瓄准发射） |

---

## Tech Stack 决定

| 层 | 选型 | 备选 | 决定理由 |
|----|------|------|---------|
| Cardputer 固件 | **Arduino + `M5Cardputer`** | ESP-IDF | M5 官方库成熟，开发快；ESP-IDF 是后续优化项 |
| MQTT client (fw) | **`PubSubClient`** | `arduino-mqtt` | 老牌稳定，文档多；⚠️ 默认 buffer 256B，固件需 `MQTT_MAX_PACKET_SIZE 1024`（见 L3） |
| MQTT broker | **Mosquitto** (brew) | EMQX/NanoMQ | macOS 装 brew 一行；30 人毛毛雨 |
| Hub 服务 | **FastAPI** (Python 3.11+) | Flask/Express | 同时 REST + WebSocket + 后台 task；CopilotX 也是 FastAPI |
| MQTT client (server) | **`paho-mqtt`** | `gmqtt` | 同步 API 简单 |
| WebSocket | FastAPI 内置 | - | 已选 FastAPI 自带 |
| 持久化 | **SQLite** | Postgres | 文件存储，0 部署，回放够用 |
| Python 环境/依赖 | **`uv`** | poetry / venv+pip | Rust 写，装依赖快一个数量级；`uv run` 直接跑，无 `poetry shell` 心智负担 |
| 大屏前端 | **原生 JS + Canvas 2D** | React/Three.js | 单文件 ≤500 行，无 build step |
| AI 头像生成 | **Azure AI Foundry gpt-image-1.5** `/images/generations`（`Api-Key` header） | CopilotX / DALL-E | dream-painter 已验证；⚠️ CopilotX 无图像端点 |
| Bitmap dither | **Pillow** (Floyd-Steinberg → 1-bit) | OpenCV | Pillow 5 行代码 |
| 域名暴露 | **Cloudflare Tunnel** | ngrok | Polly 已有 `*.polly.wang` + CF 账号 |

---

## 目录结构（计划）

```
X-Workspace/loiter/
├── AGENTS.md                   ← 本文档（设计稿 + 活文档）
├── README.md                   ← 入口（指向 AGENTS.md）
├── .gitignore
│
├── server/                     ← Hub 服务（Mac M1 上运行）
│   ├── pyproject.toml
│   ├── README.md
│   ├── loiter/
│   │   ├── __init__.py
│   │   ├── main.py             ← FastAPI 入口
│   │   ├── mqtt_bridge.py      ← paho-mqtt ↔ 房间状态
│   │   ├── room.py             ← 房间状态机
│   │   ├── achievements.py     ← 成就判定
│   │   ├── avatar.py           ← CopilotX 调用 + dither
│   │   ├── ws.py               ← WebSocket fanout
│   │   ├── db.py               ← SQLite
│   │   └── config.py
│   └── tests/
│
├── firmware/                   ← Cardputer 固件（Arduino）
│   ├── README.md
│   ├── platformio.ini          ← PlatformIO 配置
│   ├── src/
│   │   ├── main.cpp
│   │   ├── ui.cpp / ui.h       ← 终端风 UI
│   │   ├── mqtt_client.cpp     ← MQTT 封装
│   │   ├── wifi_setup.cpp      ← WiFi provisioning
│   │   ├── avatar.cpp          ← 16×16 1-bit bitmap 渲染
│   │   └── config.h            ← SSID/broker hardcode (dev)
│   └── data/                   ← SPIFFS 资源
│
├── web/                        ← loiter.polly.wang 大屏 + 创建页
│   ├── index.html              ← 大屏（Canvas 2D）
│   ├── create.html             ← 头像 / 昵称创建（手机扫码）
│   ├── replay.html             ← 回放页（P2）
│   ├── assets/
│   │   └── style.css
│   └── js/
│       ├── hall.js             ← 大屏渲染逻辑
│       └── ws.js               ← WebSocket 客户端
│
└── docs/
    ├── mqtt-protocol.md        ← MQTT topic + payload schema 契约
    ├── cardputer-pinout.md     ← 硬件引脚速查
    ├── deploy.md               ← 部署 runbook（含 Cloudflare Tunnel）
    └── day-of-checklist.md     ← 活动当天检查清单
```

---

## MQTT Topic 契约（v1 — 待 Review）

> 这是固件和服务端**并行开发的 API 契约**，改动需要明确版本号。
> 本节仅为**概要**；完整 payload / QoS / LWT / Rate Limit 以 **[`docs/mqtt-protocol.md`](docs/mqtt-protocol.md)** 为准，避免两处不同步。

### 命名规范

```
loiter/<room>/<topic>/<sub>
```

- `loiter/` — 全局前缀（namespace）
- `<room>` — 房间 ID（v1 写死 `hall`，预留多房间扩展）
- `<topic>` — 主题类别
- `<sub>` — 子分类（如 channel id / user id）

### Topics

| Topic | Direction | Payload (JSON) | 说明 |
|-------|-----------|---------------|------|
| `loiter/hall/join` | C→S | `{uid, nick, ts}` | Cardputer 上线声明 |
| `loiter/hall/leave` | C→S | `{uid, ts}` | 主动下线（含 LWT） |
| `loiter/hall/msg/<channel>` | C↔S | `{uid, nick, text, ts}` | 频道消息；channel ∈ {main, fishing, help} |
| `loiter/hall/whisper/<to_uid>` | C↔S | `{from, text, ts}` | 私聊 |
| `loiter/hall/avatar/<uid>` | S→C | `{bitmap_b64, palette}` | 16×16 1-bit bitmap 下发 |
| `loiter/hall/avatar/request` | C→S | `{uid, keywords:[...]}` | 请求生成头像 |
| `loiter/hall/achievement/<uid>` | S→C | `{badge, title, desc}` | 成就解锁推送 |
| `loiter/hall/status` | S→broadcast | `{count, ts}` | 在线人数心跳（每 5s，**只发 count**；名单增量靠 join/leave） |
| `loiter/hall/sys/notice` | S→broadcast | `{text, level}` | 系统通告（如"NPC 上线了"） |
| `loiter/hall/sys/ota` | S→broadcast | `{version, url}` | OTA 推送 |
| `loiter/hall/ir/ping` | C→S | `{from_uid, to_uid, rssi}` | ~~IR "碰一碰" 上报~~ — **废弃**，改用 `pair/shake`（见下） |
| `loiter/hall/pair/intent` | C→S | `{uid, ts}` | Sprint 7：`/pair` 进入 3s “求偶模式” |
| `loiter/hall/pair/shake` | C→S | `{uid, ts, peak_g}` | Sprint 7：求偶模式期间 BMI270 检测到剧烈 shake 上报 |
| `loiter/hall/pair/result/<uid>` | S→C | `{partner_uid, partner_nick, gained:[skill,…], stack:int}` | Sprint 7：配对成功推送，告诉双方各自新获技能 + 总收集进度 |
| `loiter/hall/skill/use` | C→S | `{uid, skill, ts}` | Sprint 7：shake 随机放技能（仅能放自己拥有的） |
| `loiter/hall/skill/unlock/<uid>` | S→C | `{skill, kind: skill\|ultimate\|omni}` | Sprint 7：锁友集齐一系 / 集齐 16 全金 toast |

**Sprint 6 新增命令（走 msg 通道，服务端拦截）：**

| 命令 | 说明 |
|------|------|
| `/anon <text>` | 匿名告白（服务端剥离身份广播） |
| `/quiz` | 发起 Vix 问答赛（5 轮 AI 出题） |
| `/ans <answer>` | 问答赛抢答 |
| `/wifi` | 重新配网（扫描→选择→输入密码→NVS 持久化） |

**Sprint 7 新增命令（技能融合）：**

| 命令 | 说明 |
|------|------|
| `/pair` | 进入 3s "求偶模式"：LED 状态灯变粉色 / 屏幕提示"SHAKE TOGETHER"，期间上报 shake 。未在求偶模式的 shake 仍是 emote。 |
| `/skills` | 列出自己拥有的技能 + 总收集进度 `x/16` |

**Sprint 7 交互逻辑 — 服务端权威、不信任客户端：**
- 双方都 `/pair` 后状态机进 `WAITING_SHAKE`（3s 超时）
- 只有两个都在 `WAITING_SHAKE` 的 uid，在 **1.5s 窗口 + peak_g 都 ≥3.0g** 才算配对
- 同一对 (A,B) **整场只能配对一次**（进 `paired_with` set）→ 逆向鼓励去碰没碰过的人
- 服务端占有 ground truth 技能集，客户端锁友推送不可伪造

### Known Limitations（v1 已知取舍 — 小龙虾 review）

| # | 问题 | 决策 |
|---|------|------|
| L1 | `status` 每 5s 广播全员列表，30 人 ~2KB，超 PubSubClient 默认 256B buffer | **改增量**：`status` 只发 `count`；在线名单增删靠 `join`/`leave`。大屏走 WS 仍可拿全量 |
| L2 | 任意客户端可订阅他人 `whisper/<uid>`（Mosquitto 默认无 ACL） | 30 人内部活动**接受风险**；v2 若需隔离改服务端中转或上 broker ACL |
| L3 | `PubSubClient` 不支持 QoS 2、默认单包 ≤256B | 固件必须 `#define MQTT_MAX_PACKET_SIZE 1024`；avatar bitmap JSON ~200B 卡边界 |

> LWT / QoS / 订阅清单 / Rate Limit 完整定义见 [`docs/mqtt-protocol.md`](docs/mqtt-protocol.md)。

---

## 开发顺序（Roadmap）

> ⚠️ 时间线 TBD，待 Polly 确认活动日期后填入

### Sprint 0 — 验证（30 分钟）
- [x] `brew install mosquitto` 启动 broker（mosquitto 2.1.2 ✅）
- [x] broker 自测：sub+pub 回环通过✅
- [x] echo 固件 + platformio.ini + config.h 已写好✅
- [x] **验证用家庭 WiFi**（路径 A）：Mac+Cardputer 同一局域网，MQTT_HOST=192.168.31.80，Mac 不断网✅
- [x] 一台 Cardputer 烧 echo 固件，能 pub/sub 到 broker（心跳 `loiter/hall/echo` 每 3s 上报，nick=cardputer-0 ✅）
- [x] **通过验证 = 80% 技术风险消除** ✅ (2026-05-30)

> 网络两条路径见 [`firmware/src/config.h`](../firmware/src/config.h)：路径 A=共用家庭 WiFi（验证用，不断网）；路径 B=现场自建热点（需额外上行）。

### Sprint 1 — P0 MVP（核心链路）
- [x] Server: FastAPI + MQTT bridge + WebSocket + 房间状态机（冒烟测试通过：join/msg→房间状态机→WS 广播全链路✅）
- [x] Firmware: 终端 UI + WiFi + MQTT + 文字聊天（`loiter_main.cpp`：三段式终端 UI / Tab 切频道 / `/nick` 改名；真机烧录验证 join+收发✅ 2026-05-30）
  - [x] 非阻塞 MQTT/WiFi 重连（`millis()` 节流，loop 零 while/delay）— review #1 ✅
  - [x] ArduinoJson 构造/解析 payload（自动转义，防注入坏包）— review #2 ✅
- [x] Web: Canvas 2D 大屏 + 实时气泡（`web/index.html`：像素头像 identicon / 闲逛漂移 / 气泡 / 终端日志 / CRT 滤镜 / WS 自动重连；mosquitto_pub 灌数据验证✅ 2026-05-30）
- [x] Cloudflare Tunnel → `loiter.polly.wang`（命名 tunnel `loiter` id `9c3f05c8`，`~/.cloudflared/config.yml` ingress → localhost:8080；公网 `https://loiter.polly.wang/healthz` 验证通过✅ 2026-05-30）
- [ ] 端到端跑通 3 台 Cardputer 互聊 + 大屏可见（1 台真机已全验：打字发消息 / `/nick` 改名 / Tab 切频道均正常✅ 2026-05-30；**多机互聊受限于手边仅 1 台设备，待补设备后实测**）

### Sprint 2 — P1 增强（AI 头像 + 成就 + 频道 + OTA）
- [x] Azure gpt-image-1.5 集成 + Pillow dither（`avatar.py`：keywords→prompt→1024 PNG→大屏 256 彩色 PNG（WS）+ Cardputer 16×16 1-bit bitmap 32B（S→`avatar/<uid>` QoS1）；`/face <keywords>` 触发；线程池避免阻塞 MQTT 线程；凭据走 env fail-closed；真机 Azure 生成端到端验证 + 真机 /face→头像 toast 渲染✅ 2026-05-31）
- [x] 5 个成就规则 + Cardputer toast（`achievements.py` 纯内存规则引擎；首位加入/破百消息/夜猫子/社交达人/频道环游；Server→`achievement/<uid>` QoS1 下发 + WS 大屏 toast；真机 toast 渲染验证✅ 2026-05-31）
- [x] 多频道切换 — 大屏可视化（节点头像外圈 channel ring + 昵称/气泡频道色 + 顶部 HUD `#MAIN/#FISHING/#HELP` 计数徽章；`Member.current_channel` 落 snapshot 让重连不丢色；CHANNEL_COLORS 集中管理便于 P2 redesign 换色）✅ 2026-05-31
- [ ] OTA framework

### Sprint 2.5 — 云端部署（紧急插队）
- [x] Azure VM 装 mosquitto + 改 `/etc/mosquitto/conf.d/loiter.conf` 监听 0.0.0.0:1883✅ 2026-05-31
- [x] rsync server + web 到 `/home/azureuser/GitHub_Workspace/loiter/`，uv venv 装依赖✅
- [x] Azure key 走 `/etc/loiter/loiter.env` chmod 600（fail-closed），systemd EnvironmentFile 注入✅
- [x] systemd: `loiter.service` (uvicorn :8080) + `cloudflared-loiter.service` (named tunnel)✅
- [x] Azure NSG 开 1883 入站（Polly 在 Portal 加 AllowMQTT_Loiter rule）✅
- [x] WS keepalive：服务端 20s 主动 ping，规避 Cloudflare tunnel idle timeout（之前大屏闪空根因）✅
- [x] 头像快照持久化：`Member.png_b64` 字段 + `applySnapshot` 恢复 → 大屏 WS 重连不丢头像✅
- [x] auto-join 兜底：msg/avatar 收到未知 uid 自动幂等补 join，避免服务端重启或 broker 切换后真机不重发 join 导致大屏看不到人✅
- [x] 固件 `config.h` MQTT_HOST → `20.51.201.85`，真机重烧 → 公网链路全通✅ 2026-05-31
- [x] 加 `mqtt.polly.wang` DNS A 记录（Cloudflare 灰云 / DNS only）→ 固件改用域名，未来换 VM 只改 DNS 不重烧✅ 2026-05-31
- [x] **mosquitto 密码鉴权**（`/etc/mosquitto/passwd` chmod 640 root:mosquitto；`allow_anonymous false`；固件 `mqtt.connect(uid, MQTT_USER, MQTT_PASS, will...)`；服务端 `paho.username_pw_set` 由 `LOITER_MQTT_USER/PASS` env 注入；用户名 `loiter`，密码 `polly-loiter-xxx`，烧固件里 + 存 `/etc/loiter/loiter.env`，**`config.h` gitignored**；公网无密码被拒、带密码通过双验证）✅ 2026-05-31
- [ ] NSG 1883 源 IP 收紧（可选；password 已足够 30 人场景。出门 IP 不固定时不强求）

### Sprint 3 — P2 锦上添花
- [x] `/emote` 表情动作（5 种明亮草原特效：🌸 bloom 樱花爆发 / ✨ spark 金色烟花 / 🍃 wind 清风拂叶 / 🦊 fox 狐火旋转 / 🌈 rain 彩虹弧；全链路 firmware→server→大屏 Canvas 粒子；3s 冷却 rate-limit；摇一摇触发随机 emote（BMI270 >2.5g 检测 + `esp_random()`）；旧版暗黑水墨 emote 已替换）✅ 2026-06-01
- [x] AI NPC Vix 狐灵（`/ask <问题>` → CopilotX Codex → 话多俏皮小狐仙风回复；gpt-image 生成 Q 版狐灵头像 `npc_vix.png`（去白底透明）；uid `npc-vix`；大屏漂浮仙灵渲染 + 觉醒态旋转光环 + 尾迹花粉粒子）✅ 2026-06-01
- [x] 破冰任务卡（`/task` 领取随机任务 → 12 种任务池覆盖全部功能 → 自动检测完成 → 大屏卷轴展开+燃尽动画；任务文案已改英文匹配 Cardputer 键盘）✅ 2026-05-31
- [ ] IR 碰一碰
- [ ] 回放页

### Sprint 4 — 大屏重制 + 体感操控（阴阳师级视觉升级）
- [x] 大屏重写：阴阳师级国风仙侠视觉（程序化渲染月夜山水背景：深靛夜空 + 120 颗明灭星辰 + 月亮三层光晕 + 三层远山剪影 + 地面雾 + 6 团漂浮薄雾 + 22 只脉动萤火虫 + 贝塞尔曲线樱花花瓣；漆木横梁顶栏 + 竹简底栏 + 毛笔书法品牌 "闲逛堂"；旧版备份 `index_v1_backup.html`）✅ 2026-05-31
- [x] AI 头像风格：水墨 → Q版国风 chibi（`avatar.py` prompt 改为 "Cute chibi Q版 + 汉服古风 + 暗底金光"；bitmap 去掉 `ImageOps.invert`，适配暗底亮主体）✅ 2026-05-31
- [x] RPG 式角色展示（去掉卡片边框 → 80px 角色直接站在场景；脚下椭圆阴影；稀有度用光环表达：R 蓝晕/SR 紫晕+粒子/SSR 旋转金芒+彩虹光；入场金光爆发 24 粒子）✅ 2026-05-31
- [x] 多行气泡（最大 280px 自动折行，最多 3 行 80 字符，RPG 对话框风格）✅ 2026-05-31
- [x] 成就法阵（全屏半透明遮罩 + Canvas 旋转六芒星阵 + 24 刻度外圈/12 刻度中圈/双层六芒星/中心径向光，4s 渐入渐出）✅ 2026-05-31
- [x] BMI270 IMU 体感操控（固件 500ms 采样加速度计 → 死区 0.15g + 量化 0.05 步进 → `loiter/hall/move` MQTT QoS0 → 服务端 250ms 限流中继 WS → 大屏 2px/帧平滑移动；平放=微小漂移，倾斜=角色朝该方向走；编译+真机烧录验证通过）✅ 2026-05-31

### Sprint 5 — 大屏视觉重构 + 新 NPC + 硬件交互（明亮梦幻国风）
- [x] 大屏背景重构：暗黑仙侠 → 明亮梦幻国风（gpt-image-1.5 生成提瓦特草原风宽幅背景 `bg_meadow.jpg`（418KB JPEG）；Canvas cover-fit 铺底 + 程序化樱花/花粉/云缕叠层动效；纯色过渡避免加载闪烁；旧版备份 `index_v2_xianxia_backup.html`）✅ 2026-06-01
- [x] UI 亮色主题（顶栏/日志：暖白奶油底 + `backdrop-filter: blur(8px)`；文字深棕 `#3a3530`；气泡奶白底 + 频道色边框；暗角极淡）✅ 2026-06-01
- [x] NPC 月灵 → Vix 狐灵（uid `npc-vix`；Q 版金毛狐灵头像 gpt-image 生成 + Pillow 去白底透明；system prompt 改为话多俏皮小狐仙；降级回复改英文狐狸风；大屏漂浮仙灵 + 觉醒态旋转光环 + 尾迹花粉）✅ 2026-06-01
- [x] Emote 重制：水墨 5 式 → 草原 5 式（bloom 樱花爆发 / spark 金色烟花 / wind 清风拂叶 / fox 狐火旋转 / rain 彩虹弧；全链路 server+frontend+firmware 更新）✅ 2026-06-01
- [x] 摇一摇随机 emote（BMI270 检测 >2.5g 加速度 → `esp_random()` 随机选 1/5 emote → 自动发送；3s 冷却与服务端对齐；编译+真机烧录验证通过）✅ 2026-06-01
- [x] AI 头像 prompt 适配亮底（`avatar.py` prompt 改为 bright/transparent background + cute chibi style）✅ 2026-06-01

### Sprint 6 — 互动增强三件套（问答赛 + 匿名告白 + 音效）
- [x] 匿名告白墙 `/anon`（服务端剥离身份 + 5s 冷却 → MQTT 广播 `uid=anon nick=???` + WS `anon_msg`；大屏粉紫渐变气泡 + 淡入淡出动画；固件 `/anon <msg>` 命令）✅ 2026-06-01
- [x] Vix 问答赛 `/quiz`（服务端状态机：5 轮 × CopilotX 生成题目 → 20s 抢答 → 10s 提示 → 计分；降级硬编码题库；大屏右侧计分板面板 + 实时排行；固件 `/quiz` 发起 + `/ans <answer>` 抢答；正确答案触发 spark emote）✅ 2026-06-01
- [x] 事件音效 ES8311 → **已删除**（`Speaker.tone()` 在 Cardputer-Adv 上导致卡死/MQTT 掉线，I2S task 与 PubSubClient 冲突；Sprint 6 踩坑后彻底移除，等硬件隔离测试后再考虑）
- [x] MQTT 重连加固（keepalive 30→60s；重连间隔 3→5s；重连前 `mqtt.disconnect()` 避免 broker "already connected" 循环踢连）✅ 2026-06-01
- [x] 大屏全英文化 + LOITER 品牌（去掉「闲逛堂」；顶栏/日志/成就/任务/连接状态全改英文；品牌字体改衬线体加粗）✅ 2026-06-01
- [x] 固件 UI 打磨（启动画面 1.5s 金色 LOITER logo；彩色消息系统：6 色角色区分 self/main/fishing/help/NPC/anon；状态栏频道色下划线 + 绿红圆点连接灯；输入栏跟随频道色；全英文系统消息）✅ 2026-06-01
- [x] `/wifi` 设备端配网（WiFi.scanNetworks → 240×135 SSID 列表 Tab/Backspace 上下选 → QWERTY 输入密码 → 8s 连接 → Preferences NVS 持久化；启动 5s 连不上自动进配网；`/wifi` 命令随时重配；手机热点需 2.4GHz）✅ 2026-06-01

### Sprint 7 — Skill Fusion（技能融合 · 双机灵魂玩法）

**核心机制**：每人入场随机获得 1-2 个技能；与他人 **`/pair` + 双机贴合 shake** 互换全部技能 → 同一对**只能配对一次** → 强制满场跑动撞新人 → 集齐一系解锁大招、集齐全 16 个解锁"万象归一" SSR。

**16 个技能（4 系 × 4）**：
- 🌿 自然：bloom / wind / rain / leaf → 大招 `gaia`（大地共鸣，全屏花瓣雨）
- 🔥 火：spark / fox / flame / comet → 大招 `solar`（烈日当空，热浪扭曲）
- 💧 水：wave / bubble / mist / tide → 大招 `dragon`（水龙腾，蜿蜒水龙绕屏）
- 🌟 光：star / aurora / lightning / halo → 大招 `galaxy`（星河倒挂，背景换星河 5s）
- 集齐 16 全部 → `omni`（万象归一）+ "OMNISCIENT" SSR 成就

#### Phase 7.0 — 双机 shake 配对核心链路 + fingerprint + proximity gate ✅ 2026-06-08
- [x] **Server**: `pair_engine.py` 状态机（WAITING_SHAKE 3s 窗口；1.5s pair 容差；peak_g ≥3.0g；同对幂等；已 paired 拒绝）
- [x] **Server**: `skills.py` 4 系 × 4 = 16 技能常量 + `Member.skills/starter_skills/paired_with/contributed_to`
- [x] **MQTT topics**: `pair/intent` / `pair/shake` / `pair/result/<uid>` (phase=armed/fused/rejected/state) 接入 mqtt_bridge
- [x] **Firmware**: `/pair` 命令进入 PAIRING_MODE 3s；BMI270 阈值 3.0g；1.2s 采样窗口收集 shake fingerprint (peaks/rhythm_ms/energy)；上报 `pair/shake`；收到 `pair/result` toast
- [x] **fingerprint 物理近距校验** — 算法 = 0.4×peaks + 0.4×rhythm + 0.2×energy；阈值 0.55；贴一起摇 ~0.79 / 分开摇 ~0.20-0.40 实测精准区分
- [x] **proximity gate** — 大屏角色距离 ≤600px 才能配（防"隔着地球摇"）；fingerprint 之前先校验，失败 toast "WALK CLOSER"
- [x] **求偶/采样期间禁用倾斜移动** — 避免"摇出去"导致 chip 行错位
- [x] **真机双机验证**：实测 fused / fingerprint-rejected / already-paired 三类 case 全过 ✅

#### Phase 7.1+7.2 — 大屏 skill chips + 5s 融合演出（B 方案） ✅ 2026-06-08
- [x] 入场随机 1-2 个 starter 写入 `Member.starter_skills`（永远冻结）+ `Member.skills`
- [x] 大屏头像下方常驻 skill chip 行（16 格 4 系颜色，已拥有亮、未拥有灰）+ 进度 `7/16` + `📨N` 贡献徽章
- [x] 大屏排行榜按 `len(skills)` 排序
- [x] 收到 `pair_fused` WS → 5s 演出：approach 1.5s → hold 0.5s 光柱 → burst 1.5s 技能 emoji 飞向 partner → release 1.5s 复位
- [x] 顶部粉紫 toast `Alice ⇄ Bob` + 双方 gained emoji + ⚡ULT / 🌌OMNI 高亮
- [x] **A+ 改进**：starter-only 共享（集齐者不再是福音站）+ 贡献排行（每给一人 starter, contributedCount+1）+ 👑 KING 皇冠（omni 玩家头顶金色脉动）

#### Phase 7.3 — 11 个新 emote + 4 系大招 + omni ✅ 2026-06-08
- [x] 11 个新 emote Canvas 粒子：leaf 落叶 / flame 火舌 / comet 流星 / wave 涟漪 / bubble 气泡 / mist 雾 / tide 水滴 / star 星辰 / aurora 极光 / lightning 闪电 / halo 光环
- [x] 4 个大招（gaia 全屏花瓣雨 / solar 暖色 tint + 火舌 / dragon 多波同心圆 + 弧形粒子 / galaxy 星河 + 流星）持续 5s
- [x] omni 万象归一 8s 终极特效（多色彩虹渐变 tint + 花瓣 + 星点混合）
- [x] 服务端集齐检测 → fused 演出 5s 后自动播放对应大招/omni
- [x] 服务端 `VALID_EMOTES` 扩展到 21 个（16 skill + 4 ult + omni）

#### Phase 7.4 — 固件 UI 收口 ✅ 2026-06-08
- [x] 顶栏下方 8px chip 行：16 个色块按 4 系着色，starter 加白框，旁边 `x/16` 进度
- [x] `/skills` 命令本地渲染（4 行按系分组，starter 带 `*`）
- [x] shake 放技能从 `gMySkills[]` 随机抽（只能放自己拥有的）
- [x] 服务端 `phase=state` 推送：入场/fuse/reset 全量同步 skill state 到固件
- [x] PAIRING_MODE 状态栏闪粉色 + 倒计时
- [x] `/reset` admin 命令（admin 工具，清场重测）

#### Phase 7.5 — 战报扩展（P1.5 兑现，活动后做）
- [ ] SQLite `pairs` 表 + `skill_unlocks` 表 持久化
- [ ] 个人战报：你配对了 N 人 / 解锁了 M 个 / 最佳 CP 是谁（互相是对方首位 pair）
- [ ] 全场战报：技能传播链路图（D3 force-directed）/ 收集王 / 万象归一首位

#### Phase 7.6 — OTA 固件升级（下次会话开干 ⏳）
- [ ] Server: `firmware.bin` 上传到 `/static/firmware.bin` + `/firmware/manifest.json` 返回 `{version, sha256, url, size}`
- [ ] Firmware: 启动 / 收到 `sys/ota` 时 → HTTP GET manifest → 比 chipid 黑白名单 + 版本号 → ESP32 OTA partition 烧写 → reset
- [ ] Server admin: `mosquitto_pub -t loiter/hall/sys/ota -m '{"version":"x.y.z","url":"https://.../firmware.bin","sha256":"...","targets":"all|<uid>"}'`
- [ ] 安全 check：sha256 比对 + 烧写失败回滚（ESP32 双 partition 天然支持）
- [ ] UI: Cardputer 进度条 `[████░░░░] 50%` + 完成后 reset 重连

### Day-of
- [ ] 提前 1h 到场 → 部署 + 烟雾测试
- [ ] 二维码海报：扫码进 SSID + 扫码进观众席
- [ ] 备用方案：broker 挂了 → Cardputer 自动降级显示 "OFFLINE MODE"

---

## Open Questions（待 Reviewers 决定）

### ❓ Q1: 中文输入怎么办？— **小龙虾提出**

Cardputer 是 QWERTY 键盘，**没有原生中文输入**。三个方案：

| 方案 | 工作量 | 体验 | 备注 |
|------|-------|------|------|
| **A. 只支持英文 + emoji 短码**（如 `:laugh:` `:wave:`） | 小（1h） | 极客感强，符合 GLEAM 人群 | **推荐** |
| B. 拼音原样发（不转换） | 0 | "ni hao" 也能聊，复古 BBS 风 | 可作 A 的备选 |
| C. 拼音 → 汉字 IME | 大（2-3d） + 词库 SPIFFS 占 1-2MB | 体验最好 | **不推荐**（工作量爆炸） |

**Reviewer 共识**：Polly + 小龙虾 都投 **A**（英文 + emoji 短码，如 `:wave:` → 👋）。✅ 定稿。

### ✅ Q2: 活动日期 / 时间线？— **已定**

> 不知道是周末 hack 还是两周正经开发，会决定 P1/P2 砍多少。

**Polly**：时间充裕，**P0+P1+P2 全做**——"相信我们 AI 天团的战斗力！"✅

### ❓ Q3: 现场预计人数？

| 人数 | 网络方案 |
|------|---------|
| ≤15 | macOS Internet Sharing 直接当 AP |
| 15–30 | **必须**外加一个家用路由器（百元级） |
| >30 | 需要分多个 Cardputer 进 broker，超出 v1 设计 |

**Polly**：**≤15 人** → 直接用 macOS Internet Sharing 当 AP，不需额外路由器。✅

### ❓ Q4: Cardputer 设备所有权？

- 活动结束后 Cardputer 归还公司、归个人、还是发给参与者？
- 影响"账号是否持久化"和"头像数据是否保留"的设计。

**Polly**：**发给参与者带走** → 账号持久化：uid 绑设备 chipid，头像+成就存 SQLite 重启不丢；可做"带回家继续玩"的 standalone 模式（v2）。✅

### ❓ Q5: 现场是否需要回放 / 战报？

- 如果需要 → P2.4 升级为 P1
- 现场结束打印 "今日金句 TOP10" 海报会非常有仪式感

**Polly**：**需要**——很有仪式感 → 回放/战报已升为 **P1.5**，进核心交付。✅

---

## Risks（已识别）

| Risk | 概率 | 影响 | 缓解 |
|------|-----|------|------|
| macOS Internet Sharing 客户端数超限 | 中 | 高 | >15 人必须用路由器，活动前 1 周决定 |
| 会场 WiFi 干扰 2.4G | 高 | 中 | 选偏冷信道（11/1/6），关闭 Mac 蓝牙 |
| Cardputer 电池只够 4-6 小时 | 中 | 中 | 准备 USB-C 充电线 + 充电宝 ×3 |
| MQTT broker 在 Mac 上被防火墙拦 | 高 | 高 | 系统设置先放行 1883；带备用 broker `mosquitto -c` |
| Cloudflare Tunnel 抖动 | 低 | 中 | 大屏可降级到 `http://localhost:8080` |
| CopilotX 头像生成超时 | 中 | 低 | 设 30s 超时 + 降级到预设头像池 |
| 现场 Demo 翻车 | 中 | 高 | 提前 1h 烟雾测试 + 视频备份 |

---

## Commands

### 云端运维（VM 20.51.201.85）

```bash
# SSH（用 SoulArena 同一份 config）
ssh -F X-Workspace/SoulArena/ssh.config Azure-Server

# 三个 systemd 服务
sudo systemctl status mosquitto                # MQTT broker，0.0.0.0:1883
sudo systemctl status loiter                   # FastAPI uvicorn 127.0.0.1:8080
sudo systemctl status cloudflared-loiter       # named tunnel → loiter.polly.wang

sudo journalctl -u loiter -f                   # 看实时日志（join/msg/avatar/achievement）
sudo journalctl -u cloudflared-loiter -f       # tunnel 连接状态

# 重启（改完代码用）
sudo systemctl restart loiter

# 同步代码（本地 → VM）
rsync -avz --exclude='__pycache__' --exclude='.venv' \
  -e "ssh -F X-Workspace/SoulArena/ssh.config" \
  X-Workspace/loiter/server/ Azure-Server:/home/azureuser/GitHub_Workspace/loiter/server/
rsync -avz \
  -e "ssh -F X-Workspace/SoulArena/ssh.config" \
  X-Workspace/loiter/web/ Azure-Server:/home/azureuser/GitHub_Workspace/loiter/web/

# 公网烟雾测试
curl -s https://loiter.polly.wang/healthz
curl -s https://loiter.polly.wang/api/snapshot | python3 -m json.tool
mosquitto_pub -h 20.51.201.85 -p 1883 -t loiter/hall/sys/notice \
  -m '{"text":"hello from anywhere","level":"info"}'
```

### Firmware（PlatformIO）

```bash
cd X-Workspace/loiter/firmware
# config.h 默认 MQTT_HOST=20.51.201.85（云端模式）
pio run -e loiter -t upload                    # 编译 + 烧录
pio device monitor                             # 串口监控（可选）
```

### 现场离线兜底（broker 上不去时）

```bash
# 1. 改 firmware/src/config.h:
#    MQTT_HOST = Mac 局域网 IP（如 192.168.31.80 或 Internet Sharing 的 192.168.2.1）
# 2. Mac 上拉起本地服务
brew services start mosquitto
cd X-Workspace/loiter/server && uv sync
set -a && source ../../../.Codex/skills/dream-painter/scripts/config.env && set +a
LOITER_MQTT_HOST=127.0.0.1 uv run uvicorn loiter.main:app --host 0.0.0.0 --port 8080
# 3. Cardputer 重烧 → 走 LAN broker
```

---

## 踩坑 & 提速（PlatformIO 工具链）

> 首次 `pio run` 要拉 framework(235MB)/toolchain/esptoolpy 等包，国内极慢。验证阶段实测踩了两个坑，记下来。

### 1. 包下载慢 → 用 aria2 多线程，**不是**开代理

实测单连接都被上游镜像限速，开不开代理都没用；**多线程并发**才是解药：

| 方式 | 速度 |
|------|------|
| 代理单线程 | ~33 KB/s |
| 直连单线程 | ~19 KB/s（更慢） |
| **aria2 -x16 直连** | **1.6–2.1 MB/s（快 50–100×）** |

**手动下包套路**（适用任何 pio 卡住的大包）：

```bash
# 1. 查真实 download_url + sha256（registry API）
curl -s "https://api.registry.platformio.org/v3/packages/platformio/tool/<包名>" \
  | python3 -c "import sys,json;d=json.load(sys.stdin);v=d['version'];f=v['files'][0];print('id',d['id']);print(f['download_url']);print(f['checksum']['sha256'])"

# 2. aria2 多线程下载（务必 -u 掉代理，代理反而慢）
cd ~/.platformio/.cache/downloads
env -u https_proxy -u http_proxy -u all_proxy \
  aria2c -x16 -s16 -k1M -o pkg.tar.gz "<download_url>"

# 3. 校验 sha256
echo "<sha256>  pkg.tar.gz" | shasum -a 256 -c -

# 4. 解压到 packages/ + 手写 .piopm（让 pio 认作已装、跳过重下）
DST=~/.platformio/packages/<包名>; mkdir -p "$DST"; tar xzf pkg.tar.gz -C "$DST"
# .piopm 格式参考已装包：{"type":"tool","name":"<包名>","version":"<ver>",
#   "spec":{"owner":"platformio","id":<id>,"name":"<包名>","requirements":null,"uri":null}}
```

> ⚠️ zsh 坑：`rm -f file*` 若无匹配会整行中止（`no matches found`），别用裸通配符。

### 2. esptoolpy 装依赖报 `No module named pip`

`uv tool install platformio` 装出来的 pio，其内嵌 Python **没有 pip**，导致 esptoolpy 装 `cryptography/ecdsa/...` 时炸。补 pip + 走清华镜像：

```bash
PIOPY=~/.local/share/uv/tools/platformio/bin/python
"$PIOPY" -m ensurepip --upgrade                          # 补 pip
PIP_INDEX_URL=https://pypi.tuna.tsinghua.edu.cn/simple \
  env -u https_proxy -u http_proxy -u all_proxy pio run -e sprint0   # PyPI 走镜像、不走代理
```

> uv / PyPI 用 `UV_DEFAULT_INDEX` 或 `PIP_INDEX_URL` 指向清华镜像即可，**不需要代理**。

---

## 待办：项目立项后

- [ ] Reviewer 批准本设计 → 把 `loiter` 加到根 `AGENTS.md` 的 Project Navigation Map 和 Architecture 图里
- [ ] 创建 GitHub repo（如需公开）
- [ ] 申请 `loiter.polly.wang` DNS 记录 + Cloudflare Tunnel
- [ ] 提前 2 周拿到 1 台 Cardputer 做 Sprint 0 验证

---

## 文档维护原则

本 AGENTS.md 是**活文档**，跟 SoulPort/CopilotX 的 AGENTS.md 同一风格：
- 不写 changelog（git log 自带）
- 不写版本号（git tag 自带）
- 只写"如何做 / 当前状态 / 决策依据"
- 决策变更 = 直接改这里 + 在 commit message 里说明 why

---

> Reviewers: 看完请直接在本文档上批注（diff/comment 均可），或在对话里说"P0.X 应该改成…"。
> 我会按 review 反馈迭代下一版。
