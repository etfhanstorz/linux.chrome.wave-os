// Touchpad (v2.0, step 1): the hana's Elan touchpad on I2C bus 4 (from the hana device tree: elan,ekth3000 @ i2c@11011000 reg 0x15).
//   controller: MT8173 I2C4 @ 0x11011000, its DMA channel @ 0x11000300; clocks PERI0 bit 27 (I2C4) and bit 12 (AP_DMA)
//   pins:       133 SCL / 134 SDA (pinmux mode 1); touchpad interrupt = GPIO117, active low, pull-up
//   power:      MT6397 LDO VGP6 3.3 V (enable DIGLDO_CON10 0x0424 bit 15, voltage DIGLDO_CON33 0x0452 bits 5-7; Linux keeps it always on)
//   protocol:   Elan I2C: 16-bit command registers (write the register, then read 2 bytes), reset 0x0005 = 0x0100 (answers with
//               two zero bytes), absolute mode 0x0300 = 1, then a 34-byte report each time GPIO117 goes low:
//               [2] = 0x5d, [3] = fingers (bits 3-7) + button (bit 0), then 5 bytes per finger: x/y high nibbles, x low, y low (y from the bottom)
// The controller moves data with its DMA channel (it has only an 8-byte FIFO); the MMU is off, so no cache upkeep is needed.

#define I2C4_BASE 0x11011000UL
#define I2C4_DMA  0x11000300UL
#define TP_IRQ_PIN 117
static void tp_w32(u64 a, u32 v) { *(volatile u32 *)a = v; }
static void i2w(u32 off, u32 v) { *(volatile u16 *)(I2C4_BASE + off) = (u16)v; }
static u32 i2r(u32 off) { return *(volatile u16 *)(I2C4_BASE + off); }
static void dmaw(u32 off, u32 v) { *(volatile u32 *)(I2C4_DMA + off) = v; }
static u32 dmar(u32 off) { return *(volatile u32 *)(I2C4_DMA + off); }
static u8 i2c_tx[64] __attribute__((aligned(64))), i2c_rx[256] __attribute__((aligned(64)));
static u32 tp_addr = 0x15, tp_maxx, tp_maxy, tp_ready;

// Bus timing: the source clock is the AXI clock / 16 (between ~8.5 and ~17 MHz, depending on what the firmware set);
// 2 samples x 43 steps keeps the bus at or below 100 kHz either way.
static void i2c4_init(void) {
    tp_w32(PERICFG_BASE + 0x10, (1u << 27) | (1u << 12));                         // clocks on (PERI0 clear = ungate)
    pin_mode(133, 1); pin_mode(134, 1);
    i2w(0x50, 1);                                                               // soft reset
    i2w(0x40, 3);                                                               // open-drain outputs
    i2w(0x54, 0);                                                               // no clock gating inside the block
    i2w(0x20, (1u << 8) | 42);                                                  // timing: sample 2, step 43
    i2w(0x48, 0);                                                               // no high-speed mode
    i2w(0x10, (1u << 5) | (1u << 3));                                           // detect missing ACKs, external clock
    i2w(0x1c, 2);                                                               // delay between transactions
    dmaw(0x0c, 2); delay_us(50); dmaw(0x0c, 0);                                 // DMA channel: hard reset
}
// One transfer to `addr`: write wlen bytes, then (repeated start) read rlen bytes; either part may be empty.
// Returns 0 = done, -1 = no ACK (nothing answers at that address), -2 = the controller never finished.
static int i2c4_xfer(u32 addr, const u8 *w, u32 wlen, u8 *r, u32 rlen) {
    if (wlen > sizeof i2c_tx || rlen > sizeof i2c_rx) return -2;
    for (u32 i = 0; i < wlen; i++) i2c_tx[i] = w[i];
    int wrrd = wlen && rlen;
    u32 ctl = (1u << 5) | (1u << 3) | (1u << 2);                               // ACK detect, external clock, DMA
    if (wrrd) ctl |= (1u << 4) | (1u << 1);                                     // direction change + repeated start
    i2w(0x10, ctl);
    i2w(0x04, (addr << 1) | (!wlen ? 1 : 0));
    i2w(0x38, 1);                                                               // empty the FIFO
    i2w(0x08, 7); i2w(0x0c, 7);                                                 // report done / ACK error / HS NACK; clear old ones
    if (wrrd) { i2w(0x14, wlen); i2w(0x6c, rlen); i2w(0x18, 2); }
    else { i2w(0x14, wlen ? wlen : rlen); i2w(0x18, 1); }
    dmaw(0x00, 0);
    if (wrrd) { dmaw(0x18, 0); dmaw(0x1c, (u32)(u64)i2c_tx); dmaw(0x20, (u32)(u64)i2c_rx); dmaw(0x24, wlen); dmaw(0x28, rlen); }
    else if (wlen) { dmaw(0x18, 0); dmaw(0x1c, (u32)(u64)i2c_tx); dmaw(0x24, wlen); }
    else { dmaw(0x18, 1); dmaw(0x20, (u32)(u64)i2c_rx); dmaw(0x28, rlen); }
    dmaw(0x08, 1);                                                              // DMA: go
    i2w(0x24, 1);                                                               // I2C: go
    u64 hz = tick_hz(), t0 = ticks(); u32 st;
    while (!((st = i2r(0x0c)) & 7)) if (!hz || ticks() - t0 > hz / 20) { i2c4_init(); return -2; }   // 50 ms
    i2w(0x0c, st & 7);
    if (st & 6) { i2c4_init(); return -1; }
    t0 = ticks(); while ((dmar(0x08) & 1) && hz && ticks() - t0 < hz / 100) {}   // the DMA finishes copying
    for (u32 i = 0; i < rlen; i++) r[i] = i2c_rx[i];
    return 0;
}
static int elan_read(u32 cmd, u8 *out, u32 n) { u8 w[2] = {(u8)cmd, (u8)(cmd >> 8)}; return i2c4_xfer(tp_addr, w, 2, out, n); }
static int elan_write(u32 reg, u32 val) { u8 w[4] = {(u8)reg, (u8)(reg >> 8), (u8)val, (u8)(val >> 8)}; return i2c4_xfer(tp_addr, w, 4, 0, 0); }
static u32 le16(const u8 *p) { return p[0] | (u32)p[1] << 8; }
static int tp_irq(void) { return !gpio_bit(0x500, TP_IRQ_PIN); }               // 1 = the touchpad has something to say

