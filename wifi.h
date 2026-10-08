// Wi-Fi step 1: a read-only probe of everything the Marvell 88W8897 (SDIO, on MSDC3) depends on.
// Nothing is switched on here; this only reports the state the firmware left behind.
//   chip power:  GPIO85 ("WIFI_PDN", also the sdio_fixed_3v3 regulator enable), active high
//   chip IRQ:    GPIO38 (MARVELL8897_IRQ), active low
//   SDIO bus:    pins 22-25 DAT0-3, 26 CLK, 27 CMD (pinmux mode 1 = MSDC3)
//   controller:  MSDC3 @ 0x11260000, clock gate = PERI0 bit 16 (status at 0x10003018, 1 = gated)
//   bus voltage: MT6397 PMIC LDO VGP3 (enable DIGLDO_CON7 0x041e bit 15, voltage DIGLDO_CON21 0x043a bits 5-7)
//                read through the PMIC wrapper (pwrap) @ 0x1000d000, software channel WACS2.

#define PERICFG_BASE 0x10003000UL
#define MSDC3_BASE   0x11260000UL
#define PWRAP_BASE   0x1000d000UL

static u32 gpio_bit(u32 base_off, u32 pin) { return (rd32(GPIO_BASE + base_off + ((pin >> 4) << 4)) >> (pin & 15)) & 1; }
static u32 gpio_mode(u32 pin) { return (rd32(GPIO_BASE + 0x600 + (pin / 5) * 0x10) >> ((pin % 5) * 3)) & 7; }

static void show_pin(u32 pin, const char *name) {
    puts("  gpio"); put_dec(pin); puts(" "); puts(name); puts(": mode "); put_dec(gpio_mode(pin));
    puts(gpio_bit(0x000, pin) ? " out" : " in "); puts(" level "); put_dec(gpio_bit(0x500, pin));
    if (gpio_bit(0x000, pin)) { puts(" drive "); put_dec(gpio_bit(0x400, pin)); }
    putc('\n');
}

// Read a 16-bit MT6397 register through the PMIC wrapper. Returns 0 on success.
static int pmic_read(u32 adr, u32 *out) {
    volatile u32 *cmd = (volatile u32 *)(PWRAP_BASE + 0xa0);
    volatile u32 *rdata = (volatile u32 *)(PWRAP_BASE + 0xa4);
    volatile u32 *vldclr = (volatile u32 *)(PWRAP_BASE + 0xa8);
    u32 v = 0, i;
    for (i = 0; i < 100000; i++) {                       // wait for the state machine to be idle
        v = *rdata;
        u32 fsm = (v >> 16) & 7;
        if (fsm == 0) break;
        if (fsm == 6) *vldclr = 1;                       // a previous read was never acknowledged
    }
    if (i == 100000) return -1;
    *cmd = (adr >> 1) << 16;                             // bit 31 = 0: read
    for (i = 0; i < 100000; i++) {                       // wait for "data valid, waiting for clear"
        v = *rdata;
        if (((v >> 16) & 7) == 6) break;
    }
    if (i == 100000) return -2;
    *out = v & 0xffff;
    *vldclr = 1;
    return 0;
}

static int pmic_write(u32 adr, u32 val) {
    volatile u32 *cmd = (volatile u32 *)(PWRAP_BASE + 0xa0);
    volatile u32 *rdata = (volatile u32 *)(PWRAP_BASE + 0xa4);
    volatile u32 *vldclr = (volatile u32 *)(PWRAP_BASE + 0xa8);
    u32 i;
    for (i = 0; i < 100000; i++) {
        u32 fsm = (*rdata >> 16) & 7;
        if (fsm == 0) break;
        if (fsm == 6) *vldclr = 1;
    }
    if (i == 100000) return -1;
    *cmd = (1u << 31) | ((adr >> 1) << 16) | (val & 0xffff);
    for (i = 0; i < 100000; i++) if (((*rdata >> 16) & 7) == 0) return 0;   // wait for the write to finish
    return -2;
}

// ---- MSDC3 (MediaTek SD controller) in PIO mode, polled. Register meanings: Linux mtk-sd.c ----
#define MSDC_CFG    0x00
#define MSDC_INT    0x0c
#define MSDC_INTEN  0x10
#define MSDC_FIFOCS 0x14
#define SDC_CFG     0x30
#define SDC_CMD     0x34
#define SDC_ARG     0x38
#define SDC_STS     0x3c
#define SDC_RESP0   0x40
#define INT_CMDRDY  (1u << 8)
#define INT_CMDTMO  (1u << 9)
#define INT_RSPCRC  (1u << 10)
#define RSP_NONE 0
#define RSP_R1   1          // also R5, R6, R7
#define RSP_R3   3          // also R4 (no CRC)
#define RSP_R1B  7

static u32 msdc_rd(u32 off) { return rd32(MSDC3_BASE + off); }
#define RAWR(off) (*(volatile u32 *)(MSDC3_BASE + (off)))     // bare register read: no fault-flag store, no barrier (the RX FIFO overruns if the drain loop is slow)
static void msdc_wr(u32 off, u32 v) { *(volatile u32 *)(MSDC3_BASE + off) = v; }

static void msdc_init_slow(void) {
    msdc_wr(MSDC_CFG, msdc_rd(MSDC_CFG) | 1 | (1u << 3));                 // SD/MMC mode, PIO
    msdc_wr(MSDC_CFG, msdc_rd(MSDC_CFG) | (1u << 2));                     // reset
    for (int i = 0; i < 100000 && (msdc_rd(MSDC_CFG) & (1u << 2)); i++) ;
    msdc_wr(MSDC_FIFOCS, msdc_rd(MSDC_FIFOCS) | (1u << 31));              // clear FIFOs
    for (int i = 0; i < 100000 && (msdc_rd(MSDC_FIFOCS) & (1u << 31)); i++) ;
    msdc_wr(MSDC_INT, msdc_rd(MSDC_INT));                                  // clear pending status
    msdc_wr(MSDC_INTEN, 0);                                                // polling only
    // Slowest clock: divider mode, CKDIV 0xff -> source / 1020 (<= ~200 kHz). Clock always running.
    u32 cfg = msdc_rd(MSDC_CFG);
    cfg &= ~((0xffu << 8) | (3u << 16));
    cfg |= (0xffu << 8) | (1u << 1);
    msdc_wr(MSDC_CFG, cfg);
    for (int i = 0; i < 100000 && !(msdc_rd(MSDC_CFG) & (1u << 7)); i++) ;   // CKSTB: clock stable
    u32 sdc = msdc_rd(SDC_CFG);
    sdc &= ~(3u << 16);                                                    // 1-bit bus for now
    sdc |= (1u << 19) | (0xffu << 24);                                     // SDIO mode, long data timeout
    msdc_wr(SDC_CFG, sdc);
}

// Send one command; returns 0 and the 32-bit response, or a WIFI error number.
static int msdc_cmd(u32 opcode, u32 arg, u32 rsptype, u32 *resp) {
    for (int i = 0; i < 100000 && (msdc_rd(SDC_STS) & 3); i++) ;          // controller/command not busy
    msdc_wr(MSDC_INT, msdc_rd(MSDC_INT));
    msdc_wr(SDC_ARG, arg);
    msdc_wr(SDC_CMD, (opcode & 0x3f) | ((rsptype & 7) << 7));
    u32 st = 0;
    u64 hz = tick_hz(), t0 = ticks();
    for (u32 i = 0; i < 5000000; i++) {
        st = msdc_rd(MSDC_INT) & (INT_CMDRDY | INT_CMDTMO | INT_RSPCRC);
        if (st) break;
        if (hz && ticks() - t0 > hz / 10) break;                            // 100 ms
    }
    msdc_wr(MSDC_INT, st);
    if (resp) *resp = msdc_rd(SDC_RESP0);
    if (st & INT_CMDRDY) return 0;
    if (st & INT_RSPCRC) return 9;                                           // CRC error
    return 3;                                                                // timeout / no answer
}

// CMD52: read one byte of function `fn` register `reg`. Returns -1 on failure.
static int sdio_read_byte(u32 fn, u32 reg) {
    u32 r;
    if (msdc_cmd(52, (fn << 28) | (reg << 9), RSP_R1, &r)) return -1;
    if ((r >> 8) & 0xcb) return -1;              // R5 flags: COM_CRC, ILLEGAL_CMD, ERROR, FUNCTION_NUMBER, OUT_OF_RANGE
    return r & 0xff;
}

// CMD52 write of one byte. Returns 0 on success.
static int sdio_write_byte(u32 fn, u32 reg, u32 val) {
    u32 r;
    if (msdc_cmd(52, (1u << 31) | (fn << 28) | (reg << 9) | (val & 0xff), RSP_R1, &r)) return -1;
    return ((r >> 8) & 0xcb) ? -1 : 0;
}

// Walk a CIS chain at `cis` for the MANFID tuple (0x20). Returns 0 and vendor/device, or -1.
static int sdio_manfid(u32 cis, u32 *vendor, u32 *device) {
    for (u32 p = cis, n = 0; n < 64; n++) {
        int code = sdio_read_byte(0, p), len = sdio_read_byte(0, p + 1);
        if (code < 0 || len < 0) return -1;
        if (code == 0xff) return -1;             // end of chain, no MANFID
        if (code == 0x20 && len >= 4) {
            *vendor = sdio_read_byte(0, p + 2) | (sdio_read_byte(0, p + 3) << 8);
            *device = sdio_read_byte(0, p + 4) | (sdio_read_byte(0, p + 5) << 8);
            return 0;
        }
        p += 2 + (u32)len;
    }
    return -1;
}

static u32 cis_ptr(u32 base) {                   // 24-bit CIS pointer at base+9..11 (CCCR or FBR)
    return (u32)sdio_read_byte(0, base + 9) | ((u32)sdio_read_byte(0, base + 10) << 8) | ((u32)sdio_read_byte(0, base + 11) << 16);
}

