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

import random

from . import avatar, avatar_pool, config, npc
from . import skills as skills_mod
from .achievements import AchievementEngine, Badge
from .pair_engine import PairEngine, PairRejection, PairResult, ShakeFingerprint
from .room import Room, now_ms
from .tasks import TaskEngine
from .ws import WSManager

log = logging.getLogger("loiter.mqtt")


class MqttBridge:
    def __init__(self, room: Room, ws: WSManager, loop: asyncio.AbstractEventLoop):
        self.room = room
        self.ws = ws
        self.loop = loop
        self.ach = AchievementEngine()
        self.tasks = TaskEngine()
        self.pair = PairEngine(room)
        self._msg_times: dict[str, deque[float]] = defaultdict(lambda: deque(maxlen=16))
        self._emote_last: dict[str, float] = {}  # uid -> last emote time (monotonic)
        self._move_last: dict[str, float] = {}   # uid -> last move time (monotonic)
        self._anon_last: dict[str, float] = {}   # uid -> last anon time (monotonic)
        # 问答赛状态
        self._quiz_state: str | None = None
        self._quiz_question = ""
        self._quiz_answer = ""
        self._quiz_hint = ""
        self._quiz_started_at = 0.0
        self._quiz_starter_uid = ""
        self._quiz_scores: dict[str, int] = {}
        self._quiz_round = 0
        self._quiz_max_rounds = self.QUIZ_ROUNDS
        # 头像/NPC 阻塞，丢到线程池跑，别堵住 MQTT 网络线程
        self._avatar_pool = ThreadPoolExecutor(max_workers=3, thread_name_prefix="avatar")
        self._avatar_inflight: set[str] = set()
        self._npc_inflight: set[str] = set()  # 每 uid 同时只允许一个 NPC 请求
        self.client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id="loiter-server",
        )
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message

    # --- 生命周期 ---
    def start(self) -> None:
        avatar_pool.load()
        avatar_pool.ensure_pool(self._avatar_pool)
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
            (config.topic("emote"), 1),
            (config.topic("npc", "ask"), 1),
            (config.topic("move"), 0),
            (config.topic("pair", "intent"), 1),
            (config.topic("pair", "shake"), 1),
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
            elif kind == "move":
                self._handle_move(data)
            elif kind == "emote":
                self._handle_emote(data)
            elif kind == "npc" and len(parts) > 3 and parts[3] == "ask":
                self._handle_npc_ask(data)
            elif kind == "avatar" and len(parts) > 3 and parts[3] == "request":
                self._handle_avatar_request(data)
            elif kind == "pair" and len(parts) > 3:
                sub = parts[3]
                if sub == "intent":
                    self._handle_pair_intent(data)
                elif sub == "shake":
                    self._handle_pair_shake(data)
        except Exception:
            log.exception("handler error on %s", msg.topic)

    # --- 业务处理 ---
    def _handle_join(self, data: dict) -> None:
        uid, nick = data.get("uid"), data.get("nick", "anon")
        if not uid:
            return
        was_empty = self.room.count == 0
        m = self.room.join(uid, nick)
        log.info("JOIN %s (%s) empty=%s count=%d skills=%s",
                 nick, uid, was_empty, self.room.count, sorted(m.skills))
        self._emit({
            "type": "join",
            "uid": uid, "nick": m.nick,
            "count": self.room.count, "ts": now_ms(),
            # Sprint 7: 让大屏立即渲染 skill chips（首次入场会带 starter skills）
            "skills": sorted(m.skills),
            "progress": list(skills_mod.progress(m.skills)),
        })
        self._award(uid, m.nick, self.ach.on_join(uid, was_empty))
        # Sprint 7 Phase 7.4: 把 starter + skill state 推回 Cardputer（让固件 UI 能画 chip 行）
        self._push_skill_state(uid, m)
        # 无头像 → 从预生成池秒分配（不需要等 30 秒 API 调用）
        if not m.png_b64:
            default_png = avatar_pool.pick(uid)
            if default_png:
                self.room.set_avatar(uid, default_png)
                self._emit({
                    "type": "avatar",
                    "uid": uid, "nick": m.nick,
                    "png_b64": default_png, "w": 16, "h": 16,
                    "ts": now_ms(),
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

    # --- 体感移动（IMU tilt）---
    MOVE_COOLDOWN = 0.25  # 每 uid 250ms 最多一次
    MOVE_SERVER_STEP = 50  # 服务端每次 move 积分的逻辑像素步长（用于 pair 距离 gate）

    def _handle_move(self, data: dict) -> None:
        uid = data.get("uid")
        if not uid or uid not in self.room.members:
            return
        now = time.monotonic()
        last = self._move_last.get(uid, 0)
        if now - last < self.MOVE_COOLDOWN:
            return
        self._move_last[uid] = now
        dx = max(-1.0, min(1.0, float(data.get("dx", 0))))
        dy = max(-1.0, min(1.0, float(data.get("dy", 0))))
        # Sprint 7: 服务端积分位置，给 pair 距离 gate 用
        # 大屏其实是按帧 2px/帧 推进；服务端粗算每 250ms tick × 50px ≈ 大屏 ~12s 走完屏宽
        from .room import LOGICAL_CANVAS_W, LOGICAL_CANVAS_H
        m = self.room.members[uid]
        m.x = max(50, min(LOGICAL_CANVAS_W - 50, m.x + dx * self.MOVE_SERVER_STEP))
        m.y = max(50, min(LOGICAL_CANVAS_H - 50, m.y + dy * self.MOVE_SERVER_STEP))
        self._emit({
            "type": "move",
            "uid": uid,
            "dx": round(dx, 2),
            "dy": round(dy, 2),
        })

    # --- 表情动作 ---
    # Sprint 7 Phase 7.3: 5 个老 emote + 11 个新 skill emote + 4 个大招 + omni = 21 个
    VALID_EMOTES = (
        skills_mod.ALL_SKILLS  # 16 个常规 skill
        | skills_mod.ALL_ULTIMATES  # 4 个大招（gaia/solar/dragon/galaxy）
        | {skills_mod.OMNI}  # omni
        | {"bloom", "spark", "wind", "fox", "rain"}  # 老的 5 个（其中 4 个已在 ALL_SKILLS，rain 是新名字）
    )
    EMOTE_COOLDOWN = 3.0  # 每 uid 3s 冷却

    def _handle_emote(self, data: dict) -> None:
        uid = data.get("uid")
        emote = data.get("emote", "")
        if not uid or emote not in self.VALID_EMOTES:
            return
        now = time.monotonic()
        last = self._emote_last.get(uid, 0)
        if now - last < self.EMOTE_COOLDOWN:
            log.debug("emote rate-limited %s", uid)
            return
        self._emote_last[uid] = now
        # auto-join 兜底
        if uid not in self.room.members:
            self._handle_join({"uid": uid, "nick": data.get("nick") or uid})
        m = self.room.members.get(uid)
        nick = m.nick if m else uid
        log.info("EMOTE %s (%s) -> %s", nick, uid, emote)
        self._emit({
            "type": "emote",
            "uid": uid, "nick": nick,
            "emote": emote, "ts": now_ms(),
        })
        # 破冰任务：表情完成检测
        self._check_task(uid, nick, f"emote:{emote}")

    def _handle_msg(self, channel: str, data: dict) -> None:
        uid = data.get("uid")
        text = (data.get("text") or "")[: config.MSG_LEN_MAX]
        if not uid or not text:
            return
        # 拦截 /e 和 /emote 命令：旧固件把命令当普通消息发出，服务端兜底转化
        stripped = text.strip()
        if stripped.startswith("/e ") or stripped.startswith("/emote "):
            emote = stripped.split(None, 1)[1].strip().lower() if " " in stripped else ""
            if emote in self.VALID_EMOTES:
                self._handle_emote({"uid": uid, "nick": data.get("nick"), "emote": emote})
                return  # 不当普通消息广播
        # 拦截 /ask 命令：问百晓生
        if stripped.startswith("/ask "):
            question = stripped[5:].strip()
            if question:
                self._handle_npc_ask({"uid": uid, "nick": data.get("nick"), "text": question})
                return
        # 拦截 /face 命令：生成 AI 头像
        if stripped.startswith("/face "):
            kw = stripped[6:].strip()
            if kw:
                keywords = [w for w in kw.split() if w.strip()]
                self._handle_avatar_request({"uid": uid, "nick": data.get("nick"), "keywords": keywords})
                return
        # 拦截 /task 命令：领取破冰任务
        if stripped == "/task":
            self._handle_task_request(uid, data.get("nick") or uid)
            return
        # 拦截 /anon 命令：匿名告白墙
        if stripped.startswith("/anon "):
            anon_text = stripped[6:].strip()[:config.MSG_LEN_MAX]
            if anon_text:
                self._handle_anon(uid, data.get("nick") or uid, anon_text)
            return
        # 拦截 /quiz 命令：发起问答赛
        if stripped == "/quiz":
            self._handle_quiz_start(uid, data.get("nick") or uid)
            return
        # 拦截 /ans 命令：问答赛抢答
        if stripped.startswith("/ans "):
            answer = stripped[5:].strip()
            if answer:
                self._handle_quiz_answer(uid, data.get("nick") or uid, answer)
            return
        # 拦截 /pair 命令：进入 3s 求偶模式（Sprint 7）
        if stripped == "/pair":
            self._handle_pair_intent({"uid": uid, "nick": data.get("nick") or uid})
            return
        # 拦截 /skills 命令：列出自己的技能 + 进度（Sprint 7）
        if stripped == "/skills":
            self._handle_skills_query(uid, data.get("nick") or uid)
            return
        # 拦截 /reset 命令（admin）：清场重置 — 所有人技能/配对/贡献归零，starter 重新随机
        if stripped == "/reset":
            self._handle_reset(uid, data.get("nick") or uid)
            return
        # 拦截 /nick 命令：改昵称 + 任务完成检测
        if stripped.startswith("/nick "):
            new_nick = stripped[6:].strip()
            if new_nick:
                self._handle_join({"uid": uid, "nick": new_nick})
                self._check_task(uid, new_nick, "nick")
            return
        if not self._allow_msg(uid):
            log.debug("rate-limited %s", uid)
            return
        # NPC / 匿名广播的回声不做 auto-join / 消息计数
        if uid == npc.NPC_UID or uid == "anon":
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
        # 破冰任务：消息完成检测
        self._check_task(uid, nick, f"msg_in:{channel}")

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

    def publish_ota(self, manifest: dict) -> None:
        """Phase 7.6 OTA — 广播 `loiter/hall/sys/ota`，retain=true 让晚到的设备也能拿到。

        固件订阅这个 topic 后，按 version + targets 决定是否升级。
        """
        payload = json.dumps(manifest, ensure_ascii=False)
        log.warning("OTA broadcast version=%s targets=%s url=%s",
                    manifest.get("version"), manifest.get("targets", "all"),
                    manifest.get("url"))
        self.client.publish(config.topic("sys", "ota"), payload, qos=1, retain=True)

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
        # 破冰任务：头像生成完成检测
        self._check_task(uid, nick, "face")
        # 大屏：显示生成中动画
        self._emit({
            "type": "avatar_generating",
            "uid": uid, "nick": nick, "ts": now_ms(),
        })
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

    # --- AI NPC（/ask → 线程池 CopilotX → 回复）---
    def _handle_npc_ask(self, data: dict) -> None:
        uid = data.get("uid")
        text = (data.get("text") or "").strip()[:200]
        if not uid or not text:
            return
        if uid in self._npc_inflight:
            log.info("NPC request from %s dropped (inflight)", uid)
            return
        self._npc_inflight.add(uid)
        # auto-join 兜底
        if uid not in self.room.members:
            self._handle_join({"uid": uid, "nick": data.get("nick") or uid})
        m = self.room.members.get(uid)
        nick = m.nick if m else uid
        log.info("NPC ask %s (%s): %s", nick, uid, text)
        # 破冰任务：问百晓生完成检测
        self._check_task(uid, nick, "ask")
        # 先广播问题到大屏
        self._emit({
            "type": "npc_ask",
            "uid": uid, "nick": nick,
            "text": text, "ts": now_ms(),
        })
        self._avatar_pool.submit(self._run_npc, uid, nick, text)

    def _run_npc(self, uid: str, nick: str, question: str) -> None:
        """线程池：阻塞调 CopilotX → 广播回复。"""
        try:
            reply = npc.ask(question, nick)
        except Exception:
            log.exception("NPC failed for %s", uid)
            reply = "……（仙人闭目不语）"
        finally:
            self._npc_inflight.discard(uid)
        log.info("NPC reply to %s: %s", nick, reply)
        # 回复到 MQTT（让 Cardputer 看到）
        payload = json.dumps(
            {"uid": npc.NPC_UID, "nick": npc.NPC_NICK, "text": reply},
            ensure_ascii=False,
        )
        self.client.publish(config.topic("msg", "main"), payload, qos=1)
        # 大屏 WS：特殊 npc_reply 事件
        self._emit({
            "type": "npc_reply",
            "uid": uid, "nick": nick,
            "npc_nick": npc.NPC_NICK,
            "text": reply, "ts": now_ms(),
        })

    # --- 破冰任务 ---
    def _handle_task_request(self, uid: str, nick: str) -> None:
        """用户输入 /task → 分配一个新任务或显示当前任务。"""
        task = self.tasks.assign(uid)
        if task is None:
            return
        log.info("TASK assigned %s (%s) -> %s: %s", nick, uid, task.id, task.title)
        # 推到 Cardputer（走 msg/main 让固件能显示）
        payload = json.dumps(
            {"uid": npc.NPC_UID, "nick": "任务", "text": f"📜 {task.title} — {task.desc}"},
            ensure_ascii=False,
        )
        self.client.publish(config.topic("msg", "main"), payload, qos=1)
        # 推到大屏
        self._emit({
            "type": "task_assign",
            "uid": uid, "nick": nick,
            "task_id": task.id, "title": task.title, "desc": task.desc,
            "ts": now_ms(),
        })

    def _check_task(self, uid: str, nick: str, event_type: str) -> None:
        """检查事件是否完成了 uid 的当前破冰任务。"""
        completed = self.tasks.check_event(uid, event_type)
        if completed is None:
            return
        log.info("TASK completed %s (%s) -> %s: %s", nick, uid, completed.id, completed.title)
        # 通知 Cardputer
        payload = json.dumps(
            {"uid": npc.NPC_UID, "nick": "任务", "text": f"✅ {completed.title} — 完成！"},
            ensure_ascii=False,
        )
        self.client.publish(config.topic("msg", "main"), payload, qos=1)
        # 大屏：卷轴燃尽动画
        self._emit({
            "type": "task_complete",
            "uid": uid, "nick": nick,
            "task_id": completed.id, "title": completed.title,
            "ts": now_ms(),
        })

    # --- 匿名告白墙 ---
    ANON_COOLDOWN = 5.0  # 每 uid 5s 冷却，防刷屏

    def _handle_anon(self, uid: str, nick: str, text: str) -> None:
        """匿名消息：服务端剥离身份，广播到大屏和所有 Cardputer。"""
        if not self._allow_msg(uid):
            return
        now = time.monotonic()
        last = self._anon_last.get(uid, 0)
        if now - last < self.ANON_COOLDOWN:
            log.debug("anon rate-limited %s", uid)
            return
        self._anon_last[uid] = now
        # auto-join 兜底
        if uid not in self.room.members:
            self._handle_join({"uid": uid, "nick": nick})
        log.info("ANON from %s (%s): %s", nick, uid, text)
        # 广播到 Cardputer（走 msg/main，但 nick 替换为匿名标识）
        payload = json.dumps(
            {"uid": "anon", "nick": "???", "text": f"💌 {text}"},
            ensure_ascii=False,
        )
        self.client.publish(config.topic("msg", "main"), payload, qos=1)
        # 大屏：专用 anon_msg 事件（特殊渲染）
        self._emit({
            "type": "anon_msg",
            "text": text, "ts": now_ms(),
        })

    # --- Sprint 7: 技能融合（pair / shake / result）---

    def _handle_pair_intent(self, data: dict) -> None:
        """`/pair` → 进入 3s 求偶模式。下发 ack 让固件切 UI 状态。"""
        uid = data.get("uid")
        nick = data.get("nick") or uid
        if not uid:
            return
        if uid not in self.room.members:
            self._handle_join({"uid": uid, "nick": nick})
        ok = self.pair.enter_pairing(uid)
        if not ok:
            return
        m = self.room.members.get(uid)
        nick = m.nick if m else (nick or uid)
        log.info("PAIR intent ack %s (%s)", nick, uid)
        # 通知本人固件进 PAIRING_MODE
        payload = json.dumps(
            {"phase": "armed", "window_s": 3.0, "ts": now_ms()},
            ensure_ascii=False,
        )
        self.client.publish(
            config.topic("pair", "result", uid), payload, qos=1
        )
        # 大屏：该用户头顶冒粉色 "❤️ pairing..." 气泡
        self._emit({
            "type": "pair_intent",
            "uid": uid, "nick": nick, "window_s": 3.0, "ts": now_ms(),
        })

    def _handle_pair_shake(self, data: dict) -> None:
        """`pair/shake` 上报 → 喂给 pair_engine。

        新版带 fingerprint：peaks / rhythm_ms / energy。
        老版仅传 peak_g 也能勉强跑（fingerprint 默认零，匹配分会低，容易被拒）
        —— 这是特意的：推动固件升级。
        """
        uid = data.get("uid")
        if not uid or uid not in self.room.members:
            return
        fp = ShakeFingerprint(
            peak_g=float(data.get("peak_g", 0.0)),
            peaks=int(data.get("peaks", 0)),
            rhythm_ms=int(data.get("rhythm_ms", 0)),
            energy=float(data.get("energy", 0.0)),
        )
        outcome = self.pair.on_shake(uid, fp)
        if outcome is None:
            return
        if isinstance(outcome, PairRejection):
            self._publish_pair_rejection(outcome)
        else:
            self._publish_pair_result(outcome)

    def _publish_pair_rejection(self, rej: PairRejection) -> None:
        """双方 toast 配对被拒（指纹不匹配 / 距离太远）。"""
        a = self.room.members.get(rej.a_uid)
        b = self.room.members.get(rej.b_uid)
        if a is None or b is None:
            return
        log.info("PAIR rejection %s ↔ %s reason=%s sim=%.2f dist=%.0f",
                 a.nick, b.nick, rej.reason, rej.similarity, rej.distance)
        for uid, partner_nick in ((rej.a_uid, b.nick), (rej.b_uid, a.nick)):
            payload = json.dumps(
                {
                    "phase": "rejected",
                    "reason": rej.reason,
                    "partner_nick": partner_nick,
                    "similarity": round(rej.similarity, 2),
                    "distance": round(rej.distance, 0),
                    "ts": now_ms(),
                },
                ensure_ascii=False,
            )
            self.client.publish(config.topic("pair", "result", uid), payload, qos=1)
        # 大屏：拒绝事件携带 reason，方便大屏画"虚线连接"提示
        self._emit({
            "type": "pair_rejected",
            "a_uid": rej.a_uid, "a_nick": a.nick,
            "b_uid": rej.b_uid, "b_nick": b.nick,
            "reason": rej.reason,
            "similarity": round(rej.similarity, 2),
            "distance": round(rej.distance, 0),
            "ts": now_ms(),
        })

    def _publish_pair_result(self, r: PairResult) -> None:
        """双方都推 pair/result/<uid>；大屏广播融合演出事件；解锁大招/omni 同步推送。"""
        a = self.room.members.get(r.a_uid)
        b = self.room.members.get(r.b_uid)
        if a is None or b is None:
            return
        # 落地 ultimates / omni
        for el in r.a_new_ultimates:
            a.unlocked_ultimates.add(el)
        for el in r.b_new_ultimates:
            b.unlocked_ultimates.add(el)
        if r.a_omni:
            a.has_omni = True
        if r.b_omni:
            b.has_omni = True
        # 推 A
        self._push_pair_result_one(
            r.a_uid, r.b_nick, r.a_gained, r.a_new_ultimates, r.a_omni,
            partner_uid=r.b_uid, total=len(a.skills),
        )
        # 推 B
        self._push_pair_result_one(
            r.b_uid, r.a_nick, r.b_gained, r.b_new_ultimates, r.b_omni,
            partner_uid=r.a_uid, total=len(b.skills),
        )
        # Phase 7.4: 紧接着推全量 state，让固件 chip 行同步
        self._push_skill_state(r.a_uid, a)
        self._push_skill_state(r.b_uid, b)
        # 大屏：融合演出事件（B 方案 — 大屏接管 5s 相向奔赴 + 光柱）
        self._emit({
            "type": "pair_fused",
            "a_uid": r.a_uid, "a_nick": r.a_nick,
            "a_gained": sorted(r.a_gained),
            "a_ultimates": r.a_new_ultimates,
            "a_omni": r.a_omni,
            "a_progress": list(skills_mod.progress(a.skills)),
            "b_uid": r.b_uid, "b_nick": r.b_nick,
            "b_gained": sorted(r.b_gained),
            "b_ultimates": r.b_new_ultimates,
            "b_omni": r.b_omni,
            "b_progress": list(skills_mod.progress(b.skills)),
            "similarity": round(r.similarity, 2),
            "ts": now_ms(),
        })

    def _push_pair_result_one(
        self,
        uid: str,
        partner_nick: str,
        gained: set[str],
        new_ultimates: list[str],
        omni: bool,
        partner_uid: str,
        total: int,
    ) -> None:
        """定向推送 pair/result/<uid>（固件 toast）。"""
        payload = json.dumps(
            {
                "phase": "fused",
                "partner_uid": partner_uid,
                "partner_nick": partner_nick,
                "gained": sorted(gained),
                "ultimates": new_ultimates,
                "omni": omni,
                "stack": total,
                "total": skills_mod.TOTAL_COUNT,
                "ts": now_ms(),
            },
            ensure_ascii=False,
        )
        self.client.publish(config.topic("pair", "result", uid), payload, qos=1)

    def _push_skill_state(self, uid: str, m) -> None:
        """Sprint 7 Phase 7.4: 定向推送 skill state 给 Cardputer 固件。

        固件已订阅 `pair/result/<uid>`；这里新增 phase=state，用于：
        - 入场时让固件 UI 立即画出 starter chip 行
        - 配对后增量更新 chip 行（fused 已带 gained，但 state 是全量真相）
        - /reset 后通知固件重置 chip 行
        """
        payload = json.dumps(
            {
                "phase": "state",
                "skills": sorted(m.skills),
                "starter": sorted(m.starter_skills),
                "ultimates": sorted(m.unlocked_ultimates),
                "omni": m.has_omni,
                "stack": len(m.skills),
                "total": skills_mod.TOTAL_COUNT,
                "ts": now_ms(),
            },
            ensure_ascii=False,
        )
        self.client.publish(config.topic("pair", "result", uid), payload, qos=1)

    def _handle_skills_query(self, uid: str, nick: str) -> None:
        """`/skills` → 回显自己当前的 skill 列表 + 进度。"""
        if uid not in self.room.members:
            self._handle_join({"uid": uid, "nick": nick})
        m = self.room.members.get(uid)
        if m is None:
            return
        collected, total = skills_mod.progress(m.skills)
        line = " ".join(sorted(m.skills)) if m.skills else "(none yet — go /pair!)"
        text = f"🃏 Skills {collected}/{total}: {line}"
        if m.unlocked_ultimates:
            text += f"  ⚡ Ultimates: {' '.join(sorted(m.unlocked_ultimates))}"
        if m.has_omni:
            text += "  🌌 OMNISCIENT!"
        payload = json.dumps(
            {"uid": npc.NPC_UID, "nick": npc.NPC_NICK, "text": text},
            ensure_ascii=False,
        )
        self.client.publish(config.topic("msg", "main"), payload, qos=1)

    def _handle_reset(self, uid: str, nick: str) -> None:
        """`/reset` admin → 清场重置：所有人技能/配对/贡献归零，starter 重新随机分配。

        无密码保护——内部测试工具。生产部署请加 admin 鉴权。
        """
        log.warning("RESET triggered by %s (%s) — wiping skill state for %d members",
                    nick, uid, len(self.room.members))
        for m in self.room.members.values():
            m.skills = skills_mod.random_starter(2)
            m.starter_skills = set(m.skills)
            m.paired_with = set()
            m.contributed_to = set()
            m.unlocked_ultimates = set()
            m.has_omni = False
        # Phase 7.4: 推全量 state 到每台 Cardputer，让 chip 行立即归零
        for member_uid, m in self.room.members.items():
            self._push_skill_state(member_uid, m)
        # 通知大屏全量刷新快照（让前端 chip/crown 立即归零）
        self._emit({
            "type": "snapshot",
            **self.room.snapshot(),
        })
        # 频道广播 Vix 风格通知
        payload = json.dumps(
            {"uid": npc.NPC_UID, "nick": npc.NPC_NICK,
             "text": f"🌪️ Reset by {nick}! Everyone got fresh starters — go pair!"},
            ensure_ascii=False,
        )
        self.client.publish(config.topic("msg", "main"), payload, qos=1)

    # --- Vix 问答赛 ---
    # 状态: None=空闲, "asking"=等待抢答, "cooldown"=题间间隔
    QUIZ_TIMEOUT = 20.0  # 每题最多等 20 秒
    QUIZ_ROUNDS = 5      # 一场 5 题
    QUIZ_COOLDOWN = 3.0  # 题间间隔

    def _handle_quiz_start(self, uid: str, nick: str) -> None:
        if self._quiz_state is not None:
            # 已在进行中
            payload = json.dumps(
                {"uid": npc.NPC_UID, "nick": npc.NPC_NICK,
                 "text": "A quiz is already running! Use /ans <answer> to play~"},
                ensure_ascii=False,
            )
            self.client.publish(config.topic("msg", "main"), payload, qos=1)
            return
        # auto-join
        if uid not in self.room.members:
            self._handle_join({"uid": uid, "nick": nick})
        self._quiz_starter_uid = uid
        self._quiz_scores = {}
        self._quiz_round = 0
        log.info("QUIZ started by %s (%s)", nick, uid)
        # 通知所有人
        self._emit({
            "type": "quiz_start",
            "uid": uid, "nick": nick,
            "rounds": self.QUIZ_ROUNDS, "ts": now_ms(),
        })
        payload = json.dumps(
            {"uid": npc.NPC_UID, "nick": npc.NPC_NICK,
             "text": f"🎯 Quiz time! {self.QUIZ_ROUNDS} rounds. I'll ask, you /ans! Let's go~"},
            ensure_ascii=False,
        )
        self.client.publish(config.topic("msg", "main"), payload, qos=1)
        # 生成第一题
        self._avatar_pool.submit(self._quiz_next_question)

    def _quiz_next_question(self) -> None:
        """线程池：调 CopilotX 生成题目，然后广播。"""
        self._quiz_round += 1
        if self._quiz_round > self._quiz_max_rounds:
            self._quiz_end()
            return
        prompt = (
            "Generate a fun trivia question for a social hall quiz game. "
            "The question should be answerable in 1-3 words. "
            "Mix topics: science, pop culture, geography, food, animals, tech, history. "
            "Format your response as exactly 3 lines:\n"
            "Q: <the question>\n"
            "A: <the answer, 1-3 words, case-insensitive>\n"
            "H: <a one-word hint>\n"
            "Example:\n"
            "Q: What planet is known as the Red Planet?\n"
            "A: Mars\n"
            "H: fourth"
        )
        try:
            import httpx
            resp = httpx.post(
                npc.ENDPOINT,
                headers={"X-Client-Id": "loiter"},
                json={
                    "model": npc.MODEL,
                    "messages": [
                        {"role": "system", "content": "You are a trivia quiz master. Output ONLY the requested format, nothing else."},
                        {"role": "user", "content": prompt},
                    ],
                    "max_tokens": 100,
                    "temperature": 1.0,
                },
                timeout=30.0,
            )
            resp.raise_for_status()
            text = resp.json()["choices"][0]["message"]["content"].strip()
            lines = text.strip().split("\n")
            q_line = next((l for l in lines if l.strip().upper().startswith("Q:")), None)
            a_line = next((l for l in lines if l.strip().upper().startswith("A:")), None)
            h_line = next((l for l in lines if l.strip().upper().startswith("H:")), None)
            if not q_line or not a_line:
                raise ValueError(f"Bad quiz format: {text}")
            self._quiz_question = q_line.split(":", 1)[1].strip()
            self._quiz_answer = a_line.split(":", 1)[1].strip().lower()
            self._quiz_hint = h_line.split(":", 1)[1].strip() if h_line else ""
        except Exception:
            log.exception("quiz question generation failed")
            # 降级：硬编码题库
            fallback = [
                ("What is the largest ocean?", "pacific", "biggest"),
                ("How many legs does a spider have?", "8", "arachnid"),
                ("What gas do plants breathe in?", "co2", "carbon"),
                ("In which country is the Great Wall?", "china", "asia"),
                ("What color do you get mixing red and blue?", "purple", "violet"),
            ]
            q, a, h = fallback[(self._quiz_round - 1) % len(fallback)]
            self._quiz_question = q
            self._quiz_answer = a
            self._quiz_hint = h

        self._quiz_state = "asking"
        self._quiz_started_at = time.monotonic()
        log.info("QUIZ round %d/%d: %s (answer: %s)",
                 self._quiz_round, self._quiz_max_rounds, self._quiz_question, self._quiz_answer)
        # 广播题目
        payload = json.dumps(
            {"uid": npc.NPC_UID, "nick": npc.NPC_NICK,
             "text": f"📝 Q{self._quiz_round}: {self._quiz_question}"},
            ensure_ascii=False,
        )
        self.client.publish(config.topic("msg", "main"), payload, qos=1)
        self._emit({
            "type": "quiz_question",
            "round": self._quiz_round,
            "total": self._quiz_max_rounds,
            "question": self._quiz_question,
            "hint": self._quiz_hint,
            "ts": now_ms(),
        })
        # 10 秒后给提示，20 秒后超时
        import threading
        threading.Timer(10.0, self._quiz_give_hint).start()
        threading.Timer(self.QUIZ_TIMEOUT, self._quiz_timeout).start()

    def _quiz_give_hint(self) -> None:
        if self._quiz_state != "asking":
            return
        if self._quiz_hint:
            payload = json.dumps(
                {"uid": npc.NPC_UID, "nick": npc.NPC_NICK,
                 "text": f"💡 Hint: {self._quiz_hint}"},
                ensure_ascii=False,
            )
            self.client.publish(config.topic("msg", "main"), payload, qos=1)
            self._emit({
                "type": "quiz_hint",
                "hint": self._quiz_hint, "ts": now_ms(),
            })

    def _quiz_timeout(self) -> None:
        if self._quiz_state != "asking":
            return
        self._quiz_state = "cooldown"
        log.info("QUIZ round %d timed out, answer was: %s", self._quiz_round, self._quiz_answer)
        payload = json.dumps(
            {"uid": npc.NPC_UID, "nick": npc.NPC_NICK,
             "text": f"⏰ Time's up! The answer was: {self._quiz_answer}"},
            ensure_ascii=False,
        )
        self.client.publish(config.topic("msg", "main"), payload, qos=1)
        self._emit({
            "type": "quiz_timeout",
            "answer": self._quiz_answer,
            "round": self._quiz_round,
            "ts": now_ms(),
        })
        # 下一题
        import threading
        threading.Timer(self.QUIZ_COOLDOWN, lambda: self._avatar_pool.submit(self._quiz_next_question)).start()

    def _handle_quiz_answer(self, uid: str, nick: str, answer: str) -> None:
        if self._quiz_state != "asking":
            return
        # auto-join
        if uid not in self.room.members:
            self._handle_join({"uid": uid, "nick": nick})
        m = self.room.members.get(uid)
        nick = m.nick if m else nick
        answer_clean = answer.strip().lower()
        correct = answer_clean == self._quiz_answer or self._quiz_answer in answer_clean
        if correct:
            self._quiz_state = "cooldown"
            self._quiz_scores[uid] = self._quiz_scores.get(uid, 0) + 1
            elapsed = round(time.monotonic() - self._quiz_started_at, 1)
            log.info("QUIZ correct! %s (%s) in %.1fs", nick, uid, elapsed)
            payload = json.dumps(
                {"uid": npc.NPC_UID, "nick": npc.NPC_NICK,
                 "text": f"🎉 {nick} got it in {elapsed}s! Answer: {self._quiz_answer}"},
                ensure_ascii=False,
            )
            self.client.publish(config.topic("msg", "main"), payload, qos=1)
            self._emit({
                "type": "quiz_correct",
                "uid": uid, "nick": nick,
                "answer": self._quiz_answer,
                "elapsed": elapsed,
                "scores": self._build_scores(),
                "round": self._quiz_round,
                "ts": now_ms(),
            })
            # 下一题
            import threading
            threading.Timer(self.QUIZ_COOLDOWN, lambda: self._avatar_pool.submit(self._quiz_next_question)).start()
        else:
            # 答错了，广播 "X" 但不泄露答案
            self._emit({
                "type": "quiz_wrong",
                "uid": uid, "nick": nick,
                "ts": now_ms(),
            })

    def _build_scores(self) -> list[dict]:
        """构建按分数降序的排行榜。"""
        result = []
        for uid, score in sorted(self._quiz_scores.items(), key=lambda x: -x[1]):
            m = self.room.members.get(uid)
            result.append({"uid": uid, "nick": m.nick if m else uid, "score": score})
        return result

    def _quiz_end(self) -> None:
        """问答赛结束，广播最终排行榜。"""
        self._quiz_state = None
        scores = self._build_scores()
        winner = scores[0] if scores else None
        log.info("QUIZ ended. Scores: %s", scores)
        if winner:
            text = f"🏆 Quiz over! Winner: {winner['nick']} ({winner['score']}pts)!"
        else:
            text = "🏆 Quiz over! No one scored... better luck next time~"
        payload = json.dumps(
            {"uid": npc.NPC_UID, "nick": npc.NPC_NICK, "text": text},
            ensure_ascii=False,
        )
        self.client.publish(config.topic("msg", "main"), payload, qos=1)
        self._emit({
            "type": "quiz_end",
            "scores": scores, "ts": now_ms(),
        })

    # --- 桥到 asyncio ---
    def _emit(self, event: dict) -> None:
        """从网络线程把事件安全投递给主循环做 WS 广播。"""
        asyncio.run_coroutine_threadsafe(self.ws.broadcast(event), self.loop)
