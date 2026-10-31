/*
 * ng WP1-2 链接脚本：内核加载在 1 MiB，ELF64、静态、无 PIE。
 *
 * 顺序/对齐要求：
 *   - .multiboot 必须在最前：multiboot2 header 必须落在文件前 32 KiB；
 *   - .note.Xen 紧随其后：QEMU 通过 PT_NOTE 找 PVH 入口；
 *   - .text/.rodata/.data/.bss 各自 4 KiB 对齐：页表要按段给不同权限
 *     （RX / RO+NX / RW+NX），两个段共享一个 4 KiB 页就没法分开保护；
 *   - 段边界符号给 paging.c 用；ASSERT 保证内核镜像落在第一个 2 MiB 内，
 *     也就是 PD[0] 的 4 KiB 页窗口里（见 docs/design/paging-ng.md）。
 */
ENTRY(_start)
SECTIONS
{
    . = 0x100000;
    __image_start = .;

    .multiboot : { KEEP(*(.multiboot)) }
    .note.Xen  : { KEEP(*(.note.Xen)) }

    /* ---- 代码：只读 + 可执行（W=0, NX=0）---- */
    . = ALIGN(4096);
    __text_start = .;
    .text : { *(.text .text.*) }

    /* ---- 只读数据：只读 + 不可执行 ---- */
    . = ALIGN(4096);
    __text_end = .;
    __rodata_start = .;
    .rodata : { *(.rodata .rodata.*) }

    /* ---- 可写数据：可写 + 不可执行 ---- */
    . = ALIGN(4096);
    __rodata_end = .;
    __data_start = .;
    .data : { *(.data .data.*) }

    . = ALIGN(4096);
    __data_end = .;
    __bss_start = .;
    .bss : { *(.bss .bss.*) *(COMMON) }

    . = ALIGN(4096);
    __bss_end = .;
    __kernel_end = .;

    /*
     * WP2-2：FAT16 测试镜像（1.44MB）放在 2 MiB，**不**参与内核 4 KiB
     * W^X 窗口（PD[0] 只覆盖 0..2 MiB）。它由 QEMU 作为独立 PT_LOAD 加载，
     * 落在 PD[1] 的 2 MiB 恒等大页（RW+NX）里；mem_selftest.c 在 PMM 里
     * 保留 [__fat16_img_start, __fat16_img_end)，启动时 fat_ramdisk 把它
     * 复制到 PMM 页。这样内核正文/.rodata/.bss 仍然 < 2 MiB。
     *
     * WP3-3：7 个用户 ELF 的 .incbin blob 也从 .rodata 挪到 2 MiB 之后的
     * `.bigdata` 段（同一条 PD[1] 恒等大页）。它们只被内核读（proc_init
     * 种进 ramfs），不执行；这样 WP3-3 新增 sessiond（24 KB）后内核正文/
     * .bss 仍留在第一个 2 MiB 窗口里，并留出 ~100 KB 余量。
     * mem_selftest.c 保留 [__bigdata_start, __bigdata_end)。
     */
    . = 0x200000;
    __fat16_img_start = .;
    .fat16 : { KEEP(*(.fat16)) }
    __fat16_img_end = .;

    . = ALIGN(4096);
    __bigdata_start = .;
    .bigdata : { KEEP(*(.bigdata)) }
    __bigdata_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame*)
        *(.note.GNU-stack)
    }
}

/*
 * 内核镜像必须整个落在第一个 2 MiB 里：PD[0] 是唯一的 4 KiB 页窗口。
 * 超了就要么加 PT（改 paging.c），要么把内核搬到别处。
 */
ASSERT(__kernel_end <= 0x200000, "ng: kernel image must fit in the first 2 MiB (4 KiB page window is PD[0])")
