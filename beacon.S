// Boot beacon: proves the kernel runs without needing the display.
// ~4 s after entry: valid DTB in x0 -> PSCI SYSTEM_RESET (reboot); otherwise -> PSCI SYSTEM_OFF.
// If this never runs, the machine just stays black.
    .section .text.boot, "ax"
    .global _start
_start:
    b       entry               // code0
    .long   0                   // code1
    .quad   0x80000             // text_offset
    .quad   image_size          // image_size
    .quad   0xa                 // flags
    .quad   0
    .quad   0
    .quad   0
    .long   0x644d5241          // magic "ARM\x64"
    .long   0

entry:
    mov     x19, x0             // DTB pointer from firmware
    msr     daifset, #0xf
    mrs     x1, mpidr_el1
    and     x1, x1, #0xff
    cbnz    x1, park            // only the boot CPU continues

    // Wait about 4 seconds on the generic timer (bounded, in case the counter is not running)
    mrs     x2, cntfrq_el0
    cbz     x2, check           // no timer frequency set: skip the wait
    lsl     x2, x2, #2          // 4 s worth of ticks
    mrs     x4, cntpct_el0
    mov     x6, #0x7fffffff     // iteration cap
1:  mrs     x5, cntpct_el0
    sub     x5, x5, x4
    cmp     x5, x2
    b.hs    check
    subs    x6, x6, #1
    b.ne    1b

check:
    // DTB pointer must be in RAM (0x40000000..0x13fffffff) and 8-byte aligned before we read it
    mov     x1, #0x40000000
    cmp     x19, x1
    b.lo    bad
    ldr     x1, =0x140000000
    cmp     x19, x1
    b.hs    bad
    tst     x19, #7
    b.ne    bad
    ldr     w1, [x19]
    ldr     w2, =0xedfe0dd0     // 0xd00dfeed read as little-endian
    cmp     w1, w2
    b.ne    bad

good:
    ldr     x0, =0x84000009     // PSCI SYSTEM_RESET
    smc     #0
    b       park
bad:
    ldr     x0, =0x84000008     // PSCI SYSTEM_OFF
    smc     #0
park:
    wfe
    b       park
    .ltorg
