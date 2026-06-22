"""FastAPI 入口 — MQTT bridge 启动 + WebSocket /ws + 大屏静态托管。"""
from __future__ import annotations

import asyncio
import contextlib
import json
import logging
import time

from fastapi import FastAPI, Header, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse, Response
from fastapi.staticfiles import StaticFiles

from . import __version__, config
from .mqtt_bridge import MqttBridge
from .room import Room
from .ws import WSManager

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
)
log = logging.getLogger("loiter")

room = Room(config.ROOM)
ws_manager = WSManager()
bridge: MqttBridge | None = None


async def _status_loop(b: MqttBridge) -> None:
    """每 STATUS_INTERVAL 秒广播一次在线人数心跳 + 清理超时 HI。"""
    while True:
        await asyncio.sleep(config.STATUS_INTERVAL)
        with contextlib.suppress(Exception):
            b.publish_status()
            b.sweep_hi_timeouts()


@contextlib.asynccontextmanager
async def lifespan(app: FastAPI):
    global bridge
    loop = asyncio.get_running_loop()
    bridge = MqttBridge(room, ws_manager, loop)
    bridge.start()
    task = asyncio.create_task(_status_loop(bridge))
    log.info("Loiter '%s' up — broker %s:%d, room=%s",
             config.INSTANCE_NAME, config.MQTT_HOST, config.MQTT_PORT, config.ROOM)
    try:
        yield
    finally:
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task
        if bridge:
            bridge.stop()


app = FastAPI(title="Loiter Hub", version=__version__, lifespan=lifespan)


@app.get("/healthz")
async def healthz():
    return {
        "ok": True,
        "instance": config.INSTANCE_NAME,
        "protocol": config.PROTOCOL_VERSION,
        "room": config.ROOM,
        "online": room.count,
        "ws_clients": ws_manager.count,
        "broker": f"{config.MQTT_HOST}:{config.MQTT_PORT}",
    }


@app.get("/api/snapshot")
async def snapshot():
    return JSONResponse(room.snapshot())


@app.get("/avatar/{uid}.png")
async def avatar_png(uid: str):
    """Per-player composite avatar PNG (32×64 pixel art, cached)."""
    from .avatar import compose_avatar, avatar_cache_key
    m = room.members.get(uid)
    if m is None:
        raise HTTPException(status_code=404, detail="unknown uid")
    shape = m.avatar.get("shape", [0, 0, 0, 0, 0]) if m.avatar else [0, 0, 0, 0, 0]
    color = m.avatar.get("color", [0, 0, 0, 0, 0]) if m.avatar else [0, 0, 0, 0, 0]
    etag = avatar_cache_key(shape, color)
    png = compose_avatar(shape, color)
    return Response(content=png, media_type="image/png",
                    headers={"ETag": etag, "Cache-Control": "public, max-age=300"})


# ── Admin 控场（host 导播台）──────────────────────────────
_STAGE_ACTIONS = {"dim", "undim", "reveal", "unreveal", "jump", "photo", "unphoto"}


def _check_admin(token: str | None) -> None:
    """Admin 鉴权 — fail-closed：未配 token 拒绝所有；配了则常量时间比对。"""
    import secrets
    expected = config.ADMIN_TOKEN
    if not expected:
        raise HTTPException(status_code=403, detail="admin disabled (no token configured)")
    if not token or not secrets.compare_digest(token, expected):
        raise HTTPException(status_code=401, detail="bad admin token")


@app.post("/admin/verify")
async def admin_verify(x_admin_token: str | None = Header(default=None)):
    """Host 导播台填 token 后校验一次（前端缓存 localStorage，但每次操作仍重校）。"""
    _check_admin(x_admin_token)
    return {"ok": True}


@app.post("/admin/stage")
async def admin_stage(
    body: dict | None = None,
    x_admin_token: str | None = Header(default=None),
):
    """Host 控场 —— DIM/REVEAL/PHOTO 通过服务端广播到所有大屏 + 观众页。

    body `{"action": "dim"|"undim"|"reveal"|"unreveal"|"photo"}`，token 走 X-Admin-Token 头。
    """
    _check_admin(x_admin_token)
    action = (body or {}).get("action")
    if action not in _STAGE_ACTIONS:
        raise HTTPException(status_code=400, detail=f"unknown action: {action}")
    if bridge is None:
        raise HTTPException(status_code=503, detail="bridge not ready")
    bridge.emit_stage(action)
    return {"ok": True, "action": action}


