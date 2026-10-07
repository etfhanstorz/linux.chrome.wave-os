// Boot beacon v3: proves the kernel runs without needing the display.
// Reboots IMMEDIATELY on entry (no timer, no waiting, no reads of firmware data):
//   1. MT8173 hardware watchdog software reset (what Linux's mtk_wdt driver does)
//   2. PSCI SYSTEM_RESET through the firmware (smc)
// Expected if our code runs: a short black flicker, then the "OS verification is OFF" screen again.
// If the screen stays black, our code almost certainly never ran.
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
    // 1. Watchdog: reset mode (IRQ_EN off, with key), then software reset. Same as Linux mtk_wdt_restart.
    ldr     x1, =0x10007000     // TOPRGU / watchdog base
    ldr     w2, [x1]            // WDT_MODE
    and     w2, w2, #0x00ffffff // drop the key field
    bic     w2, w2, #(1 << 3)   // WDT_MODE_IRQ_EN off: reset instead of interrupt
    ldr     w3, =0x22000000     // WDT_MODE_KEY
    orr     w2, w2, w3
    str     w2, [x1]
    ldr     w3, =0x1209         // WDT_SWRST_KEY
    str     w3, [x1, #0x14]     // WDT_SWRST
    dsb     sy
    str     w3, [x1, #0x14]
    dsb     sy

    // 2. Still alive: ask the firmware via PSCI
    ldr     x0, =0x84000009     // PSCI SYSTEM_RESET
    smc     #0

park:
    wfe
    b       park
    .ltorg
