# wave-os

A tiny bare-metal aarch64 operating system that boots on the **Lenovo MT8173 "hana" Chromebook**
(IdeaPad Flex 3 CB-11M735 class, firmware `Google_Hana.8438`), from a USB stick, in developer mode.

**Status (v0.7):** boots from USB, takes over the firmware's screen, turns the backlight back on and
draws text with its own font. A shell with line editing works in QEMU; keyboard input on the real
machine (Chrome EC over SPI) is the next step.

## How it boots

The ChromeOS firmware (depthcharge) loads a signed kernel partition (`.kpart`) containing a FIT image:
our arm64 `Image` + device trees. Things that turned out to matter on the real machine:

- The FIT must use the same layout as the Arch Linux ARM kernel that is known to boot
  (`kernel_noload`, its device trees in `archdt/`).
- The firmware enters at EL2 with the device tree still dirty in the data cache, so `start.S`
  cleans the whole D-cache by set/way before touching memory.
- The firmware stops the display overlay engine and the backlight before handing over;
  `display.h` turns them back on (OVL0, GPIO32, GPIO95, pin 87).
- With no serial port, wave-os writes its log into ChromeOS's **ramoops** area (1 MB at
  `0xb1f00000`). After a reboot into ChromeOS: `sudo cat /sys/fs/pstore/console-ramoops-0`.

## Building and testing (Windows + WSL Ubuntu)

| | |
|---|---|
| `build.sh` (in WSL) | builds `Image`, the FIT and the dev-key-signed `out.kpart` |
| `fake-test.bat` | runs `out.kpart` on **fakehana**, an emulated hana (Unicorn CPU + fake MediaTek hardware), no flashing |
| `run.sh` (in WSL) | QEMU build with a window; type into the shell from the terminal |
| `flash.bat` | builds and writes `out.kpart` to the USB stick's kernel partition (asks for admin + `YES`) |
| `releases/` | every milestone's signed image |

Packages: `gcc-aarch64-linux-gnu device-tree-compiler u-boot-tools vboot-kernel-utils qemu-system-arm python3-unicorn`.

## Roll back

    git tag                      # list versions
    git checkout v0.6.2-log      # go back to a known version
    git checkout master          # return to latest

Or flash any file from `releases/`. ChromeOS on the internal drive is never touched: Ctrl+D always boots it.
