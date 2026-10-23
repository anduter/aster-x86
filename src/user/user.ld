/* =============================================================================
 * user.ld - 用户程序链接脚本
 * 用户程序固定在 0x40000000 运行。
 *
 * P3 安全边界：
 *   - text 段 = R + X，加载后映射为只读；
 *   - data/bss 段 = R + W，加载后映射为可写；
 *   - 两个 PT_LOAD 分开，避免出现 RWX 段。
 *
 * 注意：32 位 x86 在未启用 PAE/NX 时不能真正禁止“读数据当代码执行”，
 * 但代码页只读可以阻止 ring3 改写自己的代码。完整 NX 留到 PAE 阶段。
 * ============================================================================= */
OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

PHDRS {
    text   PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    rodata PT_LOAD FLAGS(4);   /* PF_R，映射为 NX */
    data   PT_LOAD FLAGS(6);   /* PF_R | PF_W，映射为 NX */
}

SECTIONS {
    . = 0x40000000;

    .text : {
        *(.text.start)      /* 入口 _start 必须在最前面 */
        *(.text*)
    } :text

    . = ALIGN(0x1000);
    .rodata : { *(.rodata*) } :rodata

    . = ALIGN(0x1000);
    .data : { *(.data*) } :data
    .bss  : { *(.bss*) *(COMMON) } :data

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
