"""空字符串 profile_id 经 bridge 层的分岛/重推路径（修 Codex P2/P3）。

v3' 设备若 baked profile_id 为空（未经 skill 烧录 / 烧录异常），join {profile_id:""}
仍须能分到岛，且 fallback key（uid: 派生）要写回 m.profile_id，使后续 _push_island 重取
reason、notify_reason_ready 匹配都成立（不能把空串写回 m.profile_id）。

CI 可跑（不连 broker）：直接构造 MqttBridge，stub client 捕获 island push。
profile_store 单例在 import 时按 LOITER_PROFILE_DB 建库 → 必须在 import 前设 env + 用临时库。
"""
import asyncio
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# ⚠️ 必须在 import loiter.* 之前设 env：profile_store 单例 import 即建库
os.environ["LOITER_NPC_ENABLED"] = "false"
# 每个进程独立临时库（mkdtemp 唯一目录）→ 将来上 pytest-xdist 并行也不撞库（Codex nit）
_TMP_DB = os.path.join(tempfile.mkdtemp(prefix="loiter-test-empty-pid-"), "profiles.db")
os.environ["LOITER_PROFILE_DB"] = _TMP_DB

from loiter.room import Room
from loiter.ws import WSManager
from loiter.mqtt_bridge import MqttBridge
from loiter.profile_store import store


class _CaptureClient:
    """stub paho client：只捕获 island push payload。"""
    def __init__(self):
        self.published = []

    def publish(self, topic, payload, qos=0):
        self.published.append((topic, json.loads(payload)))


def _bridge():
    loop = asyncio.new_event_loop()
    b = MqttBridge(Room("hall"), WSManager(), loop)
    b.client = _CaptureClient()
    return b, loop


def test_empty_pid_adopts_and_binds_fallback_key():
    """join {profile_id:""} → 分岛 + m.profile_id 绑 uid 派生 key + reason 非空。"""
    store.reset()
    b, loop = _bridge()
    b.room.join("A", "ALICE")
    m = b.room.members["A"]
    assert b._apply_profile(m, {"profile_id": ""}) is True, "空 pid 应能新分岛"
    assert m.island >= 0, "空 pid 必须分到岛（不在仪式核心静默失败）"
    assert m.profile_id == "uid:A", "fallback key 须写回 m.profile_id，不是空串"
    assert m.reason_en and m.reason_cn, "adopt 带模板双语 reason"
    loop.close()


def test_empty_pid_push_island_refetches_reason():
    """_push_island 凭 m.profile_id(=uid:A) 能从 store 重取 reason（修 Codex P2）。"""
    store.reset()
    b, loop = _bridge()
    b.room.join("A", "ALICE")
    m = b.room.members["A"]
    b._apply_profile(m, {"profile_id": ""})
    m.reason_en = ""  # 模拟落定时为空，验证 push 前从 store 重取
    m.reason_cn = ""
    b._push_island(m)
    assert b.client.published, "应 push island/<uid>"
    _, payload = b.client.published[-1]
    assert payload["island"] == m.island
    assert payload["reason_en"] and payload["reason_cn"], "_push_island 须从 store 重取到 reason"
    loop.close()


def test_empty_pid_notify_reason_ready_matches():
    """notify_reason_ready 凭 fallback key 能匹配该 member 并重推（修 Codex P2）。"""
    store.reset()
    b, loop = _bridge()
    b.room.join("A", "ALICE")
    m = b.room.members["A"]
    b._apply_profile(m, {"profile_id": ""})
    b.client.published.clear()
    b.notify_reason_ready("uid:A")
    assert b.client.published, "notify_reason_ready 应匹配 uid:A 并重推 island"
    loop.close()


def test_empty_pid_rejoin_same_island():
    """同 uid 空 pid rejoin → 同岛（adopt 幂等，counter 不重复 +1）。"""
    store.reset()
    b, loop = _bridge()
    b.room.join("A", "ALICE")
    m1 = b.room.members["A"]
    b._apply_profile(m1, {"profile_id": ""})
    island1 = m1.island
    b.room.leave("A")
    b.room.join("A", "ALICE2")
    m2 = b.room.members["A"]
    b._apply_profile(m2, {"profile_id": ""})
    assert m2.island == island1, "同 uid rejoin 必须同岛"
    loop.close()


def test_missing_profile_id_field_is_legacy():
    """无 profile_id 字段（真旧设备）→ return False，不分岛（走 legacy quiz/done）。"""
    store.reset()
    b, loop = _bridge()
    b.room.join("A", "ALICE")
    m = b.room.members["A"]
    assert b._apply_profile(m, {}) is False, "无 profile_id 字段应走 legacy"
    assert m.island < 0, "legacy 路径不应在此分岛"
    loop.close()
