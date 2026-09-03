#include <Uefi.h>
#include <Guid/Acpi.h>
#include <Guid/FileInfo.h>
#include <Protocol/PciIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/EdidActive.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>

/*
 * 调试输出开关：默认关闭。
 * 开启：./build.sh DEBUG=1  或  build -D TOY_BOOT_DEBUG=1
 * 失败类 Print 始终输出，不受此开关影响。
 */
#ifndef TOY_BOOT_DEBUG
#define TOY_BOOT_DEBUG 0
#endif
#if TOY_BOOT_DEBUG
#define BootDbg(...) Print(__VA_ARGS__)
#else
#define BootDbg(...) do { } while (0)
#endif

#define PT_LOAD 1

#pragma pack(1)
typedef struct {
    UINT32 Magic; UINT8 Format; UINT8 Endianness; UINT8 Version; UINT8 OSAbi;
    UINT8 AbiVersion; UINT8 Reserved[7]; UINT16 Type; UINT16 Machine;
    UINT32 ElfVersion; UINT64 Entry; UINT64 Phoff; UINT64 Shoff; UINT32 Flags;
    UINT16 HeadSize; UINT16 PHeadSize; UINT16 PHeadCount; UINT16 SHeadSize;
    UINT16 SHeadCount; UINT16 SNameIndex;
} ELF_HEADER_64;

typedef struct {
    UINT32 Type; UINT32 Flags; UINT64 Offset; UINT64 VAddress; UINT64 PAddress;
    UINT64 SizeInFile; UINT64 SizeInMemory; UINT64 Align;
} PROGRAM_HEADER_64;
#pragma pack()

typedef struct {
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN                FrameBufferSize;
    UINT32               HorizontalResolution;
    UINT32               VerticalResolution;
    UINT32               PixelsPerScanLine;
} VIDEO_CONFIG;

typedef struct {
    VOID   *Buffer;
    UINTN  MapSize;
    UINTN  MapKey;
    UINTN  DescriptorSize;
    UINT32 DescriptorVersion;
} MEMORY_MAP;

typedef struct {
    VIDEO_CONFIG         VideoConfig;
    MEMORY_MAP           MemoryMap;
    EFI_PHYSICAL_ADDRESS KernelEntry;
    EFI_PHYSICAL_ADDRESS RsdpAddress;
    EFI_SYSTEM_TABLE     *SystemTable;
    UINT64               XhciBaseAddress;    // 新增：XHCI MMIO 基址
} BOOT_CONFIG;

/* 虚拟机（QEMU/KVM 等）上保持窗口友好的分辨率表；真机走 EDID/最大模式 */
STATIC BOOLEAN IsVirtualMachine(VOID) {
    UINT32 Eax;
    UINT32 Ebx;
    UINT32 Ecx;
    UINT32 Edx;

    AsmCpuid(1, &Eax, &Ebx, &Ecx, &Edx);
    return (Ecx & BIT31) != 0;
}

STATIC BOOLEAN IsModeUsable(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info) {
    if (Info == NULL) {
        return FALSE;
    }
    if (Info->PixelFormat == PixelBltOnly) {
        return FALSE;
    }
    if (Info->HorizontalResolution < 640 || Info->VerticalResolution < 480) {
        return FALSE;
    }
    return TRUE;
}

/* 从 EDID 详细时序描述符解析显示器首选分辨率 */
STATIC EFI_STATUS ParseEdidPreferred(const UINT8 *Edid, UINT32 Size,
                                     UINT32 *PrefW, UINT32 *PrefH) {
    UINTN i;

    if (Edid == NULL || Size < 128 || PrefW == NULL || PrefH == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    for (i = 0; i < 4; i++) {
        UINTN  Off = 0x36 + i * 18;
        UINT16 PixClk;
        UINT32 W;
        UINT32 H;

        if (Off + 18 > Size) {
            break;
        }
        PixClk = (UINT16)Edid[Off] | ((UINT16)Edid[Off + 1] << 8);
        if (PixClk == 0) {
            continue;
        }
        W = Edid[Off + 2] | ((Edid[Off + 4] & 0xF0U) << 4);
        H = Edid[Off + 5] | ((Edid[Off + 7] & 0xF0U) << 4);
        if (W >= 640 && H >= 480) {
            *PrefW = W;
            *PrefH = H;
            return EFI_SUCCESS;
        }
    }
    return EFI_NOT_FOUND;
}

STATIC EFI_STATUS TryGetEdidPreferred(EFI_HANDLE ImageHandle, EFI_HANDLE GopHandle,
                                      UINT32 *PrefW, UINT32 *PrefH) {
    EFI_STATUS                Status;
    EFI_EDID_ACTIVE_PROTOCOL  *EdidActive = NULL;
    UINTN                     Count = 0;
    EFI_HANDLE                *Handles = NULL;
    UINTN                     i;

    Status = gBS->OpenProtocol(GopHandle, &gEfiEdidActiveProtocolGuid,
                               (VOID **)&EdidActive, ImageHandle, NULL,
                               EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(Status)) {
        Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiEdidActiveProtocolGuid,
                                         NULL, &Count, &Handles);
        if (EFI_ERROR(Status) || Count == 0) {
            return EFI_NOT_FOUND;
        }
        for (i = 0; i < Count; i++) {
            Status = gBS->OpenProtocol(Handles[i], &gEfiEdidActiveProtocolGuid,
                                       (VOID **)&EdidActive, ImageHandle, NULL,
                                       EFI_OPEN_PROTOCOL_GET_PROTOCOL);
            if (!EFI_ERROR(Status) && EdidActive != NULL &&
                EdidActive->SizeOfEdid >= 128 && EdidActive->Edid != NULL) {
                break;
            }
            EdidActive = NULL;
        }
        if (Handles != NULL) {
            gBS->FreePool(Handles);
        }
    }

    if (EdidActive == NULL || EdidActive->SizeOfEdid < 128 || EdidActive->Edid == NULL) {
        return EFI_NOT_FOUND;
    }
    return ParseEdidPreferred(EdidActive->Edid, EdidActive->SizeOfEdid, PrefW, PrefH);
}

