"""FastAPI 入口 — MQTT bridge 启动 + WebSocket /ws + 大屏静态托管。"""
from __future__ import annotations

import asyncio
import contextlib
import logging
import time

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles

from . import __version__, config
from .mqtt_bridge import MqttBridge
from .room import Room
from .ws import WSManager
from . import npc

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
)
log = logging.getLogger("loiter")

room = Room(config.ROOM)
ws_manager = WSManager()
bridge: MqttBridge | None = None


async def _status_loop(b: MqttBridge) -> None:
    """每 STATUS_INTERVAL 秒广播一次在线人数心跳。"""
    while True:
        await asyncio.sleep(config.STATUS_INTERVAL)
        with contextlib.suppress(Exception):
            b.publish_status()


@contextlib.asynccontextmanager
async def lifespan(app: FastAPI):
    global bridge
    loop = asyncio.get_running_loop()
    bridge = MqttBridge(room, ws_manager, loop)
    bridge.start()
    npc.NPC_AVATAR_B64 = npc._load_npc_avatar()
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


@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket):
    await ws_manager.connect(ws)
    # 新连接立即推一份全量快照（含 NPC 头像）
    snap = {"type": "snapshot", **room.snapshot()}
    if npc.NPC_AVATAR_B64:
        snap["npc_avatar"] = npc.NPC_AVATAR_B64
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
