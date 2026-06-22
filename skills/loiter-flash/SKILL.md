---
name: loiter-flash
description: Loiter「Islands of Color」vibe-coding 烧录器——参与者打一段自由文本，脚本把它发给 server（server 负责分岛 + 生成文艺解读），生成只带一次性 profile_id 的专属固件并烧进 M5 Cardputer。Use when (1) a workshop participant wants to flash their own Islands-of-Color device, (2) 烧录 M5 Cardputer for the Pride workshop, (3) 生成专属固件 / 个性化烧录, (4) "给我烧一台" / "参加 Islands of Color" / "烧录我的设备". Triggers on keywords like "loiter 烧录", "islands of color", "烧我的卡片机", "vibe coding 烧录", "flash my cardputer", "Pride workshop 烧录". NOT for editing the loiter firmware/server code (that's normal coding) or the big-screen.
---

# loiter-flash — Vibe-Coding 烧录器（v3′ 薄版）

把参与者的一段话，变成一台只属于他的发光设备。

**架构**：**Skill 做烧录器，Server 做分岛与解读，Firmware 只带一次性 profile_id。**
你（agent）**不分析、不分岛、不写解读**——只收文本、跑脚本。岛屿与文艺 reason 全在 server，
设备烧好、输完名字后才在屏上揭晓。所以你在终端里**天生不会剧透**（你也不知道结果）。

---

## 工作流程（3 步）

### Step 1 · 收集自由文本

用**当前环境可用的输入机制**让参与者**随便写一段话**——今天的心情、一件喜欢的事、一句想说的话、随便什么。**不限格式，越自由越好**。

> 输入机制按环境选：VS Code/Copilot 用 `vscode_askQuestions`；Codex 有 `request_user_input` 则用之；都没有则**直接问用户**。
> 只收文字，不要问名字（名字在设备上输）。

### Step 2 · 烧录

```bash
python scripts/flash.py flash --text "<参与者写的原文>"
```

脚本会：`POST /flash/profile`（server 原子顺序轮转分岛 + 异步预生成文艺双语 reason）→ 拿 `profile_id` → 只 bake `LOITER_PROFILE_ID` 进 `firmware/src/user_profile.h` → 自动装 PlatformIO（如缺）→ 编译 → 烧录。

- 设备用**数据线**连上（非充电线）。多台串口时加 `--port <口>`。
- 需 `LOITER_FLASH_TOKEN`（工作坊密钥，引导员提供；也可 `--flash-token` 传）。
- 想刷新到最新固件：加 `--pull`。只编译不烧（自测）：加 `--skip-upload`。

> ⚠️ **server 必须在线**才能分岛（这是中心化设计的代价）。token 错/server 不可达会明确报错，不会静默烧一个没身份的设备。

### Step 3 · 不剧透收尾

烧录成功后对参与者说类似：
> ✨ 好了！拿起你的设备，输入名字——看看 server 把你分到了哪座岛、为什么。

你确实不知道岛屿/reason（都在 server），所以照实说即可。

---

## 环境体检（出问题先跑）

```bash
python scripts/flash.py doctor
```

列出 OS / Python / PlatformIO / 串口候选 / server 可达性 / flash token 是否已设。

常见坑：
- **看不到串口** → 装 CH9102/CP210x 驱动，换**数据线**，重插 USB。
- **pio 找不到** → 脚本会自动 `pip install --user platformio`；PATH 没生效就重开终端或用 `python -m platformio`。
- **Linux permission denied** → `sudo usermod -aG dialout $USER` 后重登。
- **flash token 报错** → 设环境变量 `LOITER_FLASH_TOKEN`（引导员给）。

## 监控（引导员用）

```bash
python scripts/flash.py tally   # 各岛已创建 profile 数（只读公开）
```

## 边界

- 脚本**只写 `user_profile.h`（仅 `LOITER_PROFILE_ID`），绝不碰 `config.h`**（WiFi/broker 密码在 config.h，gitignored）。
- **原文不进二进制**：二进制里只有不可逆的 profile_id，原文/岛屿/reason 全在 server。
- server base 可用 `--base` 或环境变量 `LOITER_FLASH_BASE` 覆盖（默认 `https://loiter.polly.wang`）。