STATIC UINTN ScoreModeQemu(UINT32 W, UINT32 H) {
    static const struct {
        UINT32 W;
        UINT32 H;
        UINTN  Score;
    } Preferred[] = {
        { 1600,  900, 3000 },
        { 1440,  900, 2900 },
        { 1680, 1050, 2800 },
        { 1280,  800, 2700 },
        { 1280,  720, 2650 },
        { 1280, 1024, 2600 },
        { 1920, 1080, 2000 },
        { 1600, 1200, 1900 },
        { 1024,  768, 1000 },
        {  800,  600,  500 },
    };
    UINTN p;

    for (p = 0; p < sizeof(Preferred) / sizeof(Preferred[0]); p++) {
        if (W == Preferred[p].W && H == Preferred[p].H) {
            return Preferred[p].Score;
        }
    }
    if (W <= 2560 && H <= 1600) {
        return (UINTN)(W * H) / 10000;
    }
    return 0;
}

STATIC UINTN ScoreModeNative(UINT32 W, UINT32 H, UINT32 TargetW, UINT32 TargetH,
                             BOOLEAN HasTarget) {
    UINTN Area = (UINTN)W * (UINTN)H;

    if (HasTarget) {
        if (W == TargetW && H == TargetH) {
            return 2000000000ULL + Area;
        }
        {
            INT64 Dw = (INT64)W - (INT64)TargetW;
            INT64 Dh = (INT64)H - (INT64)TargetH;
            if (Dw < 0) {
                Dw = -Dw;
            }
            if (Dh < 0) {
                Dh = -Dh;
            }
            return (UINTN)(1500000000ULL - (UINTN)(Dw + Dh) * 1000000ULL + Area);
        }
    }
    if (W <= 7680 && H <= 4320) {
        return Area;
    }
    return 0;
}


/* 从 FAT 小文本读 THEME.CFG 中的 mode=WxH（PR-D7，重启生效） */
STATIC BOOLEAN ParseAsciiModeLine(const CHAR8 *Line, UINT32 *OutW, UINT32 *OutH) {
    UINT32 W = 0;
    UINT32 H = 0;
    UINTN i = 0;

    if (Line == NULL || OutW == NULL || OutH == NULL) {
        return FALSE;
    }
    while (Line[i] == ' ' || Line[i] == '\t') {
        i++;
    }
    if (Line[i] == 'm' && Line[i + 1] == 'o' && Line[i + 2] == 'd' &&
        Line[i + 3] == 'e' && Line[i + 4] == '=') {
        i += 5;
    } else {
        return FALSE;
    }
    if (Line[i] == 'a' || Line[i] == 'A') {
        *OutW = 0;
        *OutH = 0;
        return TRUE;
    }
    while (Line[i] >= '0' && Line[i] <= '9') {
        W = W * 10 + (UINT32)(Line[i] - '0');
        i++;
        if (W > 10000) {
            return FALSE;
        }
    }
    if (Line[i] != 'x' && Line[i] != 'X') {
        return FALSE;
    }
    i++;
    while (Line[i] >= '0' && Line[i] <= '9') {
        H = H * 10 + (UINT32)(Line[i] - '0');
        i++;
        if (H > 10000) {
            return FALSE;
        }
    }
    if (W < 640 || H < 480) {
        return FALSE;
    }
    *OutW = W;
    *OutH = H;
    return TRUE;
}

