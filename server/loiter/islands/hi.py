"""HI 握手 + JUMP 聚合 + 匿名公屏 —— Phase 2 玩法核心。

服务端权威：HI 必须双向确认才换色；JUMP 在时间窗口内聚合 N 人；
anon 由服务端剥离身份后广播。

P0：引擎骨架 + 占位方法。完整链路（含 MQTT 接线、彩虹弧事件）待 P3。
"""
from __future__ import annotations

import time


class HiEngine:
    """HI 握手状态机。

    pending[requester_uid] = (responder_uid, ts)  —— 一次只能 pending 一个，30s 超时。
    字段语义：requester = 发起 HI 的人，responder = 被邀请回应的人。
    """

    PENDING_TIMEOUT = 30.0  # 秒

    def __init__(self) -> None:
        self._pending: dict[str, tuple[str, float]] = {}

    def request(self, requester_uid: str, responder_uid: str) -> bool:
        """发起 HI。已有未决请求则拒绝。返回是否受理。"""
        self._gc()
        if requester_uid in self._pending:
            return False
        self._pending[requester_uid] = (responder_uid, time.monotonic())
        return True

    def respond(self, requester_uid: str, responder_uid: str, accept: bool) -> bool | None:
        """responder 回应 requester 发起的 HI。三态返回（修 Codex P1：区分拒绝 vs 无 pending）：

          True  = 有效 pending + accept   → 握手成立
          False = 有效 pending + decline  → 发起方应收 declined
          None  = 无匹配 pending（超时被 GC / 从未发起 / responder 不符）→ 静默丢弃

        无论 accept 与否，匹配到的 pending 都被消费掉（拒绝 = 一次完整应答，不留悬挂）。
        """
        pend = self._pending.get(requester_uid)
        if not pend or pend[0] != responder_uid:
            return None
        # 同步判超时：30s 到期后、下次 sweep（5s 粒度）前的迟到 accept 不该换色
        # （修 Codex P1：respond 自身把过期 pending 当 None 丢，不靠外部 sweep 兜底）。
        if time.monotonic() - pend[1] > self.PENDING_TIMEOUT:
            del self._pending[requester_uid]
            return None
        del self._pending[requester_uid]
        return bool(accept)

    def cancel(self, requester_uid: str) -> bool:
        """requester 主动撤销自己未决的 HI（设备 DEL 取消）。返回是否撤掉了一个 pending。"""
        return self._pending.pop(requester_uid, None) is not None

    def sweep_expired(self) -> list[tuple[str, str]]:
        """清理超时未决的 HI，返回 [(requester, responder), ...] 供上层通知发起方。

        这是唯一的超时通知入口（request/respond 的内部 _gc 静默清理不发通知）。
        """
        now = time.monotonic()
        dead = [(u, resp) for u, (resp, ts) in self._pending.items()
                if now - ts > self.PENDING_TIMEOUT]
        for u, _ in dead:
            del self._pending[u]
        return dead

    def _gc(self) -> None:
        now = time.monotonic()
        expired = [u for u, (_, ts) in self._pending.items()
                   if now - ts > self.PENDING_TIMEOUT]
        for u in expired:
            del self._pending[u]


class JumpAggregator:
    """JUMP 集体动作聚合：滑动窗口内累计 jump 的 uid，达阈值触发全场特效。"""

    WINDOW = 10.0       # 秒
    TRIGGER_MIN = 5     # 窗口内至少 N 人（对齐 M5_PRD：进度 N/5，满 5 才 burst）

    def __init__(self) -> None:
        self._jumps: dict[str, float] = {}  # uid -> last jump monotonic

    def add(self, uid: str) -> int:
        """记一次 jump，返回当前窗口内的参与人数。"""
        now = time.monotonic()
        self._jumps[uid] = now
        self._jumps = {u: t for u, t in self._jumps.items() if now - t <= self.WINDOW}
        return len(self._jumps)

    def should_burst(self) -> bool:
        return len(self._jumps) >= self.TRIGGER_MIN