// Wi-Fi step 2: power the chip and say hello over SDIO. Ends by printing the chip's vendor/device ID.
static int wifi_on(void) {
    puts("wifi on: bus supply, chip power, SDIO hello\n");
    u32 v = 0;
    // 1. Bus supply VGP3 at 3.3 V (what Linux uses first on this board), then enable it
    if (pmic_read(0x043a, &v) || pmic_write(0x043a, (v & ~0xe0u) | (7u << 5)) ||
        pmic_read(0x041e, &v) || pmic_write(0x041e, v | (1u << 15))) {
        err("WIFI", 1, "PMIC wrapper did not answer (can't read the Wi-Fi bus supply)");
        return 0;
    }
    u32 en = 0, vs = 0;
    pmic_read(0x041e, &en); pmic_read(0x043a, &vs);
    puts("  VGP3: con7 "); put_hex(en); puts(", con21 "); put_hex(vs); putc('\n');
    if (!((en >> 15) & 1) || ((vs >> 5) & 7) != 7) { err("WIFI", 2, "Wi-Fi bus supply VGP3 did not switch on at 3.3 V"); return 0; }
    // 2. Chip power on GPIO85. The DT's sdio_fixed_3v3 regulator has no "enable-active-high", so its
    //    enable is ACTIVE LOW (the line is also named WIFI_PDN: power-down). v0.11 drove it high and the
    //    chip stayed silent (WIFI-03 on the real hana). Try low first; if silent, power off and try high.
    u32 ocr = 0;
    int answered = 0;
    for (u32 level = 0; level < 2 && !answered; level++) {
        pin_mode(85, 0);
        gpio_out(85, level);
        delay_us(30000);                         // regulator + card power-up
        // 3. Controller at a safe slow clock; give the card its >= 74 clocks
        msdc_init_slow();
        delay_us(5000);
        msdc_cmd(0, 0, RSP_NONE, 0);             // GO_IDLE (harmless for SDIO)
        answered = msdc_cmd(5, 0, RSP_R3, &ocr) == 0;
        puts("  gpio85 "); puts(level ? "high" : "low"); puts(": CMD5 "); puts(answered ? "answered" : "no answer");
        puts(" (msdc3 cfg "); put_hex(msdc_rd(MSDC_CFG)); puts(", lines "); put_hex(msdc_rd(0x08)); puts(")\n");
        if (!answered && level == 0) { gpio_out(85, 1); delay_us(50000); }   // power off before the other try
    }
    if (!answered) { err("WIFI", 3, "chip did not answer CMD5 (not powered, or bus problem)"); return 0; }
    puts("  CMD5 ocr "); put_hex(ocr); puts(": "); put_dec((ocr >> 28) & 7); puts(" functions\n");
    u32 want = ocr & 0x300000;                   // 3.2-3.4 V
    if (!want) want = ocr & 0xffffff;
    u64 hz = tick_hz(), t0 = ticks();
    for (;;) {
        if (msdc_cmd(5, want, RSP_R3, &ocr)) { err("WIFI", 3, "chip did not answer CMD5 (not powered, or bus problem)"); return 0; }
        if (ocr >> 31) break;                    // ready
        if (hz && ticks() - t0 > hz) { err("WIFI", 4, "chip never became ready (CMD5 busy for 1 s)"); return 0; }
        delay_us(10000);
    }
    u32 r6 = 0, r1 = 0;
    if (msdc_cmd(3, 0, RSP_R1, &r6)) { errs("WIFI", 5, 1, "chip did not give an address (CMD3)"); return 0; }
    u32 rca = r6 >> 16;
    if (msdc_cmd(7, rca << 16, RSP_R1B, &r1)) { errs("WIFI", 5, 2, "chip could not be selected (CMD7)"); return 0; }
    puts("  ready, address "); put_hex(rca); putc('\n');
    // 5. Card ID from the common CIS. Marvell numbers each chip as card / Wi-Fi / Bluetooth:
    //    8897 = 0x912c / 0x912d / 0x912e (Linux sdio_ids.h lists 0x912d, 0x912e; 8797 is 0x9128/9/a).
    int cccr = sdio_read_byte(0, 0x00), sdrev = sdio_read_byte(0, 0x01);
    if (cccr < 0 || sdrev < 0) { errs("WIFI", 6, 1, "SDIO register read (CMD52) failed"); return 0; }
    u32 cis = cis_ptr(0x000), vendor = 0, device = 0;
    puts("  CCCR rev "); put_hex(cccr); puts(", SD rev "); put_hex(sdrev); puts(", CIS at "); put_hex(cis); putc('\n');
    if (sdio_manfid(cis, &vendor, &device)) { errs("WIFI", 6, 2, "SDIO register read (CMD52) failed"); return 0; }
    puts("  card: vendor "); put_hex(vendor); puts(", device "); put_hex(device);
    if (vendor != 0x02df || (device != 0x912c && device != 0x912d)) {
        puts("\n"); err("WIFI", 7, "unexpected SDIO vendor/device ID (not a Marvell 88W8897)"); return 0;
    }
    puts(": Marvell 88W8897\n");
    // 6. Function 1 = Wi-Fi: its own ID (FBR1 at 0x100), switch it on, wait until ready, block size 256
    u32 fv = 0, fd = 0;
    if (sdio_manfid(cis_ptr(0x100), &fv, &fd) == 0) {
        puts("  function 1: vendor "); put_hex(fv); puts(", device "); put_hex(fd);
        puts(fd == 0x912d ? " (Wi-Fi)\n" : " (?)\n");
    }
    int ioe = sdio_read_byte(0, 0x02);
    if (ioe < 0 || sdio_write_byte(0, 0x02, (u32)ioe | 2)) { errs("WIFI", 6, 3, "SDIO register read (CMD52) failed"); return 0; }
    int ready = 0;
    t0 = ticks();
    while (!ready) {
        int ior = sdio_read_byte(0, 0x03);
        if (ior >= 0 && (ior & 2)) ready = 1;
        else if (hz && ticks() - t0 > hz) break;
        else delay_us(10000);
    }
    if (!ready) { err("WIFI", 8, "Wi-Fi function 1 did not become ready after enabling"); return 0; }
    if (sdio_write_byte(0, 0x110, 0x00) || sdio_write_byte(0, 0x111, 0x01)) {   // FBR1 block size = 256
        errs("WIFI", 6, 4, "SDIO register read (CMD52) failed"); return 0;
    }
    int bs = sdio_read_byte(0, 0x110) | (sdio_read_byte(0, 0x111) << 8);
    puts("  function 1 on and ready, block size "); put_dec((u64)bs); puts(". Hello, Wi-Fi chip!\n");
    return 1;
}

// ---- v1.4: firmware upload (flow follows Linux mwifiex sdio.c mwifiex_prog_fw_w_helper) ----
extern const u8 fw_start[] __attribute__((visibility("hidden")));
extern const u8 fw_end[] __attribute__((visibility("hidden")));

#define MSDC_TXDATA 0x18
#define SDC_BLK_NUM 0x50
#define INT_XFER_COMPL (1u << 12)
#define INT_DATTMO     (1u << 14)
#define INT_DATCRC     (1u << 15)

static void msdc_set_clock(u32 ckdiv) {          // sclk = source / (4 * ckdiv), 1 = same as 0xff slow mode formula
    u32 cfg = msdc_rd(MSDC_CFG);
    cfg &= ~(0xffu << 8);
    cfg |= (ckdiv & 0xff) << 8;
    msdc_wr(MSDC_CFG, cfg);
    for (int i = 0; i < 100000 && !(msdc_rd(MSDC_CFG) & (1u << 7)); i++) ;
}

// CMD53 block write to function 1, fixed address (a FIFO-like port). len = blocks * 256. Returns 0 or error.
static int sdio_write_port(u32 addr, const u8 *data, u32 blocks) {
    u32 len = blocks * 256, r = 0;
    for (int i = 0; i < 100000 && (msdc_rd(SDC_STS) & 3); i++) ;
    msdc_wr(MSDC_FIFOCS, msdc_rd(MSDC_FIFOCS) | (1u << 31));
    for (int i = 0; i < 100000 && (msdc_rd(MSDC_FIFOCS) & (1u << 31)); i++) ;
    msdc_wr(MSDC_INT, msdc_rd(MSDC_INT));
    msdc_wr(SDC_BLK_NUM, blocks);
    // CMD53: write, function 1, block mode, fixed address, `blocks` blocks
    u32 arg = (1u << 31) | (1u << 28) | (1u << 27) | (addr << 9) | (blocks & 0x1ff);
    u32 raw = 53 | (RSP_R1 << 7) | (256u << 16) | (1u << 13) | (blocks > 1 ? (1u << 12) : (1u << 11));
    msdc_wr(SDC_ARG, arg);
    msdc_wr(SDC_CMD, raw);
    u32 fed = 0, st = 0;
    u64 hz = tick_hz(), t0 = ticks();
    for (;;) {
        u32 txcnt = (msdc_rd(MSDC_FIFOCS) >> 16) & 0xff;
        while (fed < len && txcnt <= 124) {          // FIFO holds 128 bytes
            u32 w = data[fed] | (data[fed + 1] << 8) | ((u32)data[fed + 2] << 16) | ((u32)data[fed + 3] << 24);
            msdc_wr(MSDC_TXDATA, w);
            fed += 4; txcnt += 4;
        }
        st = msdc_rd(MSDC_INT);
        if (st & (INT_CMDTMO | INT_RSPCRC | INT_DATTMO | INT_DATCRC)) break;
        if ((st & INT_XFER_COMPL) && fed >= len) break;
        if (hz && ticks() - t0 > hz / 2) { st |= INT_DATTMO; break; }
    }
    r = msdc_rd(SDC_RESP0);
    msdc_wr(MSDC_INT, st);
    if (st & (INT_CMDTMO | INT_RSPCRC | INT_DATTMO | INT_DATCRC)) return 1 + (int)((st >> 9) & 0x7f);
    return ((r >> 8) & 0xcb) ? 100 : 0;
}

#define MSDC_RXDATA 0x1c

// CMD53 block read from function 1, fixed address. len = blocks * 256 bytes into out. Returns 0 or error.
static u32 rd_got, rd_hw_timeout;        // last read: bytes that arrived, and whether the controller's own data timeout fired
static u32 rd_dtoc;                       // data timeout counter for multi-block reads (0 = leave the controller's value; 1..255 = units of ~1M bus clocks)
static int sdio_read_port1(u32 addr, u8 *out, u32 blocks) {
    u32 len = blocks * 256, got = 0;
    if (rd_dtoc) msdc_wr(SDC_CFG, (msdc_rd(SDC_CFG) & 0x00ffffffu) | ((rd_dtoc & 0xff) << 24));
    u32 save_div = (msdc_rd(MSDC_CFG) >> 8) & 0xff;

    for (int i = 0; i < 100000 && (msdc_rd(SDC_STS) & 3); i++) ;
    msdc_wr(MSDC_FIFOCS, msdc_rd(MSDC_FIFOCS) | (1u << 31));
    for (int i = 0; i < 100000 && (msdc_rd(MSDC_FIFOCS) & (1u << 31)); i++) ;
    msdc_wr(MSDC_INT, msdc_rd(MSDC_INT));
    msdc_wr(SDC_BLK_NUM, blocks);
    u32 arg = (1u << 28) | (1u << 27) | (addr << 9) | (blocks & 0x1ff);      // read, function 1, block mode, fixed address
    u32 raw = 53 | (RSP_R1 << 7) | (256u << 16) | (blocks > 1 ? (1u << 12) : (1u << 11));
    msdc_wr(SDC_ARG, arg);
    msdc_wr(SDC_CMD, raw);
    u32 st = 0;
    u64 hz = tick_hz(), t0 = ticks();
    for (;;) {
        u32 cnt = msdc_rd(MSDC_FIFOCS) & 0xff;                               // bytes waiting in the RX FIFO
        while (cnt >= 4 && got < len) {
            u32 w = msdc_rd(MSDC_RXDATA);
            out[got] = w; out[got + 1] = w >> 8; out[got + 2] = w >> 16; out[got + 3] = w >> 24;
            got += 4; cnt -= 4;
        }
        st = msdc_rd(MSDC_INT);
        if (st & (INT_CMDTMO | INT_RSPCRC | INT_DATTMO | INT_DATCRC)) break;
        if ((st & INT_XFER_COMPL) && got >= len) break;
        if (hz && ticks() - t0 > hz / 2) { st |= INT_DATTMO; break; }
    }
    rd_got = got; rd_hw_timeout = (st & INT_DATTMO) && !(hz && ticks() - t0 > hz / 2);
    msdc_wr(MSDC_INT, st);
    if (st & (INT_CMDTMO | INT_RSPCRC | INT_DATTMO | INT_DATCRC)) return 1 + (int)((st >> 9) & 0x7f);
    return 0;
}

