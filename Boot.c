#include <Uefi.h>
#include <Guid/Acpi.h>
#include <Guid/FileInfo.h>
#include <Protocol/PciIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>

#include "BootPrivate.h"

/*
 * PR-S-boot-1：Video 已拆到 BootVideo*.c；BootDbg / 类型别名见 BootPrivate.h。
 */

#define PT_LOAD 1
#define EM_X86_64 0x3E

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

// ============================================================
//  ReadKernelFile - 从 UEFI 文件系统读取内核（同卷优先，再扫其它 FAT）
// ============================================================

STATIC EFI_STATUS OpenKernelOnFs(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs,
                                 EFI_PHYSICAL_ADDRESS *OutBuffer, UINTN *OutSize) {
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

    if (OutSize != NULL) {
        *OutSize = 0;
    }

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
        UINTN FilePageSize = ((UINTN)FileInfo->FileSize >> 12) + 1;
        Status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, FilePageSize, OutBuffer);
        if (EFI_ERROR(Status)) {
            gBS->FreePool(FileInfo);
            File->Close(File);
            Root->Close(Root);
            return Status;
        }

        {
            UINTN ReadSize = (UINTN)FileInfo->FileSize;
            Status = File->Read(File, &ReadSize, (VOID *)(UINTN)*OutBuffer);
            if (!EFI_ERROR(Status) && OutSize != NULL) {
                *OutSize = ReadSize;
            }
        }
    }

    gBS->FreePool(FileInfo);
    File->Close(File);
    Root->Close(Root);
    return Status;
}

