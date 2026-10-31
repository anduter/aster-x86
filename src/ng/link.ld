/*
 * ng WP1-1 链接脚本：内核加载在 1 MiB，ELF64、静态、无 PIE。
 *
 * 顺序要求：
 *   - .multiboot 必须在最前：multiboot2 header 必须落在文件前 32 KiB；
 *   - .note.Xen 紧随其后：QEMU 通过 PT_NOTE 找 PVH 入口；
 *   - .bss 里的页表/栈必须在 1 GiB 恒等映射范围内（这里离 0x100000 很近）。
 */
ENTRY(_start)
SECTIONS
{
    . = 0x100000;

    .multiboot : { *(.multiboot) }
    .note.Xen  : { *(.note.Xen) }
    .text      : { *(.text .text.*) }
    .rodata    : { *(.rodata .rodata.*) }
    .data      : { *(.data .data.*) }
    .bss       : { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame*)
        *(.note.GNU-stack)
    }
}
