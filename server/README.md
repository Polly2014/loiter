# Loiter Hub Server

GLEAM Hall 实例的房间状态权威：**MQTT bridge + WebSocket fanout + 房间状态机**。

## 运行

```bash
cd X-Workspace/loiter/server
uv sync                     # 创建 .venv + 装依赖
# 确保 broker 在跑：mosquitto -c ../broker/mosquitto.conf -v
uv run uvicorn loiter.main:app --host 0.0.0.0 --port 8080 --reload
```

防 Mac 睡眠：
```bash
caffeinate -dimsu uv run uvicorn loiter.main:app --host 0.0.0.0 --port 8080
```

## 端点

| 路径 | 说明 |
|------|------|
| `GET /healthz` | 存活 + 在线人数 + broker 状态 |
| `GET /api/snapshot` | 房间全量快照（成员列表） |
| `WS /ws` | 大屏实时事件流（连上即收 `snapshot`，之后 `join`/`leave`/`msg`/`status`） |
| `/` | 大屏静态资源（`../web/`，存在才挂载） |

## 环境变量

| 变量 | 默认 | 说明 |
|------|------|------|
| `LOITER_MQTT_HOST` | `127.0.0.1` | broker 地址 |
| `LOITER_MQTT_PORT` | `1883` | broker 端口 |
| `LOITER_HTTP_PORT` | `8080` | HTTP/WS 端口 |
| `LOITER_ROOM` | `hall` | 房间 ID |
| `LOITER_WEB_DIR` | `../web` | 大屏静态目录 |

## WebSocket 事件类型

```jsonc
{ "type": "snapshot", "members": [...], "count": 3, ... }   // 连接时一次
{ "type": "join",   "uid": "...", "nick": "...", "count": 4 }
{ "type": "leave",  "uid": "...", "count": 3 }
{ "type": "msg",    "channel": "main", "uid": "...", "nick": "...", "text": "..." }
{ "type": "status", "count": 3 }                            // 每 5s
```

## 架构 / 契约

- MQTT topic + payload 契约见 [`../docs/mqtt-protocol.md`](../docs/mqtt-protocol.md)
- paho 回调在网络线程，经 `run_coroutine_threadsafe` 投递到 asyncio 主循环做 WS 广播
- P1 留 hook：avatar（CopilotX + Pillow dither）、achievement、SQLite 持久化