// Multi-block PIO reads never complete on this controller (v1.20-v1.24: data timeout on every 7-block read, while
// single-block reads always work). The port is a FIFO, so read a packet one 256-byte block at a time instead.
static u32 rd_total;                     // bytes of the current packet that arrived before a failure
static u32 mb_err, mb_got, mb_hw, mb_fail;   // last failed multi-block read: controller error, bytes that arrived, hardware timeout?
static u32 rd_chunk = 512;               // bytes per byte-mode transfer (4..512, multiple of 4)
static u32 rd_div;                       // 0 = leave the bus clock alone for big reads, else the clock divider to use (wifidiv cycles it)
static u32 rd_gap_us = 0;             // pause between pieces of one packet (see sdio_read_port)
static int wifi_verbose;                 // 1 = also print the long scan diagnostics (command: wifiv)
static int wifi_read_bytes = 1;          // 1 = read a packet in BYTE mode (up to 512 bytes per transfer; clean on hana), 0 = as 256-byte blocks (garbled results on hana)
static u32 dbg_starts[8], dbg_blocks, dbg_taken;   // first 4 bytes of each 256-byte block of the first big packet (scan diagnostics)

// CMD53 BYTE-mode read from function 1, fixed address: one transfer of len bytes (4..512, multiple of 4), no block counting.
static int sdio_read_bytes_once(u32 addr, u8 *out, u32 len) {
    u32 got = 0;
    for (int i = 0; i < 100000 && (msdc_rd(SDC_STS) & 3); i++) ;
    msdc_wr(MSDC_FIFOCS, msdc_rd(MSDC_FIFOCS) | (1u << 31));
    for (int i = 0; i < 100000 && (msdc_rd(MSDC_FIFOCS) & (1u << 31)); i++) ;
    msdc_wr(MSDC_INT, msdc_rd(MSDC_INT));
    msdc_wr(SDC_BLK_NUM, 1);
    u32 arg = (1u << 28) | (addr << 9) | (len & 0x1ff);                      // read, function 1, BYTE mode (bit 27 = 0), fixed address, count = len (512 -> 0)
    u32 raw = 53 | (RSP_R1 << 7) | ((len & 0xfff) << 16) | (1u << 11);       // one "block" of len bytes
    msdc_wr(SDC_ARG, arg);
    msdc_wr(SDC_CMD, raw);
    u32 st = 0;
    u64 hz = tick_hz(), t0 = ticks();
    for (;;) {
        u32 cnt = RAWR(MSDC_FIFOCS) & 0xff;
        while (cnt >= 4 && got < len) {
            u32 w = RAWR(MSDC_RXDATA);
            out[got] = w; out[got + 1] = w >> 8; out[got + 2] = w >> 16; out[got + 3] = w >> 24;
            got += 4; cnt -= 4;
        }
        st = RAWR(MSDC_INT);
        if (st & (INT_CMDTMO | INT_RSPCRC | INT_DATTMO | INT_DATCRC)) break;
        if ((st & INT_XFER_COMPL) && got >= len) break;
        if (hz && ticks() - t0 > hz / 2) { st |= INT_DATTMO; break; }
    }
    rd_got = got;
    msdc_wr(MSDC_INT, st);
    if (st & (INT_CMDTMO | INT_RSPCRC | INT_DATTMO | INT_DATCRC)) return 1 + (int)((st >> 9) & 0x7f);
    return 0;
}

// Multi-block PIO reads never complete on this controller (v1.20+: data timeout on every 7-block read, while
// single-block reads always work). So read a packet piece by piece instead: 256-byte blocks, or byte-mode transfers.
static int sdio_read_port(u32 addr, u8 *out, u32 blocks) {
    rd_total = 0;
    if (wifi_read_bytes == 1) {
        u32 len = blocks * 256;
        int fail = 0;
        u32 save_div = (msdc_rd(MSDC_CFG) >> 8) & 0xff;
        if (rd_div && len > 256) msdc_set_clock(rd_div);
        for (u32 off = 0; off < len && !fail; off += rd_chunk) {              // 512 bytes per transfer by default; wifitry tries other sizes
            u32 n = len - off < rd_chunk ? len - off : rd_chunk;
            int e = sdio_read_bytes_once(addr, out + off, n);
            if (e) { fail = e; break; }
            rd_total += n;
            if (off + n < len) delay_us(rd_gap_us);                          // v1.36 on hana: bytes after the first 1024 of a packet were garbage; give the chip time between pieces
        }
        if (rd_div && len > 256) msdc_set_clock(save_div);
        if (fail) return fail;
    } else if (wifi_read_bytes == 2) {                                         // EXPERIMENT: the whole packet in ONE multi-block transfer
        int e = sdio_read_port1(addr, out, blocks);
        rd_total = rd_got;
        if (e) { mb_err = (u32)e; mb_got = rd_got; mb_hw = rd_hw_timeout; mb_fail++; return e; }
    } else {
        for (u32 b = 0; b < blocks; b++) {
            int e = sdio_read_port1(addr, out + b * 256, 1);
            if (e) return e;
            rd_total += 256;
        }
    }
    if (blocks > 1 && !dbg_taken) {                                      // remember how each block begins (diagnostics)
        dbg_taken = 1; dbg_blocks = blocks > 8 ? 8 : blocks;
        for (u32 b = 0; b < dbg_blocks; b++) dbg_starts[b] = out[b * 256] | (out[b * 256 + 1] << 8) | ((u32)out[b * 256 + 2] << 16) | ((u32)out[b * 256 + 3] << 24);
    }
    return 0;
}
static int fn1_rd(u32 reg) { return sdio_read_byte(1, reg); }
static int fn1_wr(u32 reg, u32 v) { return sdio_write_byte(1, reg, v); }

static int wifi_fw(void) {
    if (!wifi_on()) return 0;
    u32 fwlen = (u32)(fw_end - fw_start);
    puts("firmware: "); put_dec(fwlen); puts(" bytes built in; going 4-bit and faster\n");
    // faster bus: 4-bit (CCCR bus interface control), controller clock /16 of its source
    int bic = sdio_read_byte(0, 0x07);
    if (bic < 0 || sdio_write_byte(0, 0x07, ((u32)bic & ~3u) | 2)) { errs("WIFI", 6, 5, "SDIO register read (CMD52) failed"); return 0; }
    msdc_wr(SDC_CFG, (msdc_rd(SDC_CFG) & ~(3u << 16)) | (1u << 16));
    msdc_set_clock(4);
    if (sdio_read_byte(0, 0x00) < 0) { errs("WIFI", 9, 1, "firmware upload failed: bus unreliable at the faster speed"); return 0; }
    // "new mode": data goes through one memory port at 0x10000 (reg numbers: Linux mwifiex_reg_sd8897)
    int v;
    fn1_rd(0x03);                                                        // acknowledge the bootloader's first interrupt
    if ((v = fn1_rd(0x01)) < 0 || fn1_wr(0x01, (u32)v | 0xff)) goto bad; // interrupt status: reset on read
    if ((v = fn1_rd(0xcc)) < 0 || fn1_wr(0xcc, (u32)v | 0x10)) goto bad; // ready bits auto re-enable
    if ((v = fn1_rd(0xcd)) < 0 || fn1_wr(0xcd, (u32)v | 1)) goto bad;   // CMD53 new mode
    if (fn1_wr(0x02, 0xc3)) goto bad;                                    // host interrupt mask: data up/down + command port up/down (Linux host_int_enable)
    { int ien = sdio_read_byte(0, 0x04); if (ien < 0 || sdio_write_byte(0, 0x04, (u32)ien | 0x03)) goto bad; }   // SDIO interrupt enable: master + function 1
    if ((v = fn1_rd(0xb8)) < 0 || fn1_wr(0xb8, (u32)v | 4)) goto bad;   // cmd port: read length from register
    if ((v = fn1_rd(0xb9)) < 0 || fn1_wr(0xb9, (u32)v | 1)) goto bad;   // cmd port: auto reset
    // Like Linux (mwifiex_dnld_fw): if the chip's firmware is already running there is nothing to upload.
    // On the real hana the chip comes up running (v1.6: status 0xfedc, but it never asks for a download).
    {
        int a0 = fn1_rd(0xc0), a1 = fn1_rd(0xc1);
        if (a0 >= 0 && a1 >= 0 && (((u32)a1 << 8) | (u32)a0) == 0xfedc) {
            puts("  firmware status 0xfedc: the chip's Wi-Fi firmware is already RUNNING (nothing to upload).\n");
            return 1;
        }
    }
    static u8 buf[2312 + 256];
    u32 offset = 0, blocks = 0, retries = 0, last_kb = 0;
    for (;;) {
        // wait for "card io ready" + "download ready" (status register 0x50, bits 3 and 0)
        u32 tries; int cs = 0;
        for (tries = 0; tries < 2000; tries++) {
            cs = fn1_rd(0x50);
            if (cs >= 0 && (cs & 9) == 9) break;
            delay_us(500);
        }
        if (tries == 2000) {
            puts("  chip status 0x50 = "); put_hex((u32)cs); puts(" (need bits 0 and 3); function 1 registers:\n");
            static const u8 dumpregs[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x50, 0x60, 0x61, 0xb4, 0xb8, 0xb9, 0xc0, 0xc1, 0xcc, 0xcd};
            for (u32 i = 0; i < sizeof dumpregs; i++) {
                puts(" "); put_hex(dumpregs[i]); puts("="); put_hex((u32)fn1_rd(dumpregs[i]));
                if (i % 5 == 4) putc('\n');
            }
            errs("WIFI", 9, 2, "firmware upload failed: chip stopped asking for data");
            return 0;
        }
        if (offset >= fwlen) break;
        // how many bytes does the chip want next? (two 8-bit registers 0x60/0x61)
        u32 len = 0;
        for (tries = 0; tries < 2000 && !len; tries++) {
            int b0 = fn1_rd(0x60), b1 = fn1_rd(0x61);
            if (b0 < 0 || b1 < 0) goto bad;
            len = ((u32)b1 << 8) | (u32)b0;
            if (!len) delay_us(200);
        }
        if (!len) break;                         // chip wants nothing more
        if (len > 2312) { err("WIFI", 10, "firmware upload failed: chip asked for an impossible length"); return 0; }
        u32 txlen = len;
        if (len & 1) {                           // odd length = "resend the last block"
            if (++retries > 20) { errs("WIFI", 9, 4, "firmware upload failed: too many resend requests"); return 0; }
            txlen = 0;
        } else {
            retries = 0;
            if (fwlen - offset < txlen) txlen = fwlen - offset;
            blocks = (txlen + 255) / 256;
            for (u32 i = 0; i < blocks * 256; i++) buf[i] = i < txlen ? fw_start[offset + i] : 0;
        }
        int w = sdio_write_port(0x10000, buf, blocks);
        if (w) {
            puts("  block write failed: "); put_dec((u64)w); puts(" at offset "); put_dec(offset); putc('\n');
            errs("WIFI", 9, 3, "firmware upload failed: data write to the chip failed");
            return 0;
        }
        offset += txlen;
        if (offset / 131072 != last_kb) { last_kb = offset / 131072; puts("  sent "); put_dec(offset / 1024); puts(" KB\n"); }
    }
    puts("  upload done ("); put_dec(offset); puts(" bytes). waiting for the firmware to start...\n");
    u64 hz = tick_hz(), t0 = ticks();
    int s0 = 0, s1 = 0;
    for (;;) {
        s0 = fn1_rd(0xc0); s1 = fn1_rd(0xc1);
        if (s0 >= 0 && s1 >= 0 && (((u32)s1 << 8) | (u32)s0) == 0xfedc) break;
        if (hz && ticks() - t0 > hz * 3) {
            puts("  firmware status "); put_hex(((u32)(s1 & 0xff) << 8) | (u32)(s0 & 0xff)); putc('\n');
            err("WIFI", 11, "firmware uploaded but did not report ready (0xfedc)");
            return 0;
        }
        delay_us(10000);
    }
    puts("  firmware status 0xfedc: the Wi-Fi firmware is RUNNING.\n");
    return 1;
bad:
    errs("WIFI", 6, 6, "SDIO register read (CMD52) failed");
    return 0;
}