// Power, pins, clocks: what the touchpad needs. Turns VGP6 on if the firmware left it off. Returns 0 if the PMIC answered.
static int tp_power(int verbose) {
    u32 en = 0, vs = 0;
    if (pmic_read(0x0424, &en) || pmic_read(0x0452, &vs)) { errs("TP", 3, 1, "the PMIC did not answer (touchpad power unknown)"); return -1; }
    if (verbose) { puts("  power VGP6: "); puts(en & 0x8000 ? "on" : "OFF"); puts(", voltage step "); put_dec((vs >> 5) & 7); puts(" (7 = 3.3 V)\n"); }
    if (!(en & 0x8000) || ((vs >> 5) & 7) != 7) {
        pmic_write(0x0452, (vs & ~0xe0u) | (7u << 5)); pmic_write(0x0424, en | 0x8000);
        delay_us(20000);
        if (verbose) puts("  switched VGP6 on at 3.3 V\n");
    }
    // interrupt pin: GPIO mode, input, pull-up (registers: DIR 0x000, PULLEN 0x100, PULLSEL 0x200; +4 set, +8 clear)
    u32 port = (TP_IRQ_PIN >> 4) << 4, bit = 1u << (TP_IRQ_PIN & 15);
    pin_mode(TP_IRQ_PIN, 0);
    tp_w32(GPIO_BASE + 0x000 + port + 8, bit); tp_w32(GPIO_BASE + 0x100 + port + 4, bit); tp_w32(GPIO_BASE + 0x200 + port + 4, bit);
    return 0;
}

// `tp`: look for the touchpad and say what it is.
static void tp_probe(void) {
    puts("touchpad: Elan on I2C bus 4 (address 0x15), interrupt GPIO117\n");
    tp_power(1);
    u32 g = rd32(PERICFG_BASE + 0x18);
    puts("  clocks before: I2C4 "); puts((g >> 27) & 1 ? "gated" : "on"); puts(", DMA "); puts((g >> 12) & 1 ? "gated" : "on"); putc('\n');
    show_pin(TP_IRQ_PIN, "touchpad IRQ"); show_pin(133, "I2C4 SCL"); show_pin(134, "I2C4 SDA");
    i2c4_init();
    u8 b[4]; int e = elan_read(0x0101, b, 2);
    if (e) {
        puts(e == -1 ? "  nothing answered at 0x15" : "  the I2C controller did not finish"); putc('\n');
        if (e == -1) {                                                          // other hanas: a HID-over-I2C pad at 0x2c
            u8 w[2] = {0x20, 0x00}, d[30];
            int h = i2c4_xfer(0x2c, w, 2, d, 30);
            if (!h) { puts("  but a HID-over-I2C device answered at 0x2c: descriptor length "); put_dec(le16(d)); puts(", vendor "); put_hex(le16(d + 20)); puts(", product "); put_hex(le16(d + 22)); putc('\n'); sum_s("hid@2c"); return; }
        }
        errs("TP", e == -1 ? 1 : 2, 0, e == -1 ? "the touchpad did not answer on I2C" : "the I2C controller did not finish a transfer");
        sum_s("none"); return;
    }
    u32 id = le16(b);
    u8 v[2]; elan_read(0x0102, v, 2); u32 fw = le16(v);
    elan_read(0x0106, v, 2); tp_maxx = le16(v) & 0x0fff;
    elan_read(0x0107, v, 2); tp_maxy = le16(v) & 0x0fff;
    puts("  Elan touchpad: product "); put_hex(id); puts(", firmware "); put_hex(fw); puts(", size "); put_dec(tp_maxx); puts(" x "); put_dec(tp_maxy); putc('\n');
    sum_s("elan "); sum_u(tp_maxx); sum_c('x'); sum_u(tp_maxy);
}

