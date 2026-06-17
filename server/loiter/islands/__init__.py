"""Islands of Color — v2 玩法层（Pride workshop）。

子模块：
  assignment.py — quiz → 岛屿映射 + 6 岛常量
  spectrum.py   — 5 格 Pride 色收集引擎
  hi.py         — HI 握手 + JUMP 聚合 + 匿名公屏

P0：仅空壳 + 数据常量。具体玩法逻辑在 P3/P4 实现。
"""
from __future__ import annotations

from .assignment import ISLANDS, IslandInfo, assign_island
from .spectrum import Spectrum
from .hi import HiEngine, JumpAggregator
from .reading import generate_reading

__all__ = [
    "ISLANDS",
    "IslandInfo",
    "assign_island",
    "Spectrum",
    "HiEngine",
    "JumpAggregator",
    "generate_reading",
]
