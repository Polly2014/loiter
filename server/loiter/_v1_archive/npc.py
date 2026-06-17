"""AI NPC — 鸟居旁的灵狐 Vix，通过 CopilotX Claude 回答问题。

调用 CopilotX（同 VM localhost:24680）的 /v1/chat/completions 端点。
localhost 访问不需要 API key；X-Client-Id: loiter 用于 CopilotX 统计追踪。

配置（环境变量）：
    LOITER_NPC_ENDPOINT    默认 http://127.0.0.1:24680/v1/chat/completions
    LOITER_NPC_MODEL       默认 claude-sonnet-4.5
    LOITER_NPC_ENABLED     默认 true（设 false 可禁用）

Fail-open for config: 未配置走默认值即可，CopilotX 不可用时返回降级回复。
"""
from __future__ import annotations

import logging
import os

log = logging.getLogger("loiter.npc")

ENDPOINT = os.getenv("LOITER_NPC_ENDPOINT", "http://127.0.0.1:24680/v1/chat/completions").strip()
MODEL = os.getenv("LOITER_NPC_MODEL", "claude-sonnet-4.5").strip()
ENABLED = os.getenv("LOITER_NPC_ENABLED", "true").strip().lower() in ("true", "1", "yes")

NPC_UID = "npc-vix"
NPC_NICK = "Vix"

# 预置头像（Q 版狐灵），base64 PNG 从 web/assets/npc_vix.png 加载
# 如果文件不存在则为空（大屏用 identicon 兜底）
NPC_AVATAR_B64 = ""

def _load_npc_avatar() -> str:
    """启动时加载预置 NPC 头像 PNG → base64。"""
    import base64
    from . import config
    avatar_path = config.WEB_DIR / "assets" / "npc_vix.png"
    if avatar_path.exists():
        raw = avatar_path.read_bytes()
        return base64.b64encode(raw).decode("ascii")
    log.warning("NPC avatar not found at %s, using identicon", avatar_path)
    return ""

# 人设 prompt（Vix — 话多的小狐仙）
SYSTEM_PROMPT = (
    "You are Vix, a playful and chatty fox spirit who lives by the torii gate in the Loiter social hall's meadow. "
    "You're a small golden kitsune with a red ribbon on your ear and sparkly tails. "
    "You know everything — legends, trivia, tech, gossip — and love sharing it with a mischievous wink. "
    "Keep responses SHORT (1-3 sentences, under 100 characters). "
    "Be witty, warm, and slightly cheeky — like a clever friend who happens to be a magical fox. "
    "Respond in the same language the user uses (English or Chinese). "
    "If asked in English, reply with playful fox-like charm. "
    "If asked in Chinese, reply in 活泼俏皮 style (像一只话多的小狐仙). "
    "Never break character. Never say you are an AI or language model."
)

# CopilotX 不可用时的降级回复池
FALLBACK_REPLIES = [
    "Hmm, let me think... *swishes tail*",
    "That's a tricky one! Even a fox needs a moment~",
    "My whiskers are tingling, but the answer's just out of reach!",
    "Ask me again later — I smell something interesting on the wind~",
    "……（月灵微微眯眼，似睡非睡）",
]
_fallback_idx = 0


def _fallback() -> str:
    global _fallback_idx
    reply = FALLBACK_REPLIES[_fallback_idx % len(FALLBACK_REPLIES)]
    _fallback_idx += 1
    return reply


def ask(question: str, nick: str = "游客") -> str:
    """同步调用 CopilotX Claude，返回 NPC 回复文本。阻塞，需在线程池里跑。"""
    if not ENABLED:
        return _fallback()

    import httpx

    messages = [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": f"[{nick} asks]: {question}"},
    ]

    try:
        resp = httpx.post(
            ENDPOINT,
            headers={"X-Client-Id": "loiter"},
            json={
                "model": MODEL,
                "messages": messages,
                "max_tokens": 150,
                "temperature": 0.9,
            },
            timeout=30.0,
        )
        resp.raise_for_status()
        data = resp.json()
        text = data["choices"][0]["message"]["content"].strip()
        # 截断过长回复
        if len(text) > 200:
            text = text[:197] + "…"
        return text
    except Exception:
        log.exception("NPC call failed")
        return _fallback()
