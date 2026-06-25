"""Phase B′ profile_store 单测 —— SQLite hash 确定性分岛 / get / adopt / set_reason / reset，无需 broker。

NPC 关闭走 reason fallback（离线可测）。每个用例用独立临时 DB。
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ["LOITER_NPC_ENABLED"] = "false"

from loiter.profile_store import ProfileStore, N_ISLANDS
from loiter.islands.reading import _hash_island


def _store():
    fd, path = tempfile.mkstemp(suffix=".db")
    os.close(fd)
    return ProfileStore(path), path


def test_hash_island_deterministic():
    # B′：NPC 关 → create 无显式 island 走 hash(text)%6。同文本跨独立 store 同岛 + 在范围内。
    s1, _ = _store()
    s2, _ = _store()
    for t in ("warm and curious", "我喜欢安静", "", "x"):
        a = s1.create(t)["island"]
        b = s2.create(t)["island"]
        assert 0 <= a < N_ISLANDS
        assert a == b == _hash_island(t), (t, a, b)


def test_create_explicit_island_persisted():
    # main.py 语义选岛后显式传入 → 按传入値持久化、不可变（B′ 硬约束）。
    s, _ = _store()
    p = s.create("anything", island=4)
    assert p["island"] == 4
    assert s.get(p["profile_id"])["island"] == 4


def test_create_get_roundtrip():
    s, _ = _store()
    p = s.create("hello world")
    got = s.get(p["profile_id"])
    assert got and got["island"] == p["island"] and got["text"] == "hello world"
    assert s.get("nonexistent") is None


def test_set_reason():
    s, _ = _store()
    p = s.create("x")
    assert s.set_reason(p["profile_id"], "EN reason", "中文原因") is True
    got = s.get(p["profile_id"])
    assert got["reason_en"] == "EN reason" and got["reason_cn"] == "中文原因"
    assert s.set_reason("nope", "a", "b") is False


def test_adopt_orphan_hash_and_persists():
    s, _ = _store()
    p = s.adopt("orphan-pid")  # hash(pid)%6 定岛 + 模板 reason
    assert p["island"] == _hash_island("orphan-pid")
    assert p["reason_en"] and p["reason_cn"], "orphan 应有模板 reason"
    # rejoin 同 pid → 同岛（adopt 幂等，查现有行）
    again = s.adopt("orphan-pid")
    assert again["island"] == p["island"]


def test_reset_clears_then_same_text_same_island():
    s, _ = _store()
    first = s.create("a")["island"]
    s.create("b")
    s.reset()
    assert all(v == 0 for v in s.tally().values())
    # B′ Reset 安全：同文本 hash 不变 → 重建同岛
    assert s.create("a")["island"] == first == _hash_island("a")


def test_tally_sums_total():
    s, _ = _store()
    for i in range(8):
        s.create(f"t{i}")
    t = s.tally()
    assert sum(t.values()) == 8
    assert all(v >= 0 for v in t.values()) and set(t.keys()) <= set(range(N_ISLANDS))


if __name__ == "__main__":
    for fn in (test_hash_island_deterministic, test_create_explicit_island_persisted,
               test_create_get_roundtrip, test_set_reason,
               test_adopt_orphan_hash_and_persists, test_reset_clears_then_same_text_same_island,
               test_tally_sums_total):
        fn()
    print("profile_store: all pass")
