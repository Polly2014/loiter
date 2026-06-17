# P4c 固件 Phase 3 四屏 — 实现方案（待三方 review，不写代码）

> 协作流：本方案 → 小龙虾 + Codex review → approve 后才动固件代码。

---

## 0. 一个推翻前提的发现

**P3-01~04 不是空壳。** 它们已经被 Designer 完整画好，只是喂的是**硬编码假数据**：

| 屏 | 现状 | 真实缺口 |
|---|---|---|
| P3-01 Waiting | ✅ 铃铛 + 8s 假进度 + 4 句状态文字轮播 | **8s 硬超时直接跳 P3-02**，从不发 `reading/request`、不等真 reading |
| P3-02 Identity Tag | ✅ 完整布局 | 标题硬编码 `"SUNRISE WANDERER"`、核心句硬编码 `"both burning and still."`（spectrum 色块读 `g_collection` 已是真的）|
| P3-03 Reading | ✅ 完整布局，**EN+CN 双语已用 `efontCN_14` 渲染** | `READING_PAGES[3]` 是硬编码 const（含**假中文翻译**）|
| P3-04 Closing | ✅ 彩虹弧 + look up，完成 | 无（已读 `g_collection` 画 6 色弧）|

**两个连带结论：**
1. 🦞 之前担心的 "Cubic-11 中文字体 subset" 问题**不存在** —— 固件已经编进 `efontCN_14` 并在 P3-03 正常渲染中文。
2. 工作量比"四屏从零写"小很多：**主要是把硬编码替换成 `reading/<uid>` 下发的真数据 + 接 reading 请求/超时/回调闭环**。

---

## 1. 锁死范围

**做：**
- P3-01 发真 `reading/request` + 优雅 loading（不假成功、不黑屏）
- `net.cpp/net.h` 新增 `reading/<uid>` 订阅 + `on_reading` 回调 + `net_request_reading()`
- `on_phase(3)` 把设备自动带进 P3-01
- P3-02/P3-03 用 `g_reading_*` 真数据替换硬编码
- loading / 30s 超时 / fallback / 重进 P3 缓存行为

**不做（明确排除）：**
- ❌ 预生成（三方已决：首发方案 C 按需）
- ❌ 集体 shake 庆祝
- ❌ 任何大屏新功能（P4b 已闭环）
- ❌ 不改 server reading 引擎（除非 review 决定走下面 §3 的"双语"路径）

---

## 2. server 实际下发什么（这是设计约束的根）

`reading/<uid>` payload（已上线，真 sonnet-4.5 验证过）：
```json
{ "title": "ROOTED FLAME",
  "lines": ["you carried forest calm to firelight",
            "collected embers while your roots held",
            "became the tree that learns to glow"],
  "island": 3, "color": "#6ab04c", "spectrum": [...] }
```

**= 英文 title + 3 行英文短诗 + island/color/spectrum。没有任何中文字段。**

但 Designer 的 M5_PRD §4 设计的是 **中英双语 + 150-200 字中文 reading**（P3-02 中文副标题 + 核心句、P3-03 三页中文长文）。**现状 server 喂不出 PRD 要的中文。** 这是 P4c 必须 review 拍板的核心岔路。

---

## 3. 核心决策 Q1 —— **裁决 = B-lite（双语短诗）**

> 🦞 投 full B（双语长文，`reading_cn[9]`，P3-03 纯中文）；Codex 投 B-lite（双语短诗，`lines_cn[6]`，P3-03 英中混排，不上 150-200 字长文）。
> **收敛 = B-lite**：保住机器上已验证的双语美感 + 三页阅读节奏，但不把 P4c 膨胀成 server prompt/fallback/协议/分页排版/AI 调性全重测的大后端重构。
> 👉 **待 Polly 最终确认 B-lite**（仅剩"P3-03 纯中文 9 行 vs 英中混排 6 行"这一分叉，B-lite 选后者）。

### B-lite payload schema（新增 CN 字段，**旧 `title/lines` 保留向后兼容** —— 🦞 加项 + Codex 同意）
```json
{ "title": "ROOTED FLAME",              // EN 身份标签（旧字段，大屏 P4b 仍读）
  "title_cn": "扎根的火光",              // ≤6 汉字
  "core_cn": "你把安静带进火光",          // 核心句 ≤12 汉字
  "lines": ["you carried forest calm to firelight",   // EN 短诗（旧字段保留）
            "collected embers while your roots held",
            "became the tree that learns to glow"],
  "lines_cn": ["你带着森林的安静", "走进一小片火光",      // CN 短句 ×6，每行 ≤12 汉字
               "你收下别人的余烬", "根却依然站得很稳",
               "今天你学会发光",   "也没有离开自己"],
  "island": 3, "color": "#6ab04c", "spectrum": [...] }
```

