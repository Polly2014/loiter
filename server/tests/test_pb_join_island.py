"""Phase B′ 集成：join 带 profile_id → server 查/adopt profile 分岛 + push island/reason。

默认 **skip**（CI 不跑）；需本地 broker+server 时显式开启：
    LOITER_IT=1 pytest server/tests/test_pb_join_island.py
server 须以 `LOITER_NPC_ENABLED=false` 起（reason/reading 走 fallback）；只许**一个** loiter-server
连同一 broker（client_id `loiter-server` 硬编码，两实例会重连风暴 → 非 retain 消息丢）。
broker host/port 可用 LOITER_IT_BROKER / LOITER_IT_PORT 覆盖。

本测用 orphan adopt 路径（join 带一个 server 没见过的 profile_id → server 轮转分岛 + 模板 reason），
免去先 POST /flash/profile 的 token 依赖。
"""
import json
import os
import time
import uuid

import pytest

import paho.mqtt.client as mqtt

pytestmark = pytest.mark.skipif(
    os.environ.get("LOITER_IT") != "1",
    reason="集成测试需本地 broker+server；设 LOITER_IT=1 显式开启",
)

BROKER = os.environ.get("LOITER_IT_BROKER", "127.0.0.1")
BPORT = int(os.environ.get("LOITER_IT_PORT", "1883"))
ROOM = "hall"


def _topic(*p):
    return "/".join(["loiter", ROOM, *p])


def test_join_profile_id():
    """join 带 profile_id → island/<uid> push（island≥0 + reason）+ roster 反映 + reading 可生成 + Reset 拉取。"""
    A = "pb-A"
    pid = "it-" + uuid.uuid4().hex[:8]
    island_msgs, roster, reading = [], [], []

    sub = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2, client_id="pb-sub")

    def on_msg(c, u, m):
        if m.topic == _topic("island", A):
            island_msgs.append(json.loads(m.payload.decode()))
        elif m.topic == _topic("roster"):
            roster.append(json.loads(m.payload.decode()))
        elif m.topic == _topic("reading", A):
            reading.append(json.loads(m.payload.decode()))

    sub.on_message = on_msg
    sub.connect(BROKER, BPORT, 30)
    sub.subscribe([(_topic("island", A), 1), (_topic("roster"), 1), (_topic("reading", A), 1)])
    sub.loop_start()
    time.sleep(0.5)

    pub = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2, client_id="pb-pub")
    pub.connect(BROKER, BPORT, 30)
    pub.loop_start()

    def P(t, d):
        pub.publish(_topic(*t), json.dumps(d), qos=1)
        time.sleep(0.3)

    try:
        # 1) join 带未知 profile_id → server adopt 分岛 + push island/<uid>（island≥0 + reason）
        P(["join"], {"uid": A, "nick": "PB", "profile_id": pid})
        time.sleep(0.5)
        assert island_msgs, "应收到 island/<uid> push"
        isl = island_msgs[-1]
        assert isl["island"] >= 0 and isl["color"], f"island push 异常: {isl}"
        assert isl["reason_en"] and isl["reason_cn"], "应带文艺 reason（fallback 也非空）"
        assigned = isl["island"]

        # 2) roster 反映该 island
        assert roster, "应收到 roster"
        me = [x for x in roster[-1]["members"] if x["uid"] == A]
        assert me and me[0]["island"] == assigned

        # 3) Phase 3 reading 可生成
        P(["reading", "request"], {"uid": A})
        time.sleep(1.5)
        assert reading and reading[0].get("title"), f"应收到 reading: {reading}"

        # 4) Reset 重进：profile/request 重新拉 island/reason（同岛，不重复轮转）
        island_msgs.clear()
        P(["profile", "request"], {"uid": A})
        time.sleep(0.4)
        assert island_msgs and island_msgs[-1]["island"] == assigned, "Reset 拉取应同岛"
    finally:
        P(["leave"], {"uid": A})
        sub.loop_stop()
        pub.loop_stop()
