"""Sprint 7 — 技能体系（4 系 × 4 = 16 个 skill + 4 个大招 + 万象归一）。

每人入场随机 1-2 个；双机配对（pair_engine）互换全部 → 集齐一系解锁大招、
集齐全 16 个解锁 omni。服务端是 ground truth，客户端只显示。
"""
from __future__ import annotations

import random

# 4 系 × 4 个常规 skill ----------------------------------------------------
SKILLS_BY_ELEMENT: dict[str, list[str]] = {
    "nature": ["bloom",  "wind",    "rain",      "leaf"],
    "fire":   ["spark",  "fox",     "flame",     "comet"],
    "water":  ["wave",   "bubble",  "mist",      "tide"],
    "light":  ["star",   "aurora",  "lightning", "halo"],
}

# 集齐一系 → 解锁该系大招（大屏放 5s 强化特效）
ULTIMATES: dict[str, str] = {
    "nature": "gaia",     # 大地共鸣
    "fire":   "solar",    # 烈日当空
    "water":  "dragon",   # 水龙腾
    "light":  "galaxy",   # 星河倒挂
}

# 集齐全 16 → 万象归一（8s 终极特效 + SSR 成就）
OMNI = "omni"

# 派生集合 -----------------------------------------------------------------
ALL_SKILLS: frozenset[str] = frozenset(s for skills in SKILLS_BY_ELEMENT.values() for s in skills)
ALL_ULTIMATES: frozenset[str] = frozenset(ULTIMATES.values())
TOTAL_COUNT: int = len(ALL_SKILLS)  # 16

# skill -> element 反查
SKILL_TO_ELEMENT: dict[str, str] = {
    s: el for el, skills in SKILLS_BY_ELEMENT.items() for s in skills
}


def random_starter(n: int = 2) -> set[str]:
    """入场初始 skill：从 16 个里随机抽 n 个（默认 2）。"""
    return set(random.sample(sorted(ALL_SKILLS), k=min(n, TOTAL_COUNT)))


def completed_elements(skills: set[str]) -> list[str]:
    """返回已集齐全部 4 个 skill 的元素列表（按 nature/fire/water/light 顺序）。"""
    out = []
    for el, el_skills in SKILLS_BY_ELEMENT.items():
        if all(s in skills for s in el_skills):
            out.append(el)
    return out


def is_omni(skills: set[str]) -> bool:
    """是否集齐全 16 个 skill。"""
    return len(skills & ALL_SKILLS) == TOTAL_COUNT


def progress(skills: set[str]) -> tuple[int, int]:
    """返回 (已收集, 总数) — 仅计常规 skill，不算大招/omni。"""
    return (len(skills & ALL_SKILLS), TOTAL_COUNT)
