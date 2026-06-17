"""房间状态机 — Loiter 的唯一房间状态权威。

只管内存中的实时在线状态；持久化（SQLite）是 P1，留 hook。
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field

from . import skills as skills_mod


# 服务端用的虚拟画布尺寸（只用于距离粗算；不要求与大屏像素严格一致）
LOGICAL_CANVAS_W = 1920
LOGICAL_CANVAS_H = 1080


def _seed_position(uid: str) -> tuple[float, float]:
    """与大屏 spawnNode 公式（hashStr ^ 0x9e3779b9）独立的随机种子。
    服务端 + 大屏会各自算各自的初始位置，但 move 事件会拉平。"""
    h = 2166136261
    for ch in uid:
        h = ((h ^ ord(ch)) * 16777619) & 0xFFFFFFFF
    h ^= 0x9e3779b9
    rx = (h & 0xFFFF) / 0xFFFF
    ry = ((h >> 16) & 0xFFFF) / 0xFFFF
    return (60 + rx * (LOGICAL_CANVAS_W - 120), 50 + ry * (LOGICAL_CANVAS_H - 120))


def now_ms() -> int:
    return int(time.time() * 1000)


@dataclass
class Member:
    uid: str
    nick: str
    joined_at: int
    msg_count: int = 0
    last_seen: int = field(default_factory=now_ms)
    png_b64: str = ""  # 大屏 AI 头像（彩色 PNG base64），重连后从快照恢复
    current_channel: str = "main"  # 当前所在频道（msg 时更新），大屏 ring/昵称色用
    # Sprint 7 — 技能融合
    skills: set[str] = field(default_factory=set)         # 已拥有的常规 skill（含 starter + pair 得到的）
    starter_skills: set[str] = field(default_factory=set) # 入场冻结的初始 2 个 skill（A+ 共享规则：只能交换 starter，集齐者不会成为"福音站"）
    paired_with: set[str] = field(default_factory=set)    # 已配对过的 uid（整场一次）
    contributed_to: set[str] = field(default_factory=set) # 我贡献过 starter 给的 uid（社交贡献排行）
    unlocked_ultimates: set[str] = field(default_factory=set)  # 已解锁的大招（element 名）
    has_omni: bool = False                                 # 是否已解锁万象归一
    # Sprint 7 — 大屏位置（用于 pair 距离 gate）
    x: float = 0.0
    y: float = 0.0


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
            # Sprint 7: 首次入场随机分配 2 个 starter skill
            m.skills = skills_mod.random_starter(2)
            m.starter_skills = set(m.skills)  # 冻结快照（永远不变）
            # Sprint 7: 初始位置（用于 pair 距离 gate）
            m.x, m.y = _seed_position(uid)
            self.members[uid] = m
        else:
            m.nick = nick or m.nick
            m.last_seen = now_ms()
            # Sprint 7: 老数据兜底——位置可能是 0/0（旧 Member 字段补出来的）
            if m.x == 0.0 and m.y == 0.0:
                m.x, m.y = _seed_position(uid)
            # Sprint 7: 老数据兜底——starter_skills 可能未初始化（迁移前的 member）
            if not m.starter_skills and m.skills:
                m.starter_skills = set(m.skills)
        return m

    def leave(self, uid: str) -> Member | None:
        return self.members.pop(uid, None)

    def set_avatar(self, uid: str, png_b64: str) -> None:
        """记住成员的 AI 头像，让重连大屏能从快照恢复，不必重新生成。"""
        m = self.members.get(uid)
        if m is not None:
            m.png_b64 = png_b64

    def record_msg(self, uid: str, channel: str = "main") -> Member | None:
        m = self.members.get(uid)
        if m is not None:
            m.msg_count += 1
            m.last_seen = now_ms()
            m.current_channel = channel
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
                    "png_b64": m.png_b64,
                    "current_channel": m.current_channel,
                    # Sprint 7: 技能融合状态（让大屏能渲染 skill chips 和排行）
                    "skills": sorted(m.skills),
                    "ultimates": sorted(m.unlocked_ultimates),
                    "has_omni": m.has_omni,
                    "progress": list(skills_mod.progress(m.skills)),  # [collected, total]
                    "paired_count": len(m.paired_with),
                    "contributed_count": len(m.contributed_to),
                    "starter_skills": sorted(m.starter_skills),
                }
                for m in self.members.values()
            ],
            "ts": now_ms(),
        }