STATIC BOOLEAN ParseThemeCfgMode(const CHAR8 *Buf, UINTN Size, UINT32 *OutW,
                                 UINT32 *OutH) {
    UINTN i;
    CHAR8 Line[64];
    UINTN L = 0;

    if (Buf == NULL || Size == 0) {
        return FALSE;
    }
    for (i = 0; i <= Size; i++) {
        CHAR8 C = (i < Size) ? Buf[i] : '\n';
        if (C == '\n' || C == '\r' || i == Size) {
            if (L > 0) {
                Line[L] = 0;
                if (ParseAsciiModeLine(Line, OutW, OutH)) {
                    return (*OutW >= 640 && *OutH >= 480);
                }
                L = 0;
            }
            continue;
        }
        if (L + 1 < sizeof(Line)) {
            Line[L++] = C;
        }
    }
    return FALSE;
}

STATIC EFI_STATUS ReadThemeCfgOnFs(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs,
                                   CHAR8 **OutBuf, UINTN *OutSize) {
    STATIC CHAR16 *Paths[] = {
        L"\\THEME.CFG",
        L"\\theme.cfg",
        L"\\rootfs\\THEME.CFG",
    };
    EFI_STATUS Status;
    EFI_FILE_PROTOCOL *Root = NULL;
    EFI_FILE_PROTOCOL *File = NULL;
    EFI_FILE_INFO *FileInfo = NULL;
    UINTN InfoSize;
    UINTN p;
    CHAR8 *Buf = NULL;
    UINTN ReadSize;

    *OutBuf = NULL;
    *OutSize = 0;

    Status = Fs->OpenVolume(Fs, &Root);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    for (p = 0; p < sizeof(Paths) / sizeof(Paths[0]); p++) {
        Status = Root->Open(Root, &File, Paths[p], EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR(Status)) {
            break;
        }
        File = NULL;
    }
    if (File == NULL) {
        Root->Close(Root);
        return EFI_NOT_FOUND;
    }

    InfoSize = sizeof(EFI_FILE_INFO) + 128;
    Status = gBS->AllocatePool(EfiLoaderData, InfoSize, (VOID **)&FileInfo);
    if (EFI_ERROR(Status)) {
        File->Close(File);
        Root->Close(Root);
        return Status;
    }

    Status = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, FileInfo);
    if (EFI_ERROR(Status) || FileInfo->FileSize == 0 || FileInfo->FileSize > 4096) {
        gBS->FreePool(FileInfo);
        File->Close(File);
        Root->Close(Root);
        return EFI_NOT_FOUND;
    }

    Status = gBS->AllocatePool(EfiLoaderData, (UINTN)FileInfo->FileSize + 1,
                               (VOID **)&Buf);
    if (EFI_ERROR(Status)) {
        gBS->FreePool(FileInfo);
        File->Close(File);
        Root->Close(Root);
        return Status;
    }

    ReadSize = (UINTN)FileInfo->FileSize;
    Status = File->Read(File, &ReadSize, Buf);
    gBS->FreePool(FileInfo);
    File->Close(File);
    Root->Close(Root);
    if (EFI_ERROR(Status)) {
        gBS->FreePool(Buf);
        return Status;
    }
    Buf[ReadSize] = 0;
    *OutBuf = Buf;
    *OutSize = ReadSize;
    return EFI_SUCCESS;
}

/* Settings 写在挂载的 TOYOS 系统盘；ESP/启动盘上的 THEME.CFG 可能是旧的 */
STATIC BOOLEAN FsHasToyOsId(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs) {
    EFI_STATUS Status;
    EFI_FILE_PROTOCOL *Root = NULL;
    EFI_FILE_PROTOCOL *File = NULL;

    if (Fs == NULL) {
        return FALSE;
    }
    Status = Fs->OpenVolume(Fs, &Root);
    if (EFI_ERROR(Status)) {
        return FALSE;
    }
    Status = Root->Open(Root, &File, L"\\TOYOS.ID", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
        Status = Root->Open(Root, &File, L"\\toyos.id", EFI_FILE_MODE_READ, 0);
    }
    if (!EFI_ERROR(Status) && File != NULL) {
        File->Close(File);
        Root->Close(Root);
        return TRUE;
    }
    Root->Close(Root);
    return FALSE;
}

STATIC BOOLEAN TryParseModeOnFs(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs,
                                UINT32 *OutW, UINT32 *OutH) {
    CHAR8 *Buf = NULL;
    UINTN Size = 0;

    if (EFI_ERROR(ReadThemeCfgOnFs(Fs, &Buf, &Size))) {
        return FALSE;
    }
    if (ParseThemeCfgMode(Buf, Size, OutW, OutH)) {
        gBS->FreePool(Buf);
        return TRUE;
    }
    gBS->FreePool(Buf);
    return FALSE;
}

