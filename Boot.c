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
 * PR-S-boot-1/2：Video / Kernel 已拆出；BootDbg / 类型别名见 BootPrivate.h。
 */


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