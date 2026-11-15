ENTRY(_start)

SECTIONS
{
    . = 0x00100000;
    _kernel_start = .;

    /* The Multiboot header must occur in the first 8 KiB of the ELF image. */
    .multiboot ALIGN(4) :
    {
        KEEP(*(.multiboot))
    }

    .user_text ALIGN(4096) :
    {
        _user_text_start = .;
        KEEP(*(.usertext))
        . = ALIGN(4096);
        _user_text_end = .;
    }

    .text ALIGN(4096) :
    {
        _text_start = .;
        *(.text*)
        _text_end = .;
    }

    .rodata ALIGN(4096) :
    {
        _rodata_start = .;
        *(.rodata*)
        _rodata_end = .;
    }

    .data ALIGN(4096) :
    {
        _data_start = .;
        *(.data*)
        _data_end = .;
    }

    .bss ALIGN(4096) :
    {
        _bss_start = .;
        *(COMMON)
        *(.bss*)
        _bss_end = .;
    }

    . = ALIGN(4096);
    _kernel_end = .;
}
