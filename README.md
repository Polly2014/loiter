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

```bash
# --- 真机端 ---
cd firmware && pio run -e loiter -t upload   # 烧 Cardputer（默认连 mqtt.polly.wang 公网）

# --- 服务端（云端模式）---
# 已跑在 Azure VM 20.51.201.85，访问 https://loiter.polly.wang
# 本地开发时切回 firmware/src/config.h MQTT_HOST=127.0.0.1，
# 再 `brew services start mosquitto` + `cd server && uv run uvicorn loiter.main:app`

# --- 火线推固件 ---
scripts/publish_ota.sh 0.3.0    # 全场所有在线设备 OTA 升级
```

## Reviewers

- @Polly
- @小龙虾

