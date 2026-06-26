"""线下实测修复单测（Codex 建议）：

1. P0-1 同岛散布 —— island_spawn_point 前 N 人两两最小间距 ≥ sprite 宽，确保不重叠。
2. P1-2 控场持久态 —— emit_stage → stage_state → snapshot 注入，晚到客户端可追赶。

纯逻辑、离线可跑：
  cd X-Workspace/loiter/server && python -m pytest tests/test_live_fixes.py
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from loiter.islands.assignment import ISLAND_CENTERS, island_spawn_point
from loiter.mqtt_bridge import MqttBridge


SPRITE_W = 32  # .char-sprite width（px）；间距须远大于它才不"两位一体"


def test_island_spawn_no_overlap():
    """每座岛前 4 人两两最小间距 ≥ 80px（≥2.5× sprite 宽）。"""
    for island_idx in ISLAND_CENTERS:
        pts = [island_spawn_point(island_idx, seq) for seq in range(4)]
        for i in range(len(pts)):
            for j in range(i + 1, len(pts)):
                d = math.dist(pts[i], pts[j])
                assert d >= 80, (
                    f"island {island_idx} seq{i}/{j} 间距 {d:.1f}px < 80（sprite {SPRITE_W}px → 会重叠）"
                )


def test_spawn_solo_centered():
    """seq0（独自登岛）落在岛心。"""
    for island_idx, (cx, cy) in ISLAND_CENTERS.items():
        x, y = island_spawn_point(island_idx, 0)
        assert (x, y) == (float(cx), float(cy))


def _bare_bridge():
    """绕过 __init__（不连 MQTT），只装 stage 测试所需字段。"""
    b = MqttBridge.__new__(MqttBridge)
    b._stage = {"dim": False, "reveal": False, "photo": False}
    b._burst_total = 0          # emit_stage("jump") 计入权威 BURSTS
    b._emit = lambda ev: None   # 不触发 WS 广播
    return b


def test_stage_toggle_persists():
    """emit_stage 的开关动作更新持久态，stage_state() 反映当前态。"""
    b = _bare_bridge()
    assert b.stage_state() == {"dim": False, "reveal": False, "photo": False}

    b.emit_stage("reveal")
    assert b.stage_state()["reveal"] is True

    b.emit_stage("dim")
    assert b.stage_state() == {"dim": True, "reveal": True, "photo": False}

    b.emit_stage("unreveal")
    assert b.stage_state()["reveal"] is False

    # jump 是瞬时动作，不改持久态
    before = b.stage_state()
    b.emit_stage("jump")
    assert b.stage_state() == before


def test_stage_injected_into_snapshot_shape():
    """模拟 WS 接入：snapshot dict 注入 stage_state → 晚到客户端可追赶 reveal。"""
    b = _bare_bridge()
    b.emit_stage("reveal")
    b.emit_stage("photo")
    # main.py ws_endpoint 的注入逻辑：snap["stage"] = bridge.stage_state()
    snap = {"type": "snapshot", "members": [], "stage": b.stage_state()}
    assert snap["stage"]["reveal"] is True
    assert snap["stage"]["photo"] is True
    assert snap["stage"]["dim"] is False
