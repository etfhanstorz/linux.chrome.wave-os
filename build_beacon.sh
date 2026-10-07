#!/bin/sh
# Builds beacon.kpart: a tiny test kernel that reboots the machine ~4 s after it starts. Run in WSL.
# Uses the Arch Linux ARM FIT layout and device trees (archdt/), which are known to boot on this firmware.
set -e
X=aarch64-linux-gnu-
B=/tmp/beacon
mkdir -p $B/archdt
${X}gcc -c beacon.S -o $B/beacon.o
${X}ld -T link.ld -nostdlib $B/beacon.o -o $B/beacon.elf 2>/dev/null
${X}objcopy -O binary $B/beacon.elf $B/Image
cp archdt.its cmdline $B/
cp archdt/*.dtb $B/archdt/
cd $B
mkimage -f archdt.its image.itb >/dev/null 2>&1
head -c 512 /dev/zero > bootloader.bin
futility vbutil_kernel --pack beacon.kpart \
  --keyblock /usr/share/vboot/devkeys/kernel.keyblock \
  --signprivate /usr/share/vboot/devkeys/kernel_data_key.vbprivk \
  --version 1 --config cmdline --bootloader bootloader.bin \
  --vmlinuz image.itb --arch arm
cd - >/dev/null
cp $B/beacon.kpart beacon.kpart
ls -l beacon.kpart
