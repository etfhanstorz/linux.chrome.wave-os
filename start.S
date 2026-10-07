// Linux arm64 Image header + entry. Firmware passes DTB pointer in x0.
    .section .text.boot, "ax"
    .global _start
_start:
    b       entry               // code0
    .long   0                   // code1
    .quad   0x80000             // text_offset
    .quad   image_size          // image_size (from linker script)
    .quad   0xa                 // flags: LE, 4K pages, place anywhere
    .quad   0
    .quad   0
    .quad   0
    .long   0x644d5241          // magic "ARM\x64" at offset 56
    .long   0

entry:
    mov     x19, x0             // save DTB pointer
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
    mov     x0, x19
    bl      main
park:
    wfe
    b       park
