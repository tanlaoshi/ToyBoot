/*
 * BootPci.c — PR-S-boot-3：PCI 上找 xHCI 基址
 *
 * 从 Boot.c 原样搬家；不改语义。
 */
#include <Protocol/PciIo.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>

#include "BootPrivate.h"

EFI_STATUS GetXhciBaseAddress(UINT64 *XhciBase) {
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
