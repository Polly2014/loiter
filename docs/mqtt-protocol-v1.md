# MQTT Protocol Specification — Loiter v1

> 本文档是 **Cardputer 固件** 与 **Hub Server** 的 API 契约。
> 任何 schema 修改必须 bump 版本号 + 同步通知 Reviewers。

**Version**: v1 (draft, 2026-05-30)
**Broker**: Mosquitto on Mac M1, port 1883, no TLS (local network only)

---

## 通用约定

### 命名空间
```
loiter/<room>/<topic>[/<sub>]
```
- v1 阶段 `<room>` 固定为 `hall`
- 未来扩展多房间（如 `loiter/cafe`、`loiter/secret`）时升 v2

### Payload 格式
所有 payload 均为 **UTF-8 JSON**。

### 共用字段
| 字段 | 类型 | 说明 |
|------|------|------|
| `uid` | string (8 char) | 设备唯一 ID，固件首次启动生成 + 存 NVS |
| `nick` | string (≤16 char ASCII) | 显示昵称 |
| `ts` | uint64 | Unix epoch milliseconds (UTC) |
| `channel` | enum | `main` / `fishing` / `help` |

### QoS 规则

| 用途 | QoS | 备注 |
|------|-----|------|
| 聊天消息 (msg/whisper) | 1 | at-least-once，避免丢消息 |
| 状态广播 (status) | 0 | lossy OK，5s 一次 |
| 头像下发 (avatar) | 1 + retain | 后到的客户端立即拿到 |
| 成就推送 (achievement) | 1 | 重要 toast |
| OTA (sys/ota) | 1 + retain | 关键升级 |
| IR 碰一碰 (ir/ping) | 0 | 高频，丢一两个无所谓 |

---

## Topics（按方向分类）

### Client → Server（Cardputer 发布）

#### `loiter/hall/join`
设备上线声明。
```json
{ "uid": "a8f3c1d2", "nick": "Polly", "ts": 1748620800000, "fw_ver": "0.1.0" }
```

#### `loiter/hall/leave`
主动下线 **或 LWT 自动触发**。
```json
{ "uid": "a8f3c1d2", "ts": 1748620800000, "reason": "graceful" | "lwt" }
```

#### `loiter/hall/msg/<channel>`
频道消息。`<channel>` ∈ `{main, fishing, help}`
```json
{ "uid": "a8f3c1d2", "nick": "Polly", "text": "hello world", "ts": 1748620800000 }
```
- `text` 限长 200 字符
- 服务端做 rate-limit：每用户每秒 ≤2 条

#### `loiter/hall/whisper/<to_uid>`
私聊。
```json
{ "from": "a8f3c1d2", "text": "hey", "ts": 1748620800000 }
```
- 服务端只 forward 给 `to_uid` 所在的 session
- ⚠️ **Known limitation (L2)**：Mosquitto 默认无 ACL，任意客户端可订阅他人 `whisper/<uid>` topic。
  30 人内部活动接受此风险；v2 若需隔离改服务端中转（不用 topic 直发）或上 broker ACL。

#### `loiter/hall/avatar/request`
请求 AI 生成头像。
```json
{ "uid": "a8f3c1d2", "keywords": ["cyberpunk", "cat", "neon"] }
```
- `keywords` ≤5 个、每个 ≤20 字符
- 服务端异步处理：3-15s 后通过 `loiter/hall/avatar/<uid>` 下发结果

#### `loiter/hall/emote`
表情动作（国风特效）。
```json
{ "uid": "a8f3c1d2", "nick": "Polly", "emote": "ink", "ts": 1748620800000 }
```
- `emote` ∈ `ink`(甩墨痕) / `splash`(溅墨点) / `ripple`(涟漪墨圈) / `sword`(金色剑气) / `incense`(香炉轻烟)
- 服务端 rate-limit：每 uid ≤1 次/3s
- 服务端收到后广播到 WS（大屏特效）+ 回发 MQTT（Cardputer 显示文字提示）

#### `loiter/hall/ir/ping`
IR 碰一碰上报（P2）。
```json
{ "from_uid": "a8f3c1d2", "to_uid": "b9e4d2e3", "rssi": -45, "ts": ... }
```

---

### Server → Client / Broadcast

#### `loiter/hall/msg/<channel>` (echo)
服务端把收到的消息广播给所有订阅了该 channel 的客户端。
- 服务端不修改 payload，只做 rate-limit + 内容过滤

#### `loiter/hall/avatar/<uid>` (retain)
头像 bitmap 下发。
```json
{
  "uid": "a8f3c1d2",
  "bitmap_b64": "...",     // 32 bytes base64 → 16×16 1-bit packed
  "width": 16,
  "height": 16,
  "format": "mono_msb",     // bit packing: MSB first
  "url_hires": "https://loiter.polly.wang/avatar/a8f3c1d2.png"  // 大屏用
}
```
- `retain=true`，后加入者立即拿到全员头像
- 固件解码：每 byte = 8 像素，从 MSB 开始

#### `loiter/hall/achievement/<uid>`
成就解锁。
```json
{
  "uid": "a8f3c1d2",
  "badge": "first_join",
  "title": "First Light",
  "desc": "你是第一个加入大厅的人",
  "ts": 1748620800000
}
```

#### `loiter/hall/status`
在线人数心跳广播（每 5s）。**只发 `count`，不发全员列表**——
在线名单的增删由 `join` / `leave` 事件维护（增量），避免 30 人时 payload 撞爆
PubSubClient 默认 256B buffer（见「固件实现注意事项」）。
```json
{ "count": 7, "ts": 1748620800000 }
```
> 大屏（WebSocket，无 buffer 限制）若需要完整在线名单，由 Server 通过 WS 单独推全量快照，不走 MQTT。

