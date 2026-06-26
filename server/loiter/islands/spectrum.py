"""5 格 Pride 色收集引擎。

规则（V4 / M5_PRD §Phase 2）：
  - 5 格，第一格 = 本色（登岛即填）
  - 其余 4 格从他人 HI 握手收集（对方岛色）
  - 同岛 HI 不加色（触发"共鸣光环"）；重复 HI 同人不加色
  - 满 5 格仍可 HI（纯社交，不再加色）

P0：数据结构 + 基础 add 逻辑。HI 触发接线在 hi.py / P3。
"""
from __future__ import annotations

SPECTRUM_SIZE = 5


class Spectrum:
    """单个参与者的 5 格色彩收集状态。"""

    def __init__(self, home_color: str) -> None:
        # slot[0] 永远是本色；其余初始为空
        self.slots: list[str | None] = [home_color] + [None] * (SPECTRUM_SIZE - 1)
        self._sources: set[str] = set()  # 已贡献过色的 uid（防重复 HI 同人加色）

    @property
    def filled(self) -> int:
        return sum(1 for s in self.slots if s is not None)

    @property
    def is_full(self) -> bool:
        return self.filled >= SPECTRUM_SIZE

    def add_from(self, source_uid: str, color: str) -> int | None:
        """收一个他人岛色。返回填入的 slot index；不加色返回 None。

        不加色的情形：已满 / 已从该 uid 收过 / 同色（同岛）。
        """
        if self.is_full or source_uid in self._sources or color == self.slots[0]:
            return None
        for i in range(1, SPECTRUM_SIZE):
            if self.slots[i] is None:
                self.slots[i] = color
                self._sources.add(source_uid)
                return i
        return None

    def reset(self) -> None:
        """清空收集（保留本色），归一回登岛初态。

        设备 Reset（硬件重启 → 本地 collection 已归一到本色）后，服务端 spectrum
        用 fresh join 触发本方法同步，否则大屏残留 Reset 前 HI 攒的色。
        """
        home = self.slots[0]
        self.slots = [home] + [None] * (SPECTRUM_SIZE - 1)
        self._sources.clear()

    def as_list(self) -> list[str | None]:
        return list(self.slots)