### 四屏映射（B-lite）
- **P3-02**：`title`(EN hero) + `title_cn`(中文副标题) + spectrum 5 格 + `core_cn`(核心句框) + "ENTER >> read more"
- **P3-03**：保留 **3 页**，每页 = 顶部 1 行 `lines[page]`（英文）+ 底部 2 行 `lines_cn[page*2]`/`lines_cn[page*2+1]`（中文）。**翻页逻辑 + `g_reading_page` 原样保留**，只换数据源（`READING_PAGES` const → 动态数组）
- **P3-04**：closing（不动）

### server 侧改动范围（reading.py）
1. **SYSTEM_PROMPT**：在现有"3 行英文短诗"基础上，加要求输出 `title_cn`(≤6 汉字) / `core_cn`(≤12 汉字) / `lines_cn`(6 句，每句 ≤12 汉字，与英文诗意对应但不机械直译)。JSON-only 不变。
2. **`_parse()`**：多取 `title_cn`/`core_cn`/`lines_cn`，对每个中文字段做 `s[:12]` 截断保底（防溢出，B4）；`lines_cn` 不足 6 句时用空串补齐到 6。
3. **`_publish_reading()`**：payload 加 `title_cn`/`core_cn`/`lines_cn`，**`title`/`lines` 原样保留**（大屏不改不崩）。
4. **6 套按岛 fallback**：每套补 `title_cn`/`core_cn`/`lines_cn[6]`（静态中文，调性不崩）。
5. **测试**：`test_reading.py` 扩字段断言（CN 字段存在 + 截断生效 + fallback 双语完整）。

> payload size：原 ~200B + CN ~300B ≈ 500B，PubSubClient 1024B buffer 够（B5）。

### 不做（B-lite 明确排除）
- ❌ 150-200 字中文长文（PRD 的 full B）
- ❌ 长文 fallback（固件本地 6 套也只做短句双语）
- ❌ 大屏吃中文字段（P4b 继续只用 `title`/`lines`，不阻塞 P4c；P5 后想要再加）

---


## 4. P3-01 真 loading（Codex review 重点 #2：不假成功 / 不黑屏）

**现状问题**：进 P3-01 只跑 8s 假进度，从不请求 reading，到点直接跳 P3-02 看硬编码。

**改为：**
1. 进屏（`goto_screen(P3_01_WAITING)`）时：**先 guard** —— 若 `g_reading_ready` 已 true（重进 P3-01 / phase 重广播）→ 直接 `goto_screen(P3_02)`，不重发请求（🦞 finding #1）；否则调 `net_request_reading()` 发 `reading/request {uid}`，记 `g_reading_req_ms = millis()`
2. **假进度条**：30s 从 0 涨到 **95% 封顶**。公式 `pct = min(95, elapsed*95/30000)`
3. 状态文字 4 句轮播保留（每 3s 一换，对齐 PRD 文案）
4. **`on_reading` 回调到达** → `g_reading_ready=true` → P3-01 把进度条 **flash 到 100% 停 300ms**（让用户感知"读条满了"）→ 再 `goto_screen(P3_02_REVEAL_TAG)`（🦞 §8 #2，更丝滑；用 `g_reading_ready_at` 计时，非阻塞 delay）
5. **30s 超时**（`elapsed >= 30000` 且 `!g_reading_ready`）→ 用**本地 6 套按岛双语 fallback**（不依赖网络，🦞 finding #4）存 `g_reading_*` → 跳 P3-02
6. 所有键忽略（PRD：强制专注等 reveal）

**为什么 30s 不是 8s**：真 sonnet-4.5 几秒就回，但留 30s 容错（网络抖动 / CopilotX 排队）。多数情况 reading 3-5s 到，进度条还在低位就被推满跳转。

---

## 5. net 层改动（Codex review 重点 #1：状态机入口不打架）

