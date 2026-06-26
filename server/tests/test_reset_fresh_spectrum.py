"""设备 Reset（硬件重启）后服务端 spectrum 归一 —— fresh join 标志。

Bug：设备 Reset = 硬件重启 → 本地 collection 归一到本色（1 色）；但同 uid rejoin 时
服务端 `_apply_profile` 因 `m.island>=0` 短路，之前 HI 攒的色原封不动留着 → 大屏显 N 色、
设备显 1 色（状态分裂）。修复 = 输名确认的 join 带 `fresh:true`，服务端归一 spectrum
（+hi_count+reading 缓存）；25s 心跳 join 不带 fresh → 不重置（防每 25s 抹掉 HI 攒的色）。

CI 可跑（不连 broker）：stub client 捕获 publish；_emit 投非运行 loop 仅产 warning 不崩。
"""
import asyncio
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

os.environ["LOITER_NPC_ENABLED"] = "false"
_TMP_DB = os.path.join(tempfile.mkdtemp(prefix="loiter-test-reset-fresh-"), "profiles.db")
os.environ["LOITER_PROFILE_DB"] = _TMP_DB

from loiter.room import Room
from loiter.ws import WSManager
from loiter.mqtt_bridge import MqttBridge
from loiter.profile_store import store


class _CaptureClient:
    def __init__(self):
        self.published = []

    def publish(self, topic, payload, qos=0, retain=False):
        self.published.append((topic, json.loads(payload)))


def _bridge():
    loop = asyncio.new_event_loop()
    b = MqttBridge(Room("hall"), WSManager(), loop)
    b.client = _CaptureClient()
    return b, loop


def _joined_with_colors(b, uid="A", nick="PP", pid="p1"):
    """构造一个已分岛 + 攒了 2 个他人色（filled=3）的成员。"""
    b.room.join(uid, nick)
    m = b.room.members[uid]
    b._apply_profile(m, {"profile_id": pid})
    assert m.island >= 0
    m.spectrum.add_from("B", "#101010")
    m.spectrum.add_from("C", "#202020")
    m.hi_count = 2
    m.reading = {"title": "OLD"}
    assert m.spectrum.filled == 3
    return m


def test_fresh_join_resets_spectrum():
    """Reset 重进（fresh=true，改名 PP→OO）→ spectrum 归一到本色 + hi_count/reading 清。"""
    store.reset()
    b, loop = _bridge()
    m = _joined_with_colors(b)
    b.client.published.clear()
    b._handle_join({"uid": "A", "nick": "OO", "profile_id": "p1", "fresh": True})
    assert m.spectrum.filled == 1, "Reset 后只剩本色"
    assert m.spectrum.as_list()[0] == m.island_color, "本色保留"
    assert m.hi_count == 0, "hi_count 同步清"
    assert m.reading is None, "reading 缓存清，Phase 3 重生成"
    assert m.nick == "OO", "新昵称落定"
    # 大屏刷新（island_assign 携归一后的 spectrum）+ 设备重揭晓（island push）
    topics = [t for t, _ in b.client.published]
    assert any("island" in t for t in topics), "应重推 island/<uid>"
    loop.close()


def test_heartbeat_join_does_not_reset():
    """25s 心跳 join（无 fresh）→ 不重置，HI 攒的色保留。"""
    store.reset()
    b, loop = _bridge()
    m = _joined_with_colors(b)
    b._handle_join({"uid": "A", "nick": "PP", "profile_id": "p1"})  # 无 fresh
    assert m.spectrum.filled == 3, "心跳不该抹掉 HI 攒的色"
    assert m.hi_count == 2, "心跳不该清 hi_count"
    loop.close()


def test_fresh_join_noop_when_only_home_color():
    """已是本色（无攒色）+ fresh → 不必重推（filled<=1 且 hi_count=0 且无 reading）。"""
    store.reset()
    b, loop = _bridge()
    b.room.join("A", "PP")
    m = b.room.members["A"]
    b._apply_profile(m, {"profile_id": "p1"})
    assert m.spectrum.filled == 1
    b.client.published.clear()
    b._handle_join({"uid": "A", "nick": "PP", "profile_id": "p1", "fresh": True})
    assert m.spectrum.filled == 1
    # 无变化 → 不触发 island 重推（避免无谓广播）
    topics = [t for t, _ in b.client.published]
    assert not any("island" in t for t in topics), "无攒色时 fresh 不必重推"
    loop.close()
