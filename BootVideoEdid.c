/*
 * BootVideoEdid.c — PR-S-boot-1：EDID 首选分辨率
 *
 * 从 Boot.c 原样搬家；不改语义。
 */
#include <Protocol/EdidActive.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>

#include "BootPrivate.h"

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

EFI_STATUS TryGetEdidPreferred(EFI_HANDLE ImageHandle, EFI_HANDLE GopHandle,
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
