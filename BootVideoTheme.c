/*
 * BootVideoTheme.c — PR-S-boot-1：THEME.CFG mode= / TOYOS 卷
 *
 * 从 Boot.c 原样搬家；不改语义。
 */
#include <Guid/FileInfo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>

#include "BootPrivate.h"

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
BOOLEAN FsHasToyOsId(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs) {
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

BOOLEAN TryLoadDisplayPref(EFI_HANDLE ImageHandle, UINT32 *OutW,
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
