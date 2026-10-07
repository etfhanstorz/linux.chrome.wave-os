// Linux arm64 Image header + entry. Firmware passes DTB pointer in x0.
    .section .text.boot, "ax"
    .global _start
_start:
    b       entry               // code0
    .long   0                   // code1
    .quad   0x80000             // text_offset
    .quad   image_size          // image_size (from linker script)
    .quad   0                   // flags: 0, same as the Arch kernel that boots on this firmware
    .quad   0
    .quad   0
    .quad   0
    .long   0x644d5241          // magic "ARM\x64" at offset 56
    .long   0

entry:
    mov     x19, x0             // save DTB pointer
    // The firmware leaves data (e.g. the device tree) dirty in the D-cache. We run with caches
    // off, so write it all back to RAM first -- before we write any memory ourselves.
    bl      dcache_clean_inval_all
    // Mask interrupts; works at EL1 or EL2 since we never touch EL-specific regs.
    msr     daifset, #0xf
    // Park secondary cores (only if firmware started them)
    mrs     x1, mpidr_el1
    and     x1, x1, #0xff
    cbnz    x1, park
    // Enable FP/SIMD in case compiler emits it (EL1: cpacr, EL2: cptr)
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.ne    1f
    mov     x2, #0x33ff
    msr     cptr_el2, x2
    b       2f
1:  mov     x2, #(3 << 20)
    msr     cpacr_el1, x2
2:  isb
    // Zero BSS (PC-relative)
    adr     x0, __bss_start
    adr     x1, __bss_end
3:  cmp     x0, x1
    b.hs    4f
    str     xzr, [x0], #8
    b       3b
4:  adr     x0, stack_top
    mov     sp, x0
    // Install fault handler for the current EL: bad MMIO reads set fault_flag instead of hanging
    adr     x0, vectors
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.ne    5f
    msr     vbar_el2, x0
    b       6f
5:  msr     vbar_el1, x0
6:  isb
    mov     x0, x19
    bl      main
park:
    wfe
    b       park

// Any exception: skip the faulting instruction, set fault_flag, return.
    .balign 2048
vectors:
    .rept 16
    b       fault_handler
    .balign 128
    .endr

fault_handler:
    stp     x0, x1, [sp, #-16]!
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.eq    1f
    mrs     x0, elr_el1
    add     x0, x0, #4
    msr     elr_el1, x0
    b       2f
1:  mrs     x0, elr_el2
    add     x0, x0, #4
    msr     elr_el2, x0
2:  adr     x0, fault_flag
    mov     w1, #1
    str     w1, [x0]
    ldp     x0, x1, [sp], #16
    eret

// Clean + invalidate the whole data cache by set/way (all levels up to the Level of Coherence).
// Uses only registers x0-x11; no memory. Works at EL1 or EL2 with the MMU off.
dcache_clean_inval_all:
    mrs     x0, clidr_el1
    and     x3, x0, #0x7000000
    lsr     x3, x3, #23         // x3 = LoC * 2
    cbz     x3, 9f
    mov     x10, #0             // x10 = cache level * 2
1:  add     x2, x10, x10, lsr #1
    lsr     x1, x0, x2
    and     x1, x1, #7          // cache type at this level
    cmp     x1, #2
    b.lt    5f                  // no data cache here
    msr     csselr_el1, x10
    isb
    mrs     x1, ccsidr_el1
    and     x2, x1, #7
    add     x2, x2, #4          // log2(line length)
    mov     x4, #0x3ff
    and     x4, x4, x1, lsr #3  // max way number
    clz     w5, w4              // way shift
    mov     x7, #0x7fff
    and     x7, x7, x1, lsr #13 // max set number
2:  mov     x9, x4
3:  lsl     x6, x9, x5
    orr     x11, x10, x6
    lsl     x6, x7, x2
    orr     x11, x11, x6
    dc      cisw, x11
    subs    x9, x9, #1
    b.ge    3b
    subs    x7, x7, #1
    b.ge    2b
5:  add     x10, x10, #2
    cmp     x3, x10
    b.gt    1b
9:  mov     x10, #0
    msr     csselr_el1, x10
    dsb     sy
    isb
    ret
