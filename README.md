# Loiter

> 卡片机社交厅 · A pocket-sized social lobby for hardware hackers.

**Status**: � **LIVE** at <https://loiter.polly.wang> · Sprint 7 Skill Fusion + Phase 7.6 OTA 上线 (2026-06-08)

**Loiter** 是一套"口袋级"虚拟社交厅引擎，跑在 [M5Stack Cardputer-Adv](https://docs.m5stack.com/en/core/Cardputer%20Adv) 上。
首个部署实例 **"GLEAM Hall"** 用于公司 GLEAM 组织的线下活动；引擎本身可被任意 hackathon / 年会 / 读书会复用。

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

