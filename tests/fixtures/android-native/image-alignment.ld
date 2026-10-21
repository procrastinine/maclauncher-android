PHDRS {
  text PT_LOAD FILEHDR PHDRS FLAGS(5);
  data PT_LOAD FLAGS(6);
  dynamic PT_DYNAMIC FLAGS(6);
}
SECTIONS {
  . = SIZEOF_HEADERS;
  .hash : { *(.hash) } :text
  .gnu.hash : { *(.gnu.hash) } :text
  .dynsym : { *(.dynsym) } :text
  .dynstr : { *(.dynstr) } :text
  .rela.dyn : { *(.rela.dyn) } :text
  .text : { *(.text*) } :text
  . = ALIGN(0x1000);
  .dynamic : { *(.dynamic) } :data :dynamic
  .got : { *(.got*) } :data
  .data : { *(.data*) } :data
  .bss : { *(.bss*) *(COMMON) } :data
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
