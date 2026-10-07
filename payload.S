// Reboot payload, appended to the end of a real Linux kernel Image (see build_beacon.sh).
// Position independent. Reboots immediately: watchdog first (as Linux mtk_wdt does), then PSCI.
    .text
    .global payload
payload:
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
    ldr     x0, =0x84000009     // PSCI SYSTEM_RESET
    smc     #0
1:  wfe
    b       1b
    .ltorg
