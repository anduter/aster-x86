OUTPUT_FORMAT(elf32-i386)
ENTRY(boot_verify_entry)

SECTIONS {
    . = 0x9800;

    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
