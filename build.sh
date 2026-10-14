#!/bin/bash
# desk-info 编译脚本：静态链接 ARM soft-float 二进制（独立于设备 hard-float uClibc）
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
SDK_PATH="${SDK_PATH:-$SCRIPT_DIR/toolchain/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot}"
CC="$SDK_PATH/bin/arm-buildroot-linux-uclibcgnueabi-gcc"

mkdir -p "$BUILD_DIR"
echo "CC: $CC"
"$CC" -static -Os -mcpu=cortex-a5 -mfloat-abi=soft \
    -I"$SCRIPT_DIR" \
    -o "$BUILD_DIR/xwplug-desk" "$SCRIPT_DIR/main.c" -lpthread -lm
cp -f "$BUILD_DIR/xwplug-desk" "$BUILD_DIR/desk-info"
echo "built: $BUILD_DIR/xwplug-desk (alias desk-info)"
ls -la "$BUILD_DIR/xwplug-desk"
