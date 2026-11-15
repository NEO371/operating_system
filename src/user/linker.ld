OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)
PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}
SECTIONS
{
    . = 0x08048000;
    .text : ALIGN(16) { *(.text*) *(.rodata*) } :text
    . = ALIGN(0x1000);
    .data : { *(.data*) } :data
    .bss : { *(.bss*) *(COMMON) } :data
    /DISCARD/ : { *(.comment*) *(.note*) *(.eh_frame*) }
}
