# wave-os error codes

wave-os shows each code in **yellow** on screen, lists them with the `errors` command, and keeps a
summary as the **first line of its log**. After a reboot into ChromeOS (Ctrl+D, Ctrl+Alt+F2, log in
as `chronos`):

    sudo head -3 /sys/fs/pstore/console-ramoops-0

`status: ok, no errors` means everything worked. If the log is missing entirely, wave-os froze and the
watchdog rebooted the machine (that wipes RAM) -- or it never started (check the USB stick is pushed in).

**Codes look like `AREA-NN` or `AREA-NN.k`.** When one code can have several causes, the number after the
dot says which one it was (for example `WIFI-09.2`). Codes without a dot have a single cause.

## Boot, log, screen

| Code | Meaning | What to try |
|---|---|---|
| **BOOT-01** | Device tree unreadable (nothing valid at the address the firmware passed) | Cache flush in `start.S` failed or the firmware changed |
| **LOG-01** | ramoops log area not usable: this boot leaves no log | Only the screen shows what happened |
| **DISP-01** | No screen found (no device-tree framebuffer, no coreboot table, no active display layer) | Firmware may have changed the display setup |
| **DISP-02** | Display restarted but no frames reach the panel (RDMA0 sees no frame end) | Backlight may glow with a black picture; reboot and retry |

## Keyboard (Chrome EC over SPI)

| Code | Meaning | What to try |
|---|---|---|
| **KB-01.1** | Keyboard unavailable: the EC did not answer hello (5 tries) | Full power-off and retry; look at the `ec hello try N failed` lines |
| **KB-01.2** | Keyboard unavailable: the key-matrix read failed 3 times | |
| **KB-02** | SPI send failed: the controller never finished a chunk | SPI clock or pins off |
| **KB-03** | EC not ready while receiving our request | EC busy (e.g. updating); retry |
| **KB-04** | SPI read failed while waiting for the reply | |
| **KB-05** | EC never started a reply within 200 ms | EC asleep or protocol mismatch |
| **KB-06** | SPI read failed in the reply header | |
| **KB-07** | Reply header invalid (wrong version or too long) | |
| **KB-08** | SPI read failed in the reply data | Was the "odd-sized chunk" jam before v0.8.1 |
| **KB-09** | Reply checksum wrong | Noise on the bus, or misaligned FIFO reads |
| **KB-10** | Request too long (wave-os bug) | |
| **EC-01..11** | The EC itself answered with an error: 1 invalid command, 2 error, 3 invalid parameter, 4 access denied, 5 invalid response, 6 invalid version, 7 invalid checksum, 8 in progress, 9 unavailable, 10 timeout, 11 overflow | |

KB-02..KB-10 are wave-os's own checks in `ec.h` (the number is `-ec_cmd()`); EC-nn are result codes
the EC sent back (`EC_RES_*` in Linux `cros_ec_commands.h`).

## Wi-Fi (Marvell 88W8897 over SDIO)

| Code | Meaning | What to try |
|---|---|---|
| **WIFI-01** | PMIC wrapper did not answer: can't read or write the Wi-Fi bus supply (VGP3) | Wrapper busy or not initialised by the firmware |
| **WIFI-02** | Wi-Fi bus supply VGP3 did not switch on at 3.3 V (read-back mismatch) | PMIC write not taking effect |
| **WIFI-03** | Chip did not answer CMD5 (SDIO "what voltage?") | Chip unpowered (GPIO85 is active low), supply off, or bus/clock problem |
| **WIFI-04** | Chip never became ready (CMD5 busy for 1 s) | Voltage window refused |
| **WIFI-05.1** | Chip did not give an address (CMD3) | |
| **WIFI-05.2** | Chip could not be selected (CMD7) | |
| **WIFI-06.1** | CMD52 failed reading the card's basic registers (CCCR) | |
| **WIFI-06.2** | CMD52 failed walking the card's ID table (CIS) | |
| **WIFI-06.3** | CMD52 failed enabling Wi-Fi function 1 | |
| **WIFI-06.4** | CMD52 failed setting function 1's block size | |
| **WIFI-06.5** | CMD52 failed switching the bus to 4-bit (firmware upload) | |
| **WIFI-06.6** | CMD52 failed in the chip's setup registers (firmware upload) | |
| **WIFI-07** | Unexpected SDIO vendor/device ID (not a Marvell 88W8897: card 0x912c, Wi-Fi 0x912d) | Different Wi-Fi chip in this unit? |
| **WIFI-08** | Wi-Fi function 1 did not become ready after enabling it | |
| **WIFI-09.1** | Firmware upload: the bus became unreliable at the faster speed (4-bit) | Try a slower clock |
| **WIFI-09.2** | Firmware upload: the chip never said it is ready for data (status register 0x50) | A register snapshot is printed above the code |
| **WIFI-09.3** | Firmware upload: a data write to the chip failed | The block-write number and byte offset are printed above |
| **WIFI-09.4** | Firmware upload: too many "resend" requests from the chip | |
| **WIFI-10** | Firmware upload: the chip asked for an impossible length | |
| **WIFI-11** | Firmware uploaded but did not report ready (status 0xfedc) | The status value is printed above |
| **WIFI-12.1** | Could not send a command to the Wi-Fi firmware (data write failed) | |
| **WIFI-12.2** | The Wi-Fi firmware did not answer a command within 1 s | |
| **WIFI-12.3** | The firmware's answer was not understood (wrong length, type or command) | |
| **WIFI-13.1** | The Wi-Fi firmware rejected a command (an error number is printed above) | || **WIFI-14.1** | The scan answer was too short | |
| **WIFI-14.2** | A scan record in the answer was malformed | |
| **WIFI-14.3** | The scan finished but the firmware sent no scan result events | |
