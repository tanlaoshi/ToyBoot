#include <Uefi.h>
#include <Guid/Acpi.h>
#include <Guid/FileInfo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>

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
} BOOT_CONFIG;

// ============================================================
//  GetAndSetVideo - 获取并设置视频模式
// ============================================================

// EFI_STATUS GetAndSetVideo(EFI_HANDLE ImageHandle, VIDEO_CONFIG *VideoConfig) {
//     EFI_STATUS Status;
//     EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
//     UINTN HandleCount = 0;
//     EFI_HANDLE *HandleBuffer = NULL;

//     Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiGraphicsOutputProtocolGuid,
//                                      NULL, &HandleCount, &HandleBuffer);
//     if (EFI_ERROR(Status)) return Status;

//     Status = gBS->OpenProtocol(HandleBuffer[0], &gEfiGraphicsOutputProtocolGuid,
//                                (VOID**)&Gop, ImageHandle, NULL,
//                                EFI_OPEN_PROTOCOL_GET_PROTOCOL);
//     if (EFI_ERROR(Status)) return Status;

//     EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *ModeInfo = NULL;
//     UINTN InfoSize = sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
//     UINTN BestMode = 0, BestPixels = 0;

//     for (UINTN i = 0; i < Gop->Mode->MaxMode; i++) {
//         Gop->QueryMode(Gop, i, &InfoSize, &ModeInfo);
//         UINTN Pixels = ModeInfo->HorizontalResolution * ModeInfo->VerticalResolution;
//         if (Pixels > BestPixels) {
//             BestPixels = Pixels;
//             BestMode = i;
//         }
//     }

//     Gop->SetMode(Gop, BestMode);

//     VideoConfig->FrameBufferBase = Gop->Mode->FrameBufferBase;
//     VideoConfig->FrameBufferSize = Gop->Mode->FrameBufferSize;
//     VideoConfig->HorizontalResolution = Gop->Mode->Info->HorizontalResolution;
//     VideoConfig->VerticalResolution = Gop->Mode->Info->VerticalResolution;
//     VideoConfig->PixelsPerScanLine = Gop->Mode->Info->PixelsPerScanLine;

//     return EFI_SUCCESS;
// }
EFI_STATUS GetAndSetVideo(EFI_HANDLE ImageHandle, VIDEO_CONFIG *VideoConfig) {
    EFI_STATUS Status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
    UINTN HandleCount = 0;
    EFI_HANDLE *HandleBuffer = NULL;

    Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiGraphicsOutputProtocolGuid,
                                     NULL, &HandleCount, &HandleBuffer);
    if (EFI_ERROR(Status)) return Status;

    Status = gBS->OpenProtocol(HandleBuffer[0], &gEfiGraphicsOutputProtocolGuid,
                               (VOID**)&Gop, ImageHandle, NULL,
                               EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(Status)) return Status;

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *ModeInfo = NULL;
    UINTN InfoSize = sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
    UINTN BestMode = 0;
    UINTN BestScore = 0;
    
    // 目标分辨率：优先 1024x768，其次 800x600
    UINT32 TargetWidth = 1024;
    UINT32 TargetHeight = 768;

    Print(L"Available video modes:\n");
    for (UINTN i = 0; i < Gop->Mode->MaxMode; i++) {
        Gop->QueryMode(Gop, i, &InfoSize, &ModeInfo);
        UINTN Score = 0;
        
        // 计算与目标分辨率的匹配度
        if (ModeInfo->HorizontalResolution == TargetWidth && 
            ModeInfo->VerticalResolution == TargetHeight) {
            Score = 1000;  // 完美匹配
        } else if (ModeInfo->HorizontalResolution <= TargetWidth && 
                   ModeInfo->VerticalResolution <= TargetHeight) {
            // 小于目标分辨率，按接近程度评分
            Score = (ModeInfo->HorizontalResolution * ModeInfo->VerticalResolution) / 10000;
        }
        
        Print(L"  Mode %d: %dx%d (Score: %d)\n", i, 
              ModeInfo->HorizontalResolution, ModeInfo->VerticalResolution, Score);
        
        if (Score > BestScore) {
            BestScore = Score;
            BestMode = i;
        }
    }

    Print(L"Selected mode: %d\n", BestMode);
    Gop->SetMode(Gop, BestMode);

    VideoConfig->FrameBufferBase = Gop->Mode->FrameBufferBase;
    VideoConfig->FrameBufferSize = Gop->Mode->FrameBufferSize;
    VideoConfig->HorizontalResolution = Gop->Mode->Info->HorizontalResolution;
    VideoConfig->VerticalResolution = Gop->Mode->Info->VerticalResolution;
    VideoConfig->PixelsPerScanLine = Gop->Mode->Info->PixelsPerScanLine;

    Print(L"Final resolution: %dx%d\n", 
          VideoConfig->HorizontalResolution, VideoConfig->VerticalResolution);

    return EFI_SUCCESS;
}
// ============================================================
//  ReadKernelFile - 从 UEFI 文件系统读取内核文件
// ============================================================

