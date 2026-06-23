"""配置 — 环境变量优先，默认值对齐 docs/mqtt-protocol.md。"""
from __future__ import annotations

import os

PROTOCOL_VERSION = "v2"

# --- MQTT broker（本地 Mosquitto）---
MQTT_HOST = os.getenv("LOITER_MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(os.getenv("LOITER_MQTT_PORT", "1883"))
MQTT_KEEPALIVE = int(os.getenv("LOITER_MQTT_KEEPALIVE", "60"))
# 鉴权（公网部署必填；空字符串 = 匿名，仅供本地开发）
MQTT_USER = os.getenv("LOITER_MQTT_USER", "").strip()
MQTT_PASS = os.getenv("LOITER_MQTT_PASS", "").strip()

# --- 房间 ---
ROOM = os.getenv("LOITER_ROOM", "hall")
INSTANCE_NAME = os.getenv("LOITER_INSTANCE", "GLEAM Hall")

# --- Admin 控场鉴权（host 导播台 DIM/REVEAL/PHOTO）---
# 空 = fail-closed：未配则拒绝所有 /admin/* 操作（不给 fallback 默认值，避免印进公开代码）。
# VM 侧在 /etc/loiter/loiter.env 配 LOITER_ADMIN_TOKEN=...，本地测试 export 一下。
ADMIN_TOKEN = os.getenv("LOITER_ADMIN_TOKEN", "").strip()

# --- 烧录窗口（v3″：零 token，host 控开窗，默认开，状态持久化在 profile DB meta）---
# /flash/profile 受「窗口开 + 每 IP 限流」双门控；关窗 → 拒绝建 profile。
# 窗口开/关由 host 在 admin 面板（POST /admin/flash-window）切换，跨重启不丢。
FLASH_RATE_PER_HOUR = int(os.getenv("LOITER_FLASH_RATE_PER_HOUR", "50"))

# 烧录时随 profile 下发给设备 config.h 的 broker 连接信息。
# 设备连「公网 broker」，与 server 自身连「本地 broker」(MQTT_HOST=127.0.0.1) 不同 → 单列。
# user/pass 复用 MQTT_USER/MQTT_PASS（broker 鉴权同一套），由 /etc/loiter/loiter.env 注入，
# 不在 repo 里硬编码；窗口关闭时不下发。
FLASH_MQTT_HOST = os.getenv("LOITER_FLASH_MQTT_HOST", "mqtt.polly.wang")
FLASH_MQTT_PORT = int(os.getenv("LOITER_FLASH_MQTT_PORT", "1883"))

# --- HTTP / WebSocket ---
HTTP_HOST = os.getenv("LOITER_HTTP_HOST", "0.0.0.0")
HTTP_PORT = int(os.getenv("LOITER_HTTP_PORT", "8080"))

# --- 频道 ---
CHANNELS = ("main", "fishing", "help")

# --- 心跳 ---
STATUS_INTERVAL = float(os.getenv("LOITER_STATUS_INTERVAL", "5.0"))  # 秒

# --- Rate limit（每 uid）---
MSG_RATE_PER_SEC = 2
MSG_LEN_MAX = 200

# --- Web 静态资源目录（大屏）---
import pathlib

_SERVER_DIR = pathlib.Path(__file__).resolve().parent.parent
WEB_DIR = pathlib.Path(os.getenv("LOITER_WEB_DIR", _SERVER_DIR.parent / "web"))

# v3′ 烧录 profile 持久化（SQLite）：分岛/文艺 reason 绑 profile_id，跨重启不丢
PROFILE_DB = pathlib.Path(os.getenv("LOITER_PROFILE_DB", _SERVER_DIR / "data" / "profiles.db"))


def topic(*parts: str) -> str:
    """构造 loiter/<room>/... topic。"""
    return "/".join(("loiter", ROOM, *parts))