#### `loiter/hall/sys/notice`
系统通告（横幅滚动）。
```json
{ "text": "AI NPC 已上线，发送 /ai <消息> 找它聊天", "level": "info" }
```
`level` ∈ `info` / `warn` / `error`

#### `loiter/hall/sys/ota` (retain)
OTA 推送（Phase 7.6）。
```json
{
  "version": "0.3.0",
  "url": "https://loiter.polly.wang/firmware/loiter.bin",
  "sha256": "abcdef0123...64chars hex",
  "size": 1234567,
  "targets": "all",
  "build_ts": 1748620800000
}
```
- 服务端发布时 **retain=true** → 晚加入的设备订阅后立即拿到，无需轮询
- `version` 必须严格 semver `x.y.z`；固件做整数三段对比（避免 "0.10" < "0.2" 字典序陷阱）
- `targets` 取值：`"all"` / `"<uid>"` / `"<uid>,<uid>"`（逗号分隔，空格忽略）；缺省=all
- `sha256` 64 char 小写 hex；固件流式下载边 mbedtls SHA256 比对，不匹配则 Update.abort()
- `size` 字节数；HTTP Content-Length 为准时优先用它，否则用 manifest 提供值预分配 partition
- `url` 支持 `http://` 与 `https://`；HTTPS 用 `WiFiClientSecure.setInsecure()`（依赖 sha256 做完整性兜底）
- 固件版本 ≤ 当前 → 静默跳过；targets 不含自己 → 静默跳过
- 同一台设备同时只允许一个 OTA 任务（重入保护）；失败界面会自动还原到 lobby

---

## LWT (Last Will Testament)

每个 Cardputer 在 MQTT CONNECT 时必须设置 LWT：

```c
client.setWill(
  "loiter/hall/leave",                      // topic
  "{\"uid\":\"a8f3c1d2\",\"reason\":\"lwt\"}",  // payload
  1,                                         // QoS
  false                                      // retain
);
```

broker 检测到断线（keepalive timeout，默认 60s）会自动发布 LWT，
Server 收到 `reason=lwt` 后清理在线状态并广播 `status`。

---

## Subscriptions（Client 侧推荐订阅清单）

```
loiter/hall/msg/main          # 默认订阅主厅
loiter/hall/whisper/<my_uid>  # 私聊
loiter/hall/avatar/+          # 所有头像（retain 自动 backfill）
loiter/hall/achievement/<my_uid>  # 自己的成就
loiter/hall/status            # 在线人列表
loiter/hall/sys/+             # 所有系统消息
```

切换频道时：
- unsubscribe 旧 channel 的 `loiter/hall/msg/<old>`
- subscribe 新 channel 的 `loiter/hall/msg/<new>`

---

## Rate Limits (Server 侧实施)

| Topic | 限制 |
|-------|------|
| `msg/<channel>` | 每 uid ≤2 条/s, ≤30 条/min |
| `whisper/<to>` | 每 uid ≤5 条/s |
| `avatar/request` | 每 uid 每 5 分钟 ≤1 次 |
| `ir/ping` | 每 uid ≤2 次/s |

超限的消息**丢弃 + 不通知**（防 DoS），仅在 server log 记录。

---

## HTTP Endpoints（Phase 7.6 OTA）

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/firmware/manifest.json` | 返回当前固件 manifest（无固件则 404）|
| GET | `/firmware/loiter.bin` | 当前 canonical 固件二进制（StaticFiles 直接服务）|
| GET | `/firmware/archives/loiter-<ver>.bin` | 历史归档（可选）|
| POST | `/firmware/broadcast` | 把 manifest 通过 `loiter/hall/sys/ota` 广播；body `{targets?:"all"\|"uid,uid"}`|

发布流程：`scripts/publish_ota.sh <version>` 一键 编译 + scp + 写 manifest + POST 触发广播。

---

## Error / Disconnect 行为

| 场景 | 客户端行为 |
|------|----------|
| WiFi 断 | 屏幕显示 `[OFFLINE]`，自动重连 |
| MQTT 断（WiFi 正常） | 屏幕显示 `[RECONNECTING]`，指数退避 1/2/4/8s |
| broker 拒绝 | 屏幕显示 `[BROKER REJECT]`，停 30s 后重试 |
| 收到非法 JSON | 静默忽略，server log warn |

---

## 固件实现注意事项（PubSubClient）

| 坑 | 处理 |
|----|------|
| 默认单包 payload ≤256 bytes | `#define MQTT_MAX_PACKET_SIZE 1024`（编译前）或运行时 `client.setBufferSize(1024)`。avatar bitmap JSON ~200B、msg 长文本都可能超 256B |
| 不支持 QoS 2 | 本协议只用 QoS 0/1，已规避 |
| keepalive 默认 15s | 建议 `client.setKeepAlive(60)` 配合 LWT timeout |
| `status` 原设计发全员列表 | 已改为只发 `count`（增量），参见 status topic |

---

## 版本变更记录

| 版本 | 日期 | 变更 |
|------|------|------|
| v1 (draft) | 2026-05-30 | 初稿，待 Reviewers 拍板 |
| v1.6 | 2026-06-08 | Phase 7.6 OTA：sys/ota retain + sha256 强制校验 + targets 白名单；新增 HTTP `/firmware/manifest.json` 与 `/firmware/broadcast` |

> 任何字段增删都要 bump 版本，并在固件 / server 的 `config.py` / `config.h` 里
> 显式声明 `PROTOCOL_VERSION`，握手时校验。
