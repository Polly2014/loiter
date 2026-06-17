"""P4a reading 全链路验证（设备 reading/request → server 生成 → reading/<uid> + 大屏）。

需 server + broker 在跑，且 server 以 LOITER_NPC_ENABLED=false 启动（走 fallback，离线可测）。
跑法见 test_p3_hi_pipeline.py 注释。
"""
import json
import time

import paho.mqtt.client as mqtt

BROKER = "127.0.0.1"
ROOM = "hall"
A = "rd-A"
results = {A: []}


def topic(*p):
    return "/".join(["loiter", ROOM, *p])


def main():
    sub = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2, client_id="rd-sub")

    def on_msg(c, u, m):
        if m.topic == topic("reading", A):
            try:
                results[A].append(json.loads(m.payload.decode()))
            except Exception:
                pass

    sub.on_message = on_msg
    sub.connect(BROKER, 1883, 30)
    sub.subscribe(topic("reading", A), 1)
    sub.loop_start()
    time.sleep(0.5)

    pub = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2, client_id="rd-pub")
    pub.connect(BROKER, 1883, 30)
    pub.loop_start()

    def P(t, d):
        pub.publish(topic(*t), json.dumps(d), qos=1)
        time.sleep(0.3)

    # 1) 未分岛请求 reading → 应被忽略（无回复）
    P(["join"], {"uid": A, "nick": "ALICE"})
    P(["reading", "request"], {"uid": A})
    time.sleep(0.5)
    assert not results[A], "未分岛不该有 reading"

    # 2) 分岛后请求 → 收到 reading（fallback，结构正确）
    P(["quiz", "done"], {"uid": A, "answers": [0, 0, 0]})   # sum=0 → EMBER(0)
    P(["reading", "request"], {"uid": A})
    time.sleep(1.5)   # 等线程池 + fallback
    assert results[A], "分岛后应收到 reading"
    r = results[A][0]
    assert r["title"] and isinstance(r["lines"], list) and len(r["lines"]) == 3
    assert r["island"] == 0 and r["color"] == "#e84d3c"
    assert r["spectrum"][0] == "#e84d3c"

    # 3) 二次请求 → 缓存命中，立刻回（不重复生成）
    results[A].clear()
    P(["reading", "request"], {"uid": A})
    time.sleep(0.6)
    assert results[A], "缓存命中应直接回"
    assert results[A][0]["title"] == r["title"], "缓存内容一致"

    P(["leave"], {"uid": A})
    sub.loop_stop()
    pub.loop_stop()
    print("✅ P4a reading 全链路验证通过")


if __name__ == "__main__":
    main()
