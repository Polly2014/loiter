"""FastAPI 入口 — MQTT bridge 启动 + WebSocket /ws + 大屏静态托管。"""
from __future__ import annotations

import asyncio
import contextlib
import logging

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse
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
    # 新连接立即推一份全量快照
    await ws.send_json({"type": "snapshot", **room.snapshot()})
    try:
        while True:
            await ws.receive_text()  # 大屏暂为只读，丢弃入站
    except WebSocketDisconnect:
        pass
    finally:
        await ws_manager.disconnect(ws)


# 大屏静态资源（放最后，避免覆盖 API 路由）
if config.WEB_DIR.is_dir():
    app.mount("/", StaticFiles(directory=str(config.WEB_DIR), html=True), name="web")
else:
    log.warning("WEB_DIR not found: %s（大屏静态资源未挂载）", config.WEB_DIR)
