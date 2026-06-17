"""HiEngine 单元测试：pending 互斥 / 超时 sweep / cancel。不依赖 broker。"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from loiter.islands.hi import HiEngine


def test_hi_engine_timeout_and_cancel():
    eng = HiEngine()
    eng.PENDING_TIMEOUT = 0.05
    assert eng.request("A", "B") is True
    assert eng.request("A", "C") is False, "同一发起者不能并发两个 pending"
    import time
    time.sleep(0.1)
    assert eng.sweep_expired() == [("A", "B")], "超时应返回 (A,B)"
    assert eng.sweep_expired() == [], "已清理不应重复返回"
    assert eng.request("X", "Y") is True
    assert eng.cancel("X") is True
    assert eng.cancel("X") is False
    assert eng.sweep_expired() == []


def test_hi_respond_tristate():
    eng = HiEngine()
    # 无 pending → None（修 Codex P1：区分拒绝 vs 无 pending）
    assert eng.respond("A", "B", True) is None
    assert eng.respond("A", "B", False) is None
    # 有 pending + accept → True
    assert eng.request("A", "B") is True
    assert eng.respond("A", "B", True) is True
    assert eng.respond("A", "B", True) is None, "已消费不应再命中"
    # 有 pending + decline → False
    assert eng.request("A", "B") is True
    assert eng.respond("A", "B", False) is False
    # responder 不符 → None
    assert eng.request("A", "B") is True
    assert eng.respond("A", "Z", True) is None


def test_hi_respond_expired():
    # 修 Codex P1：pending 超时后 respond 应返回 None（不靠外部 sweep 兜底）
    eng = HiEngine()
    eng.PENDING_TIMEOUT = 0.05
    assert eng.request("A", "B") is True
    import time
    time.sleep(0.1)
    assert eng.respond("A", "B", True) is None, "超时 pending 的迟到 accept 不应成立"
    # pending 应已被消费，不残留
    assert eng.sweep_expired() == []


if __name__ == "__main__":
    test_hi_engine_timeout_and_cancel()
    test_hi_respond_tristate()
    test_hi_respond_expired()
    print("✅ HiEngine timeout/cancel/tristate/expired 单测通过")