STATIC BOOLEAN TryLoadDisplayPref(EFI_HANDLE ImageHandle, UINT32 *OutW,
                                  UINT32 *OutH) {
    EFI_STATUS Status;
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs = NULL;
    UINTN HandleCount = 0;
    EFI_HANDLE *Handles = NULL;
    UINTN i;
    UINTN Pass;

    Status = gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid,
                                 (VOID **)&LoadedImage);
    if (EFI_ERROR(Status)) {
        return FALSE;
    }

    Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                                     NULL, &HandleCount, &Handles);
    if (EFI_ERROR(Status)) {
        Handles = NULL;
        HandleCount = 0;
    }

    /* Pass0: 带 TOYOS.ID 的卷（Settings 写入处）；Pass1: 其它卷含启动盘 */
    for (Pass = 0; Pass < 2; Pass++) {
        for (i = 0; i < HandleCount; i++) {
            Status = gBS->HandleProtocol(Handles[i], &gEfiSimpleFileSystemProtocolGuid,
                                         (VOID **)&Fs);
            if (EFI_ERROR(Status)) {
                continue;
            }
            if (Pass == 0) {
                if (!FsHasToyOsId(Fs)) {
                    continue;
                }
            } else if (FsHasToyOsId(Fs)) {
                continue;
            }
            if (TryParseModeOnFs(Fs, OutW, OutH)) {
                gBS->FreePool(Handles);
                return TRUE;
            }
        }
    }

    if (Handles != NULL) {
        gBS->FreePool(Handles);
    }

    /* 单盘布局：启动卷即系统卷 */
    Status = gBS->HandleProtocol(LoadedImage->DeviceHandle,
                                 &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (!EFI_ERROR(Status) && TryParseModeOnFs(Fs, OutW, OutH)) {
        return TRUE;
    }
    return FALSE;
}

