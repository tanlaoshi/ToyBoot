# ToyBoot - UEFI Bootloader for ToyOS

A minimal UEFI bootloader that loads ELF kernels with GOP graphics support.

## Features
- GOP graphics mode selection (QEMU-friendly table / real-hardware EDID)
- ELF64 kernel loading from boot volume or another FAT volume (`Kernel.elf`)
- UEFI memory map / ACPI RSDP / XHCI BAR handoff

## Layout (third OS)

```
ESP (FAT):     EFI/ToyOS/BOOTX64.EFI   <- GRUB chainloads this
TOYOS (FAT):   Kernel.elf, HELLO.ELF   <- ToyBoot + kernel read here
```

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