// ---- v1.8: host commands to the running Wi-Fi firmware (Linux mwifiex cmdevt + sdio command port) ----
// Packet = 4-byte SDIO header {u16 length, u16 type: 1 command, 3 event} + command header
// {u16 command, u16 size, u16 sequence, u16 result} + body. Responses come back as command|0x8000.
// New-mode command port = MEM_PORT | CMD_PORT_SLCT = 0x18000, fixed address, block mode.
#define WCMD_PORT 0x18000
#define UP_LD_CMD_PORT_INT 0x40
// Is a packet waiting on the command port? The status bit (0x03, read-to-clear) is easy to miss, so also look at the
// packet-length registers 0xb4/0xb5 (v1.15 on hana: no status bit, but a 0x700-byte packet was sitting there).
static u32 wifi_last_len;
static u32 scan_done, scan_events;
static u32 ev_log, ev_sets[12], ev_more[12], ev_size[12], ev_len[12], ev_stop[12], ev_stype[12], ev_slen[12], ev_sleft[12], recs_seen;
static u8 raw_rec[40]; static u32 raw_len, raw_taken;   // raw bytes of the first scan record (diagnostics)        // set by the scan event handler (defined further down)
static u32 w1_err, w1_tries;              // last failed command write: controller error, attempts made
static u32 w3_why, w3_a, w3_b;           // why a command answer was 'malformed': 1 = bad length (a), 2 = block read failed with error a for length b
static u8 wifi_dbg[16];                   // first bytes of the last unexpected packet
static u32 wifi_dbg_n;                  // how many unexpected packets were skipped
static int cmd_packet_waiting(int st);
static u8 wbuf[2312 + 256 + 256];
static u16 wifi_seq;
static void (*wifi_event_hook)(const u8 *ev, u32 len);   // called for firmware events seen while waiting
static u32 wifi_cmd_ms = 1000;          // how long to wait for an answer (scans need longer)

static int wifi_dn_seen;                  // the chip raised 'command port ready' (0x80) since our last command
static int wifi_len_sticky;              // 1 = the chip does NOT clear the length register after we read a packet
static void after_packet_read(u32 rx) {
    int l0 = fn1_rd(0xb4), l1 = fn1_rd(0xb5);
    u32 now = (l0 < 0 || l1 < 0) ? rx : (((u32)l1 << 8) | (u32)l0);
    if (now == 0) { wifi_len_sticky = 0; wifi_last_len = 0; }       // cleared: any non-zero length is a new packet
    else { wifi_len_sticky = 1; wifi_last_len = rx; }               // still set: compare against what we read
}static int cmd_packet_waiting(int st) {
    if (st >= 0 && (st & 0x80)) wifi_dn_seen = 1;
    if (st >= 0 && (st & UP_LD_CMD_PORT_INT)) return 1;
    int l0 = fn1_rd(0xb4), l1 = fn1_rd(0xb5);
    if (l0 < 0 || l1 < 0) return 0;
    u32 len = ((u32)l1 << 8) | (u32)l0;
    if (!len) { wifi_last_len = 0; return 0; }
    if (!wifi_len_sticky) return 1;                                // cleared-after-read chip: non-zero means a new packet
    return len != wifi_last_len;                                   // a length we have not read yet
}
static void put16(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; }
static u32 get16(const u8 *p) { return p[0] | (p[1] << 8); }

// Send host command `cmd` with `blen` body bytes; copy up to rmax response body bytes to resp.
// Returns the firmware result (0 = ok, >0 firmware error) or a negative wave-os error: -1 send failed,
// -2 no response, -3 malformed response.
static int wifi_cmd_raw(u32 cmd, const u8 *body, u32 blen, u8 *resp, u32 rmax, u32 *rlen) {
    u32 total = 4 + 8 + blen;
    if (total > 2312) return -1;
    wifi_dbg_n = 0;
    for (u32 i = 0; i < 256 * ((total + 255) / 256); i++) wbuf[i] = 0;
    put16(wbuf, total); put16(wbuf + 2, 1);                       // SDIO header: length, type = command
    put16(wbuf + 4, cmd); put16(wbuf + 6, 8 + blen); put16(wbuf + 8, ++wifi_seq); put16(wbuf + 10, 0);
    for (u32 i = 0; i < blen; i++) wbuf[12 + i] = body[i];
    // Linux sends the next command only after the chip raised 'command port ready' (0x80) for the last one.
    for (u32 i = 0; i < 100 && wifi_seq > 1 && !wifi_dn_seen; i++) { int s = fn1_rd(0x03); if (s >= 0 && (s & 0x80)) wifi_dn_seen = 1; if (s >= 0 && (s & 0x40)) break; delay_us(500); }
    delay_us(opt_on(OPT_FASTGAP) ? 1000 : 5000);
    fn1_rd(0x03);                                                  // clear stale interrupt bits (reset on read)
    wifi_dn_seen = 0;
    // A failed write can leave the controller's data path stuck: reset it and retry (up to 3 times).
    for (u32 attempt = 1; ; attempt++) {
        int we = sdio_write_port(WCMD_PORT, wbuf, (total + 255) / 256);
        if (!we) break;
        w1_err = (u32)we; w1_tries = attempt;
        if (attempt >= 3) return -1;
        msdc_wr(MSDC_CFG, msdc_rd(MSDC_CFG) | (1u << 2));                         // controller reset (keeps clock/bus settings)
        for (int i = 0; i < 100000 && (msdc_rd(MSDC_CFG) & (1u << 2)); i++) ;
        msdc_wr(MSDC_FIFOCS, msdc_rd(MSDC_FIFOCS) | (1u << 31));
        for (int i = 0; i < 100000 && (msdc_rd(MSDC_FIFOCS) & (1u << 31)); i++) ;
        msdc_wr(MSDC_INT, msdc_rd(MSDC_INT));
        delay_us(5000);
    }
    u64 hz = tick_hz(), t0 = ticks();
    for (;;) {
        int st = fn1_rd(0x03);                                     // host interrupt status
        if (cmd_packet_waiting(st)) {
            int l0 = fn1_rd(0xb4), l1 = fn1_rd(0xb5);              // length of the waiting packet
            if (l0 < 0 || l1 < 0) return -2;
            u32 rx = ((u32)l1 << 8) | (u32)l0;
            wifi_last_len = rx;
            u32 blocks = (rx + 255) / 256;
            if (rx <= 4) { delay_us(200); continue; }                  // flag raised a moment before the length register updated: look again
            if (blocks * 256 > sizeof wbuf) { w3_why = 1; w3_a = rx; return -3; }
            { int re = sdio_read_port(WCMD_PORT, wbuf, blocks); if (re) { w3_why = 2; w3_a = (u32)re; w3_b = rx; return -3; } }
            after_packet_read(rx);
            u32 type = get16(wbuf + 2);
            if (type == 3) {                                       // an event: hand it on, keep waiting
                if (wifi_event_hook) wifi_event_hook(wbuf + 4, get16(wbuf) > 4 ? get16(wbuf) - 4 : 0);
                if (cmd == 0x0107 && scan_done) { if (rlen) *rlen = 0; return 0; }   // the last scan report arrived: the scan is complete (the chip may send no separate reply)
                continue;
            }
            if (type != 1 || (get16(wbuf + 4) & 0x7fff) != cmd) {      // not the answer to this command: remember what it was, keep waiting
                for (u32 i = 0; i < 16; i++) wifi_dbg[i] = wbuf[i];
                wifi_dbg_n++;
                continue;
            }
            u32 size = get16(wbuf + 6);
            u32 body_len = size > 8 ? size - 8 : 0;
            if (rlen) *rlen = body_len;
            for (u32 i = 0; i < body_len && i < rmax; i++) resp[i] = wbuf[12 + i];
            return (int)get16(wbuf + 10);
        }
        if (cmd == 0x0107 && scan_events && hz && ticks() - t0 > hz * 2) { if (rlen) *rlen = 0; return 0; }   // events came in but the 'last report' flag was missed: good enough after 2 s
        if (hz && ticks() - t0 > hz / 1000 * wifi_cmd_ms) {        // normally 1 s
            {   // a packet may still be waiting: read it anyway and show how it begins
                int q0 = fn1_rd(0xb4), q1 = fn1_rd(0xb5);
                u32 qlen = (q0 < 0 || q1 < 0) ? 0 : (((u32)q1 << 8) | (u32)q0), qb = (qlen + 255) / 256;
                if (qlen > 4 && qb * 256 <= sizeof wbuf && !sdio_read_port(WCMD_PORT, wbuf, qb)) {
                    puts("  waiting packet ("); put_dec(qlen); puts(" bytes) began:");
                    for (u32 i = 0; i < 16; i++) { putc(' '); put_hex(wbuf[i]); }
                    putc('\n');
                }
            }            if (wifi_dbg_n) { puts("  skipped "); put_dec(wifi_dbg_n); puts(" unexpected packet(s); the last began:"); for (u32 i = 0; i < 16; i++) { putc(' '); put_hex(wifi_dbg[i]); } putc('\n'); }
            puts("  no answer. status 0x03 = "); put_hex((u32)st); puts(", mask 0x02 = "); put_hex((u32)fn1_rd(0x02));
            puts(", 0x50 = "); put_hex((u32)fn1_rd(0x50)); puts(", cmd length 0xb4/5 = "); put_hex((u32)fn1_rd(0xb4)); puts("/"); put_hex((u32)fn1_rd(0xb5)); putc('\n');
            return -2;
        }
        delay_us(500);
    }
}

static int wifi_cmd(u16 cmd, const u8 *body, u32 blen, u8 *resp, u32 rmax, u32 *rlen) {   // timed wrapper: scan commands and all others are counted separately
    u64 t = prof_start();
    int rc = wifi_cmd_raw(cmd, body, blen, resp, rmax, rlen);
    prof_stop(cmd == 0x0107 ? "scan" : "cmd", t);
    return rc;
}

static void put_mac(const u8 *m) {
    for (u32 i = 0; i < 6; i++) { putc("0123456789abcdef"[m[i] >> 4]); putc("0123456789abcdef"[m[i] & 15]); if (i < 5) putc(':'); }
}

