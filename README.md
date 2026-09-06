# ToyBoot - UEFI Bootloader for ToyOS

A minimal UEFI bootloader that loads ELF kernels with GOP graphics support.

## Features
- GOP graphics mode selection (QEMU-friendly table / real-hardware EDID)
- ELF64 kernel loading：**优先 TOYOS 卷**（`TOYOS.ID` + `Kernel.elf`），启动盘仅作兜底
- UEFI memory map / ACPI RSDP / XHCI BAR handoff

## Layout (third OS)

```
ESP (FAT):     EFI/BOOT/BOOTX64.EFI   <- UEFI / GRUB 加载
TOYOS (FAT):   TOYOS.ID, Kernel.elf, THEME.CFG, HELLO.ELF, ...
```

QEMU：`ToyImage/run-split.sh`（盘0=ESP，盘1=`rootfs/`）。

GRUB example:

```grub
menuentry "ToyOS" {
    insmod part_gpt
    insmod fat
    search --file /EFI/ToyOS/BOOTX64.EFI --set=root
    chainloader /EFI/ToyOS/BOOTX64.EFI
}
```

## Build

```bash
./build.sh
# or DEBUG=1
```

Output is copied to `ToyImage/EFI/BOOT/BOOTX64.EFI`.

## Real PC / laptop (PR-H0)

Target machine: **UEFI** firmware, **FAT** USB (ESP + TOYOS layout above), USB keyboard preferred, serial **optional**.

1. Build Kernel + ToyBoot; copy `EFI/BOOT/BOOTX64.EFI` and `rootfs/` contents onto the stick.
2. Boot from firmware Boot Menu (disable Secure Boot if needed).
3. **H0 pass**：after ToyBoot loads `Kernel.elf`, the display shows GOP output (desktop clear / icons). Serial `ToyOS ready` is nice-to-have, not required on machines without COM1.

GOP path: firmware GOP → ToyBoot mode pick (VM table vs EDID on real hardware) → `BOOT_CONFIG` → `HAL/X64/Startup` → `BOOT_INFO` → `HalVideoSet`.

Known gaps (storage / keyboard / no COM1 / NIC) and per-machine notes:

→ [`ToyKernel/HAL/X64/NOTES-UEFI-PC.md`](../ToyKernel/HAL/X64/NOTES-UEFI-PC.md)  
→ roadmap **1.3c** (`Documents/路线图.md`)

Do **not** mix Duo S / Board-package work (**1.3b**) into ToyBoot PRs.
