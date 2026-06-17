"""P4a reading generation guard 单测 —— leave/rejoin 后旧 reading 不落到新 Member（修 Codex P1）。

直接构造 MqttBridge（不连 broker），调 _reading_done 模拟慢 worker 完成。
"""
import asyncio
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from loiter.room import Room
from loiter.ws import WSManager
from loiter.mqtt_bridge import MqttBridge


def _bridge():
    loop = asyncio.new_event_loop()
    return MqttBridge(Room("hall"), WSManager(), loop), loop


def test_stale_reading_dropped_after_rejoin():
    b, loop = _bridge()
    room = b.room
    # 老 member 分岛
    room.join("A", "ALICE")
    room.assign_island("A", 0)
    old_gen = room.members["A"].gen
    # leave + rejoin → 新 member，新 gen，未分岛
    room.leave("A")
    room.join("A", "ALICE2")
    new_member = room.members["A"]
    assert new_member.gen != old_gen, "新 member 应有新 gen"
    # 慢 worker 用老 gen 完成 → 不该落到新 member
    b._reading_done("A", old_gen, {"title": "OLD", "lines": ["x", "y", "z"]})
    assert new_member.reading is None, "stale reading 不该落到新 member"
    assert ("A", old_gen) not in b._reading_inflight, "旧 gen 的 inflight key 应清理"
    loop.close()


def test_fresh_reading_attaches():
    b, loop = _bridge()
    room = b.room
    room.join("A", "ALICE")
    room.assign_island("A", 0)
    gen = room.members["A"].gen
    b._reading_done("A", gen, {"title": "NEW", "lines": ["a", "b", "c"]})
    assert room.members["A"].reading == {"title": "NEW", "lines": ["a", "b", "c"]}
    loop.close()


def test_unassigned_member_no_reading():
    b, loop = _bridge()
    room = b.room
    room.join("A", "ALICE")  # 未分岛
    gen = room.members["A"].gen
    b._reading_done("A", gen, {"title": "X", "lines": ["a"]})
    assert room.members["A"].reading is None, "未分岛不该缓存 reading"
    loop.close()


def test_old_inflight_does_not_block_new_gen():
    # 修 Codex P1：旧 gen 的 inflight 不该挡住新 member 的 request（inflight key = (uid,gen)）
    b, loop = _bridge()
    room = b.room
    submitted = []
    b._reading_pool.submit = lambda *a, **k: submitted.append(a)  # 拦截入队，只计数
    room.join("A", "ALICE")
    room.assign_island("A", 0)
    old_gen = room.members["A"].gen
    b._handle_reading_request({"uid": "A"})
    assert len(submitted) == 1, "首请求应入队"
    assert (("A", old_gen) in b._reading_inflight)
    # 同 gen 重复请求 → 不重复入队
    b._handle_reading_request({"uid": "A"})
    assert len(submitted) == 1, "同 gen 重复请求不应再入队"
    # leave/rejoin → 新 gen，新 request 应独立入队（不被旧 inflight 吃掉）
    room.leave("A")
    room.join("A", "ALICE2")
    room.assign_island("A", 1)
    new_gen = room.members["A"].gen
    assert new_gen != old_gen
    b._handle_reading_request({"uid": "A"})
    assert len(submitted) == 2, "新 gen request 应独立入队，不被旧 inflight 阻塞"
    assert (("A", new_gen) in b._reading_inflight)
    loop.close()


if __name__ == "__main__":
    test_stale_reading_dropped_after_rejoin()
    test_fresh_reading_attaches()
    test_unassigned_member_no_reading()
    test_old_inflight_does_not_block_new_gen()
    print("✅ reading generation guard 单测通过")
