"""默认头像池 — 预生成 12 个 Q版角色，join 时按 uid hash 秒分配。

头像缓存在 data/default_avatars.json（base64 PNG 列表），持久跨重启。
首次启动时如果缓存不存在，在后台线程逐个生成并保存。
"""
from __future__ import annotations

import hashlib
import json
import logging
import pathlib
from concurrent.futures import ThreadPoolExecutor

from . import avatar

log = logging.getLogger("loiter.avatar_pool")

_POOL_DIR = pathlib.Path(__file__).resolve().parent.parent / "data"
_POOL_FILE = _POOL_DIR / "default_avatars.json"

# 12 种预设角色关键词
PRESETS = [
    ["cat", "warrior"], ["fox", "mage"], ["rabbit", "archer"],
    ["panda", "monk"], ["owl", "sage"], ["deer", "dancer"],
    ["wolf", "knight"], ["crane", "poet"], ["tiger", "hero"],
    ["dragon", "prince"], ["phoenix", "princess"], ["koi", "scholar"],
]

_pool: list[str] = []  # base64 PNG 列表
_generating = False


def load() -> None:
    """启动时加载缓存。"""
    global _pool
    if _POOL_FILE.exists():
        try:
            _pool = json.loads(_POOL_FILE.read_text())
            log.info("loaded %d default avatars from cache", len(_pool))
            return
        except Exception:
            log.warning("failed to load avatar pool cache, will regenerate")
    log.info("no avatar pool cache found, will generate in background")


def ensure_pool(executor: ThreadPoolExecutor) -> None:
    """如果池子不满 12 个，后台补齐。"""
    global _generating
    if len(_pool) >= len(PRESETS) or _generating or not avatar.ENABLED:
        return
    _generating = True
    executor.submit(_generate_all)


def _generate_all() -> None:
    """后台线程：逐个生成缺失的默认头像。"""
    global _generating
    try:
        _POOL_DIR.mkdir(parents=True, exist_ok=True)
        need = len(PRESETS) - len(_pool)
        log.info("generating %d default avatars...", need)
        for i in range(len(_pool), len(PRESETS)):
            try:
                kw = PRESETS[i]
                res = avatar.generate(f"default-{i}", kw)
                _pool.append(res.png_b64)
                log.info("default avatar %d/%d done (%s)", i + 1, len(PRESETS), kw)
            except Exception:
                log.exception("failed to generate default avatar %d", i)
                _pool.append("")  # 占位，避免卡住
        # 持久化
        _POOL_FILE.write_text(json.dumps(_pool))
        log.info("avatar pool saved (%d avatars)", len(_pool))
    finally:
        _generating = False


def pick(uid: str) -> str | None:
    """按 uid hash 从池子里选一个 base64 PNG。池子空返回 None。"""
    valid = [p for p in _pool if p]
    if not valid:
        return None
    idx = int(hashlib.md5(uid.encode()).hexdigest(), 16) % len(valid)
    return valid[idx]
