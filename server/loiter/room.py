"""房间状态机 — Loiter 的唯一房间状态权威。

只管内存中的实时在线状态；持久化（SQLite）是 P1，留 hook。
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field


def now_ms() -> int:
    return int(time.time() * 1000)


@dataclass
class Member:
    uid: str
    nick: str
    joined_at: int
    msg_count: int = 0
    last_seen: int = field(default_factory=now_ms)


class Room:
    """内存房间状态。线程安全留给调用方（MQTT 回调单线程串行处理）。"""

    def __init__(self, room_id: str):
        self.room_id = room_id
        self.members: dict[str, Member] = {}
        self.total_messages = 0

    # --- 成员生命周期 ---
    def join(self, uid: str, nick: str) -> Member:
        """上线。重复 join（重连）只刷新昵称/时间，不重置 msg_count。"""
        m = self.members.get(uid)
        if m is None:
            m = Member(uid=uid, nick=nick, joined_at=now_ms())
            self.members[uid] = m
        else:
            m.nick = nick or m.nick
            m.last_seen = now_ms()
        return m

    def leave(self, uid: str) -> Member | None:
        return self.members.pop(uid, None)

    def record_msg(self, uid: str) -> Member | None:
        m = self.members.get(uid)
        if m is not None:
            m.msg_count += 1
            m.last_seen = now_ms()
        self.total_messages += 1
        return m

    # --- 快照 ---
    @property
    def count(self) -> int:
        return len(self.members)

    def snapshot(self) -> dict:
        """给大屏（WebSocket）的全量快照。"""
        return {
            "room": self.room_id,
            "count": self.count,
            "total_messages": self.total_messages,
            "members": [
                {
                    "uid": m.uid,
                    "nick": m.nick,
                    "joined_at": m.joined_at,
                    "msg_count": m.msg_count,
                }
                for m in self.members.values()
            ],
            "ts": now_ms(),
        }
