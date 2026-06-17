"""破冰任务 — 水墨卷轴任务卡。

用户输入 /task 领取一个随机任务，完成后服务端自动检测并广播。
任务全部基于服务器已有状态判定，无需额外数据采集。

设计：
- 每人同一时间只能有一个活跃任务
- 完成后自动发放 + 可立即领下一个
- 任务池从已有功能中抽取，引导玩家探索各种命令
- 同一任务不重复分配给同一人
"""
from __future__ import annotations

import random
from dataclasses import dataclass, field


@dataclass
class Task:
    id: str
    title: str         # 任务名（短，大屏显示）
    desc: str          # 描述（Cardputer 显示）
    check: str         # 完成条件类型（代码内匹配用）


# 任务池 — 全部基于已有功能，无需新数据采集
TASK_POOL: list[Task] = [
    Task("emote_bloom",   "Cherry Bloom", "Use /e bloom for a petal burst",        "emote:bloom"),
    Task("emote_spark",   "Fireworks!",   "Use /e spark to launch fireworks",      "emote:spark"),
    Task("emote_wind",    "Meadow Breeze","Use /e wind for a leaf gust",           "emote:wind"),
    Task("emote_fox",     "Fox Fire",     "Use /e fox to summon foxfire",          "emote:fox"),
    Task("emote_rain",    "Rainbow Arc",  "Use /e rain to draw a rainbow",         "emote:rain"),
    Task("ask_npc",       "Ask Vix",      "Use /ask to ask Vix a question",       "ask"),
    Task("ch_fishing",    "溪边垂钓", "切换到 #fishing 频道发一条消息",    "msg_in:fishing"),
    Task("ch_help",       "仗义相助", "切换到 #help 频道发一条消息",       "msg_in:help"),
    Task("nick_change",   "改头换面", "用 /nick 给自己换个名字",           "nick"),
    Task("face_gen",      "点亮真容", "用 /face 生成一个 AI 头像",         "face"),
    Task("say_hello",     "初来乍到", "在主厅发送一条打招呼消息",          "msg_in:main"),
    Task("chat_3",        "妙语连珠", "连续发送 3 条消息",                 "msg_count:3"),
]

TASK_BY_ID = {t.id: t for t in TASK_POOL}


@dataclass
class ActiveTask:
    task: Task
    progress: int = 0     # 进度计数（如 msg_count:3 需要累计）
    target: int = 1       # 目标数


class TaskEngine:
    """每 uid 同时只有一个活跃任务，完成后自动清除。"""

    def __init__(self) -> None:
        self._active: dict[str, ActiveTask] = {}           # uid -> 当前任务
        self._completed: dict[str, set[str]] = {}           # uid -> 已完成的 task ids
        self._rng = random.Random()

    def assign(self, uid: str) -> Task | None:
        """为 uid 分配一个新任务。已有活跃任务则返回当前任务。"""
        if uid in self._active:
            return self._active[uid].task

        done = self._completed.get(uid, set())
        available = [t for t in TASK_POOL if t.id not in done]
        if not available:
            # 全做完了，重置（但洗牌顺序不同）
            self._completed[uid] = set()
            available = list(TASK_POOL)

        task = self._rng.choice(available)
        target = 1
        # 解析 msg_count:N
        if task.check.startswith("msg_count:"):
            target = int(task.check.split(":")[1])
        self._active[uid] = ActiveTask(task=task, progress=0, target=target)
        return task

    def get_active(self, uid: str) -> Task | None:
        at = self._active.get(uid)
        return at.task if at else None

    def check_event(self, uid: str, event_type: str, detail: str = "") -> Task | None:
        """检查事件是否完成了 uid 的当前任务。返回完成的 Task 或 None。

        event_type 对应 Task.check 的前缀：
            "emote:<type>"    — 使用了某种表情
            "ask"             — 问了百晓生
            "msg_in:<channel>" — 在某频道发了消息
            "msg_count:<any>" — 发了一条消息（累计判定）
            "nick"            — 改了昵称
            "face"            — 请求了头像
        """
        at = self._active.get(uid)
        if at is None:
            return None

        check = at.task.check
        matched = False

        if check == event_type:
            # 精确匹配（emote:ink, msg_in:fishing, ask, nick, face）
            matched = True
        elif check.startswith("msg_count:") and event_type.startswith("msg_in:"):
            # msg_count 任务：任何频道的消息都算
            at.progress += 1
            if at.progress >= at.target:
                matched = True

        if matched:
            # 完成！
            task = at.task
            self._active.pop(uid, None)
            if uid not in self._completed:
                self._completed[uid] = set()
            self._completed[uid].add(task.id)
            return task

        return None