# ── 烧录 profile（v3′ 中心化分岛 + 文艺 reason）────────────────
from .profile_store import N_ISLANDS as _FLASH_N, store as _profile_store


def _tally_payload() -> dict:
    from .islands.assignment import ISLANDS
    counts = _profile_store.tally()
    return {
        "tally": {str(i): counts.get(i, 0) for i in range(_FLASH_N)},
        "names": {str(isle.idx): isle.name for isle in ISLANDS},
    }


def _check_flash(token: str | None) -> None:
    """烧录写鉴权 — fail-closed：未配 FLASH_TOKEN 拒绝所有写；配了则常量时间比对。"""
    import secrets
    expected = config.FLASH_TOKEN
    if not expected:
        raise HTTPException(status_code=403, detail="flash disabled (no token configured)")
    if not token or not secrets.compare_digest(token, expected):
        raise HTTPException(status_code=401, detail="bad flash token")


def _gen_reason_blocking(pid: str, text: str, island: int) -> None:
    """线程池里跑：生成文艺双语 reason → 回填 profile → 通知 bridge 重推在线设备。永不抛。"""
    try:
        from .islands.reading import generate_island_reason
        en, cn = generate_island_reason(text, island)
        _profile_store.set_reason(pid, en, cn)
        log.info("REASON ready pid=%s island=%d", pid[:8], island)
        # 若设备在 reason ready 前已 join（拿到空 reason）→ 重推 island 带新 reason（修 Codex P1）
        if bridge is not None:
            bridge.loop.call_soon_threadsafe(bridge.notify_reason_ready, pid)
    except Exception:
        log.exception("island reason gen failed pid=%s", pid[:8])


@app.post("/flash/profile")
async def flash_profile(body: dict | None = None, x_flash_token: str | None = Header(default=None)):
    """烧录时建 profile：原子顺序轮转分岛 + 异步预生成文艺 reason。

    body `{"text": "..."}`。返回 `{ok, profile_id}` —— **不返回 island/reason（不剧透）**。
    """
    _check_flash(x_flash_token)
    text = (body or {}).get("text", "")
    if not isinstance(text, str):
        raise HTTPException(status_code=400, detail="text (str) required")
    p = _profile_store.create(text)
    # 编译/烧录窗口内异步生成 reason（join 时已就绪 → 揭晓零延迟）
    asyncio.get_running_loop().run_in_executor(None, _gen_reason_blocking, p["profile_id"], text, p["island"])
    return {"ok": True, "profile_id": p["profile_id"]}


@app.get("/flash/tally")
async def flash_tally_get():
    """各岛已创建 profile 数（监控用）。只读公开。"""
    return _tally_payload()


@app.post("/flash/reset")
async def flash_reset(x_admin_token: str | None = Header(default=None)):
    """赛前清零（清 profiles + 轮转计数器）。走 admin token（fail-closed）。"""
    _check_admin(x_admin_token)
    _profile_store.reset()
    return {"ok": True, **_tally_payload()}


@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket):
    await ws_manager.connect(ws)
    # 新连接立即推一份全量快照
    snap = {"type": "snapshot", **room.snapshot()}
    await ws.send_json(snap)
    try:
        while True:
            try:
                # 大屏只读、永不发消息 → 此 WS 完全空闲。
                # Cloudflare tunnel 会按 idle timeout 切断空闲 WebSocket，
                # 所以每 20s 无入站就主动发心跳保活（前端忽略 type=ping）。
                await asyncio.wait_for(ws.receive_text(), timeout=20.0)
            except asyncio.TimeoutError:
                await ws.send_json({"type": "ping", "ts": int(time.time() * 1000)})
    except WebSocketDisconnect:
        pass
    finally:
        await ws_manager.disconnect(ws)


# 大屏静态资源（放最后，避免覆盖 API 路由）
if config.WEB_DIR.is_dir():
    app.mount("/", StaticFiles(directory=str(config.WEB_DIR), html=True), name="web")
else:
    log.warning("WEB_DIR not found: %s（大屏静态资源未挂载）", config.WEB_DIR)
