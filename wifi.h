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
static int sdio_read_port(u32 addr, u8 *out, u32 blocks) {
    u32 len = blocks * 256, got = 0;
    u32 save_div = (msdc_rd(MSDC_CFG) >> 8) & 0xff;
    if (blocks > 1) msdc_set_clock(32);                              // big PIO reads: the FIFO is drained by hand, so slow the bus down (v1.19: data timeout on a 7-block read)
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
    if (blocks > 1) msdc_set_clock(save_div);
    if (st & (INT_CMDTMO | INT_RSPCRC | INT_DATTMO | INT_DATCRC)) return 1 + (int)((st >> 9) & 0x7f);
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
static u32 w3_why, w3_a, w3_b;           // why a command answer was 'malformed': 1 = bad length (a), 2 = block read failed with error a for length b
static u8 wifi_dbg[16];                   // first bytes of the last unexpected packet
static u32 wifi_dbg_n;                  // how many unexpected packets were skipped
static int cmd_packet_waiting(int st);
static u8 wbuf[2312 + 256 + 256];
static u16 wifi_seq;
static void (*wifi_event_hook)(const u8 *ev, u32 len);   // called for firmware events seen while waiting
static u32 wifi_cmd_ms = 1000;          // how long to wait for an answer (scans need longer)

static int wifi_dn_seen;                  // the chip raised 'command port ready' (0x80) since our last command
static int cmd_packet_waiting(int st) {
    if (st >= 0 && (st & 0x80)) wifi_dn_seen = 1;
    if (st >= 0 && (st & UP_LD_CMD_PORT_INT)) return 1;
    int l0 = fn1_rd(0xb4), l1 = fn1_rd(0xb5);
    if (l0 < 0 || l1 < 0) return 0;
    u32 len = ((u32)l1 << 8) | (u32)l0;
    if (!len) { wifi_last_len = 0; return 0; }
    return len != wifi_last_len;                                   // a length we have not read yet
}
static void put16(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; }
static u32 get16(const u8 *p) { return p[0] | (p[1] << 8); }

// Send host command `cmd` with `blen` body bytes; copy up to rmax response body bytes to resp.
// Returns the firmware result (0 = ok, >0 firmware error) or a negative wave-os error: -1 send failed,
// -2 no response, -3 malformed response.
static int wifi_cmd(u32 cmd, const u8 *body, u32 blen, u8 *resp, u32 rmax, u32 *rlen) {
    u32 total = 4 + 8 + blen;
    if (total > 2312) return -1;
    wifi_dbg_n = 0;
    for (u32 i = 0; i < 256 * ((total + 255) / 256); i++) wbuf[i] = 0;
    put16(wbuf, total); put16(wbuf + 2, 1);                       // SDIO header: length, type = command
    put16(wbuf + 4, cmd); put16(wbuf + 6, 8 + blen); put16(wbuf + 8, ++wifi_seq); put16(wbuf + 10, 0);
    for (u32 i = 0; i < blen; i++) wbuf[12 + i] = body[i];
    // Linux sends the next command only after the chip raised 'command port ready' (0x80) for the last one.
    for (u32 i = 0; i < 100 && wifi_seq > 1 && !wifi_dn_seen; i++) { int s = fn1_rd(0x03); if (s >= 0 && (s & 0x80)) wifi_dn_seen = 1; if (s >= 0 && (s & 0x40)) break; delay_us(500); }
    delay_us(5000);
    fn1_rd(0x03);                                                  // clear stale interrupt bits (reset on read)
    wifi_dn_seen = 0;
    if (sdio_write_port(WCMD_PORT, wbuf, (total + 255) / 256)) return -1;
    u64 hz = tick_hz(), t0 = ticks();
    for (;;) {
        int st = fn1_rd(0x03);                                     // host interrupt status
        if (cmd_packet_waiting(st)) {
            int l0 = fn1_rd(0xb4), l1 = fn1_rd(0xb5);              // length of the waiting packet
            if (l0 < 0 || l1 < 0) return -2;
            u32 rx = ((u32)l1 << 8) | (u32)l0;
            wifi_last_len = rx;
            u32 blocks = (rx + 255) / 256;
            if (rx <= 4 || blocks * 256 > sizeof wbuf) { w3_why = 1; w3_a = rx; return -3; }
            { int re = sdio_read_port(WCMD_PORT, wbuf, blocks); if (re) { w3_why = 2; w3_a = (u32)re; w3_b = rx; return -3; } }
            u32 type = get16(wbuf + 2);
            if (type == 3) { if (wifi_event_hook) wifi_event_hook(wbuf + 4, get16(wbuf) > 4 ? get16(wbuf) - 4 : 0); continue; }   // an event: hand it on, keep waiting
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
        if (hz && ticks() - t0 > hz / 1000 * wifi_cmd_ms) {        // normally 1 s
            if (wifi_dbg_n) { puts("  skipped "); put_dec(wifi_dbg_n); puts(" unexpected packet(s); the last began:"); for (u32 i = 0; i < 16; i++) { putc(' '); put_hex(wifi_dbg[i]); } putc('\n'); }
            puts("  no answer. status 0x03 = "); put_hex((u32)st); puts(", mask 0x02 = "); put_hex((u32)fn1_rd(0x02));
            puts(", 0x50 = "); put_hex((u32)fn1_rd(0x50)); puts(", cmd length 0xb4/5 = "); put_hex((u32)fn1_rd(0xb4)); puts("/"); put_hex((u32)fn1_rd(0xb5)); putc('\n');
            return -2;
        }
        delay_us(500);
    }
}

static void put_mac(const u8 *m) {
    for (u32 i = 0; i < 6; i++) { putc("0123456789abcdef"[m[i] >> 4]); putc("0123456789abcdef"[m[i] & 15]); if (i < 5) putc(':'); }
}

static int wifi_cmd_err(const char *name, int r, u32 sub_base) {
    (void)sub_base;
    if (r == 0) return 1;
    puts("  command "); puts(name); puts(": ");
    if (r > 0) { puts("firmware answered with error "); put_dec((u64)r); putc('\n'); errs("WIFI", 13, 1, "Wi-Fi firmware rejected a command"); }
    else if (r == -1) { puts("send failed\n"); errs("WIFI", 12, 1, "could not send a command to the Wi-Fi firmware"); }
    else if (r == -2) { puts("no answer within 1 s\n"); errs("WIFI", 12, 2, "the Wi-Fi firmware did not answer a command"); }
    else {
        puts("malformed answer");
        if (w3_why == 1) { puts(": packet length "); put_dec(w3_a); puts(" is unusable"); }
        else if (w3_why == 2) { puts(": reading the "); put_dec(w3_b); puts("-byte packet failed (controller error "); put_dec(w3_a); puts("), "); put_dec(rd_got); puts(" bytes arrived, "); puts(rd_hw_timeout ? "controller timeout" : "our own 0.5 s timeout"); }
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
static u8 wifi_mac[6];
static int wifi_ready;                  // FUNC_INIT + GET_HW_SPEC done in this boot
static int wifi_hw_spec(void) {
    u8 r[96]; u32 n = 0;
    puts("talking to the Wi-Fi firmware...\n");
    int rc = wifi_cmd(0x00a9, 0, 0, r, sizeof r, &n);
    if (rc == -2) return -2;                             // silent: caller may power-cycle
    if (!wifi_cmd_err("FUNC_INIT", rc, 0)) return 0;
    static const u8 zero[63];                            // GET_HW_SPEC request: all-zero spec
    if (!wifi_cmd_err("GET_HW_SPEC", wifi_cmd(0x0003, zero, sizeof zero, r, sizeof r, &n), 0)) return 0;
    if (n < 22) { errs("WIFI", 12, 3, "the Wi-Fi firmware's answer was not understood"); return 0; }
    for (u32 i = 0; i < 6; i++) wifi_mac[i] = r[8 + i];
    u32 fwver = r[18] | (r[19] << 8) | ((u32)r[20] << 16) | ((u32)r[21] << 24);   // hw spec: mac at 8, region 14, antennas 16, release 18
    puts("  Wi-Fi MAC address "); put_mac(wifi_mac); putc('\n');
    puts("  firmware release "); put_hex(fwver); puts(", antennas "); put_dec(get16(r + 16)); puts(", region "); put_hex(get16(r + 14)); putc('\n');
    puts("  the Wi-Fi firmware answers our commands.\n");
    wifi_ready = 1;
    return 1;
}

static int wifi_init(void) {
    if (!wifi_fw()) return 0;
    int r = wifi_hw_spec();
    if (r != -2) return r == 1;
    // The running firmware (left by ChromeOS) never answered: restart the chip and load our own firmware.
    puts("  no answer from the leftover firmware.\n");
    wifi_power_cycle();
    wifi_seq = 0;
    if (!wifi_fw()) return 0;
    r = wifi_hw_spec();
    if (r == -2) { wifi_cmd_err("FUNC_INIT", -2, 0); return 0; }
    return r == 1;
}
// ---- v1.11: scan for networks (legacy scan command 0x0006; answer layout: Linux mwifiex scan.c) ----
// Extended scan (what Linux uses on this chip): command 0x0107 = {u32 reserved, TLVs}. The command's answer carries
// no results; they come as events (id 0x58) holding, per access point, a BSS_SCAN_RSP TLV {bssid, frame body} and a
// BSS_SCAN_INFO TLV {rssi, ..., channel}.
struct ap { u8 bssid[6]; char ssid[33]; u8 chan, sec; int rssi; };
static struct ap aps[32];
static u32 nap, scan_events, scan_done, scan_bytes;

static void scan_event(const u8 *ev, u32 len) {
    if (len < 11 || get16(ev) != 0x58) return;
    scan_events++; scan_bytes += len;
    u32 size = get16(ev + 8), left = len - 11 < size ? len - 11 : size;
    const u8 *t = ev + 11;
    struct ap *cur = 0;
    while (left >= 4) {
        u32 type = get16(t), tl = get16(t + 2);
        if (left < 4 + tl) break;
        if (type == 0x0156 && tl >= 6 + 12) {                      // BSS_SCAN_RSP: bssid[6] + frame body (ts8, interval2, cap2, IEs)
            u32 k;
            for (k = 0; k < nap; k++) { u32 same = 1; for (u32 m = 0; m < 6; m++) if (aps[k].bssid[m] != t[4 + m]) same = 0; if (same) break; }
            if (k == nap && nap < 32) nap++;
            cur = k < 32 ? &aps[k] : 0;
            if (cur) {
                for (u32 m = 0; m < 6; m++) cur->bssid[m] = t[4 + m];
                cur->ssid[0] = 0; cur->chan = 0; cur->sec = 0; cur->rssi = 0;
                u32 cap = get16(t + 4 + 6 + 10);
                const u8 *ie = t + 4 + 6 + 12, *end = t + 4 + tl;
                while (ie + 2 <= end && ie + 2 + ie[1] <= end) {
                    u32 id = ie[0], l = ie[1];
                    if (id == 0) { u32 q; for (q = 0; q < l && q < 32; q++) cur->ssid[q] = ie[2 + q] >= 32 && ie[2 + q] < 127 ? ie[2 + q] : '?'; cur->ssid[q] = 0; }
                    else if (id == 3 && l >= 1) cur->chan = ie[2];
                    else if (id == 48) cur->sec = 2;
                    else if (id == 221 && l >= 4 && ie[2] == 0x00 && ie[3] == 0x50 && ie[4] == 0xf2 && ie[5] == 1 && cur->sec < 1) cur->sec = 1;
                    ie += 2 + l;
                }
                if (!cur->sec && (cap & 0x10)) cur->sec = 3;
            }
        } else if (type == 0x0157 && tl >= 7 && cur) {             // BSS_SCAN_INFO: rssi(s16) anpi(2) cca(1) radio(1) channel(1)
            cur->rssi = (short)get16(t + 4);
            if (!cur->chan) cur->chan = t[4 + 6];
        }
        t += 4 + tl; left -= 4 + tl;
    }
    if (ev[4] == 0) scan_done = 1;                                  // more_event == 0: that was the last report
}

// Wait up to `ms` for events on the command port, handing each to the hook, until a scan finishes.
static void wifi_poll_events(u32 ms) {
    u64 hz = tick_hz(), t0 = ticks();
    while (!scan_done && hz && ticks() - t0 < hz / 1000 * ms) {
        int st = fn1_rd(0x03);
        if (cmd_packet_waiting(st)) {
            int l0 = fn1_rd(0xb4), l1 = fn1_rd(0xb5);
            u32 rx = ((u32)(l1 < 0 ? 0 : l1) << 8) | (u32)(l0 < 0 ? 0 : l0), blocks = (rx + 255) / 256;
            wifi_last_len = rx;
            if (rx > 4 && blocks * 256 <= sizeof wbuf && !sdio_read_port(WCMD_PORT, wbuf, blocks) && get16(wbuf + 2) == 3 && wifi_event_hook)
                wifi_event_hook(wbuf + 4, get16(wbuf) > 4 ? get16(wbuf) - 4 : 0);
        } else delay_us(500);
    }
}

static int scan_band(u32 radio, const u8 *chans, u32 nch) {
    static u8 body[4 + 5 + 5 + 4 + 6 * 16 + 6 + 4 + 14];
    static const u8 rates24[] = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c};
    static const u8 rates5[] = {0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c};
    u32 p = 0;
    put16(body, 0); put16(body + 2, 0); p = 4;                      // u32 reserved
    put16(body + p, 0x01ce); put16(body + p + 2, 1); body[p + 4] = 3; p += 5;            // BSS mode TLV: any
    put16(body + p, 0x0112); put16(body + p + 2, 1); body[p + 4] = 32; p += 5;           // wildcard SSID TLV: max length 32 = scan for ANY name (0 would mean 'this exact, empty name': v1.14 got no answer)
    put16(body + p, 0x0101); put16(body + p + 2, nch * 6); p += 4;                       // channel list TLV
    for (u32 i = 0; i < nch; i++) {
        body[p] = radio; body[p + 1] = chans[i]; body[p + 2] = 0x02;                      // active, channel filter disabled
        put16(body + p + 3, 0); put16(body + p + 5, 110); p += 6;
    }
    const u8 *rt = radio ? rates5 : rates24; u32 rn = radio ? sizeof rates5 : sizeof rates24;
    put16(body + p, 0x0001); put16(body + p + 2, rn); p += 4;                            // supported rates TLV
    for (u32 i = 0; i < rn; i++) body[p++] = rt[i];
    put16(body + p, 0x01c5); put16(body + p + 2, 2); put16(body + p + 4, 50); p += 6;    // gap between channels: 50 TU
    u8 r[8]; u32 n = 0;
    scan_done = 0;
    wifi_cmd_ms = 10000;                                            // the firmware answers only after the scan: Linux waits ~10 s
    int rc = wifi_cmd(0x0107, body, p, r, sizeof r, &n);
    wifi_cmd_ms = 1000;
    if (!wifi_cmd_err("SCAN", rc, 0)) return 0;
    wifi_poll_events(6000);
    return 1;
}

static int wifi_scan(void) {
    if (!wifi_ready && !wifi_init()) return 0;
    u8 r[8]; u32 n = 0;
    static const u8 macctl[6] = {0x13, 0x00, 0x00, 0x00, 0x00, 0x00};   // MAC_CONTROL: receive + transmit + Ethernet-II (Linux default packet filter)
    int rc = wifi_cmd(0x0028, macctl, sizeof macctl, r, sizeof r, &n);
    if (rc > 0) puts("  (MAC_CONTROL rejected; continuing)\n");
    static const u8 ch24[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    static const u8 ch5[] = {36, 40, 44, 48, 149, 153, 157, 161, 165};
    nap = 0; scan_events = 0; scan_bytes = 0;
    wifi_event_hook = scan_event;
    puts("scanning 2.4 GHz...\n");
    int ok = scan_band(0, ch24, sizeof ch24);
    if (ok) { puts("scanning 5 GHz...\n"); ok = scan_band(1, ch5, sizeof ch5); }
    wifi_event_hook = 0;
    if (!ok) return 0;
    puts("scan events received: "); put_dec(scan_events); puts(" ("); put_dec(scan_bytes); puts(" bytes)\n");
    if (!scan_events) errs("WIFI", 14, 3, "the scan finished but the firmware sent no scan results");
    puts("found "); put_dec(nap); puts(" networks:\n");
    for (u32 i = 0; i < nap; i++) {
        puts("  "); puts(aps[i].ssid[0] ? aps[i].ssid : "(hidden)");
        u32 l = aps[i].ssid[0] ? 0 : 8; while (aps[i].ssid[l]) l++;
        for (; l < 24; l++) putc(' ');
        puts(" ch "); put_dec(aps[i].chan); puts("  "); putc('-'); put_dec((u64)(aps[i].rssi < 0 ? -aps[i].rssi : aps[i].rssi)); puts(" dBm  ");
        puts(aps[i].sec == 2 ? "WPA2" : aps[i].sec == 1 ? "WPA" : aps[i].sec == 3 ? "WEP?" : "open"); putc('\n');
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
