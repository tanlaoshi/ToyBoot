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
