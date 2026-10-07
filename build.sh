#!/bin/sh
# Run inside Linux/WSL. Needs: gcc-aarch64-linux-gnu device-tree-compiler u-boot-tools vboot-utils (futility)
set -e
X=aarch64-linux-gnu-
${X}gcc -c -O2 -ffreestanding -fno-pic -mcmodel=tiny -mgeneral-regs-only -mstrict-align -nostdlib main.c -o main.o
${X}gcc -c start.S -o start.o
${X}ld -T link.ld -nostdlib start.o main.o -o kernel.elf
${X}objcopy -O binary kernel.elf Image
[ -f mt8173-elm-hana.dtb ] || { echo "Need mt8173-elm-hana.dtb in this folder"; exit 1; }
mkimage -f hana.its image.itb
head -c 512 /dev/zero > bootloader.bin
futility vbutil_kernel --pack out.kpart \
  --keyblock /usr/share/vboot/devkeys/kernel.keyblock \
  --signprivate /usr/share/vboot/devkeys/kernel_data_key.vbprivk \
  --version 1 --config cmdline --bootloader bootloader.bin \
  --vmlinuz image.itb --arch arm
ls -l out.kpart
