PHDRS {
 metadata PT_LOAD FILEHDR PHDRS FLAGS(4);
 first PT_LOAD FLAGS(5);
 padone PT_LOAD FLAGS(4);
 middle PT_LOAD FLAGS(5);
 padtwo PT_LOAD FLAGS(4);
 last PT_LOAD FLAGS(5);
 data PT_LOAD FLAGS(6);
 dynamic PT_DYNAMIC FLAGS(6);
}
SECTIONS {
 . = SIZEOF_HEADERS;
 .hash : { *(.hash) } :metadata
 .gnu.hash : { *(.gnu.hash) } :metadata
 .dynsym : { *(.dynsym) } :metadata
 .dynstr : { *(.dynstr) } :metadata
 .rela.dyn : { *(.rela.dyn) } :metadata
 .rodata : { *(.rodata) } :metadata
 . = 0x4000;
 .text.first : { *(.text.first) } :first
 . = 0x8000;
 .padding.one (NOLOAD) : { . = 0x081fc000; } :padone
 . = 0x08208000;
 .text.middle : { *(.text.middle) } :middle
 . = 0x0820c000;
 .padding.two (NOLOAD) : { . = 0x103fc000; } :padtwo
 . = 0x10400000;
 .text.last : { *(.text.last) } :last
 . = 0x10408000;
 .dynamic : { *(.dynamic) } :data :dynamic
 /DISCARD/ : { *(.comment) *(.note*) }
}
