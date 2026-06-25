# MQTT / WebSocket Protocol — Loiter v2 · Islands of Color

> **Cardputer 固件** ↔ **Hub Server** ↔ **大屏 Web** 的三方 API 契约。
> 任何 schema 修改必须同步通知三位 reviewer（Polly / 小龙虾 / Codex）。
> 旧 v1 契约（聊天厅/技能/NPC）归档在 [`mqtt-protocol-v1.md`](mqtt-protocol-v1.md)，**已弃用**。

**Protocol**: v2 (2026-06-14)
**Broker**: Mosquitto，port 1883，密码鉴权（`allow_anonymous false`）
**权威**: Server 是唯一房间状态权威；固件/大屏不信任彼此，只信 Server。

---

## 通用约定

### 命名空间
```
loiter/<room>/<topic>[/<sub>]
```
- `<room>` 固定 `hall`（Server `config.topic(*parts)` 统一构造）
- Payload 一律 **UTF-8 JSON**

### 共用字段
| 字段 | 类型 | 说明 |
|------|------|------|
| `uid` | string | 设备唯一 ID = `card-<chipid>`，固件首启生成存 NVS |
| `nick` | string (≤12 ASCII) | 显示昵称 |
| `ts` | uint64 | Unix epoch ms (UTC) |
| `island` | int 0-5 | 岛屿索引（EMBER0/HEARTH1/SPARK2/GROVE3/TIDE4/MIST5） |
| `color` | string `#RRGGBB` | 岛屿色 |

### 6 座岛屿（Server `islands/assignment.py` 权威）
| idx | name | color |
|-----|------|-------|
| 0 | EMBER | `#e84d3c` |
| 1 | HEARTH | `#ff9f43` |
| 2 | SPARK | `#f9ca24` |
| 3 | GROVE | `#6ab04c` |
| 4 | TIDE | `#4a6fa5` |
| 5 | MIST | `#5f27cd` |

---

## MQTT Topics

### 传输层（沿用 v1 已验证资产）

| Topic | Dir | QoS | Payload | 说明 |
|-------|-----|-----|---------|------|
| `loiter/hall/join` | C→S | 1 | `{uid, nick}` | 上线声明 |
| `loiter/hall/profile` | C→S | 1 | `{uid,nick,avatar:{shape[5],color[5]},sig:{particle,action}}` | 形象/签名实时同步 |
| `loiter/hall/leave` | C→S | 1 | `{uid, reason?}` | 主动下线 / LWT |
| `loiter/hall/move` | C→S | 0 | `{uid, dx, dy}` | IMU 倾斜，dx/dy ∈ [-1,1]，服务端 250ms 限流 |
| `loiter/hall/status` | S→all | 0 | `{count, ts}` | 在线人数心跳（5s） |
| `loiter/hall/sys/notice` | S→all | 1 | `{text, level}` | 系统通告 |

### 玩法层（v2 新 · Islands of Color）

| Topic | Dir | QoS | Payload | 说明 |
|-------|-----|-----|---------|------|
| `loiter/hall/quiz/done` | C→S | 1 | `{uid, answers:[int,int,int]}` | quiz 完成 → Server 分配岛屿 |
| `loiter/hall/island/<uid>` | S→C | 1 | `{island, name, color}` | 定向告诉设备它的岛屿 |
| `loiter/hall/hi/request` | C→S | 1 | `{requester, responder?|responder_nick?, msg?}` | HI 发起（responder 给 uid 或 nick 二选一；msg ≤15 char 可选） |
| `loiter/hall/hi/respond` | C→S | 1 | `{requester, responder, accept}` | responder 回应 requester |
| `loiter/hall/hi/cancel` | C→S | 1 | `{requester}` | 发起方撤销未决 HI（server cancel pending） |
| `loiter/hall/hi/result/<uid>` | S→C | 1 | 见下 | 定向推送 HI 进展 |
| `loiter/hall/roster` | S→all | 1+retain | `{members:[{uid,nick,island}], ts}` | 在线名册（仅已分岛成员），设备缓存用于 `HI <nick>` 补全 |
| `loiter/hall/jump` | C→S | 1 | `{uid}` | 集体跳，10s 窗口聚合 ≥5 人触发 |
| `loiter/hall/jump/progress` | S→all | 0 | `{count, need}` | 实时跳跃进度（窗口内人数 count / 阈值 need）→ 设备 P2-07 显真 N/need（替换本地假倒计时） |
| `loiter/hall/anon` | C→S | 1 | `{uid, text}` | 匿名公屏（≤30 char，每人 60s 1 条） |
| `loiter/hall/sig` | C→S | 1 | `{uid, particle, action}` | `/sig` 即时动作广播 |
| `loiter/hall/phase` | S→all | 1+retain | `{phase: 1\|2\|3}` | host 控制全场阶段切换 |
| `loiter/hall/reading/request` | C→S | 1 | `{uid}` | 设备进 Phase 3 按需请求"今天的你" |
| `loiter/hall/reading/<uid>` | S→C | 1 | 见下 | Phase 3 AI 个人解读（B-lite 双语）|

#### `reading/<uid>` payload（B-lite 双语）
```json
{ "title": "ROOTED SPARK",          // EN 身份标签（旧字段，向后兼容）
  "title_cn": "扈根的火花",          // CN 副标题 ≤6 汉字
  "core_cn": "你从森林走向了火光",   // CN 核心句 ≤12 汉字
  "lines": ["l1", "l2", "l3"],       // EN 3 行短诗（旧字段）
  "lines_cn": ["c1","c2","c3","c4","c5","c6"],  // CN 6 行短句，每行 ≤12 汉字
  "island": 3, "color": "#6ab04c", "spectrum": [...] }
```
> 固件：P3-02 用 `title`/`title_cn`/`core_cn`；P3-03 三页每页 `lines[page]` + `lines_cn[page*2..+1]`。
> **旧 `title`/`lines` 保留** → 大屏 `reading_reveal` 不改不崩（向后兼容）。设备不读 `spectrum`/`color`（色块读本地 `g_collection`）。

