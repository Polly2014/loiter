# Loiter Firmware — Cardputer (M5Stack Cardputer-Adv)

Arduino + PlatformIO。ESP32-S3。

## Sprint 0 — echo 链路验证（先做这个）

目标：一台 Cardputer 把 `WiFi → MQTT broker → 收发` 整条链路打通。

### 1. 装工具链

```bash
pip install platformio        # 或 brew install platformio
```

### 2. 配置

先从模板拷贝（`config.h` 含 WiFi 密码，已 `.gitignore`，不会进 repo）：

```bash
cp src/config.h.example src/config.h
```

编辑 [`src/config.h`](src/config.h)：

- `WIFI_SSID` / `WIFI_PASS` — Mac Internet Sharing 自建热点的名字和密码
- `MQTT_HOST` — Mac 在热点网段的 IP（通常 `192.168.2.1`，用 `ipconfig getifaddr bridge100` 确认）

### 3. 烧录 + 监控

```bash
cd X-Workspace/loiter/firmware
pio run -e sprint0 -t upload      # USB-C 连 Cardputer 后烧录
pio device monitor -b 115200      # 看串口日志
```

> 第一次烧录如果识别不到端口：长按 Cardputer 侧边 G0 键再插 USB（进 bootloader）。

### 4. 验证（在 Mac 上）

```bash
# 监听所有 loiter 消息 → 应能看到 Cardputer 每 3s 的心跳
mosquitto_sub -t 'loiter/#' -v

# 反向：发一条 → Cardputer 屏幕应显示 "< {...}"
mosquitto_pub -t 'loiter/hall/echo' -m '{"hi":"mac"}'
```

屏幕上能看到：连上 WiFi → 拿到 IP → `MQTT OK` → 双向收发。
**到这一步 = Sprint 0 通过，80% 技术风险消除。**

---

## 正式固件（Sprint 1 起）

`pio run -e loiter -t upload`。模块拆分见 [../CLAUDE.md](../CLAUDE.md) 目录结构。

固件硬约束（来自 design review）：

- `#define MQTT_MAX_PACKET_SIZE 1024`（已写进 `platformio.ini` build_flags，L3）
- 只用 QoS 0/1，不用 QoS 2
- `status` topic 只收 `{count, ts}`，名单靠 join/leave 增量维护
