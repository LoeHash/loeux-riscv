OUTPUT_ARCH(riscv)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* R|X */
}

SECTIONS
{
    . = 0x1000;
    .text : { *(.text) } :text
    . = ALIGN(0x1000);
    .data : { *(.data) }
    .bss  : { *(.bss) }
}