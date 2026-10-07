// Low-level helpers: instruction cache, delays, reboot. Work at EL1 or EL2.

static u32 current_el(void) { u64 el; __asm__ volatile("mrs %0, CurrentEL" : "=r"(el)); return el >> 2; }

// Instruction cache on (allowed with the MMU off); makes loops and drawing much faster.
static void icache_on(void) {
    __asm__ volatile("ic iallu; dsb sy; isb" ::: "memory");
    u64 s;
    if (current_el() == 2) {
        __asm__ volatile("mrs %0, sctlr_el2" : "=r"(s));
        s |= 1 << 12;
        __asm__ volatile("msr sctlr_el2, %0; isb" :: "r"(s));
    } else {
        __asm__ volatile("mrs %0, sctlr_el1" : "=r"(s));
        s |= 1 << 12;
        __asm__ volatile("msr sctlr_el1, %0; isb" :: "r"(s));
    }
}

// Virtual counter: readable at EL1 without being trapped by EL2 (unlike cntpct).
static u64 ticks(void) { u64 v; __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v)); return v; }
static u64 tick_hz(void) { u64 f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); return f; }

static void delay_s(u32 s) {
    u64 hz = tick_hz(), t0 = ticks();
    for (u64 i = 0; ; i++) {
        if (hz && ticks() - t0 >= hz * s) return;
        if (i > (u64)s * 50000000UL) return;     // cap, in case the counter is not running
    }
}

// Dead-man switch: arm the MT8173 watchdog so that if we freeze, the machine reboots after `sec`
// seconds (max 31). Same register sequence as Linux mtk_wdt set_timeout + start.
static void wdt_arm(u32 sec) {
    volatile u32 *wdt = (volatile u32 *)0x10007000UL;
    if (sec > 31) sec = 31;
    wdt[0x04 / 4] = ((sec << 6) << 5) | 0x8;                     // WDT_LENGTH: timeout | key
    wdt[0x08 / 4] = 0x1971;                                      // WDT_RST: reload counter
    u32 mode = wdt[0];
    mode &= 0x00ffffff & ~((1u << 3) | (1u << 6));               // no IRQ / dual mode: real reset
    wdt[0] = mode | 1 | 0x22000000;                              // WDT_MODE_EN | key
}
static void wdt_kick(void) { *(volatile u32 *)(0x10007000UL + 0x08) = 0x1971; }

// Reboot. PSCI SYSTEM_RESET first: that is how ChromeOS reboots, and the firmware keeps RAM
// (and so the ramoops log) alive across it. A raw watchdog reset seems to wipe RAM, so it is
// only the fallback if the firmware call returns.
static void reboot(void) {
    register u64 x0 __asm__("x0") = 0x84000009;
    __asm__ volatile("smc #0" : "+r"(x0) :: "memory");
    volatile u32 *wdt = (volatile u32 *)0x10007000UL;
    u32 mode = wdt[0];
    wdt[0] = ((mode & 0x00ffffff) & ~(1u << 3)) | 0x22000000;   // reset mode, IRQ off, key
    for (int i = 0; i < 4; i++) { wdt[0x14 / 4] = 0x1209; __asm__ volatile("dsb sy" ::: "memory"); }
    for (;;) __asm__ volatile("wfe");
}
