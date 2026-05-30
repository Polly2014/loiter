"""MQTT bridge — paho-mqtt（线程） ↔ Room 状态 ↔ WebSocket（asyncio）。

paho 的回调跑在自己的网络线程里，通过 asyncio.run_coroutine_threadsafe
把事件投递回主事件循环做 WS 广播。
"""
from __future__ import annotations

import asyncio
import json
import logging
import time
from collections import defaultdict, deque

import paho.mqtt.client as mqtt

from . import config
from .room import Room, now_ms
from .ws import WSManager

log = logging.getLogger("loiter.mqtt")


class MqttBridge:
    def __init__(self, room: Room, ws: WSManager, loop: asyncio.AbstractEventLoop):
        self.room = room
        self.ws = ws
        self.loop = loop
        self._msg_times: dict[str, deque[float]] = defaultdict(lambda: deque(maxlen=16))
        self.client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id="loiter-server",
        )
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message

    # --- 生命周期 ---
    def start(self) -> None:
        log.info("connecting broker %s:%d", config.MQTT_HOST, config.MQTT_PORT)
        self.client.connect(config.MQTT_HOST, config.MQTT_PORT, config.MQTT_KEEPALIVE)
        self.client.loop_start()

    def stop(self) -> None:
        self.client.loop_stop()
        self.client.disconnect()

    # --- paho 回调（网络线程）---
    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        if reason_code != 0:
            log.error("broker connect failed: %s", reason_code)
            return
        subs = [
            (config.topic("join"), 1),
            (config.topic("leave"), 1),
            (config.topic("msg", "+"), 1),
        ]
        client.subscribe(subs)
        log.info("subscribed: %s", [s for s, _ in subs])

    def _on_message(self, client, userdata, msg):
        try:
            data = json.loads(msg.payload.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            log.warning("bad json on %s", msg.topic)
            return
        parts = msg.topic.split("/")
        kind = parts[2] if len(parts) > 2 else ""
        try:
            if kind == "join":
                self._handle_join(data)
            elif kind == "leave":
                self._handle_leave(data)
            elif kind == "msg":
                channel = parts[3] if len(parts) > 3 else "main"
                self._handle_msg(channel, data)
        except Exception:
            log.exception("handler error on %s", msg.topic)

    # --- 业务处理 ---
    def _handle_join(self, data: dict) -> None:
        uid, nick = data.get("uid"), data.get("nick", "anon")
        if not uid:
            return
        is_new = uid not in self.room.members
        m = self.room.join(uid, nick)
        log.info("JOIN %s (%s) new=%s count=%d", nick, uid, is_new, self.room.count)
        self._emit({
            "type": "join",
            "uid": uid, "nick": m.nick,
            "count": self.room.count, "ts": now_ms(),
        })

    def _handle_leave(self, data: dict) -> None:
        uid = data.get("uid")
        if not uid:
            return
        m = self.room.leave(uid)
        reason = data.get("reason", "graceful")
        log.info("LEAVE %s reason=%s count=%d", uid, reason, self.room.count)
        self._emit({
            "type": "leave",
            "uid": uid, "nick": m.nick if m else uid,
            "count": self.room.count, "ts": now_ms(),
        })

    def _handle_msg(self, channel: str, data: dict) -> None:
        uid = data.get("uid")
        text = (data.get("text") or "")[: config.MSG_LEN_MAX]
        if not uid or not text:
            return
        if not self._allow_msg(uid):
            log.debug("rate-limited %s", uid)
            return
        m = self.room.record_msg(uid)
        nick = (m.nick if m else data.get("nick")) or "anon"
        log.info("MSG[%s] %s: %s", channel, nick, text)
        self._emit({
            "type": "msg",
            "channel": channel, "uid": uid, "nick": nick,
            "text": text, "ts": now_ms(),
        })

    def _allow_msg(self, uid: str) -> bool:
        """每 uid ≤ MSG_RATE_PER_SEC 条/秒。"""
        now = time.monotonic()
        q = self._msg_times[uid]
        while q and now - q[0] > 1.0:
            q.popleft()
        if len(q) >= config.MSG_RATE_PER_SEC:
            return False
        q.append(now)
        return True

    # --- 发布（Server → Client）---
    def publish_status(self) -> None:
        payload = json.dumps({"count": self.room.count, "ts": now_ms()})
        self.client.publish(config.topic("status"), payload, qos=0)
        self._emit({"type": "status", "count": self.room.count, "ts": now_ms()})

    def publish_notice(self, text: str, level: str = "info") -> None:
        payload = json.dumps({"text": text, "level": level})
        self.client.publish(config.topic("sys", "notice"), payload, qos=1)

    # --- 桥到 asyncio ---
    def _emit(self, event: dict) -> None:
        """从网络线程把事件安全投递给主循环做 WS 广播。"""
        asyncio.run_coroutine_threadsafe(self.ws.broadcast(event), self.loop)