EFI_STATUS ReadKernelFile(EFI_HANDLE ImageHandle, EFI_PHYSICAL_ADDRESS *OutBuffer) {
    EFI_STATUS Status;
    EFI_FILE_PROTOCOL *File = NULL, *Root = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs = NULL;
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage = NULL;
    EFI_FILE_INFO *FileInfo = NULL;
    UINTN InfoSize = sizeof(EFI_FILE_INFO) + 128;

    Status = gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&LoadedImage);
    if (EFI_ERROR(Status)) return Status;

    Status = gBS->HandleProtocol(LoadedImage->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID**)&Fs);
    if (EFI_ERROR(Status)) return Status;

    Status = Fs->OpenVolume(Fs, &Root);
    if (EFI_ERROR(Status)) return Status;

    Status = Root->Open(Root, &File, L"\\Kernel.elf", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) return Status;

    Status = gBS->AllocatePool(EfiLoaderData, InfoSize, (VOID**)&FileInfo);
    if (EFI_ERROR(Status)) return Status;

    Status = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, FileInfo);
    if (EFI_ERROR(Status)) { gBS->FreePool(FileInfo); return Status; }

    UINTN FilePageSize = (FileInfo->FileSize >> 12) + 1;
    Status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, FilePageSize, OutBuffer);
    if (EFI_ERROR(Status)) { gBS->FreePool(FileInfo); return Status; }

    UINTN ReadSize = FileInfo->FileSize;
    Status = File->Read(File, &ReadSize, (VOID*)*OutBuffer);
    gBS->FreePool(FileInfo);
    File->Close(File);

    return Status;
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

    UINTN PageCount = ((High - Low) >> 12) + 1;
    EFI_PHYSICAL_ADDRESS RelocBase;
    EFI_STATUS Status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderCode, PageCount, &RelocBase);
    if (EFI_ERROR(Status)) return Status;

    // 使用有符号偏移，确保地址计算正确
    INT64 Offset = (INT64)RelocBase - (INT64)Low;
    SetMem((VOID*)RelocBase, PageCount * 4096, 0);

    for (UINTN i = 0; i < Hdr->PHeadCount; i++) {
        if (PHead[i].Type == PT_LOAD) {
            CopyMem((VOID*)((INT64)PHead[i].VAddress + Offset),
                    (VOID*)(ElfBase + PHead[i].Offset), PHead[i].SizeInFile);
            if (PHead[i].SizeInMemory > PHead[i].SizeInFile)
                SetMem((VOID*)((INT64)PHead[i].VAddress + Offset + PHead[i].SizeInFile),
                       PHead[i].SizeInMemory - PHead[i].SizeInFile, 0);
        }
    }

    *EntryPoint = (EFI_PHYSICAL_ADDRESS)((INT64)Hdr->Entry + Offset);
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

// ============================================================
//  入口
// ============================================================

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

    return JumpToKernel(ImageHandle, &BootConfig);
}