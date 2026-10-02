#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# 用法: ./build.sh          # 关闭调试输出（默认）
#       ./build.sh DEBUG=1  # 打开 BootDbg 日志
# BOX-5：TOYOS_ROOT + EDK2_SRC（Config.local）时可在 ~/ToyOS/ToyBoot 下编，
#        实际 edksetup/包路径仍走备份 EDK2；EFI 装到 $TOYOS_ROOT/ToyImage。
DEBUG=0
for Arg in "$@"; do
    case "$Arg" in
        DEBUG=1|debug=1) DEBUG=1 ;;
        DEBUG=0|debug=0) DEBUG=0 ;;
    esac
done

if [ -n "${TOYOS_ROOT:-}" ] && [ -f "${TOYOS_ROOT}/Config.local.txt" ]; then
    # shellcheck disable=SC1090
    . "${TOYOS_ROOT}/Config.local.txt"
fi

if [ -n "${TOYOS_ROOT:-}" ] && [ -f "${TOYOS_ROOT}/EDK2/edksetup.sh" ]; then
    EDK2_ROOT="$(cd "${TOYOS_ROOT}/EDK2" && pwd)"
elif [ -n "${EDK2_SRC:-}" ] && [ -f "${EDK2_SRC}/edksetup.sh" ]; then
    EDK2_ROOT="$(cd "${EDK2_SRC}" && pwd)"
elif [ -f "$(cd "$SCRIPT_DIR/.." && pwd)/edksetup.sh" ]; then
    EDK2_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
else
    echo "error: 找不到 EDK2（$TOYOS_ROOT/EDK2 或 EDK2_SRC，或在 edk2/ToyBoot 下运行）" >&2
    exit 1
fi

cd "$EDK2_ROOT"
# shellcheck disable=SC1091
source edksetup.sh

build -a X64 -p ToyBoot/Boot.dsc -t GCC -D TOY_BOOT_DEBUG="$DEBUG"

EFI_OUT="$EDK2_ROOT/Build/ToyBoot/DEBUG_GCC/X64/ToyBoot.efi"
if [ -n "${TOYOS_ROOT:-}" ] && [ -d "${TOYOS_ROOT}/ToyImage" ]; then
    IMG_DIR="${TOYOS_ROOT}/ToyImage/Esp/X64/EFI/BOOT"
else
    IMG_DIR="$EDK2_ROOT/ToyImage/Esp/X64/EFI/BOOT"
fi

if [ ! -f "$EFI_OUT" ]; then
    echo "Build failed: $EFI_OUT not found"
    exit 1
fi

mkdir -p "$IMG_DIR"
rm -f "$IMG_DIR/BOOTX64.efi" "$IMG_DIR/BOOTX64.EFI"
cp -f "$EFI_OUT" "$IMG_DIR/BOOTX64.EFI"

echo "=========================================="
echo "Build successful! TOY_BOOT_DEBUG=$DEBUG"
echo "EDK2_ROOT=$EDK2_ROOT"
echo "Installed: $IMG_DIR/BOOTX64.EFI"
ls -lh "$IMG_DIR/BOOTX64.EFI"
echo "=========================================="
