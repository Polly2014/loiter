"""成就系统 — 纯内存规则引擎。

每个成就 = 一枚 Badge + 一条判定规则。判定在房间事件（join / msg）发生后
被调用；已解锁的不重复触发。返回需要推送的 Badge 列表，由 MqttBridge 负责
下发到 `loiter/hall/achievement/<uid>`（S→C）并广播给大屏（WS）。

持久化是 P1.5（SQLite）的事；这里先用内存 set，进程重启清零可接受。
"""
from __future__ import annotations

import time
from collections import defaultdict
from dataclasses import dataclass

from . import config


@dataclass(frozen=True)
class Badge:
    id: str
    title: str
    emoji: str
    desc: str


# 5 个内置成就（对齐 CLAUDE.md P1.2）
BADGES: dict[str, Badge] = {
    "first_join": Badge("first_join", "首位加入", "🥇", "第一个踏入大厅的人"),
    "century":    Badge("century",    "破百消息", "💯", "大厅消息数突破 100"),
    "night_owl":  Badge("night_owl",  "夜猫子",   "🦉", "凌晨 0–5 点还在水群"),
    "social":     Badge("social",     "社交达人", "🦋", "个人发言满 20 条"),
    "voyager":    Badge("voyager",    "频道环游", "🧭", "在全部 3 个频道都发过言"),
}

# 个人发言成就阈值
_SOCIAL_THRESHOLD = 20
# 大厅消息成就阈值
_CENTURY_THRESHOLD = 100


class AchievementEngine:
    """房间级成就判定。单线程串行调用（与 MQTT 回调同线程）。"""

    def __init__(self) -> None:
        self._unlocked: dict[str, set[str]] = defaultdict(set)   # uid -> badge ids
        self._channels: dict[str, set[str]] = defaultdict(set)   # uid -> 发过言的频道
        self._first_join_done = False
        self._century_done = False

    def _grant(self, uid: str, badge_id: str, out: list[Badge]) -> None:
        if badge_id in self._unlocked[uid]:
            return
        self._unlocked[uid].add(badge_id)
        out.append(BADGES[badge_id])

    def on_join(self, uid: str, was_empty: bool) -> list[Badge]:
        """成员上线。was_empty=进入前房间为空 → 候选『首位加入』。"""
        out: list[Badge] = []
        if was_empty and not self._first_join_done:
            self._first_join_done = True
            self._grant(uid, "first_join", out)
        return out

    def on_msg(
        self,
        uid: str,
        channel: str,
        ts_ms: int,
        msg_count: int,
        total_messages: int,
    ) -> list[Badge]:
        """成员发言后判定 night_owl / social / voyager / century。"""
        out: list[Badge] = []

        # 夜猫子：服务器本地时间 0–4 点
        # 注：假定服务端与参会者同时区（现场 Mac 在本地，成立）。
        #     若未来部署到 UTC 云端，需改用显式时区转换。
        hour = time.localtime(ts_ms / 1000).tm_hour
        if 0 <= hour < 5:
            self._grant(uid, "night_owl", out)

        # 社交达人：个人发言满阈值
        if msg_count >= _SOCIAL_THRESHOLD:
            self._grant(uid, "social", out)

        # 频道环游：在所有频道都发过言
        if channel in config.CHANNELS:
            self._channels[uid].add(channel)
        if len(self._channels[uid]) >= len(config.CHANNELS):
            self._grant(uid, "voyager", out)

        # 破百消息：大厅累计达阈值，颁给压线那条的发送者（全局一次）
        if not self._century_done and total_messages >= _CENTURY_THRESHOLD:
            self._century_done = True
            self._grant(uid, "century", out)

        return out

    def unlocked_for(self, uid: str) -> set[str]:
        return set(self._unlocked.get(uid, ()))
