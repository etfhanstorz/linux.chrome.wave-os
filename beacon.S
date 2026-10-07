// Boot beacon v2: proves the kernel runs without needing the display.
// About 4 s after entry it reboots the machine, trying two independent methods:
//   1. MT8173 hardware watchdog software reset (what Linux's mtk_wdt driver does)
//   2. PSCI SYSTEM_RESET through the firmware (smc)
// If the screen stays black and nothing reboots, our code almost certainly never ran.
    .section .text.boot, "ax"
    .global _start
_start:
    b       entry               // code0
    .long   0                   // code1
    .quad   0x80000             // text_offset
    .quad   image_size          // image_size
    .quad   0                   // flags: 0, same as the Arch kernel that boots on this firmware
    .quad   0
    .quad   0
    .quad   0
    .long   0x644d5241          // magic "ARM\x64"
    .long   0

entry:
    msr     daifset, #0xf

    // Wait about 4 seconds on the generic timer (bounded, in case the counter is stopped)
    mrs     x2, cntfrq_el0
    cbz     x2, reboot          // no timer frequency set: skip the wait
    lsl     x2, x2, #2          // 4 s worth of ticks
    mrs     x4, cntpct_el0
    mov     x6, #0x10000000     // iteration cap
1:  mrs     x5, cntpct_el0
    sub     x5, x5, x4
    cmp     x5, x2
    b.hs    reboot
    subs    x6, x6, #1
    b.ne    1b

reboot:
    // 1. Watchdog: switch to reset mode (clear IRQ_EN, write key), then software reset
    ldr     x1, =0x10007000     // TOPRGU / watchdog base
    ldr     w2, [x1]            // WDT_MODE
    and     w2, w2, #0x00ffffff // drop the key field
    bic     w2, w2, #(1 << 3)   // WDT_MODE_IRQ_EN off: reset instead of interrupt
    ldr     w3, =0x22000000     // WDT_MODE_KEY
    orr     w2, w2, w3
    str     w2, [x1]
    ldr     w3, =0x1209         // WDT_SWRST_KEY
    mov     x7, #16             // try a few times, like Linux does
2:  str     w3, [x1, #0x14]     // WDT_SWRST
    dsb     sy
    mov     x8, #0x100000
3:  subs    x8, x8, #1
    b.ne    3b
    subs    x7, x7, #1
    b.ne    2b

    // 2. Still alive: ask the firmware via PSCI
    ldr     x0, =0x84000009     // PSCI SYSTEM_RESET
    smc     #0

park:
    wfe
    b       park
    .ltorg
