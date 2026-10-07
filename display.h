// Turn the hana panel back on after the firmware hand-off.
// What the firmware leaves (measured with v0.6.2 on the real machine): the screen buffer, DSI link,
// RDMA0 and panel power (GPIO41) are still set up, but the overlay engine is stopped (OVL0_EN = 0)
// and the backlight is off: power GPIO32 low, enable GPIO95 low, brightness PWM (pin 87) clock-gated.

#define GPIO_BASE 0x10005000UL
#define OVL0_BASE 0x1400c000UL

static void gpio_out(u32 pin, int high) {          // MT8173 GPIO: 16 pins per register, SET at +4, CLR at +8
    u64 port = (u64)(pin >> 4) << 4;
    u32 bit = 1u << (pin & 15);
    *(volatile u32 *)(GPIO_BASE + 0x400 + port + (high ? 4 : 8)) = bit;   // DOUT first...
    *(volatile u32 *)(GPIO_BASE + 0x000 + port + 4) = bit;                // ...then DIR = output
}

static void pin_mode(u32 pin, u32 mode) {           // pinmux: 5 pins per register, 3 bits each, mode 0 = GPIO
    volatile u32 *r = (volatile u32 *)(GPIO_BASE + 0x600 + (u64)(pin / 5) * 0x10);
    u32 sh = (pin % 5) * 3;
    *r = (*r & ~(7u << sh)) | (mode << sh);
}

static void delay_us(u32 us) {
    u64 hz = tick_hz(), t0 = ticks();
    for (u64 i = 0; i < (u64)us * 100; i++)
        if (hz && ticks() - t0 >= hz / 1000000 * us) return;
}

#define RDMA0_BASE 0x1400e000UL
#define RDMA_FRAME_END (1u << 2)

// Clear RDMA0's interrupt status, wait ~4 frames, return what happened:
// bit1 frame start, bit2 frame end, bit3 abnormal end, bit4 FIFO underflow.
static u32 rdma_frames(void) {
    volatile u32 *st = (volatile u32 *)(RDMA0_BASE + 0x04);
    *st = 0;
    delay_us(70000);
    return *st;
}

static void ovl_put(u32 off, u32 v) { *(volatile u32 *)(OVL0_BASE + off) = v; }

// v0.7 showed that OVL0_EN = 1 alone works only sometimes, so: stop the overlay, re-program
// layer 0 with the screen we found, start it again, and check that frames reach RDMA0. Retry.
static int display_on(const struct fb *f) {
    puts("rdma0 status before: "); put_hex(rdma_frames()); putc('\n');
    gpio_out(32, 1);                 // backlight power (bl_fixed regulator)
    delay_us(2000);
    pin_mode(87, 0);                 // brightness pin: plain GPIO held high = 100% duty, no PWM block needed
    gpio_out(87, 1);
    delay_us(1000);
    gpio_out(95, 1);                 // backlight enable
    u32 size = (f->h << 16) | f->w;
    for (int attempt = 1; attempt <= 3; attempt++) {
        ovl_put(0x0c, 0);            // OVL0_EN off
        delay_us(20000);
        ovl_put(0x20, size);         // ROI size
        ovl_put(0x38, size);         // layer 0 source size
        ovl_put(0x3c, 0);            // layer 0 offset
        ovl_put(0x44, f->stride);    // layer 0 pitch
        ovl_put(0xf40, (u32)f->addr);// layer 0 address
        ovl_put(0x2c, 1);            // only layer 0 on
        ovl_put(0x0c, 1);            // OVL0_EN on
        __asm__ volatile("dsb sy" ::: "memory");
        u32 s = rdma_frames();
        puts("display attempt "); put_dec(attempt); puts(": rdma0 status "); put_hex(s);
        puts(s & RDMA_FRAME_END ? " (frames flowing)\n" : " (no frames)\n");
        if (s & RDMA_FRAME_END) return 1;
    }
    return 0;
}
