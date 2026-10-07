ENTRY(_start)
SECTIONS {
  . = 0;
  _kstart = .;
  .text : { *(.text.boot) *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  . = ALIGN(16);
  __bss_start = .;
  .bss : { *(.bss*) *(COMMON) }
  . = ALIGN(16);
  . += 0x10000;
  stack_top = .;
  __bss_end = .;
  _kend = .;
  image_size = _kend - _kstart;
}