EFI_STATUS GetAndSetVideo(EFI_HANDLE ImageHandle, VIDEO_CONFIG *VideoConfig) {
    EFI_STATUS                            Status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL          *Gop = NULL;
    UINTN                                 HandleCount = 0;
    EFI_HANDLE                            *HandleBuffer = NULL;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *ModeInfo = NULL;
    UINTN                                 InfoSize = 0;
    UINTN                                 BestMode = 0;
    UINTN                                 BestScore = 0;
    UINT32                                BestW = 0;
    UINT32                                BestH = 0;
    BOOLEAN                               InVm = IsVirtualMachine();
    BOOLEAN                               HasEdidTarget = FALSE;
    UINT32                                EdidW = 0;
    UINT32                                EdidH = 0;
    BOOLEAN                               HasCfgTarget = FALSE;
    UINT32                                CfgW = 0;
    UINT32                                CfgH = 0;
    BOOLEAN                               CfgMatched = FALSE;

    Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiGraphicsOutputProtocolGuid,
                                     NULL, &HandleCount, &HandleBuffer);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    Status = gBS->OpenProtocol(HandleBuffer[0], &gEfiGraphicsOutputProtocolGuid,
                               (VOID **)&Gop, ImageHandle, NULL,
                               EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    if (TryLoadDisplayPref(ImageHandle, &CfgW, &CfgH)) {
        HasCfgTarget = TRUE;
        Print(L"ToyBoot: THEME.CFG mode %dx%d\n", CfgW, CfgH);
    }

    if (!InVm) {
        if (!EFI_ERROR(TryGetEdidPreferred(ImageHandle, HandleBuffer[0], &EdidW, &EdidH))) {
            HasEdidTarget = TRUE;
            Print(L"ToyBoot: monitor EDID preferred %dx%d\n", EdidW, EdidH);
        } else {
            Print(L"ToyBoot: EDID unavailable, using highest GOP mode\n");
        }
    } else {
        BootDbg(L"ToyBoot: virtual machine detected, using QEMU-friendly mode table\n");
    }

    BootDbg(L"Available video modes:\n");
    for (UINTN i = 0; i < Gop->Mode->MaxMode; i++) {
        Status = Gop->QueryMode(Gop, i, &InfoSize, &ModeInfo);
        if (EFI_ERROR(Status) || ModeInfo == NULL) {
            continue;
        }
        if (!IsModeUsable(ModeInfo)) {
            gBS->FreePool(ModeInfo);
            ModeInfo = NULL;
            continue;
        }

        {
            UINT32 W = ModeInfo->HorizontalResolution;
            UINT32 H = ModeInfo->VerticalResolution;
            UINTN  Score;

            if (HasCfgTarget && W == CfgW && H == CfgH) {
                /* Settings 偏好绝对优先（重启后生效） */
                Score = 5000000000ULL;
                CfgMatched = TRUE;
            } else if (InVm) {
                Score = ScoreModeQemu(W, H);
            } else {
                Score = ScoreModeNative(W, H, EdidW, EdidH, HasEdidTarget);
            }

            BootDbg(L"  Mode %d: %dx%d (Score: %d)\n", i, W, H, Score);

            if (Score > BestScore) {
                BestScore = Score;
                BestMode = i;
                BestW = W;
                BestH = H;
            }
        }

        gBS->FreePool(ModeInfo);
        ModeInfo = NULL;
        InfoSize = 0;
    }

    if (HasCfgTarget && !CfgMatched) {
        Print(L"ToyBoot: mode %dx%d not in GOP; keeping auto pick\n", CfgW, CfgH);
    }

    if (BestScore == 0) {
        Print(L"ToyBoot: no usable GOP mode found\n");
        return EFI_NOT_FOUND;
    }

    /* 已是目标分辨率则勿 SetMode：QEMU+GTK 下改分辨率常会整机再复位一次 */
    {
        UINT32 CurW = 0;
        UINT32 CurH = 0;

        if (Gop->Mode != NULL && Gop->Mode->Info != NULL) {
            CurW = Gop->Mode->Info->HorizontalResolution;
            CurH = Gop->Mode->Info->VerticalResolution;
        }

        if (CurW == BestW && CurH == BestH) {
            Print(L"ToyBoot: already %dx%d, skip SetMode\n", BestW, BestH);
        } else if (InVm) {
            /*
             * Guest reboot / QEMU Reset 不会重读宿主 run.sh 的 edid；若此处 SetMode
             * 改分辨率，GTK 跳变会再复位，固件又回到 edid 旧模式 → 无限重启。
             * 分辨率变更请退出 QEMU 后重新 ./run-split.sh（会按 rootfs THEME.CFG 设 edid）。
             */
            Print(L"ToyBoot: skip SetMode %dx%d -> %dx%d on VM (QEMU+GTK loop)\n",
                  CurW, CurH, BestW, BestH);
            if (HasCfgTarget && CfgMatched) {
                Print(L"ToyBoot: THEME.CFG %dx%d — quit QEMU and relaunch ./run-split.sh\n",
                      CfgW, CfgH);
            }
        } else {
            Status = Gop->SetMode(Gop, BestMode);
            if (EFI_ERROR(Status)) {
                Print(L"ToyBoot: SetMode(%d) failed: %r\n", BestMode, Status);
                return Status;
            }
        }
    }

    VideoConfig->FrameBufferBase = Gop->Mode->FrameBufferBase;
    VideoConfig->FrameBufferSize = Gop->Mode->FrameBufferSize;
    VideoConfig->HorizontalResolution = Gop->Mode->Info->HorizontalResolution;
    VideoConfig->VerticalResolution = Gop->Mode->Info->VerticalResolution;
    VideoConfig->PixelsPerScanLine = Gop->Mode->Info->PixelsPerScanLine;

    if (HasCfgTarget && CfgMatched) {
        Print(L"ToyBoot: display %dx%d (THEME.CFG)\n",
              VideoConfig->HorizontalResolution, VideoConfig->VerticalResolution);
    } else if (InVm) {
        Print(L"ToyBoot: display %dx%d (QEMU/VM)\n",
              VideoConfig->HorizontalResolution, VideoConfig->VerticalResolution);
    } else if (HasEdidTarget &&
               VideoConfig->HorizontalResolution == EdidW &&
               VideoConfig->VerticalResolution == EdidH) {
        Print(L"ToyBoot: display %dx%d (EDID native)\n", EdidW, EdidH);
    } else {
        Print(L"ToyBoot: display %dx%d (hardware best match)\n",
              VideoConfig->HorizontalResolution, VideoConfig->VerticalResolution);
    }

    BootDbg(L"Selected mode: %d, final %dx%d\n",
          BestMode, VideoConfig->HorizontalResolution, VideoConfig->VerticalResolution);

    if (HandleBuffer != NULL) {
        gBS->FreePool(HandleBuffer);
    }
    return EFI_SUCCESS;
}
// ============================================================
//  ReadKernelFile - 从 UEFI 文件系统读取内核（同卷优先，再扫其它 FAT）
// ============================================================

