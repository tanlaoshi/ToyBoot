/*
 * BootHandoff.h - ToyBoot <-> Kernel handoff ABI (PR-R1)
 *
 * ToyBoot 本地副本：本目录可单独编，不依赖 ToyKernel 树。
 * 内核侧镜像：ToyKernel/HAL/X64/BootHandoff.h — 改布局必须两边一起改。
 *
 * x86_64 sizes: VIDEO 32, MEMORY_MAP 40, BOOT_CONFIG 632（含 GOP 模式表 + Gop 指针）.
 * PR-G-hotres-pc：VideoMode 带 ModeNumber；末尾 GopProtocol 供真机热切。
 */
#ifndef TOY_BOOT_HANDOFF_H
#define TOY_BOOT_HANDOFF_H

#ifndef EFIAPI
#include "BootTypes.h"
typedef void VOID;
#endif

#define TOY_VIDEO_MODE_MAX 32
#define TOY_BOOT_GOP_HANDOFF_MAGIC 0x314E4F47u /* 'GON1' */

typedef struct {
    UINT64 FrameBufferBase;
    UINT64 FrameBufferSize;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    UINT32 PixelsPerScanLine;
} TOY_VIDEO_CONFIG;

typedef struct {
    UINT32 Width;
    UINT32 Height;
    UINT32 ModeNumber;
    UINT32 Reserved;
} TOY_VIDEO_MODE;

typedef struct {
    VOID  *Buffer;
    UINTN  MapSize;
    UINTN  MapKey;
    UINTN  DescriptorSize;
    UINT32 DescriptorVersion;
} TOY_MEMORY_MAP;

typedef struct {
    TOY_VIDEO_CONFIG     VideoConfig;
    TOY_MEMORY_MAP       MemoryMap;
    UINT64               KernelEntry;
    UINT64               RsdpAddress;
    VOID                *SystemTable;
    UINT64               XhciBaseAddress;
    /*
     * PR-G-modes：ExitBootServices 后内核无法 QueryMode。
     * Boot 枚举可用 GOP 模式供 Settings 列表（去重 WxH）。
     * PR-G-hotres-pc：ModeNumber + GopProtocol 供真机运行时 SetMode。
     */
    UINT32               VideoModeCount;
    UINT32               VideoModePad;
    TOY_VIDEO_MODE       VideoModes[TOY_VIDEO_MODE_MAX];
    UINT64               GopProtocol;
} TOY_BOOT_CONFIG;

#if defined(__GNUC__)
_Static_assert(sizeof(TOY_VIDEO_CONFIG) == 32, "TOY_VIDEO_CONFIG size");
_Static_assert(sizeof(TOY_MEMORY_MAP) == 40, "TOY_MEMORY_MAP size");
_Static_assert(sizeof(TOY_VIDEO_MODE) == 16, "TOY_VIDEO_MODE size");
_Static_assert(sizeof(TOY_BOOT_CONFIG) == 632, "TOY_BOOT_CONFIG size");
_Static_assert(__builtin_offsetof(TOY_BOOT_CONFIG, VideoConfig) == 0, "VideoConfig off");
_Static_assert(__builtin_offsetof(TOY_BOOT_CONFIG, MemoryMap) == 32, "MemoryMap off");
_Static_assert(__builtin_offsetof(TOY_BOOT_CONFIG, KernelEntry) == 72, "KernelEntry off");
_Static_assert(__builtin_offsetof(TOY_BOOT_CONFIG, RsdpAddress) == 80, "RsdpAddress off");
_Static_assert(__builtin_offsetof(TOY_BOOT_CONFIG, SystemTable) == 88, "SystemTable off");
_Static_assert(__builtin_offsetof(TOY_BOOT_CONFIG, XhciBaseAddress) == 96, "XhciBase off");
_Static_assert(__builtin_offsetof(TOY_BOOT_CONFIG, VideoModeCount) == 104, "VideoModeCount off");
_Static_assert(__builtin_offsetof(TOY_BOOT_CONFIG, VideoModes) == 112, "VideoModes off");
_Static_assert(__builtin_offsetof(TOY_BOOT_CONFIG, GopProtocol) == 624, "GopProtocol off");
#endif

#endif
