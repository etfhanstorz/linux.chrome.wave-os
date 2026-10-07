#!/bin/sh
# Run inside Linux/WSL. Needs: gcc-aarch64-linux-gnu device-tree-compiler u-boot-tools vboot-utils (futility)
set -e
X=aarch64-linux-gnu-
${X}gcc -c -O2 -ffreestanding -fpie -mgeneral-regs-only -mstrict-align -nostdlib main.c -o main.o
[ -f fw/sd8897_uapsta.bin ] || sh fetch_fw.sh
${X}gcc -c fw.S -o fw.o
${X}gcc -c start.S -o start.o
${X}ld -pie --no-dynamic-linker -z notext -T link.ld -nostdlib start.o main.o fw.o -o kernel.elf
${X}objcopy -O binary kernel.elf Image
# Sanity-check the arm64 Image header and the relocations before packing
python3 - <<'EOF'
import struct, subprocess, sys
d = open('Image', 'rb').read()
code0, _, text_offset, image_size, flags = struct.unpack_from('<IIQQQ', d, 0)
ok = d[56:60] == b'ARMd' and text_offset == 0x80000 and image_size >= len(d) and image_size % 0x10000 == 0 and flags == 0
rel = subprocess.run(['aarch64-linux-gnu-readelf', '-rW', 'kernel.elf'], capture_output=True, text=True).stdout
bad = [l for l in rel.splitlines() if 'R_AARCH64_' in l and 'R_AARCH64_RELATIVE' not in l and 'R_AARCH64_NONE' not in l]
print('Image header: magic %s, image_size %#x (file %d bytes), relocations: %d relative, %d other'
      % (d[56:60], image_size, len(d), rel.count('R_AARCH64_RELATIVE'), len(bad)))
if not ok or bad:
    print('BUILD CHECK FAILED', bad[:5]); sys.exit(1)
EOF
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
