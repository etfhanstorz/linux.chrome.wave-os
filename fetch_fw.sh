#!/bin/sh
# Copies the Marvell 88W8897 firmware into fw/ (from ~/work/firmware, or downloads it from kernel.org).
set -e
mkdir -p fw
if [ -f "$HOME/work/firmware/mrvl/sd8897_uapsta.bin" ]; then
  cp "$HOME/work/firmware/mrvl/sd8897_uapsta.bin" fw/
else
  curl -sSfL -o fw/sd8897_uapsta.bin https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mrvl/sd8897_uapsta.bin
fi
echo "firmware: $(wc -c < fw/sd8897_uapsta.bin) bytes"
