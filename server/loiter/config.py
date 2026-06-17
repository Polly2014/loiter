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
# Phase 7.6 OTA: firmware 目录（loiter.bin + manifest.json）默认放在 web/firmware/
# Web 静态 mount 会顺带把 /firmware/loiter.bin 暴露出去
FIRMWARE_DIR = pathlib.Path(os.getenv("LOITER_FIRMWARE_DIR", WEB_DIR / "firmware"))


def topic(*parts: str) -> str:
    """构造 loiter/<room>/... topic。"""
    return "/".join(("loiter", ROOM, *parts))