static int wifi_cmd_err(const char *name, int r, u32 sub_base) {
    (void)sub_base;
    if (r == 0) return 1;
    puts("  command "); puts(name); puts(": ");
    if (r > 0) { puts("firmware answered with error "); put_dec((u64)r); putc('\n'); errs("WIFI", 13, 1, "Wi-Fi firmware rejected a command"); }
    else if (r == -1) { puts("send failed (controller error "); put_dec(w1_err); puts(" after "); put_dec(w1_tries); puts(" tries)\n"); errs("WIFI", 12, 1, "could not send a command to the Wi-Fi firmware"); }
    else if (r == -2) { puts("no answer within 1 s\n"); errs("WIFI", 12, 2, "the Wi-Fi firmware did not answer a command"); }
    else {
        puts("malformed answer");
        if (w3_why == 1) { puts(": packet length "); put_dec(w3_a); puts(" is unusable"); }
        else if (w3_why == 2) { puts(": reading the "); put_dec(w3_b); puts("-byte packet failed (controller error "); put_dec(w3_a); puts("), "); put_dec(rd_total + rd_got); puts(" bytes arrived, "); puts(rd_hw_timeout ? "controller timeout" : "our own 0.5 s timeout"); }
        putc('\n'); errs("WIFI", 12, 3, "the Wi-Fi firmware's answer was not understood"); }
    return 0;
}

// Power-cycle the Wi-Fi chip: chip enable off (GPIO85 is active low, so high = off), bus supply off, wait, and
// let wifi_on() power it back up. The chip then starts from its boot ROM and waits for our firmware upload.
// (Needed because on the real hana the chip's firmware is left running by ChromeOS and ignores commands: v1.9.)
static void wifi_power_cycle(void) {
    puts("power-cycling the Wi-Fi chip so its firmware starts fresh...\n");
    pin_mode(85, 0);
    gpio_out(85, 1);                                     // chip off
    u32 v = 0;
    if (!pmic_read(0x041e, &v)) pmic_write(0x041e, v & ~(1u << 15));   // bus supply VGP3 off
    delay_us(300000);                                    // let the chip's supplies fully discharge
}

// Wi-Fi step: make sure the chip is up, then run the firmware's init commands and print its MAC address.
static u32 hw_n_cap, hw_mcs;             // the chip's own 802.11n capability word and MCS support, from GET_HW_SPEC
static int hw_rc;                       // how the first command failed (for the final message)
static u8 wifi_mac[6];
static int wifi_ready;                  // FUNC_INIT + GET_HW_SPEC done in this boot
static int wifi_hw_spec(void) {
    u8 r[96]; u32 n = 0;
    puts("talking to the Wi-Fi firmware...\n");
    int rc = wifi_cmd(0x00a9, 0, 0, r, sizeof r, &n);
    if (rc == -2 || rc == -1) { hw_rc = rc; return -2; }  // silent or refused: caller power-cycles and retries
    if (!wifi_cmd_err("FUNC_INIT", rc, 0)) return 0;
    static const u8 zero[63];                            // GET_HW_SPEC request: all-zero spec
    if (!wifi_cmd_err("GET_HW_SPEC", wifi_cmd(0x0003, zero, sizeof zero, r, sizeof r, &n), 0)) return 0;
    if (n < 22) { errs("WIFI", 12, 3, "the Wi-Fi firmware's answer was not understood"); return 0; }
    for (u32 i = 0; i < 6; i++) wifi_mac[i] = r[8 + i];
    if (n >= 44) { hw_n_cap = r[38] | (r[39] << 8) | ((u32)r[40] << 16) | ((u32)r[41] << 24); hw_mcs = r[42]; }
    u32 fwver = r[18] | (r[19] << 8) | ((u32)r[20] << 16) | ((u32)r[21] << 24);   // hw spec: mac at 8, region 14, antennas 16, release 18
    puts("  Wi-Fi MAC address "); put_mac(wifi_mac); putc('\n');
    puts("  firmware release "); put_hex(fwver); puts(", antennas "); put_dec(get16(r + 16)); puts(", region "); put_hex(get16(r + 14)); putc('\n');
    puts("  the Wi-Fi firmware answers our commands.\n");
    wifi_ready = 1;
    return 1;
}

// Always start the chip from cold: its firmware state after a previous run (unread packets, a half-finished scan)
// made the first command fail (v1.21: FUNC_INIT send error 33). Power-cycle, upload our firmware, then talk.
static int wifi_cold_start(void) {
    u64 t = prof_start();
    wifi_power_cycle();
    prof_stop("pwr", t);
    wifi_seq = 0; wifi_dn_seen = 0; wifi_last_len = 0; wifi_dbg_n = 0;
    t = prof_start();
    int ok = wifi_fw();
    prof_stop("fw", t);
    return ok;
}

static int wifi_init(void) {
    if (opt_on(OPT_WARM) && wifi_ready) return 1;                    // optimization "warm": the chip is already up and ours
    for (int attempt = 1; attempt <= 2; attempt++) {
        if (!wifi_cold_start()) return 0;
        int r = wifi_hw_spec();
        if (r != -2) return r == 1;
        if (attempt == 1) puts("  the chip did not take the first command: restarting it once more.\n");
    }
    wifi_cmd_err("FUNC_INIT", hw_rc, 0);
    return 0;
}
// ---- v1.11: scan for networks (legacy scan command 0x0006; answer layout: Linux mwifiex scan.c) ----
// Extended scan (what Linux uses on this chip): command 0x0107 = {u32 reserved, TLVs}. The command's answer carries
// no results; they come as events (id 0x58) holding, per access point, a BSS_SCAN_RSP TLV {bssid, frame body} and a
// BSS_SCAN_INFO TLV {rssi, ..., channel}.
struct ap { u8 bssid[6]; char ssid[33]; u8 chan, sec; int rssi; u8 ich, rad;   // ich/rad: channel and band as the firmware reported them (scan info block)
            u16 cap, bint; u8 nrates, rates[16], rsn_len, rsn[50]; };           // what joining needs from the beacon: capabilities, beacon interval, supported rates, the RSN (WPA2) element
static struct ap aps[32];
static u32 nap, scan_events, scan_done, scan_bytes;

static u8 evraw[1536]; static u32 evraw_len, evraw_taken;       // the first scan report as it arrived (wifi5 dumps its structure)
static void scan_event(const u8 *ev, u32 len) {
    if (len < 11 || get16(ev) != 0x58) return;
    if (!evraw_taken) { evraw_taken = 1; evraw_len = len < sizeof evraw ? len : sizeof evraw; for (u32 q = 0; q < evraw_len; q++) evraw[q] = ev[q]; }
    scan_events++; scan_bytes += len;
    if (ev_log < 12) { ev_sets[ev_log] = ev[10]; ev_more[ev_log] = ev[4]; ev_size[ev_log] = get16(ev + 8); ev_len[ev_log] = len; ev_log++; }
    u32 size = get16(ev + 8), left = len - 11 < size ? len - 11 : size;
    const u8 *t = ev + 11;
    u32 left0 = left;
    struct ap *cur = 0;
    while (left >= 4) {
        u32 type = get16(t), tl = get16(t + 2);
        if (left < 4 + tl) break;
        if (type == 0x0156 && tl >= 6 + 12) {                      // BSS_SCAN_RSP: bssid[6] + frame body (ts8, interval2, cap2, IEs)
            recs_seen++;
            if (!raw_taken) { raw_taken = 1; raw_len = tl; for (u32 q = 0; q < 40 && q < tl + 4; q++) raw_rec[q] = t[q]; }
            u32 k;
            for (k = 0; k < nap; k++) { u32 same = 1; for (u32 m = 0; m < 6; m++) if (aps[k].bssid[m] != t[4 + m]) same = 0; if (same) break; }
            if (k == nap && nap < 32) nap++;
            cur = k < 32 ? &aps[k] : 0;
            if (cur) {
                for (u32 m = 0; m < 6; m++) cur->bssid[m] = t[4 + m];
                cur->ssid[0] = 0; cur->chan = 0; cur->sec = 0; cur->rssi = 0; cur->ich = 0; cur->rad = 0xff;
                cur->nrates = 0; cur->rsn_len = 0; cur->bint = (u16)get16(t + 4 + 6 + 8);
                u32 cap = get16(t + 4 + 6 + 10); cur->cap = (u16)cap;
                const u8 *ie = t + 4 + 6 + 12, *end = t + 4 + tl;
                u32 ht_chan = 0;
                while (ie + 2 <= end && ie + 2 + ie[1] <= end) {
                    u32 id = ie[0], l = ie[1];
                    if (id == 0) { u32 q; for (q = 0; q < l && q < 32; q++) cur->ssid[q] = ie[2 + q] >= 32 && ie[2 + q] < 127 ? ie[2 + q] : '?'; cur->ssid[q] = 0; }
                    else if (id == 3 && l >= 1) cur->chan = ie[2];
                    else if (id == 1 || id == 50) { for (u32 q = 0; q < l; q++) if (cur->nrates < 16) cur->rates[cur->nrates++] = ie[2 + q]; }   // supported + extended rates
                    else if (id == 61 && l >= 1) ht_chan = ie[2];                // HT operation: primary channel (5 GHz beacons have no DS parameter element)
                    else if (id == 48) { cur->sec = 2; if (l + 2 <= sizeof cur->rsn) { for (u32 q = 0; q < l + 2; q++) cur->rsn[q] = ie[q]; cur->rsn_len = (u8)(l + 2); } }   // keep the whole RSN element for joining
                    else if (id == 221 && l >= 4 && ie[2] == 0x00 && ie[3] == 0x50 && ie[4] == 0xf2 && ie[5] == 1 && cur->sec < 1) cur->sec = 1;
                    ie += 2 + l;
                }
                if (!cur->chan && ht_chan) cur->chan = (u8)ht_chan;
                if (!cur->sec && (cap & 0x10)) cur->sec = 3;
            }
        } else if (type == 0x0157 && tl >= 7 && cur) {             // BSS_SCAN_INFO: rssi(s16) anpi(2) cca(1) radio(1) channel(1)
            cur->rssi = (short)get16(t + 4);
            cur->rad = t[4 + 5]; cur->ich = t[4 + 6];
            if (!cur->chan) cur->chan = t[4 + 6];
        }
        t += 4 + tl; left -= 4 + tl;
    }
    if (ev_log > 0 && ev_log <= 12) {
        ev_stop[ev_log - 1] = left0 - left;                                       // how far the TLV walk got
        ev_stype[ev_log - 1] = left >= 4 ? get16(t) : 0xffff;                    // the block it stopped at: type,
        ev_slen[ev_log - 1] = left >= 4 ? get16(t + 2) : 0;                      // its declared length,
        ev_sleft[ev_log - 1] = left;                                              // and how many bytes were left
    }
    if (ev[4] == 0) scan_done = 1;                                  // more_event == 0: that was the last report
}

// Wait up to `ms` for events on the command port, handing each to the hook, until a scan finishes.
static void wifi_poll_events(u32 ms) {
    u64 hz = tick_hz(), t0 = ticks();
    u64 done_at = 0;                                                // when the last-report flag was seen: keep listening a little longer for stragglers
    while (hz && ticks() - t0 < hz / 1000 * ms) {
        if (scan_done && !done_at) done_at = ticks();
        if (done_at && ticks() - done_at > hz * 3 / 2) break;
        int st = fn1_rd(0x03);
        if (cmd_packet_waiting(st)) {
            int l0 = fn1_rd(0xb4), l1 = fn1_rd(0xb5);
            u32 rx = ((u32)(l1 < 0 ? 0 : l1) << 8) | (u32)(l0 < 0 ? 0 : l0), blocks = (rx + 255) / 256;
            if (rx > 4 && blocks * 256 <= sizeof wbuf && !sdio_read_port(WCMD_PORT, wbuf, blocks) && (after_packet_read(rx), 1) && get16(wbuf + 2) == 3 && wifi_event_hook)
                wifi_event_hook(wbuf + 4, get16(wbuf) > 4 ? get16(wbuf) - 4 : 0);
        } else delay_us(500);
    }
}

