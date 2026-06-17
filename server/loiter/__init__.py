"""Loiter Hub Server — GLEAM Hall instance.

房间状态权威 + MQTT bridge + WebSocket fanout（大屏）。
"""

__version__ = "2.0.0"
# PROTOCOL_VERSION 单一来源在 config.py（避免两处不同步，review: 小龙虾 nit #1）
