"""AI 头像生成 — Azure AI Foundry gpt-image-1.5 → 大屏彩色 PNG + Cardputer 16×16 1-bit bitmap。

凭据从环境变量读取（与 dream-painter skill 对齐，绝不 hardcode）：
    AZURE_OPENAI_API_KEY        必填
    AZURE_OPENAI_ENDPOINT       必填，如 https://xxx.cognitiveservices.azure.com/
    AZURE_OPENAI_DEPLOYMENT     默认 gpt-image-1.5
    AZURE_OPENAI_API_VERSION    默认 2025-04-01-preview

Fail-closed：未配置 key/endpoint → ENABLED=False，头像功能静默禁用，不崩。
"""
from __future__ import annotations

import base64
import io
import logging
import os
from dataclasses import dataclass

log = logging.getLogger("loiter.avatar")

AZURE_KEY = os.getenv("AZURE_OPENAI_API_KEY", "").strip()
AZURE_ENDPOINT = os.getenv("AZURE_OPENAI_ENDPOINT", "").strip()
AZURE_DEPLOYMENT = os.getenv("AZURE_OPENAI_DEPLOYMENT", "gpt-image-1.5").strip()
AZURE_API_VERSION = os.getenv("AZURE_OPENAI_API_VERSION", "2025-04-01-preview").strip()

# 空 = 拒绝，不给鉴权字段留 fallback 默认值
ENABLED = bool(AZURE_KEY and AZURE_ENDPOINT)

BIG = 256   # 大屏 PNG 边长（彩色）
SMALL = 16  # Cardputer bitmap 边长（1-bit）


@dataclass
class AvatarResult:
    uid: str
    png_b64: str     # 大屏：BIG×BIG 彩色 PNG，base64
    bitmap_b64: str  # Cardputer：SMALL×SMALL 1-bit，32 bytes，base64
    w: int
    h: int


def _build_prompt(keywords: list[str]) -> str:
    kw = ", ".join(k.strip() for k in keywords if k and k.strip()) or "a friendly robot"
    return (
        f"Cute chibi Q版 character of {kw}, "
        "big head small body, 2.5-head proportion, adorable and expressive, "
        "simple clean lines, Chinese traditional costume (汉服/古风) elements, "
        "transparent background, NO background, isolated character, "
        "centered composition, 3/4 view, full body visible, "
        "game avatar icon style, clean edges, high contrast, "
        "kawaii meets Chinese fantasy aesthetic"
    )


def _call_azure(prompt: str, timeout: float = 90.0) -> bytes:
    import httpx

    base = AZURE_ENDPOINT.rstrip("/")
    url = (
        f"{base}/openai/deployments/{AZURE_DEPLOYMENT}/images/generations"
        f"?api-version={AZURE_API_VERSION}"
    )
    body = {
        "prompt": prompt,
        "n": 1,
        "size": "1024x1024",
        "quality": "medium",
        "output_format": "png",
        "background": "transparent",
    }
    resp = httpx.post(
        url,
        headers={"Api-Key": AZURE_KEY, "Content-Type": "application/json"},
        json=body,
        timeout=timeout,
    )
    resp.raise_for_status()
    b64 = resp.json()["data"][0]["b64_json"]
    return base64.b64decode(b64)


def _to_outputs(png: bytes) -> tuple[str, str]:
    """原始 PNG → (大屏彩色 PNG base64, Cardputer 16×16 1-bit base64)。"""
    from PIL import Image, ImageOps

    src = Image.open(io.BytesIO(png))

    # 大屏：BIG×BIG 彩色 PNG（保留 RGBA 透明通道）
    color = src.convert("RGBA").resize((BIG, BIG), Image.LANCZOS)
    buf = io.BytesIO()
    color.save(buf, "PNG", optimize=True)
    png_b64 = base64.b64encode(buf.getvalue()).decode()

    # Cardputer：灰度 → 16×16 → Floyd–Steinberg 抖动到 1-bit
    # 暗背景 + 亮主体：无需反色，亮主体自然→高灰度值→bit1（Cardputer 亮前景）。
    # tobytes()：每行 ceil(16/8)=2 字节、MSB 在前，共 32 bytes。
    gray = src.convert("L").resize((SMALL, SMALL), Image.LANCZOS)
    mono = gray.convert("1")
    bitmap_b64 = base64.b64encode(mono.tobytes()).decode()  # 32 bytes

    return png_b64, bitmap_b64


def generate(uid: str, keywords: list[str]) -> AvatarResult:
    """同步生成（在线程池里调用，会阻塞数十秒）。失败抛异常由调用方处理。"""
    if not ENABLED:
        raise RuntimeError("avatar disabled: AZURE_OPENAI_API_KEY/ENDPOINT not set")
    prompt = _build_prompt(keywords)
    log.info("avatar gen uid=%s prompt=%s", uid, prompt[:80])
    png = _call_azure(prompt)
    png_b64, bitmap_b64 = _to_outputs(png)
    return AvatarResult(uid=uid, png_b64=png_b64, bitmap_b64=bitmap_b64, w=SMALL, h=SMALL)
