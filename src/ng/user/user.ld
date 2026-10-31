/*
 * ng WP1-7 用户程序链接脚本：固定加载基址 0x40000000。
 *
 * 段之间 4 KiB 对齐，让内核 ELF64 加载器可以按"段 -> 页权限"映射：
 *   .text    -> RX
 *   .rodata  -> R + NX
 *   .data/.bss（同页）-> RW + NX，其中 .bss 就是 memsz-filesz 的尾巴
 *
 * 不把 .bss 单独对齐：让 .data 的文件字节和 .bss 的零填充落在同一个
 * PT_LOAD 里，正好验证加载器的"清整页再拷文件字节"逻辑。
 */
ENTRY(_start)
SECTIONS
{
    . = 0x40000000;
    __user_image_start = .;

    .text : { *(.text .text.*) }

    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }

    . = ALIGN(4096);
    .data : { *(.data .data.*) }

    .bss : { *(.bss .bss.*) *(COMMON) }

    __user_image_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame*)
        *(.note.GNU-stack)
    }
}
