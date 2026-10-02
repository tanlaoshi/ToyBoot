#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# 用法: ./build.sh          # 关闭调试输出（默认）
#       ./build.sh DEBUG=1  # 打开 BootDbg 日志
# BOX-5：TOYOS_ROOT + EDK2_SRC（Scripts/Config.local.txt）时可在 ~/ToyOS/ToyBoot 下编，
#        实际 edksetup/包路径仍走备份 EDK2；EFI 装到 $TOYOS_ROOT/ToyImage。
# 产物：EDK WORKSPACE 下 Build/ToyBoot；有 TOYOS_ROOT 时再挂到 $TOYOS_ROOT/Build/ToyBoot。
DEBUG=0
for Arg in "$@"; do
    case "$Arg" in
        DEBUG=1|debug=1) DEBUG=1 ;;
        DEBUG=0|debug=0) DEBUG=0 ;;
    esac
done

if [ -n "${TOYOS_ROOT:-}" ]; then
    if [ -f "${TOYOS_ROOT}/Scripts/Config.local.txt" ]; then
        # shellcheck disable=SC1090
        . "${TOYOS_ROOT}/Scripts/Config.local.txt"
    elif [ -f "${TOYOS_ROOT}/Config.local.txt" ]; then
        # shellcheck disable=SC1090
        . "${TOYOS_ROOT}/Config.local.txt"
    fi
fi

if [ -z "${TOYOS_ROOT:-}" ] && [ -d "$SCRIPT_DIR/../ToyKernel" ] && \
   { [ -d "$SCRIPT_DIR/../Scripts" ] || [ -f "$SCRIPT_DIR/../Config.txt" ]; }; then
    TOYOS_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
    export TOYOS_ROOT
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
# 勿沿用旧机 Conf/BuildEnv.sh 里写死的 EDK_TOOLS_PATH（拷机后常指向不存在的备份树）
unset EDK_TOOLS_PATH
if [ -d "$EDK2_ROOT/BaseTools" ]; then
    export EDK_TOOLS_PATH="$EDK2_ROOT/BaseTools"
fi
# shellcheck disable=SC1091
source edksetup.sh

if ! command -v build >/dev/null 2>&1; then
    echo "error: edksetup 后仍无 build（检查 $EDK2_ROOT/BaseTools/BinWrappers/PosixLike）" >&2
    echo "hint: 删掉过期 Conf/BuildEnv.sh 后重试；或 make -C \"\$EDK_TOOLS_PATH/Source/C\"" >&2
    exit 1
fi

# command：避开外层 source Scripts/env.sh 时可能 export -f 的同名函数
command build -a X64 -p ToyBoot/Boot.dsc -t GCC -D TOY_BOOT_DEBUG="$DEBUG"

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

# 树根 Build/ToyBoot：与 Kernel 并列展示
if [ -n "${TOYOS_ROOT:-}" ]; then
    EDK_BOOT_BUILD="$(cd "$(dirname "$EFI_OUT")/../.." && pwd)"
    mkdir -p "${TOYOS_ROOT}/Build"
    DEST_BOOT="${TOYOS_ROOT}/Build/ToyBoot"
    if [ "$EDK_BOOT_BUILD" = "$DEST_BOOT" ]; then
        :
    elif [ -L "$DEST_BOOT" ] || [ ! -e "$DEST_BOOT" ]; then
        ln -sfn "$EDK_BOOT_BUILD" "$DEST_BOOT"
        echo "BUILDDIR=$DEST_BOOT → $EDK_BOOT_BUILD"
    else
        echo "note: $DEST_BOOT exists; EDK output at $EDK_BOOT_BUILD"
    fi
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
