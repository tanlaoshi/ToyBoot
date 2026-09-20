/*
 * BootPrivate.h — ToyBoot 模块间共享（PR-S-boot-1）
 *
 * BootDbg / 类型别名 / Video 原型。只搬家用，不改 ABI。
 */
#ifndef TOY_BOOT_PRIVATE_H
#define TOY_BOOT_PRIVATE_H

#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>

#include "BootHandoff.h"
#include "BootSerial.h"

#ifndef TOY_BOOT_DEBUG
#define TOY_BOOT_DEBUG 0
#endif
#if TOY_BOOT_DEBUG
#define BootDbg(...) BootSerialPrintf(__VA_ARGS__)
#else
#define BootDbg(...) do { } while (0)
#endif

typedef TOY_BOOT_CONFIG  BOOT_CONFIG;
typedef TOY_VIDEO_CONFIG VIDEO_CONFIG;
typedef TOY_MEMORY_MAP   MEMORY_MAP;

BOOLEAN IsVirtualMachine(VOID);
BOOLEAN IsModeUsable(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info);
EFI_STATUS TryGetEdidPreferred(EFI_HANDLE ImageHandle, EFI_HANDLE GopHandle,
                               UINT32 *PrefW, UINT32 *PrefH);
UINTN ScoreModeQemu(UINT32 W, UINT32 H);
BOOLEAN SameAspectRatio(UINT32 W, UINT32 H, UINT32 TargetW, UINT32 TargetH);
UINTN ScoreModeNative(UINT32 W, UINT32 H, UINT32 TargetW, UINT32 TargetH,
                      BOOLEAN HasTarget);
VOID SortVideoModesForSettings(TOY_VIDEO_MODE *Modes, UINT32 Count,
                               BOOLEAN InVm, BOOLEAN HasEdid,
                               UINT32 EdidW, UINT32 EdidH);
BOOLEAN TryLoadDisplayPref(EFI_HANDLE ImageHandle, UINT32 *OutW, UINT32 *OutH);
BOOLEAN FsHasToyOsId(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs);
EFI_STATUS GetAndSetVideo(EFI_HANDLE ImageHandle, VIDEO_CONFIG *VideoConfig,
                          BOOT_CONFIG *BootConfig);

#endif