STATIC EFI_STATUS ReadKernelFile(EFI_HANDLE ImageHandle, EFI_PHYSICAL_ADDRESS *OutBuffer,
                                 UINTN *OutSize) {
    EFI_STATUS Status;
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs = NULL;
    UINTN HandleCount = 0;
    EFI_HANDLE *Handles = NULL;
    UINTN i;
    UINTN Pass;

    BootSerialPrintf("Boot: ReadKernelFile\n");

    if (OutSize != NULL) {
        *OutSize = 0;
    }

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
                if (IsBoot) {
                    continue;
                }
            } else {
                if (!IsBoot) {
                    continue;
                }
            }

            Status = OpenKernelOnFs(Fs, OutBuffer, OutSize);
            if (!EFI_ERROR(Status)) {
                if (Pass == 0) {
                    BootDbg("ToyBoot: Kernel.elf From TOYOS Volume\n");
                } else if (Pass == 1) {
                    BootDbg("ToyBoot: Kernel.elf From Secondary Volume\n");
                } else {
                    BootDbg("ToyBoot: Kernel.elf From Boot Volume\n");
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
    BootSerialPrintf("ToyBoot: Kernel.elf Not Found On Any Volume\n");
    return EFI_NOT_FOUND;
}

// ============================================================
//  CheckAndLoadKernel - 检查 ELF 格式并加载段
// ============================================================

STATIC EFI_STATUS CheckAndLoadKernel(EFI_PHYSICAL_ADDRESS ElfBase, UINTN FileSize,
                                      EFI_PHYSICAL_ADDRESS *EntryPoint) {
    ELF_HEADER_64 *Hdr;
    PROGRAM_HEADER_64 *PHead;
    EFI_PHYSICAL_ADDRESS Low = 0xFFFFFFFFFFFFFFFFULL;
    EFI_PHYSICAL_ADDRESS High = 0;
    UINTN i;
    UINTN PageCount;
    EFI_PHYSICAL_ADDRESS LoadBase;
    EFI_STATUS Status;
    UINT64 PhEnd;

    BootSerialPrintf("Boot: CheckAndLoadKernel Size=%lu\n", (UINT64)FileSize);

    if (FileSize < sizeof(ELF_HEADER_64)) {
        return EFI_UNSUPPORTED;
    }
    if (*(UINT32 *)(UINTN)ElfBase != 0x464C457FU || *(UINT8 *)(UINTN)(ElfBase + 4) != 2) {
        return EFI_UNSUPPORTED;
    }

    Hdr = (ELF_HEADER_64 *)(UINTN)ElfBase;
    if (Hdr->Machine != EM_X86_64) {
        BootSerialPrintf("ToyBoot: Bad ELF Machine 0x%x\n", Hdr->Machine);
        return EFI_UNSUPPORTED;
    }
    if (Hdr->PHeadSize < sizeof(PROGRAM_HEADER_64) || Hdr->PHeadCount == 0) {
        return EFI_UNSUPPORTED;
    }
    if (Hdr->Phoff >= FileSize) {
        return EFI_UNSUPPORTED;
    }
    PhEnd = Hdr->Phoff + (UINT64)Hdr->PHeadCount * (UINT64)Hdr->PHeadSize;
    if (PhEnd > FileSize || PhEnd < Hdr->Phoff) {
        return EFI_UNSUPPORTED;
    }

    PHead = (PROGRAM_HEADER_64 *)(UINTN)(ElfBase + Hdr->Phoff);
    for (i = 0; i < Hdr->PHeadCount; i++) {
        PROGRAM_HEADER_64 *Ph = (PROGRAM_HEADER_64 *)((UINT8 *)PHead + i * Hdr->PHeadSize);
        UINT64 SegEnd;

        if (Ph->Type != PT_LOAD) {
            continue;
        }
        if (Ph->Offset >= FileSize || Ph->SizeInFile > FileSize - Ph->Offset) {
            BootSerialPrintf("ToyBoot: PT_LOAD Out Of File\n");
            return EFI_UNSUPPORTED;
        }
        SegEnd = Ph->PAddress + Ph->SizeInMemory;
        if (SegEnd < Ph->PAddress) {
            BootSerialPrintf("ToyBoot: PT_LOAD Address Wrap\n");
            return EFI_UNSUPPORTED;
        }
        if (Low > Ph->PAddress) {
            Low = Ph->PAddress;
        }
        if (High < SegEnd) {
            High = SegEnd;
        }
    }
    if (High <= Low) {
        return EFI_UNSUPPORTED;
    }

    /* Kernel.elf @0x100000，-fno-pie：必须按 PhysAddr 固定加载 */
    {
        UINT64 Span = High - Low;

        /* ceil(Span/4096)；防 Span 过大导致 PageCount 回绕 */
        if (Span > (~(UINT64)0 - 0xFFFULL)) {
            BootSerialPrintf("ToyBoot: Page Count Wrap\n");
            return EFI_UNSUPPORTED;
        }
        PageCount = (UINTN)((Span + 0xFFFULL) >> 12);
        if (PageCount == 0 || (UINT64)PageCount != ((Span + 0xFFFULL) >> 12)) {
            BootSerialPrintf("ToyBoot: Page Count Wrap\n");
            return EFI_UNSUPPORTED;
        }
    }
    LoadBase = Low;
    Status = gBS->AllocatePages(AllocateAddress, EfiLoaderCode, PageCount, &LoadBase);
    if (EFI_ERROR(Status)) {
        BootSerialPrintf("AllocatePages(0x%lx, %lu Pages) Failed: %r\n", Low, PageCount, Status);
        return Status;
    }

    SetMem((VOID *)(UINTN)LoadBase, PageCount * 4096, 0);

    for (i = 0; i < Hdr->PHeadCount; i++) {
        PROGRAM_HEADER_64 *Ph = (PROGRAM_HEADER_64 *)((UINT8 *)PHead + i * Hdr->PHeadSize);
        if (Ph->Type != PT_LOAD) {
            continue;
        }
        CopyMem((VOID *)(UINTN)Ph->PAddress,
                (VOID *)(UINTN)(ElfBase + Ph->Offset), (UINTN)Ph->SizeInFile);
        if (Ph->SizeInMemory > Ph->SizeInFile) {
            SetMem((VOID *)(UINTN)(Ph->PAddress + Ph->SizeInFile),
                   (UINTN)(Ph->SizeInMemory - Ph->SizeInFile), 0);
        }
    }

    *EntryPoint = Hdr->Entry;
    BootDbg("Kernel Loaded At 0x%lx, Entry 0x%lx\n", LoadBase, *EntryPoint);
    return EFI_SUCCESS;
}

// ============================================================
//  GetRsdpAddress - 获取 ACPI RSDP 表地址
// ============================================================

STATIC EFI_STATUS GetRsdpAddress(EFI_PHYSICAL_ADDRESS *RsdpAddress) {
    EFI_STATUS Status;

    Status = EfiGetSystemConfigurationTable(&gEfiAcpiTableGuid, (VOID**)RsdpAddress);
    if (!EFI_ERROR(Status)) return EFI_SUCCESS;

    Status = EfiGetSystemConfigurationTable(&gEfiAcpi10TableGuid, (VOID**)RsdpAddress);
    return Status;
}

// ============================================================
//  JumpToKernel - 获取内存映射，退出 Boot Services，跳转
// ============================================================

STATIC EFI_STATUS JumpToKernel(EFI_HANDLE ImageHandle, BOOT_CONFIG *BootConfig) {
    EFI_STATUS Status;
    MEMORY_MAP MemoryMap = {NULL, 0, 0, 0, 0};
    UINTN MapKey = 0;
    UINTN Tries;
    UINTN Needed;

    BootSerialPrintf("Boot: JumpToKernel Entry=0x%lx\n",
                     (UINT64)BootConfig->KernelEntry);

    Status = gBS->GetMemoryMap(&MemoryMap.MapSize, NULL, &MapKey,
                               &MemoryMap.DescriptorSize, &MemoryMap.DescriptorVersion);
    if (Status != EFI_BUFFER_TOO_SMALL) {
        return Status;
    }

    MemoryMap.MapSize += MemoryMap.DescriptorSize * 16;
    Status = gBS->AllocatePool(EfiLoaderData, MemoryMap.MapSize, &MemoryMap.Buffer);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    /* ExitBootServices 常因 map 变更失败；重取 map 后重试 */
    for (Tries = 0; Tries < 8; Tries++) {
        Needed = MemoryMap.MapSize;
        Status = gBS->GetMemoryMap(&Needed, (EFI_MEMORY_DESCRIPTOR *)MemoryMap.Buffer,
                                   &MapKey, &MemoryMap.DescriptorSize,
                                   &MemoryMap.DescriptorVersion);
        if (Status == EFI_BUFFER_TOO_SMALL) {
            gBS->FreePool(MemoryMap.Buffer);
            MemoryMap.MapSize = Needed + MemoryMap.DescriptorSize * 16;
            Status = gBS->AllocatePool(EfiLoaderData, MemoryMap.MapSize, &MemoryMap.Buffer);
            if (EFI_ERROR(Status)) {
                return Status;
            }
            continue;
        }
        if (EFI_ERROR(Status)) {
            gBS->FreePool(MemoryMap.Buffer);
            return Status;
        }
        MemoryMap.MapSize = Needed;

        Status = gBS->ExitBootServices(ImageHandle, MapKey);
        if (!EFI_ERROR(Status)) {
            BootConfig->MemoryMap = MemoryMap;
            {
                UINT64 Entry = BootConfig->KernelEntry;
                BOOT_CONFIG *Cfg = BootConfig;

                /*
                 * 不再经 C 函数指针调用：MSVC/UEFI ABI 把首参放 RCX，
                 * 而 freestanding 内核按 SysV 读 RDI。两边都写入后 jmp。
                 */
                __asm__ volatile(
                    "cli\n\t"
                    "mov %[cfg], %%rdi\n\t"
                    "mov %[cfg], %%rcx\n\t"
                    "jmp *%[entry]"
                    :
                    : [cfg] "r"(Cfg), [entry] "r"(Entry)
                    : "rdi", "rcx", "memory", "cc");
                __builtin_unreachable();
            }
        }
        /* 失败则下一轮重新 GetMemoryMap（仍可调用 Boot Services） */
    }

    BootSerialPrintf("ExitBootServices Failed After Retries: %r\n", Status);
    gBS->FreePool(MemoryMap.Buffer);
    return Status;
}

STATIC EFI_STATUS GetXhciBaseAddress(UINT64 *XhciBase) {
    EFI_STATUS Status;
    UINTN HandleCount = 0;
    EFI_HANDLE *HandleBuffer = NULL;
    UINTN i;

    BootDbg("[Boot] Looking for XHCI...\n");

    Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiPciIoProtocolGuid,
                                     NULL, &HandleCount, &HandleBuffer);
    if (EFI_ERROR(Status)) {
        BootDbg("[Boot] LocateHandleBuffer failed: %r\n", Status);
        return Status;
    }

    BootDbg("[Boot] Found %d PCI devices\n", HandleCount);

    for (i = 0; i < HandleCount; i++) {
        EFI_PCI_IO_PROTOCOL *PciIo;
        UINT32 VendorID;
        UINT32 DeviceID;
        UINT32 ClassCode;
        UINT8 Class;
        UINT8 Subclass;
        UINT8 ProgIF;

        Status = gBS->OpenProtocol(HandleBuffer[i], &gEfiPciIoProtocolGuid,
                                   (VOID **)&PciIo, NULL, NULL,
                                   EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(Status)) {
            continue;
        }

        PciIo->Pci.Read(PciIo, EfiPciIoWidthUint32, 0x00, 1, &VendorID);
        PciIo->Pci.Read(PciIo, EfiPciIoWidthUint32, 0x02, 1, &DeviceID);
        PciIo->Pci.Read(PciIo, EfiPciIoWidthUint32, 0x08, 1, &ClassCode);

        Class = (UINT8)((ClassCode >> 24) & 0xFF);
        Subclass = (UINT8)((ClassCode >> 16) & 0xFF);
        ProgIF = (UINT8)((ClassCode >> 8) & 0xFF);

#if TOY_BOOT_DEBUG
        BootDbg("[Boot] Device %d: VID=0x%04x, DID=0x%04x, Class=0x%02x, Sub=0x%02x, ProgIF=0x%02x\n",
              i, VendorID & 0xFFFF, (DeviceID >> 16) & 0xFFFF, Class, Subclass, ProgIF);
#else
        (void)VendorID;
        (void)DeviceID;
#endif

        /* XHCI = USB serial bus class, xHCI ProgIF 0x30 */
        if (Class == 0x0C && Subclass == 0x03 && ProgIF == 0x30) {
            UINT32 Bar0;
            UINT64 Address;

            PciIo->Pci.Read(PciIo, EfiPciIoWidthUint32, 0x10, 1, &Bar0);
            Address = Bar0 & 0xFFFFFFF0U;
            if ((Bar0 & 0x6) == 0x4) {
                UINT32 Bar1;
                PciIo->Pci.Read(PciIo, EfiPciIoWidthUint32, 0x14, 1, &Bar1);
                Address |= ((UINT64)Bar1 << 32);
            }

            BootDbg("[Boot] XHCI found! BAR0=0x%08x, Address=0x%016lx\n", Bar0, Address);
            *XhciBase = Address;
            gBS->FreePool(HandleBuffer);
            return EFI_SUCCESS;
        }
    }

    BootDbg("[Boot] No XHCI Controller found!\n");
    gBS->FreePool(HandleBuffer);
    return EFI_NOT_FOUND;
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS Status;
    BOOT_CONFIG BootConfig = {0};
    EFI_PHYSICAL_ADDRESS ElfBuffer = 0;

    /* PR-BOOT-log-uart：最先 COM1；横幅两行；其后 bring-up 只走串口 */
    BootSerialInitialize();
    BootSerialWrite("ToyBoot\n");
    if (BootSerialPresent()) {
        BootSerialWrite("COM1 Serial OK\n");
    } else {
        BootSerialWrite("COM1 Unavailable\n");
    }

    Status = GetAndSetVideo(ImageHandle, &BootConfig.VideoConfig, &BootConfig);
    if (EFI_ERROR(Status)) {
        BootSerialPrintf("Boot: GetAndSetVideo Failed: %r\n", Status);
        return Status;
    }

    {
        UINTN ElfSize = 0;
        Status = ReadKernelFile(ImageHandle, &ElfBuffer, &ElfSize);
        if (EFI_ERROR(Status)) {
            BootSerialPrintf("Boot: ReadKernelFile Failed: %r\n", Status);
            return Status;
        }

        BootSerialPrintf("Boot: Loading Kernel ELF...\n");
        Status = CheckAndLoadKernel(ElfBuffer, ElfSize, &BootConfig.KernelEntry);
        if (EFI_ERROR(Status)) {
            BootSerialPrintf("Boot: CheckAndLoadKernel Failed: %r\n", Status);
            return Status;
        }
        BootSerialPrintf("Boot: Kernel Loaded, Entry=0x%lx\n", BootConfig.KernelEntry);
    }

    Status = GetRsdpAddress(&BootConfig.RsdpAddress);
    if (EFI_ERROR(Status)) {
        BootSerialPrintf("Boot: ACPI RSDP Not Found (Continue)\n");
        BootConfig.RsdpAddress = 0;
    }

    BootConfig.SystemTable = SystemTable;

    BootDbg("[Boot] Calling GetXhciBaseAddress...\n");
    if (!EFI_ERROR(GetXhciBaseAddress(&BootConfig.XhciBaseAddress))) {
        BootDbg("[Boot] XHCI Base: 0x%016lx\n", BootConfig.XhciBaseAddress);
    } else {
        BootConfig.XhciBaseAddress = 0;
        BootDbg("[Boot] XHCI not found, setting to 0\n");
    }
    BootDbg("[Boot] BOOT_CONFIG.XhciBaseAddress = 0x%016lx\n", BootConfig.XhciBaseAddress);
    BootSerialPrintf("Boot: ExitBootServices + Jump 0x%lx\n", BootConfig.KernelEntry);

    return JumpToKernel(ImageHandle, &BootConfig);
}