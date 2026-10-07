#!/bin/sh
# Run inside Linux/WSL. Needs: gcc-aarch64-linux-gnu device-tree-compiler u-boot-tools vboot-utils (futility)
set -e
X=aarch64-linux-gnu-
${X}gcc -c -O2 -ffreestanding -fno-pic -mcmodel=tiny -mgeneral-regs-only -mstrict-align -nostdlib main.c -o main.o
${X}gcc -c start.S -o start.o
${X}ld -T link.ld -nostdlib start.o main.o -o kernel.elf
${X}objcopy -O binary kernel.elf Image
[ -f archdt/fdt1.dtb ] || { echo "Need archdt/*.dtb (Arch device trees)"; exit 1; }
# Arch Linux ARM FIT layout + device trees: proven to be started by this firmware (hana.its untested since the USB contact problem)
mkimage -f archdt.its image.itb >/dev/null 2>&1
head -c 512 /dev/zero > bootloader.bin
futility vbutil_kernel --pack out.kpart \
  --keyblock /usr/share/vboot/devkeys/kernel.keyblock \
  --signprivate /usr/share/vboot/devkeys/kernel_data_key.vbprivk \
  --version 1 --config cmdline --bootloader bootloader.bin \
  --vmlinuz image.itb --arch arm
ls -l out.kpart
