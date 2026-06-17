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


@app.get("/firmware/manifest.json")
async def firmware_manifest():
    """Phase 7.6 OTA — 公开的固件清单。

    返回:
      { "version": "0.2.0", "url": "https://loiter.polly.wang/firmware/loiter.bin",
        "sha256": "...", "size": 1234567, "build_ts": 1748... }
    没发布过固件 → 404。
    """
    mf = config.FIRMWARE_DIR / "manifest.json"
    if not mf.is_file():
        raise HTTPException(status_code=404, detail="no firmware published")
    try:
        return JSONResponse(json.loads(mf.read_text()))
    except (OSError, ValueError) as exc:
        log.error("bad firmware manifest: %s", exc)
        raise HTTPException(status_code=500, detail="manifest unreadable")


@app.post("/firmware/broadcast")
async def firmware_broadcast(request: dict | None = None):
    """Admin: 把当前 manifest 通过 MQTT `loiter/hall/sys/ota` 广播出去 → 在线设备立即升级。

    body 可选 `{"targets": "all" | "card-abc,card-def"}`，默认 "all"。
    无鉴权——内部工具，靠 NSG / cloudflare access 兜底。
    """
    mf = config.FIRMWARE_DIR / "manifest.json"
    if not mf.is_file():
        raise HTTPException(status_code=404, detail="no firmware published")
    if bridge is None:
        raise HTTPException(status_code=503, detail="bridge not ready")
    try:
        manifest = json.loads(mf.read_text())
    except (OSError, ValueError) as exc:
        raise HTTPException(status_code=500, detail=f"manifest unreadable: {exc}")
    targets = (request or {}).get("targets", "all")
    payload = {**manifest, "targets": targets, "ts": int(time.time() * 1000)}
    bridge.publish_ota(payload)
    return {"ok": True, "broadcast": payload}


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
