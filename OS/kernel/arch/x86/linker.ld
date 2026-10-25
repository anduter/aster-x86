/* =============================================================================
 * linker.ld - 内核链接脚本
 *
 * 目标：elf32-i386，加载基址 0x100000
 *   1. 最前面 8 字节是内核头：
 *      魔数 'K','R','N','L' + 内核总字节数（dword）
 *   2. 紧接着是不对齐的 .entry 段，内核入口 kernel_entry 就在 0x100008
 *   3. 之后才是按 16 字节对齐的 .text
 * 说明：
 *   - 头部里的长度由 _end - 0x100000 在链接时算出，
 *     加载器读这个值来计算需要读多少个扇区。
 *   - _kernel_start / _kernel_end 导出给 PMM，用来把内核镜像
 *     占用的物理页标记为已用。
 *   - 入口必须固定在 0x100008（pm_entry.asm 里写死跳转到这里），
 *     所以 .entry 单独成段且 align=1；若把入口并入 .text，
 *     .text 的 16 字节对齐会把它推到 0x100010，跳转就会落空。
 * ============================================================================= */
OUTPUT_FORMAT(elf32-i386)
ENTRY(kernel_entry)

SECTIONS {
    . = 0x100000;                    /* 内核加载基址 */

    _kernel_start = .;               /* 内核镜像起始符号 */
    __kernel_text_start = .;         /* 内核可执行区起点（含头部/入口） */

    .kernel_header : {               /* 位于 0x100000 的 8 字节内核头 */
        BYTE(0x4B) BYTE(0x52) BYTE(0x4E) BYTE(0x4C)  /* 'K','R','N','L' */
        LONG(_image_end - 0x100000)  /* 镜像文件字节数（不含 BSS） */
    }

    .entry : { *(.entry) }           /* 入口段：不要求对齐，固定在 0x100008 */

    .text : ALIGN(16) { *(.text*) }  /* 代码段，恢复 16 字节对齐 */
    __kernel_text_end = .;

    . = ALIGN(0x1000);
    __kernel_rodata_start = .;
    .rodata   : { *(.rodata*) }      /* 只读数据：R + NX */
    __kernel_rodata_end = .;

    . = ALIGN(0x1000);
    __kernel_data_start = .;
    .data     : { *(.data*)   }      /* 已初始化数据：RW + NX */
    .eh_frame : { *(.eh_frame*) }    /* 显式收尾，避免段布局漂移 */
    __kernel_data_end = .;

    _image_end = .;                  /* 磁盘镜像到此为止（BSS 不占文件） */

    . = ALIGN(0x1000);
    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    _end = .;
    _kernel_end = .;                 /* 内核镜像结束符号（含 BSS） */
    __kernel_end = .;
}