// Reset the touchpad and switch it to absolute reports (finger positions). Returns 0 if it is ready.
static int tp_start(void) {
    tp_ready = 0;
    if (tp_power(0)) return -1;
    i2c4_init();
    if (elan_write(0x0005, 0x0100)) { errs("TP", 1, 0, "the touchpad did not answer on I2C"); return -1; }   // reset
    u64 hz = tick_hz(), t0 = ticks();
    while (!tp_irq() && hz && ticks() - t0 < hz / 2) {}                         // it says hello when it is back
    u8 h[2]; i2c4_xfer(tp_addr, 0, 0, h, 2);                                    // the hello (00 00); the setup below decides if all is well
    u8 d[160];
    elan_read(0x0001, d, 30); elan_read(0x0002, d, 158);                        // descriptors (read like Linux does; contents not needed)
    u8 v[2];
    if (elan_read(0x0106, v, 2)) { errs("TP", 4, 0, "the touchpad stopped answering after its reset"); return -1; }
    tp_maxx = le16(v) & 0x0fff; elan_read(0x0107, v, 2); tp_maxy = le16(v) & 0x0fff;
    if (elan_write(0x0300, 0x0001)) { errs("TP", 4, 0, "the touchpad stopped answering after its reset"); return -1; }   // absolute mode
    if (!elan_read(0x0307, v, 2)) elan_write(0x0307, le16(v) & ~1u);            // power on (clear the sleep bit)
    if (!tp_maxx || !tp_maxy) { tp_maxx = 3200; tp_maxy = 1800; }
    tp_ready = 1;
    return 0;
}
// One report, if the touchpad has one waiting. Returns 1 and fills n (fingers), x[]/y[] (0..max, y from the top), button.
static int tp_read(u32 *n, u32 *x, u32 *y, u32 *button) {
    if (!tp_ready || !tp_irq()) return 0;
    u8 r[34];
    if (i2c4_xfer(tp_addr, 0, 0, r, 34)) return 0;
    if (r[2] != 0x5d) return 0;                                                 // not a finger report
    u32 info = r[3], k = 0; const u8 *f = r + 4;
    for (u32 i = 0; i < 5; i++) {
        if (!(info & (0x08u << i))) continue;
        u32 px = ((u32)(f[0] & 0xf0) << 4) | f[1], py = ((u32)(f[0] & 0x0f) << 8) | f[2];
        x[k] = px; y[k] = py <= tp_maxy ? tp_maxy - py : 0; k++; f += 5;
    }
    *n = k; *button = info & 1;
    return 1;
}
// `tptest`: show what the fingers do (any key, or 30 s without a touch, ends it).
static void tp_test(void) {
    if (tp_start()) { sum_s("none"); return; }
    puts("touchpad ready ("); put_dec(tp_maxx); puts(" x "); put_dec(tp_maxy); puts("): touch it, press any key to stop\n");
    u64 hz = tick_hz(), last = ticks(), shown = 0;
    u32 reps = 0, maxf = 0, clicks = 0, lastb = 0, lastn = 99;
    for (;;) {
        if (kb_getc() >= 0) break;
        if (hz && ticks() - last > hz * 30) break;
        u32 n, x[5], y[5], b;
        if (!tp_read(&n, x, y, &b)) { wdt_kick(); continue; }
        reps++; last = ticks();
        if (n > maxf) maxf = n;
        if (b && !lastb) clicks++;
        if (n != lastn || b != lastb || ticks() - shown > hz / 8) {            // at most 8 lines a second
            shown = ticks();
            put_dec(n); puts(n == 1 ? " finger " : " fingers");
            for (u32 i = 0; i < n; i++) { puts("  "); put_dec(x[i]); putc(','); put_dec(y[i]); }
            if (b) puts("  CLICK");
            putc('\n');
        }
        lastb = b; lastn = n;
    }
    sum_s("rep "); sum_u(reps); sum_s(" f"); sum_u(maxf); sum_s(" click"); sum_u(clicks);
}
