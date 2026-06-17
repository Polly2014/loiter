#!/usr/bin/env bash
# publish_ota.sh — Phase 7.6 OTA 一键发布
#
# 1. pio run -e islands            # 编译固件
# 2. sha256 .pio/build/islands/firmware.bin
# 3. scp 到 VM: /home/azureuser/GitHub_Workspace/loiter/web/firmware/loiter.bin
# 4. 写 web/firmware/manifest.json
# 5. (可选) 触发 MQTT 广播让在线设备立即升级
#
# 用法:
#   scripts/publish_ota.sh <version> [--targets all|card-abc,card-def] [--no-broadcast]
#
# 示例:
#   scripts/publish_ota.sh 0.3.0
#   scripts/publish_ota.sh 0.3.1 --targets card-30c6f7a8d044
#   scripts/publish_ota.sh 0.3.2 --no-broadcast    # 只上传/写 manifest，不广播

set -euo pipefail

# ============ 配置 ============
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIRMWARE_DIR="$REPO_ROOT/firmware"
BIN_LOCAL="$FIRMWARE_DIR/.pio/build/islands/firmware.bin"

# VM 部署目标（与 mqtt_bridge.py / WEB_DIR 对齐）
SSH_CONFIG="$REPO_ROOT/../SoulArena/ssh.config"
SSH_HOST="Azure-Server"
REMOTE_FW_DIR="/home/azureuser/GitHub_Workspace/loiter/web/firmware"
REMOTE_BIN_URL="https://loiter.polly.wang/firmware/loiter.bin"

# ============ 解析参数 ============
if [[ $# -lt 1 ]]; then
    echo "usage: $0 <version> [--targets all|card-...,card-...] [--no-broadcast]"
    exit 1
fi
VERSION="$1"; shift
TARGETS="all"
BROADCAST=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --targets) TARGETS="$2"; shift 2;;
        --no-broadcast) BROADCAST=0; shift;;
        *) echo "unknown arg: $1"; exit 1;;
    esac
done

# 校验 semver
if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "❌ version must be semver x.y.z (got: $VERSION)"; exit 1
fi

# ============ 1. 编译 ============
echo "==> 1/4  pio run -e islands"
cd "$FIRMWARE_DIR"
# 让编译时的 -DLOITER_FW_VERSION 跟传入版本一致，避免烧上去版本和 manifest 对不上
PLATFORMIO_BUILD_FLAGS="-DLOITER_FW_VERSION=\\\"$VERSION\\\"" pio run -e islands
[[ -f "$BIN_LOCAL" ]] || { echo "❌ no firmware.bin produced"; exit 1; }

SIZE=$(stat -f%z "$BIN_LOCAL" 2>/dev/null || stat -c%s "$BIN_LOCAL")
SHA=$(shasum -a 256 "$BIN_LOCAL" | awk '{print $1}')
echo "    firmware.bin  size=$SIZE  sha256=$SHA"

# ============ 2. 上传到 VM ============
echo "==> 2/4  scp firmware.bin → $SSH_HOST:$REMOTE_FW_DIR/"
ssh -F "$SSH_CONFIG" "$SSH_HOST" "mkdir -p $REMOTE_FW_DIR/archives"
# canonical：loiter.bin（URL 不变） + 归档：loiter-<version>.bin
scp -F "$SSH_CONFIG" "$BIN_LOCAL" "$SSH_HOST:$REMOTE_FW_DIR/loiter.bin"
ssh -F "$SSH_CONFIG" "$SSH_HOST" \
    "cp $REMOTE_FW_DIR/loiter.bin $REMOTE_FW_DIR/archives/loiter-$VERSION.bin"

# ============ 3. 写 manifest.json ============
echo "==> 3/4  write manifest.json"
MANIFEST=$(cat <<EOF
{
  "version": "$VERSION",
  "url": "$REMOTE_BIN_URL",
  "sha256": "$SHA",
  "size": $SIZE,
  "build_ts": $(date +%s000)
}
EOF
)
echo "$MANIFEST" | ssh -F "$SSH_CONFIG" "$SSH_HOST" "cat > $REMOTE_FW_DIR/manifest.json"
echo "    https://loiter.polly.wang/firmware/manifest.json updated"

# ============ 4. 广播触发升级 ============
if [[ $BROADCAST -eq 1 ]]; then
    echo "==> 4/4  POST /firmware/broadcast (targets=$TARGETS)"
    curl -sS -X POST "https://loiter.polly.wang/firmware/broadcast" \
        -H 'content-type: application/json' \
        -d "{\"targets\":\"$TARGETS\"}" | head -c 400
    echo
else
    echo "==> 4/4  skipped broadcast (--no-broadcast)"
    echo "       手动触发: curl -X POST https://loiter.polly.wang/firmware/broadcast \\"
    echo "                  -H 'content-type: application/json' -d '{\"targets\":\"$TARGETS\"}'"
fi

echo "✅ OTA v$VERSION published. 在线 Cardputer 会在收到 retain message 后自动升级。"
