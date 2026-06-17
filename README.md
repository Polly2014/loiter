# Loiter

> 卡片机社交厅 · A pocket-sized social lobby for hardware hackers.

**Status**: 🟢 **LIVE** at <https://loiter.polly.wang> · **v2 Islands of Color** (GLEAM Pride workshop, 2026-06-14~)

![Islands of Color — Big Screen](screenshot.jpg)

**Loiter** 是一套"口袋级"虚拟社交厅引擎，跑在 [M5Stack Cardputer-Adv](https://docs.m5stack.com/en/core/Cardputer%20Adv) 上。
当前形态 **Islands of Color** 是一个 75 分钟的 Pride Month workshop：16 人各持一台 M5，走完换装→选岛→跨岛 HI 握手→AI 个人 reading 的三幕故事弧，大屏实时渲染 6 座岛屿地图 + 跨海彩虹弧 + 集体 JUMP。

```
 ┌─────────────────────┐
 │ Polly@MAIN     ●  7 │
 │ ▓▓▓▒▒▒▒░  ▓▓▒▒░ 9/16│  ← skill chip 行
 │─────────────────────│
 │ > Luna: 有人玩原神吗 │
 │   Kai: 我我我！     │
 │ > [输入消息...]     │
 │─────────────────────│
 │ Tab 切频道 · /pair  │
 └─────────────────────┘
```

## 文档入口

📖 **[CLAUDE.md](CLAUDE.md)** — 完整设计稿 / 架构 / Scope / 各 Sprint 进展

📖 **[docs/mqtt-protocol.md](docs/mqtt-protocol.md)** — MQTT topic 契约（含 v1.6 OTA schema）

📖 **[scripts/publish_ota.sh](scripts/publish_ota.sh)** — Phase 7.6 一键 OTA 发布

## Quick Start

### Flash firmware (first time, USB required)

```bash
# Option A: With PlatformIO (recommended for developers)
cd firmware && pio run -e islands -t upload

# Option B: With esptool only (no PlatformIO needed)
pip install esptool
# Download loiter-v2.0.0.bin from GitHub Releases
esptool.py --chip esp32s3 --port /dev/cu.usbmodem* write_flash 0x10000 loiter-v2.0.0.bin
```

### After first flash

Subsequent firmware updates can be pushed wirelessly via OTA:
```bash
scripts/publish_ota.sh 2.1.0    # All online devices upgrade over-the-air
```

### Server (already deployed)

Live at <https://loiter.polly.wang> — no local setup needed for participants.

For local development:
```bash
cd server && uv run uvicorn loiter.main:app --host 0.0.0.0 --port 8099
# + brew services start mosquitto
# + firmware/src/config.h: MQTT_HOST = your LAN IP
```

## Reviewers

- @Polly
- @小龙虾

