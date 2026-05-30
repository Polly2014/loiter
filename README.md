# Loiter

> 卡片机社交厅 · A pocket-sized social lobby for hardware hackers.

**Status**: 🟡 DESIGN REVIEW

**Loiter** 是一套"口袋级"虚拟社交厅引擎，跑在 [M5Stack Cardputer-Adv](https://docs.m5stack.com/en/core/Cardputer%20Adv) 上。
首个部署实例 **"GLEAM Hall"** 用于公司 GLEAM 组织的线下活动；引擎本身可被任意 hackathon / 年会 / 读书会复用。

```
 ┌─────────────────────┐
 │ GLEAM HALL  ♥ 23人  │
 │─────────────────────│
 │ > Luna: 有人玩原神吗 │
 │   Kai: 我我我！     │
 │ > [输入消息...]     │
 │─────────────────────│
 │ ← → 切房间 ↑↓ 滚动 │
 └─────────────────────┘
```

## 文档入口

📖 **[CLAUDE.md](CLAUDE.md)** — 完整设计稿 / 架构 / Scope / Open Questions

📖 **[docs/mqtt-protocol.md](docs/mqtt-protocol.md)** — MQTT topic 契约

## Quick Start (TODO — 等代码就绪后填充)

```bash
# 1. 启 broker
brew install mosquitto && brew services start mosquitto

# 2. 启 hub server
cd server && poetry install && poetry run uvicorn loiter.main:app --port 8080

# 3. 烧 Cardputer 固件
cd firmware && pio run -t upload

# 4. 开大屏
open http://localhost:8080
```

## Reviewers

- @Polly
- @小龙虾
