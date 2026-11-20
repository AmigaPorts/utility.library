MEMORY
{
    /* Module size ceiling; the HUNK loader allocates the actual linked size. */
    rom (rx) : org = 0x00000000, l = 64K
    ram (rw) : org = 0xdeaddead, l = 0M
}

ENTRY(_doNotExecute)

SECTIONS
{
    .text 0x0 : 
    {
        _rom_start = .;

        /* Shell entry precedes Resident data, independent of object order. */
        KEEP(*(.text.entry))
        KEEP(*(.rodata.romtag))

        *(.text .text.* .rodata .rodata.*)
        *(.eh_frame)

        . = ALIGN(4);
    } >rom = 0xff

    _rom_end = .;
    ASSERT(_doNotExecute == _rom_start, "Library Shell guard must be first")

    /* Writable sections have no home in ROM: any content fails the link. */
    .data : {
        *(.data .data.*)
    } >ram

    .bss : {
        *(.bss .bss.* COMMON)
    } >ram
}
