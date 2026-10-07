#!/bin/sh
# Builds beacon.kpart for the current boot experiment. Run in WSL (flash-beacon.bat does this).
#
# Current experiment: the real Arch Linux ARM kernel (archdt/arch_Image, known to be started by this
# firmware) with ONE instruction changed: its jump to Linux startup code (offset 4, "b 0x1000") now
# jumps to payload.S appended at the end of the file, which reboots the machine immediately.
# FIT layout and device trees are the Arch ones (archdt.its, archdt/).
set -e
X=aarch64-linux-gnu-
B=/tmp/beacon
mkdir -p $B/archdt
${X}gcc -c payload.S -o $B/payload.o
${X}objcopy -O binary $B/payload.o $B/payload.bin
cp archdt/arch_Image $B/Image
python3 - "$B/Image" "$B/payload.bin" <<'EOF'
import sys, struct
img, pay = sys.argv[1], sys.argv[2]
d = bytearray(open(img, 'rb').read())
assert d[56:60] == b'ARMd', 'not an arm64 Image'
assert struct.unpack_from('<I', d, 4)[0] == 0x140003ff, 'unexpected instruction at offset 4'
end = len(d)
assert end % 4 == 0
image_size = struct.unpack_from('<Q', d, 16)[0]
p = open(pay, 'rb').read()
assert end + len(p) <= image_size, 'payload would not fit inside image_size'
struct.pack_into('<I', d, 4, 0x14000000 | (((end - 4) // 4) & 0x3ffffff))   # b payload
d += p
open(img, 'wb').write(d)
print('patched: offset 4 -> b %#x, payload %d bytes, image %d bytes, image_size %#x' % (end, len(p), len(d), image_size))
EOF
cp archdt.its cmdline $B/
cp archdt/fdt*.dtb $B/archdt/
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
