"""房间状态机 — Loiter 的唯一房间状态权威（v2 · Islands of Color）。

只管内存中的实时在线状态；持久化（SQLite）留 hook。
v1 的 skills/pair 字段已移除；改为 v2 的 island/spectrum。
"""
from __future__ import annotations

import itertools
import time
from dataclasses import dataclass, field

from .islands.assignment import ISLANDS, island_spawn_point
from .islands.spectrum import Spectrum


# 进程内单调递增的 Member 代号 —— 每个新 Member 唯一（leave/rejoin 后必然不同）。
# 用作 reading 的 generation guard（joined_at 是 ms 分辨率，快速 rejoin 会撞，不可靠）。
_member_gen = itertools.count(1)


# 服务端逻辑画布尺寸。坐标是**服务端归一化空间**，仅用于 move 积分 / 将来的距离 gate。
# 大屏地图是 2752×1536（见 islands-of-color-PRD），渲染时自行把这套坐标映射到地图比例。
LOGICAL_CANVAS_W = 1920
LOGICAL_CANVAS_H = 1080


def _seed_position(uid: str) -> tuple[float, float]:
    """uid → 稳定初始位置（FNV-1a hash）。move 事件会拉平大屏/服务端各自的种子。"""
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
    last_seen: int = field(default_factory=now_ms)
    gen: int = field(default_factory=lambda: next(_member_gen))  # 唯一代号（reading guard）
    # v2 — Islands of Color
    island: int = -1                       # 0-5（quiz 后分配）；-1 = 未分配
    spectrum: Spectrum | None = None       # 5 格色彩收集（登岛后初始化）
    hi_count: int = 0                      # 完成的 HI 握手数
    quiz_answers: list[int] = field(default_factory=list)  # （legacy）旧 quiz 3 选择；v3 已弃，留作过渡兼容
    seed: str = ""                         # v3′ profile.text（喂 Phase 3 reading，替代 quiz_answers）
    profile_id: str = ""                   # v3′ 烧录 profile id（设备 baked，join 携带）
    reason_en: str = ""                    # v3′ Phase-1 文艺分岛 reason（server 预生成，push 给设备揭晓）
    reason_cn: str = ""
    reading: dict | None = None            # 缓存的 AI reading（按需生成一次，防重复打 CopilotX）
    avatar: dict = field(default_factory=dict)  # dress-up 形象（shape/color）
    sig_particle: int = -1                  # 当前展示的粒子（S 屏在 owned 内切换；JOIN/HI/大屏都用它）
    sig_action: int = -1
    sig_origin: int = -1                   # 降临时的 sig 粒子（首次确定后固定，近距复制传给别人的就是这个，永不变）
    sig_owned: set[int] = field(default_factory=set)  # 拥有的粒子背包（降临 1 个 + 近距复制收集；S 屏只能切 owned 内的）
    # 大屏位置（IMU move 累积，用于将来的距离 gate）
    x: float = 0.0
    y: float = 0.0

    @property
    def island_color(self) -> str:
        # 未分配岛屿 → 返回中性灰 fallback，避免大屏 CSS 变量空转（review: 小龙虾 #1）
        return ISLANDS[self.island].color if 0 <= self.island < len(ISLANDS) else "#888888"


class Room:
    """内存房间状态。线程安全留给调用方（MQTT 回调单线程串行处理）。"""

    def __init__(self, room_id: str):
        self.room_id = room_id
        self.members: dict[str, Member] = {}
        self._island_seq: dict[int, int] = {}   # 各岛已入岛人数（用于散布出生点）

    # --- 成员生命周期 ---
    def join(self, uid: str, nick: str) -> Member:
        """上线。重复 join（重连）只刷新昵称/时间，不重置进度。"""
        m = self.members.get(uid)
        if m is None:
            m = Member(uid=uid, nick=nick, joined_at=now_ms())
            m.x, m.y = _seed_position(uid)
            self.members[uid] = m
        else:
            m.nick = nick or m.nick
            m.last_seen = now_ms()
            if m.x == 0.0 and m.y == 0.0:
                m.x, m.y = _seed_position(uid)
        return m

    def leave(self, uid: str) -> Member | None:
        return self.members.pop(uid, None)

    def assign_island(self, uid: str, island_idx: int) -> Member | None:
        """quiz 完成 → 分配岛屿 + 初始化 5 格收集（本色登顶格）+ 放到岛屿区域。

        服务端是位置权威：x/y 设到岛屿中心附近（散布），大屏直接渲染（review Codex #3）。
        """
        m = self.members.get(uid)
        if m is not None and 0 <= island_idx < len(ISLANDS):
            # 幂等：已分过岛 → 只刷 last_seen，不重建 spectrum/坐标（防 retain 重连
            # 二次触发重置进度。reset 重玩须先 leave 再 join —— review Codex #4）。
            if m.island >= 0:
                m.last_seen = now_ms()
                return m
            m.island = island_idx
            m.spectrum = Spectrum(home_color=ISLANDS[island_idx].color)
            seq = self._island_seq.get(island_idx, 0)
            self._island_seq[island_idx] = seq + 1
            m.x, m.y = island_spawn_point(island_idx, seq)
            m.last_seen = now_ms()
        return m

    def touch(self, uid: str) -> Member | None:
        m = self.members.get(uid)
        if m is not None:
            m.last_seen = now_ms()
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
            "members": [
                {
                    "uid": m.uid,
                    "nick": m.nick,
                    "joined_at": m.joined_at,
                    "island": m.island,
                    "island_color": m.island_color,
                    "spectrum": m.spectrum.as_list() if m.spectrum else [],
                    "hi_count": m.hi_count,
                    "reading": m.reading,   # Phase 3 已生成的 reading（dict|None）→ 大屏重连不掉回 fake（review Codex）
                    "avatar": m.avatar,
                    "sig_particle": m.sig_particle,
                    "sig_action": m.sig_action,
                    "x": round(m.x, 1),
                    "y": round(m.y, 1),
                }
                for m in self.members.values()
            ],
            "ts": now_ms(),
        }
