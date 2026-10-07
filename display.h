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

static void display_on(void) {
    gpio_out(32, 1);                 // backlight power (bl_fixed regulator)
    delay_us(2000);
    pin_mode(87, 0);                 // brightness pin: plain GPIO held high = 100% duty, no PWM block needed
    gpio_out(87, 1);
    delay_us(1000);
    gpio_out(95, 1);                 // backlight enable
    *(volatile u32 *)(OVL0_BASE + 0x0c) = 1;   // restart the overlay engine (OVL0_EN)
    __asm__ volatile("dsb sy" ::: "memory");
}
