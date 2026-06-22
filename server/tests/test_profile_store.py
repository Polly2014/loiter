"""Phase B′ profile_store 单测 —— SQLite 顺序轮转 / get / adopt / set_reason / reset，无需 broker。

NPC 关闭走 reason fallback（离线可测）。每个用例用独立临时 DB。
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ["LOITER_NPC_ENABLED"] = "false"

from loiter.profile_store import ProfileStore, N_ISLANDS


def _store():
    fd, path = tempfile.mkstemp(suffix=".db")
    os.close(fd)
    return ProfileStore(path), path


def test_sequential_rotation():
    s, _ = _store()
    islands = [s.create(f"t{i}")["island"] for i in range(N_ISLANDS * 2)]
    # 顺序轮转：0,1,2,3,4,5,0,1,2,3,4,5
    assert islands == list(range(N_ISLANDS)) * 2, islands


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


def test_adopt_orphan_rotates_and_persists():
    s, _ = _store()
    s.create("a")  # seq=0 → island 0, seq→1
    p = s.adopt("orphan-pid")  # seq=1 → island 1 + 模板 reason
    assert p["island"] == 1
    assert p["reason_en"] and p["reason_cn"], "orphan 应有模板 reason"
    # rejoin 同 pid → 同岛（adopt 幂等，不再轮转）
    again = s.adopt("orphan-pid")
    assert again["island"] == 1


def test_reset_clears_and_restarts_counter():
    s, _ = _store()
    s.create("a")
    s.create("b")
    s.reset()
    assert all(v == 0 for v in s.tally().values())
    # 计数器归零 → 重新从 island 0 开始
    assert s.create("c")["island"] == 0


def test_tally_counts_per_island():
    s, _ = _store()
    for i in range(8):
        s.create(f"t{i}")  # 0,1,2,3,4,5,0,1
    t = s.tally()
    assert t[0] == 2 and t[1] == 2 and t[2] == 1 and t[5] == 1


if __name__ == "__main__":
    for fn in (test_sequential_rotation, test_create_get_roundtrip, test_set_reason,
               test_adopt_orphan_rotates_and_persists, test_reset_clears_and_restarts_counter,
               test_tally_counts_per_island):
        fn()
    print("profile_store: all pass")
