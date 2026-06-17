"""Phase 3 个人 reading —— "今天的你"，CopilotX Claude 写。

每个参与者按需触发（进 Phase 3 时设备发请求），服务端用其**整段旅程的权威态**
生成一段俏皮温暖的英文 reading，对齐 Designer 原设计的短诗风格（9 行英文 + 9 行中文）。

调用 CopilotX（同 VM localhost:24680）/v1/chat/completions —— localhost 免 API key，
`X-Client-Id: loiter` 用于统计追踪（照搬 v1 `_v1_archive/npc.py` 模式）。

输入只吃权威态（review: 小龙虾 / Codex）：
  - island      自我认领（你是谁）
  - spectrum    你跟哪些异色的人 say hi 了（你遇见了谁 / 接触了多少种多元）
  - hi_count    你多主动（社交广度）
  - quiz_answers 你性格上的原始倾向（独处 vs 凑热闹）

输出：{title(英文身份标签), title_cn(中文副标题≤6字), core_cn(核心句≤12字),
       lines[9](9 行英文，设备 P3-03 三页×3 行), lines_cn[9](9 行中文≤12字/行)} —— 双语。
       岛色/spectrum 由 bridge 从 member 补。

配置（环境变量，照 npc.py）：
  LOITER_NPC_ENDPOINT  默认 http://127.0.0.1:24680/v1/chat/completions
  LOITER_NPC_MODEL     默认 claude-sonnet-4.5
  LOITER_NPC_ENABLED   默认 true（设 false → 全走 fallback，离线开发用）

容错：httpx timeout + 重试 → 失败走按岛分组的静态 fallback 文案池（不阻塞他人）。
"""
from __future__ import annotations

import json
import logging
import os
import re

from .assignment import ISLANDS

log = logging.getLogger("loiter.reading")

ENDPOINT = os.getenv("LOITER_NPC_ENDPOINT", "http://127.0.0.1:24680/v1/chat/completions").strip()
MODEL = os.getenv("LOITER_NPC_MODEL", "claude-sonnet-4.5").strip()
ENABLED = os.getenv("LOITER_NPC_ENABLED", "true").strip().lower() in ("true", "1", "yes")
TIMEOUT = float(os.getenv("LOITER_READING_TIMEOUT", "12").strip())
RETRIES = int(os.getenv("LOITER_READING_RETRIES", "2").strip())

# Designer 原设计调性（V4 Plan 锁定）：俏皮 + 温暖，像朋友对朋友的观察，会心微笑的真诚。
# 双语：英文 title + 中文副标题 + 核心句 + 9 行英文 + 9 行中文（设备 P3-03 三页 ×3 行 / Designer 原版）。
SYSTEM_PROMPT = (
    "You are a warm, perceptive observer at a Pride-themed workshop called 'Islands of Color'. "
    "Each person picked a home island (a color/identity), then crossed the sea to meet others, "
    "collecting their colors. At the end you write them a short 'who you became today'. "
    "Voice: playful and warm, like a friend's fond observation — a knowing smile, gently sincere. "
    "Never preachy, never fake-positive, never clinical. "
    "The reading is bilingual (English + Simplified Chinese), shown on a tiny 240x135 screen, "
    "so Chinese lines MUST be very short. "
    "Respond ONLY with a JSON object, no markdown fences, exactly this shape:\n"
    '{"title": "TWO OR THREE WORDS, ALL CAPS", "title_cn": "中文标题", '
    '"core_cn": "一句核心", '
    '"lines": ["e1","e2","e3","e4","e5","e6","e7","e8","e9"], '
    '"lines_cn": ["c1","c2","c3","c4","c5","c6","c7","c8","c9"]}\n'
    "title = an evocative English identity tag (e.g. SUNRISE WANDERER, QUIET TIDE). "
    "title_cn = the same idea in Chinese, AT MOST 6 Chinese characters. "
    "core_cn = one core Chinese sentence, AT MOST 12 Chinese characters. "
    "lines = exactly 9 short English lines telling a flowing story (read as 3 pages of 3 lines each), "
    "each under 38 characters, second-person ('you'). "
    "lines_cn = exactly 9 short Chinese lines pairing line-for-line with the English, "
    "EACH AT MOST 12 Chinese characters, second-person ('你'). "
    "Make everything specific to their journey data — don't be generic."
)



