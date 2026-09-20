/*
 * BootKernelLoad.c — PR-S-boot-2：ELF 校验与段装载
 *
 * 从 Boot.c 原样搬家；不改语义。
 */
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>

#include "BootPrivate.h"

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

EFI_STATUS CheckAndLoadKernel(EFI_PHYSICAL_ADDRESS ElfBase, UINTN FileSize,
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
