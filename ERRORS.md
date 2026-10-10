# wave-os error codes

wave-os shows each code in **yellow** on screen, lists them with the `errors` command, and keeps a
summary as the **first line of its log**. After a reboot into ChromeOS (Ctrl+D, Ctrl+Alt+F2, log in
as `chronos`):

    sudo head -3 /sys/fs/pstore/console-ramoops-0

`status: ok, no errors` means everything worked. If the log is missing entirely, wave-os froze and the
watchdog rebooted the machine (that wipes RAM) -- or it never started (check the USB stick is pushed in).

**Codes look like `AREA.NN` or `AREA.NN.k`:** where it happened (`WIFI`), which error (`12`), and, when one error has several causes,
which cause (`3`). On screen and in the log each code is followed by what it says, e.g. `6.12.3: the Wi-Fi firmware's answer was not understood`.

## 1 Boot, 2 Log, 3 Screen

| Code | Meaning | What to try |
|---|---|---|
| **1.01** | Device tree unreadable (nothing valid at the address the firmware passed) | Cache flush in `start.S` failed or the firmware changed |
| **2.01** | ramoops log area not usable: this boot leaves no log | Only the screen shows what happened |
| **3.01** | No screen found (no device-tree framebuffer, no coreboot table, no active display layer) | Firmware may have changed the display setup |
| **3.02** | Display restarted but no frames reach the panel (RDMA0 sees no frame end) | Backlight may glow with a black picture; reboot and retry |

## 4 Keyboard, 5 EC (Chrome EC over SPI)

| Code | Meaning | What to try |
|---|---|---|
| **4.01.1** | Keyboard unavailable: the EC did not answer hello (5 tries) | Full power-off and retry; look at the `ec hello try N failed` lines |
| **4.01.2** | Keyboard unavailable: the key-matrix read failed 3 times | |
| **4.02** | SPI send failed: the controller never finished a chunk | SPI clock or pins off |
| **4.03** | EC not ready while receiving our request | EC busy (e.g. updating); retry |
| **4.04** | SPI read failed while waiting for the reply | |
| **4.05** | EC never started a reply within 200 ms | EC asleep or protocol mismatch |
| **4.06** | SPI read failed in the reply header | |
| **4.07** | Reply header invalid (wrong version or too long) | |
| **4.08** | SPI read failed in the reply data | Was the "odd-sized chunk" jam before v0.8.1 |
| **4.09** | Reply checksum wrong | Noise on the bus, or misaligned FIFO reads |
| **4.10** | Request too long (wave-os bug) | |
| **5.01..11** | The EC itself answered with an error: 1 invalid command, 2 error, 3 invalid parameter, 4 access denied, 5 invalid response, 6 invalid version, 7 invalid checksum, 8 in progress, 9 unavailable, 10 timeout, 11 overflow | |

4.02..4.10 are wave-os's own checks in `ec.h` (the number is `-ec_cmd()`); EC-nn are result codes
the EC sent back (`EC_RES_*` in Linux `cros_ec_commands.h`).

## 6 Wi-Fi (Marvell 88W8897 over SDIO)

