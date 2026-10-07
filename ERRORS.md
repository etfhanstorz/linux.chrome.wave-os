# wave-os error codes

wave-os shows each code in **yellow** on screen, lists them with the `errors` command, and keeps a
summary as the **first line of its log**. After a reboot into ChromeOS (Ctrl+D, Ctrl+Alt+F2, log in
as `chronos`):

    sudo head -3 /sys/fs/pstore/console-ramoops-0

`status: ok, no errors` means everything worked. If the log is missing entirely, wave-os froze and the
watchdog rebooted the machine (that wipes RAM) -- or it never started (check the USB stick is pushed in).

| Code | Meaning | What to try |
|---|---|---|
| **BOOT-01** | Device tree unreadable (nothing valid at the address the firmware passed) | Cache flush in `start.S` failed or the firmware changed |
| **LOG-01** | ramoops log area not usable: this boot leaves no log | Only the screen shows what happened |
| **DISP-01** | No screen found (no device-tree framebuffer, no coreboot table, no active display layer) | Firmware may have changed the display setup |
| **DISP-02** | Display restarted but no frames reach the panel (RDMA0 sees no frame end) | Backlight may glow with a black picture; reboot and retry |
| **KB-01** | Keyboard unavailable (the code just before it says why) | |
| **KB-02** | SPI send failed: the controller never finished a chunk | SPI clock or pins off |
| **KB-03** | EC not ready while receiving our request | EC busy (e.g. updating); retry |
| **KB-04** | SPI read failed while waiting for the reply | |
| **KB-05** | EC never started a reply within 200 ms | EC asleep or protocol mismatch |
| **KB-06** | SPI read failed in the reply header | |
| **KB-07** | Reply header invalid (wrong version or too long) | |
| **KB-08** | SPI read failed in the reply data | Was the "odd-sized chunk" jam before v0.8.1 |
| **KB-09** | Reply checksum wrong | Noise on the bus, or misaligned FIFO reads |
| **KB-10** | Request too long (wave-os bug) | |
| **EC-01** | EC: invalid command (this EC does not know the command) | |
| **EC-02** | EC: error | |
| **EC-03** | EC: invalid parameter | |
| **EC-04** | EC: access denied | |
| **EC-05** | EC: invalid response | |
| **EC-06** | EC: invalid version | |
| **EC-07** | EC: invalid checksum (it got our request garbled) | |
| **EC-08** | EC: in progress | |
| **EC-09** | EC: unavailable | |
| **EC-10** | EC: timeout | |
| **EC-11** | EC: overflow | |

KB-02..KB-10 are wave-os's own checks in `ec.h` (the number is `-ec_cmd()`); EC-nn are result codes
the EC itself sent back (`EC_RES_*` in Linux `cros_ec_commands.h`).
