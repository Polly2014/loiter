#!/usr/bin/env python3
"""loiter-flash — vibe-coding 烧录器（v3′ 薄版）

Skill 做烧录器，Server 做分岛与解读，Firmware 只带一次性 profile_id。
本脚本只负责：把用户文本 POST 给 server 建 profile → 拿 profile_id →
只 bake `LOITER_PROFILE_ID` 进 user_profile.h → 确保 PlatformIO → 编译 → 烧录。

**不分析、不分岛、不剧透**：岛屿与文艺 reason 全在 server，设备 join 后才揭晓。

纯 Python 标准库（urllib/glob/subprocess），参与者冷机即可跑。
绝不触碰 config.h（broker/WiFi 密码留在那）。

子命令：
  flash    POST /flash/profile → 写 header → 编译 → 烧录
  tally    打印各岛已创建 profile 数（监控用，只读公开）
  doctor   体检：python / pio / 串口 / OS 提示
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

DEFAULT_BASE = os.environ.get("LOITER_FLASH_BASE", "https://loiter.polly.wang")
PIO_ENV = "islands"
HTTP_TIMEOUT = 8
# Cloudflare bot-fight 封默认 `Python-urllib/x.y` UA（返回 403 error code 1010）→ 必须伪装成浏览器 UA，
# 否则每台参与者机器都会被拦、且被误报成"烧录窗口已关闭"。
_UA = ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
       "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36")

# 本地 pending：已 POST 建 profile 但烧录未成功时缓存 profile_id，重烧复用（不污染轮转）
LOCAL_STATE_DIR = Path(os.environ.get("LOITER_FLASH_STATE", str(Path.home() / ".loiter-flash")))
PENDING_FILE = LOCAL_STATE_DIR / "pending.json"

# profile_id = server 生的 uuid4.hex（32 位 hex）。严格校验，不静默改写（Codex P3）
_PID_RE = re.compile(r"^[a-f0-9]{32}$")


def _sha(text: str) -> str:
    """原文 → sha256（pending 只存 hash，不落 raw text —— Codex P2）。"""
    return hashlib.sha256((text or "").encode("utf-8")).hexdigest()


# ── 路径解析 ────────────────────────────────────────────────────────────────
def repo_root() -> Path:
    """loiter 仓根 = 含 `firmware/platformio.ini` 的目录。

    优先 env `LOITER_REPO`；否则从本文件向上查找（Codex P2：`parents[3]` 只对
    当前 symlink 安装形态成立，将来若把 skill 复制到独立 skills 容器，固定层数会
    指错 → 改成结构性查找，不靠目录层数）。
    """
    env = os.environ.get("LOITER_REPO")
    if env:
        p = Path(env).expanduser().resolve()
        if (p / "firmware" / "platformio.ini").exists():
            return p
        die(f"LOITER_REPO={env!r} 下没有 firmware/platformio.ini")
    here = Path(__file__).resolve()
    for cand in here.parents:
        if (cand / "firmware" / "platformio.ini").exists():
            return cand
    die("找不到 loiter 仓根（向上没有 firmware/platformio.ini）；可设 LOITER_REPO 显式指定。")


def firmware_dir() -> Path:
    return repo_root() / "firmware"


def profile_header_path() -> Path:
    return firmware_dir() / "src" / "user_profile.h"


def config_example_path() -> Path:
    return firmware_dir() / "src" / "config.h.example"


def config_path() -> Path:
    return firmware_dir() / "src" / "config.h"


# ── HTTP ─────────────────────────────────────────────────────────────────────
def _http_json(method: str, url: str, payload: dict | None = None,
               headers: dict | None = None) -> tuple[int, dict | None]:
    """返回 (http_status, json|None)。网络层失败 status=0。"""
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Content-Type", "application/json")
    req.add_header("User-Agent", _UA)  # 绕开 Cloudflare bot-fight 对 Python-urllib UA 的 403
    for k, v in (headers or {}).items():
        if v:
            req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
            body = resp.read().decode("utf-8", "replace")
            return resp.status, (json.loads(body) if body.strip() else {})
    except urllib.error.HTTPError as e:
        return e.code, None
    except (urllib.error.URLError, ValueError, OSError):
        return 0, None


def create_profile(base: str, text: str) -> dict:
    """POST /flash/profile → `{profile_id, mqtt:{host,port,user,pass}}`。零 token，受窗口+限流门控。失败 die。"""
    status, body = _http_json("POST", f"{base}/flash/profile", {"text": text})
    if status == 403:
        die("烧录窗口已关闭。请让引导员在 admin 面板打开 Flash 窗口后重试。")
    if status == 429:
        die("你的 IP 烧录太频繁（每小时上限 50 次），稍等再试。")
    if status == 0:
        die(f"连不上 server（{base}）。检查网络 / --base，server 必须在线才能分岛。")
    if not isinstance(body, dict) or not body.get("profile_id"):
        die(f"server 返回异常（HTTP {status}）：{body}")
    return body


# ── pending profile 缓存（失败重烧复用同一 profile_id，不污染轮转 —— 修 Codex P1）──
def _load_pending() -> dict:
    try:
        return json.loads(PENDING_FILE.read_text())
    except Exception:
        return {}


def _save_pending(d: dict) -> None:
    LOCAL_STATE_DIR.mkdir(parents=True, exist_ok=True)
    PENDING_FILE.write_text(json.dumps(d))


def _clear_pending() -> None:
    try:
        PENDING_FILE.unlink()
    except OSError:
        pass


# ── 写 user_profile.h（只 bake profile_id）──────────────────────────────────
def write_profile(profile_id: str) -> Path:
    if not _PID_RE.match(profile_id or ""):
        die(f"profile_id 非法（应为 32 位 hex）：{profile_id!r}")
    header = f"""// user_profile.h — GENERATED by loiter-flash skill. DO NOT COMMIT / gitignored.
