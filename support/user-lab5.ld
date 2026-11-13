/* Local build layout; the supplied user/user.ld remains unchanged. */
OUTPUT_ARCH(riscv)
ENTRY(start)
PHDRS {
  text PT_LOAD FLAGS(5);
  rodata PT_LOAD FLAGS(4);
  data PT_LOAD FLAGS(6);
}
SECTIONS {
  . = 0x1000;
  .text : { *(.text .text.*) } :text
  . = ALIGN(4096);
  .rodata : { *(.rodata .rodata.* .srodata .srodata.*) } :rodata
  . = ALIGN(4096);
  .data : { *(.data .data.* .sdata .sdata.*) } :data
  .bss : { *(.bss .bss.* .sbss .sbss.*) *(COMMON) } :data
  end = .;
  /DISCARD/ : { *(.eh_frame .comment) }
}