static u32 scan_cmd_ms = 10000;            // wait for a scan command's answer this long (wififind uses less: failures are common on 5 GHz)
static char scan_ssid[33]; static u32 scan_ssid_len;      // set by wififind: scan for this one name only
static u32 sb_no_bssmode, sb_no_ssid, sb_no_rates, sb_no_gap, sb_passive, sb_min, sb_ht, sb_probes;   // scan command variants (wifichan tries them one by one)
static u32 sb_max = 110;                    // longest the chip listens on each channel, in ms (wifi5 raises it)
static int scan_last_rc;                    // how the last scan command ended: 0 ok, >0 firmware error number, <0 our own failure
static int scan_band(u32 radio, const u8 *chans, u32 nch) {
    static u8 body[4 + 5 + 5 + 40 + 4 + 7 * 16 + 6 + 4 + 14 + 30 + 8];
    static const u8 rates24[] = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c};
    static const u8 rates5[] = {0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c};
    u32 p = 0;
    put16(body, 0); put16(body + 2, 0); p = 4;                      // u32 reserved
    if (!sb_no_bssmode) { put16(body + p, 0x01ce); put16(body + p + 2, 1); body[p + 4] = 3; p += 5; }   // BSS mode TLV: any
    if (sb_no_ssid) {                                                                     // variant: no SSID block at all
    } else if (scan_ssid_len) {                                                           // directed scan: only networks with exactly this name (max_ssid_length 0 + the name)
        put16(body + p, 0x0112); put16(body + p + 2, 1 + scan_ssid_len); body[p + 4] = 0;
        for (u32 i = 0; i < scan_ssid_len; i++) body[p + 5 + i] = (u8)scan_ssid[i];
        p += 5 + scan_ssid_len;
    } else {
    put16(body + p, 0x0112); put16(body + p + 2, 1); body[p + 4] = 32; p += 5;           // wildcard SSID TLV: max length 32 = scan for ANY name (0 would mean 'this exact, empty name': v1.14 got no answer)
    }
    put16(body + p, 0x0101); put16(body + p + 2, nch * 7); p += 4;                       // each channel entry is 7 bytes: radio, channel, mode, min time (2), max time (2) (v1.50.3: was 6, which the chip mis-read)                       // channel list TLV
    for (u32 i = 0; i < nch; i++) {
        body[p] = radio; body[p + 1] = chans[i];
        body[p + 2] = (radio && chans[i] >= 52 && chans[i] <= 144) ? 0x13 : 0x02;          // DFS channels: passive + hidden-SSID report + no filter; others: active, no filter (Linux MWIFIEX_*_SCAN bits)
        if (sb_passive) body[p + 2] |= 0x01;                                                // variant: listen only, no probe request
        put16(body + p + 3, sb_min); put16(body + p + 5, sb_max); p += 7;
    }
    const u8 *rt = radio ? rates5 : rates24; u32 rn = radio ? sizeof rates5 : sizeof rates24;
    if (!sb_no_rates) { put16(body + p, 0x0001); put16(body + p + 2, rn); p += 4; for (u32 i = 0; i < rn; i++) body[p++] = rt[i]; }   // supported rates TLV
    if (sb_probes) { put16(body + p, 0x0102); put16(body + p + 2, 2); put16(body + p + 4, 2); p += 6; }                               // number of probe requests per channel
    if (sb_ht) {                                                                                                                       // HT capabilities (what Linux attaches to scans)
        put16(body + p, 0x002d); put16(body + p + 2, 26); p += 4;
        for (u32 i = 0; i < 26; i++) body[p + i] = 0;
        put16(body + p, hw_n_cap & 0xffff); body[p + 2] = 0x17;                                                                 // cap info, A-MPDU parameters
        for (u32 i = 0; i < ((hw_mcs & 0xf) + ((hw_mcs >> 4) & 0xf)) / 2 + 1 && i < 4; i++) body[p + 3 + i] = 0xff;           // MCS rx mask: one byte per supported stream
        p += 26;
    }
    if (!sb_no_gap) { put16(body + p, 0x01c5); put16(body + p + 2, 2); put16(body + p + 4, 50); p += 6; }                          // gap between channels: 50 TU
    u8 r[8]; u32 n = 0;
    scan_done = 0;
    wifi_cmd_ms = scan_cmd_ms;                                      // the firmware answers only after the scan: Linux waits ~10 s
    int rc = wifi_cmd(0x0107, body, p, r, sizeof r, &n);
    wifi_cmd_ms = 1000; scan_last_rc = rc;
    if (!wifi_cmd_err("SCAN", rc, 0)) return 0;
    wifi_poll_events(6000);
    return 1;
}