def _spectrum_summary(spectrum_colors: list[str], home_color: str) -> str:
    """把收集到的异色翻成岛名描述，给 AI 当 context。"""
    by_color = {i.color: i.name for i in ISLANDS}
    others = [by_color.get(c, "?") for c in spectrum_colors if c and c != home_color]
    if not others:
        return "stayed in resonance with their own island — collected no other colors"
    uniq = list(dict.fromkeys(others))
    return "collected colors from: " + ", ".join(uniq)


def _build_user_prompt(nick: str, island_idx: int, spectrum_colors: list[str],
                       hi_count: int, quiz_answers: list[int]) -> str:
    isle = ISLANDS[island_idx] if 0 <= island_idx < len(ISLANDS) else ISLANDS[2]
    spec_line = _spectrum_summary(spectrum_colors, isle.color)
    # quiz 三题：每题 0/1/2 三选项，粗略映射性格倾向（仅供 AI 参考，不展示）
    tendency = "leans toward people/home" if quiz_answers and quiz_answers[0] == 0 else (
        "leans toward new places" if quiz_answers and quiz_answers[0] == 1 else "leans toward quiet/solo")
    return (
        f"Name: {nick}\n"
        f"Home island: {isle.name} ({isle.biome}; {isle.traits})\n"
        f"Journey: {spec_line}\n"
        f"Said HI to {hi_count} {'person' if hi_count == 1 else 'people'} today\n"
        f"Personality tendency (from their quiz): {tendency}\n\n"
        f"Write {nick}'s 'who you became today'."
    )


# 按岛分组的静态 fallback（CopilotX 不可用时，6 岛各一套，保证调性不崩）。
# 双语：title/lines 英文（9 行）+ title_cn/core_cn/lines_cn 中文（9 行，每行 ≤12 汉字）。
_FALLBACK: dict[int, dict] = {
    0: {"title": "EMBER HEART", "title_cn": "火心", "core_cn": "你燃得明亮又温柔",
        "lines": ["You arrived already burning,", "a small fierce kind of light.",
                  "You crossed to other shores,", "and didn't dim to fit in.",
                  "You left warmth where you went,", "ember by quiet ember.",
                  "You came back still glowing—", "softer now, but no less bright.",
                  "That is your color today."],
        "lines_cn": ["你来时已经燃着", "一束小小的烈火", "你走向别的海岸",
                     "却没为合群变暗", "你把暖意留在身后", "一粒粒安静的火星",
                     "归来时仍在发光", "更柔了 却不更暗", "这就是今天的你"]},
    1: {"title": "STEADY HEARTH", "title_cn": "暖炉", "core_cn": "别人在你这里找到家",
        "lines": ["You weren't the loudest one,", "but you held the door open.",
                  "People leaned toward you,", "and somehow felt at home.",
                  "You gathered other colors,", "without losing your warm one.",
                  "You became a place to rest,", "for everyone passing through.",
                  "Steady is a color too."],
        "lines_cn": ["你不是最响的那个", "却为人留着门", "别人朝你靠过来",
                     "莫名觉得是家", "你收下别的颜色", "也没丢自己的暖",
                     "你成了一处歇脚", "给每个路过的人", "稳 也是一种颜色"]},
    2: {"title": "RESTLESS SPARK", "title_cn": "火花", "core_cn": "你追着光 光也追你",
        "lines": ["You couldn't sit still today,", "always chasing the bright thing.",
                  "You asked the loud questions,", "and ran toward every spark.",
                  "You gathered colors fast,", "curious about each one.",
                  "And somewhere in the chase,", "the light turned and chased you.",
                  "You glow when you wonder."],
        "lines_cn": ["你今天坐不住", "总追着亮的东西", "你问出大声的问题",
                     "奔向每一簇火花", "你飞快地收集颜色", "对每一种都好奇",
                     "追着追着 不知何时", "光也回头追你", "你好奇时就发光"]},
    3: {"title": "ROOTED GROVE", "title_cn": "林", "core_cn": "安静也是一种颜色",
        "lines": ["You didn't rush anywhere,", "you stayed, quiet and sure.",
                  "Others drifted toward you,", "the way things grow toward calm.",
                  "You took in other colors,", "slowly, without losing root.",
                  "You let people slow down,", "just by standing where you stood.",
                  "Calm is a color too."],
        "lines_cn": ["你哪儿都没赶着去", "站得安静又笃定", "别人朝你飘过来",
                     "像万物向着平静", "你收下别的颜色", "慢慢的 根却没动",
                     "你让人也慢下来", "只因你站在那里", "安静 也是一种颜色"]},
    4: {"title": "OPEN TIDE", "title_cn": "潮", "core_cn": "你温柔地碰过每座岛",
        "lines": ["You moved the way the sea moves,", "touching every shore in turn.",
                  "You met so many colors,", "and held each one gently.",
                  "You didn't keep them all,", "you carried pieces of each home.",
                  "You left a little of yourself", "on every island you passed.",
                  "You are made of many tides."],
        "lines_cn": ["你像海一样流动", "轮流碰过每片岸", "你遇见好多颜色",
                     "每一种都轻轻握", "你没把它们都留下", "只带走每个家的碎片",
                     "你也留下一点自己", "在路过的每座岛", "你由许多潮汐组成"]},
    5: {"title": "DREAMING MIST", "title_cn": "雾", "core_cn": "你看见别人走过的",
        "lines": ["You drifted soft and deep,", "in your own quiet weather.",
                  "You saw what others passed,", "the small things in the fog.",
                  "You gathered gentle colors,", "and kept their secrets safe.",
                  "You didn't need the spotlight,", "you were the soft light itself.",
                  "The mist remembers you."],
        "lines_cn": ["你飘得又轻又深", "在自己安静的天气里", "你看见别人走过的",
                     "雾中那些小东西", "你收下温柔的颜色", "替它们藏好秘密",
                     "你不需要聚光灯", "你就是那束柔光", "雾 记得你"]},
}