STATIC EFI_STATUS OpenKernelOnFs(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs,
                                 EFI_PHYSICAL_ADDRESS *OutBuffer) {
    STATIC CHAR16 *Paths[] = {
        L"\\Kernel.elf",
        L"\\KERNEL.ELF",
        L"\\EFI\\ToyOS\\Kernel.elf",
    };
    EFI_STATUS Status;
    EFI_FILE_PROTOCOL *Root = NULL;
    EFI_FILE_PROTOCOL *File = NULL;
    EFI_FILE_INFO *FileInfo = NULL;
    UINTN InfoSize;
    UINTN p;

    Status = Fs->OpenVolume(Fs, &Root);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    for (p = 0; p < sizeof(Paths) / sizeof(Paths[0]); p++) {
        Status = Root->Open(Root, &File, Paths[p], EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR(Status)) {
            break;
        }
        File = NULL;
    }
    if (File == NULL) {
        Root->Close(Root);
        return EFI_NOT_FOUND;
    }

    InfoSize = sizeof(EFI_FILE_INFO) + 128;
    Status = gBS->AllocatePool(EfiLoaderData, InfoSize, (VOID **)&FileInfo);
    if (EFI_ERROR(Status)) {
        File->Close(File);
        Root->Close(Root);
        return Status;
    }

    Status = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, FileInfo);
    if (EFI_ERROR(Status)) {
        gBS->FreePool(FileInfo);
        File->Close(File);
        Root->Close(Root);
        return Status;
    }

    {
        UINTN FilePageSize = (FileInfo->FileSize >> 12) + 1;
        Status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, FilePageSize, OutBuffer);
        if (EFI_ERROR(Status)) {
            gBS->FreePool(FileInfo);
            File->Close(File);
            Root->Close(Root);
            return Status;
        }

        {
            UINTN ReadSize = FileInfo->FileSize;
            Status = File->Read(File, &ReadSize, (VOID *)(UINTN)*OutBuffer);
        }
    }

    gBS->FreePool(FileInfo);
    File->Close(File);
    Root->Close(Root);
    return Status;
}

EFI_STATUS ReadKernelFile(EFI_HANDLE ImageHandle, EFI_PHYSICAL_ADDRESS *OutBuffer) {
    EFI_STATUS Status;
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs = NULL;
    UINTN HandleCount = 0;
    EFI_HANDLE *Handles = NULL;
    UINTN i;
    UINTN Pass;

    Status = gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid,
                                 (VOID **)&LoadedImage);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                                     NULL, &HandleCount, &Handles);
    if (EFI_ERROR(Status)) {
        Handles = NULL;
        HandleCount = 0;
    }

    /*
     * 双盘布局：必须先读带 TOYOS.ID 的系统盘（disk1/rootfs），避免启动盘旧
     * Kernel.elf 抢先加载。Pass0=TOYOS；Pass1=其它非启动卷；Pass2=启动卷兜底。
     */
    for (Pass = 0; Pass < 3; Pass++) {
        for (i = 0; i < HandleCount; i++) {
            BOOLEAN IsBoot = (Handles[i] == LoadedImage->DeviceHandle);
            BOOLEAN IsToyOs;

            Status = gBS->HandleProtocol(Handles[i], &gEfiSimpleFileSystemProtocolGuid,
                                         (VOID **)&Fs);
            if (EFI_ERROR(Status)) {
                continue;
            }
            IsToyOs = FsHasToyOsId(Fs);
            if (Pass == 0) {
                if (!IsToyOs) {
                    continue;
                }
            } else if (Pass == 1) {
                /* 非启动盘；含 TOYOS.ID 误判失败时的 rootfs 兜底 */
                if (IsBoot) {
                    continue;
                }
            } else {
                if (!IsBoot) {
                    continue;
                }
            }

            Status = OpenKernelOnFs(Fs, OutBuffer);
            if (!EFI_ERROR(Status)) {
                if (Pass == 0) {
                    Print(L"ToyBoot: Kernel.elf from TOYOS volume\n");
                } else if (Pass == 1) {
                    Print(L"ToyBoot: Kernel.elf from secondary volume\n");
                } else {
                    Print(L"ToyBoot: Kernel.elf from boot volume\n");
                }
                if (Handles != NULL) {
                    gBS->FreePool(Handles);
                }
                return EFI_SUCCESS;
            }
        }
    }

    if (Handles != NULL) {
        gBS->FreePool(Handles);
    }
    Print(L"ToyBoot: Kernel.elf not found on any volume\n");
    return EFI_NOT_FOUND;
}

// ============================================================
//  CheckAndLoadKernel - 检查 ELF 格式并加载段
// ============================================================