### net.h 新增（B-lite：回调携双语）
```c
// NetCallbacks 增一个（双语：EN title + lines[3] + CN title_cn/core_cn + lines_cn[6]）：
void (*on_reading)(const char* title, const char* title_cn, const char* core_cn,
                   const char* lines[3], const char* lines_cn[6]);
// 公开函数增一个：
void net_request_reading();   // C→S 发 loiter/hall/reading/request {uid}
```

### net.cpp 新增
- `mqttEnsure()` 订阅列表加 `mqtt.subscribe(("loiter/hall/reading/" + gUid).c_str(), 1);`
- `onMqttMessage` 加分支（取双语字段，设备侧不碰 spectrum/color——P3-02 色块读本地 `g_collection`）：
  ```cpp
  if (t == "loiter/hall/reading/" + gUid) {
      if (gCb.on_reading) {
          const char* en[3] = { doc["lines"][0]|"", doc["lines"][1]|"", doc["lines"][2]|"" };
          const char* cn[6];
          for (int i = 0; i < 6; ++i) cn[i] = doc["lines_cn"][i] | "";
          gCb.on_reading(doc["title"]|"", doc["title_cn"]|"", doc["core_cn"]|"", en, cn);
      }
      return;
  }
  ```
- `net_request_reading()`：`publish loiter/hall/reading/request {"uid": gUid}`（QoS 1，retain=false）

> payload ~500B（EN+CN）< PubSubClient 1024B buffer（B5）。JSON 库已是 ArduinoJson。

---

## 6. main.cpp 改动

### 新全局（B-lite 双语，替换硬编码；Codex 重点 #3：全 static char 无 String → 不引入堆碎片）
```c
static char g_reading_title[32]   = "";    // EN 身份标签，最长约 "SUNRISE WANDERER"=16
static char g_reading_title_cn[20]= "";    // CN 副标题 ≤6 汉字（3B/字 → 18B）
static char g_reading_core_cn[40] = "";    // CN 核心句 ≤12 汉字
static char g_reading_lines[3][64]= {""};  // 3 行英文短诗，单行 ≤38 字符
static char g_reading_lines_cn[6][40]={""};// 6 行中文短句，每行 ≤12 汉字（40B）
static bool g_reading_ready = false;
static uint32_t g_reading_req_ms = 0;
static uint32_t g_reading_ready_at = 0;    // 真到达时间戳（flash 100% 停 300ms 用）
```
总增量 ≈ 32+20+40+192+240 ≈ **524B**（RAM 16.8% 下可忽略，B3）。`strncpy` + 末尾强制 `\0`。

### `net_on_phase(3)`（状态机入口，防三者打架）
```c
static void net_on_phase(int phase) {
    if (phase == 3) {
        // 只在还没进 P3 时带入（已在 P3 任意屏 → 忽略，防回调把正在读 reading 的人踹回 loading）
        if (g_screen < P3_01_WAITING) {
            goto_screen(P3_01_WAITING);   // 进屏逻辑里发 reading/request（见 §4）
        }
    }
}
```
**三个入口的互斥**（Codex 重点 #1）：
- `phase=3` 广播 → 只有 `g_screen < P3_01_WAITING` 才带入（已在 P3 不打断）
- 用户按 `B`（dev）→ 同样 `goto_screen(P3_01_WAITING)`，进屏即请求
- `on_reading` 到达 → **只在 `g_screen == P3_01_WAITING` 时**推进到 P3-02（若用户已手翻到别屏则只存数据不跳，防打断）

### `net_on_reading` 回调（B-lite 双语）
```c
static void net_on_reading(const char* title, const char* title_cn, const char* core_cn,
                           const char* lines[3], const char* lines_cn[6]) {
    strncpy(g_reading_title,    title,    sizeof(g_reading_title)-1);
    strncpy(g_reading_title_cn, title_cn, sizeof(g_reading_title_cn)-1);
    strncpy(g_reading_core_cn,  core_cn,  sizeof(g_reading_core_cn)-1);
    for (int i = 0; i < 3; ++i) strncpy(g_reading_lines[i],    lines[i],    sizeof(g_reading_lines[0])-1);
    for (int i = 0; i < 6; ++i) strncpy(g_reading_lines_cn[i], lines_cn[i], sizeof(g_reading_lines_cn[0])-1);
    g_reading_ready = true;
    g_reading_ready_at = millis();
    // 只在 P3-01 推进（flash 100% 停 300ms 后跳 P3-02，在 draw_p3_01 里判 ready_at）；
    // 其他屏：数据已存，用户翻过去自然读到（不强跳，🦞 finding #5）
}
```
注册：`cb.on_reading = net_on_reading;`

