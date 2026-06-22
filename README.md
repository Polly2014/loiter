# Loiter

> 卡片机社交厅 · A pocket-sized social lobby for hardware hackers.

**Status**: 🟢 **LIVE** at <https://loiter.polly.wang> · **v3′ Islands of Color — Vibe-Coding 烧录仪式**（GLEAM Pride workshop）

![Islands of Color — Big Screen](screenshot.jpg)

**Loiter** 是一套“口袋级”虚拟社交厅引擎，跑在 [M5Stack Cardputer-Adv](https://docs.m5stack.com/en/core/Cardputer%20Adv) 上。
当前形态 **Islands of Color** 是一个 75 分钟的 Pride Month workshop：~16 人各持一台 M5。
**v3′ 把个性化从「运行时点选」搬到「烧录时共创」**：参与者在自己笔记本上跑 `loiter-flash` skill、打一段自由文本 → server 原子轮转分岛 + 预生成文艺双语解读 → 烧进一个只带 `profile_id` 的专属二进制。拿起设备输名字 → 换装 → ✨揭晓屏显示他的岛屿 + AI 双语 reason → 跨岛 HI 握手 + 5 格 Pride 色收集 → Phase 3 个人光谱 reveal。大屏实时渲染 6 座岛屿地图 + 跨海彩虹弧 + 集体 JUMP。

```
烧录仪式（参与者笔记本）：
  loiter-flash skill → 打一段自由文本
    → POST /flash/profile  → server 原子分岛 + 异步生成文艺双语 reason
    → bake LOITER_PROFILE_ID → pio 编译 → USB 烧录
    → “拿起你的设备”（skill 全程不知岛屿/reason，天然不剧透）
M5 设备：输名字 → join 携 profile_id → server 查 profile → ✨揭晓屏（岛屿 + AI 双语 reason）
```

## 文档入口

📖 **[CLAUDE.md](CLAUDE.md)** — 完整设计稿 / 架构 / Scope / 各 Sprint 进展

📖 **[docs/mqtt-protocol.md](docs/mqtt-protocol.md)** — MQTT topic 契约（v2）

## Quick Start

### 🔥 烧录仪式（参与者主路径 · v3′）

在装有 Claude / VS Code 的笔记本上，唬醒 `loiter-flash` skill（触发词：“烧我的卡片机” / “参加 Islands of Color”），打一段自由文本 → 脚本自动拉 repo / 装 pio / 编译 / USB 烧录。需要：USB 数据线 + `LOITER_FLASH_TOKEN`。

```bash
# skill 底层就是这个（一般不手动跑）：
cd skills/loiter-flash/scripts
python flash.py --base https://loiter.polly.wang flash --port /dev/cu.usbmodem* --text "你随便写的一段话"
```

### 开发者手动烧录（无 profile_id，走空 pid fallback）

```bash
cd firmware && pio run -e islands -t upload
```

> ⚠️ v3′ 每人一个独特二进制（baked profile_id），**已删 OTA**：更新固件走 USB 重烧。

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
- @小龙虾🦞（架构 + 边界/踩坑）
- @Codex（协议 + 一致性）