static int wifi_scan(int with5) {
    puts(wifi_read_bytes == 2 ? "(experiment: whole packets in one multi-block transfer)\n" : wifi_read_bytes ? "(reading packets in byte mode, 512 bytes per transfer)\n" : "(reading packets in 256-byte blocks)\n");
    if (!wifi_ready && !wifi_init()) return 0;
    u8 r[8]; u32 n = 0;
    static const u8 macctl[6] = {0x13, 0x00, 0x00, 0x00, 0x00, 0x00};   // MAC_CONTROL: receive + transmit + Ethernet-II (Linux default packet filter)
    int rc = wifi_cmd(0x0028, macctl, sizeof macctl, r, sizeof r, &n);
    if (rc > 0) puts("  (MAC_CONTROL rejected; continuing)\n");
    static const u8 ch24[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    static const u8 ch5[] = {36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165};   // all US 5 GHz channels (52-144 are DFS: passive listening only)
    nap = 0; scan_events = 0; scan_bytes = 0; dbg_taken = 0; raw_taken = 0; ev_log = 0; recs_seen = 0; mb_fail = 0;
    wifi_event_hook = scan_event;
    puts("scanning 2.4 GHz...\n");
    int ok = scan_band(0, ch24, sizeof ch24);
    if (ok && with5) {                                              // 5 GHz (wifiscan5 only: it gets no answer on hana yet) in groups of 4 channels (Linux's default); a failure here must not hide the 2.4 GHz results
        for (u32 i = 0; i < sizeof ch5 && ok; i += 4) {
            u32 n5 = sizeof ch5 - i < 4 ? sizeof ch5 - i : 4;
            puts("scanning 5 GHz channels "); put_dec(ch5[i]); puts("...\n");
            ok = scan_band(1, ch5 + i, n5);
        }
    }
    wifi_event_hook = 0;
    if (!scan_events && !nap) return 0;                              // nothing at all: the error above says why
    if (wifi_verbose && raw_taken) { puts("first record: block length "); put_dec(raw_len); puts(", bytes:"); for (u32 q = 0; q < 40; q++) { puts(" "); putc("0123456789abcdef"[raw_rec[q] >> 4]); putc("0123456789abcdef"[raw_rec[q] & 15]); } putc('\n'); }
    if (wifi_verbose && dbg_taken) { puts("first big packet, block starts:"); for (u32 b = 0; b < dbg_blocks; b++) { putc(' '); put_hex(dbg_starts[b]); } putc('\n'); }
    puts("events (nets more size stop type len left):\n");
    for (u32 i = 0; i < ev_log; i++) { put_dec(i + 1); puts(": "); put_dec(ev_sets[i]); putc(' '); put_dec(ev_more[i]); putc(' '); put_dec(ev_len[i]); putc(' '); put_dec(ev_stop[i]); putc(' '); put_dec(ev_stype[i] == 0xffff ? 0 : ev_stype[i] & 0xfff); putc(' '); put_dec(ev_slen[i]); putc(' '); put_dec(ev_sleft[i]); putc('\n'); }
    puts("recs "); put_dec(recs_seen); puts("  events "); put_dec(scan_events); putc('\n');
    if (mb_fail) { puts("mb: "); put_dec(mb_fail); puts("x err"); put_dec(mb_err); puts(" got"); put_dec(mb_got); puts(mb_hw ? " hw" : " sw"); putc('\n'); }

    if (!scan_events) errs("WIFI", 14, 3, "the scan finished but the firmware sent no scan results");
    u32 hidden = 0;
    for (u32 i = 0; i < nap; i++) if (!aps[i].ssid[0]) hidden++;
    puts("found "); put_dec(nap - hidden); puts(" networks"); if (hidden) { puts(" ("); put_dec(hidden); puts(" hidden not shown)"); } puts(":\n");
    for (u32 i = 0; i < nap; i++) {
        if (!aps[i].ssid[0]) continue;                              // hidden network (no name): not listed
        puts("  "); puts(aps[i].ssid[0] ? aps[i].ssid : "(hidden)");
        u32 l = aps[i].ssid[0] ? 0 : 8; while (aps[i].ssid[l]) l++;
        for (; l < 24; l++) putc(' ');
        puts(" ch "); put_dec(aps[i].chan); puts("  "); putc('-'); put_dec((u64)(aps[i].rssi < 0 ? -aps[i].rssi : aps[i].rssi)); puts(" dBm  ");
        puts(aps[i].sec == 2 ? "WPA2" : aps[i].sec == 1 ? "WPA" : aps[i].sec == 3 ? "WEP?" : "open"); putc('\n');
    }
    return 1;
}
// ---- v1.40: wififind NAME = directed scan. Asks the chip for ONE network by name, so the answer is a tiny packet that
// reads cleanly (big multi-network scan packets lose bytes on hana). Remembers the result for the join.
static struct ap target; static int have_target;
static void ap_copy(struct ap *d, const struct ap *s) { volatile u8 *dd = (volatile u8 *)d; const u8 *ss = (const u8 *)s; for (u32 i = 0; i < sizeof *d; i++) dd[i] = ss[i]; }   // bytewise: a plain struct copy makes the compiler call memcpy, which does not exist here
static int wifi_find(const char *name) {
    u32 l = 0;
    while (name[l] && l < 32) { scan_ssid[l] = name[l]; l++; }
    scan_ssid[l] = 0; scan_ssid_len = l;
    if (!l) { puts("usage: wififind NAME\n"); return 0; }
    if (!wifi_ready && !wifi_init()) { scan_ssid_len = 0; return 0; }
    u8 r[8]; u32 n = 0;
    static const u8 macctl[6] = {0x13, 0x00, 0x00, 0x00, 0x00, 0x00};
    wifi_cmd(0x0028, macctl, sizeof macctl, r, sizeof r, &n);
    static const u8 ch24[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    static const u8 ch5[] = {36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165};   // all US 5 GHz channels (52-144 are DFS: passive listening only)
    nap = 0; scan_events = 0; scan_bytes = 0; dbg_taken = 0; raw_taken = 0; ev_log = 0; recs_seen = 0; mb_fail = 0;
    wifi_event_hook = scan_event;
    have_target = 0;
    char grp[10]; u32 gn = 0;
    if (wifi_verbose) { puts("looking for "); puts(scan_ssid); puts(" on 2.4 GHz...\n"); }
    grp[gn++] = scan_band(0, ch24, sizeof ch24) ? 'a' : '-';
    for (u32 i = 0; i < nap && !have_target; i++) if (streq(aps[i].ssid, scan_ssid)) { ap_copy(&target, &aps[i]); have_target = 1; }
    scan_cmd_ms = 4000;                                              // short wait: a 5 GHz group that gets no answer must not stall the whole search
    for (u32 i = 0; i < sizeof ch5 && !have_target; i += 4) {
        u32 n5 = sizeof ch5 - i < 4 ? sizeof ch5 - i : 4;
        u32 ev0 = scan_events;
        int ok5 = scan_band(1, ch5 + i, n5);
        if (gn < 9) grp[gn++] = ok5 ? (scan_events > ev0 ? 'e' : 'a') : '-';
        if (wifi_verbose) { puts("5g "); put_dec(ch5[i]); puts(ok5 ? ": ok\n" : ": NO ANSWER\n"); }
        for (u32 k = 0; k < nap && !have_target; k++) if (streq(aps[k].ssid, scan_ssid)) { ap_copy(&target, &aps[k]); have_target = 1; }
    }
    scan_cmd_ms = 10000;    wifi_event_hook = 0;
    scan_ssid_len = 0;
    grp[gn] = 0;
    if (!have_target) { sum_s("N "); sum_rle(grp); return 0; }      // N + one char per scan: - no answer, a answered, e got events
    sum_s("F ch"); { char t[4]; u32 c = target.chan, k = 0; if (c >= 100) t[k++] = '0' + c / 100 % 10; t[k++] = '0' + c / 10 % 10; t[k++] = '0' + c % 10; for (u32 q = 0; q < k; q++) sum_c(t[q]); }
    sum_c(' '); sum_s(target.sec == 2 ? "WPA2" : target.sec == 1 ? "WPA" : target.sec == 3 ? "WEP" : "open");
    { int d = target.rssi < 0 ? -target.rssi : target.rssi; sum_s(" -"); sum_c('0' + d / 10 % 10); sum_c('0' + d % 10); }
    if (wifi_verbose) { puts("  "); put_mac(target.bssid); putc('\n'); }
    return 1;
}
// 802.11d domain info (command 0x005b): tells the chip the country and which channels are allowed there. ChromeOS sets this
// (US); wave-os never did, which may be why the chip will not scan 5 GHz.
static char dom_res = '?', snmp_res = '?';
// SNMP MIB set (command 0x0016): {action 1 = set, oid, size 2, value}. oid 9 = 802.11d on, 10 = 802.11h on (Linux enables 11d before the domain info).
static int wifi_snmp(u16 oid, u16 val) {
    u8 b[8], r[16]; u32 n = 0;
    put16(b, 1); put16(b + 2, oid); put16(b + 4, 2); put16(b + 6, val);
    return wifi_cmd(0x0016, b, 8, r, sizeof r, &n);
}
static int wifi_set_domain(void) {
    static u8 b[2 + 4 + 3 + 3 * 8];
    static const u8 trip[][3] = {{1, 11, 30}, {36, 4, 23}, {52, 4, 23}, {100, 12, 23}, {149, 5, 30}};
    u32 nt = sizeof trip / sizeof trip[0], p = 0;
    put16(b, 1); p = 2;                                              // action: set
    put16(b + p, 7); put16(b + p + 2, 3 + 3 * nt); p += 4;           // country information block
    b[p++] = 'U'; b[p++] = 'S'; b[p++] = ' ';
    for (u32 i = 0; i < nt; i++) { b[p++] = trip[i][0]; b[p++] = trip[i][1]; b[p++] = trip[i][2]; }
    u8 r[16]; u32 n = 0;
    int rc = wifi_cmd(0x005b, b, p, r, sizeof r, &n);
    dom_res = rc == 0 ? 'A' : rc > 0 ? (rc < 10 ? '0' + rc : '+') : 'N';   // A accepted, digit = firmware error number, N no answer
    if (wifi_verbose) { puts("  domain info (US): "); putc(dom_res); putc('\n'); }
    return rc == 0;
}
// ---- v1.45: wifichan CH [NAME] = scan ONE 5 GHz channel with several variants of the scan command, to find which one the chip answers ----
static int wifi_chan(const char *arg) {
    u32 ch = 0; u32 i = 0;
    while (arg[i] >= '0' && arg[i] <= '9') ch = ch * 10 + (arg[i++] - '0');
    while (arg[i] == ' ') i++;
    if (!ch) { puts("usage: wifichan CHANNEL [NAME]   e.g. wifichan 157 HomeWifi\n"); return 0; }
    u32 nl = 0; while (arg[i + nl] && nl < 32) { scan_ssid[nl] = arg[i + nl]; nl++; }
    scan_ssid[nl] = 0;
    if (!wifi_ready && !wifi_init()) return 0;
    u8 r[8]; u32 n = 0; static const u8 macctl[6] = {0x13, 0x00, 0x00, 0x00, 0x00, 0x00};
    wifi_cmd(0x0028, macctl, sizeof macctl, r, sizeof r, &n);
    u32 radio = ch >= 36 ? 1 : 0;
    // Variants (v1.51): 0 wildcard 2.4 GHz ch1-11 (the scan that works), 1 wildcard ch6, 2 named ch1-11, 3 named ch6, 4 wildcard CH, 5 named CH,
    // 6 wildcard ch36, 7 wildcard 4 channels around CH, 8 wildcard CH + 11d/domain, 9 4 channels + 11d/domain, 10 wildcard CH longer listen, 11 wildcard CH no gap TLV.
    static const char *names[] = {"w11", "w6", "d11", "d6", "wCH", "dCH", "w36", "w4", "wCH+11d", "w4+11d", "wCH-min40", "wCH-nogap"};
    static const u8 pre[12] = {0, 0, 0, 0, 0, 0, 0, 0, 3, 3, 0, 0};          // setup before the scan: 1 domain info, 2 enable 11d, 4 enable 11h, 8 read channel region
    static const u8 cl[11] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    u8 cs[11]; u32 ncs = 1;
    wifi_event_hook = scan_event;
    char res[13]; for (u32 q = 0; q < 12; q++) res[q] = '_'; res[12] = 0;
    if (wifi_verbose) puts("variant         | answer events recs found\n");
    for (u32 v = 0; v < 12; v++) {
        sb_no_bssmode = sb_no_ssid = sb_no_rates = sb_no_gap = sb_passive = sb_min = sb_ht = sb_probes = 0;
        scan_ssid_len = (v == 2 || v == 3 || v == 5) ? nl : 0;              // 'd' variants: ask for the name only
        if (v == 10) sb_min = 40;
        if (v == 11) sb_no_gap = 1;
        radio = (v <= 3) ? 0 : 1; if (v == 4 || v == 5 || v == 8 || v == 10 || v == 11) radio = ch >= 36 ? 1 : 0;
        ncs = 1; cs[0] = (u8)ch;
        if (v == 0 || v == 2) { ncs = 11; for (u32 q = 0; q < 11; q++) cs[q] = cl[q]; }
        if (v == 1 || v == 3) cs[0] = 6;
        if (v == 6) cs[0] = 36;
        if (v == 7 || v == 9) { ncs = 4; for (u32 q = 0; q < 4; q++) cs[q] = (u8)(ch - 8 + 4 * q); }
        nap = 0; scan_events = 0; scan_bytes = 0; dbg_taken = 0; raw_taken = 0; ev_log = 0; recs_seen = 0; mb_fail = 0;
        scan_cmd_ms = 3500; wdt_kick();
        if (v) {                                                            // every variant starts on a freshly restarted chip
            if (!wifi_init()) { res[v] = 'x'; break; }
            wifi_cmd(0x0028, macctl, sizeof macctl, r, sizeof r, &n);
        }
        if (pre[v] & 2) { int sr = wifi_snmp(9, 1); snmp_res = sr == 0 ? 'A' : sr > 0 ? (sr < 10 ? '0' + sr : '+') : 'N'; }
        if (pre[v] & 1) wifi_set_domain();
        if (pre[v] & 4) wifi_snmp(10, 1);
        if (pre[v] & 8) { static const u8 rg[2] = {0, 0}; wifi_cmd(0x0242, rg, 2, r, sizeof r, &n); }
        int ok = scan_band(radio, cs, ncs);
        scan_cmd_ms = 10000;
        u32 hit = 0; for (u32 k = 0; k < nap; k++) if (nl ? streq(aps[k].ssid, scan_ssid) : aps[k].chan == ch) hit = 1;
        res[v] = hit ? 'F' : ok ? 'a' : scan_last_rc > 0 ? (scan_last_rc < 10 ? '0' + scan_last_rc : '+') : scan_last_rc == -2 ? '-' : scan_last_rc == -1 ? '=' : '~';   // - no answer, digit = firmware error number, = send failed, a answered, F found
        if (wifi_verbose) { u32 l = 0; puts(names[v]); while (names[v][l]) l++; for (; l < 15; l++) putc(' ');
            puts("| "); puts(ok ? "yes    " : "NO     "); put_dec(scan_events); puts("      "); put_dec(recs_seen); puts("    "); puts(hit ? "FOUND" : "-"); putc('\n'); }
        if (wifi_verbose && hit) { for (u32 k = 0; k < nap; k++) { puts("   "); puts(aps[k].ssid[0] ? aps[k].ssid : "(hidden)"); puts(" ch "); put_dec(aps[k].chan); puts(" -"); put_dec((u64)(aps[k].rssi < 0 ? -aps[k].rssi : aps[k].rssi)); putc('\n'); } }
    }
    sb_no_bssmode = sb_no_ssid = sb_no_rates = sb_no_gap = sb_passive = sb_min = sb_ht = sb_probes = 0;
    wifi_event_hook = 0; scan_ssid_len = 0;
    sum_s("R:"); sum_rle(res); sum_s(" D:"); sum_c(dom_res); sum_s(" S:"); sum_c(snmp_res);   // the shell prints this on the one-line summary
    return 1;
}
// ---- v1.50.2: wifi5 [CH [NAME]] = ONE 5 GHz scan over 4 channels (CH-8 .. CH+4, default 149-161), prints what came back.
// (v1.50 showed the chip rejects scans of a SINGLE channel with error 1, and answers scans of 4+ channels.)
static int wifi_5g(const char *arg) {
    u32 ch = 0, i = 0, all = 0, passive = 0, directed = 0;
    while (arg[i] >= '0' && arg[i] <= '9') ch = ch * 10 + (arg[i++] - '0');
    if (!ch && (arg[i] == 'a' || arg[i] == 'p' || arg[i] == 'd')) {                              // letters: a = all channels, p = passive (listen only, 400 ms each), d = ask for HomeWifi by name; e.g. "wifi5 pa"
        while (arg[i] && arg[i] != ' ') { if (arg[i] == 'a') all = 1; else if (arg[i] == 'p') passive = 1; else if (arg[i] == 'd') directed = 1; i++; }
    }          // "wifi5 all": every 5 GHz channel, longer listening
    while (arg[i] == ' ') i++;
    if (!ch) ch = 157;
    u32 nl = 0; while (arg[i + nl] && nl < 32) { scan_ssid[nl] = arg[i + nl]; nl++; }
    if (!nl) { const char *d = "HomeWifi"; while (d[nl]) { scan_ssid[nl] = d[nl]; nl++; } }
    scan_ssid[nl] = 0;
    if (!wifi_init()) return 0;                                      // always a freshly started chip: an earlier scan that hung would otherwise ruin this one
    u8 r[8]; u32 n = 0; static const u8 macctl[6] = {0x13, 0x00, 0x00, 0x00, 0x00, 0x00};
    wifi_cmd(0x0028, macctl, sizeof macctl, r, sizeof r, &n);
    u8 cs[12]; u32 ncs = 4;
    if (all) { static const u8 al[] = {36, 40, 44, 48, 149, 153, 157, 161, 165}; ncs = sizeof al; for (u32 q = 0; q < ncs; q++) cs[q] = al[q]; }
    else for (u32 q = 0; q < 4; q++) cs[q] = (u8)(ch - 8 + 4 * q);
    nap = 0; scan_events = 0; scan_bytes = 0; dbg_taken = 0; raw_taken = 0; ev_log = 0; recs_seen = 0; mb_fail = 0; evraw_taken = 0;
    wifi_event_hook = scan_event; scan_ssid_len = directed ? nl : 0; scan_cmd_ms = 6000;
    sb_passive = passive;
    sb_max = passive ? 400 : 250;                                                                            // listen longer than the default 110 ms per channel
    int ok = 0;
    for (u32 g = 0; g < ncs; g += 4) { u32 ng = ncs - g < 4 ? ncs - g : 4; if (scan_band(1, cs + g, ng)) ok = 1; wdt_kick(); }
    sb_max = 110; sb_passive = 0; scan_ssid_len = 0; scan_cmd_ms = 10000; wifi_event_hook = 0;
    u32 hit = 0; for (u32 k = 0; k < nap; k++) if (streq(aps[k].ssid, scan_ssid)) hit = 1;
    static const u8 home_mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};          // HomeWifi's address, from ChromeOS (chrome://network)
    u32 bss_exact = 0, bss_oui = 0, on157 = 0;
    for (u32 k = 0; k < nap; k++) {
        u32 pre = aps[k].bssid[0] == 0x02 && aps[k].bssid[1] == 0x11 && aps[k].bssid[2] == 0x22;
        u32 ex = pre; for (u32 m = 3; m < 6; m++) if (aps[k].bssid[m] != home_mac[m]) ex = 0;
        if (ex) bss_exact = 1; else if (pre) bss_oui = 1;
        if (aps[k].chan == 157) on157++;
    }
    if (wifi_verbose && evraw_taken) {                                  // structure of the first report: length, then each block as type/length, and each record's elements as id/length
        puts("  ev len "); put_dec(evraw_len); puts(" size "); put_dec(get16(evraw + 8)); puts(" sets "); put_dec(evraw[10]); putc('\n');
        u32 left = evraw_len > 11 ? evraw_len - 11 : 0, tot = get16(evraw + 8); if (tot < left) left = tot;
        const u8 *t = evraw + 11;
        while (left >= 4) {
            u32 ty = get16(t), tl = get16(t + 2);
            puts("  blk "); put_hex(ty); putc('/'); put_dec(tl);
            if (left < 4 + tl) { puts(" (cut: only "); put_dec(left - 4); puts(" left)\n"); break; }
            if (ty == 0x0156 && tl >= 18) {
                puts(" ie:"); const u8 *ie = t + 4 + 6 + 12, *end = t + 4 + tl; u32 cnt = 0;
                while (ie + 2 <= end && cnt < 16) { putc(' '); put_dec(ie[0]); putc('/'); put_dec(ie[1]); if (ie + 2 + ie[1] > end) { puts("!"); break; } ie += 2 + ie[1]; cnt++; }
            }
            putc('\n'); t += 4 + tl; left -= 4 + tl;
        }
    }
    {   // how many networks were heard on each channel, e.g.  by ch: 36:3 149:2
        u8 cc[170]; for (u32 q = 0; q < 170; q++) cc[q] = 0;
        for (u32 k = 0; k < nap; k++) if (aps[k].chan < 170 && cc[aps[k].chan] < 99) cc[aps[k].chan]++;
        puts("by ch:"); for (u32 q = 0; q < 170; q++) if (cc[q]) { putc(' '); put_dec(q); putc(':'); put_dec(cc[q]); } putc('\n');
        u8 ci[170]; for (u32 q = 0; q < 170; q++) ci[q] = 0;                  // the same count by the channel the firmware itself reported, plus its band flag
        u32 b0 = 0, b1 = 0, bx = 0;
        for (u32 k = 0; k < nap; k++) { if (aps[k].ich < 170 && ci[aps[k].ich] < 99) ci[aps[k].ich]++; if (aps[k].rad == 0) b0++; else if (aps[k].rad == 1) b1++; else bx++; }
        puts("fw ch:"); for (u32 q = 0; q < 170; q++) if (ci[q]) { putc(' '); put_dec(q); putc(':'); put_dec(ci[q]); }
        puts("  band bg:"); put_dec(b0); puts(" a:"); put_dec(b1); if (bx) { puts(" ?:"); put_dec(bx); } putc('\n');
    }
    if (wifi_verbose) for (u32 k = 0; k < nap; k++) { puts("  ch "); put_dec(aps[k].chan); puts("  "); puts(aps[k].ssid[0] ? aps[k].ssid : "(hidden)"); puts("  -"); put_dec((u64)(aps[k].rssi < 0 ? -aps[k].rssi : aps[k].rssi)); putc('\n'); }
    sum_c(ok ? 'a' : '-'); sum_s(" ev"); { char t[12]; u32 k = 0, v = scan_events; do { t[k++] = '0' + v % 10; v /= 10; } while (v); while (k) sum_c(t[--k]); }
    sum_s(" rec"); { char t[12]; u32 k = 0, v = recs_seen; do { t[k++] = '0' + v % 10; v /= 10; } while (v); while (k) sum_c(t[--k]); }
    sum_s(" net"); { char t[12]; u32 k = 0, v = nap; do { t[k++] = '0' + v % 10; v /= 10; } while (v); while (k) sum_c(t[--k]); }
    if (hit) sum_s(" FOUND");
    if (bss_exact) sum_s(" MAC"); else if (bss_oui) sum_s(" OUI");                // HomeWifi's address (or one from the same router maker) was among the networks heard
    sum_s(" c157:"); sum_c('0' + on157 % 10);                                // networks heard on channel 157
    return ok;
}
// ---- v1.39: wifitry = scan with many read settings and print a scoreboard (looking for the setting that gets whole packets through) ----
static int ci_eq(const char *s, const char *want) {                 // does s contain `want`, ignoring case?
    for (u32 i = 0; s[i]; i++) {
        u32 k = 0;
        while (want[k] && s[i + k] && ((s[i + k] | 0x20) == (want[k] | 0x20))) k++;
        if (!want[k]) return 1;
    }
    return 0;
}

struct trycfg { int mode; u32 chunk, div; };
static const struct trycfg tries[] = {
    {1, 512, 0}, {1, 512, 8}, {1, 512, 16}, {1, 512, 32}, {1, 512, 64},
    {1, 256, 0}, {1, 256, 16}, {1, 256, 64}, {1, 128, 0}, {1, 128, 32},
    {0, 256, 0}, {2, 256, 0},
};

static int wifi_try(void) {
    if (!wifi_ready && !wifi_init()) return 0;
    u8 r[8]; u32 n = 0;
    static const u8 macctl[6] = {0x13, 0x00, 0x00, 0x00, 0x00, 0x00};
    wifi_cmd(0x0028, macctl, sizeof macctl, r, sizeof r, &n);
    static const u8 ch24[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    puts("cfg  mode chunk div | events full recs nets home\n");
    int best = -1; u32 best_recs = 0, best_nap = 0;
    static struct ap best_aps[32];
    wifi_event_hook = scan_event;
    for (u32 t = 0; t < sizeof tries / sizeof tries[0]; t++) {
        wifi_read_bytes = tries[t].mode; rd_chunk = tries[t].chunk; rd_div = tries[t].div; rd_gap_us = 0;
        nap = 0; scan_events = 0; scan_bytes = 0; dbg_taken = 0; raw_taken = 0; ev_log = 0; recs_seen = 0; mb_fail = 0;
        wdt_kick();
        int ok = scan_band(0, ch24, sizeof ch24);
        u32 full = 0, home = 0;
        for (u32 i = 0; i < ev_log; i++) if (ev_sleft[i] == 0) full++;
        for (u32 i = 0; i < nap; i++) if (ci_eq(aps[i].ssid, "home")) home = 1;
        put_dec(t + 1); puts("    "); put_dec((u64)tries[t].mode); puts("    "); put_dec(tries[t].chunk); puts("   "); put_dec(tries[t].div);
        puts(" | "); put_dec(scan_events); puts("      "); put_dec(full); puts("    "); put_dec(recs_seen); puts("    "); put_dec(nap); puts("    ");
        puts(ok ? (home ? "YES" : "no") : "ERR"); putc('\n');
        if (recs_seen > best_recs || (recs_seen == best_recs && home)) { best = (int)t; best_recs = recs_seen; best_nap = nap; for (u32 i = 0; i < nap; i++) ap_copy(&best_aps[i], &aps[i]); }
    }
    wifi_event_hook = 0;
    wifi_read_bytes = 1; rd_chunk = 512; rd_div = 0;
    puts("best: cfg "); put_dec((u64)(best + 1)); puts(" with "); put_dec(best_recs); puts(" records\n");
    for (u32 i = 0; i < best_nap && i < 14; i++) {
        if (!best_aps[i].ssid[0]) continue;
        puts("  "); puts(best_aps[i].ssid); puts("  ch "); put_dec(best_aps[i].chan); putc('\n');
    }
    return 1;
}
static void wifi_probe(void) {
    puts("wifi probe (read-only): Marvell 88W8897 on SDIO/MSDC3\n");
    show_pin(85, "chip power (WIFI_PDN)");
    show_pin(38, "chip irq");
    static const char *bus[] = {"dat0", "dat1", "dat2", "dat3", "clk", "cmd"};
    for (u32 p = 22; p <= 27; p++) show_pin(p, bus[p - 22]);

    u32 gates = rd32(PERICFG_BASE + 0x18);
    int msdc3_clk = !((gates >> 16) & 1);
    puts("  peri0 clock gates "); put_hex(gates); puts(": msdc3 clock "); puts(msdc3_clk ? "ON\n" : "off (gated)\n");
    if (msdc3_clk) {
        static const u32 regs[] = {0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x30, 0x3c};
        static const char *names[] = {"cfg", "iocon", "ps", "int", "inten", "fifocs", "sdc_cfg", "sdc_sts"};
        for (u32 i = 0; i < 8; i++) { puts("  msdc3 "); puts(names[i]); puts(" "); put_hex(rd32(MSDC3_BASE + regs[i])); putc('\n'); }
    } else {
        puts("  msdc3 registers: not read (clock off; reading could freeze the bus)\n");
    }

    u32 cid = 0, en = 0, vsel = 0;
    int r1 = pmic_read(0x0100, &cid), r2 = pmic_read(0x041e, &en), r3 = pmic_read(0x043a, &vsel);
    if (r1 || r2 || r3) {
        err("WIFI", 1, "PMIC wrapper did not answer (can't read the Wi-Fi bus supply)");
        return;
    }
    static const char *volts[] = {"1.2", "1.3", "1.5", "1.8", "2.5", "2.8", "3.0", "3.3"};
    puts("  pmic id "); put_hex(cid); puts(", bus supply VGP3 "); puts((en >> 15) & 1 ? "ON" : "off");
    puts(" at "); puts(volts[(vsel >> 5) & 7]); puts(" V (con7 "); put_hex(en); puts(", con21 "); put_hex(vsel); puts(")\n");
}