#### HI 字段语义（钉死，防歧义 — review 小龙虾#2 / Codex）
- **`requester`** = 发起 HI 的人（被回应的人）
- **`responder`** = 被邀请回应的人（当前发送 respond 的人）
- 设备可发 `responder`（uid）或 `responder_nick`（键入 `HI ALICE`），服务端权威解析 nick→uid（大小写不敏 first-match）
- 双方必须都已分岛（`island≥0`）才受理 HI request（未分岛静默丢弃）
- 流程：A `hi/request{requester:A, responder:B}` → Server 给 B 发 `hi/result/B {event:"incoming", requester:A}` → B `hi/respond{requester:A, responder:B, accept:true}` → 握手成立
- **两条消息里 `requester`/`responder` 字段值始终不变**（不随发送方翻转），消除"谁是 from"的歧义

#### `hi/result/<uid>` 的 event 类型
| event | 何时 | 额外字段 |
|-------|------|---------|
| `incoming` | 有人 HI 你 | `requester`, `requester_nick`, `color`（发起者岛色）, `msg` |
| `matched` | 握手成功 | `partner`（对方昵称）, `color`（对方岛色）, `slot`（换入第几格，`-1`=同岛/重复/满格的共鸣不加色） |
| `declined` | 对方拒绝 | `partner` |
| `expired` | pending 30s 超时 | （无） |

---

## WebSocket（Server → 大屏，只读单向）

连接 `/ws`，建连即收一帧 `{type:"snapshot", ...room.snapshot()}`。Server 每 20s 对空闲连接发 `{type:"ping"}`（规避 Cloudflare tunnel idle 杀连，前端忽略未知 type）。

### snapshot 成员结构
```json
{ "uid","nick","joined_at","island","island_color",
  "spectrum":["#f9ca24",null,null,null,null], "hi_count",
  "reading": null,   // Phase 3 已生成的 reading（dict|null）→ 大屏重连不掉回 fake
  "x","y" }
```
> `island=-1` 未分配时 `island_color="#888888"`（fallback，不空转 CSS）。
> `x/y` 是**服务端归一化坐标**（逻辑画布 1920×1080）；大屏地图 2752×1536，自行映射。
> snapshot 帧还带顶层 `stage:{dim,reveal,photo}`（host 控场持久态，内存态、server 重启清零）→ 晚到/刷新的大屏据此追赶当前 reveal/dim/photo。

### 事件类型（server→大屏）
| type | 触发 | 关键字段 |
|------|------|---------|
| `join` | 上线 | `uid, nick, island, island_color, count` |
| `profile_update` | 昵称/换装/sig 更新 | `uid, nick, avatar, sig_particle, sig_action` |
| `leave` | 下线 | `uid, count` |
| `move` | IMU | `uid, dx, dy` |
| `status` | 5s 心跳 | `count` |
| `island_assign` | quiz 完成登岛 | `uid, island, island_color, spectrum` |
| `hi_arc` | HI 握手成功（P3） | `a, b`（双方 uid）, `a_spectrum, b_spectrum`（换色后各自 5 格，大屏即时刷新）（跨海彩虹弧） |
| `jump` | 单人 JUMP | `uid`（小人弹跳）, `count, need`（当前窗口内一起跳的人数 / 阈值 → 大屏实时人数胶囊） |
| `jump_burst` | ≥5 人 JUMP | `count` |
| `anon_msg` | 匿名公屏 | `text`（已剥离身份） |
| `sig_cast` | `/sig` 即时动作 | `uid, particle, action` |
| `sig_copy` | 近距同时 shake 复制对方降临 sig | `a, b, a_particle, b_particle` |
| `phase_change` | 阶段切换 | `phase` |
| `stage` | host 控场（DIM/REVEAL/JUMP/PHOTO） | `action`（`dim/undim/reveal/unreveal/jump/photo/unphoto`，`POST /admin/stage` 触发）；dim/reveal/photo 持久态注入 snapshot `stage:{dim,reveal,photo}` 供晚到客户端追赶 |
| `reading_reveal` | Phase 3 个人解读生成 | `uid, nick, title, lines[3], island, color, spectrum`（大屏只用 EN `title`/`lines`；CN 字段仅设备端 MQTT payload 有） |

---

## 规则要点（Server 权威，详见 M5_PRD / WORKSHOP_PLAN_V4）

- **5 格收集**：slot[0] = 本色（登岛即填）；其余 4 格从他人 HI 收对方岛色
- **同岛 HI 不加色**（共鸣光环）；**重复 HI 同人不加色**；满 5 格仍可 HI（纯社交）
- **HI** 单 pending + 30s 超时；**JUMP** 10s 窗口 ≥5 人触发 + 3s burst debounce
- **anon** 每人每分钟最多 1 条 + 服务端剥离身份

---

## 实现状态（截至 P0）

| 层 | 状态 |
|----|------|
| Server 传输层 | ✅ 完整 |
| Server 玩法 handler | 🟡 骨架（quiz→island 通；HI 仅校验握手，换色待 P3；reading 待 P4） |
| 大屏 | ⬜ P1 |
| 固件 | ⬜ P2（当前固件仍是 v1 协议，**未对接 v2** — Codex 提示） |
