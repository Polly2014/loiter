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
from concurrent.futures import ThreadPoolExecutor

import paho.mqtt.client as mqtt

from . import avatar, config
from .achievements import AchievementEngine, Badge
from .room import Room, now_ms
from .ws import WSManager

log = logging.getLogger("loiter.mqtt")


class MqttBridge:
    def __init__(self, room: Room, ws: WSManager, loop: asyncio.AbstractEventLoop):
        self.room = room
        self.ws = ws
        self.loop = loop
        self.ach = AchievementEngine()
        self._msg_times: dict[str, deque[float]] = defaultdict(lambda: deque(maxlen=16))
        # 头像生成阻塞数十秒，丢到线程池跑，别堵住 MQTT 网络线程
        self._avatar_pool = ThreadPoolExecutor(max_workers=2, thread_name_prefix="avatar")
        self._avatar_inflight: set[str] = set()
        self.client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id="loiter-server",
        )
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message

    # --- 生命周期 ---
    def start(self) -> None:
        if config.MQTT_USER:
            self.client.username_pw_set(config.MQTT_USER, config.MQTT_PASS)
        log.info("connecting broker %s:%d (auth=%s)",
                 config.MQTT_HOST, config.MQTT_PORT, bool(config.MQTT_USER))
        self.client.connect(config.MQTT_HOST, config.MQTT_PORT, config.MQTT_KEEPALIVE)
        self.client.loop_start()

    def stop(self) -> None:
        self._avatar_pool.shutdown(wait=False, cancel_futures=True)
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
            (config.topic("avatar", "request"), 1),
        ]
        client.subscribe(subs)
        log.info("subscribed: %s", [s for s, _ in subs])
        if not avatar.ENABLED:
            log.warning("avatar disabled: AZURE_OPENAI_API_KEY/ENDPOINT not set")

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
            elif kind == "avatar" and len(parts) > 3 and parts[3] == "request":
                self._handle_avatar_request(data)
        except Exception:
            log.exception("handler error on %s", msg.topic)

    # --- 业务处理 ---
    def _handle_join(self, data: dict) -> None:
        uid, nick = data.get("uid"), data.get("nick", "anon")
        if not uid:
            return
        was_empty = self.room.count == 0
        m = self.room.join(uid, nick)
        log.info("JOIN %s (%s) empty=%s count=%d", nick, uid, was_empty, self.room.count)
        self._emit({
            "type": "join",
            "uid": uid, "nick": m.nick,
            "count": self.room.count, "ts": now_ms(),
        })
        self._award(uid, m.nick, self.ach.on_join(uid, was_empty))

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
        # 服务端重启后真机不会重发 join，能发消息就当在场，幂等补一下
        if uid not in self.room.members:
            self._handle_join({"uid": uid, "nick": data.get("nick") or uid})
        m = self.room.record_msg(uid, channel)
        nick = (m.nick if m else data.get("nick")) or "anon"
        log.info("MSG[%s] %s: %s", channel, nick, text)
        self._emit({
            "type": "msg",
            "channel": channel, "uid": uid, "nick": nick,
            "text": text, "ts": now_ms(),
        })
        msg_count = m.msg_count if m else 0
        self._award(uid, nick, self.ach.on_msg(
            uid, channel, now_ms(), msg_count, self.room.total_messages,
        ))

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

    def _award(self, uid: str, nick: str, badges: list[Badge]) -> None:
        """成就解锁：定向下发到该 uid（S→C）+ 广播大屏（WS）。"""
        for b in badges:
            log.info("ACHIEVEMENT %s -> %s (%s)", nick, b.id, b.title)
            payload = json.dumps(
                {"badge": b.id, "title": b.title, "emoji": b.emoji, "desc": b.desc},
                ensure_ascii=False,
            )
            # 定向：QoS 1 确保 Cardputer 收到
            self.client.publish(config.topic("achievement", uid), payload, qos=1)
            # 大屏 toast
            self._emit({
                "type": "achievement",
                "uid": uid, "nick": nick,
                "badge": b.id, "title": b.title, "emoji": b.emoji, "desc": b.desc,
                "ts": now_ms(),
            })

    # --- AI 头像（C→S 请求 → 线程池生成 → S→C 下发）---
    def _handle_avatar_request(self, data: dict) -> None:
        uid = data.get("uid")
        if not uid:
            return
        kw = data.get("keywords") or []
        if isinstance(kw, str):
            kw = [kw]
        kw = [str(k)[:40] for k in kw if str(k).strip()][:6]
        if not avatar.ENABLED:
            log.warning("avatar request from %s ignored (disabled)", uid)
            self.publish_notice("头像生成未启用", level="warn")
            return
        if uid in self._avatar_inflight:
            log.info("avatar request from %s dropped (inflight)", uid)
            return
        self._avatar_inflight.add(uid)
        # 同 _handle_msg 兜底：能请求头像就当在场，没 join 过就幂等补一下
        if uid not in self.room.members:
            self._handle_join({"uid": uid, "nick": data.get("nick") or uid})
        m = self.room.members.get(uid)
        nick = m.nick if m else uid
        log.info("AVATAR request %s (%s) keywords=%s", nick, uid, kw)
        self._avatar_pool.submit(self._run_avatar, uid, nick, kw)

    def _run_avatar(self, uid: str, nick: str, keywords: list[str]) -> None:
        """线程池：阻塞生成 → 下发 bitmap（S→C QoS1）+ 广播大屏 PNG（WS）。"""
        try:
            res = avatar.generate(uid, keywords)
        except Exception:
            log.exception("avatar gen failed for %s", uid)
            self.publish_notice("头像生成失败", level="warn")
            return
        finally:
            self._avatar_inflight.discard(uid)
        # Cardputer：定向 16×16 bitmap（paho publish 线程安全）
        payload = json.dumps(
            {"bitmap_b64": res.bitmap_b64, "w": res.w, "h": res.h},
            ensure_ascii=False,
        )
        self.client.publish(config.topic("avatar", uid), payload, qos=1)
        # 记住头像，让重连大屏能从快照恢复
        self.room.set_avatar(uid, res.png_b64)
        # 大屏：彩色 PNG
        self._emit({
            "type": "avatar",
            "uid": uid, "nick": nick,
            "png_b64": res.png_b64, "w": res.w, "h": res.h,
            "ts": now_ms(),
        })
        log.info("AVATAR delivered %s (bitmap=%dB png=%dB)",
                 nick, len(res.bitmap_b64), len(res.png_b64))

    # --- 桥到 asyncio ---
    def _emit(self, event: dict) -> None:
        """从网络线程把事件安全投递给主循环做 WS 广播。"""
        asyncio.run_coroutine_threadsafe(self.ws.broadcast(event), self.loop)
