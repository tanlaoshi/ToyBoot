#include <Uefi.h>
#include <Guid/Acpi.h>
#include <Guid/FileInfo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>

#include "BootPrivate.h"

/*
 * PR-S-boot-1/2/3：Video / Kernel / Pci 已拆出；BootDbg / 类型别名见 BootPrivate.h。
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
