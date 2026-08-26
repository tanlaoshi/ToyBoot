# ToyBoot - UEFI Bootloader for ToyOS

A minimal UEFI bootloader that loads ELF kernels with GOP graphics support.

## Features
- GOP graphics mode selection
- ELF64 kernel loading and relocation
- UEFI memory map passing
- ACPI RSDP passing
- UEFI keyboard and mouse protocol passing
- Keeps Boot Services alive

## Build
```bash
source /path/to/edk2/edksetup.sh
./build.sh