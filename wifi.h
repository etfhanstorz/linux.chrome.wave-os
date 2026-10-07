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
    // 2. Chip power: GPIO85 as a plain GPIO, driven high
    pin_mode(85, 0);
    gpio_out(85, 1);
    delay_us(20000);                             // regulators + card power-up
    puts("  chip power gpio85: mode "); put_dec(gpio_mode(85)); puts(" level "); put_dec(gpio_bit(0x500, 85)); putc('\n');
    // 3. Controller at a safe slow clock; give the card its >= 74 clocks
    msdc_init_slow();
    delay_us(5000);
    puts("  msdc3 cfg "); put_hex(msdc_rd(MSDC_CFG)); puts(" sdc_cfg "); put_hex(msdc_rd(SDC_CFG)); putc('\n');
    // 4. SDIO greeting
    u32 ocr = 0;
    msdc_cmd(0, 0, RSP_NONE, 0);                 // GO_IDLE (harmless for SDIO)
    if (msdc_cmd(5, 0, RSP_R3, &ocr)) { err("WIFI", 3, "chip did not answer CMD5 (not powered, or bus problem)"); return 0; }
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
    if (msdc_cmd(3, 0, RSP_R1, &r6)) { err("WIFI", 5, "chip did not give an address (CMD3)"); return 0; }
    u32 rca = r6 >> 16;
    if (msdc_cmd(7, rca << 16, RSP_R1B, &r1)) { err("WIFI", 5, "chip could not be selected (CMD7)"); return 0; }
    puts("  ready, address "); put_hex(rca); putc('\n');
    // 5. Read the CCCR and walk the CIS for the manufacturer tuple (0x20)
    int cccr = sdio_read_byte(0, 0x00), sdrev = sdio_read_byte(0, 0x01);
    int cis = sdio_read_byte(0, 0x09) | (sdio_read_byte(0, 0x0a) << 8) | (sdio_read_byte(0, 0x0b) << 16);
    if (cccr < 0 || sdrev < 0) { err("WIFI", 6, "SDIO register read (CMD52) failed"); return 0; }
    puts("  CCCR rev "); put_hex(cccr); puts(", SD rev "); put_hex(sdrev); puts(", CIS at "); put_hex((u32)cis); putc('\n');
    u32 vendor = 0, device = 0;
    for (u32 p = (u32)cis, n = 0; n < 64; n++) {
        int code = sdio_read_byte(0, p), len = sdio_read_byte(0, p + 1);
        if (code < 0 || len < 0) { err("WIFI", 6, "SDIO register read (CMD52) failed"); return 0; }
        if (code == 0xff) break;                 // end of chain
        if (code == 0x20 && len >= 4) {
            vendor = sdio_read_byte(0, p + 2) | (sdio_read_byte(0, p + 3) << 8);
            device = sdio_read_byte(0, p + 4) | (sdio_read_byte(0, p + 5) << 8);
            break;
        }
        p += 2 + (u32)len;
    }
    puts("  vendor "); put_hex(vendor); puts(", device "); put_hex(device);
    if (vendor == 0x02df && device == 0x912d) { puts(": Marvell 88W8897. Hello, Wi-Fi chip!\n"); return 1; }
    puts("\n");
    err("WIFI", 7, "unexpected SDIO vendor/device ID (not a Marvell 88W8897)");
    return 0;
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
