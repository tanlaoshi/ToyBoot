/*
 * BootKernel.c — PR-S-boot-2：找盘读 Kernel.elf
 *
 * 从 Boot.c 原样搬家；不改语义。
 */
#include <Guid/FileInfo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>

#include "BootPrivate.h"

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

EFI_STATUS ReadKernelFile(EFI_HANDLE ImageHandle, EFI_PHYSICAL_ADDRESS *OutBuffer,
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