_CN_TITLE_MAX = 6
_CN_CORE_MAX = 12
_CN_LINE_MAX = 12


def _fallback(island_idx: int) -> dict:
    return dict(_FALLBACK.get(island_idx if 0 <= island_idx < 6 else 2, _FALLBACK[2]))


def _parse(text: str) -> dict | None:
    """从模型输出里抠出双语 reading，容忍 markdown 代码围栏。

    返回 {title, lines[9], title_cn, core_cn, lines_cn[9]}。
    中文字段按字数硬截断（防 240×135 溢出）；缺失则用空串补齐。
    """
    raw = text.strip()
    raw = re.sub(r"^```(?:json)?\s*|\s*```$", "", raw, flags=re.MULTILINE).strip()
    try:
        obj = json.loads(raw)
    except (ValueError, TypeError):
        return None
    title = str(obj.get("title", "")).strip()[:24]
    lines = obj.get("lines")
    if not title or not isinstance(lines, list):
        return None
    lines = [str(x).strip()[:42] for x in lines if str(x).strip()][:9]
    if len(lines) < 1:
        return None
    while len(lines) < 9:
        lines.append("")

    # 中文字段：截断保底（s[:N] 按 Unicode 码点切，对汉字即字数）
    title_cn = str(obj.get("title_cn", "")).strip()[:_CN_TITLE_MAX]
    core_cn = str(obj.get("core_cn", "")).strip()[:_CN_CORE_MAX]
    raw_cn = obj.get("lines_cn")
    lines_cn = []
    if isinstance(raw_cn, list):
        lines_cn = [str(x).strip()[:_CN_LINE_MAX] for x in raw_cn][:9]
    while len(lines_cn) < 9:
        lines_cn.append("")

    return {"title": title, "lines": lines,
            "title_cn": title_cn, "core_cn": core_cn, "lines_cn": lines_cn}



def generate_reading(nick: str, island_idx: int, spectrum_colors: list[str],
                     hi_count: int, quiz_answers: list[int]) -> dict:
    """同步生成 reading（阻塞，需在线程池里跑）。返回 {title, lines[9], title_cn, core_cn, lines_cn[9]}。

    失败 → 按岛 fallback。永不抛异常（单人失败不该阻塞 Phase 3）。
    """
    if not ENABLED:
        return _fallback(island_idx)

    import httpx

    messages = [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": _build_user_prompt(
            nick, island_idx, spectrum_colors, hi_count, quiz_answers)},
    ]
    payload = {"model": MODEL, "messages": messages, "max_tokens": 700, "temperature": 0.9}

    for attempt in range(RETRIES + 1):
        try:
            resp = httpx.post(ENDPOINT, headers={"X-Client-Id": "loiter"},
                              json=payload, timeout=TIMEOUT)
            resp.raise_for_status()
            text = resp.json()["choices"][0]["message"]["content"]
            parsed = _parse(text)
            if parsed:
                return parsed
            log.warning("reading parse failed (attempt %d): %r", attempt, text[:120])
        except Exception:
            log.warning("reading call failed (attempt %d)", attempt, exc_info=True)
    return _fallback(island_idx)