| Code | Meaning | What to try |
|---|---|---|
| **6.01** | PMIC wrapper did not answer: can't read or write the Wi-Fi bus supply (VGP3) | Wrapper busy or not initialised by the firmware |
| **6.02** | Wi-Fi bus supply VGP3 did not switch on at 3.3 V (read-back mismatch) | PMIC write not taking effect |
| **6.03** | Chip did not answer CMD5 (SDIO "what voltage?") | Chip unpowered (GPIO85 is active low), supply off, or bus/clock problem |
| **6.04** | Chip never became ready (CMD5 busy for 1 s) | Voltage window refused |
| **6.05.1** | Chip did not give an address (CMD3) | |
| **6.05.2** | Chip could not be selected (CMD7) | |
| **6.06.1** | CMD52 failed reading the card's basic registers (CCCR) | |
| **6.06.2** | CMD52 failed walking the card's ID table (CIS) | |
| **6.06.3** | CMD52 failed enabling Wi-Fi function 1 | |
| **6.06.4** | CMD52 failed setting function 1's block size | |
| **6.06.5** | CMD52 failed switching the bus to 4-bit (firmware upload) | |
| **6.06.6** | CMD52 failed in the chip's setup registers (firmware upload) | |
| **6.07** | Unexpected SDIO vendor/device ID (not a Marvell 88W8897: card 0x912c, Wi-Fi 0x912d) | Different Wi-Fi chip in this unit? |
| **6.08** | Wi-Fi function 1 did not become ready after enabling it | |
| **6.09.1** | Firmware upload: the bus became unreliable at the faster speed (4-bit) | Try a slower clock |
| **6.09.2** | Firmware upload: the chip never said it is ready for data (status register 0x50) | A register snapshot is printed above the code |
| **6.09.3** | Firmware upload: a data write to the chip failed | The block-write number and byte offset are printed above |
| **6.09.4** | Firmware upload: too many "resend" requests from the chip | |
| **6.10** | Firmware upload: the chip asked for an impossible length | |
| **6.11** | Firmware uploaded but did not report ready (status 0xfedc) | The status value is printed above |
| **6.12.1** | Could not send a command to the Wi-Fi firmware (data write failed) | |
| **6.12.2** | The Wi-Fi firmware did not answer a command within 1 s | |
| **6.12.3** | The firmware's answer was not understood (wrong length, type or command) | |
| **6.13.1** | The Wi-Fi firmware rejected a command (an error number is printed above) | || **6.14.1** | The scan answer was too short | |
| **6.14.2** | A scan record in the answer was malformed | |
| **6.14.3** | The scan finished but the firmware sent no scan result events | |
| **6.15.1** | The router refused the connection (the association status number is in the summary) | |
| **6.15.2** | The network has no WPA2 (RSN) element | |
| **6.15.3** | The network does not offer CCMP encryption | |
| **6.15.4** | The network does not offer a WPA2 password (PSK) login | |
| **6.15.5** | The network requires protected management frames (not supported yet) | |
| **6.15.6** | The network was not found, so there was nothing to join | |
| **6.16.1** | The router never started the password handshake (or its first message was not what we expected) | |
| **6.16.2** | The router sent no handshake message 3: the password is probably wrong | |
| **6.16.3** | Handshake message 3 was malformed, failed its signature check (wrong password) or the router changed its nonce | |
| **6.16.4** | The group key in message 3 could not be decrypted or was missing | |
| **6.16.5** | The Wi-Fi chip refused an encryption key | |
| **6.16.6** | A handshake message could not be sent (no free data port) | |
| **6.16.7** | The password must be 8 to 63 characters | |
| **6.17.1** | Connected, but the router gave no network address (DHCP) | |
| **7.01.1** | Not connected: run k (wificonnect) first | |
| **7.01.2** | No ping answer | |
| **7.30.1** | No DNS server: connect to Wi-Fi first (k) | |
| **7.30.2** | That name does not exist | |
| **7.30.3** | The DNS server did not answer | |
| **7.30.4** | Not a valid name | |
| **7.31.1** | Browser: the address is not valid (it must start with http://) | |
| **7.31.2** | Browser: the page needs HTTPS, which wave-os cannot do yet | |
| **7.32.N** | Browser: could not load the page (N = 2 no connection, 3 too slow, 4 answer not understood) | |
| **7.33.1** | Browser: too many redirects | |

## 7 Network and update

| Code | Meaning | What to try |
|---|---|---|
| **7.20** | update: could not download the manifest | Is the update server running on the PC? Right address? Firewall? |
| **7.21** | update: the manifest is malformed | Server and wave-os versions differ |
| **7.22** | update: the image size is not plausible | |
| **7.23** | update: could not download the image | Connection dropped; try again |
| **7.24** | update: the image size does not match the manifest | Image changed while downloading |
| **7.25** | update: the checksum does not match | Damaged download; try again |
| **7.26** | update: the signature is wrong (not signed with our key) | Server uses a different key: copy update_key.txt |
| **7.27** | update: not a valid arm64 image | |

## 8 Touchpad (Elan on I2C bus 4)

| Code | Meaning | What to try |
|---|---|---|
| **8.01** | the touchpad did not answer on I2C (no ACK at address 0x15) | Run `tp`: it also tries a HID-over-I2C pad at 0x2c; send its output |
| **8.02** | the I2C controller did not finish a transfer | Clock or pins not set up; send the `tp` output |
| **8.03.1** | the PMIC did not answer (touchpad power unknown) | Reboot |
| **8.04** | the touchpad stopped answering after its reset | Try `tptest` again |
