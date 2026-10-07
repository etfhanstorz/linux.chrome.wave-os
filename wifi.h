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