EFI_STATUS CheckAndLoadKernel(EFI_PHYSICAL_ADDRESS ElfBase, EFI_PHYSICAL_ADDRESS *EntryPoint) {
    if (*(UINT32*)ElfBase != 0x464C457F || *(UINT8*)(ElfBase + 4) != 2) {
        return EFI_UNSUPPORTED;
    }

    ELF_HEADER_64 *Hdr = (ELF_HEADER_64*)ElfBase;
    PROGRAM_HEADER_64 *PHead = (PROGRAM_HEADER_64*)(ElfBase + Hdr->Phoff);

    EFI_PHYSICAL_ADDRESS Low = 0xFFFFFFFFFFFFFFFF;
    EFI_PHYSICAL_ADDRESS High = 0;

    for (UINTN i = 0; i < Hdr->PHeadCount; i++) {
        if (PHead[i].Type == PT_LOAD) {
            if (Low > PHead[i].PAddress) Low = PHead[i].PAddress;
            if (High < PHead[i].PAddress + PHead[i].SizeInMemory)
                High = PHead[i].PAddress + PHead[i].SizeInMemory;
        }
    }

    // Kernel.elf 链接在 0x100000，且以 -fno-pie 编译，必须按 PhysAddr 固定加载，
    // 不能 AllocateAnyPages 再重定位，否则跳转后立刻挂死。
    UINTN PageCount = ((High - Low) >> 12) + 1;
    EFI_PHYSICAL_ADDRESS LoadBase = Low;
    EFI_STATUS Status = gBS->AllocatePages(AllocateAddress, EfiLoaderCode, PageCount, &LoadBase);
    if (EFI_ERROR(Status)) {
        Print(L"AllocatePages(0x%lx, %lu pages) failed: %r\n", Low, PageCount, Status);
        return Status;
    }

    SetMem((VOID*)LoadBase, PageCount * 4096, 0);

    for (UINTN i = 0; i < Hdr->PHeadCount; i++) {
        if (PHead[i].Type == PT_LOAD) {
            CopyMem((VOID*)(UINTN)PHead[i].PAddress,
                    (VOID*)(ElfBase + PHead[i].Offset), PHead[i].SizeInFile);
            if (PHead[i].SizeInMemory > PHead[i].SizeInFile) {
                SetMem((VOID*)(UINTN)(PHead[i].PAddress + PHead[i].SizeInFile),
                       PHead[i].SizeInMemory - PHead[i].SizeInFile, 0);
            }
        }
    }

    *EntryPoint = Hdr->Entry;
    BootDbg(L"Kernel loaded at 0x%lx, entry 0x%lx\n", LoadBase, *EntryPoint);
    return EFI_SUCCESS;
}

// ============================================================
//  GetRsdpAddress - 获取 ACPI RSDP 表地址
// ============================================================

EFI_STATUS GetRsdpAddress(EFI_PHYSICAL_ADDRESS *RsdpAddress) {
    EFI_STATUS Status;

    Status = EfiGetSystemConfigurationTable(&gEfiAcpiTableGuid, (VOID**)RsdpAddress);
    if (!EFI_ERROR(Status)) return EFI_SUCCESS;

    Status = EfiGetSystemConfigurationTable(&gEfiAcpi10TableGuid, (VOID**)RsdpAddress);
    return Status;
}

// ============================================================
//  JumpToKernel - 获取内存映射，退出 Boot Services，跳转
// ============================================================

EFI_STATUS JumpToKernel(EFI_HANDLE ImageHandle, BOOT_CONFIG *BootConfig) {
    EFI_STATUS Status;
    MEMORY_MAP MemoryMap = {NULL, 0, 0, 0, 0};
    UINTN MapKey = 0;

    // 1. 第一次调用：获取所需大小
    Status = gBS->GetMemoryMap(&MemoryMap.MapSize, NULL, &MapKey,
                               &MemoryMap.DescriptorSize, &MemoryMap.DescriptorVersion);
    if (Status != EFI_BUFFER_TOO_SMALL) return Status;

    // 2. 分配足够的缓冲区（多加几个描述符的空间，防止在分配期间内存映射变化）
    MemoryMap.MapSize += MemoryMap.DescriptorSize * 8;
    Status = gBS->AllocatePool(EfiLoaderData, MemoryMap.MapSize, &MemoryMap.Buffer);
    if (EFI_ERROR(Status)) return Status;

    // 3. 第二次调用：获取实际内存映射
    Status = gBS->GetMemoryMap(&MemoryMap.MapSize, (EFI_MEMORY_DESCRIPTOR*)MemoryMap.Buffer,
                               &MapKey, &MemoryMap.DescriptorSize,
                               &MemoryMap.DescriptorVersion);
    if (EFI_ERROR(Status)) return Status;

    // 4. 退出 Boot Services
    Status = gBS->ExitBootServices(ImageHandle, MapKey);
    if (EFI_ERROR(Status)) {
        Print(L"ExitBootServices failed: %r\n", Status);
        return Status;
    }

    BootConfig->MemoryMap = MemoryMap;

    // 5. 跳转到内核
    UINT64 (*KernelEntry)(BOOT_CONFIG*) = (UINT64 (*)(BOOT_CONFIG*))BootConfig->KernelEntry;
    return (EFI_STATUS)KernelEntry(BootConfig);
}

