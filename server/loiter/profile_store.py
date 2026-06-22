"""烧录 profile 存储 — v3′ 中心化分岛（SQLite 持久化）。

`POST /flash/profile {text}` → 原子顺序轮转分岛（counter++%6）+ 存档 + 返回 profile_id。
设备 join 带 profile_id → `get()` 查回 island/text/reason；未知 pid → `adopt()` 兜底轮转。

为什么 SQLite 而非内存：烧录（profile 创建）与设备 join 之间有时间差，可能跨一次
loiter 重启 → 内存会丢 → 设备拿 profile_id 来 join 时 server 不认识。持久化 + orphan
adopt 双保险。轮转计数器 `meta.seq` 也持久化，重启后不从 0 重来（Reset/rejoin 只查不增）。

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
from .islands.reading import island_reason_fallback

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

    # --- 内部：原子轮转下一个岛 ---
    def _next_island_locked(self) -> int:
        cur = self._db.execute("SELECT value FROM meta WHERE key='seq'")
        row = cur.fetchone()
        seq = row["value"] if row else 0
        island = seq % N_ISLANDS
        self._db.execute(
            "INSERT INTO meta(key, value) VALUES('seq', ?) "
            "ON CONFLICT(key) DO UPDATE SET value=?",
            (seq + 1, seq + 1),
        )
        return island

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
    def create(self, text: str) -> dict:
        """烧录时创建 profile：原子分岛 + 存原文（截断）。返回含 profile_id/island。"""
        pid = uuid.uuid4().hex
        text = (text or "")[:500]
        with self._lock:
            island = self._next_island_locked()
            self._db.execute(
                "INSERT INTO profiles(pid, island, text, created_at) VALUES(?,?,?,?)",
                (pid, island, text, _now()),
            )
            self._db.commit()
        return {"profile_id": pid, "island": island, "text": text,
                "reason_en": "", "reason_cn": "", "created_at": _now()}

    def adopt(self, pid: str) -> dict:
        """orphan join：server 不认识的 profile_id → 轮转一个新岛 + 模板 reason，落档保 rejoin 一致。"""
        with self._lock:
            existing = self._db.execute("SELECT * FROM profiles WHERE pid=?", (pid,)).fetchone()
            if existing is not None:
                return self._row_to_dict(existing)
            island = self._next_island_locked()
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


# 进程级单例（main.py 端点 + mqtt_bridge join 共用）
store = ProfileStore(config.PROFILE_DB)
