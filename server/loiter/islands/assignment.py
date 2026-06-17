"""岛屿分配 — quiz 答案 → 6 座 Pride 岛屿之一。

6 岛常量对齐 Designer `firmware/src/main.cpp` 的 ISLANDS[6] 与 M5_PRD / V4。
颜色用 #RRGGBB（大屏 CSS 用）；固件侧自有 RGB565 常量。

P0：常量 + 占位 assign_island()。真实 quiz→岛屿映射规则待 P4（依据 M5_PRD §Phase 1）。
"""
from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class IslandInfo:
    idx: int
    name: str       # "EMBER"
    biome: str      # "volcano / rust earth"
    traits: str     # "fierce . alive"
    color: str      # "#e84d3c"


# 6 座 Pride 岛屿（红橙黄绿蓝紫）—— 与 Designer main.cpp / M5_PRD 对齐
ISLANDS: tuple[IslandInfo, ...] = (
    IslandInfo(0, "EMBER",  "volcano / rust earth",    "fierce . alive",     "#e84d3c"),
    IslandInfo(1, "HEARTH", "warm village / dusk",     "kind . steady",      "#ff9f43"),
    IslandInfo(2, "SPARK",  "grassland / golden hour", "curious . restless", "#f9ca24"),
    IslandInfo(3, "GROVE",  "forest / canopy",         "rooted . calm",      "#6ab04c"),
    IslandInfo(4, "TIDE",   "sea / lighthouse",        "open . flowing",     "#4a6fa5"),
    IslandInfo(5, "MIST",   "fog / watchtower",        "dreamy . deep",      "#5f27cd"),
)


def assign_island(quiz_answers: list[int]) -> IslandInfo:
    """quiz 三题答案 → 岛屿。

    P0 占位：简单按答案和取模。真实映射规则（性格维度 → 岛屿）待 P4 依 M5_PRD 实现。
    """
    if not quiz_answers:
        return ISLANDS[2]  # SPARK 默认
    idx = sum(a for a in quiz_answers if isinstance(a, int)) % len(ISLANDS)
    return ISLANDS[idx]


# 岛屿在逻辑画布（1920×1080）上的中心点 —— 由大屏地图（2752×1536）的岛屿位置
# 等比缩放而来（scale_x=1920/2752, scale_y=1080/1536）。服务端是位置权威：
# 分配岛屿时把成员放到对应岛屿中心附近，大屏直接渲染 x/y（review Codex #3）。
# 岛屿在逻辑画布（1920×1080）上的视觉中心 —— Playwright 实测校准（2026-06-14）：
# 把测试点叠在大屏地图上逐岛对齐到岛屿主体中央，避免单人/少人时落到海岸边缘。
# （之前用 SPAWN_POOL 均值会偏向各岛左上角海岸。）服务端是位置权威：
# 分配岛屿时把成员放到对应岛屿中心附近，大屏直接渲染 x/y（review Codex #3）。
ISLAND_CENTERS: dict[int, tuple[int, int]] = {
    0: (365, 313),   # EMBER  火山下方陆地
    1: (653, 486),   # HEARTH 村庄聚落中央
    2: (365, 745),   # SPARK  黄岛中心
    3: (1133, 454),  # GROVE  绿岛上部主体
    4: (1344, 778),  # TIDE   青岛中部
    5: (1421, 292),  # MIST   紫岛中央
}
_ISLAND_R = 70  # 岛内散布半径（逻辑像素）


def island_spawn_point(island_idx: int, seq: int) -> tuple[float, float]:
    """岛屿中心 + 确定性散布（按入岛序号螺旋偏移），避免多人完全重叠。"""
    import math
    cx, cy = ISLAND_CENTERS.get(island_idx, (960, 540))
    if seq <= 0:
        return (float(cx), float(cy))
    ang = seq * 2.399963  # 黄金角，均匀铺开
    rad = _ISLAND_R * min(1.0, 0.25 + 0.12 * seq)
    return (cx + rad * math.cos(ang), cy + rad * math.sin(ang))
