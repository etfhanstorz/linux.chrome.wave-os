ENTRY(_start)
SECTIONS {
  . = 0;
  _kstart = .;
  .text : { *(.text.boot) *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .got : { *(.got) *(.got.plt) }
  .dynamic : { *(.dynamic) }
  .dynsym : { *(.dynsym) }
  .dynstr : { *(.dynstr) }
  .hash : { *(.hash) }
  .gnu.hash : { *(.gnu.hash) }
  . = ALIGN(8);
  .rela.dyn : { __rela_start = .; *(.rela*) __rela_end = .; }   /* applied by start.S at boot */
  . = ALIGN(16);
  __bss_start = .;
  .bss : { *(.bss*) *(COMMON) }
  . = ALIGN(16);
  . += 0x10000;
  stack_top = .;
  __bss_end = .;
  . = ALIGN(0x10000);
  _kend = .;
  image_size = ABSOLUTE(_kend - _kstart);
  /DISCARD/ : { *(.interp) *(.comment) *(.note*) *(.eh_frame*) }
}
