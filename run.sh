#!/bin/sh
# Build a QEMU test kernel and open it in a window (WSLg). Run in WSL.
set -e
X=aarch64-linux-gnu-
mkdir -p /tmp/q && cp t.dts /tmp/q/ 2>/dev/null || true
cat > /tmp/q/t.dts <<'EOD'
/dts-v1/;
/ { #address-cells=<2>; #size-cells=<2>;
  framebuffer@50000000 { compatible="simple-framebuffer"; reg=<0 0x50000000 0 0x400000>; width=<1366>; height=<768>; stride=<5464>; format="a8r8g8b8"; };
};
EOD
dtc -I dts -O dtb -o /tmp/q/t.dtb /tmp/q/t.dts 2>/dev/null
${X}gcc -DQEMU -c -O2 -ffreestanding -fno-pic -mcmodel=tiny -mgeneral-regs-only -mstrict-align -nostdlib main.c -o /tmp/q/main.o
${X}gcc -c start.S -o /tmp/q/start.o
${X}ld -T link.ld -nostdlib /tmp/q/start.o /tmp/q/main.o -o /tmp/q/k.elf
${X}objcopy -O binary /tmp/q/k.elf /tmp/q/Image
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 1G -kernel /tmp/q/Image -dtb /tmp/q/t.dtb -device ramfb -display gtk -serial mon:stdio