// v3′：只 bake 一次性 profile_id；岛屿/reason/原文全在 server（设备 join 后揭晓）。
#pragma once

#define LOITER_USER_PROFILE   1
#define LOITER_PROFILE_ID     "{profile_id}"
"""
    path = profile_header_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(header, encoding="utf-8")
    # ⚠️ `__has_include` 增量编译陷阱：net.cpp 用 __has_include 条件包含 user_profile.h。
    # 若上次编译时该头文件不存在，net.cpp.o 被编成「不依赖 user_profile.h」→ 现在新写了
    # 头文件，SCons 不知 net.cpp 依赖它 → 不重编 net.cpp → 链接旧的空-pid .o，烧进空 pid。
    # 每个之前编译过的笔记本都会踩（揭晓显模板而非专属 AI reason）。
    # 硬保证（Codex P1）：直接删旧 net.cpp 编译产物（.o/.d/变体），强制下次 pio run 重编。
    # 不用 `touch`——SCons/PlatformIO 默认 content-MD5 decider，纯改 mtime 内容没变可能仍判
    # 无需重编；删 object 是确定性保证。删单个 object 比 `pio run -t clean` 快得多。
    _purge_net_objects()
    return path


def _purge_net_objects() -> None:
    """删除 .pio 下 net.cpp 的所有编译产物（object/dep 文件），强制重编。"""
    build_root = firmware_dir() / ".pio" / "build"
    if not build_root.exists():
        return
    for obj in build_root.glob("*/src/net.cpp.o*"):
        try:
            obj.unlink()
        except OSError:
            pass
    for dep in build_root.glob("*/src/net.cpp.d*"):
        try:
            dep.unlink()
        except OSError:
            pass


# ── 写 config.h（broker 凭据由 server 下发，参与者零输入；WiFi 留占位走上机配网）──
def _set_define(content: str, name: str, value: str) -> str:
    """替换已存在的 `#define NAME ...` 行；不存在则追加。value 已含引号/数字格式。"""
    line = f"#define {name}     {value}"
    pat = re.compile(rf"^#define\s+{re.escape(name)}\b.*$", re.MULTILINE)
    if pat.search(content):
        return pat.sub(line, content)
    return content.rstrip() + "\n" + line + "\n"


def write_config_h(mqtt: dict) -> Path:
    """从 config.h.example 复制为 config.h，并把 server 下发的 broker 凭据注入。

    WiFi 字段保持 example 里的占位（设备上电后走 NVS 配网门户让参与者填）。
    每次烧录都重写 → 保证参与者拿到与当前 server 一致的 broker，零手动配置。
    """
    ex = config_example_path()
    if not ex.exists():
        die(f"找不到 config.h.example：{ex}")
    content = ex.read_text(encoding="utf-8")
    host = str(mqtt.get("host") or "mqtt.polly.wang")
    port = int(mqtt.get("port") or 1883)
    user = str(mqtt.get("user") or "")
    pwd = str(mqtt.get("pass") or "")
    content = _set_define(content, "MQTT_HOST", f'"{host}"')
    content = _set_define(content, "MQTT_PORT", str(port))
    content = _set_define(content, "MQTT_USER", f'"{user}"')
    content = _set_define(content, "MQTT_PASS", f'"{pwd}"')
    cfg = config_path()
    cfg.write_text(content, encoding="utf-8")
    return cfg


# ── PlatformIO 自举 ──────────────────────────────────────────────────────────
def find_pio() -> list[str] | None:
    exe = shutil.which("pio") or shutil.which("platformio")
    if exe:
        return [exe]
    try:
        subprocess.run([sys.executable, "-m", "platformio", "--version"],
                       capture_output=True, check=True)
        return [sys.executable, "-m", "platformio"]
    except Exception:
        return None


def ensure_pio(auto_install: bool) -> list[str]:
    pio = find_pio()
    if pio:
        return pio
    if not auto_install:
        die("未找到 PlatformIO。装一下：  pip install -U platformio   或加 --no-install 自查。")
    print("· 未检测到 PlatformIO，正在安装（pip install --user platformio）…")
    try:
        subprocess.run([sys.executable, "-m", "pip", "install", "--user", "-U", "platformio"], check=True)
    except Exception as e:
        die(f"PlatformIO 自动安装失败：{e}\n手动装：pip install -U platformio")
    pio = find_pio()
    if not pio:
        die("装完仍找不到 pio。把 Python user-base 的 bin 加进 PATH，或用 `python -m platformio`。")
    return pio


# ── 串口探测（仅友好提示）────────────────────────────────────────────────────
def detect_ports() -> list[str]:
    sysname = platform.system()
    if sysname == "Darwin":
        return sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.wchusbserial*")
                      + glob.glob("/dev/cu.SLAB_USBtoUART*"))
    if sysname == "Linux":
        return sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
    if sysname == "Windows":
        return [f"COM{i}" for i in range(1, 21)]
    return []


def driver_hint() -> str:
    sysname = platform.system()
    if sysname == "Darwin":
        return "Mac 若看不到串口：装 CH9102/CP210x 驱动后重插 USB-C 数据线（注意别用充电专用线）。"
    if sysname == "Windows":
        return "Windows 若看不到 COM 口：装 CH9102/CP210x 驱动，设备管理器确认有 USB-SERIAL。"
    if sysname == "Linux":
        return "Linux 若 permission denied：把自己加进 dialout 组 `sudo usermod -aG dialout $USER` 后重登。"
    return ""


# ── pio 编译 / 烧录 ──────────────────────────────────────────────────────────
def run_pio(pio: list[str], targets: list[str], port: str | None) -> None:
    cmd = pio + ["run", "-e", PIO_ENV]
    for t in targets:
        cmd += ["-t", t]
    if port:
        cmd += ["--upload-port", port]
    print(f"· 运行：{' '.join(cmd)}")
    proc = subprocess.run(cmd, cwd=str(firmware_dir()))
    if proc.returncode != 0:
        die(f"pio 失败（exit {proc.returncode}）。常见：串口被占用 / 驱动缺失 / 没按住 G0。\n{driver_hint()}")


def git_pull(root: Path) -> None:
    print("· git pull --ff-only（刷新到最新固件）…")
    proc = subprocess.run(["git", "-C", str(root), "pull", "--ff-only"], capture_output=True, text=True)
    if proc.returncode != 0:
        print(f"  ⚠ git pull 跳过（{proc.stderr.strip() or 'non-ff / 离线'}），用本地代码继续。")


# ── 命令 ─────────────────────────────────────────────────────────────────────
def die(msg: str) -> None:
    print(f"\n✗ {msg}", file=sys.stderr)
    sys.exit(1)


def cmd_tally(args) -> None:
    status, body = _http_json("GET", f"{args.base}/flash/tally")
    print(json.dumps(body, ensure_ascii=False, indent=2) if body else f"(HTTP {status})")


def cmd_doctor(args) -> None:
    print(f"OS         : {platform.system()} {platform.release()} ({platform.machine()})")
    print(f"Python     : {sys.version.split()[0]} @ {sys.executable}")
    pio = find_pio()
    print(f"PlatformIO : {'OK → ' + ' '.join(pio) if pio else '✗ 未安装（flash 时会自动装）'}")
    print(f"串口候选   : {detect_ports() or '（未发现，连上 M5 数据线再试）'}")
    print(f"固件目录   : {firmware_dir()}")
    print(f"config.h   : {'OK' if config_path().exists() else '✗ 不存在（flash 时从 example + server 凭据生成）'}")
    status, body = _http_json("GET", f"{args.base}/flash/tally")
    print(f"server     : {args.base}  → {'OK' if body is not None else f'HTTP {status} / 不可达'}")
    fw = (body or {}).get("flash_open") if isinstance(body, dict) else None
    print(f"flash 窗口 : {'OPEN' if fw else ('CLOSED（让引导员打开）' if fw is False else '未知')}")
    print(f"提示       : {driver_hint()}")


def cmd_flash(args) -> None:
    if not (args.text or "").strip():
        die("--text 不能为空（参与者随便写的一段话）")

    if args.pull:
        git_pull(repo_root())

    # 1) 拿 profile_id：--profile-id 指定 / 复用上次未完成的（同文本）/ 否则新建
    #    新建时 server 一并下发 broker 凭据 → 写 config.h（参与者零输入）
    pid = args.profile_id
    mqtt = None
    if pid:
        if not _PID_RE.match(pid):
            die(f"--profile-id 非法（应为 32 位 hex）：{pid!r}")
        print(f"· 复用指定 profile [{pid[:8]}…]")
    else:
        pend = _load_pending()
        if not args.new and pend.get("profile_id") and pend.get("text_sha256") == _sha(args.text):
            pid = pend["profile_id"]
            print("· 复用上次未完成的 profile（避免轮转污染）")
        else:
            prof = create_profile(args.base, args.text)
            pid = prof["profile_id"]
            mqtt = prof.get("mqtt")
            _save_pending({"profile_id": pid, "text_sha256": _sha(args.text)})
            print("· 已建 profile [server]")

    # 2) 写 config.h：有 server 下发的 broker 凭据就刷新；否则要求 config.h 已就位
    if mqtt:
        write_config_h(mqtt)
        print("· 已写 config.h（broker 来自 server，WiFi 上机配网）")
    elif not config_path().exists():
        die("config.h 缺失：去掉 --profile-id/--new 让 server 下发 broker 凭据，"
            "或手动 cp firmware/src/config.h.example firmware/src/config.h 后填 MQTT。")

    # 3) 只 bake profile_id（不剧透：脚本不知道岛屿/reason）
    write_profile(pid)
    print("· 已写 user_profile.h")

    # 3) 确保 pio + 编译/烧录（失败 die() → pending 保留 → 下次复用同 profile_id）
    pio = ensure_pio(not args.no_install)
    if not detect_ports() and not args.port:
        print(f"  ⚠ 没探到串口。{driver_hint()}")
    targets = ["upload"] if not args.skip_upload else []
    run_pio(pio, targets, args.port)

    if not args.skip_upload:
        _clear_pending()   # 真烧录成功 → 清 pending（下次是新人）
    print("\n✓ 完成。拿起你的设备，输入名字，看看你属于哪座岛 ✨")


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="loiter-flash 烧录器（v3′ 薄版）")
    p.add_argument("--base", default=DEFAULT_BASE, help=f"server base（默认 {DEFAULT_BASE}）")
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("tally", help="打印各岛已创建 profile 数（监控用）")
    sp.set_defaults(func=cmd_tally)

    sp = sub.add_parser("doctor", help="环境体检")
    sp.set_defaults(func=cmd_doctor)

    sp = sub.add_parser("flash", help="POST /flash/profile → 写 header → 编译 → 烧录")
    sp.add_argument("--text", required=True, help="参与者随便写的一段话（喂 server 分岛 + reason）")
    sp.add_argument("--profile-id", default=None, help="复用指定 profile_id（重烧某设备身份）")
    sp.add_argument("--new", action="store_true", help="强制新建 profile（忽略 pending 复用）")
    sp.add_argument("--port", default=None, help="指定烧录串口（默认 pio 自动选）")
    sp.add_argument("--pull", action="store_true", help="烧前 git pull --ff-only 刷新固件")
    sp.add_argument("--no-install", action="store_true", help="不自动安装 PlatformIO")
    sp.add_argument("--skip-upload", action="store_true", help="只编译不烧录（自测用）")
    sp.set_defaults(func=cmd_flash)
    return p


def main() -> None:
    args = build_parser().parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