### P3-01：进屏 guard + 发请求 + 进度封顶 95% + 真到达 flash 100% 停 300ms / 30s fallback（§4 逻辑）

### P3-02：删硬编码，读 `g_reading_title` + `g_reading_title_cn` + `g_reading_core_cn`
- hero EN 标题 → `g_reading_title`
- 中文副标题 → `g_reading_title_cn`（`efontCN_14` 渲染，B-lite 保留）
- 核心句框 → `g_reading_core_cn`
- spectrum 5 格不动（已读 `g_collection`）

### P3-03：**保留 3 页**，只换数据源（`READING_PAGES` const → 动态全局，🦞 finding #3）
- 每页 = 顶部 1 行英文 `g_reading_lines[page]` + 底部 2 行中文 `g_reading_lines_cn[page*2]` / `[page*2+1]`
- **翻页逻辑 + `g_reading_page` 原样保留**（DEL↔ENTER±页，Page 1 DEL→P3-02、Page 3 ENTER→P3-04），布局代码几乎不动

### P3-04：不动

### 重进 P3 缓存行为（Codex 重点）
`g_reading_ready` + `g_reading_*` 持久全局 → 用户从 P3-04 按 `←` 回看 / 离开 P3 再回，**直接读缓存**。`net_request_reading()` 只在"进 P3-01 且 `!g_reading_ready`"时发（server 侧也有 `m.reading` 缓存 + `(uid,gen)` inflight 双保险）。

### 超慢达达边界（🦞 finding #5）
若 reading 在用户已看完 fallback、到 P3-04 之后才迟到：`g_reading_ready=true` 后所有 draw 都读真数据，用户按 `←` 回看自然看到真 reading 而非 fallback。✅ 已覆盖。

---

## 7. 验证计划

- **server**：`reading.py` prompt + `_parse()` + `_publish_reading()` + 6 套 fallback 加 CN 字段；`test_reading.py` 扩断言（CN 字段存在 + `[:12]` 截断生效 + fallback 双语完整 + 旧 `title`/`lines` 仍在）；VM 上 `LOITER_NPC_ENABLED=true` 真跱一条验中文调性
- **编译**：`pio run -e islands`（Flash 当前 73.8%，加 ~524B char + 一个回调，预计 +<0.3%）
- **本地单机**：config.h `MQTT_HOST` 改回 Mac 局域网 IP；设备走完 P1/P2 → host 发 `phase=3`（或按 `B`）→ P3-01 真 loading → reading 到达 flash 100% 跳 P3-02 显真 EN+CN title+core → P3-03 三页英中混排 → P3-04 closing
- **超时路径**：临时停 server `reading/request`（或断网）→ 验 30s 本地双语 fallback 跳转、不黑屏、不假成功
- **重进缓存**：P3-04 按 `←` 回 P3-03 看同一份 reading，serial 日志确认无第二次 `reading/request`

---

## 8. review 状态（两轮后）

- 🦞：方案结构 9.0/10 APPROVE；投 full B。findings #1-5 已全部纳入（进 P3-01 guard / `g_screen<P3_01` enum 序确认 / P3-03 保 3 页 / fallback 本地双语 / 超慢达达边界）；§8 加项 server 向下兼容（新增 CN 字段但保留 `title`/`lines`）已纳入；loading flash 100% 停 300ms 已纳入
- Codex：投 B-lite（双语短诗，不上 150-200 字长文，每行 ≤12 汉字，保 3 页英中混排）

**收敛 = B-lite**（本方案 §3 采纳）。两人唯一分叉：P3-03 是「纯中文 9 行」(🦞) vs 「英中混排 6 行」(Codex)。B-lite 选后者。

### 待 Polly 拍板
- **确认 B-lite**（vs 🦞 的 full B 9 行纯中文）—— 这是唯一未定项，approve 后即开写。

### approve 后写代码的顺序
1. server `reading.py`（prompt + `_parse` + `_publish_reading` + 6 套 CN fallback）+ `test_reading.py` 扩展 → 跱 VM 验中文调性
2. 固件 `net.h`/`net.cpp`（订阅 + 回调 + `net_request_reading`）
3. 固件 `main.cpp`（全局 + `net_on_phase(3)` + `net_on_reading` + P3-01~03 去硬编码 + 本地 6 套双语 fallback）
4. 编译 + 本地单机验证 + 交 review
