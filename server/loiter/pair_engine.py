"""Sprint 7 — 双机 shake 配对引擎（含 shake fingerprint 物理近距校验）。

流程:
  1. 双方各发 `/pair` → MQTT `pair/intent` → 服务端 enter_pairing()
     → 状态机进 WAITING_SHAKE，3s 超时；同时下发 `pair/result {phase:armed}`。
  2. 双方贴合一起摇 1.2s → 固件本地计算 shake fingerprint:
       - peaks: 摇晃次数（peak count, 0-10）
       - rhythm: 平均峰值间隔 ms（节奏快慢）
       - energy: 总能量（强度，所有样本 mag 之和）
     → 一条 `pair/shake` 上报 `{uid, peak_g, peaks, rhythm, energy}`。
  3. 服务端收到 shake 后，与其他在求偶模式的 uid 比较指纹相似度：
       similarity = 0.4*peaks_match + 0.4*rhythm_match + 0.2*energy_match
       命中阈值 PAIR_SIMILARITY_THRESHOLD (0.55) 即配对成功。
     这强制要求双方"真贴在一起摇"——远程同时摇也摇不出同一条曲线。
  4. 已配对过的 (A,B) 整场只能配一次 → 强制鼓励满场撞新人。

服务端是 ground truth。Cardputer 不持有 skill 集合，只显示推送内容。
"""
from __future__ import annotations

import logging
import time
from dataclasses import dataclass, field

from .room import Member, Room
from . import skills

log = logging.getLogger("loiter.pair")

# Tunables (small / explicit — review easily) -----------------------------
PAIRING_WINDOW_S: float = 3.0            # `/pair` 后 3s 等待 shake
PAIR_SHAKE_TOLERANCE_S: float = 1.5      # 双方 shake 时间差 ≤ 1.5s 算同步
SHAKE_THRESHOLD_G: float = 3.0           # peak_g 阈值（高于单人 emote 的 2.5）
PAIR_SIMILARITY_THRESHOLD: float = 0.55  # fingerprint 相似度阈值（0-1，0.55=中等）
PAIR_PROXIMITY_PX: float = 600.0         # 大屏角色距离 gate（逻辑像素 1920×1080；600px ≈ 1/3 屏宽，宽松值，保证现场 2-5 人测试不会误拒。后续接入大屏真位置同步后再收紧）


@dataclass
class ShakeFingerprint:
    """固件上报的 1.2s 摇晃指纹（3 个标量）。"""
    peak_g: float = 0.0     # 最大瞬时加速度（g）
    peaks: int = 0          # 摇晃次数（局部峰值计数）
    rhythm_ms: int = 0      # 平均峰值间隔 ms（0=单峰或无峰）
    energy: float = 0.0     # 总能量（sum of mag）


def fingerprint_similarity(a: ShakeFingerprint, b: ShakeFingerprint) -> float:
    """两条指纹的相似度 0-1。1.0 = 完全相同。

    分量权重 (0.4 peaks + 0.4 rhythm + 0.2 energy):
      - peaks 最重要：摇了几下，最能体现"是否同时同步"
      - rhythm 次重要：摇的快慢节奏，物理共谋才能同步
      - energy 较弱：力气大小，同一人左右手都会有差异，权重低
    """
    # peaks 匹配：差 0 → 1.0；差 ≥3 → 0
    peaks_match = max(0.0, 1.0 - abs(a.peaks - b.peaks) / 3.0)
    # rhythm 匹配：差 0 → 1.0；差 ≥300ms → 0；任一方为 0（单峰）则不可比
    if a.rhythm_ms == 0 or b.rhythm_ms == 0:
        rhythm_match = 1.0 if a.rhythm_ms == b.rhythm_ms else 0.0
    else:
        rhythm_match = max(0.0, 1.0 - abs(a.rhythm_ms - b.rhythm_ms) / 300.0)
    # energy 匹配：相对比值 ≥0.5 计分，<0.5 算 0
    bigger = max(a.energy, b.energy)
    smaller = min(a.energy, b.energy)
    if bigger == 0:
        energy_match = 0.0
    else:
        ratio = smaller / bigger
        energy_match = max(0.0, (ratio - 0.5) * 2)  # 0.5→0, 1.0→1.0
    return 0.4 * peaks_match + 0.4 * rhythm_match + 0.2 * energy_match


