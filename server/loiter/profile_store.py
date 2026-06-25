"""烧录 profile 存储 — v3′ 中心化分岛（SQLite 持久化）。

`POST /flash/profile {text}` → B′ 语义选岛（LLM 读文本同步定岛，超时→hash(text)%6）+ 存档 + 返回 profile_id。
设备 join 带 profile_id → `get()` 查回 island/text/reason；未知 pid → `adopt()` 用 hash(pid) 兜底定岛。

为什么 SQLite 而非内存：烧录（profile 创建）与设备 join 之间有时间差，可能跨一次
loiter 重启 → 内存会丢 → 设备拿 profile_id 来 join 时 server 不认识。持久化 + orphan
adopt 双保险。B′：岛由文本语义/哈希确定性定，创建即持久化、不可变（Reset/rejoin 只查不改）。

线程安全：FastAPI async 端点 + 异步 reason worker 可能并发 → 全程 `_lock` + 每次新游标。
"""
from __future__ import annotations

import sqlite3
import threading
import time
import uuid
from pathlib import Path

from . import config
from .islands.assignment import ISLANDS
from .islands.reading import island_reason_fallback, _hash_island

N_ISLANDS = len(ISLANDS)


def _now() -> int:
    return int(time.time())


class ProfileStore:
    def __init__(self, db_path: str | Path) -> None:
        Path(db_path).parent.mkdir(parents=True, exist_ok=True)
        # check_same_thread=False：reason worker 线程也会写；并发由 _lock 串行化
        self._db = sqlite3.connect(str(db_path), check_same_thread=False)
        self._db.row_factory = sqlite3.Row
        self._lock = threading.Lock()
        self._init_schema()

    def _init_schema(self) -> None:
        with self._lock:
            self._db.executescript(
                """
                CREATE TABLE IF NOT EXISTS profiles (
                    pid TEXT PRIMARY KEY,
                    island INTEGER NOT NULL,
                    text TEXT NOT NULL DEFAULT '',
                    reason_en TEXT NOT NULL DEFAULT '',
                    reason_cn TEXT NOT NULL DEFAULT '',
                    created_at INTEGER NOT NULL
                );
                CREATE TABLE IF NOT EXISTS meta (
                    key TEXT PRIMARY KEY,
                    value INTEGER NOT NULL
                );
                """
            )
            self._db.commit()

    # --- 内部：确定性哈希分岛见 reading._hash_island（B′：废弃顺序轮转 counter）---

    @staticmethod
    def _row_to_dict(row: sqlite3.Row) -> dict:
        return {
            "profile_id": row["pid"],
            "island": row["island"],
            "text": row["text"],
            "reason_en": row["reason_en"],
            "reason_cn": row["reason_cn"],
            "created_at": row["created_at"],
        }

    # --- 公开 API ---
    def create(self, text: str, island: int | None = None) -> dict:
        """烧录时创建 profile：持久化 island + 原文（截断）。返回含 profile_id/island。

        B′：island 由调用方（main.py）先语义选定再传入 → 创建时即持久化、不可再变。
        island 缺省 None → hash(text)%6 兑底（供测试 / 调用方未分类时，仍确定性）。
        """
        pid = uuid.uuid4().hex
        text = (text or "")[:500]
        if island is None:
            island = _hash_island(text)
        island = int(island) % N_ISLANDS
        with self._lock:
            self._db.execute(
                "INSERT INTO profiles(pid, island, text, created_at) VALUES(?,?,?,?)",
                (pid, island, text, _now()),
            )
            self._db.commit()
        return {"profile_id": pid, "island": island, "text": text,
                "reason_en": "", "reason_cn": "", "created_at": _now()}

    def adopt(self, pid: str) -> dict:
        """orphan join：server 不认识的 profile_id → hash(pid)%6 定岛 + 模板 reason，落档保 rejoin 一致。

        B′：用 hash(pid) 而非轮转计数器 → 同 pid 永远同岛（确定性 + 跨重启稳定）。orphan 无原文可分类。
        """
        with self._lock:
            existing = self._db.execute("SELECT * FROM profiles WHERE pid=?", (pid,)).fetchone()
            if existing is not None:
                return self._row_to_dict(existing)
            island = _hash_island(pid)
            en, cn = island_reason_fallback(island)
            self._db.execute(
                "INSERT INTO profiles(pid, island, text, reason_en, reason_cn, created_at) "
                "VALUES(?,?,?,?,?,?)",
                (pid, island, "", en, cn, _now()),
            )
            self._db.commit()
        return {"profile_id": pid, "island": island, "text": "",
                "reason_en": en, "reason_cn": cn, "created_at": _now()}

    def get(self, pid: str) -> dict | None:
        if not pid:
            return None
        with self._lock:
            row = self._db.execute("SELECT * FROM profiles WHERE pid=?", (pid,)).fetchone()
        return self._row_to_dict(row) if row is not None else None

    def set_reason(self, pid: str, reason_en: str, reason_cn: str) -> bool:
        """异步 reason 生成完回填。返回是否命中该 pid。"""
        with self._lock:
            cur = self._db.execute(
                "UPDATE profiles SET reason_en=?, reason_cn=? WHERE pid=?",
                (reason_en[:120], reason_cn[:60], pid),
            )
            self._db.commit()
            return cur.rowcount > 0

    def tally(self) -> dict[int, int]:
        """各岛已创建 profile 数（监控用）。"""
        counts = {i: 0 for i in range(N_ISLANDS)}
        with self._lock:
            for row in self._db.execute("SELECT island, COUNT(*) c FROM profiles GROUP BY island"):
                counts[row["island"]] = row["c"]
        return counts

    def reset(self) -> None:
        """赛前清零（清 profiles + 轮转计数器）。"""
        with self._lock:
            self._db.execute("DELETE FROM profiles")
            self._db.execute("DELETE FROM meta")
            self._db.commit()

    # --- 烧录窗口开关（host 控场，默认开，持久化在 meta；重启不丢）---
    def get_flash_open(self) -> bool:
        """烧录窗口是否开启。meta 无记录 → 默认 True（default-open）。"""
        with self._lock:
            row = self._db.execute("SELECT value FROM meta WHERE key='flash_open'").fetchone()
        return True if row is None else bool(row["value"])

    def set_flash_open(self, open_: bool) -> None:
        """host 开/关烧录窗口，持久化（reset 会清 meta → 回到默认 open，符合赛前清零语义）。"""
        v = 1 if open_ else 0
        with self._lock:
            self._db.execute(
                "INSERT INTO meta(key, value) VALUES('flash_open', ?) "
                "ON CONFLICT(key) DO UPDATE SET value=?",
                (v, v),
            )
            self._db.commit()


# 进程级单例（main.py 端点 + mqtt_bridge join 共用）
store = ProfileStore(config.PROFILE_DB)
