# wave-os
Bare-metal aarch64 OS for the Lenovo MT8173 Chromebook ("hana").

- `build.sh` (WSL): builds Image + FIT + signed out.kpart
- `run.sh` (WSL): builds a QEMU test kernel and opens a window
- `releases/`: known-good signed kpart files, one per tagged version

## Roll back
    git tag                      # list versions
    git checkout v0.1-red        # go back to a known-good version
    git checkout master          # return to latest

Milestone 1 (v0.1-red): finds the firmware framebuffer in the DTB and fills it red.
Untested on real hardware.