EFI_STATUS GetXhciBaseAddress(UINT64 *XhciBase) {
    EFI_STATUS Status;
    UINTN HandleCount = 0;
    EFI_HANDLE *HandleBuffer = NULL;

    BootDbg(L"[Boot] Looking for XHCI...\n");

    Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiPciIoProtocolGuid,
                                     NULL, &HandleCount, &HandleBuffer);
    if (EFI_ERROR(Status)) {
        BootDbg(L"[Boot] LocateHandleBuffer failed: %r\n", Status);
        return Status;
    }

    BootDbg(L"[Boot] Found %d PCI devices\n", HandleCount);

    for (UINTN i = 0; i < HandleCount; i++) {
        EFI_PCI_IO_PROTOCOL *PciIo;
        Status = gBS->OpenProtocol(HandleBuffer[i], &gEfiPciIoProtocolGuid,
                                   (VOID**)&PciIo, NULL, NULL,
                                   EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(Status)) continue;

        UINT32 VendorID;
        UINT32 DeviceID;
        UINT32 ClassCode;

        PciIo->Pci.Read(PciIo, EfiPciIoWidthUint32, 0x00, 1, &VendorID);
        PciIo->Pci.Read(PciIo, EfiPciIoWidthUint32, 0x02, 1, &DeviceID);
        PciIo->Pci.Read(PciIo, EfiPciIoWidthUint32, 0x08, 1, &ClassCode);

        UINT8 Class = (ClassCode >> 24) & 0xFF;
        UINT8 Subclass = (ClassCode >> 16) & 0xFF;
        UINT8 ProgIF = (ClassCode >> 8) & 0xFF;

#if TOY_BOOT_DEBUG
        BootDbg(L"[Boot] Device %d: VID=0x%04x, DID=0x%04x, Class=0x%02x, Sub=0x%02x, ProgIF=0x%02x\n",
              i, VendorID & 0xFFFF, (DeviceID >> 16) & 0xFFFF, Class, Subclass, ProgIF);
#else
        (void)VendorID;
        (void)DeviceID;
        (void)ProgIF;
#endif

        if (Class == 0x0C && Subclass == 0x03) {
            UINT32 Bar0;
            PciIo->Pci.Read(PciIo, EfiPciIoWidthUint32, 0x10, 1, &Bar0);

            UINT64 Address = Bar0 & 0xFFFFFFF0;
            if ((Bar0 & 0x6) == 0x4) {
                UINT32 Bar1;
                PciIo->Pci.Read(PciIo, EfiPciIoWidthUint32, 0x14, 1, &Bar1);
                Address |= ((UINT64)Bar1 << 32);
            }

            BootDbg(L"[Boot] USB Controller found! BAR0=0x%08x, Address=0x%016lx\n", Bar0, Address);
            *XhciBase = Address;
            return EFI_SUCCESS;
        }
    }

    BootDbg(L"[Boot] No USB Controller found!\n");
    return EFI_NOT_FOUND;
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS Status;
    BOOT_CONFIG BootConfig = {0};
    EFI_PHYSICAL_ADDRESS ElfBuffer = 0;

    Status = GetAndSetVideo(ImageHandle, &BootConfig.VideoConfig);
    if (EFI_ERROR(Status)) return Status;

    Status = ReadKernelFile(ImageHandle, &ElfBuffer);
    if (EFI_ERROR(Status)) return Status;

    Status = CheckAndLoadKernel(ElfBuffer, &BootConfig.KernelEntry);
    if (EFI_ERROR(Status)) return Status;

    Status = GetRsdpAddress(&BootConfig.RsdpAddress);
    if (EFI_ERROR(Status)) return Status;

    BootConfig.SystemTable = SystemTable;

    BootDbg(L"[Boot] Calling GetXhciBaseAddress...\n");
    if (!EFI_ERROR(GetXhciBaseAddress(&BootConfig.XhciBaseAddress))) {
        BootDbg(L"[Boot] XHCI Base: 0x%016lx\n", BootConfig.XhciBaseAddress);
    } else {
        BootConfig.XhciBaseAddress = 0;
        BootDbg(L"[Boot] XHCI not found, setting to 0\n");
    }
    BootDbg(L"[Boot] BOOT_CONFIG.XhciBaseAddress = 0x%016lx\n", BootConfig.XhciBaseAddress);
    BootDbg(L"[Boot] Jumping to kernel entry 0x%lx\n", BootConfig.KernelEntry);

    return JumpToKernel(ImageHandle, &BootConfig);
}