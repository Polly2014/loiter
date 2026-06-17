"""P3 HI 握手全链路验证（模拟两台设备 A/B 经 mosquitto，需 server + broker 在跑）。

跑法：
  cd X-Workspace/loiter/server
  uv run python -m uvicorn loiter.main:app --host 0.0.0.0 --port 8099 --app-dir "$PWD" &  # 另开
  uv run python tests/test_p3_hi_pipeline.py
"""
import json
import time
import urllib.request

import paho.mqtt.client as mqtt

BROKER = "127.0.0.1"
ROOM = "hall"
A, B = "dev-A", "dev-B"
results = {A: [], B: []}


def topic(*p):
    return "/".join(["loiter", ROOM, *p])


def main():
    sub = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2, client_id="test-sub")

    def on_msg(c, u, m):
        try:
            d = json.loads(m.payload.decode())
        except Exception:
            return
        if m.topic == topic("hi", "result", A):
            results[A].append(d)
        elif m.topic == topic("hi", "result", B):
            results[B].append(d)

    sub.on_message = on_msg
    sub.connect(BROKER, 1883, 30)
    sub.subscribe([(topic("hi", "result", A), 1), (topic("hi", "result", B), 1),
                   (topic("roster"), 1)])
    roster = {"last": None}

    def on_msg2(c, u, m):
        if m.topic == topic("roster"):
            try:
                roster["last"] = json.loads(m.payload.decode())
            except Exception:
                pass
        else:
            on_msg(c, u, m)

    sub.on_message = on_msg2
    sub.loop_start()
    time.sleep(0.5)

    pub = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2, client_id="test-pub")
    pub.connect(BROKER, 1883, 30)
    pub.loop_start()

    def P(t, d):
        pub.publish(topic(*t), json.dumps(d), qos=1)
        time.sleep(0.3)

    def snap():
        with urllib.request.urlopen("http://127.0.0.1:8099/api/snapshot") as r:
            return json.load(r)

    def member(uid):
        for m in snap()["members"]:
            if m["uid"] == uid:
                return m
        return None

    # 1) join + quiz 到不同岛
    P(["join"], {"uid": A, "nick": "ALICE"})
    P(["join"], {"uid": B, "nick": "BOBBY"})
    P(["quiz", "done"], {"uid": A, "answers": [0, 0, 0]})   # EMBER(0)
    P(["quiz", "done"], {"uid": B, "answers": [4, 0, 0]})   # TIDE(4)
    ma, mb = member(A), member(B)
    assert ma["island"] == 0 and mb["island"] == 4

    # 2) A 发起 HI → B 收 incoming（含岛色 + 昵称）
    P(["hi", "request"], {"requester": A, "responder": B, "msg": "hey"})
    time.sleep(0.3)
    inc = [r for r in results[B] if r.get("event") == "incoming"]
    assert inc and inc[0]["requester"] == A and inc[0]["color"] == ma["island_color"]
    assert inc[0]["requester_nick"] == "ALICE"

    # 3) B 接受 → 双向换色 + matched + hi_count++
    P(["hi", "respond"], {"requester": A, "responder": B, "accept": True})
    time.sleep(0.4)
    ma, mb = member(A), member(B)
    assert mb["island_color"] in ma["spectrum"]
    assert ma["island_color"] in mb["spectrum"]
    assert ma["hi_count"] == 1 and mb["hi_count"] == 1
    mra = [r for r in results[A] if r.get("event") == "matched"]
    mrb = [r for r in results[B] if r.get("event") == "matched"]
    assert mra and mra[0]["partner"] == "BOBBY" and mra[0]["color"] == mb["island_color"]
    assert mrb and mrb[0]["partner"] == "ALICE" and mrb[0]["color"] == ma["island_color"]

    # 4) 重复 HI 同人不加色（slot=-1）
    prev_a = list(ma["spectrum"])
    P(["hi", "request"], {"requester": A, "responder": B})
    P(["hi", "respond"], {"requester": A, "responder": B, "accept": True})
    time.sleep(0.3)
    assert member(A)["spectrum"] == prev_a
    assert [r for r in results[A] if r.get("event") == "matched"][-1]["slot"] == -1

    # 5) 同岛 HI 共鸣不加色
    P(["join"], {"uid": "dev-C", "nick": "CARL"})
    P(["quiz", "done"], {"uid": "dev-C", "answers": [0, 0, 0]})  # 同 EMBER
    P(["hi", "request"], {"requester": A, "responder": "dev-C"})
    P(["hi", "respond"], {"requester": A, "responder": "dev-C", "accept": True})
    time.sleep(0.3)
    assert member(A)["spectrum"] == prev_a

    # 6) 拒绝 → declined 通知发起方
    results[B].clear()
    P(["hi", "request"], {"requester": B, "responder": A})
    P(["hi", "respond"], {"requester": B, "responder": A, "accept": False})
    time.sleep(0.3)
    assert [r for r in results[B] if r.get("event") == "declined"]

    # 7) 伪造 declined：无 pending 时 respond(accept=False) 不应发 declined（修 Codex P1）
    results[A].clear()
    P(["hi", "respond"], {"requester": A, "responder": B, "accept": False})
    time.sleep(0.3)
    assert not [r for r in results[A] if r.get("event") == "declined"], \
        "无 pending 的拒绝不应通知任何人"

    # 8) 未分岛用户不能发起 HI（修 Codex P2）
    P(["join"], {"uid": "dev-D", "nick": "DANA"})   # 未 quiz → island=-1
    results[A].clear()
    P(["hi", "request"], {"requester": "dev-D", "responder": A})
    time.sleep(0.3)
    # A 不应收到 incoming（D 未分岛，请求被拒）
    assert not [r for r in results[A] if r.get("event") == "incoming"], \
        "未分岛用户发起的 HI 应被拒"

    # 9) nick→uid 解析：键入 `HI ALICE`（A 的昵称）
    results[A].clear()
    P(["hi", "request"], {"requester": B, "responder_nick": "alice"})  # 大小写不敏感
    time.sleep(0.3)
    assert [r for r in results[A] if r.get("event") == "incoming"], \
        "responder_nick 应解析到 A 并发 incoming"
    P(["hi", "respond"], {"requester": B, "responder": A, "accept": False})  # 收尾

    # 10) roster 在线名册（retain）只含已分岛成员
    assert roster["last"] is not None, "应收到 roster retain"
    nicks = {m["nick"] for m in roster["last"]["members"]}
    assert "ALICE" in nicks and "BOBBY" in nicks, "roster 缺已分岛成员"
    assert "DANA" not in nicks, "roster 不应含未分岛成员"

    # 11) nick 解析口径与 roster 一致：未分岛同名者不能 shadow 已分岛目标（修 Codex P2）
    #     A=ALICE(已分岛 EMBER)。再加一个未分岛 ALICE-shadow，B 发 `HI ALICE` 应解析到已分岛 A。
    P(["join"], {"uid": "dev-shadow", "nick": "ALICE"})  # 未 quiz → island=-1
    results[A].clear()
    P(["hi", "request"], {"requester": B, "responder_nick": "alice"})
    time.sleep(0.3)
    assert [r for r in results[A] if r.get("event") == "incoming"], \
        "未分岛同名者不应 shadow 已分岛 ALICE，应解析到 A 并发 incoming"
    P(["hi", "respond"], {"requester": B, "responder": A, "accept": False})  # 收尾

    # 12) cancel：A 发起 → cancel → B accept 时无 pending，应静默不换色（防单边换色 Codex P3）
    P(["hi", "request"], {"requester": A, "responder": B})
    time.sleep(0.2)
    spec_b_before = list(member(B)["spectrum"])
    P(["hi", "cancel"], {"requester": A})
    time.sleep(0.2)
    results[A].clear()
    P(["hi", "respond"], {"requester": A, "responder": B, "accept": True})  # 迟到 accept
    time.sleep(0.3)
    assert member(B)["spectrum"] == spec_b_before, "cancel 后 B accept 不应换色（无 pending）"
    assert not [r for r in results[A] if r.get("event") == "matched"], \
        "cancel 后不应有 matched"

    for u in (A, B, "dev-C", "dev-D", "dev-shadow"):
        P(["leave"], {"uid": u})
    sub.loop_stop()
    pub.loop_stop()
    print("✅ P3 HI 全链路验证通过")


if __name__ == "__main__":
    main()
