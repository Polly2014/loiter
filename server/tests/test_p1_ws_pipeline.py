"""P1 大屏 WS 管道验证：连 /ws，经 MQTT 灌 mock 事件，断言 server→大屏事件到达。
不依赖浏览器，纯验证 v2 WS 契约。"""
import json
import threading
import time

import paho.mqtt.client as mqtt
import websocket  # websocket-client

WS_URL = "ws://127.0.0.1:8099/ws"
MQTT_HOST = "127.0.0.1"

received = []
ready = threading.Event()


def on_message(ws, message):
    ev = json.loads(message)
    received.append(ev)
    if ev.get("type") == "snapshot":
        ready.set()


def on_open(ws):
    pass


ws = websocket.WebSocketApp(WS_URL, on_open=on_open, on_message=on_message)
t = threading.Thread(target=ws.run_forever, daemon=True)
t.start()

assert ready.wait(5), "no snapshot frame within 5s"
print("✓ snapshot frame received on connect")

pub = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2, client_id="p1-test-pub")
pub.connect(MQTT_HOST, 1883, 30)
pub.loop_start()


def emit(topic, payload):
    pub.publish(f"loiter/hall/{topic}", json.dumps(payload), qos=1)
    time.sleep(0.4)


# 模拟 4 人入场 + 分岛
people = [
    ("card-a", "ALICE", [0, 1, 1]),   # sum2 → SPARK(2)
    ("card-b", "BOB",   [0, 0, 0]),   # sum0 → EMBER(0)
    ("card-c", "CAI",   [1, 1, 1]),   # sum3 → GROVE(3)
    ("card-d", "DANI",  [2, 2, 1]),   # sum5 → MIST(5)
]
for uid, nick, _ in people:
    emit("join", {"uid": uid, "nick": nick})
for uid, nick, ans in people:
    emit("quiz/done", {"uid": uid, "answers": ans})

# JUMP burst: 5 人触发
for uid, _, _ in people:
    emit("jump", {"uid": uid})
emit("jump", {"uid": "card-e"})  # 第 5 个（card-e 未 join，应被服务端忽略，不计数）
# 再补一个真实的第 5 人
emit("join", {"uid": "card-e", "nick": "EVAN"})
emit("jump", {"uid": "card-e"})

# 匿名公屏
emit("anon", {"uid": "card-a", "text": "happy pride!"})

time.sleep(3.0)
ws.close()
pub.loop_stop()

types = [e.get("type") for e in received]
print("event types received:", types)


def count(t):
    return sum(1 for e in received if e.get("type") == t)


assert count("snapshot") >= 1, "no snapshot"
assert count("join") >= 4, f"join events {count('join')} < 4"
assert count("island_assign") >= 4, f"island_assign {count('island_assign')} < 4"

# 验证 island_assign 的岛屿正确
islands = {e["uid"]: e["island"] for e in received if e.get("type") == "island_assign"}
assert islands.get("card-a") == 2, f"ALICE island {islands.get('card-a')} != 2 (SPARK)"
assert islands.get("card-b") == 0, f"BOB island {islands.get('card-b')} != 0 (EMBER)"
assert islands.get("card-c") == 3, f"CAI island {islands.get('card-c')} != 3 (GROVE)"
assert islands.get("card-d") == 5, f"DANI island {islands.get('card-d')} != 5 (MIST)"
print("✓ island assignment correct: ALICE→SPARK BOB→EMBER CAI→GROVE DANI→MIST")

# 验证 spectrum 首格 = 本色
sp_a = next(e["spectrum"] for e in received if e.get("type") == "island_assign" and e["uid"] == "card-a")
assert sp_a[0] == "#f9ca24", f"ALICE spectrum[0] {sp_a[0]} != SPARK color"
print("✓ spectrum first slot = home color")

assert count("anon_msg") >= 1, "no anon_msg"
anon = next(e for e in received if e.get("type") == "anon_msg")
assert "uid" not in anon, "anon_msg leaks uid!"
assert anon["text"] == "happy pride!"
print("✓ anon_msg broadcast, identity stripped")

# jump_burst: 5 真实在场者 jump 应触发至少 1 次 burst
jb = count("jump_burst")
print(f"jump_burst count = {jb} (expect >=1 once 5 real members jumped)")
assert jb >= 1, "jump_burst not triggered at 5 members"
print("✓ jump_burst triggered at 5")

print("\n=== P1 WS pipeline: ALL CHECKS PASSED ===")
