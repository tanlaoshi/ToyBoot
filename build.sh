cd ~/tanlaoshi/edk2
source edksetup.sh

build -a X64 -p ToyBoot/Boot.dsc -t GCC

if [ -f "Build/ToyBoot/DEBUG_GCC5/FV/ToyBoot.efi" ]; then
    echo ""
    echo "=========================================="
    echo "Build successful!"
    echo "Output: Build/ToyBoot/DEBUG_GCC5/FV/ToyBoot.efi"
    echo "=========================================="
    ls -lh Build/ToyBoot/DEBUG_GCC5/FV/ToyBoot.efi
    cp Build/ToyBoot/DEBUG_GCC/X64/ToyBoot.efi ToyImage/EFI/BOOT/BOOTX64.EFI
else
    echo ""
    echo "Build failed!"
    exit 1
fi

cd ~/tanlaoshi/edk2/ToyBoot