@dataclass
class _Waiting:
    """正在求偶模式的人。"""
    entered_at: float
    shake_at: float | None = None
    fp: ShakeFingerprint = field(default_factory=ShakeFingerprint)


@dataclass
class PairResult:
    """配对成功的快照。"""
    a_uid: str
    a_nick: str
    a_gained: set[str]
    a_new_ultimates: list[str]
    a_omni: bool
    b_uid: str
    b_nick: str
    b_gained: set[str]
    b_new_ultimates: list[str]
    b_omni: bool
    ts: float
    similarity: float = 0.0  # 这次配对的指纹相似度（信息上报，便于调试）


@dataclass
class PairRejection:
    """配对被拒 — 让前端能 toast 教学。"""
    a_uid: str
    b_uid: str
    similarity: float
    reason: str = "fingerprint_mismatch"  # 或 "too_far"
    distance: float = 0.0                 # 大屏距离（逻辑像素）；reason=too_far 时填


class PairEngine:
    """与 MqttBridge 同线程（paho 网络线程）调用，串行，免锁。"""

    def __init__(self, room: Room):
        self.room = room
        self._waiting: dict[str, _Waiting] = {}

    # ---- 公开 API ----
    def enter_pairing(self, uid: str) -> bool:
        """用户 /pair：进入 3s 求偶模式。已在求偶则刷新窗口。"""
        if uid not in self.room.members:
            return False
        self._waiting[uid] = _Waiting(entered_at=time.monotonic())
        log.info("PAIR intent %s (window %.1fs)", uid, PAIRING_WINDOW_S)
        return True

    def on_shake(self, uid: str, fp: ShakeFingerprint) -> PairResult | PairRejection | None:
        """
        用户 shake 信号到达（含 fingerprint）。只在求偶模式内有效。
        返回:
          - PairResult: 配对成功（指纹匹配）
          - PairRejection: 同步但指纹不匹配（"远程作弊"被挡）
          - None: 无事发生（不在求偶模式 / 强度不够 / 窗口里没人陪）
        """
        self._gc()
        w = self._waiting.get(uid)
        if w is None:
            return None  # 不在求偶模式
        if fp.peak_g < SHAKE_THRESHOLD_G:
            return None  # 强度不够
        now = time.monotonic()
        w.shake_at = now
        w.fp = fp
        log.info(
            "PAIR shake %s peak_g=%.2f peaks=%d rhythm=%dms energy=%.1f",
            uid, fp.peak_g, fp.peaks, fp.rhythm_ms, fp.energy,
        )
        # 找另一个：在窗口里、shake 时间差 ≤ tolerance、强度够 → 比指纹
        best: tuple[str, float] | None = None  # (uid, similarity)
        for other_uid, ow in self._waiting.items():
            if other_uid == uid:
                continue
            if ow.shake_at is None or ow.fp.peak_g < SHAKE_THRESHOLD_G:
                continue
            if abs(now - ow.shake_at) > PAIR_SHAKE_TOLERANCE_S:
                continue
            sim = fingerprint_similarity(fp, ow.fp)
            log.info("PAIR similarity %s ↔ %s = %.2f", uid, other_uid, sim)
            if best is None or sim > best[1]:
                best = (other_uid, sim)
        if best is None:
            return None  # 暂时没人陪
        other_uid, sim = best
        # Sprint 7: 距离 gate — 双方大屏角色必须靠近（≤ PAIR_PROXIMITY_PX）
        me = self.room.members.get(uid)
        other = self.room.members.get(other_uid)
        if me is not None and other is not None:
            import math
            dist = math.hypot(me.x - other.x, me.y - other.y)
            if dist > PAIR_PROXIMITY_PX:
                log.info("PAIR rejected (too_far) %s ↔ %s dist=%.0fpx > %.0f",
                         uid, other_uid, dist, PAIR_PROXIMITY_PX)
                return PairRejection(
                    a_uid=other_uid, b_uid=uid, similarity=sim,
                    reason="too_far", distance=dist,
                )
        # 兼容旧固件：任一方没有有效 fingerprint（peaks==0）→ 跳过指纹校验
        ow = self._waiting[other_uid]
        has_fp = fp.peaks > 0 and ow.fp.peaks > 0
        if has_fp and sim < PAIR_SIMILARITY_THRESHOLD:
            # 指纹不匹配 → 拒绝，**双方都留在 waiting 里**，可以再试
            log.info("PAIR rejected (fingerprint) %s ↔ %s sim=%.2f < %.2f",
                     uid, other_uid, sim, PAIR_SIMILARITY_THRESHOLD)
            return PairRejection(
                a_uid=other_uid, b_uid=uid, similarity=sim,
                reason="fingerprint_mismatch",
            )
        # 命中 → 配对成功
        result = self._fuse(other_uid, uid, now, sim)
        # 双方都清出 waiting
        self._waiting.pop(uid, None)
        self._waiting.pop(other_uid, None)
        return result

    def cancel(self, uid: str) -> None:
        self._waiting.pop(uid, None)

    def is_waiting(self, uid: str) -> bool:
        self._gc()
        return uid in self._waiting

    # ---- 内部 ----
    def _gc(self) -> None:
        """移除超时（>PAIRING_WINDOW_S）的 uid。"""
        now = time.monotonic()
        stale = [u for u, w in self._waiting.items()
                 if now - w.entered_at > PAIRING_WINDOW_S]
        for u in stale:
            del self._waiting[u]
            log.debug("PAIR window expired for %s", u)

    def _fuse(self, a_uid: str, b_uid: str, ts: float, similarity: float = 0.0) -> PairResult | None:
        """执行技能融合：A+ 改进版 — 只交换 starter_skills，避免"福音站"问题。

        - A 给 B 的：A 入场冻结的 starter_skills 中 B 没有的（最多 2 个）
        - B 给 A 的：B 入场冻结的 starter_skills 中 A 没有的（最多 2 个）
        - 集齐 16 个的人不会再次成为"全场福音"，因为只能传播 starter
        - 同时记录 contributed_to 让贡献排行可视化
        """
        a = self.room.members.get(a_uid)
        b = self.room.members.get(b_uid)
        if a is None or b is None:
            return None
        # 同一对整场只能配一次
        if b_uid in a.paired_with or a_uid in b.paired_with:
            log.info("PAIR rejected: %s ↔ %s already paired", a.nick, b.nick)
            return None
        a_before_ult = set(skills.completed_elements(a.skills))
        b_before_ult = set(skills.completed_elements(b.skills))
        a_before_omni = skills.is_omni(a.skills)
        b_before_omni = skills.is_omni(b.skills)
        # A+ 改进：只传 starter
        a_gained = b.starter_skills - a.skills
        b_gained = a.starter_skills - b.skills
        a.skills |= a_gained
        b.skills |= b_gained
        a.paired_with.add(b_uid)
        b.paired_with.add(a_uid)
        # 如果各自贡献了 starter 给对方，记入贡献排行
        if a_gained:
            b.contributed_to.add(a_uid)  # B 通过 starter 让 A 学到了
        if b_gained:
            a.contributed_to.add(b_uid)
        # 新解锁的大招/omni
        a_new_ults = [el for el in skills.completed_elements(a.skills) if el not in a_before_ult]
        b_new_ults = [el for el in skills.completed_elements(b.skills) if el not in b_before_ult]
        a_omni = skills.is_omni(a.skills) and not a_before_omni
        b_omni = skills.is_omni(b.skills) and not b_before_omni
        log.info(
            "PAIR fused %s ↔ %s | A+=%s(from B starter %s) | B+=%s(from A starter %s) | "
            "ult A=%s B=%s | omni A=%s B=%s",
            a.nick, b.nick,
            sorted(a_gained), sorted(b.starter_skills),
            sorted(b_gained), sorted(a.starter_skills),
            a_new_ults, b_new_ults, a_omni, b_omni,
        )
        return PairResult(
            a_uid=a_uid, a_nick=a.nick,
            a_gained=a_gained, a_new_ultimates=a_new_ults, a_omni=a_omni,
            b_uid=b_uid, b_nick=b.nick,
            b_gained=b_gained, b_new_ultimates=b_new_ults, b_omni=b_omni,
            ts=ts, similarity=similarity,
        )
