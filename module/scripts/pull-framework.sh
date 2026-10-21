#!/usr/bin/env bash
# 从已连接设备拉取编译/打包需要 Android 系统框架文件（本机没有卸载 Android SDK 也可构建）。
# 用法: adb devices 确认设备后执行 ./scripts/pull-framework.sh
set -euo pipefail

MODULE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT_DIR="$(cd "$MODULE_DIR/../.." && pwd)"   # 仓库根（program/ 的上一级）；work/ 在那里
BOOT_DIR="${BOOT_DIR:-$ROOT_DIR/work/boot}"
ADB="${ADB:-adb}"
SERIAL="${SERIAL:-}"
mkdir -p "$BOOT_DIR"

dev=(adb)
[ -n "$SERIAL" ] && dev=("$ADB" -s "$SERIAL")

# 1) 编译用的 boot classpath 三件套
for j in framework services android.test.runner; do
    "${dev[@]}" pull "/system/framework/$j.jar" "$BOOT_DIR/$j.jar"
done

# 2) aapt2 link 需要的 framework-res.apk（真机在 /system/framework/，模拟器/部分 ROM 在别处）
if [ ! -f "$BOOT_DIR/framework-res.apk" ]; then
    for p in /system/framework/framework-res.apk /system/framework/base.apk /system/framework/framework.jar; do
        if "${dev[@]}" shell "test -f $p" >/dev/null 2>&1; then
            "${dev[@]}" pull "$p" "$BOOT_DIR/framework-res.apk" && break
        fi
    done
fi

ls -la "$BOOT_DIR"
echo "boot dir ready: $BOOT_DIR"
