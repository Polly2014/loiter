"""MQTT bridge — paho-mqtt（线程） ↔ Room 状态 ↔ WebSocket（asyncio）。

v2 · Islands of Color。

传输层（沿用 v1 已验证资产）：join / leave / move / status + WS fanout。
玩法层（v2 新）：island / hi / jump / anon / phase / reading。

paho 的回调跑在自己的网络线程里，通过 asyncio.run_coroutine_threadsafe
把事件投递回主事件循环做 WS 广播。

P0：传输层完整 + 玩法层 handler 接到 islands/ 引擎的占位调用（真实链路待 P3/P4）。
"""
from __future__ import annotations

import asyncio
import json
import logging
import threading
import time
from collections import defaultdict, deque
from concurrent.futures import ThreadPoolExecutor

import paho.mqtt.client as mqtt

from . import config
from .islands import HiEngine, JumpAggregator, assign_island, generate_reading
from .islands.assignment import ISLANDS
from .room import Room, now_ms
from .ws import WSManager

log = logging.getLogger("loiter.mqtt")


class MqttBridge:
    def __init__(self, room: Room, ws: WSManager, loop: asyncio.AbstractEventLoop):
        self.room = room
        self.ws = ws
        self.loop = loop
        self.hi = HiEngine()
        self.jump = JumpAggregator()
        self._move_last: dict[str, float] = {}   # uid -> last move time (monotonic)
        self._anon_last: dict[str, float] = {}    # uid -> last anon time (monotonic)
        self._prox_last: dict[tuple[str, str], float] = {}
        self._recent_shake: dict[str, float] = {}  # uid -> last proximity shake time (monotonic)
        self._jump_last_burst = 0.0
        # Host 控场的持久态（dim/reveal/photo 是开关，jump 是瞬时）。后连入的大屏/观众页
        # 从 snapshot 拿到当前态做追赶，避免「reveal 后才打开的浏览器看不到 story」(P1-2)。
        self._stage = {"dim": False, "reveal": False, "photo": False}
        # Phase 3 reading 生成走线程池（CopilotX 调用阻塞，不能占 paho 网络线程）；
        # max_workers=5 限并发，防同时多人触发打爆 CopilotX。
        self._reading_pool = ThreadPoolExecutor(max_workers=5, thread_name_prefix="reading")
        self._reading_inflight: set[tuple[str, int]] = set()   # (uid, gen) 正在生成（防同 gen 重复请求）
        # reading worker 跑在线程池 + 结果投回 event loop 线程，与 paho 网络线程并发改 Room；
        # 这把锁专护 _reading_inflight + m.reading 的跨线程访问（修 Codex P2）。
        self._reading_lock = threading.Lock()
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
        self.client.loop_stop()
        self.client.disconnect()
        self._reading_pool.shutdown(wait=False, cancel_futures=True)

    # --- paho 回调（网络线程）---
    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        if reason_code != 0:
            log.error("broker connect failed: %s", reason_code)
            return
        subs = [
            # 传输层
            (config.topic("join"), 1),
            (config.topic("profile"), 1),
            (config.topic("profile", "request"), 1),   # C→S 设备拉 island+reason（Reset 重取）
            (config.topic("leave"), 1),
            (config.topic("move"), 0),
            # 玩法层 v2
            (config.topic("quiz", "done"), 1),   # C→S quiz 完成（含答案）→ 分配岛屿
            (config.topic("hi", "request"), 1),
            (config.topic("hi", "respond"), 1),
            (config.topic("hi", "cancel"), 1),
            (config.topic("jump"), 1),
            (config.topic("shake"), 1),
            (config.topic("anon"), 1),
            (config.topic("sig"), 1),
            (config.topic("reading", "request"), 1),   # C→S 设备进 Phase 3 按需请求 reading
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
        sub = parts[3] if len(parts) > 3 else ""
        try:
            if kind == "join":
                self._handle_join(data)
            elif kind == "profile":
                (self._handle_profile_request if sub == "request" else self._handle_profile)(data)
            elif kind == "leave":
                self._handle_leave(data)
            elif kind == "move":
                self._handle_move(data)
            elif kind == "quiz" and sub == "done":
                self._handle_quiz_done(data)
            elif kind == "hi" and sub == "request":
                self._handle_hi_request(data)
            elif kind == "hi" and sub == "respond":
                self._handle_hi_respond(data)
            elif kind == "hi" and sub == "cancel":
                self._handle_hi_cancel(data)
            elif kind == "jump":
                self._handle_jump(data)
            elif kind == "shake":
                self._handle_shake(data)
            elif kind == "anon":
                self._handle_anon(data)
            elif kind == "sig":
                self._handle_sig(data)
            elif kind == "reading" and sub == "request":
                self._handle_reading_request(data)
        except Exception:
            log.exception("handler error on %s", msg.topic)

    # ─────────────────────────────────────────────────────────────────────
    # 传输层 handler（沿用 v1 已验证逻辑）
    # ─────────────────────────────────────────────────────────────────────
    @staticmethod
    def _sanitize_nick(raw: str) -> str:
        """裁到 ≤12 可打印 ASCII（防大屏 DOM 被 `/nick ` 注入打坏 — review Codex #2）。"""
        s = "".join(c for c in str(raw or "") if 32 <= ord(c) < 127).strip()
        return s[:12] or "anon"

    def _resolve_nick(self, nick: str | None) -> str | None:
        """nick → uid（大小写不敏感 first-match）。设备键入 `HI ALICE` 时服务端解析。

        只在**已分岛成员**里匹配，口径与 roster 一致（修 Codex P2：否则未分岛同名者
        会 shadow 一个 roster 里可见的合法目标 → 解析到未分岛 uid → request 被静默拒）。
        服务端是权威：昵称已 sanitize；同名取首个匹配（workshop 规模小，冲突罕见）。
        """
        if not nick:
            return None
        target = self._sanitize_nick(nick).lower()
        for m in self.room.members.values():
            if m.island >= 0 and m.spectrum is not None and m.nick.lower() == target:
                return m.uid
        return None

    @staticmethod
    def _sanitize_profile(data: dict) -> tuple[dict, int, int]:
        avatar = data.get("avatar") if isinstance(data.get("avatar"), dict) else {}
        shape = avatar.get("shape") if isinstance(avatar.get("shape"), list) else []
        color = avatar.get("color") if isinstance(avatar.get("color"), list) else []
        shape5 = [int(shape[i]) if i < len(shape) and isinstance(shape[i], (int, float)) else 0 for i in range(5)]
        color5 = [int(color[i]) if i < len(color) and isinstance(color[i], (int, float)) else 0 for i in range(5)]
        sig = data.get("sig") if isinstance(data.get("sig"), dict) else {}
        particle = int(sig.get("particle", data.get("sig_particle", -1)))
        action = int(sig.get("action", data.get("sig_action", -1)))
        particle = particle if -1 <= particle <= 3 else -1
        action = action if -1 <= action <= 2 else -1
        return {"shape": shape5, "color": color5}, particle, action

    def _emit_profile_update(self, m) -> None:
        self._emit({
            "type": "profile_update",
            "uid": m.uid,
            "nick": m.nick,
            "avatar": m.avatar,
            "sig_particle": m.sig_particle,
            "sig_action": m.sig_action,
            "ts": now_ms(),
        })

    def _emit_island_assign(self, m) -> None:
        """大屏：小人登岛（服务端位置权威，携 spectrum/坐标）。quiz_done 与 v3 join 共用。"""
        self._emit({
            "type": "island_assign",
            "uid": m.uid, "nick": m.nick,
            "island": m.island, "island_color": m.island_color,
            "spectrum": m.spectrum.as_list() if m.spectrum else [],
            "avatar": m.avatar,
            "sig_particle": m.sig_particle,
            "sig_action": m.sig_action,
            "x": round(m.x, 1), "y": round(m.y, 1),
            "ts": now_ms(),
        })

    @staticmethod
    def _sanitize_seed(raw: str) -> str:
        """profile.text —— 剖控制字符 + 截断（仅用作 reading prompt context，不上屏）。"""
        s = "".join(c for c in str(raw or "") if ord(c) >= 32)
        return s[:400].strip()

    def _push_island(self, m) -> None:
        """S→C 把岛屿 + 文艺 reason 推给设备（Phase-1 揭晓）。fresh join 发 + profile/request 拉。

        reason 异步生成：member 落定时可能还空 → **每次 push 前从 store 取最新**
        （修 Codex P1：早 join 拿空 reason；P2：profile/request 重取最新）。
        """
        if m.profile_id:
            from .profile_store import store
            prof = store.get(m.profile_id)
            if prof:
                if prof.get("reason_en"):
                    m.reason_en = prof["reason_en"]
                if prof.get("reason_cn"):
                    m.reason_cn = prof["reason_cn"]
        name = ISLANDS[m.island].name if 0 <= m.island < len(ISLANDS) else ""
        self.client.publish(
            config.topic("island", m.uid),
            json.dumps({"island": m.island, "color": m.island_color, "name": name,
                        "reason_en": m.reason_en, "reason_cn": m.reason_cn}),
            qos=1,
        )

    def notify_reason_ready(self, profile_id: str) -> None:
        """reason 异步生成完 → 若该 profile 的设备已在线（ready 前就 join 了）→ 重推 island 带新 reason。

        跑在 event-loop 线程（main.py reason worker via call_soon_threadsafe）。
        list() 快照防与 paho 线程并发改 members（修 Codex P1）。
        """
        for m in list(self.room.members.values()):
            if m.profile_id == profile_id and m.island >= 0:
                self._push_island(m)

    def _apply_profile(self, m, data: dict) -> bool:
        """v3′：从 join payload 读 profile_id → 查 server profile 分岛 + reason。

        返回是否本次**新分岛**（需 push island + emit island_assign + roster）。
        - 无 profile_id 字段（真旧设备）→ False，走 legacy quiz/done。
        - profile_id="" （v3' 设备但未经 skill 烧录 / 烧录异常）→ 用 uid 作确定性 fallback 身份
          adopt，保证仍能分到岛（不在仪式核心静默失败；同 uid rejoin → 同岛）。
        - 已分岛（Reset/rejoin 同 pid）→ False，幂等不重复（island 绑 profile_id，同岛）。
        - 未知 pid → store.adopt 轮转一个新岛（兼容 orphan，rejoin 一致）。
        """
        pid = data.get("profile_id")
        if pid is None:
            return False  # 真旧设备：无 profile_id 字段 → legacy quiz/done
        if not isinstance(pid, str):
            return False
        if m.island >= 0:
            return False
        from .profile_store import store
        # 空 pid → 以 uid 派生确定性 fallback key（review Codex P1）。effective_pid 贯穿
        # adopt + m.profile_id，使 _push_island 重取 reason / profile_request 重推 / notify_reason_ready
        # 都能匹配上这个 fallback profile（review Codex P2：不能把空串写回 m.profile_id）。
        effective_pid = pid or f"uid:{m.uid}"
        if pid:
            prof = store.get(pid) or store.adopt(pid)
        else:
            prof = store.adopt(effective_pid)
        assigned = self.room.assign_island(m.uid, prof["island"])
        if assigned is None or assigned.island < 0:
            return False
        m.profile_id = effective_pid
        m.seed = self._sanitize_seed(prof.get("text", ""))
        m.reason_en = prof.get("reason_en", "")
        m.reason_cn = prof.get("reason_cn", "")
        return True

    def _handle_join(self, data: dict) -> None:
        uid = data.get("uid")
        nick = self._sanitize_nick(data.get("nick", "anon"))
        if not uid:
            return
        avatar, sig_particle, sig_action = self._sanitize_profile(data)
        # 心跳每 25s 重发 join（服务端重启自愈）。对存活服务端，已在册成员的重复 join
        # 只刷 last_seen/nick，不再 emit WS join，避免大屏每 25s 假 arrival 日志
        # （16 台 ≈ 每分钟 38 条假 arrival — review Codex P2 / 🦞 nit）。
        was_new = uid not in self.room.members
        old = self.room.members.get(uid)
        old_nick = old.nick if old else ""
        old_avatar = old.avatar if old else {}
        old_sig_particle = old.sig_particle if old else -1
        old_sig_action = old.sig_action if old else -1
        m = self.room.join(uid, nick)
        m.avatar = avatar
        # sig_particle 只有未注册 origin 或在背包内才允许写入（防绕过 owned 校验）
        if m.sig_origin < 0 <= sig_particle:
            m.sig_origin = sig_particle      # 降临 sig 首次确定 → 锁定为 origin（近距复制传给别人的就是这个）
            m.sig_owned.add(sig_particle)    # 同时收进背包
            m.sig_particle = sig_particle
        elif sig_particle >= 0 and sig_particle in m.sig_owned:
            m.sig_particle = sig_particle
        m.sig_action = sig_action
        # v3′：join 携 baked profile_id → 查 server profile 分岛 + reason（代替旧 baked island/seed）。
        freshly_assigned = self._apply_profile(m, data)
        if not was_new:
            if freshly_assigned:
                # 重连/服务端重启后首个 profile join 补分岛 → push + 登岛 + 名册
                self._push_island(m)
                self._emit_island_assign(m)
                self.publish_roster()
            if (m.nick != old_nick or m.avatar != old_avatar or
                    m.sig_particle != old_sig_particle or m.sig_action != old_sig_action):
                self._emit_profile_update(m)
            if m.nick != old_nick and m.island >= 0 and not freshly_assigned:
                self.publish_roster()   # 名册（含 nick）随改名刷新（review Codex P2）
            return
        log.info("JOIN %s (%s) count=%d island=%d", nick, uid, self.room.count, m.island)
        self._emit({
            "type": "join",
            "uid": uid, "nick": m.nick,
            "island": m.island, "island_color": m.island_color,
            "avatar": m.avatar,
            "sig_particle": m.sig_particle,
            "sig_action": m.sig_action,
            "count": self.room.count, "ts": now_ms(),
        })
        if freshly_assigned:
            self._push_island(m)          # S→C 揭晓 island+reason
            self._emit_island_assign(m)   # 大屏登岛
        self.publish_roster()

    def _handle_profile(self, data: dict) -> None:
        uid = data.get("uid")
        if not uid or uid not in self.room.members:
            return
        m = self.room.members[uid]
        nick = self._sanitize_nick(data.get("nick", m.nick))
        avatar, sig_particle, sig_action = self._sanitize_profile(data)
        old_nick = m.nick
        old_avatar = m.avatar
        old_sig_particle = m.sig_particle
        old_sig_action = m.sig_action
        m.nick = nick
        m.avatar = avatar
        # sig_particle 只有未注册 origin 或在背包内才允许写入（防绕过 owned 校验）
        if m.sig_origin < 0 <= sig_particle:
            m.sig_origin = sig_particle   # 降临 sig 首次确定 → 锁定为 origin
            m.sig_owned.add(sig_particle)
            m.sig_particle = sig_particle
        elif sig_particle >= 0 and sig_particle in m.sig_owned:
            m.sig_particle = sig_particle
        m.sig_action = sig_action
        m.last_seen = now_ms()
        nick_changed = m.nick != old_nick
        changed = (
            nick_changed or
            m.avatar != old_avatar or
            m.sig_particle != old_sig_particle or
            m.sig_action != old_sig_action
        )
        if changed:
            self._emit_profile_update(m)
        if nick_changed and m.island >= 0:
            self.publish_roster()   # 名册随改名刷新（review Codex P2）

    def _handle_profile_request(self, data: dict) -> None:
        """设备进揭晓屏时拉自己的 island + 文艺 reason（首次 + Reset 重进都走这）。

        payload: {uid}。已分岛才回 island/<uid>；未分岛静默（device 自带 loading）。
        """
        uid = data.get("uid")
        m = self.room.members.get(uid) if uid else None
        if m is None or m.island < 0:
            return
        self._push_island(m)

    def _handle_leave(self, data: dict) -> None:
        uid = data.get("uid")
        if not uid:
            return
        m = self.room.leave(uid)
        self._recent_shake.pop(uid, None)
        self._move_last.pop(uid, None)
        reason = data.get("reason", "graceful")
        log.info("LEAVE %s reason=%s count=%d", uid, reason, self.room.count)
        self._emit({
            "type": "leave",
            "uid": uid, "nick": m.nick if m else uid,
            "count": self.room.count, "ts": now_ms(),
        })
        self.publish_roster()

    MOVE_COOLDOWN = 0.25      # 每 uid 250ms 最多一次
    MOVE_SERVER_STEP = 50     # 服务端每次 move 积分的逻辑像素步长

    def _handle_move(self, data: dict) -> None:
        uid = data.get("uid")
        if not uid or uid not in self.room.members:
            return
        now = time.monotonic()
        if now - self._move_last.get(uid, 0) < self.MOVE_COOLDOWN:
            return
        self._move_last[uid] = now
        dx = max(-1.0, min(1.0, float(data.get("dx", 0))))
        dy = max(-1.0, min(1.0, float(data.get("dy", 0))))
        from .room import LOGICAL_CANVAS_W, LOGICAL_CANVAS_H
        m = self.room.members[uid]
        m.x = max(50, min(LOGICAL_CANVAS_W - 50, m.x + dx * self.MOVE_SERVER_STEP))
        m.y = max(50, min(LOGICAL_CANVAS_H - 50, m.y + dy * self.MOVE_SERVER_STEP))
        self._emit({"type": "move", "uid": uid, "dx": round(dx, 2), "dy": round(dy, 2)})

    # ─────────────────────────────────────────────────────────────────────
    # 玩法层 handler（v2 — P0 占位骨架，完整链路待 P3/P4）
    # ─────────────────────────────────────────────────────────────────────
    def _handle_quiz_done(self, data: dict) -> None:
        """quiz 完成 → 分配岛屿 + 初始化 5 格收集 → 通知该设备 + 大屏。"""
        uid = data.get("uid")
        answers = data.get("answers", [])
        if not uid or uid not in self.room.members:
            return
        # 心跳自愈：设备每 25s 重发 join+quiz/done（服务端重启后让小人自动回归）。
        # 对存活服务端，已分岛成员的重复 quiz/done 是纯幂等心跳 → 在此短路，避免每 25s
        # 给大屏重放 island_assign 登岛动画 + 给设备重推 island/<uid>。仅真正首次分岛
        # （member 存在但 island<0）才走下面的发布。assign_island 自身已幂等保岛。
        pre = self.room.members.get(uid)
        if pre is not None and pre.island >= 0:
            pre.last_seen = now_ms()
            return
        info = assign_island(answers)
        m = self.room.assign_island(uid, info.idx)
        if m is None:
            return
        # 存 quiz 原始选择（Phase 3 reading 用）。仅首次分岛时记，重玩 quiz 不覆盖
        # （幂等：已分岛的人 answers 已定，保持旅程数据一致）。
        if not m.quiz_answers and isinstance(answers, list):
            m.quiz_answers = [a for a in answers if isinstance(a, int)]
        # 发布一律用 m 的权威字段（已分岛时 room 幂等保留旧岛，info 是新答案算的，
        # 若发 info 会造成"新 island + 旧 spectrum/pos"状态分裂 — review Codex #1）。
        from .islands.assignment import ISLANDS
        m_island = m.island
        m_color = m.island_color
        m_name = ISLANDS[m_island].name if 0 <= m_island < len(ISLANDS) else ""
        log.info("ISLAND %s (%s) -> %s %s", m.nick, uid, m_name, m_color)
        # S→C 定向告诉设备它的岛屿
        self.client.publish(
            config.topic("island", uid),
            json.dumps({"island": m_island, "name": m_name, "color": m_color}),
            qos=1,
        )
        # 大屏：小人登岛（复用 helper，与 v3 join 一致）
        self._emit_island_assign(m)
        self.publish_roster()   # island 变了，名册带岛色重推

    def _handle_hi_request(self, data: dict) -> None:
        """HI 发起 → 受理 pending + 通知被邀请方（携发起者岛色/昵称，供其屏显）。

        payload: {requester, responder?, responder_nick?, msg?}
          requester     = 发起 HI 的人（uid）
          responder     = 被邀请方 uid（直接指定）
          responder_nick= 被邀请方昵称（设备键入 `HI ALICE`，服务端解析成 uid）
        二者给其一即可，优先用 responder uid。nick→uid 由服务端权威解析（review 选项 A）。
        """
        requester = data.get("requester")
        responder = data.get("responder") or self._resolve_nick(data.get("responder_nick"))
        if not requester or not responder or requester == responder:
            return
        rq = self.room.members.get(requester)
        rp = self.room.members.get(responder)
        if rq is None or rp is None:          # 双方都得在场
            return
        # 双方都得已分岛（有 spectrum）才允许 HI（修 Codex P2：未分岛发起会让发起方
        # accept 后因 spectrum is None 静默作废、永远卡在等待屏）。
        if rq.island < 0 or rp.island < 0 or rq.spectrum is None or rp.spectrum is None:
            return
        if self.hi.request(requester, responder):
            log.info("HI request %s -> %s", requester, responder)
            # 通知被邀请方有人 HI（S→C，走 responder 的 hi/result topic）
            self.client.publish(
                config.topic("hi", "result", responder),
                json.dumps({
                    "event": "incoming",
                    "requester": requester,
                    "requester_nick": rq.nick,
                    "color": rq.island_color,
                    "msg": (data.get("msg") or "")[:15],
                }),
                qos=1,
            )

    def _handle_hi_respond(self, data: dict) -> None:
        """HI 应答 → 双向换色 + spectrum 入格 + 通知双方设备 + 大屏彩虹弧。

        payload: {requester, responder, accept}
          responder 回应 requester 之前发起的 HI。字段名与发起方对齐，
          避免 from/to 在 "谁是发送者" 上的歧义（review: 小龙虾 #2 / Codex）。
        """
        requester = data.get("requester")
        responder = data.get("responder")
        accept = bool(data.get("accept"))
        if not requester or not responder:
            return
        ok = self.hi.respond(requester, responder, accept)
        if ok is None:
            # 无匹配 pending（超时已 GC / 从未发起 / responder 不符）→ 静默丢弃，
            # 不发 declined（修 Codex P1：否则任何人可伪造 declined 砸他人等待屏）。
            return
        if ok is False:
            # 有效 pending 但被拒 → 告知发起方收尾（退出"等待回复"屏）。
            log.info("HI declined %s -X- %s", responder, requester)
            if requester in self.room.members:
                self.client.publish(
                    config.topic("hi", "result", requester),
                    json.dumps({"event": "declined", "partner": responder}),
                    qos=1,
                )
            return
        self._complete_hi_match(requester, responder)

    def _complete_hi_match(self, requester: str, responder: str) -> None:
        rq = self.room.members.get(requester)
        rp = self.room.members.get(responder)
        if rq is None or rp is None or rq.spectrum is None or rp.spectrum is None:
            return  # 任一方掉线/未分岛 → 握手作废
        # 双向换色：各自收对方岛色（同岛/重复/已满 → add_from 返回 None = 共鸣不加色）
        slot_rq = rq.spectrum.add_from(responder, rp.island_color)
        slot_rp = rp.spectrum.add_from(requester, rq.island_color)
        rq.hi_count += 1
        rp.hi_count += 1
        log.info("HI handshake OK %s <-> %s (slots %s/%s)",
                 requester, responder, slot_rq, slot_rp)
        # S→C 双方各收一条结果（partner 昵称 + 对方岛色 + 自己填入的格位，-1=共鸣不加色）
        self.client.publish(
            config.topic("hi", "result", requester),
            json.dumps({"event": "matched", "partner": rp.nick,
                        "color": rp.island_color,
                        "slot": slot_rq if slot_rq is not None else -1}),
            qos=1,
        )
        self.client.publish(
            config.topic("hi", "result", responder),
            json.dumps({"event": "matched", "partner": rq.nick,
                        "color": rq.island_color,
                        "slot": slot_rp if slot_rp is not None else -1}),
            qos=1,
        )
        # 大屏：跨海彩虹弧（a/b = uid，大屏从 wsChars 取坐标）+ 双方 spectrum 刷新
        self._emit({
            "type": "hi_arc",
            "a": requester, "b": responder,
            "a_spectrum": rq.spectrum.as_list(),
            "b_spectrum": rp.spectrum.as_list(),
            "ts": now_ms(),
        })

    PROX_HI_DISTANCE = 170.0
    PROX_HI_COOLDOWN = 4.0
    PROX_SHAKE_WINDOW = 1.5     # 双方 shake 必须落在这个时间窗内才算"同时晃动"（参考 v1 PAIR_SHAKE_TOLERANCE_S）

    def _try_sig_exchange(self, uid: str) -> bool:
        """近距 shake 复制 sig：两人靠近 + 双方都在 PROX_SHAKE_WINDOW 内 shake →
        各自把当前 sig 复制成**对方降临时的 sig**（像岛色一样复制，不是交换）。

        用 sig_origin（对方降临值，永不变）而非对方 current，防链式污染：A 先和 C 复制后
        B 再和 A shake，拿到的仍是 A 的降临 sig，不是 C 的。与 JUMP（集体跳）是两个独立场景。
        调用前 _handle_shake 已记 self._recent_shake[uid]=now。
        """
        m = self.room.members.get(uid)
        if m is None or m.island < 0:
            return False
        now = time.monotonic()
        nearest = None
        nearest_d2 = None
        for oid, other in self.room.members.items():
            if oid == uid or other.island < 0:
                continue
            # 对方也得在"同时晃动"窗口内 shake 过
            if now - self._recent_shake.get(oid, 0.0) > self.PROX_SHAKE_WINDOW:
                continue
            dx = m.x - other.x
            dy = m.y - other.y
            d2 = dx * dx + dy * dy
            if nearest is None or d2 < nearest_d2:
                nearest = oid
                nearest_d2 = d2
        if nearest is None:
            return False
        if nearest_d2 > self.PROX_HI_DISTANCE * self.PROX_HI_DISTANCE:
            return False
        pair = tuple(sorted((uid, nearest)))
        if now - self._prox_last.get(pair, 0.0) < self.PROX_HI_COOLDOWN:
            return False
        other = self.room.members[nearest]
        # 复制对方降临 sig（origin）进自己背包 owned + 切 current 展示。origin <0 时回退用对方 current。
        a_origin = m.sig_origin if m.sig_origin >= 0 else m.sig_particle
        b_origin = other.sig_origin if other.sig_origin >= 0 else other.sig_particle
        if a_origin < 0 and b_origin < 0:
            return False   # 双方都没 sig → 无可复制
        if b_origin >= 0:
            m.sig_owned.add(b_origin)       # 收进背包（永久，之后 S 屏可切回）
            m.sig_particle = b_origin       # 切 current 展示“我复制了对方”
        if a_origin >= 0:
            other.sig_owned.add(a_origin)
            other.sig_particle = a_origin
        self._prox_last[pair] = now
        self._recent_shake.pop(uid, None)
        self._recent_shake.pop(nearest, None)
        log.info("SIG copy %s<-%s / %s<-%s", uid, b_origin, nearest, a_origin)
        # S→C 告诉双方各自复制到的 sig
        self.client.publish(
            config.topic("sig", uid),
            json.dumps({"particle": m.sig_particle, "action": m.sig_action, "from": other.nick}),
            qos=1,
        )
        self.client.publish(
            config.topic("sig", nearest),
            json.dumps({"particle": other.sig_particle, "action": other.sig_action, "from": m.nick}),
            qos=1,
        )
        # 大屏：双方播作 + 粒子（各自显示复制后的 sig）
        self._emit_profile_update(m)
        self._emit_profile_update(other)
        self._emit({
            "type": "sig_copy",
            "a": uid, "b": nearest,
            "a_particle": m.sig_particle, "b_particle": other.sig_particle,
            "ts": now_ms(),
        })
        return True

    def sweep_hi_timeouts(self) -> None:
        """周期性清理超时未决 HI，通知发起方收尾（由 status loop 驱动）。"""
        for requester, _responder in self.hi.sweep_expired():
            if requester in self.room.members:
                log.info("HI timeout expired for %s", requester)
                self.client.publish(
                    config.topic("hi", "result", requester),
                    json.dumps({"event": "expired"}),
                    qos=1,
                )

    def _handle_hi_cancel(self, data: dict) -> None:
        """发起方撤销未决 HI → cancel pending，对方 accept 时会因无 pending 静默丢，
        不换色（防单边换色 — Codex P3）。被邀方的 incoming 屏自行 30s 超时收尾。"""
        requester = data.get("requester")
        if requester and self.hi.cancel(requester):
            log.info("HI cancelled by %s", requester)

    def _handle_jump(self, data: dict) -> None:
        uid = data.get("uid")
        if not uid or uid not in self.room.members:
            return
        # Per-person bounce on big screen
        self._emit({"type": "jump", "uid": uid, "ts": now_ms()})
        n = self.jump.add(uid)
        if self.jump.should_burst() and time.monotonic() - self._jump_last_burst > 3.0:
            self._jump_last_burst = time.monotonic()
            log.info("JUMP burst (n=%d)", n)
            self._emit({"type": "jump_burst", "count": n, "ts": now_ms()})

    def _handle_shake(self, data: dict) -> None:
        """move 模式近距 shake（与 JUMP 独立）→ 记时间 + 试近距 sig 交换。"""
        uid = data.get("uid")
        if not uid or uid not in self.room.members:
            return
        self._recent_shake[uid] = time.monotonic()
        self._try_sig_exchange(uid)

    ANON_COOLDOWN = 60.0  # 每人每分钟最多 1 条
    ANON_LEN_MAX = 30

    def _handle_anon(self, data: dict) -> None:
        uid = data.get("uid")
        text = (data.get("text") or "")[: self.ANON_LEN_MAX].strip()
        if not uid or not text:
            return
        now = time.monotonic()
        if now - self._anon_last.get(uid, 0) < self.ANON_COOLDOWN:
            return
        self._anon_last[uid] = now
        log.info("ANON %s: %s", uid, text)
        # 服务端剥离身份后广播（大屏只见匿名）
        self._emit({"type": "anon_msg", "text": text, "ts": now_ms()})

    def _handle_sig(self, data: dict) -> None:
        """S 屏切 current（彩蛋）→ 只能切到 owned 背包里的粒子。未拥有静默拒。"""
        uid = data.get("uid")
        m = self.room.members.get(uid) if uid else None
        if m is None:
            return
        particle = int(data.get("particle", -1))
        action = int(data.get("action", m.sig_action))
        if not (0 <= particle <= 3):
            return
        if particle not in m.sig_owned:
            return   # 不在背包里→不允许凭空切（只能先近距复制获得）
        if not (-1 <= action <= 2):
            action = m.sig_action
        m.sig_particle = particle
        m.sig_action = action
        m.last_seen = now_ms()
        self._emit({
            "type": "sig_cast",
            "uid": uid,
            "particle": particle,
            "action": action,
            "ts": now_ms(),
        })

    # ─────────────────────────────────────────────────────────────────────
    # Phase 3 reading（按需：设备进 P3 → 请求 → 线程池生成 → 推回 + 大屏）
    # ─────────────────────────────────────────────────────────────────────
    def _handle_reading_request(self, data: dict) -> None:
        """设备进 Phase 3 时按需请求"今天的你"。已分岛才受理；缓存命中直接回。

        payload: {uid}
        """
        uid = data.get("uid")
        m = self.room.members.get(uid) if uid else None
        if m is None or m.island < 0 or m.spectrum is None:
            return  # 未分岛不该有 reading
        gen = m.gen   # generation guard：leave/rejoin 后新 Member 有新 gen（单调递增，不会撞）
        with self._reading_lock:
            if m.reading is not None:
                cached = True
            elif (uid, gen) in self._reading_inflight:
                return  # 该 generation 正在生成，忽略重复请求（旧 gen 的 inflight 不挡新 gen — 修 Codex P1）
            else:
                cached = False
                self._reading_inflight.add((uid, gen))
        if cached:
            self._publish_reading(uid, m)   # 缓存命中（重进 P3 / 重连）→ 直接回
            return
        # 快照旅程数据（_handle_reading_request 跑在 paho 线程串行，join/leave/quiz 不并发；
        # 唯一并发是 event-loop 线程的 _reading_done，它只写 m.reading/inflight，不写这些）。
        nick, island = m.nick, m.island
        spectrum_colors = [c for c in m.spectrum.as_list() if c]
        hi_count = m.hi_count
        # v3：优先用 baked seed；legacy quiz 成员（无 seed）回落从 quiz 答案派生一句倾向。
        seed = m.seed
        if not seed and m.quiz_answers:
            a0 = m.quiz_answers[0]
            seed = ("leans toward people/home" if a0 == 0 else
                    "leans toward new places" if a0 == 1 else "leans toward quiet/solo")
        log.info("READING gen %s (%s) island=%d hi=%d", nick, uid, island, hi_count)
        self._reading_pool.submit(
            self._reading_worker, uid, gen, nick, island, spectrum_colors, hi_count, seed)

    def _reading_worker(self, uid, gen, nick, island, spectrum_colors, hi_count, seed) -> None:
        """线程池里跑：调 CopilotX 生成 → 投递回主循环存缓存 + 发布。永不抛。"""
        try:
            result = generate_reading(nick, island, spectrum_colors, hi_count, seed)
        except Exception:
            log.exception("reading worker crashed for %s", uid)
            result = None
        # 回 event loop 线程：安全改 Member + 发布（避免与 paho 线程竞争 Room）。
        self.loop.call_soon_threadsafe(self._reading_done, uid, gen, result)

    def _reading_done(self, uid: str, gen: int, result: dict | None) -> None:
        # 跑在 event-loop 线程。member re-fetch 放锁内，与 inflight 清理 / m.reading 写原子化
        # （修 Codex P2：避免 fetch 与 guard 之间与 paho leave/rejoin 交错）。
        with self._reading_lock:
            self._reading_inflight.discard((uid, gen))   # 只清自己这个 generation 的 key
            m = self.room.members.get(uid)
            # generation guard：member 已 leave/rejoin（gen 变）→ 旧旅程结果作废，
            # 不缓存到新 member 上（修 Codex P1：慢 AI + 重连会让旧 reading 落错人）。
            if m is None or m.gen != gen or m.island < 0 or m.spectrum is None:
                return
            if result is None:
                from .islands.reading import _fallback
                result = _fallback(m.island)
            m.reading = result
        self._publish_reading(uid, m)

    def _publish_reading(self, uid: str, m) -> None:
        """S→C 推 reading 给设备 + 大屏 reading_reveal。

        双语 9 英 + 9 中（Designer 原版 3 页×3 行）。设备 payload 只发文本字段
        （title/title_cn/core_cn/lines[9]/lines_cn[9]），**不发 spectrum/color/island**
        —— 设备 P3-02 色块读本地 g_collection，省字节让 9+9 行塞进 PubSubClient 1024B buffer。
        大屏 reading_reveal 仍发 title/lines[:3]（前 3 行英文）+ spectrum 等 → P4b hover card 不改不崩。
        """
        spectrum = m.spectrum.as_list() if m.spectrum else []
        device_payload = {
            "title": m.reading.get("title", ""),
            "title_cn": m.reading.get("title_cn", ""),
            "core_cn": m.reading.get("core_cn", ""),
            "lines": m.reading.get("lines", []),
            "lines_cn": m.reading.get("lines_cn", []),
        }
        # ensure_ascii=False → 中文发紧凑 UTF-8（3 字节/字）而非 `\uXXXX`（6 字节），
        # 9+9 双语包从 ~946B 降到 ~766B，防 PubSubClient buffer 溢出静默丢包（设备收不到）。
        self.client.publish(config.topic("reading", uid),
                            json.dumps(device_payload, ensure_ascii=False), qos=1)
        self._emit({
            "type": "reading_reveal",
            "uid": uid, "nick": m.nick,
            "title": device_payload["title"],
            "title_cn": device_payload.get("title_cn", ""),
            "lines": device_payload["lines"],
            "lines_cn": device_payload.get("lines_cn", []),
            "island": m.island, "color": m.island_color,
            "spectrum": spectrum, "ts": now_ms(),
        })

    # ─────────────────────────────────────────────────────────────────────
    # 发布 / 桥接
    # ─────────────────────────────────────────────────────────────────────
    def publish_status(self) -> None:
        payload = json.dumps({"count": self.room.count, "ts": now_ms()})
        self.client.publish(config.topic("status"), payload, qos=0)
        self._emit({"type": "status", "count": self.room.count, "ts": now_ms()})

    def publish_roster(self) -> None:
        """在线名册（retain）→ 设备本地缓存，供键入 `HI <nick>` 时自动补全 / 校验。

        只含已分岛成员（未分岛不是合法 HI 目标）。retain=true 让晚到设备也能拿到。
        """
        members = [
            {"uid": m.uid, "nick": m.nick, "island": m.island}
            for m in self.room.members.values() if m.island >= 0
        ]
        self.client.publish(
            config.topic("roster"),
            json.dumps({"members": members, "ts": now_ms()}),
            qos=1, retain=True,
        )

    def publish_phase(self, phase: int) -> None:
        """全场阶段切换（host 控制）。S→broadcast + 大屏。"""
        self.client.publish(config.topic("phase"), json.dumps({"phase": phase}), qos=1, retain=True)
        self._emit({"type": "phase_change", "phase": phase, "ts": now_ms()})

    # action -> (stage key, on/off)：开关型动作先更新持久态，再广播。
    _STAGE_TOGGLE = {
        "dim": ("dim", True), "undim": ("dim", False),
        "reveal": ("reveal", True), "unreveal": ("reveal", False),
        "photo": ("photo", True), "unphoto": ("photo", False),
    }

    def stage_state(self) -> dict:
        """当前 Host 控场持久态（供 snapshot 给晚到客户端追赶）。"""
        return dict(self._stage)

    def emit_stage(self, action: str) -> None:
        """Admin 控场动作（DIM/REVEAL/PHOTO 等）→ WS 广播给所有大屏 + 观众页同步。

        纯 WS 视觉效果，不落 MQTT（设备不关心）。鉴权在 HTTP 端点 /admin/stage 做。
        开关型动作（dim/reveal/photo）同步更新持久态 → 后连入客户端从 snapshot 追赶（P1-2）。
        """
        toggle = self._STAGE_TOGGLE.get(action)
        if toggle is not None:
            self._stage[toggle[0]] = toggle[1]
        self._emit({"type": "stage", "action": action, "ts": now_ms()})

    def publish_notice(self, text: str, level: str = "info") -> None:
        self.client.publish(config.topic("sys", "notice"),
                            json.dumps({"text": text, "level": level}), qos=1)

    def _emit(self, event: dict) -> None:
        """从网络线程把事件安全投递给主循环做 WS 广播。"""
        asyncio.run_coroutine_threadsafe(self.ws.broadcast(event), self.loop)
