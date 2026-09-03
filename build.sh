#!/bin/bash
set -e
cd "$(dirname "$0")"

# 用法: ./build.sh          # 关闭调试输出（默认）
#       ./build.sh DEBUG=1  # 打开 BootDbg 日志
DEBUG=0
for Arg in "$@"; do
    case "$Arg" in
        DEBUG=1|debug=1) DEBUG=1 ;;
        DEBUG=0|debug=0) DEBUG=0 ;;
    esac
done

EDK2_ROOT="$(cd .. && pwd)"
cd "$EDK2_ROOT"
# shellcheck disable=SC1091
source edksetup.sh

build -a X64 -p ToyBoot/Boot.dsc -t GCC -D TOY_BOOT_DEBUG="$DEBUG"

EFI_OUT="$EDK2_ROOT/Build/ToyBoot/DEBUG_GCC/X64/ToyBoot.efi"
IMG_DIR="$EDK2_ROOT/ToyImage/EFI/BOOT"

if [ ! -f "$EFI_OUT" ]; then
    echo "Build failed: $EFI_OUT not found"
    exit 1
fi

mkdir -p "$IMG_DIR"
# FAT/vvfat 上 .efi 与 .EFI 是两个文件；删掉旧小写文件，避免 OVMF 仍启动旧引导
rm -f "$IMG_DIR/BOOTX64.efi" "$IMG_DIR/BOOTX64.EFI"
cp -f "$EFI_OUT" "$IMG_DIR/BOOTX64.EFI"

echo "=========================================="
echo "Build successful! TOY_BOOT_DEBUG=$DEBUG"
echo "Installed: $IMG_DIR/BOOTX64.EFI"
ls -lh "$IMG_DIR/BOOTX64.EFI"
echo "=========================================="
